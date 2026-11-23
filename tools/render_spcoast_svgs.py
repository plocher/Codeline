#!/usr/bin/env python3
"""Render SPCoast corridor overview and per-interlocking detail SVGs directly from spcoast.layout.json
with unified horizontal swimlanes (16.51 mm pitch) and 1-swimlane crossover compression in Christopher."""
from __future__ import annotations

import copy
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from trackplan import fmt_pts, page_of, placed, pts  # noqa: E402  (the shared trackplan geometry)
from trackplan import timetable_label as sp_we_label  # noqa: E402  (N/S -> W/E, codeline_chart.DIRECTION_LETTER)

TOOLS = Path(__file__).resolve().parent
MODEL_PATH = TOOLS / "spcoast.layout.json"
OUT_DIR = TOOLS.parent / "docs" / "images"          # default; `--out DIR` writes elsewhere

# Appliances that stand beside the track (lamps, calls, contacts, train-order boards): they do not
# set a plant's extent. Told by the project file's kind, never by name.
OFF_TRACK = ("lamp", "contact", "train_order")


def off_track(appliance: dict) -> bool:
    return appliance.get("given", {}).get("Appliance") in OFF_TRACK


SWIMLANE_MM = 16.51

# In each interlocking's native geometry frame, this is the Y coordinate (mm) of Swimlane 0:
# - GCaltrain: 1SAT / 1NAT main track is at y = 15.24
# - Gilroy: GCaltrain:1NAT / 02T / Gilroy:1NAT main track is at y = 24.13 (siding 01T is at 7.62 = Lane -1)
# - Luchessa: Gilroy:1NAT / 01T / 2NAT is at y = 43.18 (02T & 3NAT at 26.67 = Lane -1; 1NAT at 59.69 = Lane +1)
# - Christopher (after compressing top track down by +16.51mm): 2NAT is at y = 33.02 (Lane 0), 1NAT is at 49.53 (Lane +1), 01T is at 66.04 (Lane +2)
# - Corporal: 2NAT / 832T / 834T is at y = 35.56 (Lane 0); 1NAT is at 52.07 (Lane +1); 01T/Beet 2 is at 19.05 (Lane -1); Beet 1 is at 2.54 (Lane -2)
# - Sargent: 834T / 836T / 1NAT is at y = 33.02 (Lane 0); 2SAT/Beet 2 is at 16.51 (Lane -1); Beet 1 is at 0.0 (Lane -2)
LANE0_Y: dict[str, float] = {
    "GCaltrain": 15.24,
    "Gilroy": 24.13,
    "Luchessa": 43.18,
    "Christopher": 33.02,
    "Corporal": 35.56,
    "Sargent": 33.02,
}


def compress_christopher_geometry(model: dict) -> None:
    """Compress Christopher's crossover (815 / 815A) so the two main tracks are 1 swimlane (16.51 mm) apart
    instead of 2 swimlanes (33.02 mm).
    In native Christopher geometry:
      - Top main (2NAT) is at y = 16.51 -> shift down by +16.51 to y = 33.02 (Lane 0)
      - Bottom main (1NAT) stays at y = 49.53 (Lane +1)
      - Siding (01T) stays at y = 66.04 (Lane +2)
      - Switch 815 (at x=78.74, y=49.53) and Switch 815A (moved to x=121.92, y=33.02) now meet directly across 1 swimlane!
    """
    il = model["interlockings"]["Christopher"]
    g = il["geometry"]

    # Shift top-track appliances (y <= 25) down by +16.51 mm, and shift 815A left by 16.51 mm so its 45-deg R leg meets 815's R leg
    # Wait: in Switch_Powered:
    #   815 at (78.74, 49.53) has C=(78.74, 49.53), N=(107.95, 49.53), R=(107.95, 33.02).
    #   Notice 815's R port is ALREADY at (107.95, 33.02) — which is y = 33.02 (1 swimlane above 49.53)!
    #   In the original drawing, 815's R port landed at y = 33.02, and then 815A (at y = 16.51, rot=180) dropped another 16.51 mm to y = 33.02!
    #   To make the two main tracks 1 swimlane apart (top track at y = 33.02, bottom track at y = 49.53),
    #   we use a 1-swimlane crossover between y = 49.53 and y = 33.02:
    #   Each switch's diverging leg spans half a swimlane (8.255 mm vertically), OR 815's diverging leg goes from y=49.53 to y=33.02
    #   and meets 815A on the top track at y=33.02!
    #   Let's create a half-height crossover glyph 'Railroad-Trackplan:Switch_Powered_Xover1' whose R port is at (19.05, -8.255)
    #   so 815 (at y=49.53) rises 8.255 mm to y=41.275, and 815A (at y=33.02, rot=180) drops 8.255 mm to y=41.275!
    glyphs = model["layout"]["glyphs"]
    base_sw = copy.deepcopy(glyphs["Railroad-Trackplan:Switch_Powered"])
    base_sw["ports"]["R"] = {"x": 20.955, "y": -8.255}
    for prim in base_sw["shape"]:
        if prim["type"] == "polyline" and prim["points"].startswith("29.21,-16.51"):
            prim["points"] = "20.955,-8.255 18.415,-8.255 12.7,-1.905 10.795,-1.905"
    glyphs["Railroad-Trackplan:Switch_Powered_Xover1"] = base_sw

    # Shift top-track items (10 < y < 25) down by +16.51 mm
    for k, pl in g["appliances"].items():
        if 10.0 <= pl["at"]["y"] <= 25.0:
            pl["at"]["y"] = round(pl["at"]["y"] + SWIMLANE_MM, 2)
    g["appliances"]["815"]["glyph"] = "Railroad-Trackplan:Switch_Powered_Xover1"
    g["appliances"]["815A"]["glyph"] = "Railroad-Trackplan:Switch_Powered_Xover1"
    # With R=(20.955, -8.255):
    #   815 at (78.74, 49.53) -> R port is at (78.74 + 20.955, 49.53 - 8.255) = (99.695, 41.275)
    #   815A at (123.19, 33.02, rot=180) -> R port is at (123.19 - 20.955, 33.02 + 8.255) = (102.235, 41.275)
    #   And the IRJ between 815.R and 815A.R sits at (99.695, 41.275) -> (102.235, 41.275) (exact 2.54 mm IRJ width)!
    g["appliances"]["815A"]["at"]["x"] = 123.19

    for mkr in g["markers"]:
        if 10.0 <= mkr["at"]["y"] <= 25.0:
            mkr["at"]["y"] = round(mkr["at"]["y"] + SWIMLANE_MM, 2)
        elif abs(mkr["at"]["y"] - 33.02) < 1.0 and abs(mkr["at"]["x"] - 107.95) < 5.0:
            # This is the crossover midpoint IRJ between 815.R and 815A.R
            mkr["at"]["x"] = 99.70
            mkr["at"]["y"] = 41.28

    for t in g["track"]:
        new_pts = []
        for x, y in pts(t["points"]):
            if abs(y - 16.51) < 1.0:
                y = round(y + SWIMLANE_MM, 2)
                if abs(x - 110.49) < 1.0:
                    x = 93.98  # 815A.N is now at 123.19 - 29.21 = 93.98
                elif abs(x - 139.7) < 1.0:
                    x = 123.19  # 815A.C is now at 123.19
            new_pts.append((x, y))
        t["points"] = fmt_pts(new_pts)

    # Extend Corporal's right-hand Beet Loader tracks (y=2.54 and y=19.05) to match Corporal's right boundary (x=143.51)
    corp_g = model["interlockings"]["Corporal"]["geometry"]
    corp_g["track"].append({"points": "124.46,2.54 143.51,2.54", "derived": {"circuit": "Corporal:1T1"}})
    corp_g["track"].append({"points": "124.46,19.05 143.51,19.05", "derived": {"circuit": "Corporal:1T1"}})


def render_glyph_svg(
    pl: dict,
    glyph: dict,
    scale: float,
    ox: float = 0.0,
    oy: float = 0.0,
    stroke_override: str | None = None,
    track_col: str | None = None,
    track_width: float = 3.2,
) -> str:
    """Render a glyph placement from spcoast.layout.json into SVG elements."""
    out: list[str] = []

    def tx(p: tuple[float, float]) -> tuple[float, float]:
        return (p[0] + ox) * scale, (p[1] + oy) * scale

    for item in placed(pl, glyph):
        prim = item["prim"]
        st = prim.get("stroke", {})
        fl = prim.get("fill")
        raw_w = st.get("width", 0.0) or 0.0
        is_rail = track_col is not None and raw_w >= 0.35 and prim["type"] == "polyline"

        stroke_col = track_col if is_rail else (stroke_override or "#222222")
        sw = track_width if is_rail else max((raw_w or 0.22) * scale, 1.2)

        style_parts = [f"stroke:{stroke_col}", f"stroke-width:{sw:.1f}", "stroke-linecap:round", "stroke-linejoin:round"]
        if st.get("type"):
            dash_map = {
                "dash": "6 3",
                "dot": "1.5 3",
                "dash_dot": "6 3 1.5 3",
                "dash_dot_dot": "6 3 1.5 3 1.5 3",
            }
            if st["type"] in dash_map:
                style_parts.append(f"stroke-dasharray:{dash_map[st['type']]}")
        if fl:
            if fl["type"] == "color":
                fill_col = fl.get("color", "#888888")
            elif fl["type"] == "background":
                fill_col = "#ffffff"
            else:
                fill_col = stroke_col
            style_parts.append(f"fill:{fill_col}")
            if "opacity" in fl:
                style_parts.append(f"fill-opacity:{fl['opacity']}")
        else:
            style_parts.append("fill:none")
        style_attr = ";".join(style_parts)

        if item["shape"] == "polyline":
            p_str = " ".join(f"{x:.1f},{y:.1f}" for x, y in (tx(p) for p in item["points"]))
            out.append(f'<polyline points="{p_str}" style="{style_attr}"/>')
        elif item["shape"] == "circle":
            cx, cy = tx(item["center"])
            rad = max(item["radius"] * scale, 2.2)
            out.append(f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="{rad:.1f}" style="{style_attr}"/>')
        elif item["shape"] == "text":
            if item["text"].strip().lower() == "signal":
                continue
            x, y = tx(item["at"])
            fs = max(item["size"] * scale * 0.85, 9.0)
            out.append(f'<text x="{x:.1f}" y="{y + fs * 0.35:.1f}" text-anchor="middle" font-size="{fs:.1f}" fill="#222222">{item["text"]}</text>')
    return "\n  ".join(out)


def circuit_regime_color(interlocking: str, circuit: str | None) -> str:
    """Color code track by operating regime:
    - Blue (#1d4ed8): CTC (Lick through Luchessa entry, and Corporal south to Watsonville)
    - Green (#15803d): DTC / Rule 251 Direction of Traffic (Luchessa through Christopher to Corporal)
    - Purple (#7e22ce): Yard Limits / Restricted Speed / Industry / Branch (01T..03T stubs, industry spurs, Beet Loader, Hollister)
    """
    if not circuit:
        return "#7e22ce"
    if circuit in (
        "Luchessa:1NAT", "Luchessa:2NAT",
        "Christopher:1NAT", "Christopher:2NAT", "Christopher:815T1", "Christopher:815AT1",
    ):
        return "#15803d"
    if circuit in (
        "GCaltrain:01T", "GCaltrain:02T", "GCaltrain:03T",
        "Luchessa:02T", "Luchessa:3NAT",
        "Corporal:01T", "Corporal:1T1",
        "Sargent:1T1", "Sargent:2SAT",
    ) or (circuit.startswith("Watsonville:") and not circuit.endswith("837T1")):
        return "#7e22ce"
    return "#1d4ed8"


def render_interlocking_detail(model: dict, name: str, title: str, out_file: Path) -> None:
    il = model["interlockings"][name]
    g = il["geometry"]
    apps = il["appliances"]
    glyphs = model["layout"]["glyphs"]

    all_x = [p[0] for t in g["track"] for p in pts(t["points"])]
    for k, a in g["appliances"].items():
        if off_track(apps.get(k, {})):
            continue
        all_x.append(a["at"]["x"])
        gl = glyphs[a["glyph"]]
        for pv in gl["ports"].values():
            px, _ = page_of(a, (pv["x"], pv["y"]))
            all_x.append(px)

    min_x, max_x = min(all_x), max(all_x)
    span_x = max(max_x - min_x, 10.0)

    svg_w, svg_h = 1120, 380
    pad_left, pad_right = 60.0, 60.0
    avail_w = svg_w - pad_left - pad_right

    # Use a uniform vertical scale so swimlanes have consistent spacing across all detail drawings,
    # and anchor Swimlane 0 at a consistent Y position on the canvas!
    scale = min(avail_w / span_x, 3.8)
    base_x = pad_left - min_x * scale
    ox = base_x / scale

    # Anchor Swimlane 0 at y_canvas = 165 px (or 125 px for GCaltrain which has 3 lanes below Lane 0)
    lane0_canvas_y = 125.0 if name == "GCaltrain" else 165.0
    oy = (lane0_canvas_y / scale) - LANE0_Y[name]

    lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{svg_w}" height="{svg_h}" viewBox="0 0 {svg_w} {svg_h}" font-family="Helvetica, Arial, sans-serif">',
        f'  <title>{title}</title>',
        f'  <rect width="{svg_w}" height="{svg_h}" fill="#ffffff"/>',
        f'  <text x="28" y="38" font-size="20" font-weight="bold" fill="#333333">{title}</text>',
        f'  <rect x="28" y="54" width="{svg_w - 56}" height="{svg_h - 102}" rx="8" fill="#f8fafc" stroke="#cbd5e1" stroke-width="1.5"/>',
    ]

    # Subtle horizontal swimlane guide lines across the canvas
    for lane_idx in range(-2, 4):
        ly = lane0_canvas_y + lane_idx * SWIMLANE_MM * scale
        if 66.0 <= ly <= svg_h - 85.0:
            lines.append(f'  <line x1="40" y1="{ly:.1f}" x2="{svg_w - 40}" y2="{ly:.1f}" stroke="#e2e8f0" stroke-width="1"/>')

    for t in g["track"]:
        circ = (t.get("derived") or {}).get("circuit")
        col = circuit_regime_color(name, circ)
        mapped = []
        for x, y in pts(t["points"]):
            mx = (x + ox) * scale
            my = (y + oy) * scale
            if abs(x - max_x) < 2.0 and name in ("Corporal", "Sargent", "Christopher"):
                mx = svg_w - pad_right
            mapped.append((mx, my))
        p_str = " ".join(f"{mx:.1f},{my:.1f}" for mx, my in mapped)
        lines.append(f'  <polyline points="{p_str}" stroke="{col}" stroke-width="3.6" fill="none" stroke-linecap="round" stroke-linejoin="round"/>')

    for mkr in g["markers"]:
        g_name = mkr["glyph"].split(":")[1]
        if g_name in ("IRJ", "IRJ-Signal", "IRJ-Signals", "Bumper"):
            lines.append("  " + render_glyph_svg(mkr, glyphs[mkr["glyph"]], scale, ox, oy, stroke_override="#b00000" if "IRJ" in g_name else "#475569"))

    switch_bottom_y = svg_h - 62.0
    sw_xs: list[tuple[float, str]] = []
    for k, pl in sorted(g["appliances"].items()):
        app = apps.get(k, {})
        app_kind = app.get("given", {}).get("Appliance")
        if k == "Gilroy" and app_kind == "train_order":
            continue
        if app_kind in ("lamp", "maintainer_call", "contact"):
            continue
        if app_kind == "track_circuit":
            cx, cy = (pl["at"]["x"] + ox) * scale, (pl["at"]["y"] + oy) * scale
            lines.append(f'  <text x="{cx:.1f}" y="{cy - 8:.1f}" text-anchor="middle" font-family="monospace" font-size="11" fill="#475569">{sp_we_label(k)}</text>')
            continue
        if app_kind == "switch":
            os_circ = (app.get("derived") or {}).get("osCircuit")
            if not os_circ and k == "827D":
                os_circ = "Corporal:827T1"
            col = circuit_regime_color(name, os_circ)
            lines.append("  " + render_glyph_svg(pl, glyphs[pl["glyph"]], scale, ox, oy, stroke_override="#333333", track_col=col, track_width=3.6))
            if "Derail" in pl["glyph"] or "Switch_Local" in pl["glyph"] or k.endswith("D"):
                continue
            gl = glyphs[pl["glyph"]]
            ports = {p: page_of(pl, (xy["x"], xy["y"])) for p, xy in gl["ports"].items()}
            if "C" in ports and "R" in ports:
                sx = ((0.65 * ports["C"][0] + 0.35 * ports["R"][0]) + ox) * scale
                sy = (max(p[1] for p in ports.values()) + oy) * scale
            else:
                sx = (pl["at"]["x"] + ox) * scale
                sy = (pl["at"]["y"] + oy) * scale
            lines.append(f'  <line x1="{sx:.1f}" y1="{sy + 6:.1f}" x2="{sx:.1f}" y2="{switch_bottom_y - 14:.1f}" stroke="#cbd5e1" stroke-width="1.2" stroke-dasharray="3 3"/>')
            sw_xs.append((sx, k))
        elif app_kind == "mast":
            gl = glyphs[pl["glyph"]]
            lines.append("  " + render_glyph_svg(pl, gl, scale, ox, oy, stroke_override="#0f172a"))
            xs, ys = [], []
            for prim in gl["shape"]:
                if "points" in prim:
                    for p in pts(prim["points"]):
                        px, py = page_of(pl, p)
                        xs.append((px + ox) * scale)
                        ys.append((py + oy) * scale)
                elif "center" in prim:
                    px, py = page_of(pl, (prim["center"]["x"], prim["center"]["y"]))
                    r = prim["radius"] * scale
                    xs.extend([(px + ox) * scale - r, (px + ox) * scale + r])
                    ys.extend([(py + oy) * scale - r, (py + oy) * scale + r])
            rot = pl.get("rotation", 0)
            ly = ((min(ys) + max(ys)) / 2.0 if ys else (pl["at"]["y"] + oy) * scale) + 3.8
            if rot == 180:
                lx = (min(xs) if xs else (pl["at"]["x"] + ox) * scale) - 4.0
                anchor = "end"
            else:
                lx = (max(xs) if xs else (pl["at"]["x"] + ox) * scale) + 4.0
                anchor = "start"
            lines.append(f'  <text x="{lx:.1f}" y="{ly:.1f}" text-anchor="{anchor}" font-family="monospace" font-size="11" font-weight="bold" fill="#b00000">{sp_we_label(k)}</text>')

    sw_xs.sort(key=lambda item: item[0])
    placed_x: list[float] = []
    for sx, k in sw_xs:
        adj_x = sx
        for px_prev in placed_x:
            if abs(adj_x - px_prev) < 28.0:
                adj_x = px_prev + 30.0
        placed_x.append(adj_x)
        lines.append(f'  <text x="{adj_x:.1f}" y="{switch_bottom_y:.1f}" text-anchor="middle" font-family="monospace" font-size="13" font-weight="bold" fill="#0f172a">{k}</text>')

    leg_y = svg_h - 20
    lines.append(f'  <line x1="36" y1="{leg_y - 4}" x2="66" y2="{leg_y - 4}" stroke="#1d4ed8" stroke-width="4" stroke-linecap="round"/>')
    lines.append(f'  <text x="74" y="{leg_y}" font-size="12" fill="#333333" font-weight="bold">CTC (Rule 261 / Ch. 10)</text>')
    lines.append(f'  <line x1="240" y1="{leg_y - 4}" x2="270" y2="{leg_y - 4}" stroke="#15803d" stroke-width="4" stroke-linecap="round"/>')
    lines.append(f'  <text x="278" y="{leg_y}" font-size="12" fill="#333333" font-weight="bold">DTC / Rule 251 Direction of Traffic</text>')
    lines.append(f'  <line x1="520" y1="{leg_y - 4}" x2="550" y2="{leg_y - 4}" stroke="#7e22ce" stroke-width="4" stroke-linecap="round"/>')
    lines.append(f'  <text x="558" y="{leg_y}" font-size="12" fill="#333333" font-weight="bold">Yard Limits (Rule 93) / Industry (Rule 105) / Branch</text>')
    lines.append(f'  <text x="{svg_w - 36}" y="{leg_y}" text-anchor="end" font-size="12" fill="#555555">Switch numbers shown along bottom</text>')

    lines.append("</svg>")
    out_file.write_text("\n".join(lines) + "\n", encoding="utf-8")


def render_watsonville_detail(out_file: Path) -> None:
    svg_w, svg_h = 1120, 400
    lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{svg_w}" height="{svg_h}" viewBox="0 0 {svg_w} {svg_h}" font-family="Helvetica, Arial, sans-serif">',
        '  <title>Watsonville</title>',
        f'  <rect width="{svg_w}" height="{svg_h}" fill="#ffffff"/>',
        '  <text x="28" y="38" font-size="20" font-weight="bold" fill="#333333">Watsonville</text>',
        f'  <rect x="28" y="56" width="{svg_w - 56}" height="{svg_h - 104}" rx="8" fill="#f8fafc" stroke="#cbd5e1" stroke-width="1.5"/>',
    ]

    ty_north = 165.0
    ty_south = 265.0
    sw837_x = 180.0
    ybox_x, ybox_y, ybox_w, ybox_h = 460.0, 130.0, 360.0, 70.0

    # Incoming CTC mainline from Sargent to Switch 837
    lines.append(f'  <line x1="70" y1="{ty_north:.1f}" x2="{sw837_x:.1f}" y2="{ty_north:.1f}" stroke="#1d4ed8" stroke-width="4" stroke-linecap="round"/>')
    lines.append(f'  <text x="120" y="{ty_north - 14:.1f}" text-anchor="middle" font-family="monospace" font-size="12" fill="#1d4ed8" font-weight="bold">Sargent:1WAT</text>')

    # Departure track on west/north (Lane 0) from Switch 837 to left edge of Watsonville box
    lines.append(f'  <line x1="{sw837_x:.1f}" y1="{ty_north:.1f}" x2="{ybox_x:.1f}" y2="{ty_north:.1f}" stroke="#15803d" stroke-width="4" stroke-linecap="round"/>')
    lines.append(f'  <circle cx="{sw837_x + 12:.1f}" cy="{ty_north:.1f}" r="3.0" fill="#ff0000" stroke="#222222" stroke-width="1"/>')
    lines.append(f'  <text x="{(sw837_x + ybox_x) / 2:.1f}" y="{ty_north - 14:.1f}" text-anchor="middle" font-size="13" font-weight="bold" fill="#15803d">← Departure (Westbound)</text>')

    # Arrival track diverging below the box, running underneath the box, and doing a U-turn around the east end into the box
    lines.append(f'  <polyline points="{sw837_x + 12:.1f},{ty_north:.1f} {sw837_x + 82:.1f},{ty_south:.1f} {ybox_x:.1f},{ty_south:.1f}" stroke="#15803d" stroke-width="4" fill="none" stroke-linecap="round" stroke-linejoin="round"/>')
    lines.append(f'  <text x="{(sw837_x + 82 + ybox_x) / 2:.1f}" y="{ty_south - 12:.1f}" text-anchor="middle" font-size="13" font-weight="bold" fill="#15803d">Arrival (Eastbound) →</text>')

    r_uturn = (ty_south - ty_north) / 2.0
    uturn_x = ybox_x + ybox_w + 24.0
    uturn_path = (
        f"M {ybox_x:.1f} {ty_south:.1f} "
        f"L {uturn_x:.1f} {ty_south:.1f} "
        f"A {r_uturn:.1f} {r_uturn:.1f} 0 0 0 {uturn_x:.1f} {ty_north:.1f} "
        f"L {ybox_x + ybox_w:.1f} {ty_north:.1f}"
    )
    lines.append(f'  <path d="{uturn_path}" stroke="#7e22ce" stroke-width="4" fill="none" stroke-linecap="round" stroke-linejoin="round"/>')

    # Watsonville box sits cleanly on the north track above ty_south (no intersection with the arrival track)
    lines.append(f'  <rect x="{ybox_x:.1f}" y="{ybox_y:.1f}" width="{ybox_w:.1f}" height="{ybox_h:.1f}" rx="8" fill="#faf5ff" stroke="#7e22ce" stroke-width="2.5"/>')
    lines.append(f'  <text x="{ybox_x + ybox_w / 2:.1f}" y="{ybox_y + 32:.1f}" text-anchor="middle" font-size="16" font-weight="bold" fill="#7e22ce">WATSONVILLE STAGING YARD</text>')
    lines.append(f'  <text x="{ybox_x + ybox_w / 2:.1f}" y="{ybox_y + 54:.1f}" text-anchor="middle" font-size="12.5" fill="#555555">7 Loop Tracks (Rule 93)</text>')

    switch_bottom_y = svg_h - 62.0
    lines.append(f'  <line x1="{sw837_x + 12:.1f}" y1="{ty_north + 10:.1f}" x2="{sw837_x + 12:.1f}" y2="{switch_bottom_y - 14:.1f}" stroke="#cbd5e1" stroke-width="1.2" stroke-dasharray="3 3"/>')
    lines.append(f'  <text x="{sw837_x + 12:.1f}" y="{switch_bottom_y:.1f}" text-anchor="middle" font-family="monospace" font-size="13" font-weight="bold" fill="#0f172a">837</text>')

    leg_y = svg_h - 20
    lines.append(f'  <line x1="36" y1="{leg_y - 4}" x2="66" y2="{leg_y - 4}" stroke="#1d4ed8" stroke-width="4" stroke-linecap="round"/>')
    lines.append(f'  <text x="74" y="{leg_y}" font-size="12" fill="#333333" font-weight="bold">CTC (Rule 261 / Ch. 10)</text>')
    lines.append(f'  <line x1="240" y1="{leg_y - 4}" x2="270" y2="{leg_y - 4}" stroke="#15803d" stroke-width="4" stroke-linecap="round"/>')
    lines.append(f'  <text x="278" y="{leg_y}" font-size="12" fill="#333333" font-weight="bold">DTC / Rule 251 Direction of Traffic</text>')
    lines.append(f'  <line x1="520" y1="{leg_y - 4}" x2="550" y2="{leg_y - 4}" stroke="#7e22ce" stroke-width="4" stroke-linecap="round"/>')
    lines.append(f'  <text x="558" y="{leg_y}" font-size="12" fill="#333333" font-weight="bold">Yard Limits (Rule 93) / Staging Loop</text>')

    lines.append("</svg>")
    out_file.write_text("\n".join(lines) + "\n", encoding="utf-8")


def render_full_corridor(model: dict, out_file: Path) -> None:
    """Render the complete SPCoast corridor overview in two horizontal bands with unified horizontal swimlanes
    so mainline tracks align across adjacent interlockings."""
    glyphs = model["layout"]["glyphs"]
    svg_w, svg_h = 1120, 548

    # Uniform vertical & horizontal scale across the corridor panels so every swimlane is 16.51 * SC pixels apart
    # and the 4 panels in Row 1 fill the full width with tracks extending edge-to-edge inside each panel box.
    SC = 1.2937
    LANE_PX = SWIMLANE_MM * SC  # ~21.36 px per swimlane

    lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{svg_w}" height="{svg_h}" viewBox="0 0 {svg_w} {svg_h}" font-family="Helvetica, Arial, sans-serif">',
        '  <title>SPCoast South Subdivision</title>',
        f'  <rect width="{svg_w}" height="{svg_h}" fill="#ffffff"/>',
    ]

    # Draw subtle continuous swimlane guide lines across Row 1 and Row 2
    row1_y, row1_h = 16.0, 224.0
    row1_lane0_y = row1_y + 82.0  # Lane 0 (Main Track) in Row 1
    for lane_idx in range(-1, 4):
        ly = row1_lane0_y + lane_idx * LANE_PX
        lines.append(f'  <line x1="28" y1="{ly:.1f}" x2="{svg_w - 28}" y2="{ly:.1f}" stroke="#e2e8f0" stroke-width="1"/>')

    row2_y, row2_h = 256.0, 224.0
    row2_lane0_y = row2_y + 108.0  # Lane 0 (Main Track) in Row 2 (leaves room for Beet Loader Lanes -1 and -2 above, and Lane +1 below)
    for lane_idx in range(-2, 3):
        ly = row2_lane0_y + lane_idx * LANE_PX
        lines.append(f'  <line x1="28" y1="{ly:.1f}" x2="{svg_w - 28}" y2="{ly:.1f}" stroke="#e2e8f0" stroke-width="1"/>')

    def draw_panel(name: str, label: str, bx: float, by: float, bw: float, bh: float, lane0_canvas_y: float) -> tuple[float, float]:
        lines.append(f'  <rect x="{bx:.1f}" y="{by:.1f}" width="{bw:.1f}" height="{bh:.1f}" rx="8" fill="#f8fafc" fill-opacity="0.7" stroke="#94a3b8" stroke-width="1.5"/>')
        lines.append(f'  <text x="{bx + 12:.1f}" y="{by + 20:.1f}" font-size="13.5" font-weight="bold" fill="#0f172a">{label}</text>')

        il = model["interlockings"][name]
        g = il["geometry"]
        apps = il["appliances"]

        all_x = [p[0] for t in g["track"] for p in pts(t["points"])]
        for k, a in g["appliances"].items():
            if off_track(apps.get(k, {})):
                continue
            all_x.append(a["at"]["x"])
            gl = glyphs[a["glyph"]]
            for pv in gl["ports"].values():
                px, _ = page_of(a, (pv["x"], pv["y"]))
                all_x.append(px)

        min_x, max_x = min(all_x), max(all_x)
        span_x = max(max_x - min_x, 10.0)

        pad_l, pad_r = 10.0, 10.0
        aw = bw - pad_l - pad_r
        sc_x = min(aw / span_x, SC)

        # Anchor left boundary at bx + pad_l so tracks start at the left edge of the box
        px0 = bx + pad_l - min_x * sc_x
        ox = px0 / sc_x
        oy = (lane0_canvas_y / SC) - LANE0_Y[name]

        def map_pt(pt_mm: tuple[float, float]) -> tuple[float, float]:
            return (pt_mm[0] + ox) * sc_x, (pt_mm[1] + oy) * SC

        # Track runs
        for t in g["track"]:
            circ = (t.get("derived") or {}).get("circuit")
            col = circuit_regime_color(name, circ)
            pts_mm = pts(t["points"])
            mapped = []
            for x_mm, y_mm in pts_mm:
                mx, my = map_pt((x_mm, y_mm))
                # If a track segment reaches the right edge of the interlocking (max_x), extend it to fill the box width
                if abs(x_mm - max_x) < 2.0:
                    mx = bx + bw - pad_r
                mapped.append((mx, my))
            p_str = " ".join(f"{mx:.1f},{my:.1f}" for mx, my in mapped)
            lines.append(f'  <polyline points="{p_str}" stroke="{col}" stroke-width="3.0" fill="none" stroke-linecap="round" stroke-linejoin="round"/>')

        # IRJs and Bumpers
        for mkr in g["markers"]:
            g_name = mkr["glyph"].split(":")[1]
            if g_name in ("IRJ", "IRJ-Signal", "IRJ-Signals", "Bumper"):
                gl = glyphs[mkr["glyph"]]
                for prim in gl["shape"]:
                    if prim["type"] == "polyline":
                        p_str = " ".join(f"{map_pt(page_of(mkr, p))[0]:.1f},{map_pt(page_of(mkr, p))[1]:.1f}" for p in pts(prim["points"]))
                        col = "#b00000" if "IRJ" in g_name else "#475569"
                        lines.append(f'  <polyline points="{p_str}" stroke="{col}" stroke-width="1.5" fill="none"/>')

        # Switches & Masts
        sw_label_y = by + bh - 10.0
        sw_xs: list[tuple[float, str]] = []
        for k, pl in sorted(g["appliances"].items()):
            app = apps.get(k, {})
            app_kind = app.get("given", {}).get("Appliance")
            if k == "Gilroy" and app_kind == "train_order":
                continue
            if app_kind == "switch":
                os_circ = (app.get("derived") or {}).get("osCircuit")
                if not os_circ and k == "827D":
                    os_circ = "Corporal:827T1"
                col = circuit_regime_color(name, os_circ)
                gl = glyphs[pl["glyph"]]
                for prim in gl["shape"]:
                    st = prim.get("stroke", {})
                    raw_w = st.get("width", 0.0) or 0.0
                    if prim["type"] == "polyline" and raw_w >= 0.35:
                        p_str = " ".join(f"{map_pt(page_of(pl, p))[0]:.1f},{map_pt(page_of(pl, p))[1]:.1f}" for p in pts(prim["points"]))
                        lines.append(f'  <polyline points="{p_str}" stroke="{col}" stroke-width="3.0" fill="none" stroke-linecap="round" stroke-linejoin="round"/>')
                    elif prim["type"] == "circle":
                        cx, cy = map_pt(page_of(pl, (prim["center"]["x"], prim["center"]["y"])))
                        fl_col = (prim.get("fill") or {}).get("color", "#ff0000")
                        lines.append(f'  <circle cx="{cx:.1f}" cy="{cy:.1f}" r="2.2" fill="{fl_col}" stroke="#222222" stroke-width="1"/>')
                # Skip switch number callouts for derails and local non-dispatcher switches
                if "Derail" in pl["glyph"] or "Switch_Local" in pl["glyph"] or k.endswith("D"):
                    continue
                ports = {p: map_pt(page_of(pl, (xy["x"], xy["y"]))) for p, xy in gl["ports"].items()}
                if "C" in ports and "R" in ports:
                    sx = 0.65 * ports["C"][0] + 0.35 * ports["R"][0]
                    sy = max(p[1] for p in ports.values())
                else:
                    sx, sy = map_pt((pl["at"]["x"], pl["at"]["y"]))
                sw_xs.append((sx, sy, k))
            elif app_kind == "mast":
                gl = glyphs[pl["glyph"]]
                for prim in gl["shape"]:
                    if prim["type"] == "polyline":
                        p_str = " ".join(f"{map_pt(page_of(pl, p))[0]:.1f},{map_pt(page_of(pl, p))[1]:.1f}" for p in pts(prim["points"]))
                        lines.append(f'  <polyline points="{p_str}" stroke="#0f172a" stroke-width="1.3" fill="none"/>')
                    elif prim["type"] == "circle":
                        cx, cy = map_pt(page_of(pl, (prim["center"]["x"], prim["center"]["y"])))
                        rad = max(prim["radius"] * SC, 2.0)
                        lines.append(f'  <circle cx="{cx:.1f}" cy="{cy:.1f}" r="{rad:.1f}" fill="#ffffff" stroke="#0f172a" stroke-width="1.2"/>')

        sw_xs.sort(key=lambda item: item[0])
        placed_x: list[float] = []
        for sx, sy, k in sw_xs:
            adj_x = sx
            for px_prev in placed_x:
                if abs(adj_x - px_prev) < 28.0:
                    adj_x = px_prev + 28.0
            placed_x.append(adj_x)
            lines.append(f'  <line x1="{sx:.1f}" y1="{sy + 4:.1f}" x2="{adj_x:.1f}" y2="{sw_label_y - 11:.1f}" stroke="#cbd5e1" stroke-width="1" stroke-dasharray="2 2"/>')
            lines.append(f'  <text x="{adj_x:.1f}" y="{sw_label_y:.1f}" text-anchor="middle" font-family="monospace" font-size="10.5" font-weight="bold" fill="#0f172a">{k}</text>')

        return bx + pad_l, bx + bw - pad_r

    # Top Row: Gilroy Caltrain, Gilroy, Luchessa, Christopher (widths sized directly to native track spans at SC)
    _, gc_r = draw_panel("GCaltrain", "Gilroy Caltrain", 28, row1_y, 232, row1_h, row1_lane0_y)
    gi_l, gi_r = draw_panel("Gilroy", "Gilroy", 272, row1_y, 206, row1_h, row1_lane0_y)
    lu_l, lu_r = draw_panel("Luchessa", "Luchessa", 490, row1_y, 285, row1_h, row1_lane0_y)
    ch_l, _ = draw_panel("Christopher", "Christopher", 787, row1_y, 305, row1_h, row1_lane0_y)

    # Connect Top Row inter-panel tracks along Swimlanes
    lines.append(f'  <line x1="{gc_r:.1f}" y1="{row1_lane0_y:.1f}" x2="{gi_l:.1f}" y2="{row1_lane0_y:.1f}" stroke="#1d4ed8" stroke-width="3.0" stroke-dasharray="4 3"/>')
    lines.append(f'  <line x1="{gi_r:.1f}" y1="{row1_lane0_y:.1f}" x2="{lu_l:.1f}" y2="{row1_lane0_y:.1f}" stroke="#1d4ed8" stroke-width="3.0" stroke-dasharray="4 3"/>')
    lines.append(f'  <line x1="{lu_r:.1f}" y1="{row1_lane0_y:.1f}" x2="{ch_l:.1f}" y2="{row1_lane0_y:.1f}" stroke="#15803d" stroke-width="3.0" stroke-dasharray="4 3"/>')
    lines.append(f'  <line x1="{lu_r:.1f}" y1="{row1_lane0_y + LANE_PX:.1f}" x2="{ch_l:.1f}" y2="{row1_lane0_y + LANE_PX:.1f}" stroke="#15803d" stroke-width="3.0" stroke-dasharray="4 3"/>')

    # Bottom Row: Corporal, Sargent, and Watsonville (shrunk box widths + extended exit tracks so tracks fill each box)
    _, co_r = draw_panel("Corporal", "Corporal", 28, row2_y, 232, row2_h, row2_lane0_y)
    sa_l, sa_r = draw_panel("Sargent", "Sargent", 272, row2_y, 206, row2_h, row2_lane0_y)

    # Connect Corporal -> Sargent along Swimlane 0 (CTC Blue) and Swimlanes -1 & -2 (Beet Loader Purple)
    lines.append(f'  <line x1="{co_r:.1f}" y1="{row2_lane0_y:.1f}" x2="{sa_l:.1f}" y2="{row2_lane0_y:.1f}" stroke="#1d4ed8" stroke-width="3.0" stroke-dasharray="4 3"/>')
    lines.append(f'  <line x1="{co_r:.1f}" y1="{row2_lane0_y - LANE_PX:.1f}" x2="{sa_l:.1f}" y2="{row2_lane0_y - LANE_PX:.1f}" stroke="#7e22ce" stroke-width="2.5" stroke-dasharray="4 3"/>')
    lines.append(f'  <line x1="{co_r:.1f}" y1="{row2_lane0_y - 2 * LANE_PX:.1f}" x2="{sa_l:.1f}" y2="{row2_lane0_y - 2 * LANE_PX:.1f}" stroke="#7e22ce" stroke-width="2.5" stroke-dasharray="4 3"/>')

    # Watsonville Simplified Box aligned on Swimlane 0 (row2_lane0_y) with Arrival track routing cleanly around the south end of the box
    wx, wy, ww, wh = 490.0, row2_y, 380.0, row2_h
    lines.append(f'  <rect x="{wx:.1f}" y="{wy:.1f}" width="{ww:.1f}" height="{wh:.1f}" rx="8" fill="#f8fafc" fill-opacity="0.7" stroke="#94a3b8" stroke-width="1.5"/>')
    lines.append(f'  <text x="{wx + 12:.1f}" y="{wy + 20:.1f}" font-size="13.5" font-weight="bold" fill="#0f172a">Watsonville</text>')

    ty_north = row2_lane0_y
    ty_south = row2_lane0_y + 2.0 * LANE_PX
    sw837_x = wx + 46.0
    ybox_x, ybox_y, ybox_w, ybox_h = wx + 150.0, ty_north - 20.0, 172.0, 40.0

    # Sargent -> Watsonville connector on Swimlane 0 (CTC Blue)
    lines.append(f'  <line x1="{sa_r:.1f}" y1="{ty_north:.1f}" x2="{wx + 10:.1f}" y2="{ty_north:.1f}" stroke="#1d4ed8" stroke-width="3.0" stroke-dasharray="4 3"/>')
    lines.append(f'  <line x1="{wx + 10:.1f}" y1="{ty_north:.1f}" x2="{sw837_x:.1f}" y2="{ty_north:.1f}" stroke="#1d4ed8" stroke-width="3.2" stroke-linecap="round"/>')

    # Departure track on Swimlane 0 (North) from Switch 837 to left edge of Watsonville box
    lines.append(f'  <line x1="{sw837_x:.1f}" y1="{ty_north:.1f}" x2="{ybox_x:.1f}" y2="{ty_north:.1f}" stroke="#15803d" stroke-width="3.2" stroke-linecap="round"/>')
    lines.append(f'  <circle cx="{sw837_x + 8:.1f}" cy="{ty_north:.1f}" r="2.2" fill="#ff0000" stroke="#222222" stroke-width="1"/>')
    lines.append(f'  <text x="{(sw837_x + ybox_x) / 2 + 4:.1f}" y="{ty_north - 8:.1f}" text-anchor="middle" font-size="10" font-weight="bold" fill="#15803d">← Departure</text>')

    # Arrival track diverging south to ty_south (well below the bottom of ybox), running beneath ybox, and U-turning around the east/south end into the right side of ybox
    lines.append(f'  <polyline points="{sw837_x + 8:.1f},{ty_north:.1f} {sw837_x + 44:.1f},{ty_south:.1f} {ybox_x:.1f},{ty_south:.1f}" stroke="#15803d" stroke-width="3.2" fill="none" stroke-linecap="round" stroke-linejoin="round"/>')
    lines.append(f'  <text x="{(sw837_x + 44 + ybox_x) / 2:.1f}" y="{ty_south + 14:.1f}" text-anchor="middle" font-size="10" font-weight="bold" fill="#15803d">Arrival →</text>')

    r_uturn = (ty_south - ty_north) / 2.0
    uturn_x = ybox_x + ybox_w + 14.0
    uturn_path = (
        f"M {ybox_x:.1f} {ty_south:.1f} "
        f"L {uturn_x:.1f} {ty_south:.1f} "
        f"A {r_uturn:.1f} {r_uturn:.1f} 0 0 0 {uturn_x:.1f} {ty_north:.1f} "
        f"L {ybox_x + ybox_w:.1f} {ty_north:.1f}"
    )
    lines.append(f'  <path d="{uturn_path}" stroke="#7e22ce" stroke-width="3.2" fill="none" stroke-linecap="round" stroke-linejoin="round"/>')

    # Watsonville Yard box centered on ty_north (y = ty_north - 20 .. ty_north + 20, completely clear of ty_south = ty_north + 42.7)
    lines.append(f'  <rect x="{ybox_x:.1f}" y="{ybox_y:.1f}" width="{ybox_w:.1f}" height="{ybox_h:.1f}" rx="6" fill="#faf5ff" stroke="#7e22ce" stroke-width="2"/>')
    lines.append(f'  <text x="{ybox_x + ybox_w / 2:.1f}" y="{ybox_y + 18:.1f}" text-anchor="middle" font-size="11.5" font-weight="bold" fill="#7e22ce">WATSONVILLE YARD</text>')
    lines.append(f'  <text x="{ybox_x + ybox_w / 2:.1f}" y="{ybox_y + 32:.1f}" text-anchor="middle" font-size="10" fill="#555555">7 Loop Tracks (Rule 93)</text>')

    sw_label_y = wy + wh - 10.0
    lines.append(f'  <line x1="{sw837_x + 8:.1f}" y1="{ty_north + 6:.1f}" x2="{sw837_x + 8:.1f}" y2="{sw_label_y - 11:.1f}" stroke="#cbd5e1" stroke-width="1" stroke-dasharray="2 2"/>')
    lines.append(f'  <text x="{sw837_x + 8:.1f}" y="{sw_label_y:.1f}" text-anchor="middle" font-family="monospace" font-size="10.5" font-weight="bold" fill="#0f172a">837</text>')

    # Bottom Regime Color Legend Banner
    leg_y = 516.0
    lines.append(f'  <rect x="28" y="492" width="{svg_w - 56}" height="40" rx="6" fill="#fff3cc" stroke="#8a6d00" stroke-width="1.5"/>')
    lines.append(f'  <line x1="48" y1="{leg_y - 4}" x2="82" y2="{leg_y - 4}" stroke="#1d4ed8" stroke-width="4" stroke-linecap="round"/>')
    lines.append(f'  <text x="90" y="{leg_y}" font-size="12.5" font-weight="bold" fill="#1e293b">CTC (Gilroy Caltrain to Luchessa &amp; Corporal to Watsonville)</text>')

    lines.append(f'  <line x1="455" y1="{leg_y - 4}" x2="489" y2="{leg_y - 4}" stroke="#15803d" stroke-width="4" stroke-linecap="round"/>')
    lines.append(f'  <text x="497" y="{leg_y}" font-size="12.5" font-weight="bold" fill="#1e293b">Rule 251 Current of Traffic (Luchessa to Corporal)</text>')

    lines.append(f'  <line x1="835" y1="{leg_y - 4}" x2="869" y2="{leg_y - 4}" stroke="#7e22ce" stroke-width="4" stroke-linecap="round"/>')
    lines.append(f'  <text x="877" y="{leg_y}" font-size="12.5" font-weight="bold" fill="#1e293b">Yard Limits / Industry / Branch</text>')

    lines.append("</svg>")
    out_file.write_text("\n".join(lines) + "\n", encoding="utf-8")


def render_corporal_sargent_combined(model: dict, out_file: Path) -> None:
    """Render a large, annotated combined Corporal + Sargent diagram for the Section 3 walkthrough."""
    glyphs = model["layout"]["glyphs"]
    corp = model["interlockings"]["Corporal"]
    sarg = model["interlockings"]["Sargent"]

    svg_w, svg_h = 1120, 430
    pad_l, pad_r = 56.0, 56.0
    # Corporal spans x = 0..143.51 mm; Sargent spans x = 0..109.22 mm.
    # Join Sargent directly at x_offset = 143.51 mm (since Corporal:834T at 143.51 meets Sargent's Corporal:834T at 0.0)
    sarg_dx = 143.51
    sarg_dy = LANE0_Y["Corporal"] - LANE0_Y["Sargent"]  # +2.54 mm so Lane 0 (35.56) matches Sargent's Lane 0 (33.02)
    total_mm_x = sarg_dx + 109.22  # 252.73 mm

    scale = (svg_w - pad_l - pad_r) / total_mm_x  # ~3.99 px/mm
    ox = pad_l / scale
    lane0_canvas_y = 218.0
    oy = (lane0_canvas_y / scale) - LANE0_Y["Corporal"]

    lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{svg_w}" height="{svg_h}" viewBox="0 0 {svg_w} {svg_h}" font-family="Helvetica, Arial, sans-serif">',
        '  <title>CP Corporal and CP Sargent — Combined Trackplan</title>',
        '  <defs>',
        '    <marker id="arr-nb" viewBox="0 0 10 10" refX="8" refY="5" markerWidth="7" markerHeight="7" orient="auto">',
        '      <path d="M0,1 L9,5 L0,9 z" fill="#b45309"/>',
        '    </marker>',
        '  </defs>',
        f'  <rect width="{svg_w}" height="{svg_h}" fill="#ffffff"/>',
        f'  <rect x="20" y="16" width="{svg_w - 40}" height="{svg_h - 62}" rx="8" fill="#f8fafc" stroke="#cbd5e1" stroke-width="1.5"/>',
    ]

    # Subtle region dividers for CP Corporal, Intermediate APB/CTC Blocks (831/832 & 833/834), and CP Sargent
    x_corp_end = (93.98 + ox) * scale
    x_sarg_start = (sarg_dx + 46.99 + ox) * scale
    lines.append(f'  <rect x="28" y="24" width="{x_corp_end - 28:.1f}" height="28" rx="5" fill="#e2e8f0"/>')
    lines.append(f'  <text x="{(28 + x_corp_end) / 2:.1f}" y="43" text-anchor="middle" font-size="13" font-weight="bold" fill="#0f172a">CP CORPORAL (MP 83.0)</text>')

    lines.append(f'  <rect x="{x_corp_end + 8:.1f}" y="24" width="{x_sarg_start - x_corp_end - 16:.1f}" height="28" rx="5" fill="#eff6ff" stroke="#bfdbfe" stroke-width="1"/>')
    lines.append(f'  <text x="{(x_corp_end + x_sarg_start) / 2:.1f}" y="43" text-anchor="middle" font-size="13" font-weight="bold" fill="#1e40af">Beet Loader</text>')

    lines.append(f'  <rect x="{x_sarg_start:.1f}" y="24" width="{svg_w - 28 - x_sarg_start:.1f}" height="28" rx="5" fill="#e2e8f0"/>')
    lines.append(f'  <text x="{(x_sarg_start + svg_w - 28) / 2:.1f}" y="43" text-anchor="middle" font-size="13" font-weight="bold" fill="#0f172a">CP SARGENT (MP 83.5)</text>')

    # Horizontal swimlane lines
    for lane_idx in range(-2, 2):
        ly = lane0_canvas_y + lane_idx * SWIMLANE_MM * scale
        lines.append(f'  <line x1="32" y1="{ly:.1f}" x2="{svg_w - 32}" y2="{ly:.1f}" stroke="#e2e8f0" stroke-width="1"/>')

    # Highlight the lined straight northbound route through Corporal (830NAB -> 829 N -> 827 N -> MT1)
    hl_x1 = (0.0 + ox) * scale
    hl_x2 = (93.98 + ox) * scale
    lines.append(f'  <line x1="{hl_x1:.1f}" y1="{lane0_canvas_y:.1f}" x2="{hl_x2:.1f}" y2="{lane0_canvas_y:.1f}" stroke="#fde68a" stroke-width="14" stroke-linecap="round"/>')

    # Continuous Beet Loader tracks between Corporal Switch 1 (x=124.46) and Sargent Switch 1 (x=143.51)
    for y_mm in (2.54, 19.05):
        x1_px = (124.46 + ox) * scale
        x2_px = (sarg_dx + ox) * scale
        y_px = (y_mm + oy) * scale
        lines.append(f'  <line x1="{x1_px:.1f}" y1="{y_px:.1f}" x2="{x2_px:.1f}" y2="{y_px:.1f}" stroke="#7e22ce" stroke-width="3.6" stroke-linecap="round"/>')
    by_top = (2.54 + oy) * scale
    by_bot = (19.05 + oy) * scale
    bx_mid = ((124.46 + sarg_dx) / 2.0 + ox) * scale
    lines.append(f'  <text x="{bx_mid:.1f}" y="{by_top - 8:.1f}" text-anchor="middle" font-size="11.5" font-weight="bold" fill="#6b21a8">Beet Loader Track 1</text>')
    lines.append(f'  <text x="{bx_mid:.1f}" y="{by_bot - 8:.1f}" text-anchor="middle" font-size="11.5" font-weight="bold" fill="#6b21a8">Beet Loader Track 2</text>')
    lines.append(f'  <text x="{bx_mid:.1f}" y="{(by_top + by_bot) / 2 + 4:.1f}" text-anchor="middle" font-family="monospace" font-size="11" font-weight="bold" fill="#475569">1T1</text>')

    switch_bottom_y = svg_h - 60.0
    sw_items: list[tuple[float, float, str]] = []

    def render_plant(il_name: str, il_obj: dict, dx_mm: float, dy_mm: float) -> None:
        g = il_obj["geometry"]
        apps = il_obj["appliances"]
        p_ox = ox + dx_mm
        p_oy = oy + dy_mm

        for t in g["track"]:
            circ = (t.get("derived") or {}).get("circuit")
            # Skip the tiny vertical stubs on the removed Industry markers at x=124.46 and x=0.0
            pts_list = pts(t["points"])
            if len(pts_list) == 2 and abs(pts_list[0][0] - pts_list[1][0]) < 0.01:
                continue
            col = circuit_regime_color(il_name, circ)
            p_str = " ".join(f"{(x + p_ox) * scale:.1f},{(y + p_oy) * scale:.1f}" for x, y in pts_list)
            lines.append(f'  <polyline points="{p_str}" stroke="{col}" stroke-width="3.8" fill="none" stroke-linecap="round" stroke-linejoin="round"/>')

        for mkr in g["markers"]:
            g_name = mkr["glyph"].split(":")[1]
            if g_name in ("IRJ", "IRJ-Signal", "IRJ-Signals"):
                lines.append("  " + render_glyph_svg(mkr, glyphs[mkr["glyph"]], scale, p_ox, p_oy, stroke_override="#b00000"))

        for k, pl in sorted(g["appliances"].items()):
            app = apps.get(k, {})
            app_kind = app.get("given", {}).get("Appliance")
            if app_kind in ("lamp", "maintainer_call", "contact", "sensor"):
                continue
            if app_kind == "track_circuit":
                if k == "833T":
                    continue
                cx, cy = (pl["at"]["x"] + p_ox) * scale, (pl["at"]["y"] + p_oy) * scale
                lines.append(f'  <text x="{cx:.1f}" y="{cy - 8:.1f}" text-anchor="middle" font-family="monospace" font-size="11" font-weight="bold" fill="#475569">{sp_we_label(k)}</text>')
                continue
            if app_kind == "switch":
                os_circ = (app.get("derived") or {}).get("osCircuit")
                if not os_circ and k == "827D":
                    os_circ = "Corporal:827T1"
                col = circuit_regime_color(il_name, os_circ)
                lines.append("  " + render_glyph_svg(pl, glyphs[pl["glyph"]], scale, p_ox, p_oy, stroke_override="#333333", track_col=col, track_width=3.8))
                if "Derail" in pl["glyph"] or "Switch_Local" in pl["glyph"] or k.endswith("D"):
                    continue
                gl = glyphs[pl["glyph"]]
                ports = {p: page_of(pl, (xy["x"], xy["y"])) for p, xy in gl["ports"].items()}
                sx = ((0.65 * ports["C"][0] + 0.35 * ports["R"][0]) + p_ox) * scale
                sy = (max(p[1] for p in ports.values()) + p_oy) * scale
                sw_items.append((sx, sy, k))
            elif app_kind == "mast":
                gl = glyphs[pl["glyph"]]
                lines.append("  " + render_glyph_svg(pl, gl, scale, p_ox, p_oy, stroke_override="#0f172a"))
                xs, ys = [], []
                for prim in gl["shape"]:
                    if "points" in prim:
                        for p in pts(prim["points"]):
                            px, py = page_of(pl, p)
                            xs.append((px + p_ox) * scale)
                            ys.append((py + p_oy) * scale)
                    elif "center" in prim:
                        px, py = page_of(pl, (prim["center"]["x"], prim["center"]["y"]))
                        r = prim["radius"] * scale
                        xs.extend([(px + p_ox) * scale - r, (px + p_ox) * scale + r])
                        ys.extend([(py + p_oy) * scale - r, (py + p_oy) * scale + r])
                rot = pl.get("rotation", 0)
                ly = ((min(ys) + max(ys)) / 2.0 if ys else (pl["at"]["y"] + p_oy) * scale) + 4.0
                if rot == 180:
                    lx = (min(xs) if xs else (pl["at"]["x"] + p_ox) * scale) - 4.0
                    anchor = "end"
                else:
                    lx = (max(xs) if xs else (pl["at"]["x"] + p_ox) * scale) + 4.0
                    anchor = "start"
                lines.append(f'  <text x="{lx:.1f}" y="{ly:.1f}" text-anchor="{anchor}" font-family="monospace" font-size="11.5" font-weight="bold" fill="#b00000">{sp_we_label(k)}</text>')

    render_plant("Corporal", corp, 0.0, 0.0)
    render_plant("Sargent", sarg, sarg_dx, sarg_dy)

    # Track labels at left (MT1, MT2) and OS circuit labels (827T1, 829T1) clear of all signal masts
    mt1_y = lane0_canvas_y
    mt2_y = lane0_canvas_y + SWIMLANE_MM * scale
    lines.append(f'  <text x="56" y="{mt1_y - 10:.1f}" font-size="12" font-weight="bold" fill="#15803d">← MT1</text>')
    lines.append(f'  <text x="56" y="{mt2_y - 10:.1f}" font-size="12" font-weight="bold" fill="#15803d">MT2 →</text>')
    lines.append(f'  <text x="{(20.0 + ox) * scale:.1f}" y="{mt1_y - 8:.1f}" text-anchor="middle" font-family="monospace" font-size="11" font-weight="bold" fill="#475569">827T1</text>')
    lines.append(f'  <text x="{(36.0 + ox) * scale:.1f}" y="{mt2_y - 8:.1f}" text-anchor="middle" font-family="monospace" font-size="11" font-weight="bold" fill="#475569">829T1</text>')
    lines.append(f'  <text x="{(sarg_dx + 68.0 + ox) * scale:.1f}" y="{mt1_y - 8:.1f}" text-anchor="middle" font-family="monospace" font-size="11" font-weight="bold" fill="#475569">835T1</text>')

    # Switch numbers along the bottom
    sw_items.sort(key=lambda item: item[0])
    for sx, sy, k in sw_items:
        lines.append(f'  <line x1="{sx:.1f}" y1="{sy + 6:.1f}" x2="{sx:.1f}" y2="{switch_bottom_y - 14:.1f}" stroke="#cbd5e1" stroke-width="1.2" stroke-dasharray="3 3"/>')
        lines.append(f'  <text x="{sx:.1f}" y="{switch_bottom_y:.1f}" text-anchor="middle" font-family="monospace" font-size="13" font-weight="bold" fill="#0f172a">{k}</text>')

    leg_y = svg_h - 18
    lines.append(f'  <line x1="36" y1="{leg_y - 4}" x2="66" y2="{leg_y - 4}" stroke="#1d4ed8" stroke-width="4" stroke-linecap="round"/>')
    lines.append(f'  <text x="74" y="{leg_y}" font-size="12" fill="#333333" font-weight="bold">CTC (Single Track &amp; OS)</text>')
    lines.append(f'  <line x1="250" y1="{leg_y - 4}" x2="280" y2="{leg_y - 4}" stroke="#15803d" stroke-width="4" stroke-linecap="round"/>')
    lines.append(f'  <text x="288" y="{leg_y}" font-size="12" fill="#333333" font-weight="bold">Rule 251 Double Track (MT1 / MT2)</text>')
    lines.append(f'  <line x1="530" y1="{leg_y - 4}" x2="560" y2="{leg_y - 4}" stroke="#7e22ce" stroke-width="4" stroke-linecap="round"/>')
    lines.append(f'  <text x="568" y="{leg_y}" font-size="12" fill="#333333" font-weight="bold">Beet Loader Siding &amp; Spurs (01T / 1T1 / 2EAT)</text>')
    lines.append(f'  <text x="{svg_w - 36}" y="{leg_y}" text-anchor="end" font-size="12" fill="#555555">Switch numbers shown along bottom</text>')

    lines.append("</svg>")
    out_file.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main(argv: list[str]) -> int:
    """usage: render_spcoast_svgs.py [project.json] [--out DIR]"""
    args = list(argv)
    out_dir = OUT_DIR
    if "--out" in args:
        i = args.index("--out")
        if i + 1 >= len(args):
            print(main.__doc__, file=sys.stderr)
            return 2
        out_dir = Path(args[i + 1])
        del args[i:i + 2]
    if len(args) > 1 or any(a.startswith("-") for a in args):
        print(main.__doc__, file=sys.stderr)
        return 2
    model = json.loads(Path(args[0] if args else MODEL_PATH).read_text(encoding="utf-8"))
    compress_christopher_geometry(model)
    out_dir.mkdir(parents=True, exist_ok=True)

    render_full_corridor(model, out_dir / "spcoast-full-corridor.svg")
    render_full_corridor(model, out_dir / "01-subdivision-corridor.svg")
    render_corporal_sargent_combined(model, out_dir / "spcoast-corporal-sargent.svg")

    details = [
        ("GCaltrain", "Gilroy Caltrain", "spcoast-01-gcaltrain.svg"),
        ("Gilroy", "Gilroy", "spcoast-02-gilroy.svg"),
        ("Luchessa", "Luchessa", "spcoast-03-luchessa.svg"),
        ("Christopher", "Christopher", "spcoast-04-christopher.svg"),
        ("Corporal", "Corporal", "spcoast-05-corporal.svg"),
        ("Sargent", "Sargent", "spcoast-06-sargent.svg"),
    ]
    for name, title, fname in details:
        render_interlocking_detail(model, name, title, out_dir / fname)
    render_watsonville_detail(out_dir / "spcoast-07-watsonville.svg")

    print(f"Rendered SPCoast corridor and interlocking SVGs with aligned swimlanes into {out_dir}.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
