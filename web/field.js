// A FIELD view: one station = one interlocking, as it stands on the ground.
//
// It has its own MQTT connection and speaks only on the appliance channel (each appliance's
// /state, /cmd and /event), the simulation channel (the physical world: a train, a crew, a
// power cut) and the station's debug topics. It never sends or reads a code line frame.
// The track is coloured from the appliances' own /state, not from indications.

import { drawTrackplan, setLook, setTitle } from "./draw.js";

const DEBUG_LINES = 400;
const SEPARATORS = /[ \t\r\n]+/;

function el(name, cls, text) {
  const n = document.createElement(name);
  if (cls) n.className = cls;
  if (text !== undefined) n.textContent = text;
  return n;
}

export class FieldView {
  // client: this station's own MqttClient (not yet connected); the view connects it.
  constructor(panel, client) {
    this.panel = panel;
    this.appliances = new Map();        // key -> {appliance, state, cmd, event, row cells}
    for (const appliance of panel.appliances) this.appliances.set(appliance.key, { appliance });
    this.root = el("div", "field-view");
    this.linkNote = el("span", "link");
    this.note = el("span", "field-note");
    this.build();

    this.client = client;
    client.onStatus = (state, detail) => this.onLink(state, detail);
    for (const entry of this.appliances.values()) {
      const t = entry.appliance.topics;
      this.client.subscribe(t.state, 1, (topic, payload) => { entry.state = payload; this.refresh(); });
      this.client.subscribe(t.cmd, 0, (topic, payload) => { entry.cmd = payload; this.refresh(); });
      this.client.subscribe(t.event, 0, (topic, payload) => { entry.event = payload; this.refresh(); });
    }
    this.client.subscribe(panel.topics.debug, 0, (topic, payload) => this.debugLine(payload));
    this.client.subscribe(panel.topics.plant, 1, (topic, payload) => { this.plant.textContent = payload; });
    this.client.connect();
    this.refresh();
  }

  build() {
    const bar = el("div", "bar");
    bar.append(el("h2", "", this.panel.station), this.linkNote, this.note);
    this.root.appendChild(bar);
    this.root.appendChild(el("p", "hint",
      "Click a track, or a switch, to shunt or clear its circuit (place and move a train). " +
      "Shift-click a switch to throw it by hand. Click a lock or contact lamp to open or close it. " +
      "What a crew has at an electrically locked switch is in the box under the track."));

    const drawn = drawTrackplan(this.panel.drawing, 0);
    this.items = drawn.items.filter((item) => item.element.field);
    for (const item of this.items) {
      item.node.classList.add("live");
      item.node.addEventListener("click", (event) => this.clicked(item.element, event));
    }
    const holder = el("div", "field-plan");
    holder.appendChild(drawn.svg);
    this.root.appendChild(holder);

    this.locals = [];
    const boxes = el("div", "locals");
    for (const entry of this.appliances.values()) {
      if (entry.appliance.local) boxes.appendChild(this.buildLocal(entry));
    }
    if (this.locals.length) this.root.appendChild(boxes);

    const table = el("table", "appliances");
    const head = el("tr");
    for (const h of ["appliance", "kind", "/state", "last /cmd", "last /event", "the physical world"]) head.appendChild(el("th", "", h));
    table.appendChild(head);
    for (const entry of this.appliances.values()) {
      const a = entry.appliance;
      const row = el("tr");
      row.appendChild(el("td", "key", a.key));
      row.appendChild(el("td", "", a.operated ? `${a.kind} (${a.operated})` : a.kind));
      entry.cells = { state: el("td", "state"), cmd: el("td", "cmd"), event: el("td", "event") };
      row.append(entry.cells.state, entry.cells.cmd, entry.cells.event);
      const acts = el("td", "acts");
      for (const action of a.actions) {
        const b = el("button", "", action.label);
        b.title = `${a.topics.simulate} ${action.payload}`;
        b.addEventListener("click", () => this.simulate(a, action.payload));
        acts.appendChild(b);
      }
      entry.power = el("button", "power", "Cut power");
      entry.power.addEventListener("click", () => this.simulate(a, this.unavailable(entry) ? a.power.on : a.power.off));
      acts.appendChild(entry.power);
      row.appendChild(acts);
      entry.row = row;
      table.appendChild(row);
    }
    this.root.appendChild(table);

    this.root.appendChild(el("h3", "", "Plant (debug snapshot)"));
    this.plant = el("pre", "plant", "(nothing published on the station's debug topics yet)");
    this.root.appendChild(this.plant);
    this.root.appendChild(el("h3", "", "Debug log"));
    this.log = el("pre", "log", "");
    this.root.appendChild(this.log);
  }

  // What a crew has at an electrically locked switch: the release lamp, the request button, the
  // lock itself, and the hand switches it locks. Every word and topic is the panel file's.
  buildLocal(entry) {
    const a = entry.appliance;
    const box = el("div", "local");
    box.appendChild(el("div", "local-title", `Electric lock ${a.name} · at the switch`));
    const row = el("div", "local-row");
    const local = { entry, held: false, switches: [] };
    if (a.local.lamp) {
      const cell = el("span", "lamp");
      local.jewel = el("span", "jewel lamp-amber");
      cell.append(local.jewel, el("span", "lamp-label", "RELEASED"));
      cell.title = `lit by ${a.topics.cmd} ${a.local.lamp}`;
      row.appendChild(cell);
    }
    if (a.local.button) {
      local.request = el("button", "", "Request unlock");
      local.request.title = `${a.topics.simulate} ${a.local.button}: press to hold the request, press again to let go`;
      local.request.addEventListener("click", () => {
        local.held = !local.held;
        this.simulate(a, local.held ? a.local.button : `(${a.local.button})`);
        this.refresh();
      });
      row.appendChild(local.request);
    }
    if (a.local.contact) {
      local.lock = el("button", "", "Open lock");
      local.lock.addEventListener("click", () => this.toggle(a.key));
      row.appendChild(local.lock);
    }
    for (const name of a.covers || []) {
      const sw = [...this.appliances.values()].find((e) => e.appliance.kind === "switch" && e.appliance.name === name);
      if (!sw) continue;
      const group = el("span", "local-switch");
      group.appendChild(el("span", "local-name", name));
      const buttons = sw.appliance.actions.map((action) => {
        const b = el("button", "", action.payload);
        b.title = `${sw.appliance.topics.simulate} ${action.payload}`;
        b.addEventListener("click", () => this.simulate(sw.appliance, action.payload));
        group.appendChild(b);
        return { b, payload: action.payload };
      });
      local.switches.push({ sw, buttons });
      row.appendChild(group);
    }
    box.appendChild(row);
    this.locals.push(local);
    return box;
  }

  // ---- what an appliance's /state says (through the panel file's words only)

  unavailable(entry) {
    return entry.state === this.panel.unavailable;
  }

  // The looks the panel file gives the terms of a /state: e.g. ["occupied"], ["normal"], ["stop"].
  looks(entry) {
    if (entry.state === undefined || this.unavailable(entry)) return null;
    const out = [];
    for (const term of entry.state.split(SEPARATORS)) {
      if (entry.appliance.shows[term]) out.push(entry.appliance.shows[term]);
    }
    return out;
  }

  asserts(entry, term) {
    return entry.state !== undefined && entry.state.split(SEPARATORS).includes(term);
  }

  // ---- acting on the physical world

  simulate(appliance, payload) {
    if (!this.client.publish(appliance.topics.simulate, payload)) {
      this.note.textContent = "not sent: no connection";
      return;
    }
    this.note.textContent = `${appliance.topics.simulate} ${payload}`;
  }

  toggle(key) {
    const entry = this.appliances.get(key);
    if (!entry || !entry.appliance.click) return;
    const c = entry.appliance.click;
    this.simulate(entry.appliance, this.asserts(entry, c.term) ? c.asserted : c.otherwise);
  }

  clicked(element, event) {
    const f = element.field;
    // A click places or removes a train, on a switch as on plain track; throwing the switch by
    // hand is the rarer act and takes the shift key. Something with no circuit has only itself.
    if (f.circuit && !(event.shiftKey && f.appliance)) this.toggle(f.circuit);
    else if (f.appliance) this.toggle(f.appliance);
  }

  // ---- drawing the state

  onLink(state, detail) {
    this.linkNote.textContent = `appliances + simulation: ${state}${detail ? " (" + detail + ")" : ""}`;
    this.linkNote.className = `link ${state}`;
    if (this.onStatus) this.onStatus(state);
    if (state === "disconnected" || state === "closed") {
      for (const entry of this.appliances.values()) entry.state = undefined;   // nothing is known without the link
      this.refresh();
    }
  }

  refresh() {
    for (const entry of this.appliances.values()) {
      const gone = this.unavailable(entry);
      entry.cells.state.textContent = entry.state === undefined ? "—" : entry.state;
      entry.cells.cmd.textContent = entry.cmd === undefined ? "—" : entry.cmd;
      entry.cells.event.textContent = entry.event === undefined ? "—" : entry.event;
      entry.row.classList.toggle("gone", gone);
      entry.row.classList.toggle("silent", entry.state === undefined);
      entry.power.textContent = gone ? "Restore power" : "Cut power";
    }
    for (const local of this.locals) {
      const a = local.entry.appliance;
      const cmd = local.entry.cmd;
      if (local.jewel) local.jewel.classList.toggle("lit", cmd !== undefined && cmd.split(SEPARATORS).includes(a.local.lamp));
      if (local.request) local.request.classList.toggle("on", local.held);
      if (local.lock) {
        const open = this.asserts(local.entry, a.local.contact);
        local.lock.textContent = open ? "Close lock" : "Open lock";
        local.lock.classList.toggle("on", open);
      }
      for (const { sw, buttons } of local.switches) {
        for (const { b, payload } of buttons) b.classList.toggle("on", this.asserts(sw, payload));
      }
    }
    for (const item of this.items) {
      const f = item.element.field;
      const tag = item.element.tag;
      const looks = ["live"];
      const tips = [];
      const circuit = f.circuit && this.appliances.get(f.circuit);
      const own = f.appliance && this.appliances.get(f.appliance);
      if (circuit) {
        const l = this.looks(circuit);
        looks.push(l === null ? "unknown" : l.includes("occupied") ? "occupied" : "vacant");
        tips.push(`${circuit.appliance.key}: ${circuit.state === undefined ? "no state" : circuit.state}`);
      }
      if (own) {
        const l = this.looks(own);
        if (l === null) looks.push("unknown");
        else if (tag.part === "N") looks.push(l.includes("normal") ? "lined" : "unlined");
        else if (tag.part === "R") looks.push(l.includes("reverse") ? "lined" : "unlined");
        else for (const look of l) looks.push(`is-${look}`);
        if (l !== null && own.appliance.kind === "switch" && l.length === 0) looks.push("moving");
        tips.push(`${own.appliance.key}: ${own.state === undefined ? "no state" : own.state}`);
      }
      setLook(item, looks);
      setTitle(item, tips.join("\n"));
    }
  }

  debugLine(text) {
    const lines = (this.log.textContent ? this.log.textContent.split("\n") : []).concat(text.split("\n"));
    this.log.textContent = lines.slice(-DEBUG_LINES).join("\n");
    this.log.scrollTop = this.log.scrollHeight;
  }
}
