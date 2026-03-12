import http from "node:http";
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

const CTRL_REF_W = parseInt(process.env.CTRL_REF_W || "0", 10);
const CTRL_REF_H = parseInt(process.env.CTRL_REF_H || "0", 10);

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

const server = http.createServer((req, res) => serveStatic(req, res));

const wss = new WebSocketServer({ server, path: "/ws" });

console.log(`[ws_ctrl_bridge] listen :${LISTEN_PORT}`);
console.log(`[ws_ctrl_bridge] agent ctrl ${AGENT_HOST}:${AGENT_CTRL_PORT} token=${AGENT_TOKEN ? "yes" : "no"}`);
console.log(`[ws_ctrl_bridge] stream=${STREAM_NAME} mtx_http_port=${MTX_HTTP_PORT}`);

wss.on("connection", (ws) => {
  let seq = 0n;

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

  ctrlReady
    .then(() => {
      if (AGENT_TOKEN) {
        sendToCtrl(Buffer.from(`AUTH ${AGENT_TOKEN}\n`, "utf-8"));
      }
      // best-effort: request an IDR on viewer connect
      seq += 1n;
      const reqIdr = framePacket(packCmd({ type: ML_CTRL_CMD_REQ_IDR, seq }));
      sendToCtrl(reqIdr);
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
          seq += 1n;
          sendToCtrl(framePacket(packCmd({ type: ML_CTRL_CMD_KEY_PRESS, a: vk, b: 0, seq })));
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
      seq += 1n;
      sendToCtrl(framePacket(packCmd({ type: ML_CTRL_CMD_REQ_IDR, seq })));
    }
  });

  ws.on("close", () => {
    try {
      ctrl.destroy();
    } catch {
      // ignore
    }
  });
});

server.listen(LISTEN_PORT);

