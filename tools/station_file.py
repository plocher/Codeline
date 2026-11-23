#!/usr/bin/env python3
"""Write one station's flat station file: its chart and its plant, read from the project file.

The C++ host (src/codeline_host.cpp) and the in-process scenarios read this file instead of the
project file. Why a generated flat file rather than JSON in C++:

- The chart must come from the one deriver (tools/codeline_chart.py, Python). A Python step is
  therefore needed anyway; doing the plant extraction beside it keeps one JSON reader.
- The C++ side then needs only "split a line on spaces": no vendored JSON library, nothing with
  its own project to maintain.

The file is a cache of the project file, like `derived`: rebuild it (the Makefile does) whenever
the project file or these tools change. It names the project file and the SHA-256 prefix of its
bytes on its `source` line.

Usage:

    python3 tools/station_file.py [project.json] --station NAME     # writes to stdout

Exits 1, writing nothing, if the station has problems.

Record grammar (one record per line, fields separated by spaces, `#` starts a comment line):

    station <name>
    source <project path> <sha256 first 8 hex>
    chart <chart id>
    control <class> <appliance> <word> <aar token> <group>      in chart order
    indication <class> <appliance> <word> <aar token> <group>   in chart order
    channel <appliances|codeline|simulation> <topic prefix>
    circuit <name> <role> <id>
    switch <name> <power|hand> <role> <id> os=<circuit|-> travel_timeout_ms=<n>
    lock <name> <role> <id> switches=<s1,s2,...> nv=<path:value,...>
    lever <name> time_locking_ms=<n>
    mast <name> <role> <id> nv=mast.heads:<n>
    call <name>
    contact <name> <role> <id>          a field contact shown to the office as a lamp
    lamp <name>                         a chart lamp with no source in the project file: dark
    route <name> mast=<mast> presents=<i1,...> caps=<i1,...|-> levers=<lever:DIR,...>
          align=<switch:POSITION:releasing circuit,...> clears=<c1,...> approach=<circuit|->
          next=<-|fact>                                         (all on one line)

<group> numbers a positive-control group within the station (0: the function is in none). The
groups and the chart order are both from tools/codeline_chart.py; the C++ side restates neither.

Plant rules (what this tool derives, and how):

- Signal levers: the signal functions of the chart. Masts: the CONTROLLED masts of the
  interlocking; each mast belongs to the lever named by its given.signal; its head count
  (given.heads) is stored as the appliance's mast.heads (`nv=` is NV for the simulated core).
- Switches: the switches and derails a route uses. A switch with switch controls in the chart is
  `power` (the engine commands it); any other is `hand` (the engine only reads it).
- Electric locks: a switch with given.Style `lock` is locked by the lever device of Style `lock`
  with the same Name; the lock covers the switch and its derived.derails.
- Routes: one walk of the track graph from each controlled mast's signal joint, through its
  governed port, switch by switch (C -> N/R, N/R -> C), across plain insulated joints, until it
  leaves through another signal joint (a walk that ends anywhere else gives no route). Then:
    * a route whose exit circuit carries a GCOR rule 9.14 policy for the other direction is
      dropped (no signal leads against the current of traffic);
    * levers: the mast's own lever in the mast's facing direction, then the lever of every other
      controlled mast whose signal joint governs a circuit the route crosses, same direction
      (there is no home signal between them, Walkthrough section 1);
    * align: each switch crossed at the position crossed, releasing with its own circuit; a
      crossed switch's derived.derails not otherwise crossed are required NORMAL (derailing);
    * clears: the circuits crossed in order, then the exit circuit when this interlocking has it;
    * approach: the signal joint's approach circuit, when this interlocking has it;
    * presents / caps: the indications every head of the mast presents, and each crossed
      switch's IndicationCeiling for its position (C++ takes the least of the most permissive
      presented indication and the caps);
    * a route whose ceiling (that least) is not an indication its mast presents is no route and
      is dropped: the engine would line and lock it, and the mast could never show it.
- Circuits: the circuits any route or switch uses.

Everything the engine needs that the project file does not state is in STOPGAPS below, and only
there. Each stopgap is repeated as a comment in the file it writes.
"""
from __future__ import annotations

import hashlib
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import codeline_chart  # noqa: E402  (the one chart deriver)

WRITER = "station_file/1"

# ------------------------------------------------------------------------------------ STOPGAPS
# Facts the engine or host needs that tools/spcoast.layout.json does not contain. Each
# entry: value used, and why it is a stopgap. Nothing else in this file or in C++ invents a fact.
STOPGAPS = {
    "channel.appliances": (
        "/layout/SPCoast/appliances",
        "The project file names no Smart Appliance channel; docs/bindings/MQTT.md uses this one."),
    "channel.simulation": (
        "/layout/SPCoast/simulation",
        "The simulated physical world (a train shunting a circuit, a crew throwing a hand switch, "
        "opening a lock) has no place in the project file or the docs; the host and every script "
        "and tool that drives it read it from the station file."),
    "role": (
        {"track_circuit": "sensor", "switch": "turnout", "mast": "signalmast", "lock": "control",
         "contact": "sensor"},
        "The project file gives an appliance kind (given.Appliance), not its Smart Appliance role; "
        "this mapping follows docs/appliances/README.Appliances.md."),
    "id.lock": (
        "lock:{name}",
        "The lock lever device has Name 827; its CONTROL id lock:827 is the example of "
        "docs/bindings/MQTT.md section 5a, not a project-file fact."),
    "lock.nv": (
        "lever.inputs:1,lever.outputs:1,button.inputs:1,button.mode:LEVEL",
        "MQTT.md section 5a's lock:827 profile (SW1 lock open, BTN1 request, LAMP1 release lamp). "
        "The project file's lock device instead binds an input WLQ and two outputs WLK, WLQK, and "
        "no lock contact. The host stores it in the simulated CONTROL core's NV."),
    "time_locking_ms": (
        15000,
        "No time-locking (TER) duration in the project file; 15 s is the layout default of "
        "docs/Codeline.md section 4 (and SignalLever.h). The host may override it (--time-locking-ms)."),
    "travel_timeout_ms": (
        2000,
        "No switch travel limit in the project file; 2 s is the layout default of docs/Codeline.md "
        "section 4 (and Switch.h). How "
        "long a signal demand waits for the switches of its frame (README.CodeLine 5.1 rule 1)."),
    "next": (
        "-",
        "No route says which signal is next beyond its exit, nor gives a way to read it (the next "
        "westward signal is at Christopher, outside this file). Every route reads its next signal "
        "as not proven proceed (the DR term), so it shows no better than APPROACH."),
}


# Permissiveness of the Cluster:MAST indications, least first: the engine's ranking
# (src/codeline/engine/Types.h permissiveness). The host checks every route's ceiling again with
# the engine's own ranking and refuses a station file that disagrees.
PERMISSIVENESS = {"STOP": 0, "RESTRICTING": 1, "DIVERGING_RESTRICTING": 1, "APPROACH": 2,
                  "DIVERGING_APPROACH": 2, "APPROACH_MEDIUM": 3, "ADVANCE_APPROACH": 4,
                  "DIVERGING_CLEAR": 5, "CLEAR": 6}


def route_ceiling(presents: list[str], caps: list[str]) -> str:
    """The least of the most permissive presented indication and the caps, as the host computes it."""
    ceiling = "STOP"
    for p in presents:
        if PERMISSIVENESS[p] >= PERMISSIVENESS[ceiling]:
            ceiling = p
    for c in caps:
        if PERMISSIVENESS[c] < PERMISSIVENESS[ceiling]:
            ceiling = c
    return ceiling


def stopgap(key: str):
    return STOPGAPS[key][0]


def codeline_channel(line: dict) -> str:
    """The code line channel a layout.codelines entry states (given.Topic), without a final '/'."""
    return line.get("given", {}).get("Topic", "").rstrip("/")


def project_codeline_channel(project: Path | str = codeline_chart.DEFAULT_PROJECT) -> str:
    """The one code line channel of a project file, for tools that are not given a station."""
    import json
    lines = json.loads(Path(project).read_text()).get("layout", {}).get("codelines", {})
    found = sorted({codeline_channel(line) for line in lines.values()} - {""})
    if len(found) != 1:
        raise SystemExit(f"station_file: {project} states {len(found)} code line channels; need exactly one")
    return found[0]


# ------------------------------------------------------------------------------------- helpers

class Problem(Exception):
    pass


def _split(qualified: str) -> tuple[str, str]:
    ix, _, name = qualified.partition(":")
    return ix, name


def _local(ix_name: str, qualified: str | None) -> str | None:
    """The bare name of a qualified name of this interlocking, else None."""
    if not qualified:
        return None
    ix, name = _split(qualified)
    return name if ix == ix_name else None


class Station:
    def __init__(self, model: dict, chart: dict):
        self.chart = chart
        self.ix_name = chart["interlocking"]
        self.ix = model["interlockings"][self.ix_name]
        line = model.get("layout", {}).get("codelines", {}).get(chart.get("codeline") or "", {})
        self.codeline_channel = codeline_channel(line)
        self.apps = self.ix.get("appliances", {})
        self.joints = {j["id"]: j for j in self.ix.get("track", {}).get("joints", [])}
        self.policies = self.ix.get("track", {}).get("policies", [])
        self.problems: list[str] = []
        self.dropped: list[str] = []
        self.unused: list[str] = []

    # -- what the interlocking has

    def circuit_names(self) -> set[str]:
        out = {n for n, a in self.apps.items() if a.get("given", {}).get("Appliance") == "track_circuit"}
        for a in self.apps.values():
            if a.get("given", {}).get("Appliance") == "switch":
                c = self.switch_circuit(a)
                if c:
                    out.add(c)
        return out

    def switch_circuit(self, a: dict) -> str | None:
        d = a.get("derived", {})
        return _local(self.ix_name, d.get("osCircuit") or d.get("track"))

    def controlled_masts(self) -> dict[str, dict]:
        return {n: a for n, a in self.apps.items()
                if a.get("given", {}).get("Appliance") == "mast" and a["given"].get("Kind") == "CONTROLLED"
                and _split(a.get("derived", {}).get("controlPoint", ""))[0] == self.ix_name}

    def signal_joint(self, mast: str) -> dict | None:
        found = [j for j in self.joints.values() if j.get("given", {}).get("Kind") == "IRJ_SIGNAL"
                 and _local(self.ix_name, j.get("relations", {}).get("mast")) == mast]
        if len(found) != 1:
            self.problems.append(f"mast {mast}: {len(found)} signal joints name it; need exactly one")
            return None
        return found[0]

    # -- the track graph

    def _targets(self, refs: list[str], came_from: str | None = None) -> list[tuple]:
        """Followable targets of a port's references: ('port', appliance, port) of this
        interlocking's switches, or ('joint', id, side). Anything else is not followed."""
        out = []
        for r in refs:
            if r == came_from:
                continue
            ix, rest = _split(r) if ":" in r else (None, r)
            if ix == self.ix_name:
                name, _, port = rest.rpartition(".")
                if self.apps.get(name, {}).get("given", {}).get("Appliance") == "switch":
                    out.append(("port", name, port))
            elif ix is None:
                jid, _, side = r.rpartition(".")
                if jid in self.joints:
                    out.append(("joint", jid, side))
        return out

    def _walk(self, sw: str, port_in: str, path: list, results: list, depth: int = 0) -> None:
        if depth > 20:
            self.problems.append(f"route walk through {sw} does not end")
            return
        rel = self.apps[sw].get("relations", {})
        if port_in == "C":
            exits = [(p, w) for p, w in (("N", "NORMAL"), ("R", "REVERSE")) if p in rel]
        else:
            exits = [("C", "NORMAL" if port_in == "N" else "REVERSE")]
        for port_out, word in exits:
            self._follow(sw, port_out, path + [(sw, word)], results, depth)

    def _follow(self, sw: str, port: str, path: list, results: list, depth: int) -> None:
        refs = self.apps[sw].get("relations", {}).get(port, [])
        targets = self._targets(refs)
        if len(targets) != 1:
            results.append((path, None, None))      # the track ends here (or is not followable)
            if len(targets) > 1:
                self.problems.append(f"{sw}.{port}: {len(targets)} followable connections; need at most one")
            return
        kind, a, b = targets[0]
        if kind == "port":
            self._walk(a, b, path, results, depth + 1)
            return
        self._cross(a, b, path, results, depth)

    def _cross(self, jid: str, side: str, path: list, results: list, depth: int) -> None:
        """Arrive at joint jid on its given side. A plain insulated joint is crossed: to the switch
        port beyond it, or, where a track circuit with no switch lies beyond it (between two
        switches), through that circuit to its far joint. Any other joint is the route's exit."""
        joint = self.joints[jid]
        if joint.get("given", {}).get("Kind") != "IRJ":
            results.append((path, joint, side))     # a signal joint (or other): the route's exit
            return
        if depth > 20:
            self.problems.append(f"route walk through joint {jid} does not end")
            return
        other = [s for s in joint.get("relations", {}) if s != side and s in ("A", "B")]
        nxt = self._targets(joint["relations"].get(other[0], [])) if other else []
        ports = [t for t in nxt if t[0] == "port"]
        if len(ports) == 1:
            self._walk(ports[0][1], ports[0][2], path, results, depth + 1)
            return
        far = [t for t in nxt if t[0] == "joint"]
        circuit = _local(self.ix_name, joint.get("derived", {}).get("circuits", {}).get(other[0])) if other else None
        if ports or len(far) != 1 or not circuit:
            self.problems.append(f"joint {jid}: no single switch port or intermediate circuit beyond side {side}")
            return
        # An entry with no word is a track circuit the route runs through between two switches.
        self._cross(far[0][1], far[0][2], path + [(circuit, "")], results, depth + 1)

    # -- routes

    def exit_circuit(self, joint: dict | None, side: str | None) -> str | None:
        if joint is None:
            return None
        circuits = joint.get("derived", {}).get("circuits", {})
        if joint.get("given", {}).get("Kind") == "IRJ_SIGNAL":
            other = "approach" if side == "governed" else "governed"
        else:
            other = "B" if side == "A" else "A"
        return circuits.get(other)

    def against_traffic(self, circuit: str | None, facing: str) -> bool:
        for p in self.policies:
            if p.get("given", {}).get("Kind") == "GCOR_RULE_9_14" and \
                    p.get("derived", {}).get("circuits", {}).get("track") == circuit:
                if p["given"].get("Direction") not in (None, facing):
                    return True
        return False

    def governing_levers(self, circuit: str, masts: dict[str, dict]) -> list[str]:
        out = []
        for name, m in masts.items():
            j = self.signal_joint(name)
            if j and _local(self.ix_name, j.get("derived", {}).get("circuits", {}).get("governed")) == circuit:
                out.append(m["given"]["signal"])
        return out

    def routes(self, masts: dict[str, dict], circuits: set[str]) -> list[dict]:
        out = []
        for mast_name in sorted(masts, key=codeline_chart._natural):
            mast = masts[mast_name]
            facing = mast.get("relations", {}).get("facing")
            lever = mast["given"].get("signal")
            joint = self.signal_joint(mast_name)
            if joint is None or facing not in ("LEFT", "RIGHT"):
                continue
            results: list = []
            for t in self._targets(joint.get("relations", {}).get("governed", [])):
                if t[0] == "port":
                    self._walk(t[1], t[2], [], results)
            heads = [p.get("presents", []) for p in mast.get("derived", {}).get("positions", {}).values()]
            presents = sorted(set.intersection(*map(set, heads)) - {"DARK"}) if heads else []
            for path, exit_joint, side in results:
                exit_c = self.exit_circuit(exit_joint, side)
                desc = "-".join([mast_name] + [f"{s}{w[0]}" for s, w in path if w])
                if exit_joint is None:
                    self.dropped.append(f"{desc} because the track ends without a signal joint")
                    continue
                if self.against_traffic(exit_c, facing):
                    self.dropped.append(f"{desc} because it exits into {exit_c}, GCOR rule 9.14 for the "
                                        f"other direction")
                    continue
                align, crossed, caps = [], [], []
                for sw, word in path:
                    if not word:                    # an intermediate track circuit
                        if sw not in crossed:
                            crossed.append(sw)
                        continue
                    a = self.apps[sw]
                    c = self.switch_circuit(a)
                    align.append((sw, word, c))
                    if c and c not in crossed:
                        crossed.append(c)
                    ceiling = a.get("given", {}).get("IndicationCeiling", "")
                    if ceiling:
                        n, _, r = ceiling.partition("/")
                        caps.append(n if word == "NORMAL" else r)
                for sw, word in path:
                    if not word:
                        continue
                    for d in self.apps[sw].get("derived", {}).get("derails", []):
                        dn = _local(self.ix_name, d)
                        if dn and dn not in {s for s, _, _ in align}:
                            align.append((dn, "NORMAL", self.switch_circuit(self.apps[dn])))
                levers = [(lever, facing)]
                for c in crossed:
                    for other in self.governing_levers(c, masts):
                        if (other, facing) not in levers:
                            levers.append((other, facing))
                clears = list(crossed)
                exit_local = _local(self.ix_name, exit_c)
                if exit_local in circuits and exit_local not in clears:
                    clears.append(exit_local)
                ceiling = route_ceiling(presents, caps)
                if ceiling not in presents:
                    self.dropped.append(f"{desc} because its ceiling {ceiling} (caps {','.join(caps)}) is no "
                                        f"indication mast {mast_name} presents ({','.join(presents) or 'none'})")
                    continue
                approach = _local(self.ix_name, joint.get("derived", {}).get("circuits", {}).get("approach"))
                out.append({
                    "name": desc, "mast": mast_name, "presents": presents, "caps": caps,
                    "levers": levers, "align": align, "clears": clears,
                    "approach": approach if approach in circuits else None,
                })
        return out

    # -- the station file

    def lines(self, source: str, digest: str) -> list[str]:
        ch = self.chart
        roles = stopgap("role")
        out = [f"# Generated by tools/station_file.py ({WRITER}) from {source}. Do not edit.",
               f"# Chart from tools/codeline_chart.py ({codeline_chart.DERIVER}).",
               "#"]
        for key, (value, why) in STOPGAPS.items():
            out.append(f"# STOPGAP {key} = {value}: {why}")
        out += ["#", f"station {ch['station']}", f"source {source} {digest}", f"chart {ch['chartId']}"]

        group_ids: dict[tuple, int] = {}
        for direction, bucket in (("control", "controls"), ("indication", "indications")):
            for f in ch[bucket]:
                gid = 0
                for gi, group in enumerate(codeline_chart.GROUPS.get((direction, f["class"]), [])):
                    if f["word"] in group:
                        key = (direction, f["class"], f["appliance"], gi)
                        gid = group_ids.setdefault(key, len(group_ids) + 1)
                out.append(f"{direction} {f['class']} {f['appliance']} {f['word']} {f['aar']} {gid}")

        out.append(f"channel appliances {stopgap('channel.appliances')}")
        if not self.codeline_channel:
            self.problems.append(f"code line {ch.get('codeline')!r}: no given.Topic for the code line channel")
        out.append(f"channel codeline {self.codeline_channel}")
        out.append(f"channel simulation {stopgap('channel.simulation')}")

        controls = ch["controls"]
        levers = sorted({f["appliance"] for f in controls if f["class"] == "signal"}, key=codeline_chart._natural)
        commanded = {f["appliance"] for f in controls if f["class"] == "switch"}
        calls = [f["appliance"] for f in controls if f["class"] == "maintainer_call"]

        all_masts = self.controlled_masts()
        masts = {n: m for n, m in all_masts.items() if m["given"].get("signal") in levers}
        for n, m in all_masts.items():
            if n not in masts:
                self.problems.append(f"mast {n}: its signal {m['given'].get('signal')} has no lever in the chart")
        circuits = self.circuit_names()
        routes = self.routes(masts, circuits)

        switches: list[str] = []
        for r in routes:
            for sw, _, _ in r["align"]:
                if sw not in switches:
                    switches.append(sw)
        for sw in commanded:
            if sw not in switches:
                switches.append(sw)
        used = set()
        for r in routes:
            used.update(r["clears"])
            if r["approach"]:
                used.add(r["approach"])
        for sw in switches:
            c = self.switch_circuit(self.apps[sw])
            if c:
                used.add(c)

        # A circuit the chart indicates is in the plant even where no route runs over it.
        used.update(f["appliance"] for f in ch["indications"]
                    if f["class"] == "track_circuit" and f["appliance"] in circuits)
        for c in sorted(used, key=codeline_chart._natural):
            out.append(f"circuit {c} {roles['track_circuit']} {c}")
        for sw in switches:
            kind = "power" if sw in commanded else "hand"
            os = self.switch_circuit(self.apps[sw]) or "-"
            out.append(f"switch {sw} {kind} {roles['switch']} {sw} os={os} "
                       f"travel_timeout_ms={stopgap('travel_timeout_ms')}")
        lock_fns = sorted({f["appliance"] for f in controls if f["class"] == "lock"})
        for name in lock_fns:
            a = self.apps.get(name, {})
            if a.get("given", {}).get("Style") != "lock":
                self.problems.append(f"lock {name}: no switch {name} of Style lock in the interlocking")
                continue
            devices = [d for d in self.ix.get("devices", []) if d.get("given", {}).get("Appliance") == "lever"
                       and d["given"].get("Style") == "lock" and d["given"].get("Name") == name]
            if len(devices) != 1:
                self.problems.append(f"lock {name}: {len(devices)} lock lever devices; need exactly one")
                continue
            covered = [name] + [_local(self.ix_name, d) for d in a.get("derived", {}).get("derails", [])]
            for sw in covered:
                if sw not in switches:
                    self.problems.append(f"lock {name}: covers {sw}, which no route uses")
            out.append(f"lock {name} {roles['lock']} {stopgap('id.lock').format(name=name)} switches={','.join(covered)} "
                       f"nv={stopgap('lock.nv')}")
        for lever in levers:
            out.append(f"lever {lever} time_locking_ms={stopgap('time_locking_ms')}")
        for n in sorted(masts, key=codeline_chart._natural):
            heads = [h for h in masts[n]["given"].get("heads", "").split(",") if h]
            out.append(f"mast {n} {roles['mast']} {n} nv=mast.heads:{len(heads)}")
        for c in calls:
            out.append(f"call {c}")
        lamp_fns = [f["appliance"] for f in ch["indications"] if f["class"] == "lamp"]
        for n in lamp_fns:
            kind = self.apps.get(n, {}).get("given", {}).get("Appliance")
            if kind == "contact":
                out.append(f"contact {n} {roles['contact']} {n}")
            else:
                out.append(f"# STOPGAP lamp {n}: the project file names no source for this lamp; it reads dark")
                out.append(f"lamp {n}")
        for r in routes:
            out.append(
                f"route {r['name']} mast={r['mast']} presents={','.join(r['presents']) or '-'} "
                f"caps={','.join(r['caps']) or '-'} "
                f"levers={','.join(f'{l}:{d}' for l, d in r['levers'])} "
                f"align={','.join(f'{s}:{w}:{c or chr(45)}' for s, w, c in r['align'])} "
                f"clears={','.join(r['clears'])} approach={r['approach'] or '-'} next={stopgap('next')}")
        for d in self.dropped:
            out.append(f"# no route {d}")
        listed = set(switches) | used | set(masts) | set(lock_fns) | set(calls) | set(lamp_fns)
        for n, a in sorted(self.apps.items()):
            if n not in listed:
                out.append(f"# not in the plant: {a.get('given', {}).get('Appliance')} {n}")
        return out


def build(project: Path, station: str) -> tuple[list[str], list[str]]:
    raw = project.read_bytes()
    model = codeline_chart.load(project)
    result = codeline_chart.derive(model)
    charts = [s for s in result["stations"] if s["station"] == station]
    if len(charts) != 1:
        return [], [f"no station named {station!r}"]
    st = Station(model, charts[0])
    problems = [f"{p['where']}: {p['text']}" for p in result["problems"]
                if p["where"].startswith("interlockings/" + station)]
    try:
        source = str(project.resolve().relative_to(Path.cwd()))
    except ValueError:
        source = str(project)
    lines = st.lines(source, hashlib.sha256(raw).hexdigest()[:8])
    return lines, problems + st.problems


USAGE = "usage: station_file.py [project.json] --station NAME"


def main(argv: list[str]) -> int:
    args = list(argv)
    if "--station" not in args or args.index("--station") + 1 >= len(args):
        print(USAGE, file=sys.stderr)
        return 2
    i = args.index("--station")
    station = args[i + 1]
    del args[i:i + 2]
    project = Path(args[0]) if args else codeline_chart.DEFAULT_PROJECT
    lines, problems = build(project, station)
    if problems:
        for p in problems:
            print(f"station_file: {p}", file=sys.stderr)
        return 1
    sys.stdout.write("".join(line + "\n" for line in lines))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
