#!/usr/bin/env python3
"""Functional scenario: web/ itself, executed under node, against the chart deriver.

The page's code (web/office.js, web/field.js, web/mqtt.js) is run by scenarios/web_office.mjs
with cases computed here from tools/codeline_chart.py, for every station that derives:

- lever settings (all of them, or a seeded sample) with the frame encode_frame spells for each;
- indication texts: frames encode_frame spells for sampled plant states, and texts a reader must
  not accept (another chart, missing / extra / swapped / unknown terms, two words of one group,
  text that is no frame, UNAVAILABLE), each with what decode_frame makes of it and which lamps
  are lit, and where each lever is found on such a frame.

The node side checks controlFrame and readIndications against those; that the office's lamps
follow indication frames and never a lever; that levers are placed by a station's first current
frame only; that the office subscribes only to indications topics and publishes only on controls
topics; that a field view never publishes on the code line; and web/mqtt.js against a scripted
WebSocket (framing, a failing handler, protocol errors, CONNACK timeout, refused subscription,
probe on wake, backoff).

node is required: without it this scenario FAILS (web/ would go unexecuted).

Run from the repository root: uv run python scenarios/web_office.py
"""
import json
import random
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))        # before scenarios/, which has a codeline_chart.py of its own
sys.path.append(str(ROOT / "scenarios"))

import codeline_chart as cc  # noqa: E402
import codeline_office  # noqa: E402
import panel_file  # noqa: E402
from panel_files import FORMS, PROJECT  # noqa: E402  (what a lever is, stated apart from panel_file.py)

MAX_SETTINGS = 250
FRAMES_PER_STATION = 40


def lever_names(model, chart):
    """(style, name) of every lever of a station, from the chart's own functions."""
    seen = []
    for f in chart["controls"] + chart["indications"]:
        if f["class"] in FORMS and (f["class"], f["appliance"]) not in seen:
            seen.append((f["class"], f["appliance"]))
    return seen


def control_cases(chart, levers, rng):
    has = {(f["class"], f["appliance"], f["word"]) for f in chart["controls"]}
    choices = [[(label, word) for label, word, _ in FORMS[style]["positions"] if word is None or (style, name, word) in has]
               for style, name in levers]
    total = 1
    for c in choices:
        total *= len(c)
    picks = []
    if total <= MAX_SETTINGS:
        picks = [[]]
        for c in choices:
            picks = [p + [x] for p in picks for x in c]
    else:
        picks = [[rng.choice(c) for c in choices] for _ in range(MAX_SETTINGS)]
        picks.append([next(x for x in c if x[0] == FORMS[style]["home"]) for c, (style, _) in zip(choices, levers)])
    out = []
    for pick in picks:
        words = {(name, word) for (style, name), (label, word) in zip(levers, pick) if word}
        out.append({"positions": {f"{style} {name}": label for (style, name), (label, _) in zip(levers, pick)},
                    "frame": cc.encode_frame(chart, "control", words)})
    return out


def start_of(chart, levers, lit):
    """Where each lever is found when the office first hears the field: the position whose
    answering indication is asserted, else where the lever rests."""
    out = {}
    for style, name in levers:
        found = [label for label, _, answer in FORMS[style]["positions"] if answer and (name, answer) in lit
                 and any((f["class"], f["appliance"], f["word"]) == (style, name, answer) for f in chart["indications"])]
        out[f"{style} {name}"] = found[0] if found else FORMS[style]["home"]
    return out


def verdict(chart, text):
    """What decode_frame makes of a text, labelled as the office labels it: text that does not
    open with CHART <8 hex> is malformed; a well-formed CHART <other id> is out of sync."""
    try:
        lit = cc.decode_frame(chart, "indication", text)
    except cc.FrameError:
        parts = cc.frame_terms(text)
        opens = len(parts) >= 2 and parts[0] == "CHART" and re.fullmatch(r"[0-9a-f]{8}", parts[1])
        return ("out of sync" if opens and parts[1] != chart["chartId"] else "malformed"), None
    return ("unavailable", None) if lit is None else ("valid", lit)


def indication_cases(chart, levers, rng):
    fns = chart["indications"]
    texts = []
    while len(texts) < FRAMES_PER_STATION:
        lit = {(f["appliance"], f["word"]) for f in fns if rng.random() < 0.35}
        try:
            texts.append(cc.encode_frame(chart, "indication", lit))
        except cc.FrameError:
            continue                                  # two words of one group: not a state of the field
    texts[0] = cc.encode_frame(chart, "indication", set())
    good = texts[1].split(" ")
    grouped = [f for f in fns if any(f["word"] in g for g in cc.GROUPS.get(("indication", f["class"]), []))]
    pair = next((a, b) for a in grouped for b in grouped if a is not b and a["appliance"] == b["appliance"]
                and any(a["word"] in g and b["word"] in g for g in cc.GROUPS[("indication", a["class"])]))
    two = " ".join(["CHART", chart["chartId"]] + [f["aar"] if f in pair else f"({f['aar']})" for f in fns])
    texts += [
        cc.UNAVAILABLE,
        " ".join(good).replace(chart["chartId"], "0badc0de"),           # another chart
        " ".join(["CHART", "0badc0de"] + good[2:-1]),                   # another chart, and short
        " ".join(good[:-1]),                                            # a term missing
        " ".join(good + [good[-1]]),                                    # a term repeated
        " ".join(good[:2] + [good[3], good[2]] + good[4:]),             # two terms out of order
        " ".join(good[:2] + ["(NOSUCHK)"] + good[3:]),                  # an unknown term
        " ".join(good[:2] + [good[2].strip("()").lower()] + good[3:]),  # wrong case
        " ".join(good[:2] + ["(" + good[2].strip("()")] + good[3:]),    # an unclosed parenthesis
        two,                                                            # two words of one group
        "CHART", "CHART " + chart["chartId"], "", "chart " + " ".join(good[1:]), "CHART XYZ " + " ".join(good[2:]),
        cc.UNAVAILABLE + " " + cc.UNAVAILABLE, cc.UNAVAILABLE.lower(), " ".join(good[2:]),
        "\t".join(good) + "\r\n",                                       # other separators, same frame
        "  ".join(good),
    ]
    out = []
    for text in texts:
        kind, lit = verdict(chart, text)
        case = {"text": text, "kind": kind}
        if kind == "valid":
            case["lit"] = [f["aar"] for f in fns if (f["appliance"], f["word"]) in lit]
            case["start"] = start_of(chart, levers, lit)
        out.append(case)
    return out


def main():
    node = shutil.which("node")
    if not node:
        print("  FAIL  node is not installed: web/ cannot be executed, so nothing here is checked")
        print("FAILED: 1")
        return 1
    model = cc.load(PROJECT)
    result = cc.derive(model)
    charts = {s["station"]: s for s in result["stations"]}
    rng = random.Random(3)
    with tempfile.TemporaryDirectory() as tmp:
        if panel_file.main([str(PROJECT), "--all", tmp]) != 0:
            print("  FAIL  panel_file.py --all")
            return 1
        index = json.loads((Path(tmp) / "panels.json").read_text())
        stations = []
        for entry in index["stations"]:
            chart = charts[entry["station"]]
            levers = lever_names(model, chart)
            controls, indications = codeline_office.topics(entry["station"])
            stations.append({
                "station": entry["station"], "panel": str(Path(tmp) / entry["file"]),
                "controls_topic": controls, "indications_topic": indications,
                "codeline": codeline_office.CHANNELS["codeline"], "simulation": codeline_office.CHANNELS["simulation"],
                "controls": control_cases(chart, levers, rng), "indications": indication_cases(chart, levers, rng)})
        kinds = {}
        for s in stations:
            for c in s["indications"]:
                kinds[c["kind"]] = kinds.get(c["kind"], 0) + 1
        print(f"cases for {len(stations)} stations: {sum(len(s['controls']) for s in stations)} lever settings, "
              f"indication texts {kinds}")
        if set(kinds) != {"valid", "unavailable", "out of sync", "malformed"}:
            print("  FAIL  the cases do not cover valid, unavailable, out of sync and malformed")
            return 1
        cases = Path(tmp) / "cases.json"
        cases.write_text(json.dumps({"stations": stations}))
        done = subprocess.run([node, str(ROOT / "scenarios" / "web_office.mjs"), str(cases)], cwd=ROOT, timeout=120)
    if done.returncode != 0:
        print(f"FAILED: node exited {done.returncode}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
