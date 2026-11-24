// The OFFICE: the CTC machine, the model board and the code line traffic list.
//
// An office on the Code Line Interface and nothing more (docs/codeline/README.CodeLine.md):
// - it speaks only on the code line channel, on its own MQTT connection;
// - a lever holds a position; CODE sends one complete control frame built from the lever
//   positions (the panel file's frame.rule; tools/panel_file.py lever_frame is the same rule);
// - every lamp comes from the station's latest valid indication frame, never from a lever;
// - it holds no plant and computes no logic. Every name, word and topic is the panel file's.

import { drawBoard, setLook, setTitle } from "./draw.js";
import { trackGraph, tracePaths } from "./trace.js";

const BOARD_UNITS_PER_COLUMN = 100;   // the board is drawn in units of a lever column's width
const COLUMN_MAX_PX = 104;            // a lever column on screen (2 inches on a real machine)
// The rows of a lever column, top to bottom, the same in every column of the machine.
const ROWS = ["call", "switch", "signal", "code", "call-lamp"];
const ROW_OF = { maintainer_call: "call", switch: "switch", lock: "switch", signal: "signal" };

// The swimlanes of a trackplan: the levels at which track runs level for some length. A plan
// may be drawn with lanes further apart than its neighbours' (a crossover drawn with full-size
// switch symbols puts an empty lane between two main tracks). On the board every lane sits one
// `pitch` under the one above, so two main tracks stay adjacent and both meet the next station;
// a crossover simply slopes less. Returns y -> y for one drawing.
const LEVEL_RUN_MM = 3;
export function laneLevels(drawing) {
  const levels = new Set();
  for (const e of drawing.elements) {
    if (e.shape !== "polyline" || e.role !== "rail") continue;
    for (let i = 1; i < e.points.length; i++) {
      const [a, b] = [e.points[i - 1], e.points[i]];
      if (Math.abs(a[1] - b[1]) < 0.01 && Math.abs(a[0] - b[0]) >= LEVEL_RUN_MM) levels.add(Math.round(a[1] * 100) / 100);
    }
  }
  return [...levels].sort((a, b) => a - b);
}

export function lanePitch(panels) {
  let pitch = Infinity;
  for (const panel of panels) {
    const levels = laneLevels(panel.drawing);
    for (let i = 1; i < levels.length; i++) pitch = Math.min(pitch, levels[i] - levels[i - 1]);
  }
  return pitch;
}

export function laneMap(drawing, pitch) {
  const levels = laneLevels(drawing);
  if (levels.length < 2 || !Number.isFinite(pitch)) return (y) => y;
  const placed = levels.map((level, i) => levels[0] + i * pitch);
  return (y) => {
    if (y <= levels[0]) return y;
    const last = levels.length - 1;
    if (y >= levels[last]) return placed[last] + (y - levels[last]);
    let i = 1;
    while (levels[i] < y) i++;
    return placed[i - 1] + ((y - levels[i - 1]) / (levels[i] - levels[i - 1])) * pitch;
  };
}

// How far (mm) each station's trackplan is moved down so that track running out of one
// station's drawing meets the same track in the next station's drawing at one level. A drawing
// shows the neighbour's circuit it runs into, tagged "<neighbour>:<circuit>".
export function boardShifts(panels, maps) {
  const rails = (panel, name) => panel.drawing.elements.filter((e) =>
    e.shape === "polyline" && e.tag && e.tag.kind === "track_circuit" && e.tag.name === name);
  const edge = (elements, pick) => elements.flatMap((e) => e.points).reduce((best, p) => (best === null || pick(p, best) ? p : best), null);
  const shifts = [];
  panels.forEach((panel, k) => {
    let shift = k ? shifts[k - 1] : 0;
    if (k) {
      const before = panels[k - 1];
      const prefix = before.interlocking + ":";
      const names = [...new Set(panel.drawing.elements.map((e) => e.tag && e.tag.kind === "track_circuit" ? e.tag.name : "")
        .filter((n) => n.startsWith(prefix)))].sort();
      for (const name of names) {
        const here = edge(rails(panel, name), (p, best) => p[0] < best[0]);
        const there = edge(rails(before, name.slice(prefix.length)), (p, best) => p[0] > best[0]);
        if (here && there) { shift = shifts[k - 1] + maps[k - 1](there[1]) - maps[k](here[1]); break; }
      }
    }
    shifts.push(shift);
  });
  return shifts;
}
const TRAFFIC_ROWS = 300;
const SEPARATORS = /[ \t\r\n]+/;      // the term separators of CodeLineMQTT.md section 2

function el(name, cls, text) {
  const n = document.createElement(name);
  if (cls) n.className = cls;
  if (text !== undefined) n.textContent = text;
  return n;
}

function clock() {
  return new Date().toLocaleTimeString([], { hour12: false }) + "." + String(Date.now() % 1000).padStart(3, "0");
}

// ---- frames (spelled only from the panel file's `frame`, `controls` and `indications`)

// positions: Map lever -> index of the position it stands in.
// Returns {text} or, when the levers would assert two words of one positive-control group (a
// frame the field must reject as a whole), {refused: why} and no frame.
export function controlFrame(panel, positions) {
  const asserted = new Set();
  for (const column of panel.columns) {
    for (const lever of column.levers) {
      const position = lever.positions[positions.get(lever)];
      for (const i of position.asserts) asserted.add(i);
    }
  }
  const groups = new Map();
  for (const i of asserted) {
    const c = panel.controls[i];
    if (c.group > 0) {
      if (groups.has(c.group)) return { refused: `${groups.get(c.group)} and ${c.aar} would both be asserted` };
      groups.set(c.group, c.aar);
    }
  }
  const f = panel.frame;
  const terms = panel.controls.map((c, i) => (asserted.has(i) ? c.aar : f.dropped[0] + c.aar + f.dropped[1]));
  return { text: f.open.concat(terms).join(f.separator) };
}

// Where each lever stands when the office first learns the state of the field: the position
// whose answering indication is asserted, else its `home`. Returns Map lever -> position index.
// Used once per station, on its first current indication frame; after that only a person
// moves a lever.
export function startingPositions(panel, asserted) {
  const out = new Map();
  for (const column of panel.columns) {
    for (const lever of column.levers) {
      let at = lever.positions.findIndex((p) => p.expects !== null && asserted[p.expects]);
      if (at < 0) at = lever.positions.findIndex((p) => p.label === lever.home);
      out.set(lever, at < 0 ? 0 : at);
    }
  }
  return out;
}

// {kind: "valid", asserted: [bool]} | {kind: "unavailable" | "out of sync" | "malformed", detail}
export function readIndications(panel, text) {
  const f = panel.frame;
  const parts = text.split(SEPARATORS).filter(Boolean);
  if (parts.length === 1 && parts[0] === f.unavailable) return { kind: "unavailable", detail: "the field is gone" };
  // A frame opens with the words of `open`, the last of them a chart identity. Text that does
  // not open so is malformed; a well-formed identity that is not ours is out of sync.
  const last = f.open.length - 1;
  for (let i = 0; i < last; i++) {
    if (parts[i] !== f.open[i]) return { kind: "malformed", detail: "not a frame" };
  }
  if (parts.length <= last || !new RegExp(f.identity).test(parts[last])) {
    return { kind: "malformed", detail: "no chart identity" };
  }
  if (parts[last] !== f.open[last]) {
    return { kind: "out of sync", detail: `field chart ${parts[last]}, office chart ${f.open[last]}` };
  }
  const terms = parts.slice(f.open.length);
  if (terms.length !== panel.indications.length) {
    return { kind: "malformed", detail: `${terms.length} terms, the chart has ${panel.indications.length}` };
  }
  const asserted = [];
  const groups = new Map();
  for (let i = 0; i < terms.length; i++) {
    const fn = panel.indications[i];
    if (terms[i] === fn.aar) asserted.push(true);
    else if (terms[i] === f.dropped[0] + fn.aar + f.dropped[1]) asserted.push(false);
    else return { kind: "malformed", detail: `term ${terms[i]} where the chart has ${fn.aar}` };
    if (asserted[i] && fn.group > 0) {
      if (groups.has(fn.group)) return { kind: "malformed", detail: `${groups.get(fn.group)} and ${fn.aar} both asserted` };
      groups.set(fn.group, fn.aar);
    }
  }
  return { kind: "valid", asserted };
}

// ---- the office

export class Office {
  // panels: the panel files in panel-column order. client: the code line MqttClient.
  constructor(panels, client, root) {
    this.client = client;
    this.root = root;
    this.stations = new Map();
    for (const panel of panels) {
      this.stations.set(panel.station, {
        panel, positions: new Map(), started: false, reading: null, lamps: [], levers: [], status: null,
        previous: { control: null, indication: null }, awaiting: null, note: "",
      });
    }
    this.board = [];
    this.buildMachine();
    for (const st of this.stations.values()) {
      client.subscribe(st.panel.topics.indications, 1, (topic, payload, retained) => this.onIndications(st, payload, retained));
    }
    // One frame to every station that is current; a station that is not has nothing to answer.
    root.codeAll.addEventListener("click", () => {
      for (const st of this.stations.values()) if (this.current(st)) this.code(st);
    });
    this.refresh();
  }

  // -- the CTC machine: the model board across the top as one drawing, and under each station's
  //    piece of it that station's lever columns. Every column has the same rows (ROWS); CODE
  //    stands in the station's rightmost column.

  buildMachine() {
    const stations = [...this.stations.values()];
    const total = stations.reduce((n, st) => n + st.panel.columns.length, 0);
    const frame = el("div", "machine-frame");
    frame.style.maxWidth = `${total * COLUMN_MAX_PX}px`;
    frame.appendChild(this.buildBoard(stations));
    const grid = el("div", "columns");
    grid.style.gridTemplateColumns = `repeat(${total}, minmax(0, 1fr))`;
    const foot = el("div", "columns foot");
    foot.style.gridTemplateColumns = grid.style.gridTemplateColumns;
    for (const st of stations) {
      st.columns = [];
      st.panel.columns.forEach((column, n) => {
        const last = n === st.panel.columns.length - 1;
        const col = el("div", "column" + (n === 0 ? " first" : "") + (last ? " last" : ""));
        col.appendChild(el("div", "column-plate", column.label));
        const rows = {};
        for (const name of ROWS) rows[name] = col.appendChild(el("div", `row row-${name}`));
        for (const lever of column.levers) {
          const built = this.buildLever(st, lever);
          const row = ROW_OF[lever.style] || "switch";
          if (row === "call") {                    // the call's lever at the top, its lamp at the foot
            rows.call.appendChild(built.box);
            rows["call-lamp"].appendChild(built.lamps);
          } else {
            built.box.insertBefore(built.lamps, built.box.firstChild);
            rows[row].appendChild(built.box);
          }
        }
        if (last) {
          const code = el("button", "code", "CODE");
          code.title = "Send one complete control frame built from this station's lever positions";
          code.addEventListener("click", () => this.code(st));
          st.code = code;
          rows.code.appendChild(code);
        }
        st.columns.push(col);
        grid.appendChild(col);
      });
      const block = el("div", "station");
      block.style.gridColumn = `span ${st.panel.columns.length}`;
      st.status = el("span", "station-status");
      block.appendChild(st.status);
      // Indications with no lever lamp and nothing drawn for them: loose jewels, so none is invisible.
      st.loose = [];
      for (const i of st.panel.unplaced.indications) {
        const lamp = st.panel.board.lamps.find((l) => l.indications.includes(i));
        const jewel = el("span", `jewel lamp-${lamp ? lamp.color : "white"}`);
        const cell = el("span", "lamp");
        cell.append(jewel, el("span", "lamp-label", st.panel.indications[i].aar));
        block.appendChild(cell);
        st.loose.push({ jewel, i });
      }
      st.block = block;
      foot.appendChild(block);
    }
    frame.append(grid, foot);
    this.root.machine.appendChild(frame);
  }

  buildLever(st, lever) {
    const box = el("div", `lever style-${lever.style}`);
    const lamps = el("div", "lamps");
    const cells = [];
    for (const lamp of lever.lamps) {
      const jewel = el("span", `jewel lamp-${lamp.color}`);
      const cell = el("span", "lamp");
      cell.append(jewel, el("span", "lamp-label", lamp.label));
      cell.title = st.panel.indications[lamp.indication].aar + (lamp.inverted ? " (lit when dropped)" : "") +
        (lamp.litUnless ? ` flashes it; steady while none of ${lamp.litUnless.map((i) => st.panel.indications[i].aar).join(", ")} is lit` : "");
      cells.push(cell);
      st.lamps.push({ jewel, lamp });
    }
    // As on a US&S machine: the lever's number plate sits between its left and right lamps, and
    // a middle lamp (a signal's STOP) sits above the plate. A call lever has no plate: it stands
    // in the column of the station it calls.
    const plate = lever.style === "maintainer_call" ? null : el("div", "lever-plate", lever.name);
    if (plate && cells.length >= 2) {
      const middle = el("div", "lamps-middle");
      middle.append(...cells.slice(1, -1), plate);
      lamps.append(cells[0], middle, cells[cells.length - 1]);
    } else {
      lamps.append(...cells);
      if (plate) lamps.appendChild(plate);
    }
    const handle = el("div", "positions");
    st.positions.set(lever, 0);                   // not shown, not sent: replaced by the first current frame
    const buttons = lever.positions.map((position, i) => {
      const b = el("button", "position", position.label);
      b.title = position.asserts.map((c) => st.panel.controls[c].aar).join(" ") || "asserts nothing";
      b.addEventListener("click", () => {
        if (!st.started) return;                  // no lever stands anywhere until the field has spoken
        st.positions.set(lever, i);
        this.refresh();
      });
      handle.appendChild(b);
      return b;
    });
    box.appendChild(handle);
    st.levers.push({ lever, box, buttons });
    return { box, lamps };
  }

  // -- the model board: one drawing across the machine, every station's track in the width of
  //    its own columns, lit from indications only

  buildBoard(stations) {
    const panels = stations.map((st) => st.panel);
    const pitch = lanePitch(panels);
    const maps = panels.map((panel) => laneMap(panel.drawing, pitch));
    const shifts = boardShifts(panels, maps);
    const drawn = drawBoard(stations.map((st, k) => ({
      drawing: st.panel.drawing, mapY: maps[k], columns: st.panel.columns.length, shift: shifts[k], label: st.panel.station,
    })), BOARD_UNITS_PER_COLUMN);
    this.links = drawn.links.map((l) => ({ node: l.node, a: [stations[l.from.part], l.from.element], b: [stations[l.to.part], l.to.element] }));
    drawn.parts.forEach((part, k) => {
      const st = stations[k];
      st.boardPart = part;
      // For the lined-path picture: the track graph, every rail by its element, and where each
      // switch's position is indicated. A power switch has NORMAL / REVERSE indications; a hand
      // switch under an electric lock is drawn normal while locked and reverse while released.
      st.graph = trackGraph(st.panel.drawing);
      st.rails = new Map(part.items.filter((item) => item.element.role === "rail").map((item) => [item.element, item]));
      st.switches = new Map();
      for (const element of st.panel.drawing.elements) {
        const tag = element.tag, office = element.office;
        if (!tag || tag.kind !== "switch" || !office) continue;
        if (!st.switches.has(tag.name)) st.switches.set(tag.name, {});
        const at = st.switches.get(tag.name);
        if (office.lined && (tag.part === "N" || tag.part === "R")) at[tag.part] = office.lined;
        if (office.lit && tag.part === "lamp") at.released = office.lit;
      }
      this.board.push({ st, items: part.items.filter((item) => item.element.office) });
    });
    const holder = el("div", "board-holder");
    holder.appendChild(drawn.svg);
    return holder;
  }

  // Bring a station's columns into view (only matters when the machine is wider than the window).
  show(station) {
    const st = this.stations.get(station);
    if (st && st.columns[0].scrollIntoView) st.columns[0].scrollIntoView({ block: "nearest", inline: "center" });
  }

  // Is the indication a drawn element is bound to asserted? undefined when its station is not current.
  shown(ref) {
    const st = this.stations.get(ref.station);
    if (!st || !st.reading || st.reading.kind !== "valid") return undefined;
    return st.reading.asserted[ref.i];
  }

  // -- CODE

  current(st) {
    return Boolean(st.reading && st.reading.kind === "valid");
  }

  code(st) {
    if (!this.current(st)) return;                // not current: its levers may have no position yet, and nothing answers
    clearTimeout(st.awaiting);
    const frame = controlFrame(st.panel, st.positions);
    if (frame.refused) {
      st.note = `not sent: ${frame.refused}`;
      this.refresh();
      return;
    }
    if (!this.client.publish(st.panel.topics.controls, frame.text)) {
      st.note = "not sent: no connection (a control is never queued)";
      this.refresh();
      return;
    }
    this.traffic(st, "control", frame.text, false);
    st.note = "sent; waiting for the field";
    // Any indication frame of this station ends the wait (the field answers every control frame).
    st.awaiting = setTimeout(() => { st.note = "no response: press CODE again"; this.refresh(); },
                             st.panel.office.answerTimeoutMs);
    this.refresh();
  }

  onIndications(st, payload, retained) {
    clearTimeout(st.awaiting);
    st.note = "";
    st.reading = readIndications(st.panel, payload);
    if (!st.started && st.reading.kind === "valid") {
      // The first current frame: the levers are found where the field is. Never again after this.
      st.positions = startingPositions(st.panel, st.reading.asserted);
      st.started = true;
    }
    this.traffic(st, "indication", payload, retained);
    this.refresh();
  }

  // The code line connection came or went. Without it nothing shown is known to be current.
  // Either way a note about the old connection ("not sent: no connection") no longer holds.
  linkChanged(connected) {
    for (const st of this.stations.values()) {
      clearTimeout(st.awaiting);
      st.note = "";
      if (!connected) st.reading = null;
    }
    this.refresh();
  }

  // -- drawing the state

  refresh() {
    for (const st of this.stations.values()) {
      const current = this.current(st);
      st.code.disabled = !current;              // as CODE all: a station that is not current has nothing to answer
      for (const node of [st.block, ...st.columns]) node.classList.toggle("pending", !st.started);
      for (const { jewel, lamp } of st.lamps) {
        const own = Boolean(current && st.reading.asserted[lamp.indication] !== lamp.inverted);
        // A signal lever's STOP lamp: steady while no signal of the lever is indicated clear,
        // flashing while its own indication (time locking) is asserted.
        const rest = Boolean(current && lamp.litUnless && !lamp.litUnless.some((i) => st.reading.asserted[i]));
        jewel.classList.toggle("lit", own || rest);
        jewel.classList.toggle("flash", own && Boolean(lamp.litUnless));
      }
      for (const { jewel, i } of st.loose) jewel.classList.toggle("lit", Boolean(current && st.reading.asserted[i]));
      for (const { lever, box, buttons } of st.levers) {
        const at = st.positions.get(lever);
        buttons.forEach((b, i) => b.classList.toggle("on", st.started && i === at));
        // Out of correspondence: a position's answering indication is lit and the lever is not
        // there, or the lever is there and it is not lit. Shown, never corrected.
        const out = current && st.started && lever.positions.some((p, i) => p.expects !== null && st.reading.asserted[p.expects] !== (i === at));
        box.classList.toggle("out", Boolean(out));
        box.title = out ? "Out of correspondence with its lamps" : "";
      }
      for (const node of [st.block, ...st.columns, st.boardPart.group]) node.classList.toggle("dark", !current);
      st.boardPart.label.textContent = current ? st.panel.station : `${st.panel.station} (not current)`;
      let text;
      if (!st.reading) text = st.started ? "no indications" : "no indications yet: levers wait for the field";
      else if (current) text = "current";
      else text = `${st.reading.kind.toUpperCase()}: ${st.reading.detail}`;
      st.status.textContent = st.note ? `${text} · ${st.note}` : text;
      st.status.classList.toggle("bad", !current);
    }
    for (const st of this.stations.values()) {
      // How the switches stand, as paths from each entry (trace.js). Not a route: no signal is read.
      const traced = this.current(st) ? tracePaths(st.graph, (name) => this.stands(st, name)) : new Set();
      st.traced = new Set([...traced].map((index) => st.panel.drawing.elements[index]));
      for (const [element, item] of st.rails) if (!element.office) setLook(item, st.traced.has(element) ? ["traced"] : []);
    }
    // The piece of a track between two stations' drawings is on a path when both its ends are.
    for (const { node, a, b } of this.links) node.classList.toggle("traced", a[0].traced.has(a[1]) && b[0].traced.has(b[1]));
    for (const { st, items } of this.board) {
      for (const item of items) {
        const o = item.element.office;
        const looks = st.traced.has(item.element) ? ["traced"] : [];
        const tips = [];
        if (o.occupied) {
          const on = this.shown(o.occupied);
          if (on) looks.push("occupied", `lamp-${o.occupied.color || "red"}`);
          if (on === undefined) looks.push("unknown");
          tips.push(this.term(o.occupied));
        }
        if (o.lined) {
          const on = this.shown(o.lined);
          looks.push(on === undefined ? "unknown" : on ? "lined" : "unlined");
          tips.push(this.term(o.lined));
        }
        if (o.clear) {                                  // a mast: stop or not stop is all the office knows
          const on = this.shown(o.clear);
          looks.push(on === undefined ? "unknown" : on ? "clear" : "stop");
          tips.push(on === undefined ? this.term(o.clear) : on ? `${this.term(o.clear)}: not at stop (aspect not known here)` : `(${this.term(o.clear)}): at stop`);
        }
        if (o.lit) {
          const on = this.shown(o.lit);
          if (on) looks.push("lit", `lamp-${o.lit.color || "white"}`);
          if (on === undefined) looks.push("unknown");
          tips.push(this.term(o.lit));
        }
        setLook(item, [...new Set(looks)]);
        setTitle(item, tips.join(" "));
      }
    }
  }

  // Where a switch stands, for the lined-path picture: "N" | "R" | null (not known).
  stands(st, name) {
    const at = st.switches.get(name);
    if (!at) return null;
    if (at.N || at.R) {
      const n = at.N ? this.shown(at.N) : false, r = at.R ? this.shown(at.R) : false;
      return n && !r ? "N" : r && !n ? "R" : null;
    }
    if (at.released) {
      const released = this.shown(at.released);
      return released === undefined ? null : released ? "R" : "N";
    }
    return null;
  }

  term(ref) {
    const st = this.stations.get(ref.station);
    return st ? st.panel.indications[ref.i].aar : `${ref.station}?`;
  }

  // -- the traffic list: every frame as a row, newest first

  traffic(st, direction, text, retained) {
    const terms = text.split(SEPARATORS).filter(Boolean);
    const before = st.previous[direction];
    st.previous[direction] = terms;
    const row = el("div", `frame ${direction}`);
    row.appendChild(el("span", "when", clock()));
    row.appendChild(el("span", "who", st.panel.station));
    row.appendChild(el("span", "way", direction === "control" ? "control ▶" : "◀ indication"));
    if (retained) row.appendChild(el("span", "retained", "retained"));
    const f = st.panel.frame;
    const body = el("span", "terms");
    terms.forEach((term, i) => {
      let cls = "term";
      if (i < f.open.length || terms.length === 1) cls += " open";
      else cls += term.startsWith(f.dropped[0]) && term.endsWith(f.dropped[1]) ? " dropped" : " asserted";
      if (before && before.length === terms.length && before[i] !== term) cls += " changed";
      body.appendChild(el("span", cls, term));
      body.appendChild(document.createTextNode(" "));
    });
    row.appendChild(body);
    if (direction === "indication") {
      const reading = st.reading;
      if (reading && reading.kind !== "valid") row.appendChild(el("span", "verdict", `${reading.kind}: ${reading.detail}`));
    }
    const list = this.root.traffic;
    list.insertBefore(row, list.firstChild);
    while (list.children.length > TRAFFIC_ROWS) list.removeChild(list.lastChild);
  }
}
