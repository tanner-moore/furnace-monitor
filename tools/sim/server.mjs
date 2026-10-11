// Serves data/index.html with simulated furnace data, so the web page can be
// tried without a CO16. Same endpoints as src/web.cpp; the data comes from
// tools/sim/sim.cpp running the real furnace model. No npm packages needed.
//
//   node tools/sim/server.mjs [--port 8080] [--speed 1] [--seed 1] [--all-sensors] [--dirty-filter]
//
// --speed runs the simulated furnace faster than real time (up to 50x).
// --all-sensors also fits the optional room, outdoor, spare, CO and pressure sensors, with made-up
// readings. --dirty-filter, with --all-sensors, raises the filter pressure drop past the dirty limit.

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
const allSensors = process.argv.includes("--all-sensors");
const dirtyFilter = process.argv.includes("--dirty-filter");
let lastFilterDp = null;
// Made-up readings for the optional sensors: a room near 21 °C, an outdoor day/night swing,
// a basement probe and a trace of CO.
// Made-up pressures: filter drop and TESP both follow the blower. --dirty-filter loads the filter up.
const pressures = (s) => {
  const wob = (k) => Math.sin(s.time / 97 + k) * 2;
  return { p1: (s.blower ? (dirtyFilter ? 70 : 30) + s.i_blower * 3 : 0) + wob(1), p2: (s.blower ? 125 + s.i_blower * 8 : 0) + wob(2),
           p3: (s.blower ? 60 + s.i_blower * 2 : 0) + wob(3) };
};
const extras = (t) => ({
  room: 21 + 0.6 * Math.sin(t / 2700), outdoor: 6 + 5 * Math.sin(2 * Math.PI * (t / 86400 - 0.35)),
  spare: 15.5 + 0.4 * Math.sin(t / 5000), co: Math.max(0, 1.5 + Math.sin(t / 1800)),
});

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
                   inducer: s.i_inducer, blower: s.i_blower, flags: s.flags, ...extras(s.time), ...pressures(s) });
    if (history.length > HISTORY_S / 5) history.shift();
  }
  delete s.events;
  delete s.flags;
  state = { ...s, link: "eth", ip: "192.168.1.50", rssi: 0, mqtt: true };
  if (allSensors) {
    const x = extras(s.time);
    Object.assign(state, { room_fitted: true, t_room: x.room, out_fitted: true, t_out: x.outdoor, out_src: "weather",
                           spare_fitted: true, spare_name: "Basement", t_spare: x.spare, co_ppm_fitted: true, co_ppm: x.co,
                           ...pressures(s), p1_fitted: true, p2_fitted: true, p3_fitted: true, p1_name: "Filter pressure drop",
                           p2_name: "TESP", p3_name: "Pressure 3", press_unit: "inwc" });
    // The firmware keeps the last minute average with the blower running; the latest reading will do here.
    if (s.blower) lastFilterDp = state.p1;
    Object.assign(state, { filter_press: 1, filter_dp: lastFilterDp, filter_dirty_pa: CONFIG.filterDirtyPa,
                           filter_dirty: lastFilterDp != null && lastFilterDp >= CONFIG.filterDirtyPa });
  }
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
  filterLifeHours: 500, sdLogging: true, display: true, displayWidth: 240,
  displayHeight: 240, displayRotation: 0, tempUnit: "C",
  aiInducer: 1, aiBlower: 2, aiLed: 3, inducerAmpsPerVolt: 0.5, blowerAmpsPerVolt: 2, ledOnVolts: 2,
  rtdSupply: 1, rtdReturn: 2, rtdFlue: 3, rtdSpare: 0, offsetSupply: 0, offsetReturn: 0, offsetFlue: 0, offsetSpare: 0,
  boardPreset: "generic", aiCo: 0, coPpmPerVolt: 25, coAlertPpm: 35, aiRoom: 0, roomLowC: 0, roomHighC: 50,
  aiOutdoor: 0, outdoorLowC: -40, outdoorHighC: 60, weatherOn: false, weatherLat: 0, weatherLon: 0, dashLayout: "", dashChart: "",
  aiPress1: 0, aiPress2: 0, aiPress3: 0, press1Name: "Filter pressure drop", press2Name: "TESP", press3Name: "Pressure 3",
  press1LowPa: 0, press1HighPa: 249.09, press2LowPa: 0, press2HighPa: 498.18, press3LowPa: 0, press3HighPa: 249.09, pressUnit: "inwc",
  press1Type: "sdp810", press2Type: "sdp811", press3Type: "ai", filterPressSensor: 1, filterDirtyPa: 62.27,
  spareName: "Spare",
  model: { inducerOnAmps: 0.3, inducerHighAmps: 1.2, blowerOnAmps: 1, flameConfirmS: 5, ignitionTimeoutS: 90,
           shortCycleS: 180, blowerDelayS: 90, settleS: 300, minDeltaTC: 15, maxSupplyC: 85, minFlueRiseC: 10,
           ignitionTrials: 2, riseSettleS: 180 },
};

if (allSensors) Object.assign(CONFIG, { aiRoom: 5, aiOutdoor: 6, weatherOn: true, rtdSpare: 4, aiCo: 4, spareName: "Basement",
                                 aiPress1: 7, aiPress2: 8, aiPress3: 9 });

function historyJson(hours) {
  const want = Math.min(hours * 720, history.length);
  const step = Math.max(1, Math.ceil(want / 720));
  const out = { t: [], supply: [], return: [], flue: [], inducer: [], blower: [], flags: [] };
  if (allSensors) Object.assign(out, { room: [], outdoor: [], spare: [], co: [], p1: [], p2: [], p3: [] });
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
  else if (url.pathname === "/api/defaults") json(CONFIG);
  else if (url.pathname === "/api/config") text(200, "simulator: settings not saved");
  else if (url.pathname === "/api/layout" && req.method === "GET") json({ layout: CONFIG.dashLayout, chart: CONFIG.dashChart });
  else if (url.pathname === "/api/layout") {
    let body = "";
    req.on("data", (d) => (body += d));
    req.on("end", () => { const j = JSON.parse(body || "{}"); CONFIG.dashLayout = j.layout || ""; CONFIG.dashChart = j.chart || ""; text(200, "layout saved"); });
  }
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
