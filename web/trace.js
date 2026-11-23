// Where the switches of an interlocking lead: from every track that enters the drawing at its
// left or right edge, follow the rail inward, taking each facing switch the way it stands, until
// the path leaves the drawing or meets points set against it. A path that stops at such points
// is dead: no signal could be clear over it, so it is drawn only as far as the last signal it
// passed that governs its direction (the train would be standing there), not up to the points.
// Two trains lined into different tracks for a meet show as two paths.
//
// This is a picture of how the switches stand, made from the drawing and the switch positions
// alone. It is NOT a route and says nothing about any signal.
//
// The drawing has no connectivity, so it is found from the rails themselves: the three legs of a
// switch (tag.part C, N, R) meet inside its symbol, and any two rail ends within JOIN_MM of each
// other are joined (a joint or signal symbol leaves a gap that wide between two pieces).

const JOIN_MM = 2.7;
const EDGE_MM = 2;
const LEVEL_MM = 0.01;
const MAST_ALONG_MM = 1.3;      // a mast's base stands at the gap between two rails, this close along the track
const MAST_ASIDE_MM = 7;        // and this far to one side of it at most

const distance = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1]);

// elements: a drawing's elements. Returns the graph trace() walks.
//   rails: [{index (into elements), ends: [point, point], switch: name | null, part: C | N | R | null,
//            inner: 0 | 1 | null (the end inside the switch symbol), joins: [[rail, end], ...] per end}]
//            masts: [[dir, ...], [dir, ...]] per end: a signal stands at the gap beyond that end and
//            governs travel toward +x (dir 1) or -x (dir -1)}]
//   entries: [[rail, end]]  the ends that leave the drawing: those carrying a wire label, and the
//            ends of level track at the left or right edge of the drawing
export function trackGraph(drawing) {
  const rails = [];
  drawing.elements.forEach((e, index) => {
    if (e.shape !== "polyline" || e.role !== "rail" || e.points.length < 2) return;
    const tag = e.tag || {};
    const leg = tag.kind === "switch" && ["C", "N", "R"].includes(tag.part);
    rails.push({ index, ends: [e.points[0], e.points[e.points.length - 1]], switch: leg ? tag.name : null,
                 part: leg ? tag.part : null, inner: null, joins: [[], []], masts: [[], []],
                 labels: e.labels || [null, null],
                 level: Math.abs(e.points[0][1] - e.points[e.points.length - 1][1]) < LEVEL_MM });
  });
  // The inner end of each switch leg: N and R point at the C leg, C points at the N leg.
  const legs = new Map();
  for (const rail of rails) {
    if (!rail.switch) continue;
    if (!legs.has(rail.switch)) legs.set(rail.switch, {});
    legs.get(rail.switch)[rail.part] = rail;
  }
  const nearer = (rail, other) => {
    const d = (end) => Math.min(distance(rail.ends[end], other.ends[0]), distance(rail.ends[end], other.ends[1]));
    return d(0) <= d(1) ? 0 : 1;
  };
  for (const [name, set] of legs) {
    if (!set.C || !set.N || !set.R) {                 // not a whole switch: its legs are plain track
      for (const rail of Object.values(set)) { rail.switch = null; rail.part = null; }
      legs.delete(name);
      continue;
    }
    set.C.inner = nearer(set.C, set.N);
    set.N.inner = nearer(set.N, set.C);
    set.R.inner = nearer(set.R, set.C);
  }
  // Join every free end (not the inside of a switch) to the free ends of other rails close by.
  const free = [];
  rails.forEach((rail, r) => { for (const end of [0, 1]) if (rail.inner !== end) free.push([r, end]); });
  for (const [r, end] of free) {
    for (const [q, other] of free) {
      if (q !== r && distance(rails[r].ends[end], rails[q].ends[other]) <= JOIN_MM) rails[r].joins[end].push([q, other]);
    }
  }
  // Masts. The symbol's base (a bar across the pole, level with the heads) stands beside the gap
  // the signal governs, and its heads point the way the governed train runs.
  const masts = new Map();
  for (const e of drawing.elements) {
    if (!e.tag || e.tag.kind !== "mast") continue;
    if (!masts.has(e.tag.name)) masts.set(e.tag.name, { heads: [], bars: [] });
    if (e.shape === "circle") masts.get(e.tag.name).heads.push(e.center);
    else if (e.shape === "polyline" && e.points.length === 2 && e.points[0][0] === e.points[1][0]) masts.get(e.tag.name).bars.push(e.points);
  }
  for (const { heads, bars } of masts.values()) {
    if (!heads.length) continue;
    const bar = bars.find((b) => Math.abs((b[0][1] + b[1][1]) / 2 - heads[0][1]) < LEVEL_MM);
    if (!bar) continue;
    const at = [bar[0][0], heads[0][1]];
    const dir = Math.sign(heads[0][0] - at[0]);
    let best = null;
    for (const [r, end] of free) {
      for (const [q, other] of rails[r].joins[end]) {
        const a = rails[r].ends[end], b = rails[q].ends[other];
        const along = Math.abs((a[0] + b[0]) / 2 - at[0]), aside = Math.abs((a[1] + b[1]) / 2 - at[1]);
        if (along <= MAST_ALONG_MM && aside <= MAST_ASIDE_MM && (!best || aside < best.aside)) best = { r, end, q, other, aside };
      }
    }
    if (best && dir) { rails[best.r].masts[best.end].push(dir); rails[best.q].masts[best.other].push(dir); }
  }
  const [x0, , x1] = drawing.bounds;
  const entries = free.filter(([r, end]) => !rails[r].joins[end].length && (rails[r].labels[end] ||
    (rails[r].level && (rails[r].ends[end][0] - x0 <= EDGE_MM || x1 - rails[r].ends[end][0] <= EDGE_MM))));
  return { rails, legs, entries };
}

// position(name) -> "N" | "R" | null (not known: a path stops there).
// Returns the Set of element indexes of every rail on a path from an entry. A path that ends at
// points set against it counts only up to the last signal it passed that governs its direction;
// with no such signal, or at points whose position is not known, it counts as far as it got.
export function tracePaths(graph, position) {
  const lit = new Set();
  const path = [];                                     // element indexes, entry first
  const on = new Set();                                // (rail, end) entered on the path being walked
  const keep = (count) => { for (const index of path.slice(0, count)) lit.add(index); };
  const walk = (r, from, mast) => {                    // enter rail r at its end `from`; mast: path length at the last signal
    const key = r * 2 + from;
    if (on.has(key)) { keep(path.length); return; }    // a loop: the path is whole
    on.add(key);
    const rail = graph.rails[r];
    path.push(rail.index);
    const out = 1 - from;
    if (rail.inner !== out) {                          // came out at a free end: on into whatever joins it
      if (!rail.joins[out].length) keep(path.length);  // left the drawing, or the end of the track
      for (const [q, end] of rail.joins[out]) {
        const dir = Math.sign(graph.rails[q].ends[end][0] - rail.ends[out][0]);
        walk(q, end, dir && rail.masts[out].includes(dir) ? path.length : mast);
      }
    } else {                                           // came to the inside of a switch
      const set = graph.legs.get(rail.switch);
      const stands = position(rail.switch);
      let next = null;
      if (rail.part === "C") next = stands === "N" ? set.N : stands === "R" ? set.R : null;   // facing: the way they stand
      else if (stands === rail.part) next = set.C;     // trailing, and the points are set for this leg
      if (next) walk(graph.rails.indexOf(next), next.inner, mast);
      else if (stands === null || mast === null) keep(path.length);   // not known, or no signal passed: as far as it got
      else keep(mast);                                 // set against it: dead, back to the signal
    }
    path.pop();
    on.delete(key);
  };
  for (const [r, end] of graph.entries) walk(r, end, null);
  return lit;
}
