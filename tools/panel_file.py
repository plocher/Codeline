#!/usr/bin/env python3
"""Write a station's panel file: everything the on-screen panel (web/) shows for one station.

The page reads only build/panels.json and build/<Station>.panel.json. It derives nothing, holds
no plant and knows no name, number or token: every string it sends or compares is in these files.

    uv run python tools/panel_file.py [project.json] --station NAME     # one panel file, to stdout
    uv run python tools/panel_file.py [project.json] --all DIR          # every station + DIR/panels.json

Exits 1 (writing nothing for that station) when the station's chart or station file has problems.
With --all, a station with problems is left out and listed in panels.json under `notDerived`.

Nothing is derived twice:

- the chart (functions, order, tokens, groups, identity) is tools/codeline_chart.py's;
- the appliances (role, id, channels) are the records of tools/station_file.py's station file;
- the track drawing is tools/trackplan.py's;
- this tool only lays those out as a panel: which lever shows which functions (through each chart
  function's `sources`, which name the lever or lamp of the office description it reflects), and
  which drawn element is lit by which indication or appliance.

A panel file (JSON):

    writer, station, interlocking, source {project, sha256}
    chartId, encoding
    topics      {controls, indications, debug, plant, clockCommand, clockState}   full MQTT topics
    channels    {codeline, appliances, simulation}
    frame       {open: [words that open a frame], separator, dropped: [prefix, suffix],
                 unavailable, identity, rule}   how a frame is spelled; `rule` says it in words;
                 `identity` is the pattern of a chart identity
    office      {answerTimeoutMs}        how long after CODE the office waits for an indication frame
    controls, indications
                the chart, in chart order: {class, appliance, word, aar, group}
                (group > 0: a positive-control group; at most one of its functions asserted)
    columns     panel columns of this station, left to right:
                {unit, number, label, code: bool, levers: [...]}; a lever is
                {style, name, home, positions: [{label, word, asserts: [control index], expects:
                 indication index | null}], lamps: [{label, word, indication, color, inverted}]}
    board       {lamps: [{name, color, board, indications: [index]}]}   the office's model-board lamps
    unplaced    {controls: [index], indications: [index]}   chart functions no lever position
                asserts / no lever lamp or drawn element shows
    drawing     tools/trackplan.py's drawing(); each tagged element also has
                office {occupied | lined | lit | clear: {station, i, color?}}   the indication that lights it
                                                          (a mast has only `clear`: its lever's LEFT or RIGHT
                                                          indication; the code line has no per-mast function)
                field  {appliance | circuit: key}                        the appliance that colours it
    appliances  [{key, kind, name, role, id, operated?, topics {state, cmd, event, simulate},
                  shows {state term: look}, click {term, asserted, otherwise}, actions [{label,
                  payload}], power {off, on}}]
    unavailable the word an indication frame or an appliance /state reads when its source is gone
    stopgaps    what the project file did not say and what was used instead

The frame-building rule (frame.rule) is lever_frame() below; scenarios/panel_files.py holds it
against codeline_chart.encode_frame, and web/office.js is the same few lines.
"""
from __future__ import annotations

import re
import hashlib
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import codeline_chart  # noqa: E402  (the one chart deriver)
import station_file  # noqa: E402  (the one plant extraction)
import trackplan  # noqa: E402  (the one trackplan geometry)

WRITER = "panel_file/1"

# How a lever of each Style presents its functions on a US&S-style panel: its positions left to
# right (label, the control word the position asserts, the indication word that answers it), the
# position it rests in, and its lamps (label, indication word, colour). A signal lever's centre
# lamp is its STOP lamp: the chart gives a signal lever LEFT, RIGHT and TIME indications and no
# STOP, so the lamp is lit steady while neither LEFT nor RIGHT is asserted (`litUnless`) and
# flashes while TIME is (time locking running, or STOP not yet proven). This is presentation; the
# words themselves are the chart's (codeline_chart.WORD_ORDER), and a word the chart lacks for a
# lever drops that position's control or that lamp.
LEVER_FORMS = {
    "switch": {
        "positions": [("N", "NORMAL", "NORMAL"), ("R", "REVERSE", "REVERSE")], "home": "N",
        "lamps": [("N", "NORMAL", "green"), ("R", "REVERSE", "amber")]},
    "signal": {
        "positions": [("L", "LEFT", "LEFT"), ("STOP", "STOP", None), ("R", "RIGHT", "RIGHT")], "home": "STOP",
        "lamps": [("L", "LEFT", "green"), ("STOP", "TIME", "red"), ("R", "RIGHT", "green")],
        "stop": {"lamp": "TIME", "unless": ("LEFT", "RIGHT")}},
    "lock": {
        "positions": [("LOCK", None, None), ("REL", "RELEASE", "RELEASED")], "home": "LOCK",
        "lamps": [("REL", "RELEASED", "amber"), ("REQ", "REQUEST", "white")]},
    "maintainer_call": {
        "positions": [("OFF", None, None), ("CALL", "CALL", "CALL")], "home": "OFF",
        "lamps": [("CALL", "CALL", "white")]},
}
# The label of a lamp the office description inverts (Indication.<pin> = "!token"): lit when dropped.
INVERTED_LABEL = {("lock", "RELEASED"): "LOCK"}
LEVER_ORDER = ("switch", "signal", "lock", "maintainer_call")

# ------------------------------------------------------------------------------------ STOPGAPS
# What the panel needs that neither the project file nor the station file states. Each is
# repeated in every panel file (`stopgaps`).
STOPGAPS = {
    "lever.positions": (
        "LEVER_FORMS",
        "The office description gives a lever a Style and its pins, not its positions: that a signal "
        "lever has LEFT / centre / RIGHT and that centre sends STOP, that a lock lever rests locked, or "
        "which position a lever rests in. tools/panel_file.py LEVER_FORMS says it per Style."),
    "lamp.stop": (
        "lit unless LEFT or RIGHT; flashes while TIME",
        "The chart has no STOP indication for a signal lever (the office description's Indication.STOP pin "
        "carries TEK). The centre lamp is derived at the office: steady red while no signal of the lever is "
        "indicated clear, flashing while TEK is asserted. No document says so yet."),
    "lamp.colors": (
        "LEVER_FORMS",
        "A lever's own lamps have no colour in the project file (model-board lamps do: given.Color). "
        "Green / amber / red / white per word, in LEVER_FORMS."),
    "board.placement": (
        "the trackplan",
        "A model-board lamp has a Name, a Color and the interlocking whose panel it sits on, but no "
        "place on a board and no link to a piece of track. The board is drawn from the interlocking's "
        "trackplan geometry; a track-circuit lamp lights the track drawn for that circuit, and a lamp "
        "whose function has no drawn element is listed in `unplaced` and shown as a loose jewel."),
    "mast.lever": (
        "<number><E|W>...: lever <number>, E is RIGHT, W is LEFT",
        "The code line has no per-mast function, so the office cannot know an aspect. The model board shows "
        "a mast red while its lever's signals in its direction are not indicated clear and unlit while they "
        "are (every mast of that lever and direction alike, so no colour is claimed for any one of them). Which lever and direction is read from the "
        "mast's name. No document says so yet."),
    "mast.aspects": (
        "by permissiveness",
        "Neither file says which lights a mast shows for an indication (that is MastCore's rulebook). "
        "The field view colours every head of a mast alike: red at STOP, green from DIVERGING_CLEAR up, "
        "yellow between (station_file.PERMISSIVENESS), and prints the indication."),
    "simulation.words": (
        "SHUNT, NORMAL/REVERSE, SW<k>, BTN<k>, POWER",
        "Owner: src/codeline/station/StationHost.h (header comment and simulate()). The physical-world "
        "payloads are the host's, not a document's; that a `contact` is tripped with SHUNT is read from "
        "its code. Copied in tools/panel_file.py SIMULATION_WORDS."),
    "state.words": (
        "ACTIVE, NORMAL/REVERSE, SW<k>, LIT, LAMP<k>, ON",
        "Owner: src/codeline/generated/MqttTables.h (docs/bindings/MQTT.md section 3). The /state words "
        "the field view reads. Copied in tools/panel_file.py STATE_WORDS."),
    "clock.words": (
        "PAUSE, RUN, STEP <ms>, RATE <permille>; RUNNING | PAUSED RATE <permille> T <ms>; step 1..3600000, rate 1..10000",
        "Owner: src/codeline/station/SimClock.h. Copied in tools/panel_file.py CLOCK_WORDS, CLOCK_LIMITS."),
    "debug.clock.topics": (
        "{codeline}/{station}/debug, .../debug/plant, {simulation}/clock, {simulation}/clock/{station}",
        "Owners: src/codeline/station/DebugChannel.h and SimClock.h. No document yet."),
    "topic.station": (
        "each space of the station name replaced by '-'",
        "Owners: src/codeline/station/CodeLineField.h stationTopic() and tools/codeline_office.py topics() "
        "(docs/bindings/CodeLineMQTT.md section 1). Copied in tools/panel_file.py topic_name(); importing "
        "the office tool would bring an MQTT client and a channel fixed at import into a file writer."),
    "frame.identity": (
        "^[0-9a-f]{8}$",
        "Owner: docs/bindings/CodeLineMQTT.md section 2 (chart-id = 8 lowercase hex digits). What tells a "
        "frame of another chart (out of sync) from text that is no frame (malformed)."),
    "office.answerTimeoutMs": (
        3000,
        "CodeLineMQTT.md section 5 says an office shows `no response` when no indication frame follows a "
        "CODE within a timeout, and gives no value. 3 s, as tools/codeline_office.py waits."),
}

# Words whose owner is C++ only. Each is listed in STOPGAPS with its owner, and
# scenarios/panel_files.py greps the owner for the same words, so a rename there fails here.
SIMULATION_WORDS = {"shunt": "SHUNT", "normal": "NORMAL", "reverse": "REVERSE", "contact": "SW", "button": "BTN",
                    "power": "POWER"}                      # src/codeline/station/StationHost.h simulate()
STATE_WORDS = {"active": "ACTIVE", "normal": "NORMAL", "reverse": "REVERSE", "contact": "SW", "lit": "LIT",
               "lamp": "LAMP", "on": "ON"}
#                                                            src/codeline/generated/MqttTables.h (docs/bindings/MQTT.md 3)
CLOCK_WORDS = {"pause": "PAUSE", "run": "RUN", "step": "STEP", "rate": "RATE", "running": "RUNNING",
               "paused": "PAUSED", "time": "T"}             # src/codeline/station/SimClock.h
CLOCK_LIMITS = {"realTime": 1000, "maxRate": 10000, "maxStepMs": 3600000}      # SimClock.h kMaxRate, kMaxStepMs
ANSWER_TIMEOUT_MS = 3000


def dropped(word: str) -> str:
    """The negated form of a TOKEN on the appliance and simulation channels (MQTT.md section 2)."""
    return f"({word})"


# How an appliance's /state reads on the field view, and how the physical world acts on it.
SENSOR_SHOWS = {STATE_WORDS["active"]: "occupied"}
TURNOUT_SHOWS = {STATE_WORDS["normal"]: "normal", STATE_WORDS["reverse"]: "reverse"}
POWER = {"off": dropped(SIMULATION_WORDS["power"]), "on": SIMULATION_WORDS["power"]}

for _form in LEVER_FORMS.values():                          # every word of a form is a chart word
    for _label, _control, _answer in _form["positions"]:
        assert all(w is None or any(w in ws for ws in codeline_chart.WORD_ORDER.values()) for w in (_control, _answer))


def stopgap(key: str):
    return STOPGAPS[key][0]


def topic_name(station: str) -> str:
    """A station's name in a topic (docs/bindings/CodeLineMQTT.md section 1)."""
    return station.replace(" ", "-")


def file_name(station: str) -> str:
    return topic_name(station).replace(":", "-") + ".panel.json"


# ---------------------------------------------------------------------------- the frame rule

FRAME_RULE = ("A control frame is the words of `open`, then one term per entry of `controls`, in order, "
              "joined by `separator`. A term is the entry's `aar` when its index is in the `asserts` of the "
              "position some lever stands in, else `aar` between `dropped[0]` and `dropped[1]`.")


def lever_frame(panel: dict, positions: dict[str, str]) -> str:
    """The control frame one CODE press sends: `positions` maps "<style> <name>" to a position
    label; a lever not named stands in its `home` position. This is frame.rule, as code."""
    asserted: set[int] = set()
    for column in panel["columns"]:
        for lever in column["levers"]:
            label = positions.get(f"{lever['style']} {lever['name']}", lever["home"])
            position = next(p for p in lever["positions"] if p["label"] == label)
            asserted.update(position["asserts"])
    f = panel["frame"]
    terms = [c["aar"] if i in asserted else f["dropped"][0] + c["aar"] + f["dropped"][1]
             for i, c in enumerate(panel["controls"])]
    return f["separator"].join(f["open"] + terms)


# ------------------------------------------------------------------------------------ helpers

def _device(model: dict, source: str) -> dict | None:
    """The office device a chart function's source path names (officeUnits/<unit>/devices/<i>)."""
    parts = source.split("/")
    if len(parts) == 4 and parts[0] == "officeUnits" and parts[2] == "devices":
        return model["officeUnits"][parts[1]]["devices"][int(parts[3])]
    return None


def _records(lines: list[str]) -> dict[str, list[list[str]]]:
    """The station file's records by keyword (grammar: tools/station_file.py)."""
    out: dict[str, list[list[str]]] = {}
    for line in lines:
        if line and not line.startswith("#"):
            fields = line.split(" ")
            out.setdefault(fields[0], []).append(fields[1:])
    return out


def _options(fields: list[str]) -> dict[str, str]:
    return dict(f.split("=", 1) for f in fields if "=" in f)


class Index:
    """Every chart function of the project, by where the plant has it: (interlocking, class,
    appliance, word) -> {station, i}. A board shows track of a neighbour, lit by the neighbour."""

    def __init__(self, model: dict, charts: list[dict]):
        self.model = model
        self.refs: dict[tuple, dict] = {}
        for ch in charts:
            for i, f in enumerate(ch["indications"]):
                ref = {"station": ch["station"], "i": i}
                color = self._color(f)
                if color:
                    ref["color"] = color
                self.refs.setdefault((ch["interlocking"], f["class"], f["appliance"], f["word"]), ref)

    def _color(self, fn: dict) -> str | None:
        for source in fn["sources"]:
            given = (_device(self.model, source) or {}).get("given", {})
            if given.get("Appliance") == "lamp" and given.get("Color"):
                return given["Color"]
        return None

    def ref(self, ix_name: str, cls: str, name: str | None, word: str) -> dict | None:
        if not name:
            return None
        if ":" in name:                                  # a neighbour's circuit, as trackplan names it
            ix_name, _, name = name.partition(":")
        found = self.refs.get((ix_name, cls, name, word))
        return dict(found) if found else None


# ------------------------------------------------------------------------------------- panel

def _levers(model: dict, chart: dict) -> tuple[list[dict], set[int], set[int], list[str]]:
    """The panel columns of one station, and the control / indication indices its levers cover."""
    problems: list[str] = []
    by_device: dict[str, dict] = {}
    for direction, bucket in (("control", "controls"), ("indication", "indications")):
        for i, f in enumerate(chart[bucket]):
            for source in f["sources"]:
                dev = _device(model, source)
                given = (dev or {}).get("given", {})
                if given.get("Appliance") == "lever" and given.get("Style") == f["class"]:
                    by_device.setdefault(source, {"dev": dev, "control": {}, "indication": {}})[direction][f["word"]] = i

    columns: dict[tuple[str, int], dict] = {}

    def column(dev: dict) -> dict | None:
        unit, _, number = dev.get("relations", {}).get("column", "").partition(":")
        if not number.isdigit():
            return None
        key = (unit, int(number))
        if key not in columns:
            given = model["officeUnits"][unit].get("columns", {}).get(number, {}).get("given", {})
            columns[key] = {"unit": unit, "number": int(number), "label": given.get("CP Name", ""),
                            "code": False, "levers": []}
        return columns[key]

    placed_c: set[int] = set()
    placed_i: set[int] = set()
    for source, entry in by_device.items():
        dev = entry["dev"]
        style, name = dev["given"]["Style"], dev["given"].get("Name", "")
        col = column(dev)
        form = LEVER_FORMS.get(style)
        if col is None or form is None:
            problems.append(f"{source}: lever {name} has no panel column or no form for Style {style!r}")
            continue
        inverted = set()
        for key, value in dev["given"].items():           # Indication.<pin> = "!<token>": the lamp is inverted
            if key.startswith("Indication.") and value.startswith("!") and value[1:].startswith(name):
                word = codeline_chart.SUFFIXES.get(style, {}).get(value[1 + len(name):])
                if word and word[0] == "indication":
                    inverted.add(word[1])
        positions = []
        for label, control, answer in form["positions"]:
            if control is not None and control not in entry["control"]:
                continue                                    # the chart has no such control for this lever
            asserts = [entry["control"][control]] if control else []
            positions.append({"label": label, "word": control, "asserts": asserts,
                              "expects": entry["indication"].get(answer) if answer else None})
            placed_c.update(asserts)
        lamps = []
        for label, word, color in form["lamps"]:
            if word in entry["indication"]:
                inv = word in inverted
                lamps.append({"label": INVERTED_LABEL.get((style, word), label) if inv else label, "word": word,
                              "indication": entry["indication"][word], "color": color, "inverted": inv})
                placed_i.add(entry["indication"][word])
                stop = form.get("stop")
                if stop and stop["lamp"] == word and all(w in entry["indication"] for w in stop["unless"]):
                    lamps[-1]["litUnless"] = [entry["indication"][w] for w in stop["unless"]]
        col["levers"].append({"style": style, "name": name, "home": form["home"], "positions": positions,
                              "lamps": lamps})

    # The CODE button's column: a lever device of Style code, in one of this station's columns.
    for unit_name, unit in model.get("officeUnits", {}).items():
        for dev in unit.get("devices", []):
            if dev.get("given", {}).get("Appliance") == "lever" and dev["given"].get("Style") == "code":
                u, _, number = dev.get("relations", {}).get("column", "").partition(":")
                if number.isdigit() and (u, int(number)) in columns:
                    columns[(u, int(number))]["code"] = True

    out = [columns[k] for k in sorted(columns)]
    for col in out:
        col["levers"].sort(key=lambda lv: (LEVER_ORDER.index(lv["style"]), codeline_chart._natural(lv["name"])))
    return out, placed_c, placed_i, problems


def _appliances(records: dict, channels: dict) -> list[dict]:
    out = []

    def add(kind: str, name: str, role: str, ident: str, **more) -> None:
        base = f"{channels['appliances']}/{role}/{ident}"
        out.append({"key": f"{role}/{ident}", "kind": kind, "name": name, "role": role, "id": ident,
                    "topics": {"state": base + "/state", "cmd": base + "/cmd", "event": base + "/event",
                               "simulate": f"{channels['simulation']}/{role}/{ident}"},
                    "power": dict(POWER), **more})

    shunt, normal, reverse = (SIMULATION_WORDS[k] for k in ("shunt", "normal", "reverse"))
    for name, role, ident in (r[:3] for r in records.get("circuit", [])):
        add("track_circuit", name, role, ident, shows=dict(SENSOR_SHOWS),
            click={"term": STATE_WORDS["active"], "asserted": dropped(shunt), "otherwise": shunt},
            actions=[{"label": "Shunt", "payload": shunt}, {"label": "Unshunt", "payload": dropped(shunt)}])
    for r in records.get("switch", []):
        name, operated, role, ident = r[:4]
        add("switch", name, role, ident, operated=operated, shows=dict(TURNOUT_SHOWS),
            click={"term": STATE_WORDS["normal"], "asserted": reverse, "otherwise": normal},
            actions=[{"label": "Throw normal", "payload": normal}, {"label": "Throw reverse", "payload": reverse}])
    for r in records.get("lock", []):
        name, role, ident = r[:3]
        opts = _options(r[3:])
        nv = dict(item.split(":", 1) for item in opts.get("nv", "").split(",") if ":" in item)
        contacts = [f"{SIMULATION_WORDS['contact']}{k}" for k in range(1, int(nv.get("lever.inputs", "0")) + 1)]
        buttons = [f"{SIMULATION_WORDS['button']}{k}" for k in range(1, int(nv.get("button.inputs", "0")) + 1)]
        actions = []
        for c in contacts:
            actions += [{"label": f"Open lock ({c})", "payload": c}, {"label": f"Close lock ({c})", "payload": dropped(c)}]
        for b in buttons:
            actions += [{"label": f"Press request ({b})", "payload": b}, {"label": f"Release request ({b})", "payload": dropped(b)}]
        lamps = [f"{STATE_WORDS['lamp']}{k}" for k in range(1, int(nv.get("lever.outputs", "0")) + 1)]
        # What a crew has at the switch (MQTT.md section 5a): the lock itself, its request button, its
        # release lamp, and the hand switches it locks. The field view draws this as one box.
        local = {"contact": contacts[0] if contacts else None, "button": buttons[0] if buttons else None,
                 "lamp": lamps[0] if lamps else None}
        add("lock", name, role, ident, covers=[s for s in opts.get("switches", "").split(",") if s],
            local=local, shows={c: "open" for c in contacts},
            click={"term": contacts[0], "asserted": dropped(contacts[0]), "otherwise": contacts[0]} if contacts else None,
            actions=actions)
    for name, role, ident in (r[:3] for r in records.get("mast", [])):
        shows = {word: "stop" if rank == 0 else "clear" if rank >= station_file.PERMISSIVENESS["DIVERGING_CLEAR"] else "caution"
                 for word, rank in station_file.PERMISSIVENESS.items()}
        shows[dropped(STATE_WORDS["lit"])] = "dark"
        add("mast", name, role, ident, shows=shows, click=None, actions=[])
    for r in records.get("call", []):
        if len(r) >= 3:                                    # call <name> <role> <id>: a lamp at the field
            add("maintainer_call", r[0], r[1], r[2], shows={STATE_WORDS["on"]: "lit"}, click=None, actions=[])
    for name, role, ident in (r[:3] for r in records.get("contact", [])):
        add("contact", name, role, ident, shows=dict(SENSOR_SHOWS),
            click={"term": STATE_WORDS["active"], "asserted": dropped(shunt), "otherwise": shunt},
            actions=[{"label": "Trip", "payload": shunt}, {"label": "Restore", "payload": dropped(shunt)}])
    return out


MAST_NAME = re.compile(r"(\d+)([EW])")                   # STOPGAP mast.lever


def _bind(model: dict, chart: dict, index: Index, appliances: list[dict], drawing: dict) -> None:
    """Give every tagged element the indication that lights it (office) and the appliance that
    colours it (field)."""
    ix_name = chart["interlocking"]
    by_kind = {(a["kind"], a["name"]): a["key"] for a in appliances}
    for el in drawing["elements"]:
        tag = el.get("tag")
        if not tag:
            continue
        kind, name = tag["kind"], tag["name"]
        office: dict = {}
        field: dict = {}

        def put(slot: str, ref: dict | None) -> None:
            if ref:
                office[slot] = ref

        if kind == "track_circuit":
            put("occupied", index.ref(ix_name, "track_circuit", name, "OCCUPIED"))
            if ("track_circuit", name) in by_kind:
                field["circuit"] = by_kind[("track_circuit", name)]
        elif kind == "switch":
            if tag.get("part") == "lamp":                  # the lamp of the switch symbol: its electric lock
                put("lit", index.ref(ix_name, "lock", name, "RELEASED"))
                if ("lock", name) in by_kind:
                    field["appliance"] = by_kind[("lock", name)]
            else:
                put("occupied", index.ref(ix_name, "track_circuit", tag.get("circuit"), "OCCUPIED"))
                if tag.get("part") in ("N", "R"):
                    put("lined", index.ref(ix_name, "switch", name, "NORMAL" if tag["part"] == "N" else "REVERSE"))
                if ("switch", name) in by_kind:
                    field["appliance"] = by_kind[("switch", name)]
                if ("track_circuit", tag.get("circuit")) in by_kind:
                    field["circuit"] = by_kind[("track_circuit", tag["circuit"])]
        elif kind == "mast":
            # The code line carries no per-mast function (README.CodeLine sections 3, 9): the office
            # knows only whether the lever's signals in the mast's direction are indicated clear.
            m = MAST_NAME.match(name)
            if m:
                put("clear", index.ref(ix_name, "signal", m.group(1), "RIGHT" if m.group(2) == "E" else "LEFT"))
            if ("mast", name) in by_kind:
                field["appliance"] = by_kind[("mast", name)]
        elif kind == "maintainer_call":
            put("lit", index.ref(ix_name, "maintainer_call", name, "CALL"))
            if ("maintainer_call", name) in by_kind:
                field["appliance"] = by_kind[("maintainer_call", name)]
        elif kind in ("lamp", "contact"):
            put("lit", index.ref(ix_name, "lamp", name, "LIT"))
            if ("contact", name) in by_kind:
                field["appliance"] = by_kind[("contact", name)]
        if office:
            el["office"] = office
        if field:
            el["field"] = field


def build(model: dict, charts: list[dict], chart_problems: list[dict], project: Path, digest: str,
          station: str) -> tuple[dict | None, list[str]]:
    """One station's panel, or (None, problems)."""
    found = [c for c in charts if c["station"] == station]
    if len(found) != 1:
        return None, [f"no station named {station!r}"]
    chart = found[0]
    lines, problems = station_file.build(project, station)
    # A chart problem is the whole project's: the deriver itself exits 1 on any.
    problems = list(problems) + [f"{p['where']}: {p['text']}" for p in chart_problems]
    if problems:
        return None, problems
    records = _records(lines)
    channels = {kind: prefix for kind, prefix in (r[:2] for r in records.get("channel", []))}

    def functions(direction: str, bucket: str) -> list[dict]:
        """The chart's functions with the station file's positive-control group numbers."""
        rows = records.get(direction, [])
        out = []
        for f, row in zip(chart[bucket], rows, strict=True):
            assert row[:4] == [f["class"], f["appliance"], f["word"], f["aar"]], (row, f)
            out.append({"class": f["class"], "appliance": f["appliance"], "word": f["word"], "aar": f["aar"],
                        "group": int(row[4])})
        return out

    columns, placed_c, placed_i, problems = _levers(model, chart)
    if problems:
        return None, problems
    appliances = _appliances(records, channels)
    drawing = trackplan.drawing(model, chart["interlocking"])
    index = Index(model, charts)
    _bind(model, chart, index, appliances, drawing)

    for el in drawing["elements"]:
        for ref in el.get("office", {}).values():
            if ref["station"] == station:
                placed_i.add(ref["i"])

    lamps: dict[str, dict] = {}
    for i, f in enumerate(chart["indications"]):
        for source in f["sources"]:
            dev = _device(model, source) or {}
            given = dev.get("given", {})
            if given.get("Appliance") == "lamp":
                lamp = lamps.setdefault(source, {"name": given.get("Name", ""), "color": given.get("Color", ""),
                                                 "board": dev.get("relations", {}).get("interlocking", ""),
                                                 "indications": []})
                lamp["indications"].append(i)

    base = f"{channels['codeline']}/{topic_name(station)}"
    try:
        source = str(project.resolve().relative_to(Path.cwd()))
    except ValueError:
        source = str(project)
    panel = {
        "writer": WRITER,
        "station": station,
        "interlocking": chart["interlocking"],
        "source": {"project": source, "sha256": digest},
        "chartId": chart["chartId"],
        "encoding": chart["encoding"],
        "topics": {"controls": base + "/controls", "indications": base + "/indications",
                   "debug": base + "/debug", "plant": base + "/debug/plant",
                   "clockCommand": f"{channels['simulation']}/clock",
                   "clockState": f"{channels['simulation']}/clock/{topic_name(station)}"},
        "channels": channels,
        "frame": {"open": ["CHART", chart["chartId"]], "separator": " ", "dropped": ["(", ")"],
                  "unavailable": codeline_chart.UNAVAILABLE, "identity": stopgap("frame.identity"),
                  "rule": FRAME_RULE},
        "office": {"answerTimeoutMs": stopgap("office.answerTimeoutMs")},
        "controls": functions("control", "controls"),
        "indications": functions("indication", "indications"),
        "columns": columns,
        "board": {"lamps": [lamps[k] for k in sorted(lamps, key=codeline_chart._natural)]},
        "unplaced": {"controls": sorted(set(range(len(chart["controls"]))) - placed_c),
                     "indications": sorted(set(range(len(chart["indications"]))) - placed_i)},
        "drawing": drawing,
        "appliances": appliances,
        "unavailable": codeline_chart.UNAVAILABLE,
        "stopgaps": [{"what": key, "used": value, "why": why} for key, (value, why) in STOPGAPS.items()],
    }
    return panel, []


def index_file(panels: list[dict], not_derived: list[dict], source: dict) -> dict:
    """build/panels.json: the stations that derived, in panel-column order, and the clock."""
    def first_column(p: dict) -> tuple:
        return min(((c["unit"], c["number"]) for c in p["columns"]), default=("~", 0))

    simulation = sorted({p["channels"]["simulation"] for p in panels})
    return {
        "writer": WRITER,
        "source": source,
        "stations": [{"station": p["station"], "file": file_name(p["station"]), "chartId": p["chartId"],
                      "columns": [c["number"] for c in p["columns"]], "channels": p["channels"],
                      "topics": p["topics"]}
                     for p in sorted(panels, key=first_column)],
        "notDerived": not_derived,
        "unavailable": codeline_chart.UNAVAILABLE,
        "clock": {"commands": [f"{s}/clock" for s in simulation],
                  "states": [{"station": p["station"], "topic": p["topics"]["clockState"]}
                             for p in sorted(panels, key=first_column)],
                  **CLOCK_WORDS, **CLOCK_LIMITS},
    }


USAGE = "usage: panel_file.py [project.json] (--station NAME | --all DIR)"


def _dump(obj: dict) -> str:
    return json.dumps(obj, indent=1, sort_keys=False) + "\n"


def main(argv: list[str]) -> int:
    args = list(argv)
    mode = next((m for m in ("--station", "--all") if m in args), None)
    if mode is None or args.index(mode) + 1 >= len(args):
        print(USAGE, file=sys.stderr)
        return 2
    i = args.index(mode)
    value = args[i + 1]
    del args[i:i + 2]
    if len(args) > 1 or any(a.startswith("-") for a in args):
        print(USAGE, file=sys.stderr)
        return 2
    project = Path(args[0]) if args else codeline_chart.DEFAULT_PROJECT
    digest = hashlib.sha256(project.read_bytes()).hexdigest()[:8]
    model = codeline_chart.load(project)
    result = codeline_chart.derive(model)
    charts = result["stations"]

    if mode == "--station":
        panel, problems = build(model, charts, result["problems"], project, digest, value)
        if panel is None:
            for p in problems:
                print(f"panel_file: {value}: {p}", file=sys.stderr)
            return 1
        sys.stdout.write(_dump(panel))
        return 0

    out_dir = Path(value)
    out_dir.mkdir(parents=True, exist_ok=True)
    panels, not_derived = [], []
    for chart in charts:
        panel, problems = build(model, charts, result["problems"], project, digest, chart["station"])
        if panel is None:
            not_derived.append({"station": chart["station"], "problems": problems})
            print(f"panel_file: {chart['station']}: no panel ({len(problems)} problems; first: {problems[0]})",
                  file=sys.stderr)
            continue
        panels.append(panel)
        (out_dir / file_name(chart["station"])).write_text(_dump(panel), encoding="utf-8")
    if not panels:
        return 1
    (out_dir / "panels.json").write_text(_dump(index_file(panels, not_derived, panels[0]["source"])), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
