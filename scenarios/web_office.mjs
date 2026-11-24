// Runs web/ itself (office.js, field.js, mqtt.js) under node, against cases that
// scenarios/web_office.py computed with tools/codeline_chart.py. No browser: a small stand-in
// for the DOM, a recording stand-in for the MQTT client, and a scripted WebSocket.
//
//   node scenarios/web_office.mjs <cases.json>
//
// Prints one line per check; exits 1 if any failed.

import { readFileSync } from "node:fs";

let failed = 0;
function check(ok, text) {
  if (!ok) failed++;
  console.log((ok ? "  ok    " : "  FAIL  ") + text);
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const sorted = (x) => (x && typeof x === "object" && !Array.isArray(x)) ? Object.fromEntries(Object.keys(x).sort().map((k) => [k, x[k]])) : x;
const same = (a, b) => JSON.stringify(sorted(a)) === JSON.stringify(sorted(b));
async function until(condition, ms = 3000) {
  const end = Date.now() + ms;
  while (!condition() && Date.now() < end) await sleep(5);
  return condition();
}

// ---------------------------------------------------------------- a stand-in for the DOM

class Node {
  constructor(tag) {
    this.tag = tag; this.children = []; this.attributes = {}; this.listeners = {}; this.text = "";
    this.dataset = {}; this.style = {}; this.hidden = false; this.disabled = false; this.title = "";
  }
  get className() { return this.attributes.class || ""; }
  set className(v) { this.attributes.class = v; }
  get classList() {
    const node = this;
    const list = () => node.className.split(" ").filter(Boolean);
    return {
      contains: (c) => list().includes(c),
      add: (c) => { if (!list().includes(c)) node.className = list().concat(c).join(" "); },
      remove: (c) => { node.className = list().filter((x) => x !== c).join(" "); },
      toggle(c, force) { const on = force === undefined ? !this.contains(c) : force; if (on) this.add(c); else this.remove(c); return on; },
      [Symbol.iterator]: () => list()[Symbol.iterator](),
    };
  }
  setAttribute(k, v) { this.attributes[k] = String(v); }
  getAttribute(k) { return this.attributes[k]; }
  appendChild(c) { this.children.push(c); return c; }
  append(...cs) { this.children.push(...cs); }
  insertBefore(c, ref) { const i = this.children.indexOf(ref); this.children.splice(i < 0 ? this.children.length : i, 0, c); return c; }
  removeChild(c) { this.children = this.children.filter((x) => x !== c); return c; }
  get firstChild() { return this.children[0] || null; }
  get lastChild() { return this.children[this.children.length - 1] || null; }
  get textContent() { return this.text + this.children.map((c) => c.textContent).join(""); }
  set textContent(v) { this.children = []; this.text = String(v); }
  querySelector(tag) { return this.children.find((c) => c.tag === tag) || null; }
  addEventListener(type, fn) { (this.listeners[type] ||= []).push(fn); }
  fire(type, event = {}) { for (const fn of this.listeners[type] || []) fn(event); }
  click(event = {}) { if (!this.disabled) this.fire("click", event); }
  all(tag, out = []) { for (const c of this.children) { if (c.tag === tag) out.push(c); c.all(tag, out); } return out; }
}
globalThis.document = {
  createElement: (tag) => new Node(tag),
  createElementNS: (ns, tag) => new Node(tag),
  createTextNode: (text) => { const n = new Node("#text"); n.text = text; return n; },
};

// ---------------------------------------------------------------- a recording MQTT client

class FakeClient {
  constructor() { this.connected = true; this.subscriptions = []; this.published = []; this.onStatus = () => {}; }
  subscribe(filter, qos, handler) { this.subscriptions.push({ filter, qos, handler }); }
  publish(topic, payload) { if (!this.connected) return false; this.published.push([topic, payload]); return true; }
  connect() {}
  deliver(topic, payload, retained = false) {
    for (const s of this.subscriptions) if (topicMatches(s.filter, topic)) s.handler(topic, payload, retained);
  }
}

const { MqttClient, topicMatches } = await import("../web/mqtt.js");
const { Office, controlFrame, readIndications, startingPositions } = await import("../web/office.js");
const { FieldView } = await import("../web/field.js");

const cases = JSON.parse(readFileSync(process.argv[2], "utf8"));
const panels = cases.stations.map((s) => JSON.parse(readFileSync(s.panel, "utf8")));
const leversOf = (panel) => panel.columns.flatMap((c) => c.levers);
const keyOf = (lever) => `${lever.style} ${lever.name}`;
const positionsFrom = (panel, labels) => new Map(leversOf(panel).map((lv) => [lv, lv.positions.findIndex((p) => p.label === labels[keyOf(lv)])]));
const labelsOf = (panel, positions) => Object.fromEntries(leversOf(panel).map((lv) => [keyOf(lv), lv.positions[positions.get(lv)].label]));
const litOf = (panel, asserted) => panel.indications.filter((f, i) => asserted[i]).map((f) => f.aar);

// ---------------------------------------------------------------- 1. the frame functions
console.log("1. office.js controlFrame / readIndications / startingPositions against codeline_chart");
cases.stations.forEach((s, n) => {
  const panel = panels[n];
  const wrongC = s.controls.filter((c) => controlFrame(panel, positionsFrom(panel, c.positions)).text !== c.frame);
  check(!wrongC.length, `${s.station}: ${s.controls.length} lever settings spell encode_frame's frame ${wrongC.length ? JSON.stringify(wrongC[0]) : ""}`);
  const wrongI = s.indications.filter((c) => {
    const r = readIndications(panel, c.text);
    return r.kind !== c.kind || (c.kind === "valid" && !same(litOf(panel, r.asserted), c.lit));
  });
  const kinds = {};
  for (const c of s.indications) kinds[c.kind] = (kinds[c.kind] || 0) + 1;
  check(!wrongI.length, `${s.station}: ${s.indications.length} indication texts read as decode_frame reads them ${JSON.stringify(kinds)} ` +
    (wrongI.length ? JSON.stringify([wrongI[0].text, wrongI[0].kind, readIndications(panel, wrongI[0].text)]) : ""));
  const wrongS = s.indications.filter((c) => c.kind === "valid" &&
    !same(labelsOf(panel, startingPositions(panel, readIndications(panel, c.text).asserted)), c.start));
  check(!wrongS.length, `${s.station}: levers are found where each of ${s.indications.filter((c) => c.kind === "valid").length} frames says the field is ` +
    (wrongS.length ? JSON.stringify(wrongS[0]) : ""));
});
{
  // Two words of one positive-control group: refused, no frame.
  const panel = structuredClone(panels[0]);
  const group = panel.controls.findIndex((c) => c.group > 0);
  const other = panel.controls.findIndex((c, i) => i !== group && c.group === panel.controls[group].group);
  const lever = leversOf(panel).find((lv) => lv.positions.some((p) => p.asserts.includes(group)));
  lever.positions.find((p) => p.asserts.includes(group)).asserts.push(other);
  const positions = new Map(leversOf(panel).map((lv) => [lv, lv === lever ? lv.positions.findIndex((p) => p.asserts.includes(group)) : 0]));
  const r = controlFrame(panel, positions);
  check(r.text === undefined && typeof r.refused === "string", `two words of one group are refused, with a reason: ${r.refused}`);
}

// ---------------------------------------------------------------- 2. the office as a whole
console.log("2. the Office: lamps from indications only, levers from the first frame only, one channel");
{
  const client = new FakeClient();
  const root = { machine: new Node("div"), traffic: new Node("div"), codeAll: new Node("button") };
  const office = new Office(panels, client, root);
  const controlsTopics = cases.stations.map((s) => s.controls_topic);
  const indicationsTopics = cases.stations.map((s) => s.indications_topic);
  check(same(client.subscriptions.map((s) => s.filter).sort(), [...indicationsTopics].sort()),
    "it subscribes to the stations' indications topics and nothing else");

  const lamps = (st) => st.lamps.map(({ jewel }) => jewel.classList.contains("lit"));
  const levers = (st) => Object.fromEntries(st.levers.map(({ lever, buttons }) =>
    [keyOf(lever), buttons.filter((b) => b.classList.contains("on")).map((b) => b.textContent).join("+")]));
  // A lamp is lit by its own indication; a signal lever's centre lamp (word TIME, labelled STOP)
  // is also lit, steady, while neither the LEFT nor the RIGHT lamp of its lever is.
  const expectLamps = (st, lit) => st.levers.flatMap(({ lever }) => {
    const on = (lamp) => lit.includes(st.panel.indications[lamp.indication].aar) !== lamp.inverted;
    const clear = lever.lamps.some((l) => (l.word === "LEFT" || l.word === "RIGHT") && on(l));
    return lever.lamps.map((l) => on(l) || (lever.style === "signal" && l.word === "TIME" && !clear));
  });
  const flashing = (st) => st.lamps.map(({ jewel }) => jewel.classList.contains("flash"));
  const expectFlash = (st, lit) => st.levers.flatMap(({ lever }) => lever.lamps.map((l) =>
    lever.style === "signal" && l.word === "TIME" && lit.includes(st.panel.indications[l.indication].aar)));

  // before any frame: nothing can be sent, no lever stands anywhere, every lamp is dark
  for (const st of office.stations.values()) { st.code.click(); st.code.fire("click"); office.code(st); st.levers[0].buttons[0].click(); }
  root.codeAll.click();
  const first = [...office.stations.values()];
  check(client.published.length === 0 && first.every((st) => st.code.disabled), "before a station has spoken its CODE is disabled and nothing is sent");
  check(first.every((st) => lamps(st).every((x) => !x) && Object.values(levers(st)).every((x) => x === "")),
    "before a station has spoken every lamp is dark and no lever shows a position");

  cases.stations.forEach((s, n) => {
    const st = office.stations.get(s.station);
    const valid = s.indications.filter((c) => c.kind === "valid");
    const bad = [];
    // the first current frame places the levers
    client.deliver(s.indications_topic, valid[0].text, true);
    if (!same(levers(st), valid[0].start)) bad.push(`first frame: levers ${JSON.stringify(levers(st))}, expected ${JSON.stringify(valid[0].start)}`);
    if (!same(lamps(st), expectLamps(st, valid[0].lit))) bad.push("first frame: lamps");
    if (st.code.disabled) bad.push("CODE still disabled after the first frame");
    // moving every lever through every position never changes a lamp
    const before = lamps(st);
    for (const { buttons } of st.levers) for (const b of buttons) { b.click(); if (!same(lamps(st), before)) bad.push("a lamp followed a lever"); }
    // later frames move lamps, never levers
    const stand = levers(st);
    for (const c of s.indications) {
      client.deliver(s.indications_topic, c.text);
      const want = c.kind === "valid" ? expectLamps(st, c.lit) : st.lamps.map(() => false);
      if (!same(lamps(st), want)) bad.push(`lamps after ${c.kind} frame ${c.text.slice(0, 60)}`);
      const wantFlash = c.kind === "valid" ? expectFlash(st, c.lit) : st.lamps.map(() => false);
      if (!same(flashing(st), wantFlash)) bad.push(`flashing lamps after ${c.kind} frame ${c.text.slice(0, 60)}`);
      if (!same(levers(st), stand)) bad.push("a lever moved on a later frame");
      if (st.block.classList.contains("dark") !== (c.kind !== "valid")) bad.push(`${c.kind}: station shown ${st.block.classList.contains("dark") ? "dark" : "current"}`);
    }
    // CODE sends the levers as they stand, on the controls topic, once
    for (const c of s.controls.slice(0, 12)) {
      for (const { lever, buttons } of st.levers) buttons.find((b) => b.textContent === c.positions[keyOf(lever)]).click();
      const sent = client.published.length;
      st.code.click();
      const out = client.published.slice(sent);
      if (!(out.length === 1 && out[0][0] === s.controls_topic && out[0][1] === c.frame)) bad.push(`CODE sent ${JSON.stringify(out)}, expected ${c.frame}`);
    }
    client.deliver(s.indications_topic, valid[0].text);
    check(!bad.length, `${s.station}: levers placed by the first frame only; lamps follow ${s.indications.length} frames and never a lever; CODE sends the levers ${bad.slice(0, 2).join("; ")}`);
  });
  for (const st of office.stations.values()) clearTimeout(st.awaiting);

  // code all: only to stations that are current
  const gone = cases.stations[0];
  client.deliver(gone.indications_topic, panels[0].unavailable);
  const sent = client.published.length;
  root.codeAll.click();
  const to = client.published.slice(sent).map((m) => m[0]).sort();
  check(same(to, controlsTopics.filter((t) => t !== gone.controls_topic).sort()), `"code all" sends one frame to each current station and none to the one that is unavailable (${to.length})`);
  for (const st of office.stations.values()) clearTimeout(st.awaiting);
  check(client.published.every(([topic]) => controlsTopics.includes(topic)), `all ${client.published.length} publishes are on controls topics`);

  // no connection: refused, said so; the note goes when the connection returns
  const st = office.stations.get(cases.stations[1].station);
  client.connected = false;
  const n = client.published.length;
  st.code.click();
  const said = st.status.textContent;
  office.linkChanged(false);
  client.connected = true;
  office.linkChanged(true);
  check(client.published.length === n && said.includes("not sent") && !st.status.textContent.includes("not sent"),
    `without a connection nothing is sent ("${said}"); the note is gone once the connection is back ("${st.status.textContent}")`);

}

// ---------------------------------------------------------------- 3. the field views
console.log("3. FieldView: appliances, simulation and debug only");
cases.stations.forEach((s, n) => {
  const panel = panels[n];
  const client = new FakeClient();
  const view = new FieldView(panel, client);
  const filters = client.subscriptions.map((x) => x.filter);
  const allowed = new Set(panel.appliances.flatMap((a) => [a.topics.state, a.topics.cmd, a.topics.event]).concat([panel.topics.debug, panel.topics.plant]));
  const subsOk = filters.every((f) => allowed.has(f)) && !filters.includes(s.controls_topic) && !filters.includes(s.indications_topic);
  for (const a of panel.appliances) client.deliver(a.topics.state, Object.keys(a.shows)[0] || "x", true);
  for (const b of view.root.all("button")) b.click();
  for (const item of view.items) { item.node.fire("click", { shiftKey: false }); item.node.fire("click", { shiftKey: true }); }
  for (const a of panel.appliances) client.deliver(a.topics.state, panel.unavailable, true);
  for (const b of view.root.all("button")) b.click();
  const topics = client.published.map((m) => m[0]);
  const simulate = new Set(panel.appliances.map((a) => a.topics.simulate));
  const pubOk = topics.length > 0 && topics.every((t) => simulate.has(t) && t.startsWith(s.simulation + "/") && !t.startsWith(s.codeline + "/"));
  const payloads = new Set(client.published.map((m) => m[1]));
  const wanted = new Set(panel.appliances.flatMap((a) => a.actions.map((x) => x.payload).concat([a.power.off, a.power.on])));
  check(subsOk && pubOk && same([...payloads].sort(), [...wanted].sort()),
    `${s.station}: ${filters.length} subscriptions (appliances and debug only), ${topics.length} publishes, all on its simulation topics, none on the code line`);
});

// ---------------------------------------------------------------- 4. mqtt.js
console.log("4. mqtt.js against a scripted WebSocket");
{
  const sockets = [];
  class Socket {
    constructor(url, protocol) { this.url = url; this.protocol = protocol; this.sent = []; this.readyState = 0; this.wasClosed = false; sockets.push(this); }
    send(bytes) { this.sent.push([...bytes]); }
    close() { this.readyState = 3; this.wasClosed = true; }
    accept() { this.readyState = 1; this.onopen(); }
    give(bytes) { this.onmessage({ data: new Uint8Array(bytes).buffer }); }
  }
  globalThis.WebSocket = Socket;
  const errors = [];
  const realError = console.error;
  console.error = (...args) => errors.push(args.join(" "));
  const text = (s) => [...new TextEncoder().encode(s)];
  const publish = (topic, payload, { qos = 0, retain = false, id = 7 } = {}) => {
    const body = [0, topic.length, ...text(topic), ...(qos ? [id >> 8, id & 255] : []), ...text(payload)];
    return [0x30 | (qos << 1) | (retain ? 1 : 0), body.length, ...body];
  };

  check(topicMatches("/a/+/c", "/a/b/c") && !topicMatches("/a/+/c", "/a/b/d") && topicMatches("/a/#", "/a/b/c") &&
        !topicMatches("/a/+", "/a/b/c") && !topicMatches("/a/b", "/a/b/c") &&
        !topicMatches("#", "$SYS/x") && !topicMatches("+/x", "$SYS/x") && topicMatches("$SYS/#", "$SYS/x") && topicMatches("#", "/a"),
    "topic filters: + and #, and a leading wildcard does not match a $ topic");

  const status = [];
  const c = new MqttClient({ url: "ws://x", clientId: "t", onStatus: (s, d) => status.push([s, d]), waits: { connackMs: 60, probeMs: 60 } });
  const got = [];
  let thrown = 0;
  c.subscribe("/t/#", 1, (topic, payload, retained) => { if (payload === "boom") { thrown++; throw new Error("handler"); } got.push([topic, payload, retained]); });
  check(c.publish("/t/a", "x") === false, "publish before connecting is refused");
  c.connect();
  let s = sockets.at(-1);
  s.accept();
  const connect = s.sent[0];
  check(s.protocol === "mqtt" && connect[0] === 0x10 && same(connect.slice(2, 9), [0, 4, ...text("MQTT"), 4]) && connect[9] === 0x02 &&
        (connect[10] << 8 | connect[11]) >= 60, `CONNECT: MQTT 3.1.1, clean session, keep-alive ${connect[10] << 8 | connect[11]} s (long enough for throttled timers)`);
  s.give([0x20, 2, 0, 0]);
  const subscribe = s.sent[1];
  check(c.connected && status.at(-1)[0] === "connected" && subscribe[0] === 0x82, "CONNACK connects; the subscriptions follow");

  // (a) a failing handler loses one message, not the stream; a packet split across chunks is joined
  const third = publish("/t/c", "three", { qos: 1, retain: true, id: 9 });
  s.give([...publish("/t/a", "boom"), ...publish("/t/b", "two"), ...third.slice(0, 5)]);
  s.give(third.slice(5));
  check(thrown === 1 && same(got, [["/t/b", "two", false], ["/t/c", "three", true]]) && errors.length === 1,
    "a handler that throws loses its own message only; the next and a split one arrive, with the retain flag");
  check(same(s.sent.at(-1), [0x40, 2, 0, 9]), "a QoS 1 publish is answered with PUBACK");
  check(c.publish("/t/out", "hello") && s.sent.at(-1)[0] === 0x30, "publish goes out at QoS 0, unretained");

  // (d) a refused subscription is surfaced
  s.give([0x90, 3, subscribe[2], subscribe[3], 0x80]);
  check(status.at(-1)[0] === "refused" && status.at(-1)[1].includes("/t/#"), `SUBACK 0x80 is surfaced: ${status.at(-1)[1]}`);

  // (f) probe on becoming visible: answered -> stays; unanswered -> given up
  c.wake();
  const pinged = same(s.sent.at(-1), [0xc0, 0]);
  await sleep(20);
  s.give([0xd0, 0]);
  await sleep(80);
  check(pinged && c.connected, "becoming visible pings at once; an answer keeps the connection");
  c.wake();
  await sleep(100);
  check(!c.connected && s.wasClosed && status.at(-1)[0] === "disconnected" && status.at(-1)[1].includes("silent"),
    `no answer to the probe: the socket is given up (${status.at(-1)[1]})`);
  check(c.publish("/t/out", "late") === false, "nothing is published or queued while disconnected");

  // backoff: the next socket opens after 1 s; (b) a 5-byte remaining length is a protocol error
  const t0 = Date.now();
  await until(() => sockets.length === 2);
  s = sockets.at(-1);
  check(sockets.length === 2 && Date.now() - t0 > 700, "it reconnects after the first backoff (1 s)");
  s.accept();
  s.give([0x20, 2, 0, 0]);
  check(c.connected && s.sent[1][0] === 0x82, "reconnected: subscribed again");
  s.give([0x30, 0xff, 0xff, 0xff, 0xff, 0x01, 1, 2, 3]);
  check(!c.connected && s.wasClosed && status.at(-1)[1].includes("protocol error") && c.buffer.length === 0,
    `a fifth length byte is a protocol error: closed, buffer dropped (${status.at(-1)[1]})`);

  // (c) no CONNACK: given up and retried through the backoff
  await until(() => sockets.length === 3);
  s = sockets.at(-1);
  s.accept();
  await sleep(100);
  check(sockets.length === 3 && s.wasClosed && status.at(-1)[1].includes("no answer") && status.at(-1)[1].includes("2 s"),
    `no CONNACK in time: given up, backoff doubled (${status.at(-1)[1]})`);
  c.close();
  check(status.at(-1)[0] === "closed", "close() ends it: no further retry");
  await sleep(50);

  // (g) publishing does not put the ping off: a broker that went silent is found out
  const busyStatus = [];
  const busy = new MqttClient({ url: "ws://x", clientId: "b", onStatus: (st, d) => busyStatus.push([st, d]), waits: { connackMs: 60, probeMs: 40, pingMs: 60 } });
  busy.connect();
  s = sockets.at(-1);
  s.accept();
  s.give([0x20, 2, 0, 0]);
  for (let i = 0; i < 20 && busy.connected; i++) { busy.publish("/t/out", "x"); await sleep(10); }
  await sleep(60);
  check(!busy.connected && s.sent.some((p) => same(p, [0xc0, 0])) && busyStatus.at(-1)[1].includes("silent"),
    "a client that keeps publishing still pings, and gives up a broker that never answers");
  busy.close();
  console.error = realError;
}

// ---------------------------------------------------------------- 5. trace.js
console.log("5. trace.js: how the switches stand, as paths from each entry (never a route)");
{
  const { trackGraph, tracePaths } = await import("../web/trace.js");
  const panel = panels[cases.stations.findIndex((s) => s.station === "Corporal")];
  const graph = trackGraph(panel.drawing);
  const label = (i) => { const t = panel.drawing.elements[i].tag || {}; return `${t.name}${t.part ? "." + t.part : ""}`; };
  const entry = (name) => graph.entries.filter(([r]) => label(graph.rails[r].index) === name);
  const from = (name, stands) => new Set([...tracePaths({ ...graph, entries: entry(name) }, (sw) => stands[sw] || null)].map(label));
  const has = (set, yes, no) => yes.every((x) => set.has(x)) && no.every((x) => !set.has(x));
  check(same(graph.entries.map(([r]) => label(graph.rails[r].index)).sort(), ["1T1", "1T1", "834T", "Christopher:819T", "Christopher:820T"]),
    "Corporal's entries are the tracks that leave its drawing: three at its edges and the two beet loader tracks, by their wire labels");
  {
    const sargent = panels[cases.stations.findIndex((s) => s.station === "Sargent")];
    const ends = (p) => p.drawing.elements.flatMap((e) => e.labels || []).filter((l) => l && l.startsWith("Beet Loader")).sort();
    check(same(ends(panel), ["Beet Loader 1", "Beet Loader 2"]) && same(ends(sargent), ["Beet Loader 1", "Beet Loader 2"]),
      "the beet loader tracks leave Corporal and enter Sargent on the same two wire labels");
  }
  check(has(from("834T", { 829: "N", 827: "N" }), ["832T", "829.C", "829.N", "827.N", "827.C", "Christopher:819T"], ["829.R", "Christopher:820T", "827.R"]),
    "from the east, 829 normal and 827 normal: through both switches to the one main track");
  check(has(from("834T", { 829: "R", 827: "N" }), ["829.C", "829.R", "829T1", "Christopher:820T"], ["829.N", "827.N", "Christopher:819T"]),
    "from the east, 829 reverse: to the other main track");
  check(same([...from("834T", { 829: "N", 827: "R" })].sort(), ["832T", "834T"]),
    "from the east, 827 reverse: dead at 827's points, so drawn only up to the last westward signal (830W)");
  check(same([...from("Christopher:819T", { 829: "R", 827: "N" })].sort(), ["Christopher:819T"]),
    "from the west on the first main, 829 reverse: dead at 829's points, so drawn only up to signal 828E (a meet)");
  check(same([...from("Christopher:820T", { 829: "N", 827: "N" })].sort(), ["Christopher:820T"]),
    "from the west on the second main, 829 normal: dead, drawn only up to signal 830E, not into 829's reverse leg");
  check(has(from("Christopher:819T", { 829: "N", 827: "R" }), ["827.C", "827.R", "827D.body", "01T"], ["827.N", "1.R", "1T1"]),
    "from the west, 827 reverse: into the siding, and it stops at a switch whose position is not known");
  check(has(from("834T", {}), ["832T", "829.C"], ["829.N", "829.R"]),
    "a switch whose position is not known ends the path");

  // The office: 829 from its NORMAL / REVERSE indications, 827 from its lock (locked: normal, released: reverse).
  const client = new FakeClient();
  const root = { machine: new Node("div"), traffic: new Node("div"), codeAll: new Node("button") };
  const office = new Office(panels, client, root);
  const st = office.stations.get("Corporal");
  const traced = () => new Set([...st.rails].filter(([, item]) => item.node.classList.contains("traced")).map(([e]) => `${e.tag.name}${e.tag.part ? "." + e.tag.part : ""}`));
  check(traced().size === 0, "before the station has spoken no path is drawn");
  check(same(office.links.map((l) => `${l.a[0].panel.station}>${l.b[0].panel.station}`).sort(), ["Corporal>Sargent", "Corporal>Sargent"]),
    "the board draws the two beet loader tracks across from Corporal to Sargent, and no other piece between stations");
  const frame = (lit) => panel.frame.open.concat(panel.indications.map((f) => (lit.includes(f.aar) ? f.aar : `(${f.aar})`))).join(" ");
  client.deliver(panel.topics.indications, frame(["829NWK"]));
  check(has(traced(), ["834T", "829.N", "827.N", "827.C", "Christopher:819T", "Christopher:820T"], ["827.R", "829.R"]),
    "829NWK and the lock locked: the main is drawn through 827 normal, the other main only up to its signal");
  // Masts: stop or not stop, from the lever's LEFT / RIGHT indication for the mast's direction.
  const heads = (name) => office.board.flatMap((b) => b.items).filter((item) => item.element.role === "head" && item.element.tag.name === name);
  const look = (name, cls) => heads(name).length > 0 && heads(name).every((item) => item.node.classList.contains(cls));
  check(look("830EAB", "stop") && look("830WAB", "stop") && look("828EA", "stop"), "no signal indicated clear: every mast of the station's levers is at stop");
  check(heads("832EA").length === 0, "a mast of another station's lever is not bound: it stays unlit");
  client.deliver(panel.topics.indications, frame(["829NWK", "830EGK"]));
  check(look("830EAB", "clear") && !look("830EAB", "stop") && look("830WAB", "stop") && look("828EA", "stop"),
    "830EGK: the eastward mast of lever 830 is not at stop; its westward mast and lever 828's masts still are");
  client.deliver(panel.topics.indications, frame(["829RWK", "827WLK"]));
  check(has(traced(), ["829.R", "Christopher:820T", "827.R", "01T"], ["827.N"]), "829RWK and 827WLK: 829 reverse, and the released lock draws 827 reverse");
  client.deliver(panel.topics.indications, "UNAVAILABLE");
  check(traced().size === 0, "a station that is not current draws no path");
}

console.log(failed ? `FAILED: ${failed}` : "PASSED");
process.exit(failed ? 1 : 0);
