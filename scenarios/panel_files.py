#!/usr/bin/env python3
"""Functional scenario: the panel files the on-screen panel (web/) is generated from.

Every check holds a panel file against something that tools/panel_file.py did not write: the
chart (tools/codeline_chart.py), the project file's own pins, joints and ports, the station
file's records, the office tool's topics, and the C++ headers that own the words.

1. A panel file for every station that derives; Watsonville, known not to, is reported.
2. The chart exactly as the deriver has it; groups as the station file numbers them.
3. Levers: every position asserts the chart control of its word (and nothing else), answers to
   the chart indication of its word, rests where a US&S lever rests; every lamp is the chart
   indication of its word; the lever's words are the project file's Control.* / Indication.*
   pins; a lamp is inverted exactly where the project file marks the pin with `!`.
4. The drawing: every tagged element names a real appliance; a rail carries the circuit the
   project file's joints give for it; a switch leg is the leg at that port; every office binding
   is the chart function of the thing drawn (never a mast), in the project file's colour.
5. Topics and words: against tools/codeline_office.py, the station file, and the C++ owners.
6. Frames built from lever positions by the panel file's rule against encode_frame, and the
   Walkthrough's control frames.
7. web/ names no station, lever, appliance or AAR token of the fixture.

Run from the repository root: uv run python scenarios/panel_files.py
"""
import hashlib
import json
import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import codeline_chart as cc  # noqa: E402
import codeline_office  # noqa: E402  (the scripted office: its topics and channels)
import panel_file  # noqa: E402
import station_file  # noqa: E402
import trackplan  # noqa: E402  (page_of only: where a glyph's port lies on the page)

PROJECT = ROOT / "tools" / "spcoast.layout.json"
DERIVES = ["Christopher", "Corporal", "GCaltrain", "Gilroy", "Luchessa", "Sargent"]
DOES_NOT = ["Watsonville"]

# What a US&S-style lever is, stated here independently of tools/panel_file.py LEVER_FORMS:
# positions left to right (label, control word, answering indication word), where it rests,
# and its lamps (label, indication word). scenarios/web_office.py uses the same table.
FORMS = {
    "switch": {"positions": [("N", "NORMAL", "NORMAL"), ("R", "REVERSE", "REVERSE")], "home": "N",
               "lamps": [("N", "NORMAL"), ("R", "REVERSE")]},
    "signal": {"positions": [("L", "LEFT", "LEFT"), ("STOP", "STOP", None), ("R", "RIGHT", "RIGHT")], "home": "STOP",
               "lamps": [("L", "LEFT"), ("STOP", "TIME"), ("R", "RIGHT")]},
    "lock": {"positions": [("LOCK", None, None), ("REL", "RELEASE", "RELEASED")], "home": "LOCK",
             "lamps": [("REL", "RELEASED"), ("REQ", "REQUEST")]},
    "maintainer_call": {"positions": [("OFF", None, None), ("CALL", "CALL", "CALL")], "home": "OFF",
                        "lamps": [("CALL", "CALL")]},
}
INVERTED_LABELS = {"RELEASED": "LOCK"}

failures = []


def check(cond, text):
    if not cond:
        failures.append(text)
    print(("  ok    " if cond else "  FAIL  ") + text)


def build_all():
    """Every panel file and panels.json, written by the tool as `make` runs it."""
    with tempfile.TemporaryDirectory() as tmp:
        code = panel_file.main([str(PROJECT), "--all", tmp])
        written = sorted(p.name for p in Path(tmp).iterdir())
        index = json.loads((Path(tmp) / "panels.json").read_text())
        panels = {s["station"]: json.loads((Path(tmp) / s["file"]).read_text()) for s in index["stations"]}
    return code, written, index, panels


def lever_device(model, column, lever):
    """The office device of the project file that is this lever: same column, Style and Name."""
    want = f"{column['unit']}:{column['number']}"
    found = [d for d in model["officeUnits"][column["unit"]]["devices"]
             if d.get("given", {}).get("Appliance") == "lever" and d["given"].get("Style") == lever["style"]
             and d["given"].get("Name") == lever["name"] and d.get("relations", {}).get("column") == want]
    return found[0] if len(found) == 1 else None


def pin_words(dev, prefix, direction):
    """The words of a lever device's Control.* or Indication.* pins, decoded with the deriver's
    table: {word: inverted}."""
    name, style = dev["given"]["Name"], dev["given"]["Style"]
    out = {}
    for key, value in dev["given"].items():
        if not key.startswith(prefix) or not value:
            continue
        token = value.lstrip("!")
        decoded = cc.SUFFIXES[style].get(token[len(name):]) if token.startswith(name) else None
        if decoded and decoded[0] == direction:
            out[decoded[1]] = value.startswith("!")
    return out


def main():
    model = cc.load(PROJECT)
    result = cc.derive(model)
    charts = {s["station"]: s for s in result["stations"]}

    # ------------------------------------------------------------------ 1. the files
    print("1. panel files")
    code, written, index, panels = build_all()
    check(code == 0, "panel_file.py --all exits 0")
    check(sorted(panels) == DERIVES, f"a panel file for each of {DERIVES} (got {sorted(panels)})")
    check([n["station"] for n in index["notDerived"]] == DOES_NOT and all(n["problems"] for n in index["notDerived"]),
          f"{DOES_NOT} reported as not derived, with its problems")
    check(written == sorted([panel_file.file_name(s) for s in DERIVES] + ["panels.json"]),
          "nothing else is written (no file for a station that does not derive)")
    firsts = [min(c["number"] for c in panels[s["station"]]["columns"]) for s in index["stations"]]
    check(firsts == sorted(firsts), f"panels.json lists stations in panel-column order (first columns {firsts})")
    digest = hashlib.sha256(PROJECT.read_bytes()).hexdigest()[:8]
    check(all(p["source"]["sha256"] == digest for p in panels.values()), "every panel file names the project file's digest")

    # ------------------------------------------------------------------ 2. the chart, unchanged
    print("2. the chart as the deriver has it")
    records = {}
    for name in DERIVES:
        p, ch = panels[name], charts[name]
        same = p["chartId"] == ch["chartId"] and p["frame"]["open"] == ["CHART", ch["chartId"]]
        for bucket in ("controls", "indications"):
            same = same and [(f["class"], f["appliance"], f["word"], f["aar"]) for f in p[bucket]] == \
                            [(f["class"], f["appliance"], f["word"], f["aar"]) for f in ch[bucket]]
        check(same, f"{name}: chart {ch['chartId']}, {len(ch['controls'])} controls and {len(ch['indications'])} indications, in order")
        lines, _ = station_file.build(PROJECT, name)
        records[name] = [line.split(" ") for line in lines if line and not line.startswith("#")]
        groups = {(r[0], r[4]): int(r[5]) for r in records[name] if r[0] in ("control", "indication")}
        check(all(f["group"] == groups[(d, f["aar"])] for d, b in (("control", "controls"), ("indication", "indications")) for f in p[b]),
              f"{name}: positive-control groups as in the station file")
        expect = {}
        for direction, bucket in (("control", "controls"), ("indication", "indications")):
            for f in ch[bucket]:
                for gi, group in enumerate(cc.GROUPS.get((direction, f["class"]), [])):
                    if f["word"] in group:
                        expect.setdefault((direction, f["appliance"], gi), set()).add(f["aar"])
        got = {}
        for direction, bucket in (("control", "controls"), ("indication", "indications")):
            for f in p[bucket]:
                if f["group"]:
                    got.setdefault((direction, f["group"]), set()).add(f["aar"])
        check(sorted(map(sorted, expect.values())) == sorted(map(sorted, got.values())),
              f"{name}: the groups are codeline_chart.GROUPS' ({len(expect)} groups)")

    # ------------------------------------------------------------------ 3. levers and lamps
    print("3. levers: positions, answers, rest, lamps, against the chart and the project file's pins")
    for name in DERIVES:
        p, ch = panels[name], charts[name]
        control = {(f["class"], f["appliance"], f["word"]): i for i, f in enumerate(ch["controls"])}
        indication = {(f["class"], f["appliance"], f["word"]): i for i, f in enumerate(ch["indications"])}
        wrong = []
        asserted = []
        lamp_indications = set()
        for col in p["columns"]:
            for lv in col["levers"]:
                who = f"{lv['style']} {lv['name']}"
                form = FORMS.get(lv["style"])
                dev = lever_device(model, col, lv)
                if form is None or dev is None:
                    wrong.append(f"{who}: no such lever device in column {col['number']}")
                    continue
                if (dev.get("derived", {}).get("serves") or "") != f"{p['interlocking']}:{lv['name']}":
                    wrong.append(f"{who}: the device serves {dev.get('derived', {}).get('serves')}")
                key = (lv["style"], lv["name"])
                want_positions = [
                    {"label": label, "word": word, "asserts": [control[key + (word,)]] if word else [],
                     "expects": indication.get(key + (answer,)) if answer else None}
                    for label, word, answer in form["positions"] if word is None or key + (word,) in control]
                if lv["positions"] != want_positions:
                    wrong.append(f"{who}: positions {lv['positions']} are not {want_positions}")
                if lv["home"] != form["home"]:
                    wrong.append(f"{who}: rests in {lv['home']}, not {form['home']}")
                pins_c = pin_words(dev, "Control.", "control")
                if set(pins_c) != {q["word"] for q in lv["positions"] if q["word"]}:
                    wrong.append(f"{who}: positions send {[q['word'] for q in lv['positions']]}, its pins are {sorted(pins_c)}")
                pins_i = pin_words(dev, "Indication.", "indication")
                want_lamps = [
                    {"label": INVERTED_LABELS[word] if pins_i.get(word) else label, "word": word,
                     "indication": indication[key + (word,)], "inverted": bool(pins_i.get(word))}
                    for label, word in form["lamps"] if key + (word,) in indication]
                got_lamps = [{k: l[k] for k in ("label", "word", "indication", "inverted")} for l in lv["lamps"]]
                if got_lamps != want_lamps:
                    wrong.append(f"{who}: lamps {got_lamps} are not {want_lamps}")
                # A signal lever's centre lamp is its STOP lamp: steady unless LEFT or RIGHT is lit.
                for l in lv["lamps"]:
                    want_unless = ([indication[key + (w,)] for w in ("LEFT", "RIGHT")]
                                   if lv["style"] == "signal" and l["word"] == "TIME" else None)
                    if l.get("litUnless") != want_unless:
                        wrong.append(f"{who}: lamp {l['label']} litUnless {l.get('litUnless')} is not {want_unless}")
                if set(pins_i) != {l["word"] for l in lv["lamps"]}:
                    wrong.append(f"{who}: lamps show {[l['word'] for l in lv['lamps']]}, its pins are {sorted(pins_i)}")
                asserted += [i for q in lv["positions"] for i in q["asserts"]]
                lamp_indications |= {l["indication"] for l in lv["lamps"]}
        n_levers = sum(len(col["levers"]) for col in p["columns"])
        devices = [d for u in model["officeUnits"].values() for d in u["devices"]
                   if d.get("given", {}).get("Appliance") == "lever" and d["given"].get("Style") in FORMS
                   and (d.get("derived", {}).get("serves") or "").startswith(p["interlocking"] + ":")]
        check(not wrong, f"{name}: {n_levers} levers are the chart's functions, word for word, and the project file's pins {wrong[:3] or ''}")
        check(n_levers == len(devices), f"{name}: every lever device of the office description is on the panel ({len(devices)})")
        check(sorted(asserted) == list(range(len(p["controls"]))) and not p["unplaced"]["controls"],
              f"{name}: each of {len(p['controls'])} controls is asserted by exactly one lever position")
        drawn = {ref["i"] for e in p["drawing"]["elements"] for ref in e.get("office", {}).values() if ref["station"] == name}
        unplaced = set(p["unplaced"]["indications"])
        check(lamp_indications | drawn | unplaced == set(range(len(p["indications"]))) and not (unplaced & (lamp_indications | drawn)),
              f"{name}: {len(lamp_indications)} indications on lever lamps, {len(drawn - lamp_indications)} on the board only, "
              f"{len(unplaced)} unplaced {[p['indications'][i]['aar'] for i in sorted(unplaced)]}")
        codes = [(col["unit"], col["number"]) for col in p["columns"] if col["code"]]
        project_codes = [tuple(d["relations"]["column"].split(":")) for u in model["officeUnits"].values() for d in u["devices"]
                         if d.get("given", {}).get("Style") == "code"]
        check(len(codes) == 1 and (codes[0][0], str(codes[0][1])) in project_codes,
              f"{name}: one column carries the CODE button, where the project file has one")
        timeout = p["office"]["answerTimeoutMs"]
        check(isinstance(timeout, int) and 500 <= timeout <= 30000 and any(s["what"] == "office.answerTimeoutMs" for s in p["stopgaps"]),
              f"{name}: the answer timeout ({timeout} ms) is in the panel file, declared a stopgap")

    # ------------------------------------------------------------------ 4. the drawing
    print("4. the drawing: real appliances, the project file's circuits and ports, the chart's functions")

    def circuits_of(ix_name):
        """Qualified names of every track circuit an interlocking has (a switch's own included)."""
        out = set()
        for n, a in model["interlockings"].get(ix_name, {}).get("appliances", {}).items():
            g, d = a.get("given", {}), a.get("derived", {})
            if g.get("Appliance") == "track_circuit":
                out.add(f"{ix_name}:{n}")
            for c in (d.get("osCircuit"), d.get("track")):
                if c:
                    out.add(c)
            if g.get("TC"):
                out.add(f"{ix_name}:{g['TC']}")
        return out

    def qualified(ix_name, local):
        return local if ":" in local else f"{ix_name}:{local}"

    def end_circuit(ix, ix_name, ref):
        """The circuit the project file gives one end of a piece of track: <joint id>.<side>,
        <policy id>.track, or <interlocking>:<switch>.<port>. None for a label."""
        if ref.startswith("label:"):
            return None
        head, _, side = ref.rpartition(".")
        for kind in ("joints", "policies", "ends"):
            for item in ix.get("track", {}).get(kind, []):
                if item.get("id") == head:
                    return item.get("derived", {}).get("circuits", {}).get(side)
        if head.startswith(ix_name + ":"):
            d = ix["appliances"].get(head.partition(":")[2], {}).get("derived", {})
            return d.get("osCircuit") or d.get("track")
        return None

    for name in DERIVES:
        p = panels[name]
        ix_name = p["interlocking"]
        ix = model["interlockings"][ix_name]
        apps = ix["appliances"]
        elements = p["drawing"]["elements"]
        tagged = [e for e in elements if "tag" in e]

        # names
        unreal = []
        for e in tagged:
            t = e["tag"]
            if t["kind"] == "track_circuit":
                if qualified(ix_name, t["name"]) not in circuits_of(qualified(ix_name, t["name"]).partition(":")[0]):
                    unreal.append(t["name"])
            else:
                given = apps.get(t["name"], {}).get("given", {})
                kind = "maintainer_call" if given.get("Style") == "maintainer_call" else given.get("Appliance")
                if kind != t["kind"]:
                    unreal.append(f"{t['kind']} {t['name']}")
            if "circuit" in t and qualified(ix_name, t["circuit"]) not in circuits_of(ix_name):
                unreal.append(t["circuit"])
        check(not unreal, f"{name}: {len(tagged)} tagged elements, each naming an appliance of its kind in the project file {sorted(set(unreal)) or ''}")

        # rails of plain track: the circuit of the joints at their ends
        track = ix["geometry"]["track"]
        rails = [e for e in elements if e["role"] == "rail" and e.get("tag", {}).get("kind", "track_circuit") == "track_circuit"]
        bad, proven = [], 0
        if len(rails) != len(track):
            bad.append(f"{len(rails)} rails drawn for {len(track)} pieces of track")
        for e, t in zip(rails, track):
            pts = [[round(x, 3), round(y, 3)] for x, y in trackplan.pts(t["points"])]
            ends = {end_circuit(ix, ix_name, t["relations"][k]) for k in ("from", "to")} - {None}
            got = qualified(ix_name, e["tag"]["name"]) if "tag" in e else None
            if e["points"] != pts:
                bad.append(f"rail {e['points']} is not the project file's {pts}")
            elif len(ends) > 1:
                bad.append(f"track {t['points']}: its ends give {sorted(ends)}")
            elif ends:
                proven += 1
                if got not in ends:
                    bad.append(f"track {t['points']} is tagged {got}; its joints give {sorted(ends)}")
        distinct = {e["tag"]["name"] for e in rails if "tag" in e}
        check(not bad and proven >= len(track) - 2 and len(distinct) >= 3,
              f"{name}: {proven} of {len(track)} pieces of track carry the circuit their joints give ({len(distinct)} circuits) {bad[:2] or ''}")

        # switch legs: the leg at that port; the switch's own circuit
        bad = []
        glyphs = model["layout"]["glyphs"]
        for sw, pl in ix["geometry"]["appliances"].items():
            if apps.get(sw, {}).get("given", {}).get("Appliance") != "switch":
                continue
            ports = {n: trackplan.page_of(pl, (xy["x"], xy["y"])) for n, xy in glyphs[pl["glyph"]]["ports"].items()}
            legs = [e for e in elements if e["role"] == "rail" and e.get("tag", {}).get("kind") == "switch" and e["tag"]["name"] == sw]
            d = apps[sw].get("derived", {})
            own = d.get("osCircuit") or d.get("track")
            for e in legs:
                part = e["tag"].get("part")
                on = [n for n, (x, y) in ports.items()
                      if any(abs(q[0] - x) < 0.01 and abs(q[1] - y) < 0.01 for q in (e["points"][0], e["points"][-1]))]
                if (part in ports and on != [part]) or (part == "body" and len(on) == 1) or part not in list(ports) + ["body"]:
                    bad.append(f"{sw}: a rail tagged {part} ends at ports {on}")
                if own and qualified(ix_name, e["tag"].get("circuit", "")) != own:
                    bad.append(f"{sw}: rail of circuit {e['tag'].get('circuit')}, the project file says {own}")
            if {"N", "R"} <= set(ports) and not {"N", "R"} <= {e["tag"].get("part") for e in legs}:
                bad.append(f"{sw}: no rail for its N or R leg")
        check(not bad, f"{name}: every switch leg is the rail at that port, on the switch's own circuit {bad[:2] or ''}")

        # office bindings: the chart function of the thing drawn, in the project file's colour
        def function(ref):
            q = panels[ref["station"]]
            f = q["indications"][ref["i"]]
            return (q["interlocking"], f["class"], f["appliance"], f["word"], f["aar"])

        def colours(ix_of, aar):
            return {d["given"].get("Color") for u in model["officeUnits"].values() for d in u["devices"]
                    if d.get("given", {}).get("Appliance") == "lamp" and f"{ix_of}:{aar}" in d.get("derived", {}).get("indications", [])}

        chart_has = {(q["interlocking"], f["class"], f["appliance"], f["word"]) for q in panels.values() for f in q["indications"]}
        bad, bound = [], 0
        for e in tagged:
            t, office = e["tag"], e.get("office", {})
            want = {}
            if t["kind"] == "track_circuit":
                i_x, _, c = qualified(ix_name, t["name"]).partition(":")
                want["occupied"] = (i_x, "track_circuit", c, "OCCUPIED")
            elif t["kind"] == "switch" and t.get("part") == "lamp":
                want["lit"] = (ix_name, "lock", t["name"], "RELEASED")
            elif t["kind"] == "switch":
                if "circuit" in t:
                    i_x, _, c = qualified(ix_name, t["circuit"]).partition(":")
                    want["occupied"] = (i_x, "track_circuit", c, "OCCUPIED")
                if t.get("part") in ("N", "R"):
                    want["lined"] = (ix_name, "switch", t["name"], "NORMAL" if t["part"] == "N" else "REVERSE")
            elif t["kind"] == "maintainer_call":
                want["lit"] = (ix_name, "maintainer_call", t["name"], "CALL")
            elif t["kind"] in ("lamp", "contact"):
                want["lit"] = (ix_name, "lamp", t["name"], "LIT")
            elif t["kind"] == "mast":                    # its lever's LEFT / RIGHT, by the mast's name
                m = re.match(r"(\d+)([EW])", t["name"])
                if m:
                    want["clear"] = (ix_name, "signal", m.group(1), "RIGHT" if m.group(2) == "E" else "LEFT")
            want = {slot: fn for slot, fn in want.items() if fn in chart_has}     # a mast of another station's lever wants nothing
            if set(office) != set(want):
                bad.append(f"{t}: bound {sorted(office)}, the chart gives {sorted(want)}")
                continue
            for slot, ref in office.items():
                bound += 1
                fn = function(ref)
                if fn[:4] != want[slot]:
                    bad.append(f"{t} {slot}: bound to {fn[4]}, not {want[slot]}")
                if ref.get("color") not in (colours(fn[0], fn[4]) or {None}):
                    bad.append(f"{t} {slot}: colour {ref.get('color')}, the project file has {colours(fn[0], fn[4])}")
        check(not bad, f"{name}: {bound} office bindings, each the chart function of the thing drawn, in the project file's colour {bad[:2] or ''}")
        check(all(set(e.get("office", {})) <= {"clear"} for e in tagged if e["tag"]["kind"] == "mast"),
              f"{name}: a mast shows only whether its lever's signals are indicated clear (the code line has no mast function)")

        # field bindings: the station file's appliances
        keys = {a["key"]: a for a in p["appliances"]}
        # (a maintainer call is an appliance only where the station file binds it to a lamp)
        rec = {(r[0], r[1]): r for r in records[name] if r[0] in ("circuit", "switch", "lock", "mast", "contact")
               or (r[0] == "call" and len(r) == 4)}
        kind_of = {"circuit": "track_circuit", "call": "maintainer_call"}
        role_id = {(kind_of.get(k, k), n): (r[3], r[4]) if k == "switch" else (r[2], r[3])
                   for (k, n), r in rec.items()}
        bad = []
        if {(a["kind"], a["name"]): (a["role"], a["id"]) for a in p["appliances"]} != role_id:
            bad.append("the appliance list is not the station file's records")
        for e in tagged:
            t, field = e["tag"], e.get("field", {})
            want = {}
            if t["kind"] == "track_circuit" and ("track_circuit", t["name"]) in role_id:
                want["circuit"] = ("track_circuit", t["name"])
            elif t["kind"] == "switch" and t.get("part") == "lamp":
                if ("lock", t["name"]) in role_id:
                    want["appliance"] = ("lock", t["name"])
            elif t["kind"] == "switch":
                if ("switch", t["name"]) in role_id:
                    want["appliance"] = ("switch", t["name"])
                if ("track_circuit", t.get("circuit")) in role_id:
                    want["circuit"] = ("track_circuit", t["circuit"])
            elif (t["kind"], t["name"]) in role_id:
                want["appliance"] = (t["kind"], t["name"])
            got = {slot: (keys[k]["kind"], keys[k]["name"]) if k in keys else k for slot, k in field.items()}
            if got != want:
                bad.append(f"{t}: field {got}, the station file gives {want}")
        bound_keys = {k for e in elements for k in e.get("field", {}).values()}
        check(not bad and bound_keys == set(keys),
              f"{name}: the {len(keys)} appliances are the station file's, each drawn and bound to its own symbol {bad[:2] or sorted(set(keys) - bound_keys) or ''}")

    # ------------------------------------------------------------------ 5. topics and words
    print("5. topics and words, against the office tool, the station file and the C++ owners")
    src = ROOT / "src" / "codeline"
    host = (src / "station" / "StationHost.h").read_text()
    simclock = (src / "station" / "SimClock.h").read_text()
    debug = (src / "station" / "DebugChannel.h").read_text()
    field_h = (src / "station" / "CodeLineField.h").read_text()
    tables = (src / "generated" / "MqttTables.h").read_text() + (src / "generated" / "Enums.h").read_text()

    def in_literal(text, word):
        """Is `word` a whole word inside some string literal of this C++ source?"""
        return re.search(r'"[^"\n]*(?<![A-Za-z_])' + re.escape(word) + r'(?![A-Za-z_])[^"\n]*"', text) is not None
    channels = codeline_office.CHANNELS
    for name in DERIVES:
        p = panels[name]
        t = p["topics"]
        controls, indications = codeline_office.topics(name)
        ok = (t["controls"], t["indications"]) == (controls, indications) and controls != indications
        ok = ok and p["channels"] == channels and len(set(channels.values())) == 3
        base = f"{channels['codeline']}/{name.replace(' ', '-')}"
        ok = ok and t["debug"] == base + "/debug" and t["plant"] == base + "/debug/plant"
        ok = ok and t["clockCommand"] == channels["simulation"] + "/clock"
        ok = ok and t["clockState"] == f"{channels['simulation']}/clock/{name.replace(' ', '-')}"
        ok = ok and len(set(t.values())) == len(t)
        check(ok, f"{name}: controls, indications, debug and clock topics are the office tool's and the host's, all different")
        bad = []
        for a in p["appliances"]:
            base = f"{channels['appliances']}/{a['role']}/{a['id']}"
            want = {"state": base + "/state", "cmd": base + "/cmd", "event": base + "/event",
                    "simulate": f"{channels['simulation']}/{a['role']}/{a['id']}"}
            if a["topics"] != want or a["key"] != f"{a['role']}/{a['id']}":
                bad.append(a["key"])
        check(not bad, f"{name}: every appliance's state, cmd, event and simulation topics are its channel, role and id {bad or ''}")
        check(p["unavailable"] == cc.UNAVAILABLE == p["frame"]["unavailable"] and f'"{p["unavailable"]}"' in field_h
              and f'"{p["unavailable"]}"' in tables, f"{name}: the unavailable word is the chart tool's and the C++ bindings'")
        bad = []
        for a in p["appliances"]:
            said = [x["payload"] for x in a["actions"]] + list(a["power"].values())
            said += [a["click"]["asserted"], a["click"]["otherwise"]] if a["click"] else []
            for payload in said:
                word = re.sub(r"\d+$", "", payload.strip("()"))
                if not in_literal(host, word):
                    bad.append(f"{a['key']} says {payload}")
            read = list(a["shows"]) + ([a["click"]["term"]] if a["click"] else [])
            for term in read:
                word = re.sub(r"\d+$", "", term.strip("()"))
                if not in_literal(tables, word):
                    bad.append(f"{a['key']} reads {term}")
            kinds = {"track_circuit": ({"SHUNT", "(SHUNT)"}, {"ACTIVE"}), "contact": ({"SHUNT", "(SHUNT)"}, {"ACTIVE"}),
                     "switch": ({"NORMAL", "REVERSE"}, {"NORMAL", "REVERSE"}), "mast": (set(), None)}
            if a["kind"] in kinds:
                acts, shows = kinds[a["kind"]]
                if {x["payload"] for x in a["actions"]} != acts or (shows is not None and set(a["shows"]) != shows):
                    bad.append(f"{a['key']}: actions or state words are not a {a['kind']}'s")
            if a["kind"] == "mast" and (set(a["shows"]) - {"(LIT)"} != set(station_file.PERMISSIVENESS)
                                        or a["shows"]["STOP"] != "stop" or a["shows"]["(LIT)"] != "dark" or a["shows"]["CLEAR"] != "clear"):
                bad.append(f"{a['key']}: a mast's indications are not the engine's")
            if a["kind"] == "lock":
                nv = dict(x.split(":") for x in next(f for f in rec_of(records[name], "lock", a["name"]) if f.startswith("nv="))[3:].split(","))
                want = {f"SW{k}" for k in range(1, int(nv["lever.inputs"]) + 1)} | {f"BTN{k}" for k in range(1, int(nv["button.inputs"]) + 1)}
                if {x["payload"].strip("()") for x in a["actions"]} != want or set(a["shows"]) != {w for w in want if w.startswith("SW")}:
                    bad.append(f"{a['key']}: contacts and buttons are not its NV's")
            if a["click"] and not ({a["click"]["asserted"], a["click"]["otherwise"]} <= {x["payload"] for x in a["actions"]}
                                   and a["click"]["asserted"] != a["click"]["otherwise"] and a["click"]["term"] in a["shows"]):
                bad.append(f"{a['key']}: a click does not toggle between two of its actions on a word it reads")
            if a["power"] != {"off": "(POWER)", "on": "POWER"}:
                bad.append(f"{a['key']}: power words")
        check(not bad, f"{name}: field words are the host's simulation words and the binding's state words {bad[:3] or ''}")
    clock = index["clock"]
    words = {k: clock[k] for k in ("pause", "run", "step", "rate", "running", "paused", "time")}
    check(all(in_literal(simclock, w) for w in words.values()) and len(set(words.values())) == 7,
          f"clock words {sorted(words.values())} are SimClock.h's")
    check(f"kMaxRate = {clock['maxRate']};" in simclock and f"kMaxStepMs = {clock['maxStepMs']};" in simclock
          and "1000 is real time" in simclock and clock["realTime"] == 1000, "clock limits are SimClock.h's")
    check(clock["commands"] == [channels["simulation"] + "/clock"] and '"/clock"' in simclock
          and [s["station"] for s in clock["states"]] == [s["station"] for s in index["stations"]]
          and all(s["topic"] == panels[s["station"]]["topics"]["clockState"] for s in clock["states"]),
          "clock command and state topics: one command topic, one state topic per station of panels.json")
    check('"/debug"' in debug and '"/plant"' in debug and "stationTopic(codeline_channel" in debug and "stationTopic(commands_" in simclock,
          "debug and clock topics are built in C++ as the panel file spells them")
    check("c == ' ' ? '-' : c" in field_h and panel_file.topic_name("A B C") == "A-B-C",
          "the station's name in a topic: stationTopic()'s rule")
    check(index["unavailable"] == cc.UNAVAILABLE, "panels.json carries the unavailable word")
    owners = {s["what"]: s["why"] for s in panels["Corporal"]["stopgaps"]}
    named = {"simulation.words": "StationHost.h", "state.words": "MqttTables.h", "clock.words": "SimClock.h",
             "debug.clock.topics": "DebugChannel.h", "topic.station": "stationTopic()"}
    check(all(owner in owners.get(what, "") for what, owner in named.items()),
          f"every word copied from C++ is a declared stopgap naming its owner ({sorted(named)})")

    # ------------------------------------------------------------------ 6. the frame rule
    print("6. frames from lever positions (the panel file's rule) against codeline_chart.encode_frame")
    p, ch = panels["Corporal"], charts["Corporal"]
    levers = [lv for col in p["columns"] for lv in col["levers"]]

    def words_of(positions):
        """The (appliance, word) pairs a lever setting sends, by FORMS and nothing of the panel file."""
        out = set()
        for lv in levers:
            label = positions[f"{lv['style']} {lv['name']}"]
            word = next(w for l, w, _ in FORMS[lv["style"]]["positions"] if l == label)
            if word:
                out.add((lv["name"], word))
        return out

    settings = [{}]
    for lv in levers:
        settings = [dict(s, **{f"{lv['style']} {lv['name']}": label}) for s in settings for label, _, _ in FORMS[lv["style"]]["positions"]]
    check(all(panel_file.lever_frame(p, s) == cc.encode_frame(ch, "control", words_of(s)) for s in settings),
          f"Corporal: all {len(settings)} lever settings spell encode_frame's frame")
    home = {f"{lv['style']} {lv['name']}": FORMS[lv["style"]]["home"] for lv in levers}
    check(panel_file.lever_frame(p, {}) == cc.encode_frame(ch, "control", words_of(home)),
          "Corporal: a lever not named stands where a US&S lever rests")

    def positions_for(frame):
        want = cc.decode_frame(ch, "control", frame)
        return next((s for s in settings if words_of(s) == want), None)

    sends, step = [], ""
    for line in (ROOT / "scenarios" / "corporal_walkthrough.script").read_text().splitlines():
        if line.startswith("step "):
            step = line[5:]
        elif line.startswith("send codeline/Corporal/controls "):
            sends.append((step, line.split(" ", 2)[2]))
    walkthrough = [(s, f) for s, f in sends if s.startswith("1. press CODE") or s.startswith("cancel:")]
    check(len(walkthrough) == 2, "the script has the Walkthrough's two control frames (step 1 and the cancel)")
    for step, frame in walkthrough:
        s = positions_for(frame)
        check(s is not None and panel_file.lever_frame(p, s) == frame, f"Walkthrough {step.split(':')[0]!r}: levers {s} send {frame}")
    by_lever = [f for _, f in sends if positions_for(f) is not None]
    check(all(panel_file.lever_frame(p, positions_for(f)) == f for f in by_lever),
          f"{len(by_lever)} of the script's {len(sends)} control frames are lever settings, each reproduced exactly")
    print(f"        ({len(sends) - len(by_lever)} script frames leave a group unchanged, which no lever position says)")

    # ------------------------------------------------------------------ 7. nothing of the fixture in web/
    print("7. web/ names nothing of the fixture")
    names = set()
    for ch_ in result["stations"]:
        names.update(ch_["station"].split(":"))
        names.add(ch_["interlocking"])
        for f in ch_["controls"] + ch_["indications"]:
            names.update((f["aar"], f["appliance"]))
    for ix in model["interlockings"].values():
        names.update(ix.get("appliances", {}))
        names.update(ix.get("houses", {}))
    names.update(model.get("officeUnits", {}))
    names.add(model["layout"]["name"])
    searched = sorted(n for n in names if len(n) >= 3)     # a name must be telling to be searched for
    pattern = re.compile(r"(?<![A-Za-z0-9])(" + "|".join(re.escape(n) for n in searched) + r")(?![A-Za-z0-9])")
    web = sorted(q for q in (ROOT / "web").iterdir() if q.suffix in (".html", ".js", ".css"))
    check(len(web) >= 3, f"web/ has the page ({[q.name for q in web]})")
    hits = [(q.name, n, m.group(1)) for q in web for n, line in enumerate(q.read_text().splitlines(), 1) for m in pattern.finditer(line)]
    check(not hits, f"none of {len(searched)} fixture names (stations, levers, appliances, AAR tokens) appears in web/ {hits[:5] or ''}")
    loads = [q.name for q in web if re.search(r"https?://(?!127\.0\.0\.1)", q.read_text().replace("http://www.w3.org/2000/svg", ""))]
    check(not loads, f"web/ loads nothing from elsewhere (no library, no CDN) {loads or ''}")

    print()
    if failures:
        print(f"FAILED: {len(failures)}")
        for f in failures:
            print("  " + f)
        return 1
    print("PASSED")
    return 0


def rec_of(records, kind, name):
    return next(r for r in records if r[0] == kind and r[1] == name)


if __name__ == "__main__":
    sys.exit(main())
