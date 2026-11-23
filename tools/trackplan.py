#!/usr/bin/env python3
"""The trackplan geometry of the project file, for anything that draws it.

Two users: tools/render_spcoast_svgs.py (the document drawings) and tools/panel_file.py (the
model board and field view of the on-screen panel). Both read the same glyph placements with
the functions here; neither restates how a glyph is placed, which of its lines are rails, or
how a name is lettered.

- pts / fmt_pts / page_of / arc_points: coordinates.
- placed(): one glyph placement as primitives in page millimetres.
- timetable_label(): a name lettered by timetable direction.
- drawing(): one interlocking's trackplan as a flat list of elements, each tagged with the
  name of the thing it shows, in page millimetres. It holds no colours and no pixel sizes:
  whoever draws it decides those.

Standard library only.
"""
from __future__ import annotations

import math
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import codeline_chart  # noqa: E402  (DIRECTION_LETTER, _natural)

# A glyph polyline at least this wide (mm) is rail; thinner lines are the symbol's detail.
RAIL_MIN_WIDTH = 0.35

# Markers drawn on a trackplan (the rest are rule signs and labels of the schematic sheet).
JOINT_GLYPHS = ("IRJ", "IRJ-Signal", "IRJ-Signals", "IRJ_Diag")
END_GLYPHS = ("Bumper",)


def pts(points_str: str) -> list[tuple[float, float]]:
    return [tuple(float(v) for v in xy.split(",")) for xy in points_str.split(" ") if xy]


def fmt_pts(point_list: list[tuple[float, float]]) -> str:
    return " ".join(f"{round(x, 4)},{round(y, 4)}" for x, y in point_list)


def page_of(pl: dict, local: tuple[float, float]) -> tuple[float, float]:
    """A glyph-local point on the page, for a placement {at, rotation, mirror}."""
    x, y = local
    if pl.get("mirror") == "y":
        x = -x
    if pl.get("mirror") == "x":
        y = -y
    a = math.radians(pl.get("rotation", 0))
    return (
        pl["at"]["x"] + x * math.cos(a) + y * math.sin(a),
        pl["at"]["y"] - x * math.sin(a) + y * math.cos(a),
    )


def arc_points(start: tuple[float, float], mid: tuple[float, float], end: tuple[float, float], n: int = 16) -> list[tuple[float, float]]:
    (x1, y1), (x2, y2), (x3, y3) = start, mid, end
    d = 2 * (x1 * (y2 - y3) + x2 * (y3 - y1) + x3 * (y1 - y2))
    if abs(d) < 1e-9:
        return [start, mid, end]
    ux = ((x1 ** 2 + y1 ** 2) * (y2 - y3) + (x2 ** 2 + y2 ** 2) * (y3 - y1) + (x3 ** 2 + y3 ** 2) * (y1 - y2)) / d
    uy = ((x1 ** 2 + y1 ** 2) * (x3 - x2) + (x2 ** 2 + y2 ** 2) * (x1 - x3) + (x3 ** 2 + y3 ** 2) * (x2 - x1)) / d
    r = math.hypot(x1 - ux, y1 - uy)
    a1, a2, a3 = (math.atan2(y - uy, x - ux) for x, y in (start, mid, end))

    def ccw(a: float, b: float) -> float:
        return (b - a) % (2 * math.pi)

    sweep = ccw(a1, a3) if ccw(a1, a2) <= ccw(a1, a3) else -ccw(a3, a1)
    return [(ux + r * math.cos(a1 + sweep * i / n), uy + r * math.sin(a1 + sweep * i / n)) for i in range(n + 1)]


def placed(pl: dict, glyph: dict) -> list[dict]:
    """The primitives of one glyph placement, in page millimetres, in the glyph's own order:

        {"shape": "polyline", "points": [(x, y), ...], "prim": <the glyph primitive>}
        {"shape": "circle", "center": (x, y), "radius": r, "prim": ...}
        {"shape": "text", "at": (x, y), "text": str, "size": mm, "prim": ...}

    Arcs and rectangles come back as polylines. `prim` carries stroke and fill as authored."""
    out = []
    for prim in glyph["shape"]:
        t = prim["type"]
        if t == "polyline":
            local = pts(prim["points"])
        elif t == "arc":
            local = arc_points((prim["start"]["x"], prim["start"]["y"]), (prim["mid"]["x"], prim["mid"]["y"]),
                               (prim["end"]["x"], prim["end"]["y"]))
        elif t == "rectangle":
            a, b = prim["start"], prim["end"]
            local = [(a["x"], a["y"]), (b["x"], a["y"]), (b["x"], b["y"]), (a["x"], b["y"]), (a["x"], a["y"])]
        elif t == "circle":
            out.append({"shape": "circle", "center": page_of(pl, (prim["center"]["x"], prim["center"]["y"])),
                        "radius": prim["radius"], "prim": prim})
            continue
        elif t == "text":
            out.append({"shape": "text", "at": page_of(pl, (prim["at"]["x"], prim["at"]["y"])),
                        "text": prim["text"], "size": prim["size"], "prim": prim})
            continue
        else:
            continue
        out.append({"shape": "polyline", "points": [page_of(pl, p) for p in local], "prim": prim})
    return out


def is_rail(prim: dict) -> bool:
    return prim["type"] == "polyline" and ((prim.get("stroke") or {}).get("width", 0.0) or 0.0) >= RAIL_MIN_WIDTH


_DIRECTED = re.compile(r"^(\d+)([A-Z])([A-D]+|AT)$")


def timetable_label(label: str) -> str:
    """A mast or track name lettered by timetable direction: a design may letter by compass
    (776NAB); the railroad reads timetable (776WAB). The letters are codeline_chart's."""
    m = _DIRECTED.match(label)
    if m and m.group(2) in codeline_chart.DIRECTION_LETTER:
        return f"{m.group(1)}{codeline_chart.DIRECTION_LETTER[m.group(2)]}{m.group(3)}"
    return label


# ---------------------------------------------------------------------------------- drawing

def _local_name(ix_name: str, qualified: str | None) -> str | None:
    """A circuit as this interlocking names it: bare when it is its own, qualified when not."""
    if not qualified:
        return None
    ix, _, name = qualified.partition(":")
    return name if ix == ix_name else qualified


def _xy(p: tuple[float, float]) -> list[float]:
    return [round(p[0], 3), round(p[1], 3)]


def _switch_part(prim: dict, ports: dict) -> str:
    """Which leg of a switch a rail of its glyph is: the port (C, N, R) one of its ends sits on.
    A rail on two ports (a derail: one piece of track) or on none (the derail's arm) is `body`."""
    ends = pts(prim["points"])
    ends = (ends[0], ends[-1])
    on = [name for name, xy in ports.items()
          if any(abs(e[0] - xy["x"]) < 1e-6 and abs(e[1] - xy["y"]) < 1e-6 for e in ends)]
    return on[0] if len(on) == 1 else "body"


def drawing(model: dict, ix_name: str) -> dict:
    """One interlocking's trackplan: {"bounds": [x0, y0, x1, y1], "elements": [...]}, in page mm.

    An element is {"shape": "polyline", "points"} | {"shape": "circle", "center", "radius"} |
    {"shape": "text", "at", "text", "size", "anchor"}, with

        "role"  rail | detail | joint | end | head | lamp | label     how to draw it
        "tag"   {"kind", "name", "part"?, "circuit"?}                  what it shows

        "labels" [first end, last end]: on a rail, the wire label a leaving end carries (or null)

    `tag` is absent on joints, track ends and track of no circuit. kind is the appliance kind of
    the project file (track_circuit, switch, mast, lamp, contact, maintainer_call); name is the
    appliance's name in this interlocking (a track circuit of another interlocking keeps its
    qualified name); part is a switch leg (C, N, R, body) or lamp; circuit, on a switch, is the
    track circuit its rails belong to."""
    il = model["interlockings"][ix_name]
    g = il.get("geometry", {})
    apps = il.get("appliances", {})
    glyphs = model["layout"]["glyphs"]
    out: list[dict] = []

    for t in g.get("track", []):
        el = {"shape": "polyline", "role": "rail", "points": [_xy(p) for p in pts(t["points"])]}
        circuit = _local_name(ix_name, (t.get("derived") or {}).get("circuit"))
        if circuit:
            el["tag"] = {"kind": "track_circuit", "name": circuit}
        # A track that leaves the sheet ends on a wire label; the same label on a neighbour's sheet
        # is the other end of the same track.
        rel = t.get("relations") or {}
        labels = [str(rel.get(k, ""))[len("label:"):] if str(rel.get(k, "")).startswith("label:") else None for k in ("from", "to")]
        if any(labels):
            el["labels"] = labels
        out.append(el)

    for mkr in g.get("markers", []):
        name = mkr["glyph"].split(":")[1]
        if name not in JOINT_GLYPHS and name not in END_GLYPHS:
            continue
        for p in placed(mkr, glyphs[mkr["glyph"]]):
            if p["shape"] == "polyline":
                out.append({"shape": "polyline", "role": "joint" if name in JOINT_GLYPHS else "end",
                            "points": [_xy(q) for q in p["points"]]})

    for k in sorted(g.get("appliances", {}), key=codeline_chart._natural):
        pl = g["appliances"][k]
        given = apps.get(k, {}).get("given", {})
        kind = given.get("Appliance")
        if kind == "lamp" and given.get("Style") == "maintainer_call":
            kind = "maintainer_call"
        if kind not in ("switch", "mast", "track_circuit", "lamp", "contact", "maintainer_call"):
            continue
        glyph = glyphs[pl["glyph"]]
        prims = placed(pl, glyph)
        tag = {"kind": kind, "name": k}
        if kind == "switch":
            d = apps[k].get("derived", {})
            circuit = _local_name(ix_name, d.get("osCircuit") or d.get("track"))
            if circuit:
                tag["circuit"] = circuit
        circles = [p for p in prims if p["shape"] == "circle"]
        for p in prims:
            if p["shape"] == "polyline":
                rail = kind == "switch" and is_rail(p["prim"])
                el = {"shape": "polyline", "role": "rail" if rail else "detail",
                      "points": [_xy(q) for q in p["points"]], "tag": dict(tag)}
                if rail:
                    el["tag"]["part"] = _switch_part(p["prim"], glyph.get("ports", {}))
                out.append(el)
            elif p["shape"] == "circle":
                el = {"shape": "circle", "role": "head" if kind == "mast" else "lamp",
                      "center": _xy(p["center"]), "radius": round(p["radius"], 3), "tag": dict(tag)}
                if kind == "switch":
                    el["tag"]["part"] = "lamp"
                out.append(el)
            elif p["shape"] == "text" and kind == "switch":       # the D of a derail
                out.append({"shape": "text", "role": "label", "at": _xy(p["at"]), "text": p["text"],
                            "size": p["size"], "anchor": "middle", "tag": dict(tag)})
        # The name, beside the symbol (the glyphs' own ${Name} text is a schematic field).
        if kind == "switch":
            ports = {n: page_of(pl, (xy["x"], xy["y"])) for n, xy in glyph.get("ports", {}).items()}
            if circles and "R" in ports:                  # beside the symbol's lamp, away from the diverging leg
                cx, cy = circles[0]["center"]
                at = (cx, cy + 4.4) if ports["R"][1] <= cy else (cx, cy - 2.8)
            else:
                xs = [q[0] for p in prims if p["shape"] == "polyline" for q in p["points"]]
                ys = [q[1] for p in prims if p["shape"] == "polyline" for q in p["points"]]
                at = (sum(xs) / len(xs), max(ys) + 4.0) if xs else (pl["at"]["x"], pl["at"]["y"] + 4.0)
            out.append({"shape": "text", "role": "label", "at": _xy(at), "text": k, "size": 2.2,
                        "anchor": "middle", "tag": dict(tag)})
        elif kind == "mast":
            xs = [q[0] for p in prims if p["shape"] == "polyline" for q in p["points"]]
            ys = [q[1] for p in prims if p["shape"] == "polyline" for q in p["points"]]
            for c in circles:
                xs += [c["center"][0] - c["radius"], c["center"][0] + c["radius"]]
                ys += [c["center"][1] - c["radius"], c["center"][1] + c["radius"]]
            if not xs:
                xs, ys = [pl["at"]["x"]], [pl["at"]["y"]]
            left = pl.get("rotation", 0) == 180
            above = (sum(ys) / len(ys)) < pl["at"]["y"]          # the heads stand off the track
            at = ((max(xs) if left else min(xs)), (min(ys) - 1.0) if above else (max(ys) + 2.6))
            out.append({"shape": "text", "role": "label", "at": _xy(at), "text": timetable_label(k), "size": 1.9,
                        "anchor": "end" if left else "start", "tag": dict(tag)})
        else:
            c = circles[0]["center"] if circles else (pl["at"]["x"], pl["at"]["y"])
            out.append({"shape": "text", "role": "label", "at": _xy((c[0], c[1] - 2.2)), "text": timetable_label(k),
                        "size": 1.9, "anchor": "middle", "tag": dict(tag)})

    xs, ys = [], []
    for el in out:
        for x, y in el.get("points", []) + ([el["center"]] if "center" in el else []) + ([el["at"]] if "at" in el else []):
            xs.append(x)
            ys.append(y)
    bounds = [min(xs), min(ys), max(xs), max(ys)] if xs else [0, 0, 0, 0]
    return {"bounds": [round(v, 3) for v in bounds], "elements": out}
