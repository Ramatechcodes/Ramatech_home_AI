const crypto = require("crypto");
const express = require("express");
const http = require("http");
const WebSocket = require("ws");
const path = require("path");

const app = express();
const server = http.createServer(app);
const wss = new WebSocket.Server({ server, path: "/ws" });

const PORT = process.env.PORT || 10000;
const DEVICE_TOKEN = process.env.DEVICE_TOKEN || "RMT_DEVICE_HOME";
const ACCESS_CODE = process.env.ACCESS_CODE || "123456";
const DEVICE_ID = process.env.DEVICE_ID || "RAMATECH_HOME_01";
const BRIDGE_TOKEN = process.env.BRIDGE_TOKEN || "";

let deviceSocket = null;
let deviceLastSeen = 0;
let latestStatus = {
  type: "status",
  deviceId: DEVICE_ID,
  online: false,
  light: false,
  rope: false,
  fan: false,
  tv: false
};

app.use(express.static(path.join(__dirname)));
app.use(express.json({ limit: "32kb" }));

app.get("/health", (req, res) => {
  res.json({ ok: true, deviceOnline: !!deviceSocket, deviceId: DEVICE_ID });
});

// ======================================================
// SECURE RAMADANAI HTTP BRIDGE
// ======================================================

function bridgeAuth(req, res, next) {
  const supplied = (req.get("authorization") || "").replace(/^Bearer\s+/i, "");

  if (!BRIDGE_TOKEN) {
    return res.status(503).json({
      ok: false,
      error: "BRIDGE_TOKEN is not configured"
    });
  }

  const a = Buffer.from(supplied);
  const b = Buffer.from(BRIDGE_TOKEN);

  if (a.length !== b.length || !crypto.timingSafeEqual(a, b)) {
    return res.status(401).json({
      ok: false,
      error: "Unauthorized"
    });
  }

  next();
}

function isDeviceOnline() {
  return !!(
    deviceSocket &&
    deviceSocket.readyState === WebSocket.OPEN &&
    Date.now() - deviceLastSeen < 45000
  );
}

// Get current appliance status
app.get("/api/bridge/status", bridgeAuth, (req, res) => {
  res.json({
    ok: true,
    deviceOnline: isDeviceOnline(),
    status: {
      ...latestStatus,
      online: isDeviceOnline()
    }
  });
});

// Send appliance commands to the authenticated ESP32
app.post("/api/bridge/command", bridgeAuth, (req, res) => {
  if (!isDeviceOnline()) {
    return res.status(503).json({
      ok: false,
      error: "ESP32 device is offline"
    });
  }

  const { type, device, state } = req.body || {};

  let message;

  if (type === "command") {
    const allowedDevices = ["light", "rope", "fan", "tv"];

    if (!allowedDevices.includes(device) || !["on", "off"].includes(state)) {
      return res.status(400).json({
        ok: false,
        error: "Invalid appliance or state"
      });
    }

    message = {
      type: "command",
      deviceId: DEVICE_ID,
      device,
      state
    };

  } else if (type === "all") {
    if (!["on", "off"].includes(state)) {
      return res.status(400).json({
        ok: false,
        error: "State must be on or off"
      });
    }

    message = {
      type: "all",
      deviceId: DEVICE_ID,
      state
    };

  } else if (type === "status_request") {
    message = {
      type: "status_request",
      deviceId: DEVICE_ID
    };

  } else {
    return res.status(400).json({
      ok: false,
      error: "Unsupported command type"
    });
  }

  const sent = send(deviceSocket, message);

  if (!sent) {
    return res.status(503).json({
      ok: false,
      error: "Could not send command to ESP32"
    });
  }

  res.json({
    ok: true,
    accepted: true,
    message: "Command forwarded to ESP32",
    deviceOnline: true,
    command: message
  });
});


function send(ws, data) {
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify(data));
    return true;
  }
  return false;
}

function broadcast(data) {
  for (const client of wss.clients) {
    if (client.readyState === WebSocket.OPEN && client.role === "browser" && client.authenticated) {
      client.send(JSON.stringify(data));
    }
  }
}

function markOffline() {
  latestStatus.online = false;
  deviceLastSeen = 0;
  broadcast(latestStatus);
}

wss.on("connection", (ws) => {
  ws.authenticated = false;
  ws.role = null;
  ws.isAlive = true;

  ws.on("pong", () => { ws.isAlive = true; });

  ws.on("message", (raw) => {
    let msg;
    try { msg = JSON.parse(raw.toString()); }
    catch { return send(ws, { type: "error", message: "Invalid JSON" }); }

    // DEVICE AUTHENTICATION
    if (msg.type === "auth" && msg.role === "device") {
      if (msg.token !== DEVICE_TOKEN || msg.deviceId !== DEVICE_ID) {
        return send(ws, { type: "auth_error" });
      }

      if (deviceSocket && deviceSocket !== ws) {
        try { deviceSocket.close(); } catch {}
      }

      deviceSocket = ws;
      ws.authenticated = true;
      ws.role = "device";
      ws.deviceId = DEVICE_ID;
      deviceLastSeen = Date.now();

      send(ws, { type: "auth_ok", deviceId: DEVICE_ID });
      return;
    }

    // BROWSER AUTHENTICATION
    if (msg.type === "auth" && msg.role === "browser") {
      if (msg.code !== ACCESS_CODE) {
        return send(ws, { type: "auth_error" });
      }

      ws.authenticated = true;
      ws.role = "browser";
      send(ws, { type: "auth_ok", deviceId: DEVICE_ID });
      send(ws, { ...latestStatus, online: !!deviceSocket });
      return;
    }

    if (!ws.authenticated) {
      return send(ws, { type: "auth_error", message: "Authenticate first" });
    }

    // DEVICE STATUS
    if (ws.role === "device" && msg.type === "status") {
      if (ws !== deviceSocket) return;
      latestStatus = {
        type: "status",
        deviceId: DEVICE_ID,
        online: true,
        light: !!msg.light,
        rope: !!msg.rope,
        fan: !!msg.fan,
        tv: !!msg.tv
      };
      deviceLastSeen = Date.now();
      broadcast(latestStatus);
      return;
    }

    // DEVICE PING/HEARTBEAT STATUS
    if (ws.role === "device" && msg.type === "heartbeat") {
      deviceLastSeen = Date.now();
      return;
    }

    // BROWSER COMMAND
    if (ws.role === "browser" && (msg.type === "command" || msg.type === "all" || msg.type === "status_request" || msg.type === "display")) {
      if (!deviceSocket || deviceSocket.readyState !== WebSocket.OPEN) {
        return send(ws, { type: "device_offline" });
      }

      if (msg.deviceId !== DEVICE_ID) return;

      send(deviceSocket, msg);
      return;
    }
  });

  ws.on("close", () => {
    if (ws === deviceSocket) {
      deviceSocket = null;
      markOffline();
    }
  });

  ws.on("error", () => {});
});

// Keepalive for WebSockets. Render recommends ping/pong and client reconnects.
setInterval(() => {
  for (const ws of wss.clients) {
    if (ws.isAlive === false) {
      try { ws.terminate(); } catch {}
      continue;
    }
    ws.isAlive = false;
    try { ws.ping(); } catch {}
  }

  if (deviceSocket && Date.now() - deviceLastSeen > 45000) {
    try { deviceSocket.terminate(); } catch {}
    deviceSocket = null;
    markOffline();
  }
}, 30000);

server.listen(PORT, "0.0.0.0", () => {
  console.log(`RAMATECH HOME AI Cloud running on port ${PORT}`);
  console.log(`Device ID: ${DEVICE_ID}`);
});
