import http from "node:http";
import crypto from "node:crypto";
import net from "node:net";
import fs from "node:fs";
import path from "node:path";
import { WebSocketServer } from "ws";

const LISTEN_PORT = parseInt(process.env.LISTEN_PORT || "8080", 10);

const AGENT_HOST = process.env.AGENT_HOST || "127.0.0.1";
const AGENT_CTRL_PORT = parseInt(process.env.AGENT_CTRL_PORT || "31235", 10);
const AGENT_TOKEN = process.env.AGENT_TOKEN || "";

const STREAM_NAME = process.env.STREAM_NAME || "mystream";
const MTX_HTTP_PORT = parseInt(process.env.MTX_HTTP_PORT || "8889", 10);
const MTX_HOST = process.env.MTX_HOST || "127.0.0.1";

const CTRL_REF_W = parseInt(process.env.CTRL_REF_W || "0", 10);
const CTRL_REF_H = parseInt(process.env.CTRL_REF_H || "0", 10);

// Session / on-demand streaming (optional).
const TOKEN_REQUIRED = parseInt(process.env.TOKEN_REQUIRED || "0", 10) ? 1 : 0;
const TOKEN_TTL_SEC = parseInt(process.env.TOKEN_TTL_SEC || "300", 10);
const TOKEN_ISSUE_SECRET = process.env.TOKEN_ISSUE_SECRET || ""; // If set, require header X-Token-Secret
const SINGLE_SESSION = parseInt(process.env.SINGLE_SESSION || "0", 10) ? 1 : 0;
// If enabled, an incoming WS connection with missing/invalid token will trigger issuing a fresh token
// (sent only via webhook) and the WS will be rejected with error=need_token.
const TOKEN_ISSUE_ON_WS_CONNECT = parseInt(process.env.TOKEN_ISSUE_ON_WS_CONNECT || "0", 10) ? 1 : 0;
// If enabled, consume token after a successful WS auth so reconnect requires a new token.
const TOKEN_SINGLE_USE = parseInt(process.env.TOKEN_SINGLE_USE || "0", 10) ? 1 : 0;
// If >0, tokens are issued/validated per client IP and rate-limited to "one token per IP per window".
// This keeps the logic self-consistent: same IP within 5 minutes gets the same token (no infinite rotation).
const TOKEN_PER_IP_WINDOW_SEC = parseInt(process.env.TOKEN_PER_IP_WINDOW_SEC || "300", 10);

const ONDEMAND_ENABLE = parseInt(process.env.ONDEMAND_ENABLE || "0", 10) ? 1 : 0;
const ONDEMAND_CONTAINERS = (process.env.ONDEMAND_CONTAINERS || "").split(",").map((s) => s.trim()).filter(Boolean);
const ONDEMAND_IDLE_TIMEOUT_SEC = parseInt(process.env.ONDEMAND_IDLE_TIMEOUT_SEC || "60", 10);
const DOCKER_SOCK = process.env.DOCKER_SOCK || "/var/run/docker.sock";

const TOKEN_WEBHOOK_URL = process.env.TOKEN_WEBHOOK_URL || ""; // optional, JSON POST
const TOKEN_WEBHOOK_AUTH = process.env.TOKEN_WEBHOOK_AUTH || ""; // optional, sent as Authorization header
const TOKEN_WEBHOOK_FORMAT = (process.env.TOKEN_WEBHOOK_FORMAT || "raw").trim(); // raw | dingTalk
const TOKEN_WEBHOOK_DING_TYPE = process.env.TOKEN_WEBHOOK_DING_TYPE || "common";
const TOKEN_WEBHOOK_DING_FLUSHNOW = parseInt(process.env.TOKEN_WEBHOOK_DING_FLUSHNOW || "1", 10) ? true : false;
const TOKEN_WEBHOOK_DEBUG = parseInt(process.env.TOKEN_WEBHOOK_DEBUG || "0", 10) ? 1 : 0;
const TOKEN_WEBHOOK_RETRY_MIN_SEC = parseInt(process.env.TOKEN_WEBHOOK_RETRY_MIN_SEC || "5", 10);

// Admin endpoints (best-effort, intended for private ops usage).
// If ADMIN_SECRET is not set, fall back to TOKEN_ISSUE_SECRET. If neither is set, admin endpoints are disabled.
const ADMIN_SECRET = process.env.ADMIN_SECRET || "";

// IDR strategy:
// - On viewer connect, send a small burst of REQ_IDR to improve late-join experience.
// - Optionally send periodic REQ_IDR as a safety net if the encoder uses a very long GOP.
const REQ_IDR_ON_CONNECT_BURST = parseInt(process.env.REQ_IDR_ON_CONNECT_BURST || "3", 10);
const REQ_IDR_BURST_INTERVAL_MS = parseInt(process.env.REQ_IDR_BURST_INTERVAL_MS || "500", 10);
const REQ_IDR_PERIOD_SEC = parseFloat(process.env.REQ_IDR_PERIOD_SEC || "0");

const ML_CTRL_MAGIC = 0x4d4c4354; // "MLCT"
const ML_CTRL_VERSION = 1;
const ML_CTRL_CMD_MOUSE_ABS = 1;
const ML_CTRL_CMD_MOUSE_BUTTON = 3;
const ML_CTRL_CMD_MOUSE_SCROLL = 5;
const ML_CTRL_CMD_MOUSE_HSCROLL = 6;
const ML_CTRL_CMD_KEY_PRESS = 8;
const ML_CTRL_CMD_TEXT = 9;
const ML_CTRL_CMD_REQ_IDR = 10;

const BUTTON_ACTION_PRESS = 0x07;
const BUTTON_ACTION_RELEASE = 0x08;
const BUTTON_LEFT = 0x01;
const BUTTON_MIDDLE = 0x02;
const BUTTON_RIGHT = 0x03;

const MODIFIER_SHIFT = 0x01;
const MODIFIER_CTRL = 0x02;
const MODIFIER_ALT = 0x04;
const MODIFIER_META = 0x08;

// macOS: many users expect Command-based shortcuts to work against a Windows host.
// If enabled, map META (Command) to CTRL when CTRL isn't already pressed.
const MAP_MAC_META_TO_CTRL = parseInt(process.env.MAP_MAC_META_TO_CTRL || "1", 10) ? 1 : 0;

function clampI8(v) {
  if (v < -127) return -127;
  if (v > 127) return 127;
  return v | 0;
}

function vkFromKey(key) {
  // Best-effort mapping to Win32 VK (US layout).
  if (!key) return 0;

  // Special keys
  const special = {
    Escape: 0x1b,
    Backspace: 0x08,
    Tab: 0x09,
    Enter: 0x0d,
    ArrowLeft: 0x25,
    ArrowUp: 0x26,
    ArrowRight: 0x27,
    ArrowDown: 0x28,
    Delete: 0x2e,
    Insert: 0x2d,
    Home: 0x24,
    End: 0x23,
    PageUp: 0x21,
    PageDown: 0x22,
    " ": 0x20
  };
  if (special[key]) return special[key];

  // Single-char letters/digits
  if (key.length === 1) {
    const c = key;
    const o = c.charCodeAt(0);
    if (o >= 0x61 && o <= 0x7a) return o - 0x20; // a-z -> A-Z
    if ((o >= 0x41 && o <= 0x5a) || (o >= 0x30 && o <= 0x39)) return o;
    const oem = {
      "-": 0xbd,
      "=": 0xbb,
      "[": 0xdb,
      "]": 0xdd,
      "\\": 0xdc,
      ";": 0xba,
      "'": 0xde,
      ",": 0xbc,
      ".": 0xbe,
      "/": 0xbf,
      "`": 0xc0
    };
    return oem[c] || 0;
  }

  return 0;
}

function packCmd({ type, a = 0, b = 0, c = 0, d = 0, seq = 0n }) {
  const hdr = Buffer.alloc(32);
  hdr.writeUInt32LE(ML_CTRL_MAGIC, 0);
  hdr.writeUInt16LE(ML_CTRL_VERSION, 4);
  hdr.writeUInt16LE(type & 0xffff, 6);
  hdr.writeInt32LE(a | 0, 8);
  hdr.writeInt32LE(b | 0, 12);
  hdr.writeInt32LE(c | 0, 16);
  hdr.writeInt32LE(d | 0, 20);
  hdr.writeBigUInt64LE(seq, 24);
  return hdr;
}

function framePacket(payload) {
  const len = Buffer.alloc(4);
  len.writeUInt32BE(payload.length, 0);
  return Buffer.concat([len, payload]);
}

function serveStatic(req, res) {
  const root = path.join(process.cwd(), "web");
  const url = new URL(req.url || "/", `http://${req.headers.host || "localhost"}`);

  let p = url.pathname;
  if (p === "/") p = "/index.html";
  if (p.includes("..")) {
    res.writeHead(400);
    res.end("bad path");
    return;
  }

  const file = path.join(root, p);
  if (!file.startsWith(root)) {
    res.writeHead(400);
    res.end("bad path");
    return;
  }
  if (!fs.existsSync(file) || !fs.statSync(file).isFile()) {
    res.writeHead(404);
    res.end("not found");
    return;
  }
  const ext = path.extname(file);
  const ct =
    ext === ".html"
      ? "text/html; charset=utf-8"
      : ext === ".js"
        ? "text/javascript; charset=utf-8"
        : ext === ".css"
          ? "text/css; charset=utf-8"
          : "application/octet-stream";
  res.writeHead(200, { "content-type": ct });
  fs.createReadStream(file).pipe(res);
}

function forwardToMtxWhep(req, res) {
  // Proxy WHEP via ws_ctrl_bridge to avoid cross-origin/CORS issues (ws_ctrl_bridge:34567 -> mediamtx:34569).
  // Client:
  //   POST /whep?stream=mystream  (body: SDP offer)
  // Upstream:
  //   POST http://127.0.0.1:${MTX_HTTP_PORT}/mystream/whep
  const url = new URL(req.url || "/whep", `http://${req.headers.host || "localhost"}`);
  const stream = (url.searchParams.get("stream") || STREAM_NAME).trim();
  if (!/^[A-Za-z0-9._-]+$/.test(stream)) {
    res.writeHead(400, { "content-type": "text/plain; charset=utf-8" });
    res.end("bad stream");
    return;
  }

  const opts = {
    host: MTX_HOST,
    port: MTX_HTTP_PORT,
    method: "POST",
    path: `/${encodeURIComponent(stream)}/whep`,
    headers: {
      "content-type": req.headers["content-type"] || "application/sdp",
      accept: "application/sdp"
    }
  };

  const upstream = http.request(opts, (up) => {
    const chunks = [];
    up.on("data", (c) => chunks.push(c));
    up.on("end", () => {
      const body = Buffer.concat(chunks);
      res.writeHead(up.statusCode || 502, {
        "content-type": up.headers["content-type"] || "application/sdp",
        "cache-control": "no-store"
      });
      res.end(body);
    });
  });

  upstream.on("error", (e) => {
    res.writeHead(502, { "content-type": "text/plain; charset=utf-8" });
    res.end(`whep proxy error: ${e?.message || String(e)}`);
  });

  req.pipe(upstream);
}

async function readAll(req) {
  const chunks = [];
  for await (const c of req) chunks.push(c);
  return Buffer.concat(chunks);
}

function json(res, code, obj) {
  const body = Buffer.from(JSON.stringify(obj, null, 2), "utf-8");
  res.writeHead(code, {
    "content-type": "application/json; charset=utf-8",
    "cache-control": "no-store"
  });
  res.end(body);
}

function adminAuthed(req) {
  const want = ADMIN_SECRET || TOKEN_ISSUE_SECRET;
  if (!want) return false;
  const got = (req.headers["x-admin-secret"] || "").toString();
  return got === want;
}

function nowMs() {
  return Date.now();
}

function genToken() {
  // 4-digit numeric token (0000-9999).
  const n = crypto.randomInt(0, 10000);
  return String(n).padStart(4, "0");
}

let activeWs = null;
let idleStopTimer = null;
// Treat token gating as required when any "on-demand" control is enabled.
// This avoids accidental "no token but still starts containers / controls host" when envs are partially set.
const effectiveTokenRequired = (TOKEN_REQUIRED || TOKEN_ISSUE_ON_WS_CONNECT || ONDEMAND_ENABLE) ? 1 : 0;

/** @type {Map<string, {token: string, expMs: number, issuedMs: number}>} */
const tokenByIp = new Map();
/** @type {Map<string, number>} */
const lastTokenIssueAttemptMsByIp = new Map();

// Brute-force protection for token attempts (in-memory, best-effort).
const BAD_TOKEN_WINDOW_SEC = parseInt(process.env.BAD_TOKEN_WINDOW_SEC || "60", 10);
const BAD_TOKEN_MAX = parseInt(process.env.BAD_TOKEN_MAX || "10", 10);
const BAN_SEC = parseInt(process.env.BAN_SEC || "600", 10);

/** @type {Map<string, number[]>} */
const badTokenTimesByIp = new Map();
/** @type {Map<string, number>} */
const bannedUntilMsByIp = new Map();

function clientIp(req) {
  // If behind a reverse proxy, set and trust X-Forwarded-For explicitly in your infra.
  const xff = (req.headers["x-forwarded-for"] || "").toString().trim();
  if (xff) return xff.split(",")[0].trim();
  return req.socket?.remoteAddress || "";
}

function isBanned(ip) {
  const until = bannedUntilMsByIp.get(ip) || 0;
  if (!until) return false;
  if (nowMs() >= until) {
    bannedUntilMsByIp.delete(ip);
    return false;
  }
  return true;
}

function noteBadToken(ip, req) {
  const now = nowMs();
  const winMs = Math.max(1, BAD_TOKEN_WINDOW_SEC) * 1000;
  const maxBad = Math.max(1, BAD_TOKEN_MAX);
  const banMs = Math.max(1, BAN_SEC) * 1000;

  const arr = badTokenTimesByIp.get(ip) || [];
  const cutoff = now - winMs;
  const kept = arr.filter((t) => t >= cutoff);
  kept.push(now);
  badTokenTimesByIp.set(ip, kept);

  if (kept.length >= maxBad) {
    badTokenTimesByIp.delete(ip);
    bannedUntilMsByIp.set(ip, now + banMs);
    webhookPush({
      t: "ban",
      ip,
      until_ms: now + banMs,
      reason: "too_many_bad_tokens",
      ua: req.headers["user-agent"] || ""
    });
    return true;
  }
  return false;
}

async function webhookPush(payload) {
  if (!TOKEN_WEBHOOK_URL) return { ok: false, error: "webhook_not_configured" };
  let bodyObj = payload;
  if (TOKEN_WEBHOOK_FORMAT === "dingTalk") {
    let msg = "";
    if (payload && payload.t === "token") {
      // User-facing: include a prefix; some webhook forwarders may treat purely-numeric messages specially.
      msg = `token=${String(payload.token || "")}`;
    } else if (payload && payload.t === "ban") {
      const until = payload.until_ms ? new Date(payload.until_ms).toISOString() : "";
      msg = `BANNED ip=${payload.ip || ""} until=${until} reason=${payload.reason || ""}`;
    } else {
      msg = JSON.stringify(payload);
    }
    bodyObj = { msg, flushNow: TOKEN_WEBHOOK_DING_FLUSHNOW, type: TOKEN_WEBHOOK_DING_TYPE };
  }
  if (TOKEN_WEBHOOK_DEBUG) {
    console.log(`[ws_ctrl_bridge] webhook url=${TOKEN_WEBHOOK_URL} body=${JSON.stringify(bodyObj)}`);
  }
  const ac = new AbortController();
  const t = setTimeout(() => ac.abort(), 5000);
  try {
    const r = await fetch(TOKEN_WEBHOOK_URL, {
      method: "POST",
      headers: {
        "content-type": "application/json",
        ...(TOKEN_WEBHOOK_AUTH ? { authorization: TOKEN_WEBHOOK_AUTH } : {})
      },
      body: JSON.stringify(bodyObj),
      signal: ac.signal
    });
    const txt = await r.text().catch(() => "");
    if (TOKEN_WEBHOOK_DEBUG) {
      console.log(`[ws_ctrl_bridge] webhook t=${payload?.t || "?"} status=${r.status} body=${txt.slice(0, 200).replaceAll("\n", "\\n")}`);
    }
    if (!r.ok) return { ok: false, status: r.status, body: txt };
    return { ok: true, status: r.status, body: txt };
  } catch {
    // best-effort
    return { ok: false, error: "webhook_request_failed" };
  } finally {
    clearTimeout(t);
  }
}

function tokenRec(ip) {
  if (!ip) return null;
  const rec = tokenByIp.get(ip);
  if (!rec) return null;
  if (rec.expMs > 0 && nowMs() > rec.expMs) {
    tokenByIp.delete(ip);
    return null;
  }
  return rec;
}

async function issueTokenForIp(ip, reason, req) {
  // If there is no webhook configured, don't issue tokens on behalf of users.
  // Otherwise the token exists but cannot be delivered, and the user will be stuck.
  if (!TOKEN_WEBHOOK_URL) return { ok: false, error: "webhook_not_configured" };

  const now = nowMs();
  const ttlMs = Math.max(10, TOKEN_TTL_SEC) * 1000;
  const winMs = Math.max(1, TOKEN_PER_IP_WINDOW_SEC) * 1000;
  const retryMinMs = Math.max(1, TOKEN_WEBHOOK_RETRY_MIN_SEC) * 1000;

  const existing = tokenRec(ip);
  if (existing && now - existing.issuedMs < winMs) {
    // Within the window, do NOT push again.
    return { ok: true, status: "already_issued", token: existing.token, expMs: existing.expMs };
  }

  // Avoid hammering the webhook endpoint on repeated CONNECT clicks.
  const lastAttempt = lastTokenIssueAttemptMsByIp.get(ip) || 0;
  if (lastAttempt && now - lastAttempt < retryMinMs) {
    return { ok: false, error: "retry_later", retry_after_ms: retryMinMs - (now - lastAttempt) };
  }
  lastTokenIssueAttemptMsByIp.set(ip, now);

  const token = genToken();
  const expMs = now + ttlMs;
  if (TOKEN_WEBHOOK_DEBUG) console.log(`[ws_ctrl_bridge] token issue ip=${ip} token=${token} reason=${reason}`);
  const pushRes = await webhookPush({
    t: "token",
    reason,
    token,
    exp_ms: expMs,
    ip,
    ua: req.headers["user-agent"] || ""
  });
  if (!pushRes.ok) {
    return { ok: false, error: "webhook_failed", webhook: pushRes };
  }

  const rec = { token, expMs, issuedMs: now };
  tokenByIp.set(ip, rec);
  return { ok: true, status: "sent", token, expMs, webhook: pushRes };
}

function dockerRequest(method, p, bodyBuf = null) {
  return new Promise((resolve, reject) => {
    const req = http.request(
      {
        socketPath: DOCKER_SOCK,
        method,
        path: p,
        headers: bodyBuf
          ? {
              "content-type": "application/json",
              "content-length": bodyBuf.length
            }
          : undefined
      },
      (res) => {
        const chunks = [];
        res.on("data", (c) => chunks.push(c));
        res.on("end", () => resolve({ status: res.statusCode || 0, body: Buffer.concat(chunks) }));
      }
    );
    req.on("error", reject);
    if (bodyBuf) req.write(bodyBuf);
    req.end();
  });
}

async function ensureContainersRunning() {
  if (!ONDEMAND_ENABLE) return;
  if (ONDEMAND_CONTAINERS.length === 0) return;
  for (const name of ONDEMAND_CONTAINERS) {
    try {
      await dockerRequest("POST", `/containers/${encodeURIComponent(name)}/start`);
    } catch {
      // best-effort
    }
  }
}

async function stopContainers() {
  if (!ONDEMAND_ENABLE) return;
  if (ONDEMAND_CONTAINERS.length === 0) return;
  const stopOrder = [...ONDEMAND_CONTAINERS].reverse();
  for (const name of stopOrder) {
    try {
      await dockerRequest("POST", `/containers/${encodeURIComponent(name)}/stop?t=2`);
    } catch {
      // best-effort
    }
  }
}

function scheduleIdleStop() {
  if (!ONDEMAND_ENABLE) return;
  if (idleStopTimer) clearTimeout(idleStopTimer);
  idleStopTimer = setTimeout(() => {
    if (activeWs) return;
    stopContainers().catch(() => null);
  }, Math.max(1, ONDEMAND_IDLE_TIMEOUT_SEC) * 1000);
}

const server = http.createServer(async (req, res) => {
  if (req && req.url) {
    const url = new URL(req.url, `http://${req.headers.host || "localhost"}`);
    if (url.pathname === "/config.json") {
      const body = JSON.stringify(
        {
          streamName: STREAM_NAME,
          mtxHttpPort: MTX_HTTP_PORT,
          ctrlRefW: CTRL_REF_W,
          ctrlRefH: CTRL_REF_H,
          tokenRequired: !!effectiveTokenRequired,
          ondemand: !!ONDEMAND_ENABLE
        },
        null,
        2
      );
      res.writeHead(200, {
        "content-type": "application/json; charset=utf-8",
        "cache-control": "no-store"
      });
      res.end(body);
      return;
    }
    if (req.method === "POST" && url.pathname === "/whep") {
      forwardToMtxWhep(req, res);
      return;
    }
    if (req.method === "POST" && url.pathname === "/admin/unban") {
      if (!adminAuthed(req)) {
        json(res, 401, { ok: false, error: "unauthorized" });
        return;
      }
      let ip = (url.searchParams.get("ip") || "").trim();
      let all = (url.searchParams.get("all") || "").trim() === "1";
      if (!ip && req.headers["content-type"] && req.headers["content-type"].toString().includes("application/json")) {
        try {
          const body = await readAll(req);
          const j = JSON.parse(body.toString("utf-8"));
          if (typeof j?.ip === "string") ip = j.ip.trim();
          if (j?.all === true) all = true;
        } catch {
          // ignore
        }
      }

      if (all) {
        const bannedCount = bannedUntilMsByIp.size;
        const badCount = badTokenTimesByIp.size;
        bannedUntilMsByIp.clear();
        badTokenTimesByIp.clear();
        json(res, 200, { ok: true, cleared: "all", bannedCount, badCount });
        return;
      }

      if (!ip) {
        json(res, 400, { ok: false, error: "missing_ip" });
        return;
      }

      const wasBanned = bannedUntilMsByIp.delete(ip);
      const hadBad = badTokenTimesByIp.delete(ip);
      json(res, 200, { ok: true, ip, wasBanned, hadBad });
      return;
    }
    if (req.method === "POST" && url.pathname === "/token") {
      if (TOKEN_ISSUE_SECRET) {
        const got = (req.headers["x-token-secret"] || "").toString();
        if (got !== TOKEN_ISSUE_SECRET) {
          json(res, 401, { ok: false, error: "unauthorized" });
          return;
        }
      }
      const ipParam = (url.searchParams.get("ip") || "").trim();
      const ip = ipParam || clientIp(req);
      if (!ip) {
        json(res, 400, { ok: false, error: "no_ip" });
        return;
      }
      const issued = await issueTokenForIp(ip, "http_token", req);
      if (!issued.ok) {
        json(res, 502, { ok: false, error: issued.error, webhook: issued.webhook || null, retry_after_ms: issued.retry_after_ms || 0 });
        return;
      }
      // Invalidate current session if any.
      if (activeWs) {
        try {
          activeWs.close(4001, "token rotated");
        } catch {}
        activeWs = null;
      }
      scheduleIdleStop();
      json(res, 200, { ok: true, status: issued.status, token: issued.token, exp_ms: issued.expMs });
      return;
    }
  }
  serveStatic(req, res);
});

const wss = new WebSocketServer({ server, path: "/ws" });

console.log(`[ws_ctrl_bridge] listen :${LISTEN_PORT}`);
console.log(`[ws_ctrl_bridge] agent ctrl ${AGENT_HOST}:${AGENT_CTRL_PORT} token=${AGENT_TOKEN ? "yes" : "no"}`);
console.log(`[ws_ctrl_bridge] stream=${STREAM_NAME} mtx_http_port=${MTX_HTTP_PORT}`);
console.log(
  `[ws_ctrl_bridge] session token_required=${TOKEN_REQUIRED} issue_on_ws_connect=${TOKEN_ISSUE_ON_WS_CONNECT} per_ip_window_sec=${TOKEN_PER_IP_WINDOW_SEC} single_session=${SINGLE_SESSION} ondemand=${ONDEMAND_ENABLE} containers=${ONDEMAND_CONTAINERS.join(",")}`
);

function isTokenValid(tok, ip) {
  if (!effectiveTokenRequired) return true;
  if (!tok) return false;
  if (!ip) return false;
  const rec = tokenRec(ip);
  if (!rec) return false;
  return tok === rec.token;
}

function startControlBridge(ws) {
  let seq = 0n;
  let periodicIdrTimer = null;
  const burstTimers = [];

  const ctrl = new net.Socket();
  ctrl.setNoDelay(true);

  const ctrlReady = new Promise((resolve, reject) => {
    ctrl.once("error", reject);
    ctrl.connect(AGENT_CTRL_PORT, AGENT_HOST, () => resolve());
  });

  function sendToCtrl(buf) {
    try {
      ctrl.write(buf);
    } catch {
      // ignore
    }
  }

  function requestIdr() {
    if (!ctrl.writable) return;
    seq += 1n;
    sendToCtrl(framePacket(packCmd({ type: ML_CTRL_CMD_REQ_IDR, seq })));
  }

  ctrlReady
    .then(() => {
      if (AGENT_TOKEN) {
        sendToCtrl(Buffer.from(`AUTH ${AGENT_TOKEN}\n`, "utf-8"));
      }

      // best-effort: request IDR on viewer connect (burst)
      const burstN = Number.isFinite(REQ_IDR_ON_CONNECT_BURST) ? Math.max(0, Math.min(10, REQ_IDR_ON_CONNECT_BURST | 0)) : 0;
      const burstInterval = Number.isFinite(REQ_IDR_BURST_INTERVAL_MS)
        ? Math.max(50, Math.min(5000, REQ_IDR_BURST_INTERVAL_MS | 0))
        : 500;
      for (let i = 0; i < burstN; i++) {
        burstTimers.push(setTimeout(requestIdr, i * burstInterval));
      }

      // Optional periodic REQ_IDR if encoder does not produce keyframes for minutes.
      const periodSec = Number.isFinite(REQ_IDR_PERIOD_SEC) ? REQ_IDR_PERIOD_SEC : 0;
      if (periodSec > 0) {
        const ms = Math.max(250, Math.floor(periodSec * 1000));
        periodicIdrTimer = setInterval(requestIdr, ms);
      }
    })
    .catch(() => {
      // ignore; ws still connected but control won't work
    });

  ws.on("message", async (data) => {
    let msg;
    try {
      msg = JSON.parse(data.toString("utf-8"));
    } catch {
      return;
    }

    await ctrlReady.catch(() => null);
    if (!ctrl.writable) return;

    const t = msg.t || "";
    const k = msg.k || "";

    if (t === "mouse") {
      if (k === "move") {
        seq += 1n;
        const refW = (msg.ref_w | 0) > 0 ? msg.ref_w | 0 : (CTRL_REF_W > 0 ? CTRL_REF_W : 0);
        const refH = (msg.ref_h | 0) > 0 ? msg.ref_h | 0 : (CTRL_REF_H > 0 ? CTRL_REF_H : 0);
        const payload = packCmd({
          type: ML_CTRL_CMD_MOUSE_ABS,
          a: msg.x | 0,
          b: msg.y | 0,
          c: refW | 0,
          d: refH | 0,
          seq
        });
        sendToCtrl(framePacket(payload));
        return;
      }

      if (k === "down" || k === "up") {
        const bname = msg.b || "left";
        const button = bname === "right" ? BUTTON_RIGHT : bname === "middle" ? BUTTON_MIDDLE : BUTTON_LEFT;
        const action = k === "down" ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE;
        seq += 1n;
        const payload = packCmd({
          type: ML_CTRL_CMD_MOUSE_BUTTON,
          a: action,
          b: button,
          c: 0,
          d: 0,
          seq
        });
        sendToCtrl(framePacket(payload));
        return;
      }

      if (k === "wheel") {
        const dy = msg.dy || 0;
        const dx = msg.dx || 0;
        // Best-effort: dy<0 means scroll up.
        const vClicks = dy === 0 ? 0 : dy < 0 ? 1 : -1;
        const hClicks = dx === 0 ? 0 : dx < 0 ? 1 : -1;
        if (vClicks) {
          seq += 1n;
          sendToCtrl(framePacket(packCmd({ type: ML_CTRL_CMD_MOUSE_SCROLL, a: clampI8(vClicks), seq })));
        }
        if (hClicks) {
          seq += 1n;
          sendToCtrl(framePacket(packCmd({ type: ML_CTRL_CMD_MOUSE_HSCROLL, a: clampI8(hClicks), seq })));
        }
        return;
      }
    }

    if (t === "key") {
      if (k === "press") {
        const vk = (msg.vk | 0) > 0 ? msg.vk | 0 : vkFromKey(msg.key || "");
        if (vk) {
          let modifiers = msg.modifiers | 0;
          if (MAP_MAC_META_TO_CTRL && (modifiers & MODIFIER_META) && !(modifiers & MODIFIER_CTRL)) {
            modifiers = (modifiers | MODIFIER_CTRL) & ~MODIFIER_META;
          }
          seq += 1n;
          sendToCtrl(framePacket(packCmd({ type: ML_CTRL_CMD_KEY_PRESS, a: vk, b: modifiers, seq })));
        } else if (msg.key && msg.key.length === 1) {
          const payload = Buffer.from(msg.key, "utf-8");
          seq += 1n;
          sendToCtrl(framePacket(Buffer.concat([packCmd({ type: ML_CTRL_CMD_TEXT, a: payload.length, seq }), payload])));
        }
        return;
      }
      if (k === "text" && typeof msg.text === "string") {
        const payload = Buffer.from(msg.text, "utf-8");
        seq += 1n;
        sendToCtrl(framePacket(Buffer.concat([packCmd({ type: ML_CTRL_CMD_TEXT, a: payload.length, seq }), payload])));
        return;
      }
    }

    if (t === "req_idr") {
      requestIdr();
    }
  });

  ws.on("close", () => {
    if (periodicIdrTimer) {
      clearInterval(periodicIdrTimer);
      periodicIdrTimer = null;
    }
    for (const t of burstTimers) clearTimeout(t);
    try {
      ctrl.destroy();
    } catch {
      // ignore
    }
  });
}

wss.on("connection", async (ws, req) => {
  const url = new URL(req.url || "/ws", `http://${req.headers.host || "localhost"}`);
  const tok = (url.searchParams.get("token") || "").trim();
  const ip = clientIp(req);
  const tokProvided = tok.length > 0;

  if (ip && isBanned(ip)) {
    try {
      ws.send(JSON.stringify({ t: "auth", ok: false, error: "banned" }));
    } catch {}
    try {
      ws.close(4004, "banned");
    } catch {}
    return;
  }

  if (!isTokenValid(tok, ip)) {
    // If token is missing, try issuing one via webhook (rate-limited per IP and per 5-min window).
    if (effectiveTokenRequired && TOKEN_ISSUE_ON_WS_CONNECT && !tokProvided) {
      if (!ip) {
        try {
          ws.send(JSON.stringify({ t: "auth", ok: false, error: "no_ip" }));
        } catch {}
        try {
          ws.close(4003, "auth failed");
        } catch {}
        return;
      }
      const issued = await issueTokenForIp(ip, "ws_connect", req);
      if (!issued.ok) {
        try {
          ws.send(JSON.stringify({ t: "auth", ok: false, error: issued.error, webhook: issued.webhook || null }));
        } catch {}
        try {
          ws.close(4003, "auth failed");
        } catch {}
        return;
      }
      try {
        ws.send(JSON.stringify({ t: "auth", ok: false, error: "need_token", status: issued.status, exp_ms: issued.expMs }));
      } catch {}
      try {
        ws.close(4003, "need token");
      } catch {}
      return;
    }
    // Only count as a "bad token" attempt when the client actually provided a token.
    // Clicking CONNECT without a token is treated as "request a token" in the webhook flow.
    if (ip && tokProvided) noteBadToken(ip, req);
    try {
      ws.send(JSON.stringify({ t: "auth", ok: false, error: effectiveTokenRequired ? "need_token" : "bad_token" }));
    } catch {}
    try {
      ws.close(4003, "auth failed");
    } catch {}
    return;
  }

  if (SINGLE_SESSION && activeWs && activeWs !== ws) {
    try {
      activeWs.close(4000, "replaced");
    } catch {}
    activeWs = null;
  }

  activeWs = ws;
  if (idleStopTimer) {
    clearTimeout(idleStopTimer);
    idleStopTimer = null;
  }

  ensureContainersRunning().catch(() => null);

  try {
    ws.send(JSON.stringify({ t: "auth", ok: true }));
  } catch {}

  if (effectiveTokenRequired && TOKEN_SINGLE_USE && TOKEN_PER_IP_WINDOW_SEC <= 0) {
    // Consume token after successful auth only when not in per-IP window mode.
    // (Per-IP mode intentionally allows reuse within the window.)
    if (ip) tokenByIp.delete(ip);
  }

  startControlBridge(ws);

  ws.on("close", () => {
    if (activeWs === ws) activeWs = null;
    scheduleIdleStop();
  });
});

server.listen(LISTEN_PORT);
