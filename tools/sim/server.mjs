// Serves data/index.html with simulated furnace data, so the web page can be
// tried without a CO16. Same endpoints as src/web.cpp; the data comes from
// tools/sim/sim.cpp running the real furnace model. No npm packages needed.
//
//   node tools/sim/server.mjs [--port 8080] [--speed 1] [--seed 1]
//
// --speed runs the simulated furnace faster than real time (up to 50x).

import { spawn, execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, readFileSync, statSync } from "node:fs";
import { createServer } from "node:http";
import { createInterface } from "node:readline";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, "..", "..");
const arg = (name, def) => {
  const i = process.argv.indexOf(`--${name}`);
  return i > 0 ? process.argv[i + 1] : def;
};
const port = Number(arg("port", 8080));
const speed = Math.min(50, Math.max(1, Number(arg("speed", 1))));
const seed = arg("seed", "1");

// Build the simulator when it is missing or older than its sources.
const core = join(root, "lib", "furnace_core", "src");
const sources = [join(here, "sim.cpp"), join(core, "furnace_model.cpp"), join(core, "flash_decoder.cpp")];
const bin = join(here, ".build", "sim");
const mtime = (f) => statSync(f).mtimeMs;
if (!existsSync(bin) || sources.some((s) => mtime(s) > mtime(bin))) {
  console.log("building simulator…");
  execFileSync("mkdir", ["-p", dirname(bin)]);
  execFileSync("g++", ["-std=c++17", "-O2", "-I", core, ...sources, "-o", bin], { stdio: "inherit" });
}

// --- simulator process -----------------------------------------------------

const HISTORY_S = 24 * 3600;
const sim = spawn(bin, [String(Math.floor(Date.now() / 1000) - HISTORY_S), seed], { stdio: ["pipe", "pipe", "inherit"] });
sim.on("exit", (code) => { console.error(`simulator exited (${code})`); process.exit(1); });
const lines = createInterface({ input: sim.stdout });
const waiting = [];
lines.on("line", (l) => waiting.shift()?.(JSON.parse(l)));
const next = () => new Promise((resolve) => waiting.push(resolve));

// --- board state, as monitor.cpp keeps it ------------------------------------

const history = []; // one sample every 5 s, 24 h
const events = [];  // newest last, 200 kept
let eventSeq = 0;
let state = null;

function absorb(s) {
  for (const text of s.events) {
    events.push({ seq: ++eventSeq, time: s.time, text });
    if (events.length > 200) events.shift();
  }
  if (s.uptime_s % 5 === 0) {
    history.push({ t: s.time, supply: s.t_supply, return: s.t_return, flue: s.t_flue,
                   inducer: s.i_inducer, blower: s.i_blower, flags: s.flags });
    if (history.length > HISTORY_S / 5) history.shift();
  }
  delete s.events;
  delete s.flags;
  state = { ...s, link: "eth", ip: "192.168.1.50", rssi: 0, mqtt: true };
}

async function step(n) {
  const got = Array.from({ length: n }, next);
  sim.stdin.write(`${n}\n`);
  for (const p of got) absorb(await p);
}

// --- WebSocket (server to browser text frames only) --------------------------

const clients = new Set();
function wsFrame(text) {
  const body = Buffer.from(text);
  const n = body.length;
  const head = n < 126 ? Buffer.from([0x81, n])
    : n < 65536 ? Buffer.from([0x81, 126, n >> 8, n & 255])
    : Buffer.concat([Buffer.from([0x81, 127, 0, 0, 0, 0]), Buffer.from([n >>> 24, (n >> 16) & 255, (n >> 8) & 255, n & 255])]);
  return Buffer.concat([head, body]);
}
const broadcast = () => { const f = wsFrame(JSON.stringify(state)); for (const c of clients) c.write(f); };

// --- HTTP ---------------------------------------------------------------------

const CONFIG = {
  hostname: "furnace-co16", wifiSsid: "", tz: "PST8PDT,M3.2.0,M11.1.0", ntpServer: "pool.ntp.org",
  mqttHost: "homeassistant.local", mqttPort: 1883, mqttUser: "furnace", baseTopic: "furnace/co16",
  discoveryPrefix: "homeassistant", webUser: "admin", diW1: 1, diW2: 2, diG: 4, diMvl: 5, diMvh: 6,
  aiInducer: 1, aiBlower: 2, aiLed: 3, inducerAmpsPerVolt: 0.5, blowerAmpsPerVolt: 2, ledOnVolts: 2,
  rtdSupply: 1, rtdReturn: 2, rtdFlue: 3, rtdSpare: 0, offsetSupply: 0, offsetReturn: 0, offsetFlue: 0, offsetSpare: 0,
  model: { inducerOnAmps: 0.3, inducerHighAmps: 1.2, blowerOnAmps: 1, flameConfirmS: 5, ignitionTimeoutS: 90,
           shortCycleS: 180, blowerDelayS: 90, settleS: 300, minDeltaTC: 15, maxSupplyC: 85, minFlueRiseC: 10 },
};

function historyJson(hours) {
  const want = Math.min(hours * 720, history.length);
  const step = Math.max(1, Math.ceil(want / 720));
  const out = { t: [], supply: [], return: [], flue: [], inducer: [], blower: [], flags: [] };
  for (let i = history.length - want; i < history.length; i += step) {
    for (const k of Object.keys(out)) out[k].push(history[i][k]);
  }
  out.flag_bits = "1=W1 2=W2 4=G 8=MVL 16=MVH 32=flame 64=blower";
  return out;
}

const server = createServer((req, res) => {
  const url = new URL(req.url, "http://x");
  const json = (v) => { res.writeHead(200, { "Content-Type": "application/json" }); res.end(JSON.stringify(v)); };
  const text = (code, t) => { res.writeHead(code, { "Content-Type": "text/plain" }); res.end(t); };
  if (url.pathname === "/" || url.pathname === "/index.html") {
    res.writeHead(200, { "Content-Type": "text/html" });
    res.end(readFileSync(join(root, "data", "index.html"))); // re-read so page edits show on reload
  } else if (url.pathname === "/api/state") json(state);
  else if (url.pathname === "/api/history") json(historyJson(Math.min(24, Math.max(1, Number(url.searchParams.get("hours")) || 24))));
  else if (url.pathname === "/api/events") json([...events].reverse());
  else if (url.pathname === "/api/config" && req.method === "GET") json(CONFIG);
  else if (url.pathname === "/api/config") text(200, "simulator: settings not saved");
  else if (url.pathname === "/update") { req.resume(); req.on("end", () => text(200, "simulator: update ignored")); }
  else text(404, "not found");
});

server.on("upgrade", (req, socket) => {
  if (new URL(req.url, "http://x").pathname !== "/ws") return socket.destroy();
  const accept = createHash("sha1").update(req.headers["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest("base64");
  socket.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n\r\n`);
  socket.write(wsFrame(JSON.stringify(state)));
  clients.add(socket);
  socket.on("data", (d) => { if ((d[0] & 0x0f) === 8) socket.end(); }); // close frame
  socket.on("close", () => clients.delete(socket));
  socket.on("error", () => clients.delete(socket));
});

// --- run ------------------------------------------------------------------------

console.log("simulating the last 24 hours…");
for (let done = 0; done < HISTORY_S; done += 3600) await step(3600);
sim.stdin.write("L\n"); // live from here: one of each phase and fault first
server.listen(port, () => console.log(`furnace simulator at http://localhost:${port}/ (speed ${speed}x)`));
let busy = false;
setInterval(async () => {
  if (busy) return;
  busy = true;
  await step(1);
  broadcast();
  busy = false;
}, 1000 / speed);
