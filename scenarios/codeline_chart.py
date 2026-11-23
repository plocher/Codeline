#!/usr/bin/env python3
"""Functional scenario: CP Corporal's derived code line chart against the Walkthrough's packets.

Part 1 asserts what the chart and the Walkthrough agree on, and the frame rules of the contract.
A failure there is a bug in the deriver or the contract.

Part 2 compares every term of every Walkthrough packet with the derived chart and lists each
disagreement. It does not excuse any of them: while one remains, the scenario exits 1. Each one is
a fact for the owner to settle in the project file or in docs/Walkthrough.md.

Run from the repository root: python3 scenarios/codeline_chart.py
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import codeline_chart as cc  # noqa: E402

failures = []


def check(cond, text):
    if not cond:
        failures.append(text)
    print(("  ok    " if cond else "  FAIL  ") + text)


def rejects(chart, direction, text):
    try:
        cc.decode_frame(chart, direction, text)
    except cc.FrameError:
        return True
    return False


model = cc.load(ROOT / "tools" / "spcoast.layout.json")
result = cc.derive(model)
again = cc.derive(cc.load(ROOT / "tools" / "spcoast.layout.json"))
corporal = next((s for s in result["stations"] if s["station"] == "Corporal"), None)

print("Part 1: what the chart and the contract must hold")
check(json.dumps(result) == json.dumps(again), "derivation is deterministic")
check(not result["problems"], f"project file derives with no problems ({len(result['problems'])})")
check(corporal is not None, "a station named Corporal exists")
if corporal is None:
    sys.exit(1)
check(corporal["encoding"] == "AAR" and corporal["house"] is None,
      "Corporal is on an AAR line, so the interlocking is the station")

ctl = [f["aar"] for f in corporal["controls"]]
ind = [f["aar"] for f in corporal["indications"]]
print(f"  chart {corporal['chartId']}\n    controls:    {' '.join(ctl)}\n    indications: {' '.join(ind)}")

check(ctl[:2] == ["829NWS", "829RWS"], "switch 829 controls NORMAL then REVERSE, first in the chart")
check(ind[:2] == ["829NWK", "829RWK"], "switch 829 indications NORMAL then REVERSE, first in the chart")
for lever in ("828", "830"):
    words = [f["word"] for f in corporal["controls"] if f["appliance"] == lever and f["class"] == "signal"]
    check(words == ["LEFT", "RIGHT", "STOP"], f"signal lever {lever} controls are LEFT, RIGHT, STOP in that order")
    iwords = [f["word"] for f in corporal["indications"] if f["appliance"] == lever and f["class"] == "signal"]
    check(iwords == ["LEFT", "RIGHT", "TIME"], f"signal lever {lever} indications are LEFT, RIGHT, TIME in that order")
check("830HS" in ctl, "830HS (signal 830 / STOP) is a control")
check("830TEK" in ind, "830TEK (signal 830 / TIME) is an indication")
check("827WLS" in ctl and "827WLK" in ind and "827WLQK" in ind, "827 is a lock lever (827WLS / 827WLK / 827WLQK)")
classes = [f["class"] for f in corporal["controls"]]
check(classes.index("switch") < classes.index("lock") < classes.index("signal"), "switch, then lock, then signal controls")

# The Walkthrough's lever positions, as far as the chart can carry them.
lined = {("829", "NORMAL"), ("828", "LEFT"), ("830", "LEFT")}
frame = cc.encode_frame(corporal, "control", lined)
check(cc.decode_frame(corporal, "control", frame) == lined, f"control frame round-trips: {frame}")
iframe = cc.encode_frame(corporal, "indication", lined)
check(cc.decode_frame(corporal, "indication", iframe) == lined, f"indication frame round-trips: {iframe}")
check(rejects(corporal, "control", frame.replace("(829RWS)", "829RWS")), "829NWS with 829RWS is rejected")
check(rejects(corporal, "control", frame.replace("(830HS)", "830HS")), "two words of signal 830 asserted is rejected")
check(rejects(corporal, "control", frame.replace(corporal["chartId"], "00000000")), "another chart identity is rejected")
terms = frame.split()
check(rejects(corporal, "control", " ".join(terms[:2] + terms[3:])), "a frame missing a term is rejected")
check(rejects(corporal, "control", " ".join(terms[:2] + [terms[3], terms[2]] + terms[4:])), "out-of-order terms are rejected")
for sep, what in ((" \t ", "a tab"), ("\r\n", "CR LF"), ("   ", "a run of spaces")):
    check(not rejects(corporal, "control", sep.join(terms)), f"terms separated by {what} are read")
for sep, what in (("\u00a0", "NBSP"), ("\x0b", "VT"), ("\x0c", "FF"), ("\x1c", "FS"), ("\u2003", "EM SPACE")):
    check(rejects(corporal, "control", sep.join(terms)), f"terms separated by {what} are rejected (ASCII space, tab, CR, LF only)")
unchanged = cc.encode_frame(corporal, "control", set())
check(cc.decode_frame(corporal, "control", unchanged) == set(), "a frame asserting nothing is valid (all unchanged)")

# Tokens the deriver spells against the tokens the fixture itself states for the same lever or lamp.
devices = {f"officeUnits/{u}/devices/{i}": d for u, unit in model["officeUnits"].items()
           for i, d in enumerate(unit.get("devices", []))}
spelled, notes = 0, []
for st in result["stations"]:
    for direction, bucket in (("control", "controls"), ("indication", "indications")):
        for fn in st[bucket]:
            for src in fn["sources"]:
                stated = devices[src].get("derived", {}).get(bucket)
                if stated is None:                                  # a selector states no tokens
                    continue
                token = f"{st['interlocking']}:{fn['aar']}"
                if token in stated:
                    spelled += 1
                elif fn["class"] == "signal" and fn["word"] in ("LEFT", "RIGHT"):
                    notes.append(f"{token} where the fixture states FieldUnit's left/right letter ({src})")
                else:
                    check(False, f"{token} is not among the tokens {src} states: {stated}")
check(spelled > 0, f"{spelled} spelled tokens equal the fixture's own tokens for the same lever or lamp")
for n in notes:
    print(f"  note  signal spelled by timetable letter: {n}")

# Codec rules the fixture cannot exercise (Corporal has no TIME indication), on a small synthetic chart.
def fn(cls, app, word, **extra):
    return {"class": cls, "appliance": app, "word": word, "column": None, "sources": [], **extra}


syn = {"station": "Test", "encoding": "AAR",
       "controls": [fn("switch", "1", "NORMAL"), fn("switch", "1", "REVERSE"), fn("signal", "2", "LEFT", timetable="W"),
                    fn("signal", "2", "RIGHT", timetable="E"), fn("signal", "2", "STOP")],
       "indications": [fn("switch", "1", "NORMAL"), fn("switch", "1", "REVERSE"), fn("signal", "2", "LEFT", timetable="W"),
                       fn("signal", "2", "RIGHT", timetable="E"), fn("signal", "2", "TIME"), fn("track_circuit", "1T1", "OCCUPIED")]}
for d, b in (("control", "controls"), ("indication", "indications")):
    for f in syn[b]:
        f["aar"] = cc.spell(f, d)
syn["chartId"] = cc.chart_id(syn)
ind_set = {("1", "REVERSE"), ("2", "TIME"), ("1T1", "OCCUPIED")}
syn_ind = cc.encode_frame(syn, "indication", ind_set)
check(cc.decode_frame(syn, "indication", syn_ind) == ind_set, f"synthetic indication round-trips: {syn_ind}")
check(cc.decode_frame(syn, "indication", "UNAVAILABLE") is None, "UNAVAILABLE decodes as station not current")
check(rejects(syn, "control", "UNAVAILABLE"), "UNAVAILABLE is no control frame")
check(rejects(syn, "indication", syn_ind.replace("(2WGK)", "2WGK")), "TIME with LEFT is rejected")
syn_ctl = cc.encode_frame(syn, "control", {("2", "LEFT")})
check(rejects(syn, "control", syn_ctl.replace("(2EGS)", "2EGS")), "signal LEFT with RIGHT is rejected")
t = syn_ctl.split()
check(rejects(syn, "control", " ".join(t[:3] + [t[2]] + t[4:])), "a duplicate token is rejected")
check(rejects(syn, "control", syn_ctl.replace("(1RWS)", "(1XWS)")), "an unknown token is rejected")


def raises(exc, call):
    try:
        call()
    except exc:
        return True
    return False


check(raises(cc.FrameError, lambda: cc.encode_frame(syn, "control", {("1", "NORMAL"), ("1", "REVERSE")})),
      "encode refuses NORMAL with REVERSE")
check(raises(cc.FrameError, lambda: cc.encode_frame(syn, "control", {("9", "NORMAL")})), "encode refuses an unknown function")
check(raises(ValueError, lambda: cc.encode_frame(syn, "controls", set())), "encode refuses an unknown direction")
renamed = json.loads(json.dumps(syn))
renamed["controls"][2]["timetable"] = "N"
for d, b in (("control", "controls"), ("indication", "indications")):
    for f in renamed[b]:
        f["aar"] = cc.spell(f, d)
check(cc.chart_id(renamed) != syn["chartId"], "a spelling change (mast renamed) changes the chart identity")

print("\nPart 2: Walkthrough complete frames against the derived Corporal chart")
cid = corporal["chartId"]
WALKTHROUGH = [
    ("control", "step 1, CODE",
     f"CHART {cid} 829NWS (829RWS) (827WLS) 828WGS (828EGS) (828HS) 830WGS (830EGS) (830HS) (1MCS)"),
    ("indication", "step 2, republish after CODE (829 out of correspondence)",
     f"CHART {cid} (829NWK) (829RWK) (827WLK) (827WLQK) (828WGK) (828EGK) (828TEK) (830WGK) (830EGK) (830TEK) (1MCK) (827T1K) (829T1K)"),
    ("indication", "step 5, 829 locked normal",
     f"CHART {cid} 829NWK (829RWK) (827WLK) (827WLQK) (828WGK) (828EGK) (828TEK) (830WGK) (830EGK) (830TEK) (1MCK) (827T1K) (829T1K)"),
    ("indication", "step 7, lined",
     f"CHART {cid} 829NWK (829RWK) (827WLK) (827WLQK) 828WGK (828EGK) (828TEK) 830WGK (830EGK) (830TEK) (1MCK) (827T1K) (829T1K)"),
    ("control", "cancel 830",
     f"CHART {cid} 829NWS (829RWS) (827WLS) 828WGS (828EGS) (828HS) (830WGS) (830EGS) 830HS (1MCS)"),
    ("indication", "cancel 830, time running",
     f"CHART {cid} 829NWK (829RWK) (827WLK) (827WLQK) (828WGK) (828EGK) (828TEK) (830WGK) (830EGK) 830TEK (1MCK) (827T1K) (829T1K)"),
]
discrepancies = []
for direction, label, frame_text in WALKTHROUGH:
    try:
        decoded = cc.decode_frame(corporal, direction, frame_text)
        reencoded = cc.encode_frame(corporal, direction, decoded)
        if reencoded != frame_text:
            discrepancies.append(f"{label}: round-trip mismatch ({reencoded!r})")
        else:
            print(f"  ok    {label}: valid complete {direction} frame ({len(decoded)} asserted)")
    except cc.FrameError as exc:
        discrepancies.append(f"{label}: {exc}")

for d in discrepancies:
    print(f"  DIFF  {d}")

if failures:
    print(f"\ncodeline_chart: {len(failures)} assertion(s) failed")
if discrepancies:
    print(f"\ncodeline_chart: {len(discrepancies)} discrepancy(ies) between docs/Walkthrough.md and the project file")
sys.exit(1 if failures or discrepancies else 0)
