#!/usr/bin/env python3
"""Run a simulated layout for the on-screen panel: a broker, one station host per station, and
the page.

    uv run python tools/run_layout.py [--stations Corporal,Gilroy,...] [--port 1883]
                                      [--ws-port 9001] [--http-port 8080] [--time-locking-ms N]

What it starts, all on 127.0.0.1, and stops again on Ctrl-C, SIGTERM or SIGHUP (the terminal closed):

- mosquitto, with a temporary configuration: an MQTT listener on --port for the station hosts
  and command-line tools, and a WebSockets listener on --ws-port for the page; anonymous, no
  persistence. If --port is not given and 1883 is taken (a broker of your own is running), a free
  port is used instead and printed; a taken --ws-port or --http-port is an error, since the page
  is told those.
- build/codeline_host for each station (--stations, default: every station of the project
  file). A station whose station file does not derive, or whose host does not come up, is
  reported and skipped. An appliance two stations both list (a track circuit between them) is
  one appliance on the wire: the first station's host simulates it, and each later host is
  started with --remote for it.
- an HTTP server for the page: /web/ and /build/*.json (the panel files) and nothing else of the
  repository. The address it prints carries ?ws=... when --ws-port is not the page's default 9001.

Exits 1 if it could not start, or if the broker stopped by itself.

Whatever is missing or out of date is built first through make (the host, the station files,
the panel files). Stopping is clean: each host is told to stop first, so every retained topic of
the layout reads UNAVAILABLE before the broker goes.
"""
from __future__ import annotations

import argparse
import functools
import http.server
import posixpath
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import codeline_chart  # noqa: E402

HOST = "127.0.0.1"
MOSQUITTO_PLACES = ("/opt/homebrew/sbin/mosquitto", "/usr/local/sbin/mosquitto", "/usr/sbin/mosquitto")


def say(text: str) -> None:
    print(f"run_layout: {text}", flush=True)


def port_free(port: int) -> bool:
    with socket.socket() as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind((HOST, port))
        except OSError:
            return False
    # Something may listen on every address without holding 127.0.0.1 exclusively.
    try:
        with socket.create_connection((HOST, port), timeout=0.2):
            return False
    except OSError:
        return True


def free_port() -> int:
    with socket.socket() as s:
        s.bind((HOST, 0))
        return s.getsockname()[1]


def wait_listening(port: int, proc: subprocess.Popen, seconds: float) -> bool:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline and proc.poll() is None:
        try:
            with socket.create_connection((HOST, port), timeout=0.2):
                return True
        except OSError:
            time.sleep(0.05)
    return False


def stop(proc: subprocess.Popen | None) -> None:
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(5)


def make(*targets: str) -> bool:
    return subprocess.run(["make", "-s", *targets], cwd=ROOT).returncode == 0


class StationHost:
    """One build/codeline_host. Its stdout is read for the "ready" line; its stderr is ours."""

    def __init__(self, station: str, broker: str, time_locking_ms: int | None, remote: list[str]):
        self.station = station
        cmd = [str(ROOT / "build" / "codeline_host"), "--station", str(ROOT / "build" / f"{station}.station"),
               "--broker", broker]
        for appliance in remote:
            cmd += ["--remote", appliance]
        if time_locking_ms:
            cmd += ["--time-locking-ms", str(time_locking_ms)]
        # Its own session: a Ctrl-C at the terminal reaches only this launcher, which stops things in order.
        self.proc = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE, text=True, start_new_session=True)
        self.ready = threading.Event()
        self.reported = False
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self) -> None:
        for line in self.proc.stdout:
            if line.startswith("ready"):
                self.ready.set()

    def wait_ready(self, seconds: float, stopping: threading.Event) -> bool:
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline and self.proc.poll() is None and not stopping.is_set():
            if self.ready.wait(0.1):
                return True
        return self.ready.is_set()


def appliances(station: str) -> list[str]:
    """Every appliance ("role/id") a station file binds."""
    out = []
    for line in (ROOT / "build" / f"{station}.station").read_text().splitlines():
        f = line.split(" ")
        if f[0] in ("circuit", "contact", "lock", "mast"):
            out.append(f"{f[2]}/{f[3]}")
        elif f[0] == "switch":
            out.append(f"{f[3]}/{f[4]}")
        elif f[0] == "call" and len(f) == 4:    # a maintainer call bound to a lamp
            out.append(f"{f[2]}/{f[3]}")
    return out


class Page(http.server.SimpleHTTPRequestHandler):
    """/web/ and /build/*.json of the repository and nothing else; never cached (a rebuilt panel
    file is fetched again); no request log."""

    def send_head(self):
        # Decided on the path as it will be resolved ("/web/../.git/config" is "/.git/config").
        path = posixpath.normpath(urllib.parse.unquote(self.path.split("?", 1)[0].split("#", 1)[0]))
        if not (path == "/web" or path.startswith("/web/") or re.fullmatch(r"/build/[^/]+\.json", path)):
            self.send_error(404, "Not found")
            return None
        return super().send_head()

    def end_headers(self) -> None:
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, *_: object) -> None:
        pass


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--stations", help="comma-separated station names (default: every station of the project file)")
    ap.add_argument("--port", type=int, help="the broker's MQTT port (default 1883, or a free one if that is taken)")
    ap.add_argument("--ws-port", type=int, default=9001, help="the broker's WebSockets port, for the page")
    ap.add_argument("--http-port", type=int, default=8080, help="the page's HTTP port")
    ap.add_argument("--time-locking-ms", type=int, help="passed to every host (shorter time locking for a demo)")
    args = ap.parse_args()

    mosquitto = shutil.which("mosquitto") or next((p for p in MOSQUITTO_PLACES if Path(p).exists()), None)
    if mosquitto is None:
        say("mosquitto (the broker) is not installed")
        return 1
    for name, port in (("--ws-port", args.ws_port), ("--http-port", args.http_port)):
        if not port_free(port):
            say(f"port {port} is in use; stop what holds it or pass another {name}")
            return 1
    port = args.port or 1883
    if not port_free(port):
        if args.port:
            say(f"port {port} is in use; stop what holds it or pass another --port")
            return 1
        port = free_port()
        say(f"port 1883 is in use (a broker of your own?); this layout's broker takes port {port} instead")

    if args.stations:
        stations = [s.strip() for s in args.stations.split(",") if s.strip()]
    else:
        stations = [st["station"] for st in codeline_chart.derive(codeline_chart.load(codeline_chart.DEFAULT_PROJECT))["stations"]]

    # Build what is missing. The station files one by one: one that does not derive is skipped.
    if not make("build/codeline_host"):
        say("the station host did not build")
        return 1
    skipped: dict[str, str] = {}
    for s in list(stations):
        if not make(f"build/{s}.station"):
            skipped[s] = "its station file does not derive (see above)"
            (ROOT / "build" / f"{s}.station.tmp").unlink(missing_ok=True)  # what the failed rule left
            stations.remove(s)
    if not stations:
        say("no station to run")
        return 1
    if not make("build/panels.json"):
        say("the panel files did not build; the page will have nothing to draw")

    stopping = threading.Event()
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):  # SIGHUP: the terminal went away
        signal.signal(sig, lambda *_: stopping.set())

    tmp = Path(tempfile.mkdtemp(prefix="codeline-layout-"))
    broker = httpd = None
    hosts: list[StationHost] = []
    try:
        (tmp / "mosquitto.conf").write_text(
            "allow_anonymous true\npersistence false\nlog_type error\nlog_type warning\n"
            f"listener {port} {HOST}\n"
            f"listener {args.ws_port} {HOST}\nprotocol websockets\n")
        # The broker's log goes to a file, never to a pipe nobody reads (a full pipe stops the broker).
        with open(tmp / "mosquitto.log", "w") as log:
            broker = subprocess.Popen([mosquitto, "-c", str(tmp / "mosquitto.conf")],
                                      stdout=subprocess.DEVNULL, stderr=log, start_new_session=True)
        if not (wait_listening(port, broker, 5) and wait_listening(args.ws_port, broker, 5)):
            stop(broker)
            say(f"the broker did not start on ports {port} and {args.ws_port} (WebSockets): "
                f"{(tmp / 'mosquitto.log').read_text().strip()}")
            return 1

        owner: dict[str, str] = {}  # appliance -> the station whose host simulates it
        for s in stations:
            if stopping.is_set():
                break
            mine = appliances(s)
            remote = [a for a in mine if a in owner]
            h = StationHost(s, f"{HOST}:{port}", args.time_locking_ms, remote)
            if h.wait_ready(15, stopping):
                hosts.append(h)
                for a in mine:
                    owner.setdefault(a, s)
                for a in remote:
                    say(f"{s} reads {a} from the host of {owner[a]}")
            else:
                stop(h.proc)
                skipped[s] = "its host did not come up (see above)"

        if stopping.is_set():
            return 0
        if not hosts:
            say("no station came up")
            return 1

        httpd = http.server.ThreadingHTTPServer((HOST, args.http_port), functools.partial(Page, directory=str(ROOT)))
        threading.Thread(target=httpd.serve_forever, daemon=True).start()

        say("stations: " + ", ".join(h.station for h in hosts))
        for s, why in skipped.items():
            say(f"skipped {s}: {why}")
        say(f"broker: mqtt://{HOST}:{port}   websockets: ws://{HOST}:{args.ws_port}")
        if not (ROOT / "web" / "index.html").exists():
            say("web/index.html is not there yet; the address below will list the directory")
        ws = "" if args.ws_port == 9001 else f"?ws=ws://{HOST}:{args.ws_port}"  # web/main.js reads ?ws=
        say(f"open http://{HOST}:{args.http_port}/web/{ws}")
        say("Ctrl-C stops everything")

        while not stopping.wait(0.5):
            if broker.poll() is not None:
                say("the broker stopped; stopping the layout")
                return 1
            for h in hosts:
                if h.proc.poll() is not None and not h.reported:
                    h.reported = True
                    say(f"the host of {h.station} stopped (exit {h.proc.returncode})")
        return 0
    finally:
        say("stopping")
        if httpd is not None:
            httpd.shutdown()
            httpd.server_close()
        for h in hosts:  # hosts first: each says UNAVAILABLE on its retained topics while the broker is there
            if h.proc.poll() is None:
                h.proc.terminate()
        for h in hosts:
            stop(h.proc)
        stop(broker)
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
