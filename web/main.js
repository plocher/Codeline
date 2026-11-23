// The page: loads the panel files, then builds the office (top, always visible), the clock
// controls, and one field tab per station. Three kinds of MQTT connection, kept apart:
//
//   office       the code line channel only            (office.js)
//   field tab    appliances + simulation + debug       (field.js, one connection per station)
//   clock        the simulation clock topics           (here)
//
// The two halves can live in separate windows: ?view=office or ?view=field builds only that half
// (plus the clock). Each window has its own connections; nothing passes between windows except
// through the broker, exactly as between a real office and the field.
//
// Everything shown comes from ../build/panels.json and ../build/<Station>.panel.json
// (tools/panel_file.py). Nothing about a station is written in web/.

import { MqttClient } from "./mqtt.js";
import { Office } from "./office.js";
import { FieldView } from "./field.js";

// The broker, unless ?ws= names one. A page served over HTTPS is behind a proxy that also carries
// the broker's WebSocket at /mqtt on the same host (a browser refuses ws:// from an https page).
const DEFAULT_BROKER = location.protocol === "https:" ? `wss://${location.host}/mqtt` : "ws://127.0.0.1:9001";
const BUILD = "../build/";
const SEPARATORS = /[ \t\r\n]+/;

const $ = (id) => document.getElementById(id);

function el(name, cls, text) {
  const n = document.createElement(name);
  if (cls) n.className = cls;
  if (text !== undefined) n.textContent = text;
  return n;
}

async function fetchJson(path) {
  const response = await fetch(path, { cache: "no-store" });
  if (!response.ok) throw new Error(`${path}: ${response.status} ${response.statusText}`);
  return response.json();
}

function showLink(node, name, state, detail) {
  node.textContent = `${name}: ${state}`;
  node.title = detail;
  node.className = `link ${state}`;
}

// ---- the simulation clock (degrades to "no clock" when no host publishes a clock state)
//
// Every host of the layout listens on the one command topic and states its own clock on its
// own retained topic; a host that is gone leaves the unavailable word there. Shown: one cell
// per station of panels.json that has said anything. The rate selector shows what the hosts
// report. Step is offered only while every reporting host is paused (a host refuses it otherwise).

const RATES = [0.1, 0.25, 0.5, 1, 2, 5, 10];      // offered, as multiples of real time

function buildClock(index, client) {
  const words = index.clock;
  const states = new Map();           // station -> its clock payload
  const stepMs = $("clock-step-ms");
  stepMs.max = words.maxStepMs;

  // {mode: running | paused | gone | unknown, rate (permille) | NaN, t (ms) | NaN}
  const read = (payload) => {
    const parts = payload.split(SEPARATORS).filter(Boolean);
    if (parts.length === 1 && parts[0] === index.unavailable) return { mode: "gone", rate: NaN, t: NaN };
    const value = (word) => { const i = parts.indexOf(word); return i >= 0 ? Number(parts[i + 1]) : NaN; };
    const mode = parts[0] === words.paused ? "paused" : parts[0] === words.running ? "running" : "unknown";
    return { mode, rate: value(words.rate), t: value(words.time) };
  };

  function show() {
    const out = $("clock-state");
    out.textContent = "";
    const readings = [...states].map(([station, payload]) => [station, payload, read(payload)]);
    // One clock for the layout: every host follows the same commands, so they are shown as one
    // (mode, rate, time of the longest-running host). A host is named only when it differs:
    // gone, or in another mode or at another rate than the rest.
    const live = readings.filter(([, , r]) => r.mode === "running" || r.mode === "paused");
    const kind = (r) => `${r.mode} ${r.rate}`;
    const counts = new Map();
    for (const [, , r] of live) counts.set(kind(r), (counts.get(kind(r)) || 0) + 1);
    const common = [...counts].sort((x, y) => y[1] - x[1])[0];
    if (common) {
      const lead = live.map((x) => x[2]).filter((r) => kind(r) === common[0]).sort((x, y) => (y.t || 0) - (x.t || 0))[0];
      const seconds = Math.floor((lead.t || 0) / 1000);
      const stamp = [Math.floor(seconds / 3600), Math.floor(seconds / 60) % 60, seconds % 60].map((n) => String(n).padStart(2, "0")).join(":");
      const cell = el("span", `clock-cell ${lead.mode}`,
        `${lead.mode === "paused" ? words.paused : words.running}` + (isNaN(lead.rate) ? "" : ` ×${lead.rate / words.realTime}`) + ` ${stamp}`);
      cell.title = readings.map(([station, payload]) => `${station}: ${payload}`).join("\n");
      out.appendChild(cell);
    }
    for (const [station, payload, r] of readings) {
      if (common && kind(r) === common[0]) continue;
      const cell = el("span", `clock-cell ${r.mode}`);
      cell.textContent = r.mode === "gone" ? `${station} ${payload}`
        : `${station} ${payload.split(SEPARATORS)[0]}` + (isNaN(r.rate) ? "" : ` ×${r.rate / words.realTime}`);
      cell.title = payload;
      out.appendChild(cell);
    }
    const reporting = readings.map((x) => x[2]).filter((r) => r.mode === "running" || r.mode === "paused");
    if (!reporting.length) out.appendChild(el("span", "clock-none", "no clock state from any host"));
    const allPaused = reporting.length > 0 && reporting.every((r) => r.mode === "paused");
    $("clock-step").disabled = !allPaused;
    $("clock-step").title = allPaused ? "" : "Only while every reporting host is paused";
    $("clock-hint").hidden = !reporting.some((r) => r.mode === "paused");

    // The rate selector: the offered rates, plus whatever the hosts report.
    const reported = [...new Set(reporting.map((r) => r.rate).filter((n) => !isNaN(n)))];
    const select = $("clock-rate");
    const signature = reported.join(",") + "/" + reporting.length;
    if (select.dataset.shows === signature) return;      // unchanged: leave an open menu alone
    select.dataset.shows = signature;
    select.textContent = "";
    const option = (value, text) => { const o = el("option", "", text); o.value = value; select.appendChild(o); return o; };
    if (reported.length !== 1) option("", reported.length ? "mixed" : "—").selected = true;
    const permille = new Set(RATES.map((x) => Math.round(x * words.realTime)).filter((n) => n >= 1 && n <= words.maxRate));
    if (reported.length === 1) permille.add(reported[0]);
    for (const n of [...permille].sort((a, b) => a - b)) {
      option(String(n), `×${n / words.realTime}`).selected = reported.length === 1 && n === reported[0];
    }
  }

  for (const { station, topic } of words.states) {
    client.subscribe(topic, 1, (t, payload) => {
      if (payload === "") states.delete(station); else states.set(station, payload);
      show();
    });
  }
  const send = (payload) => {
    let sent = true;
    for (const topic of words.commands) sent = client.publish(topic, payload) && sent;
    $("clock-note").textContent = sent ? payload : "not sent: no connection";
  };
  $("clock-pause").addEventListener("click", () => send(words.pause));
  $("clock-run").addEventListener("click", () => send(words.run));
  $("clock-step").addEventListener("click", () => {
    const ms = Math.min(words.maxStepMs, Math.max(1, Math.round(Number(stepMs.value) || 0)));
    stepMs.value = ms;
    send(`${words.step} ${ms}`);
  });
  $("clock-rate").addEventListener("change", () => {
    const n = Number($("clock-rate").value);
    if (n >= 1 && n <= words.maxRate) send(`${words.rate} ${n}`);
    delete $("clock-rate").dataset.shows;
    show();                                        // the selector shows the hosts, not the wish
  });
  client.onStatus = (state, detail) => {
    showLink($("clock-link"), "simulation", state, detail);
    if (state === "disconnected" || state === "closed") { states.clear(); show(); }
  };
  show();
  client.connect();
}

// ---- the field tabs

function buildTabs(panels, url, unique, office) {
  const tabs = $("tabs"), holder = $("tabpanels");
  const views = [];
  const select = (which) => {
    views.forEach(({ tab, view }, i) => {
      tab.classList.toggle("on", i === which);
      view.root.hidden = i !== which;
    });
    if (office) office.show(panels[which].station);   // the machine is one long row: bring that station's levers up too
    history.replaceState(null, "", `${location.search}#${which}`);
  };
  panels.forEach((panel, i) => {
    const tab = el("button", "tab");
    const dot = el("span", "dot");
    tab.append(dot, document.createTextNode(panel.station));
    tab.addEventListener("click", () => select(i));
    tabs.appendChild(tab);
    const view = new FieldView(panel, new MqttClient({ url, clientId: `panel-field-${i}-${unique}` }));
    view.onStatus = (state) => { dot.className = `dot ${state}`; };
    holder.appendChild(view.root);
    views.push({ tab, view });
  });
  const wanted = Number(location.hash.slice(1));
  if (views.length) select(Number.isInteger(wanted) && wanted >= 0 && wanted < views.length ? wanted : 0);
}

// ---- start

async function start() {
  const url = new URLSearchParams(location.search).get("ws") || DEFAULT_BROKER;
  const unique = Math.random().toString(16).slice(2, 10);
  $("broker-url").textContent = url;

  let index, panels;
  try {
    index = await fetchJson(BUILD + "panels.json");
    panels = await Promise.all(index.stations.map((s) => fetchJson(BUILD + s.file)));
  } catch (e) {
    $("problem").hidden = false;
    $("problem").textContent = `No panel files: ${e.message}. Build them with \`make build/panels.json\` and serve the ` +
                               "repository root over HTTP (python3 -m http.server 8080, then /web/).";
    return;
  }
  if (index.notDerived.length) {
    $("not-derived").textContent = "No panel for: " +
      index.notDerived.map((n) => `${n.station} (${n.problems.length} problems; ${n.problems[0]})`).join("; ");
  }

  // Which halves this window shows, and the buttons that put a half in a window of its own.
  const view = new URLSearchParams(location.search).get("view");
  const only = view === "office" || view === "field" ? view : null;
  if (only) {
    document.body.classList.add(`${only}-only`);
    document.title = `Codeline ${only}`;
  }
  const open = (half) => {
    const query = new URLSearchParams(location.search);
    query.set("view", half);
    window.open(`${location.pathname}?${query}${half === "field" ? location.hash : ""}`, `codeline-${half}`, "popup,width=1400,height=800");
  };
  const fold = (folded) => {
    document.body.classList.toggle("field-folded", folded);
    $("field-fold").textContent = folded ? "Show here" : "Hide here";
  };
  $("office-window").addEventListener("click", () => open("office"));
  $("field-window").addEventListener("click", () => { open("field"); fold(true); });
  $("field-fold").addEventListener("click", () => fold(!document.body.classList.contains("field-folded")));

  buildClock(index, new MqttClient({ url, clientId: `panel-clock-${unique}` }));
  if (only === "field") {
    showLink($("office-link"), "code line", "not in this window", "this window shows the field only");
    buildTabs(panels, url, unique, null);
    return;
  }

  let office = null;
  const officeClient = new MqttClient({
    url, clientId: `panel-office-${unique}`,
    onStatus: (state, detail) => {
      showLink($("office-link"), "code line", state, detail);
      if (office) office.linkChanged(state === "connected" || state === "refused");
    },
  });
  office = new Office(panels, officeClient, { machine: $("machine"), traffic: $("traffic"), codeAll: $("code-all") });
  officeClient.connect();

  if (only !== "office") buildTabs(panels, url, unique, office);
}

start();
