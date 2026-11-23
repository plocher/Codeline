#!/usr/bin/env python3
"""Derive each code line station's chart from the project file.

A chart is the ordered list of control functions and indication functions of one station
(docs/codeline/README.CodeLine.md). A function is an appliance plus a word, such as switch
829 / NORMAL. This tool also spells each function as its AAR token (docs/bindings/CodeLineMQTT.md),
because that is the encoding in use today; the spelled token is included in the chart's identity.

Where the functions come from (the office description in the project file):

- Controls reflect levers. A desk lever in officeUnits.<unit>.devices gives the controls of the
  appliance it serves (derived.serves). A selector gives a NORMAL or REVERSE control for every
  switch in any of its choices.
- Indications reflect lamps. A lever's own lamps give the indications of its appliance. A
  model-board lamp gives the indication its token names, for the appliance that reports it.

The fixture still states lever and lamp content as FieldUnit tokens (derived.controls,
derived.indications). This tool decodes each one into a function with one table (SUFFIXES) and
spells it again with another (spell). No token is copied through.

Standard library only. Usage:

    python3 tools/codeline_chart.py [project.json] [--station NAME]

Prints JSON to stdout. Exits 1 when the project file has problems the chart cannot resolve; the
problems are listed in the output.
"""
from __future__ import annotations

import hashlib
import json
import re
import sys
from pathlib import Path

DERIVER = "codeline_chart/1"
DEFAULT_PROJECT = Path(__file__).resolve().parent / "spcoast.layout.json"

# The order of classes in a chart, and of words inside one appliance. This is the chart's ordering
# rule; nothing else may restate it.
# A design may letter directions by compass (N, S) or by timetable (W, E); on this railroad north
# is timetable west. Charts always spell the timetable letter, so either lettering gives one chart.
DIRECTION_LETTER = {"N": "W", "S": "E"}

CLASS_ORDER = ("switch", "lock", "signal", "maintainer_call", "track_circuit", "lamp")
WORD_ORDER = {
    "switch": ("NORMAL", "REVERSE"),
    "lock": ("RELEASE", "RELEASED", "REQUEST"),
    "signal": ("LEFT", "RIGHT", "STOP", "TIME"),
    "maintainer_call": ("CALL",),
    "track_circuit": ("OCCUPIED",),
    "lamp": ("LIT",),
}

# Positive-control groups: at most one word of a group may be asserted in one frame.
GROUPS = {
    ("control", "switch"): [("NORMAL", "REVERSE")],
    ("control", "signal"): [("LEFT", "RIGHT", "STOP")],
    ("indication", "switch"): [("NORMAL", "REVERSE")],
    ("indication", "signal"): [("LEFT", "RIGHT", "TIME")],
}

# Decoding the fixture's FieldUnit tokens: lever Style -> token suffix -> (direction, word).
SUFFIXES = {
    "switch": {"NWS": ("control", "NORMAL"), "RWS": ("control", "REVERSE"),
               "NWK": ("indication", "NORMAL"), "RWK": ("indication", "REVERSE")},
    "signal": {"NGS": ("control", "LEFT"), "SGS": ("control", "RIGHT"), "HS": ("control", "STOP"),
               "NGK": ("indication", "LEFT"), "SGK": ("indication", "RIGHT"), "TEK": ("indication", "TIME")},
    "lock": {"WLS": ("control", "RELEASE"), "WLK": ("indication", "RELEASED"), "WLQK": ("indication", "REQUEST")},
    "maintainer_call": {"S": ("control", "CALL"), "K": ("indication", "CALL")},
}


# ----------------------------------------------------------------------------------- spelling

def spell(fn: dict, direction: str) -> str:
    """The AAR token of one function: control tokens end in S, indication tokens in K."""
    cls, name, word = fn["class"], fn["appliance"], fn["word"]
    k = "S" if direction == "control" else "K"
    if cls == "switch":
        return f"{name}{word[0]}W{k}"                       # NORMAL -> NWS/NWK, REVERSE -> RWS/RWK
    if cls == "signal":
        if word == "STOP":
            return f"{name}HS"
        if word == "TIME":
            return f"{name}TEK"
        return f"{name}{fn['timetable']}G{k}"               # LEFT/RIGHT -> the timetable letter of that side
    if cls == "lock":
        return {"RELEASE": f"{name}WLS", "RELEASED": f"{name}WLK", "REQUEST": f"{name}WLQK"}[word]
    if cls == "maintainer_call":
        return f"{name}{k}"
    return name if name.endswith("K") else f"{name}K"      # track circuit, field lamp


# ----------------------------------------------------------------------------------- helpers

def _natural(text: str) -> tuple:
    return tuple(int(p) if p.isdigit() else p for p in re.split(r"(\d+)", text))


def _split(qualified: str) -> tuple[str, str]:
    ix, _, name = qualified.partition(":")
    return ix, name


def _column_number(dev: dict) -> int | None:
    col = dev.get("relations", {}).get("column", "")
    number = col.partition(":")[2]
    return int(number) if number.isdigit() else None


class Deriver:
    def __init__(self, model: dict):
        self.model = model
        self.ixs = model.get("interlockings", {})
        self.problems: list[dict] = []
        self.stations: dict[tuple[str, str | None], dict] = {}
        for ix_name, ix in self.ixs.items():
            scope = self._scope(ix_name)
            if scope == "house":
                for house in ix.get("houses", {}):
                    self._station(ix_name, house)
            elif scope == "interlocking":
                self._station(ix_name, None)

    # -- stations

    def _codeline(self, ix_name: str) -> tuple[str | None, dict]:
        line = self.ixs.get(ix_name, {}).get("relations", {}).get("codeline")
        return line, self.model.get("layout", {}).get("codelines", {}).get(line, {}) if line else {}

    def _scope(self, ix_name: str) -> str | None:
        return self._codeline(ix_name)[1].get("derived", {}).get("station")

    def _station(self, ix_name: str, house: str | None) -> dict:
        key = (ix_name, house)
        if key not in self.stations:
            line, cl = self._codeline(ix_name)
            self.stations[key] = {
                "station": ix_name if house is None else f"{ix_name}:{house}",
                "interlocking": ix_name,
                "house": house,
                "codeline": line,
                "encoding": cl.get("given", {}).get("Kind"),
                "controls": [],
                "indications": [],
            }
        return self.stations[key]

    def _problem(self, where: str, text: str) -> None:
        self.problems.append({"where": where, "text": text})

    def _add(self, station: dict | None, direction: str, fn: dict) -> None:
        if station is None:
            return
        bucket = station["controls" if direction == "control" else "indications"]
        same = [f for f in bucket if (f["class"], f["appliance"], f["word"]) == (fn["class"], fn["appliance"], fn["word"])]
        if same:
            same[0]["sources"] = sorted(set(same[0]["sources"]) | set(fn["sources"]))
            columns = [c for c in (same[0].get("column"), fn.get("column")) if c is not None]
            same[0]["column"] = min(columns) if columns else None     # the leftmost lever that reflects it
        else:
            bucket.append(fn)

    # -- levers and selectors (controls, and the lever's own lamps)

    def _lever_station(self, dev: dict, ix_name: str) -> dict | None:
        scope = self._scope(ix_name)
        if scope == "interlocking":
            return self._station(ix_name, None)
        if scope == "house":
            unit, _, number = dev.get("relations", {}).get("column", "").partition(":")
            house = self.model["officeUnits"].get(unit, {}).get("columns", {}).get(number, {}).get("derived", {}).get("house", "")
            hb, hn = _split(house)
            if hb == ix_name and (ix_name, hn) in self.stations:
                return self.stations[(ix_name, hn)]
        return None

    def _timetable(self, ix_name: str, lever: str, masts: list[str], where: str) -> dict[str, str]:
        """For a signal lever: the timetable direction letter of its LEFT and RIGHT sides, read from
        the names of the masts facing each way (mast id = signal number + direction + heads)."""
        out: dict[str, set[str]] = {"LEFT": set(), "RIGHT": set()}
        apps = self.ixs.get(ix_name, {}).get("appliances", {})
        for qualified in masts:
            name = _split(qualified)[1]
            facing = apps.get(name, {}).get("relations", {}).get("facing")
            if facing in out and name.startswith(lever) and len(name) > len(lever):
                out[facing].add(DIRECTION_LETTER.get(name[len(lever)], name[len(lever)]))
        letters = {}
        for side, found in out.items():
            if len(found) != 1:
                self._problem(where, f"signal {ix_name}:{lever}: masts facing {side} give direction letters "
                                     f"{sorted(found) or 'none'}; need exactly one to spell its {side} functions")
            else:
                letters[side] = found.pop()
        return letters

    def levers(self) -> None:
        for unit_name, unit in self.model.get("officeUnits", {}).items():
            for i, dev in enumerate(unit.get("devices", [])):
                g, d = dev.get("given", {}), dev.get("derived", {})
                if g.get("Appliance") != "lever" or not d.get("serves"):
                    continue
                where = f"officeUnits/{unit_name}/devices/{i}"
                ix_name, name = _split(d["serves"])
                style = g.get("Style", "")
                table = SUFFIXES.get(style)
                if table is None:
                    self._problem(where, f"lever {d['serves']} has Style {style!r}, which has no code line functions")
                    continue
                station = self._lever_station(dev, ix_name)
                letters = self._timetable(ix_name, name, d.get("masts", []), where) if style == "signal" else {}
                for direction, key in (("control", "controls"), ("indication", "indications")):
                    for token in d.get(key, []):
                        tb, bare = _split(token)
                        suffix = bare[len(name):] if bare.startswith(name) else None
                        if tb != ix_name or suffix not in table or table[suffix][0] != direction:
                            self._problem(where, f"lever {d['serves']}: {direction} {token} decodes to no function of a {style} lever")
                            continue
                        word = table[suffix][1]
                        fn = {"class": style, "appliance": name, "word": word, "column": _column_number(dev), "sources": [where]}
                        if style == "signal" and word in ("LEFT", "RIGHT"):
                            if word not in letters:
                                continue                     # reported by _timetable
                            fn["timetable"] = letters[word]
                        self._add(station, direction, fn)

    def selectors(self) -> None:
        for unit_name, unit in self.model.get("officeUnits", {}).items():
            for i, dev in enumerate(unit.get("devices", [])):
                if dev.get("given", {}).get("Appliance") != "selector":
                    continue
                where = f"officeUnits/{unit_name}/devices/{i}"
                for choice in dev.get("derived", {}).get("choices") or []:
                    for entry in choice.get("alignment", []):
                        ix_name, name = _split(entry["appliance"])
                        cp = self.ixs.get(ix_name, {}).get("appliances", {}).get(name, {}).get("derived", {}).get("controlPoint", "")
                        station = self._reporter_station(ix_name, cp)
                        for word in ("NORMAL", "REVERSE"):
                            self._add(station, "control", {"class": "switch", "appliance": name, "word": word,
                                                           "column": None, "sources": [where]})

    # -- lamps (indications)

    def _reporters(self, ix_name: str, bare: str) -> list[tuple[str, str]]:
        """(class, appliance) pairs of one interlocking whose own Indication field states this token.
        A switch states its OS circuit's token; the circuit (its TC) is the appliance of the function."""
        out = []
        for name, a in self.ixs.get(ix_name, {}).get("appliances", {}).items():
            g = a.get("given", {})
            stated = g.get("Indication", "").strip().lstrip("!")
            if stated != bare:
                continue
            kind = g.get("Appliance")
            if kind == "track_circuit":
                out.append(("track_circuit", name, a))
            elif kind == "switch" and g.get("TC", "").strip():
                out.append(("track_circuit", g["TC"].strip(), a))
            elif kind in ("lamp", "contact"):
                out.append(("lamp", name, a))
        return out

    def _reporter_station(self, ix_name: str, control_point: str) -> dict | None:
        scope = self._scope(ix_name)
        if scope == "interlocking":
            return self._station(ix_name, None)
        if scope == "house":
            hb, hn = _split(control_point)
            return self.stations.get((ix_name, hn)) if hb == ix_name else None
        return None

    def lamps(self) -> None:
        for unit_name, unit in self.model.get("officeUnits", {}).items():
            for i, dev in enumerate(unit.get("devices", [])):
                if dev.get("given", {}).get("Appliance") != "lamp":
                    continue
                where = f"officeUnits/{unit_name}/devices/{i}"
                for token in dev.get("derived", {}).get("indications", []):
                    ix_name, bare = _split(token)
                    if ix_name not in self.ixs:
                        self._problem(where, f"lamp consumes {token}, but {ix_name} is no interlocking in this file")
                        continue
                    reps = self._reporters(ix_name, bare)
                    if len({(c, n) for c, n, _ in reps}) != 1:
                        self._problem(where, f"lamp consumes {token}, reported by {len(reps)} appliances of {ix_name}; need exactly one")
                        continue
                    cls, name, app = reps[0]
                    station = self._reporter_station(ix_name, app.get("derived", {}).get("controlPoint", ""))
                    if station is None:
                        self._problem(where, f"lamp consumes {token}, whose reporter belongs to no station")
                        continue
                    word = WORD_ORDER[cls][0]
                    self._add(station, "indication", {"class": cls, "appliance": name, "word": word,
                                                      "column": None, "sources": [where]})

    # -- result

    def charts(self) -> list[dict]:
        self.levers()
        self.selectors()
        self.lamps()
        out = []
        for key in sorted(self.stations, key=lambda k: (k[0], k[1] or "")):
            st = self.stations[key]
            for direction, bucket in (("control", "controls"), ("indication", "indications")):
                st[bucket].sort(key=_order)
                for fn in st[bucket]:
                    fn["aar"] = spell(fn, direction)
            st["chartId"] = chart_id(st)
            out.append(st)
        return out


def _order(fn: dict) -> tuple:
    """The chart ordering rule: class, then panel column (lamps and selectors have none and sort
    after), then appliance name in natural order, then the plain name (so "01" and "1" are ordered
    by code point, not by file order), then the word's place in its class."""
    col = fn.get("column")
    return (CLASS_ORDER.index(fn["class"]), col is None, col or 0, _natural(fn["appliance"]), fn["appliance"],
            WORD_ORDER[fn["class"]].index(fn["word"]))


def canonical(station: dict) -> bytes:
    """The bytes a chart identity is computed over (README.CodeLine.md §8): UTF-8, one line per
    record, each line ended by LF, fields separated by TAB. First line ``CHART1<TAB><station>``;
    then one ``C`` line per control and one ``I`` line per indication, in chart order:
    ``C<TAB><class><TAB><appliance><TAB><word><TAB><token>``. ``token`` is the encoding's
    spelling (the AAR token on an AAR line), empty on an encoding that spells no tokens."""
    spelled = station.get("encoding") == "AAR"
    lines = [f"CHART1\t{station['station']}"]
    for tag, bucket in (("C", "controls"), ("I", "indications")):
        for f in station[bucket]:
            lines.append("\t".join((tag, f["class"], f["appliance"], f["word"], f["aar"] if spelled else "")))
    return "".join(line + "\n" for line in lines).encode("utf-8")


def chart_id(station: dict) -> str:
    """The identity of a chart: the first 8 lowercase hex digits of SHA-256 over canonical()."""
    return hashlib.sha256(canonical(station)).hexdigest()[:8]


def derive(model: dict) -> dict:
    d = Deriver(model)
    stations = d.charts()
    return {"deriver": DERIVER, "schema": model.get("schema"), "stations": stations, "problems": d.problems}


# ----------------------------------------------------------------------------- AAR frames

class FrameError(ValueError):
    """A frame that a receiver must reject as a whole."""


UNAVAILABLE = "UNAVAILABLE"
DIRECTIONS = {"control": "controls", "indication": "indications"}


def _bucket(chart: dict, direction: str) -> list[dict]:
    if direction not in DIRECTIONS:
        raise ValueError(f"direction {direction!r} is not one of {sorted(DIRECTIONS)}")
    return chart[DIRECTIONS[direction]]


def _check_groups(bucket: list[dict], direction: str, asserted: set[tuple[str, str]]) -> None:
    """Positive control: at most one word of each group of one appliance asserted."""
    for appliance in sorted({f["appliance"] for f in bucket}):
        classes = {f["class"] for f in bucket if f["appliance"] == appliance}
        for cls in sorted(classes):
            for group in GROUPS.get((direction, cls), []):
                on = [w for w in group if (appliance, w) in asserted]
                if len(on) > 1:
                    raise FrameError(f"{appliance}: {' and '.join(on)} both asserted")


def encode_frame(chart: dict, direction: str, asserted: set[tuple[str, str]]) -> str:
    """The AAR text frame for one station: CHART <id> then every function of the chart, in chart
    order, asserted (TOKEN) when (appliance, word) is in ``asserted``, else dropped ((TOKEN)).
    Raises FrameError for an (appliance, word) the chart lacks or an invalid group, and ValueError
    for an unknown direction: a writer never produces a frame a reader must reject."""
    bucket = _bucket(chart, direction)
    unknown = sorted(asserted - {(f["appliance"], f["word"]) for f in bucket})
    if unknown:
        raise FrameError(f"no {direction} function {unknown} in chart {chart['chartId']}")
    _check_groups(bucket, direction, asserted)
    terms = [f["aar"] if (f["appliance"], f["word"]) in asserted else f"({f['aar']})" for f in bucket]
    return " ".join(["CHART", chart["chartId"], *terms])


# Term separators: ASCII space, tab, CR and LF, any run of them (CodeLineMQTT.md: readers may
# accept any run of whitespace). Nothing else, so every reader (this one, the field's C++
# CodeLineField) splits a frame the same way; str.split() would also split on NBSP, VT, FF, ...
_SEPARATORS = re.compile(r"[ \t\r\n]+")


def frame_terms(text: str) -> list[str]:
    return [t for t in _SEPARATORS.split(text) if t]


def decode_frame(chart: dict, direction: str, text: str) -> set[tuple[str, str]] | None:
    """The asserted (appliance, word) pairs of a frame; None for an indication frame reading
    UNAVAILABLE (the station is not current). Raises FrameError for a frame whose chart identity
    differs (out of sync) or that is malformed: terms not exactly the chart's functions in chart
    order, or two words of one positive-control group asserted."""
    bucket = _bucket(chart, direction)
    parts = frame_terms(text)
    if direction == "indication" and parts == [UNAVAILABLE]:
        return None
    if parts[:2] != ["CHART", chart["chartId"]]:
        raise FrameError(f"out of sync: {' '.join(parts[:2])!r} is not CHART {chart['chartId']}")
    terms = parts[2:]
    if len(terms) != len(bucket):
        raise FrameError(f"malformed: {len(terms)} terms, chart has {len(bucket)}")
    asserted = set()
    for term, fn in zip(terms, bucket):
        bare = term[1:-1] if term.startswith("(") and term.endswith(")") else term
        if bare != fn["aar"]:
            raise FrameError(f"malformed: term {term!r} where the chart has {fn['aar']}")
        if bare == term:
            asserted.add((fn["appliance"], fn["word"]))
    _check_groups(bucket, direction, asserted)
    return asserted


# ------------------------------------------------------------------------------------- main

def load(path: Path | str = DEFAULT_PROJECT) -> dict:
    with open(path, encoding="utf-8") as f:
        return json.load(f)


USAGE = "usage: codeline_chart.py [project.json] [--station NAME]"


def main(argv: list[str]) -> int:
    args = list(argv)
    if "-h" in args or "--help" in args:
        print(USAGE + "\n\n" + (__doc__ or "").strip())
        return 0
    station = None
    if "--station" in args:
        i = args.index("--station")
        if i + 1 >= len(args):
            print(USAGE, file=sys.stderr)
            return 2
        station = args[i + 1]
        del args[i:i + 2]
    if len(args) > 1 or any(a.startswith("-") for a in args):
        print(USAGE, file=sys.stderr)
        return 2
    result = derive(load(args[0] if args else DEFAULT_PROJECT))
    if station is not None:
        result["stations"] = [s for s in result["stations"] if s["station"] == station]
        if not result["stations"]:
            print(f"codeline_chart: no station named {station!r}", file=sys.stderr)
            return 1
    json.dump(result, sys.stdout, indent=2, sort_keys=False)
    sys.stdout.write("\n")
    return 1 if result["problems"] else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
