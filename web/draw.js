// Draws a panel file's `drawing` (tools/trackplan.py: elements in page millimetres) as SVG.
// It decides line widths and nothing else: every colour is a CSS class the caller sets from
// what the element is bound to (`office` for the model board, `field` for the field view).

const SVG = "http://www.w3.org/2000/svg";
const WIDTH = { rail: 1.1, detail: 0.3, joint: 0.3, end: 0.5 };   // mm
const PAD = 3;                                                     // mm around the bounds
const TEXT_SCALE = 1.25;

function node(name, attributes) {
  const n = document.createElementNS(SVG, name);
  for (const [k, v] of Object.entries(attributes)) n.setAttribute(k, v);
  return n;
}

// Returns {svg, items}: items is [{node, element}] for every drawn element, in drawing order.
export function drawTrackplan(drawing, pixelsPerMm) {
  const [x0, y0, x1, y1] = drawing.bounds;
  const w = x1 - x0 + 2 * PAD, h = y1 - y0 + 2 * PAD;
  const svg = node("svg", { viewBox: `${x0 - PAD} ${y0 - PAD} ${w} ${h}`, class: "trackplan" });
  if (pixelsPerMm) {
    svg.setAttribute("width", Math.round(w * pixelsPerMm));
    svg.setAttribute("height", Math.round(h * pixelsPerMm));
  }
  const items = [];
  // Rails first, so symbols and lamps sit on top of the track.
  const ordered = [...drawing.elements].sort((a, b) => (a.role === "rail" ? 0 : 1) - (b.role === "rail" ? 0 : 1));
  for (const element of ordered) {
    let n;
    if (element.shape === "polyline") {
      n = node("polyline", { points: element.points.map((p) => p.join(",")).join(" "),
                             "stroke-width": WIDTH[element.role] || 0.3 });
    } else if (element.shape === "circle") {
      n = node("circle", { cx: element.center[0], cy: element.center[1], r: element.radius });
    } else {
      n = node("text", { x: element.at[0], y: element.at[1], "font-size": element.size * TEXT_SCALE,
                         "text-anchor": element.anchor });
      n.textContent = element.text;
    }
    const tag = element.tag || {};
    n.setAttribute("class", ["el", `role-${element.role}`, tag.kind ? `kind-${tag.kind}` : "",
                             tag.part ? `part-${tag.part}` : ""].filter(Boolean).join(" "));
    svg.appendChild(n);
    items.push({ node: n, element });
  }
  return { svg, items };
}

// The model board of the whole machine as ONE drawing: each part's trackplan squeezed into the
// width of its own lever columns, all parts at one vertical scale and shifted so that track
// running from one part into the next meets at the same level.
//   parts: [{drawing, columns, mapY (mm -> mm, the part's lanes), shift (mm, added after mapY), label}]   unit: SVG units per lever column
// Returns {svg, parts: [{group, label, items}]}, items as drawTrackplan's.
export function drawBoard(parts, unit) {
  const TOP = 0.16 * unit, BOTTOM = 0.04 * unit;                // room for the names above the track
  const scales = parts.map((p) => (p.columns * unit) / (p.drawing.bounds[2] - p.drawing.bounds[0]));
  const sy = [...scales].sort((a, b) => a - b)[Math.floor(scales.length / 2)] || 1;
  const top = Math.min(...parts.map((p) => p.mapY(p.drawing.bounds[1]) + p.shift));
  const bottom = Math.max(...parts.map((p) => p.mapY(p.drawing.bounds[3]) + p.shift));
  const columns = parts.reduce((n, p) => n + p.columns, 0);
  const height = (bottom - top) * sy + TOP + BOTTOM;
  const svg = node("svg", { viewBox: `0 0 ${columns * unit} ${height}`, class: "trackplan board" });
  const out = [];
  let first = 0;
  parts.forEach((part, k) => {
    const sx = scales[k], x0 = part.drawing.bounds[0], left = first * unit;
    const X = (x) => +(left + (x - x0) * sx).toFixed(2);
    const Y = (y) => +((part.mapY(y) + part.shift - top) * sy + TOP).toFixed(2);
    const mm = Math.min(sx, sy);
    const group = node("g", { class: "board-part" });
    const label = node("text", { x: left + (part.columns * unit) / 2, y: 0.1 * unit, "text-anchor": "middle",
                                 "font-size": 0.085 * unit, class: "board-name" });
    label.textContent = part.label;
    group.appendChild(label);
    const items = [];
    const ordered = [...part.drawing.elements].sort((a, b) => (a.role === "rail" ? 0 : 1) - (b.role === "rail" ? 0 : 1));
    for (const element of ordered) {
      let n;
      if (element.shape === "polyline") {
        n = node("polyline", { points: element.points.map((p) => `${X(p[0])},${Y(p[1])}`).join(" "),
                               "stroke-width": (WIDTH[element.role] || 0.3) * mm * (element.role === "rail" ? 1.6 : 1) });
      } else if (element.shape === "circle") {
        n = node("circle", { cx: X(element.center[0]), cy: Y(element.center[1]), r: element.radius * mm * 1.3 });
      } else {
        n = node("text", { x: X(element.at[0]), y: Y(element.at[1]), "font-size": Math.max(element.size * mm, 0.06 * unit),
                           "text-anchor": element.anchor });
        n.textContent = element.text;
      }
      const tag = element.tag || {};
      n.setAttribute("class", ["el", `role-${element.role}`, tag.kind ? `kind-${tag.kind}` : "",
                               tag.part ? `part-${tag.part}` : ""].filter(Boolean).join(" "));
      group.appendChild(n);
      items.push({ node: n, element });
    }
    svg.appendChild(group);
    out.push({ group, label, items, at: (p) => [X(p[0]), Y(p[1])], mm });
    first += part.columns;
  });
  // A track that leaves one station to the right on a wire label and enters the next from the left
  // on the same label is one track: draw the piece between the two drawings. (Main track labels
  // repeat on every sheet, so the direction an end faces is part of the match.)
  const ends = (part, facing) => part.drawing.elements.flatMap((e) => (e.labels || []).map((label, i) => {
    const point = i ? e.points[e.points.length - 1] : e.points[0], other = i ? e.points[0] : e.points[e.points.length - 1];
    return label && Math.sign(point[0] - other[0]) === facing ? { label, element: e, point } : null;
  }).filter(Boolean));
  const links = [];
  for (let k = 1; k < parts.length; k++) {
    for (const a of ends(parts[k - 1], 1)) {
      for (const b of ends(parts[k], -1)) {
        if (a.label !== b.label) continue;
        const n = node("polyline", { points: [out[k - 1].at(a.point), out[k].at(b.point)].map((p) => p.join(",")).join(" "),
                                     "stroke-width": (WIDTH.rail || 0.3) * Math.min(out[k - 1].mm, out[k].mm) * 1.6 });
        n.setAttribute("class", "el role-rail link");
        const title = node("title", {});
        title.textContent = a.label;
        n.appendChild(title);
        svg.appendChild(n);
        links.push({ node: n, label: a.label, from: { part: k - 1, element: a.element }, to: { part: k, element: b.element } });
      }
    }
  }
  return { svg, parts: out, links };
}

// Sets exactly the given state classes on a drawn element, keeping its structural ones.
export function setLook(item, looks) {
  const keep = [...item.node.classList].filter((c) => c === "el" || c.startsWith("role-") ||
                                                      c.startsWith("kind-") || c.startsWith("part-"));
  item.node.setAttribute("class", keep.concat(looks).join(" "));
}

export function setTitle(item, text) {
  let t = item.node.querySelector("title");
  if (!t) {
    t = document.createElementNS(SVG, "title");
    item.node.appendChild(t);
  }
  if (t.textContent !== text) t.textContent = text;
}
