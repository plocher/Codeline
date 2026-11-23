"""CP Corporal end to end over a real broker.

Starts mosquitto on a free local port, then the station host (build/codeline_host, libmosquitto,
wall-clock time) for build/Corporal.station, then replays scenarios/corporal_walkthrough.script
with the scripted office (tools/codeline_office.py). The same script runs in process in
scenarios/corporal_walkthrough.cpp; here each topic's messages are checked in order (messages
from different clients may reach the office in either order, so the order across topics is
checked in process only).

Then the transport itself (CodeLineMQTT.md 2 and 5, MQTT.md):
  - every indication frame reaches the office retained, at QoS 1;
  - the field subscribes to its controls at QoS 0 (read from the broker's own log);
  - a clean stop (SIGTERM) leaves the indications, every appliance /state, the debug snapshot
    ({station}/debug/plant) and the clock ({simulation}/clock/Corporal) retained UNAVAILABLE;
  - losing the debug connection (another client takes its id) does not stop the host: the
    indications stay live and the debug snapshot comes back by itself;
  - losing a connection the station needs (the field's) stops the host with exit 3, and every
    retained topic of it reads UNAVAILABLE;
  - an abrupt stop (SIGKILL, another host) leaves the same, through the wills alone.

The time locking is shortened to 3 s through the host's --time-locking-ms. Every process is
stopped on the way out; a broker or host that does not come up fails the scenario at once
instead of hanging, and the whole run has a hard limit.
"""
from __future__ import annotations

import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import codeline_office  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "scenarios" / "corporal_walkthrough.script"
STATION = ROOT / "build" / "Corporal.station"
HOST = ROOT / "build" / "codeline_host"
TIME_LOCKING_MS = 3000
LIMIT_S = 240


def station_topics() -> tuple[dict[str, str], list[str]]:
    """The station file's channels, and every appliance's /state topic."""
    channels, states = {}, []
    for line in STATION.read_text().splitlines():
        f = line.split(" ")
        if f[0] == "channel":
            channels[f[1]] = f[2]
        elif f[0] in ("circuit", "lock", "mast"):
            states.append(f"{f[2]}/{f[3]}")
        elif f[0] == "switch":
            states.append(f"{f[3]}/{f[4]}")
        elif f[0] == "call" and len(f) == 4:    # a maintainer call bound to a lamp
            states.append(f"{f[2]}/{f[3]}")
    return channels, [f"{channels['appliances']}/{s}/state" for s in states]


def retained_all(url: str, topics: list[str], want: str | None, seconds: float) -> dict[str, str | None]:
    """The retained payload of each topic, from fresh subscriptions, polled until every one reads
    `want` (when given) or `seconds` pass."""
    deadline = time.monotonic() + seconds
    while True:
        probe = codeline_office.Link(url, f"codeline-probe-{time.monotonic_ns()}")
        codeline_office.subscribe_all(probe)
        time.sleep(0.5)
        got: dict[str, str | None] = {t: None for t in topics}
        with probe.lock:
            for t, p, r, *_ in probe.messages:
                if r and t in got:
                    got[t] = p
        probe.close()
        if want is None or all(v == want for v in got.values()) or time.monotonic() > deadline:
            return got


def all_unavailable(got: dict[str, str | None], when: str) -> bool:
    bad = {t: v for t, v in got.items() if v != "UNAVAILABLE"}
    for t, v in bad.items():
        print(f"    after {when}: {t} reads {v!r}, not UNAVAILABLE")
    return not bad


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_port(port: int, proc: subprocess.Popen, seconds: float) -> bool:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            return False
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
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


class Host:
    def __init__(self, url: str, initial: list[str]):
        cmd = [str(HOST), "--station", str(STATION), "--broker", url, "--time-locking-ms", str(TIME_LOCKING_MS)]
        for i in initial:
            cmd += ["--initial", i]
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.ready = threading.Event()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self) -> None:
        for line in self.proc.stdout:
            if line.startswith("ready"):
                self.ready.set()

    def wait_ready(self) -> bool:
        if self.ready.wait(10):
            return True
        stop(self.proc)
        print(f"    the host did not become ready: {(self.proc.stderr.read() if self.proc.stderr else '').strip()}")
        return False


def main() -> int:
    signal.signal(signal.SIGALRM, lambda *_: (_ for _ in ()).throw(TimeoutError(f"over {LIMIT_S} s")))
    signal.alarm(LIMIT_S)
    name = "CP Corporal Walkthrough over a real mosquitto: same traffic and indications, in order per topic"
    transport = "CP Corporal on a real mosquitto: retained QoS 1 indications, QoS 0 controls, UNAVAILABLE on stop and on loss"
    broker = office = None
    hosts: list[Host] = []
    tmp = Path(tempfile.mkdtemp(prefix="codeline-broker-"))
    try:
        mosquitto = shutil.which("mosquitto") or next(
            (p for p in ("/opt/homebrew/sbin/mosquitto", "/usr/local/sbin/mosquitto", "/usr/sbin/mosquitto")
             if Path(p).exists()), None)
        if mosquitto is None:
            print("    mosquitto (the broker) is not installed")
            return fail(name)
        if not HOST.exists() or not STATION.exists():
            print(f"    {HOST} or {STATION} is missing: run make build")
            return fail(name)
        channels, states = station_topics()
        if channels != codeline_office.CHANNELS:
            print(f"    the station file's channels {channels} are not the office's {codeline_office.CHANNELS}")
            return fail(name)
        port = free_port()
        log = tmp / "mosquitto.log"
        (tmp / "mosquitto.conf").write_text(
            f"listener {port} 127.0.0.1\nallow_anonymous true\npersistence false\n"
            f"log_type all\nlog_dest file {log}\n")
        broker = subprocess.Popen([mosquitto, "-c", str(tmp / "mosquitto.conf")],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        if not wait_port(port, broker, 5):
            stop(broker)
            err = broker.stderr.read() if broker.stderr else ""
            print(f"    the broker did not start on port {port}: {err.strip()}")
            return fail(name)
        url = f"127.0.0.1:{port}"

        office = codeline_office.Link(url, "codeline-office-scenario")
        codeline_office.subscribe_all(office)  # recording starts before the host does
        time.sleep(0.2)

        script = codeline_office.parse_script(SCRIPT)
        hosts.append(Host(url, [f"{w['topic']}={w['payload']}" for w in script if w["d"] == "initial"]))
        if not hosts[0].wait_ready():
            return fail(name)
        replay = codeline_office.Replay(url, "Corporal", TIME_LOCKING_MS, office)
        ok = replay.run(script)
        print(f"    {replay.differences} listed differences from the Walkthrough")
        walkthrough_ok = ok

        # Transport. Indications as delivered: retained, QoS 1 (CodeLineMQTT.md 2).
        ok = True
        with office.lock:
            frames = [m for m in office.messages if m[0] == replay.indications]
        wrong = [m for m in frames if not m[2] or m[4] != 1]
        for t, p, r, _, q in wrong[:3]:
            print(f"    indication delivered retain={r} qos={q}: {p}")
        ok = ok and bool(frames) and not wrong
        print(f"    {len(frames)} indication frames delivered, {len(wrong)} not retained at QoS 1")
        # Controls subscribed at QoS 0 (CodeLineMQTT.md 5 rule 1), from the broker's log.
        lines = log.read_text(errors="replace").splitlines()
        subs = [lines[i + 1].strip() for i, line in enumerate(lines[:-1])
                if line.endswith("Received SUBSCRIBE from Corporal-field")]
        wanted = f"{replay.controls} (QoS 0)"
        if not any(s.endswith(wanted) for s in subs):
            print(f"    the broker log shows the field subscribing {subs}, not {wanted!r}")
            ok = False
        # Every retained topic of the host: its indications, every appliance's /state, its debug
        # snapshot (DebugChannel.h) and its clock (SimClock.h).
        retained = [replay.indications] + states + [replay.indications.rsplit("/", 1)[0] + "/debug/plant",
                                                    f"{channels['simulation']}/clock/Corporal"]
        # Clean stop: the host says UNAVAILABLE itself, everywhere (exit 0). A will would read the
        # same, so the broker's log must show the host publishing each one (a will is not logged
        # as a received PUBLISH).
        before = len(log.read_text(errors="replace").splitlines())
        hosts[0].proc.send_signal(signal.SIGTERM)
        hosts[0].proc.wait(5)
        if hosts[0].proc.returncode != 0:
            print(f"    after SIGTERM the host exited {hosts[0].proc.returncode}")
            ok = False
        ok = all_unavailable(retained_all(url, retained, None, 0), "SIGTERM") and ok
        said = log.read_text(errors="replace").splitlines()[before:]
        for t in retained:
            if not any("Received PUBLISH from" in line and " r1," in line and f"'{t}'" in line
                       and line.endswith(f"({len('UNAVAILABLE')} bytes))") for line in said):
                print(f"    after SIGTERM the host never published UNAVAILABLE on {t} itself")
                ok = False
        def start_stated() -> Host | None:
            """Another host, running until every appliance, the station, its debug snapshot and its
            clock have stated themselves."""
            h = Host(url, [])
            hosts.append(h)
            if not h.wait_ready():
                return None
            deadline = time.monotonic() + 10
            live = retained_all(url, retained, None, 0)
            while any(v in (None, "UNAVAILABLE") for v in live.values()) and time.monotonic() < deadline:
                live = retained_all(url, retained, None, 0)
            if any(v in (None, "UNAVAILABLE") for v in live.values()):
                print(f"    a host never stated everything: {live}")
                return None
            return h

        # The debug connection is informative: taking it away (another client with its id) does
        # not stop the host, the indications stay live, and the snapshot comes back by itself.
        debug_plant = retained[-2]
        h = start_stated()
        if h is None:
            return fail(transport)
        codeline_office.Link(url, "Corporal-debug").close()
        gone = retained_all(url, [debug_plant], "UNAVAILABLE", 1).get(debug_plant)
        back = retained_all(url, [debug_plant, replay.indications], None, 0)
        deadline = time.monotonic() + 8
        while back[debug_plant] in (None, "UNAVAILABLE") and time.monotonic() < deadline:
            back = retained_all(url, [debug_plant, replay.indications], None, 0)
        if h.proc.poll() is not None:
            print(f"    losing the debug connection stopped the host (exit {h.proc.returncode})")
            ok = False
        if gone != "UNAVAILABLE":
            print(f"    after the debug connection was taken, {debug_plant} read {gone!r}, not its will")
            ok = False
        if back[debug_plant] in (None, "UNAVAILABLE") or back[replay.indications] in (None, "UNAVAILABLE"):
            print(f"    after the debug connection was taken and 8 s: {back}")
            ok = False
        # A connection the station needs: taking the field's stops the host (exit 3), and no
        # retained topic of it is left reading live.
        thief = codeline_office.Link(url, "Corporal-field")
        try:
            h.proc.wait(5)
        except subprocess.TimeoutExpired:
            print("    losing the field connection did not stop the host")
            ok = False
        if h.proc.returncode != 3:
            print(f"    after losing the field connection the host exited {h.proc.returncode}, not 3")
            ok = False
        ok = all_unavailable(retained_all(url, retained, "UNAVAILABLE", 5), "the field connection was lost") and ok
        thief.close()
        # Abrupt loss: another host runs until everything is stated, then is killed; the wills
        # alone must say UNAVAILABLE.
        h = start_stated()
        if h is None:
            return fail(transport)
        h.proc.kill()
        h.proc.wait(5)
        ok = all_unavailable(retained_all(url, retained, "UNAVAILABLE", 5), "SIGKILL") and ok
        r1 = passed(name) if walkthrough_ok else fail(name)
        r2 = passed(transport) if ok else fail(transport)
        return r1 or r2
    except TimeoutError as e:
        print(f"    the run took {e}")
        return fail(name)
    finally:
        signal.alarm(0)
        if office is not None:
            office.close()
        for h in hosts:
            stop(h.proc)
        stop(broker)
        shutil.rmtree(tmp, ignore_errors=True)


def passed(name: str) -> int:
    print(f"  ok    {name}")
    return 0


def fail(name: str) -> int:
    print(f"  FAIL  {name}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
