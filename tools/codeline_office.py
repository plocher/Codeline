#!/usr/bin/env python3
"""A scripted office on the Code Line MQTT Binding (docs/bindings/CodeLineMQTT.md).

It holds no model of its own: it reads the station's chart from the one deriver
(tools/codeline_chart.py), spells control frames with encode_frame, and reads indication
frames with decode_frame. Controls go out at QoS 0, unretained, and never while disconnected
(paho drops a QoS 0 publish when there is no connection; this office checks first).

    uv run python tools/codeline_office.py code   [--broker H:P] [--station S] LEVER=WORD ...
    uv run python tools/codeline_office.py watch  [--broker H:P] [--station S]
    uv run python tools/codeline_office.py replay [--broker H:P] [--station S] [--time-locking-ms N] SCRIPT

code     one CODE press: every lever named asserts that word (829=NORMAL 828=LEFT 830=LEFT
         827=RELEASE 1MC=CALL; 830=STOP centers 830); every other group is sent with nothing
         asserted ("unchanged"). Prints the frame and the field's answer.
watch    prints every indication frame of the station, decoded into lit lamps.
replay   replays a wire script (grammar: src/codeline/testing/WireScript.h) against a running
         station host and checks every message on the topics the script names, every appliance
         command and every station's indications, in order per topic; anything unscripted on
         them fails. Control frames in the script are decoded and re-encoded with the chart
         first, so the office sends exactly what its own codec spells. Exits 1 on the first
         mismatch.

The channels (appliances, codeline, simulation) are the stopgaps of tools/station_file.py
(STOPGAPS["channel.*"]), the same values the station file carries. A script topic
<channel>/<rest> is read under that channel.
"""
from __future__ import annotations

import sys
import threading
import time
from pathlib import Path

import paho.mqtt.client as mqtt
from paho.mqtt.subscribeoptions import SubscribeOptions

sys.path.insert(0, str(Path(__file__).resolve().parent))
import codeline_chart  # noqa: E402
import station_file  # noqa: E402

CHANNELS = {"appliances": station_file.stopgap("channel.appliances"),
            "codeline": station_file.project_codeline_channel(),
            "simulation": station_file.stopgap("channel.simulation")}
CHANNEL = CHANNELS["codeline"]


def wire_topic(topic: str) -> str:
    """'appliances/turnout/829/cmd' -> '{appliances channel}/turnout/829/cmd'."""
    name, _, rest = topic.partition("/")
    if name not in CHANNELS or not rest:
        raise SystemExit(f"codeline_office: topic {topic!r} names no channel of {sorted(CHANNELS)}")
    return f"{CHANNELS[name]}/{rest}"


def always_watched(topic: str) -> bool:
    """Every appliance command ({appliances}/+/+/cmd) and every station's indications."""
    for channel, levels, last in ((CHANNELS["appliances"], 3, "cmd"), (CHANNELS["codeline"], 2, "indications")):
        if topic.startswith(channel + "/"):
            rest = topic[len(channel) + 1:].split("/")
            if len(rest) == levels and rest[-1] == last:
                return True
    return False


def chart_of(station: str, project: Path | str = codeline_chart.DEFAULT_PROJECT) -> dict:
    result = codeline_chart.derive(codeline_chart.load(project))
    for st in result["stations"]:
        if st["station"] == station:
            return st
    raise SystemExit(f"codeline_office: no station {station!r}")


def topics(station: str) -> tuple[str, str]:
    base = f"{CHANNEL}/{station.replace(' ', '-')}"
    return f"{base}/controls", f"{base}/indications"


def lamps(chart: dict, frame: str) -> str:
    try:
        lit = codeline_chart.decode_frame(chart, "indication", frame)
    except codeline_chart.FrameError as e:
        return f"NOT CURRENT ({e})"
    if lit is None:
        return "NOT CURRENT (UNAVAILABLE)"
    return " ".join(f["aar"] for f in chart["indications"] if (f["appliance"], f["word"]) in lit) or "(all dark)"


class Link:
    """One paho client, MQTT 5, clean start, subscriptions with retain-as-published."""

    def __init__(self, broker: str, client_id: str):
        host, _, port = broker.rpartition(":")
        self.messages: list[tuple[str, str, bool, float, int]] = []  # topic, payload, retain, time, qos
        self.lock = threading.Lock()
        self.connected = threading.Event()
        self.c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id, protocol=mqtt.MQTTv5)
        self.c.on_connect = lambda c, u, f, rc, p=None: self.connected.set()
        self.c.on_message = self._on_message
        self.c.connect(host or "127.0.0.1", int(port), keepalive=10, clean_start=True)
        self.c.loop_start()
        if not self.connected.wait(5):
            raise SystemExit(f"codeline_office: no connection to {broker}")

    def _on_message(self, c, u, msg) -> None:
        with self.lock:
            self.messages.append((msg.topic, msg.payload.decode("utf-8", "replace"), bool(msg.retain), time.monotonic(),
                                  msg.qos))

    def subscribe(self, topic: str) -> None:
        self.c.subscribe(topic, options=SubscribeOptions(qos=1, retainAsPublished=True))

    def publish(self, topic: str, payload: str, retain: bool = False) -> None:
        if not self.c.is_connected():
            raise SystemExit("codeline_office: not connected; a control is never queued")
        self.c.publish(topic, payload, qos=1 if retain else 0, retain=retain)

    def close(self) -> None:
        self.c.disconnect()
        self.c.loop_stop()


# ------------------------------------------------------------------------------------ code

def code_frame(chart: dict, levers: list[str]) -> str:
    asserted = set()
    for item in levers:
        name, _, word = item.partition("=")
        asserted.add((name, word))
    return codeline_chart.encode_frame(chart, "control", asserted)


def cmd_code(broker: str, station: str, levers: list[str]) -> int:
    chart = chart_of(station)
    frame = code_frame(chart, levers)
    controls, indications = topics(station)
    link = Link(broker, "codeline-office")
    link.subscribe(indications)
    time.sleep(0.3)
    with link.lock:
        seen = len(link.messages)
    link.publish(controls, frame)
    print(f"sent      {frame}")
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        with link.lock:
            fresh = [m for m in link.messages[seen:] if m[0] == indications]
        if fresh:
            print(f"answer    {fresh[0][1]}\nlit       {lamps(chart, fresh[0][1])}")
            link.close()
            return 0
        time.sleep(0.05)
    print("no response: press CODE again")
    link.close()
    return 1


def cmd_watch(broker: str, station: str) -> int:
    chart = chart_of(station)
    _, indications = topics(station)
    link = Link(broker, "codeline-office-watch")
    link.subscribe(indications)
    shown = 0
    try:
        while True:
            with link.lock:
                fresh = link.messages[shown:]
                shown = len(link.messages)
            for _, payload, retained, *_ in fresh:
                print(f"{'R ' if retained else '  '}{payload}\n  lit: {lamps(chart, payload)}", flush=True)
            time.sleep(0.1)
    except KeyboardInterrupt:
        link.close()
        return 0


# ------------------------------------------------------------------------------------ replay

def parse_script(path: Path) -> list[dict]:
    out = []
    for number, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.rstrip()
        if not line or line.startswith("#"):
            continue
        directive, _, rest = line.partition(" ")
        w = {"n": number, "d": directive, "topic": "", "payload": "", "walkthrough": ""}
        if directive in ("step", "quiet", "mark", "elapsed"):
            w["payload"] = rest
        elif directive != "begin":
            w["topic"], _, w["payload"] = rest.partition(" ")
            if directive == "differs":
                w["payload"], _, w["walkthrough"] = w["payload"].partition(" | ")
        if directive not in ("initial", "step", "await", "begin", "retained", "send", "expect", "extra",
                             "differs", "quiet", "mark", "elapsed"):
            raise SystemExit(f"{path}:{number}: unknown directive {directive}")
        out.append(w)
    return out


class Replay:
    TIMEOUT_S = 20.0

    def __init__(self, broker: str, station: str, time_locking_ms: int, link: Link):
        self.broker, self.station, self.tl = broker, station, time_locking_ms
        self.chart = chart_of(station)
        self.controls, self.indications = topics(station)
        self.link = link
        self.consumed: set[int] = set()
        self.begin = 0
        self.marks: dict[str, float] = {}
        self.last_time = 0.0
        self.step = ""
        self.differences = 0

    def fail(self, w: dict, what: str) -> bool:
        print(f"    script line {w['n']} ({self.step}): {what}")
        return False

    def next_on(self, topic: str) -> int | None:
        with self.link.lock:
            for i in range(self.begin, len(self.link.messages)):
                if i not in self.consumed and self.link.messages[i][0] == topic:
                    return i
        return None

    def unmatched(self, w: dict, watched: set[str]) -> bool:
        ok = True
        with self.link.lock:
            msgs = list(enumerate(self.link.messages))
        for i, (topic, payload, *_) in msgs[self.begin:]:
            if (topic in watched or always_watched(topic)) and i not in self.consumed:
                ok = self.fail(w, f"unscripted {topic} \"{payload}\"")
                self.consumed.add(i)
        return ok

    def retained_now(self, topic: str) -> str | None:
        probe = Link(self.broker, f"codeline-office-probe-{time.monotonic_ns()}")
        probe.subscribe(topic)
        deadline = time.monotonic() + 3
        value = None
        while time.monotonic() < deadline and value is None:
            with probe.lock:
                for t, p, r, *_ in probe.messages:
                    if t == topic and r:
                        value = p
            time.sleep(0.02)
        probe.close()
        return value

    def office_frame(self, w: dict) -> str | None:
        """A control frame as this office spells it: the script's frame decoded and encoded again."""
        try:
            asserted = codeline_chart.decode_frame(self.chart, "control", w["payload"])
            frame = codeline_chart.encode_frame(self.chart, "control", asserted)
        except codeline_chart.FrameError as e:
            self.fail(w, f"the office's codec rejects the script's frame: {e}")
            return None
        if frame != w["payload"]:
            self.fail(w, f"the office spells it \"{frame}\"")
            return None
        return frame

    def run(self, script: list[dict]) -> bool:
        watched = {wire_topic(w["topic"]) for w in script if w["d"] in ("expect", "extra", "differs")}
        for w in script:
            if not self.line(w, watched):
                return False
        time.sleep(1.0)
        return self.unmatched(script[-1], watched)

    def line(self, w: dict, watched: set[str]) -> bool:
        d = w["d"]
        if d == "initial":
            return True
        topic = wire_topic(w["topic"]) if w["topic"] else ""
        if d == "step":
            self.step = w["payload"]
            return True
        if d == "begin":
            with self.link.lock:
                self.begin = len(self.link.messages)
            return True
        if d == "send":
            payload = w["payload"]
            if topic == self.controls:
                payload = self.office_frame(w)
                if payload is None:
                    return False
            self.link.publish(topic, payload)
            return True
        if d == "retained":
            got = self.retained_now(topic)
            return got == w["payload"] or self.fail(w, f"retained {w['topic']} is \"{got}\", script \"{w['payload']}\"")
        if d == "quiet":
            time.sleep(int(w["payload"]) / 1000)
            return self.unmatched(w, watched)
        if d == "mark":
            self.marks[w["payload"]] = self.last_time
            return True
        if d == "elapsed":
            e = (self.last_time - self.marks[w["payload"]]) * 1000
            if not (self.tl - 50 <= e <= self.tl + 1500):
                return self.fail(w, f"elapsed since {w['payload']} is {e:.0f} ms, time locking {self.tl} ms")
            return True
        deadline = time.monotonic() + self.TIMEOUT_S
        while True:
            i = self.next_on(topic)
            if i is not None:
                self.consumed.add(i)
                with self.link.lock:
                    _, payload, _, at, _ = self.link.messages[i]
                if d == "await":
                    if payload == w["payload"]:
                        return True
                    continue
                if topic == self.indications:
                    try:
                        codeline_chart.decode_frame(self.chart, "indication", payload)
                    except codeline_chart.FrameError as e:
                        return self.fail(w, f"the office's codec rejects the indication frame: {e}")
                if payload != w["payload"]:
                    return self.fail(w, f"on {w['topic']}\n        observed \"{payload}\"\n        script   \"{w['payload']}\"")
                self.last_time = at
                if d == "differs":
                    self.differences += 1
                    print(f"    LISTED DIFFERENCE line {w['n']} ({self.step}):\n        observed    {w['topic']} "
                          f"{payload}\n        Walkthrough {w['walkthrough']}")
                return True
            if time.monotonic() > deadline:
                return self.fail(w, f"nothing on {w['topic']} within {self.TIMEOUT_S:.0f} s")
            time.sleep(0.01)


def subscribe_all(link: Link) -> None:
    for channel in CHANNELS.values():
        link.subscribe(channel + "/#")


def replay(broker: str, station: str, script_path: Path, time_locking_ms: int, link: Link | None = None) -> tuple[bool, int]:
    """Replays a script. Pass a Link that has subscribed to every channel (subscribe_all) since
    before the station host started, so the boot is recorded too; otherwise one is made now."""
    own = link is None
    if own:
        link = Link(broker, "codeline-office-replay")
        subscribe_all(link)
    r = Replay(broker, station, time_locking_ms, link)
    ok = r.run(parse_script(script_path))
    if own:
        link.close()
    return ok, r.differences


# ------------------------------------------------------------------------------------ main

def main(argv: list[str]) -> int:
    args = list(argv)
    opts = {"--broker": "127.0.0.1:1883", "--station": "Corporal", "--time-locking-ms": "15000"}
    for key in list(opts):
        if key in args:
            i = args.index(key)
            opts[key] = args[i + 1]
            del args[i:i + 2]
    if not args or args[0] not in ("code", "watch", "replay"):
        print(__doc__)
        return 2
    if args[0] == "code":
        return cmd_code(opts["--broker"], opts["--station"], args[1:])
    if args[0] == "watch":
        return cmd_watch(opts["--broker"], opts["--station"])
    if len(args) != 2:
        print(__doc__)
        return 2
    ok, differences = replay(opts["--broker"], opts["--station"], Path(args[1]), int(opts["--time-locking-ms"]))
    print(f"{'ok' if ok else 'FAILED'}; {differences} listed differences from the Walkthrough")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
