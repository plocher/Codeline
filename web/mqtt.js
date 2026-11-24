// A small MQTT 3.1.1 client over WebSocket (subprotocol "mqtt"). No library, no build step.
//
//   const c = new MqttClient({ url, clientId, onStatus });
//   c.subscribe(filter, qos, (topic, payload, retained) => ...);   // filter may use + and #
//   c.publish(topic, payload);          // QoS 0, unretained; false (and nothing sent) when not connected
//   c.connect();   c.close();
//
// onStatus(state, detail): connecting | connected | disconnected | closed | refused.
// `refused` means connected, but the broker refused a subscription (detail names the filter).
//
// What it does: CONNECT with a clean session, SUBSCRIBE (again after every reconnect), receives
// PUBLISH at QoS 0 and 1 (answers QoS 1 with PUBACK) and passes the retain flag on, sends PUBLISH
// at QoS 0 only, PINGREQ, and reconnects with a backoff that starts at 1 s and doubles to 30 s.
// A connection that gets no CONNACK in time, a broker that goes silent, and bytes that are not
// MQTT all end the same way: the socket is given up and the backoff runs.
//
// Keep-alive is long (120 s), so a browser that throttles the timers of a background tab does
// not lose the connection. A ping goes out every 60 s whatever else is sent, and any answer must
// follow it: sending publishes is no sign the broker hears them, so it does not put the ping off.
// When the page becomes visible again the client probes at once and reconnects if the broker
// does not answer.
//
// What it does not do, on purpose: it never queues a publish while disconnected (a code line
// control must not be sent late; docs/bindings/CodeLineMQTT.md section 5), has no will, no QoS 2,
// no persistent session, no authentication.

const KEEPALIVE_S = 120;
const PING_EVERY_MS = (KEEPALIVE_S * 1000) / 2;
const PROBE_WAIT_MS = 5000;                        // for any answer to a ping sent to test the link
const CONNACK_WAIT_MS = 5000;
const BACKOFF_FIRST_MS = 1000;
const BACKOFF_MAX_MS = 30000;
const SUBSCRIPTION_REFUSED = 0x80;

const encoder = new TextEncoder();
const decoder = new TextDecoder("utf-8");

function utf8Field(text) {
  const bytes = encoder.encode(text);
  return [bytes.length >> 8, bytes.length & 0xff, ...bytes];
}

function packet(firstByte, body) {
  const head = [firstByte];
  let n = body.length;
  do {
    let digit = n % 128;
    n = Math.floor(n / 128);
    if (n > 0) digit |= 0x80;
    head.push(digit);
  } while (n > 0);
  const out = new Uint8Array(head.length + body.length);
  out.set(head, 0);
  out.set(body, head.length);
  return out;
}

// Does a subscription filter match a topic? (+ one level, # the rest; MQTT 3.1.1 section 4.7.
// A filter that starts with a wildcard does not match a topic that starts with $.)
export function topicMatches(filter, topic) {
  const f = filter.split("/");
  const t = topic.split("/");
  if (topic.startsWith("$") && (f[0] === "#" || f[0] === "+")) return false;
  for (let i = 0; i < f.length; i++) {
    if (f[i] === "#") return true;
    if (i >= t.length) return false;
    if (f[i] !== "+" && f[i] !== t[i]) return false;
  }
  return f.length === t.length;
}

export class MqttClient {
  // waits: {connackMs, probeMs, pingMs} override the waits (scenarios use short ones).
  constructor({ url, clientId, onStatus, waits }) {
    this.url = url;
    this.clientId = clientId;
    this.onStatus = onStatus || (() => {});
    this.connackMs = (waits && waits.connackMs) || CONNACK_WAIT_MS;
    this.probeMs = (waits && waits.probeMs) || PROBE_WAIT_MS;
    this.pingMs = (waits && waits.pingMs) || PING_EVERY_MS;
    this.subscriptions = [];          // {filter, qos, handler}
    this.pendingSubscribes = new Map();   // packet id -> [filter]
    this.connected = false;
    this.closed = true;
    this.socket = null;
    this.buffer = new Uint8Array(0);
    this.nextId = 1;
    this.backoffMs = BACKOFF_FIRST_MS;
    this.retryTimer = null;
    this.pingTimer = null;
    this.probeTimer = null;
    this.connackTimer = null;
    this.lastHeard = 0;
    if (typeof document !== "undefined" && document.addEventListener) {
      document.addEventListener("visibilitychange", () => { if (!document.hidden) this.wake(); });
    }
  }

  connect() {
    this.closed = false;
    this.open();
  }

  close() {
    this.closed = true;
    clearTimeout(this.retryTimer);
    if (this.socket) {
      if (this.connected) this.send(packet(0xe0, []));   // DISCONNECT
      const socket = this.socket;
      this.socket = null;
      try { socket.close(); } catch (e) { /* already gone */ }
      this.dropped("closed");
    } else {
      this.status("closed");
    }
  }

  subscribe(filter, qos, handler) {
    this.subscriptions.push({ filter, qos, handler });
    if (this.connected) this.sendSubscribe([{ filter, qos }]);
  }

  // QoS 0, unretained. Returns false, sending nothing, when there is no connection.
  publish(topic, payload) {
    if (!this.connected) return false;
    this.send(packet(0x30, [...utf8Field(topic), ...encoder.encode(payload)]));
    return true;
  }

  // ---- connection

  open() {
    this.status("connecting");
    let socket;
    try {
      socket = new WebSocket(this.url, "mqtt");
    } catch (e) {
      this.retry(String(e));
      return;
    }
    this.socket = socket;
    this.buffer = new Uint8Array(0);
    socket.binaryType = "arraybuffer";
    clearTimeout(this.connackTimer);
    this.connackTimer = setTimeout(() => {
      if (socket === this.socket && !this.connected) this.abandon("no answer from the broker");
    }, this.connackMs);
    socket.onopen = () => {
      if (socket !== this.socket) return;
      // CONNECT: protocol "MQTT" level 4, clean session, keep-alive, client id.
      this.send(packet(0x10, [...utf8Field("MQTT"), 4, 0x02, KEEPALIVE_S >> 8, KEEPALIVE_S & 0xff,
                              ...utf8Field(this.clientId)]));
    };
    socket.onmessage = (event) => {
      if (socket !== this.socket) return;
      this.lastHeard = Date.now();
      this.receive(new Uint8Array(event.data));
    };
    socket.onclose = () => {
      if (socket !== this.socket) return;
      this.socket = null;
      this.dropped("connection closed");
    };
    socket.onerror = () => {};         // onclose follows
  }

  // Give this socket up now (a close handshake with a dead peer can take a long time) and retry.
  abandon(why) {
    const socket = this.socket;
    if (!socket) return;
    this.socket = null;                 // its late events are ignored (see open())
    try { socket.close(); } catch (e) { /* already gone */ }
    this.dropped(why);
  }

  dropped(why) {
    clearTimeout(this.pingTimer);
    clearTimeout(this.probeTimer);
    clearTimeout(this.connackTimer);
    this.pendingSubscribes.clear();
    this.connected = false;
    if (this.closed) {
      this.status("closed");
      return;
    }
    this.retry(why);
  }

  retry(why) {
    const wait = this.backoffMs;
    this.backoffMs = Math.min(this.backoffMs * 2, BACKOFF_MAX_MS);
    this.status("disconnected", `${why}; next try in ${wait / 1000} s`);
    clearTimeout(this.retryTimer);
    this.retryTimer = setTimeout(() => { if (!this.closed && !this.socket) this.open(); }, wait);
  }

  status(state, detail) {
    try {
      this.onStatus(state, detail || "");
    } catch (e) {
      console.error("mqtt: status handler failed", e);
    }
  }

  send(bytes) {
    if (!this.socket || this.socket.readyState !== 1) return;
    this.socket.send(bytes);
  }

  sendSubscribe(list) {
    const id = this.nextId;
    this.nextId = (this.nextId % 0xffff) + 1;
    const body = [id >> 8, id & 0xff];
    for (const { filter, qos } of list) body.push(...utf8Field(filter), qos);
    this.pendingSubscribes.set(id, list.map((s) => s.filter));
    this.send(packet(0x82, body));
  }

  // ---- keep-alive

  schedulePing() {
    clearTimeout(this.pingTimer);
    this.pingTimer = setTimeout(() => this.probe(), this.pingMs);
  }

  // Send a PINGREQ and expect to hear anything at all within the probe wait.
  probe() {
    if (!this.connected) return;
    this.schedulePing();
    const asked = Date.now();
    this.send(packet(0xc0, []));
    clearTimeout(this.probeTimer);
    this.probeTimer = setTimeout(() => {
      if (this.connected && this.lastHeard < asked) this.abandon("the broker went silent");
    }, this.probeMs);
  }

  // The page is visible again: timers may have slept. Test the link now; hurry a pending retry.
  wake() {
    if (this.closed) return;
    if (this.connected) {
      if (!this.socket || this.socket.readyState !== 1) this.abandon("the connection died in the background");
      else this.probe();
    } else if (!this.socket) {
      clearTimeout(this.retryTimer);
      this.open();
    }
  }

  // ---- receiving

  receive(chunk) {
    const joined = new Uint8Array(this.buffer.length + chunk.length);
    joined.set(this.buffer, 0);
    joined.set(chunk, this.buffer.length);
    const packets = [];
    let at = 0;
    for (;;) {
      // Fixed header: one byte, then the remaining length in 1 to 4 bytes.
      let length = 0, shift = 0, i = at + 1, complete = false, digits = 0;
      while (i < joined.length && digits < 4) {
        const digit = joined[i++];
        digits++;
        length += (digit & 0x7f) << shift;
        shift += 7;
        if (!(digit & 0x80)) { complete = true; break; }
      }
      if (!complete && digits === 4) {                 // a fifth length byte: this is not MQTT
        this.buffer = new Uint8Array(0);
        this.abandon("protocol error: malformed remaining length");
        return;
      }
      if (!complete || i + length > joined.length) break;
      packets.push([joined[at], joined.slice(i, i + length)]);
      at = i + length;
    }
    // The buffer is settled before anything is dispatched: a failure below loses one message,
    // never the bytes behind it.
    this.buffer = joined.slice(at);
    const socket = this.socket;
    for (const [first, body] of packets) {
      if (socket !== this.socket) return;              // given up while dispatching
      try {
        this.handle(first, body);
      } catch (e) {
        console.error("mqtt: a packet could not be handled", e);
      }
    }
  }

  handle(first, body) {
    const type = first >> 4;
    if (type === 2) {                                   // CONNACK
      clearTimeout(this.connackTimer);
      if (body[1] !== 0) {
        this.abandon(`the broker refused the connection (code ${body[1]})`);
        return;
      }
      this.connected = true;
      this.backoffMs = BACKOFF_FIRST_MS;
      this.status("connected");
      if (this.subscriptions.length) this.sendSubscribe(this.subscriptions);
      this.schedulePing();
    } else if (type === 3) {                            // PUBLISH
      const qos = (first >> 1) & 3;
      const retained = (first & 1) === 1;
      const topicLength = (body[0] << 8) | body[1];
      const topic = decoder.decode(body.subarray(2, 2 + topicLength));
      let at = 2 + topicLength;
      if (qos > 0) {
        this.send(packet(0x40, [body[at], body[at + 1]]));   // PUBACK
        at += 2;
      }
      const payload = decoder.decode(body.subarray(at));
      for (const s of this.subscriptions) {
        if (!topicMatches(s.filter, topic)) continue;
        try {
          s.handler(topic, payload, retained);
        } catch (e) {
          console.error(`mqtt: the handler for ${s.filter} failed on ${topic}`, e);
        }
      }
    } else if (type === 9) {                            // SUBACK
      const id = (body[0] << 8) | body[1];
      const filters = this.pendingSubscribes.get(id) || [];
      this.pendingSubscribes.delete(id);
      const refused = filters.filter((f, i) => body[2 + i] === SUBSCRIPTION_REFUSED);
      if (refused.length) this.status("refused", `the broker refused the subscription to ${refused.join(", ")}`);
    }
    // PINGRESP: hearing it is enough (lastHeard).
  }
}
