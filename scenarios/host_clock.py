"""Two real station hosts on a real broker: the clock they share, and a name they share.

Starts mosquitto on a free port, then build/codeline_host for Christopher and for Sargent. Each
has a track circuit named 01T; on the appliance channel they are two appliances, each fully
qualified by its interlocking (sensor/Christopher:01T, sensor/Sargent:01T).

The clock (src/codeline/station/SimClock.h), by commands on {simulation}/clock:
  - PAUSE: both hosts state PAUSED and T stands still;
  - STEP n: T moves by exactly n on each host, in about n ms of real time, and ends PAUSED;
  - RATE 100, RUN: plant time runs at a tenth of real time;
  - each change of mode is said on the station's debug channel ("clock PAUSED RATE 1000").
The two circuits named 01T:
  - the broker saw one client for each of sensor/Christopher:01T and sensor/Sargent:01T;
  - a SHUNT of Christopher's shows as occupied in Christopher's debug snapshot and not in Sargent's;
  - a host told --remote for an appliance its station does not have refuses to start.
"""
from __future__ import annotations

import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import corporal_broker as rig  # noqa: E402  (free_port, wait_port, stop; it puts tools/ on the path)
import codeline_office  # noqa: E402

ROOT = rig.ROOT
HOST = ROOT / "build" / "codeline_host"
SIM = codeline_office.CHANNELS["simulation"]
CODELINE = codeline_office.CHANNELS["codeline"]
APPLIANCES = codeline_office.CHANNELS["appliances"]
STATIONS = ("Christopher", "Sargent")
LIMIT_S = 90


class Host:
    def __init__(self, url: str, station: str, extra: list[str]):
        self.proc = subprocess.Popen([str(HOST), "--station", str(ROOT / "build" / f"{station}.station"), "--broker", url,
                                      "--time-locking-ms", "1000", *extra],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.ready = threading.Event()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self) -> None:
        for line in self.proc.stdout:
            if line.startswith("ready"):
                self.ready.set()


def main() -> int:
    signal.signal(signal.SIGALRM, lambda *_: (_ for _ in ()).throw(TimeoutError(f"over {LIMIT_S} s")))
    signal.alarm(LIMIT_S)
    clock_name = "two real hosts, one clock: PAUSE holds T, STEP n moves T by exactly n and ends PAUSED, RATE scales, and the debug channel says each change"
    remote_name = "two real hosts, each with a track circuit 01T: two appliances, and a shunt of one is not seen by the other"
    broker = watch = None
    hosts: list[Host] = []
    tmp = Path(tempfile.mkdtemp(prefix="codeline-clock-"))
    failed: set[str] = set()  # the scenario names with a failed check
    section = remote_name

    def check(what: str, good: bool) -> bool:
        if not good:
            print(f"    {what}")
            failed.add(section)
        return good

    try:
        mosquitto = shutil.which("mosquitto") or next(
            (p for p in ("/opt/homebrew/sbin/mosquitto", "/usr/local/sbin/mosquitto", "/usr/sbin/mosquitto")
             if Path(p).exists()), None)
        missing = [str(p) for p in (HOST, *(ROOT / "build" / f"{s}.station" for s in STATIONS)) if not p.exists()]
        if mosquitto is None or missing:
            print(f"    missing: {missing or 'mosquitto (the broker)'}: run make build")
            return fail(clock_name)
        port = rig.free_port()
        log = tmp / "mosquitto.log"
        (tmp / "mosquitto.conf").write_text(
            f"listener {port} 127.0.0.1\nallow_anonymous true\npersistence false\nlog_type all\nlog_dest file {log}\n")
        broker = subprocess.Popen([mosquitto, "-c", str(tmp / "mosquitto.conf")],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not rig.wait_port(port, broker, 5):
            print(f"    the broker did not start on port {port}")
            return fail(clock_name)
        url = f"127.0.0.1:{port}"

        # A host told an appliance of another station is remote refuses to start, and says why.
        bad = subprocess.run([str(HOST), "--station", str(ROOT / "build" / "Sargent.station"), "--broker", url,
                              "--remote", "sensor/no-such"], capture_output=True, text=True, timeout=20)
        check(f"--remote sensor/no-such: exit {bad.returncode}, said {bad.stderr.strip()!r}",
              bad.returncode == 2 and "remote sensor/no-such is no appliance of station Sargent" in bad.stderr)

        watch = codeline_office.Link(url, "codeline-clock-scenario")
        codeline_office.subscribe_all(watch)
        hosts.append(Host(url, "Christopher", []))
        hosts.append(Host(url, "Sargent", []))
        for h, s in zip(hosts, STATIONS):
            if not h.ready.wait(15):
                print(f"    the host of {s} did not become ready: {h.proc.stderr.read().strip() if h.proc.poll() is not None else ''}")
                return fail(clock_name)

        def last(topic: str) -> str | None:
            with watch.lock:
                return next((p for t, p, *_ in reversed(watch.messages) if t == topic), None)

        def clock(station: str) -> tuple[str, int, int] | None:
            """(RUNNING|PAUSED, rate, T) as the host last stated it."""
            f = (last(f"{SIM}/clock/{station}") or "").split()
            return (f[0], int(f[2]), int(f[4])) if len(f) == 5 and f[1] == "RATE" and f[3] == "T" else None

        def said(station: str, text: str) -> bool:
            with watch.lock:
                return any(t == f"{CODELINE}/{station}/debug" and p.split(" ", 1)[-1] == text for t, p, *_ in watch.messages)

        def until(cond, seconds: float) -> bool:
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                if cond():
                    return True
                time.sleep(0.02)
            return cond()

        def command(text: str) -> None:
            watch.publish(f"{SIM}/clock", text)

        section = clock_name
        check("the hosts did not state RUNNING RATE 1000 at start: " + str([clock(s) for s in STATIONS]),
              until(lambda: all((clock(s) or ("", 0, 0))[:2] == ("RUNNING", 1000) for s in STATIONS), 5))

        # ---- the shared appliance, while the clock runs --------------------------------------
        section = remote_name
        def shows(s: str, text: str) -> bool:
            return f"\ncircuit 01T {text}\n" in "\n" + (last(f"{CODELINE}/{s}/debug/plant") or "")

        check("Sargent's snapshot never showed circuit 01T clear", until(lambda: shows("Sargent", "clear"), 8))
        watch.publish(f"{SIM}/sensor/Christopher:01T", "SHUNT")
        check(f"Christopher's snapshot never showed circuit 01T occupied:\n{last(f'{CODELINE}/Christopher/debug/plant')}",
              until(lambda: shows("Christopher", "occupied"), 8))
        time.sleep(1.0)
        check("Sargent's 01T followed Christopher's shunt", shows("Sargent", "clear"))
        watch.publish(f"{SIM}/sensor/Christopher:01T", "(SHUNT)")
        for s in STATIONS:
            connects = [line for line in log.read_text(errors="replace").splitlines()
                        if "New client connected" in line and f" as {APPLIANCES}/sensor/{s}:01T " in line]
            check(f"the broker saw {len(connects)} clients for sensor/{s}:01T, not 1", len(connects) == 1)

        # ---- the clock -----------------------------------------------------------------------
        section = clock_name
        command("PAUSE")
        check("PAUSE: not both PAUSED: " + str([clock(s) for s in STATIONS]),
              until(lambda: all((clock(s) or ("",))[0] == "PAUSED" for s in STATIONS), 3))
        held = {s: clock(s) for s in STATIONS}
        time.sleep(1.5)
        check(f"paused 1.5 s: {held} became {[clock(s) for s in STATIONS]}", all(clock(s) == held[s] for s in STATIONS))
        for s in STATIONS:
            check(f"{s} never said 'clock PAUSED RATE 1000' on its debug channel", until(lambda s=s: said(s, "clock PAUSED RATE 1000"), 3))

        t0 = time.monotonic()
        command("STEP 500")
        check("STEP 500: the hosts never stated RUNNING during the step", until(
            lambda: all((clock(s) or ("",))[0] == "RUNNING" for s in STATIONS), 1))
        done = until(lambda: all(clock(s) == ("PAUSED", 1000, held[s][2] + 500) for s in STATIONS), 5)
        took = time.monotonic() - t0
        check(f"STEP 500 from {held}: ended at {[clock(s) for s in STATIONS]}", done)
        check(f"STEP 500 took {took:.2f} s of real time, not about half a second", 0.4 <= took <= 1.5)
        time.sleep(0.5)
        check("after the step T moved on", all(clock(s) == ("PAUSED", 1000, held[s][2] + 500) for s in STATIONS))

        command("RATE 100")
        command("RUN")
        check("RATE 100, RUN: not both RUNNING RATE 100", until(
            lambda: all((clock(s) or ("", 0))[:2] == ("RUNNING", 100) for s in STATIONS), 3))
        for s in STATIONS:
            check(f"{s} never said 'clock RUNNING RATE 100' on its debug channel", until(lambda s=s: said(s, "clock RUNNING RATE 100"), 3))
        time.sleep(3.0)
        command("PAUSE")
        until(lambda: all((clock(s) or ("",))[0] == "PAUSED" for s in STATIONS), 3)
        for s in STATIONS:
            moved = clock(s)[2] - (held[s][2] + 500)
            check(f"{s}: about 3 s at RATE 100 moved plant time {moved} ms, not about 300", 250 <= moved <= 450)
        command("RATE 1000")
        command("RUN")

        return max((fail if n in failed else passed)(n) for n in (clock_name, remote_name))
    except TimeoutError as e:
        print(f"    the run took {e}")
        return fail(clock_name)
    finally:
        signal.alarm(0)
        if watch is not None:
            watch.close()
        for h in hosts:
            rig.stop(h.proc)
        rig.stop(broker)
        shutil.rmtree(tmp, ignore_errors=True)


def passed(name: str) -> int:
    print(f"  ok    {name}")
    return 0


def fail(name: str) -> int:
    print(f"  FAIL  {name}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
