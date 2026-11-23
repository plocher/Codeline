"""The layout launcher (tools/run_layout.py) started for two stations on free ports.

  - it comes up: both hosts ready, the address printed carries ?ws= for the WebSockets port;
  - the page server gives /web/ and /build/Corporal.panel.json, and nothing else of the
    repository (/.git/config, also by way of /web/../, is 404);
  - the broker answers MQTT over WebSockets on the port given, and both stations are on it;
  - SIGHUP (the terminal closed) stops it, exit 0, leaving no broker and no host behind.
"""
from __future__ import annotations

import os
import re
import signal
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

import paho.mqtt.client as mqtt

ROOT = Path(__file__).resolve().parent.parent
LIMIT_S = 120


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def status(url: str) -> int:
    try:
        with urllib.request.urlopen(url, timeout=5) as r:
            return r.status
    except urllib.error.HTTPError as e:
        return e.code


def raw_status(port: int, path: str) -> int:
    """GET with the path sent exactly as written (urllib would tidy '/web/../' away)."""
    with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
        s.sendall(f"GET {path} HTTP/1.0\r\n\r\n".encode())
        return int(s.recv(64).split()[1])


def children(pid: int) -> list[int]:
    out = subprocess.run(["pgrep", "-P", str(pid)], capture_output=True, text=True).stdout
    return [int(p) for p in out.split()]


def alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def retained_over_websockets(ws_port: int, topics: list[str]) -> dict[str, str]:
    got: dict[str, str] = {}
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="launcher-scenario", transport="websockets")
    c.on_message = lambda cl, u, m: got.__setitem__(m.topic, m.payload.decode())
    c.connect("127.0.0.1", ws_port)
    c.loop_start()
    for t in topics:
        c.subscribe(t, 1)
    deadline = time.monotonic() + 5
    while len(got) < len(topics) and time.monotonic() < deadline:
        time.sleep(0.05)
    c.loop_stop()
    c.disconnect()
    return got


def main() -> int:
    name = "the launcher: two stations, the page and panel files served and nothing else, SIGHUP leaves nothing running"
    port, ws, http = free_port(), free_port(), free_port()
    proc = subprocess.Popen([sys.executable, str(ROOT / "tools" / "run_layout.py"), "--stations", "Corporal,Sargent",
                             "--port", str(port), "--ws-port", str(ws), "--http-port", str(http)],
                            cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    lines: list[str] = []
    up = threading.Event()

    def read() -> None:
        for line in proc.stdout:
            lines.append(line.rstrip())
            if "Ctrl-C stops everything" in line:
                up.set()

    threading.Thread(target=read, daemon=True).start()
    kids: list[int] = []
    ok = True

    def check(what: str, good: bool) -> None:
        nonlocal ok
        if not good:
            print(f"    {what}")
            ok = False

    try:
        if not up.wait(LIMIT_S):
            print("    the launcher did not come up:\n      " + "\n      ".join(lines[-8:]))
            return fail(name)
        kids = children(proc.pid)
        check(f"expected a broker and two hosts under the launcher, found {len(kids)} processes", len(kids) == 3)
        check(f"no 'stations: Corporal, Sargent' line in {lines}", "run_layout: stations: Corporal, Sargent" in lines)
        want = f"run_layout: open http://127.0.0.1:{http}/web/?ws=ws://127.0.0.1:{ws}"
        check(f"the address line is not {want!r}: {[x for x in lines if ' open ' in x]}", want in lines)
        base = f"http://127.0.0.1:{http}"
        for path, code in (("/web/", 200), ("/build/Corporal.panel.json", 200), ("/build/panels.json", 200),
                           ("/.git/config", 404), ("/Makefile", 404), ("/build/Corporal.station", 404),
                           ("/tools/run_layout.py", 404)):
            got = status(base + path)
            check(f"GET {path} gave {got}, not {code}", got == code)
        got = raw_status(http, "/")
        check(f"GET / gave {got}, not the 302 to /web/", got == 302)
        for path in ("/web/../.git/config", "/web/%2e%2e/.git/config", "/build/../.git/config"):
            got = raw_status(http, path)
            check(f"GET {path} gave {got}, not 404", got == 404)
        topics = [f"/layout/SPCoast/codeline/{s}/indications" for s in ("Corporal", "Sargent")]
        got = retained_over_websockets(ws, topics)
        for t in topics:
            check(f"over WebSockets {t} reads {got.get(t)!r}", re.match(r"CHART [0-9a-f]{8} ", got.get(t, "")) is not None)
        proc.send_signal(signal.SIGHUP)
        try:
            proc.wait(15)
        except subprocess.TimeoutExpired:
            check("the launcher did not stop on SIGHUP", False)
        check(f"after SIGHUP the launcher exited {proc.returncode}", proc.returncode == 0)
        deadline = time.monotonic() + 5
        while any(alive(k) for k in kids) and time.monotonic() < deadline:
            time.sleep(0.05)
        left = [k for k in kids if alive(k)]
        check(f"after SIGHUP {len(left)} of its processes still run: {left}", not left)
        return passed(name) if ok else fail(name)
    finally:
        if proc.poll() is None:
            proc.kill()
        for k in kids:  # whatever a failed run left
            if alive(k):
                os.kill(k, signal.SIGKILL)


def passed(name: str) -> int:
    print(f"  ok    {name}")
    return 0


def fail(name: str) -> int:
    print(f"  FAIL  {name}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
