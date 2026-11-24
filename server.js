
"use strict";

const express = require("express");
const http = require("http");
const WebSocket = require("ws");
const crypto = require("crypto");

const app = express();
app.use(express.json({ limit: "100kb" }));

const server = http.createServer(app);
const wss = new WebSocket.Server({ server, path: "/ws" });

const PORT = process.env.PORT || 3000;
const DEVICE_ID = process.env.DEVICE_ID || "RAMATECH_HOME_01";
const DEVICE_TOKEN = process.env.DEVICE_TOKEN || "";
const ACCESS_CODE = process.env.ACCESS_CODE || "";
const BRIDGE_TOKEN = process.env.BRIDGE_TOKEN || "";

const DEVICE_TIMEOUT = 45000;

let deviceSocket = null;
let lastDeviceSeen = 0;

const latestStatus = {
  type: "status",
  deviceId: DEVICE_ID,
  online: false,
  light: false,
  rope: false,
  fan: false,
  tv: false
};

function safeEqual(a, b) {
  if (!a || !b) return false;

  const first = Buffer.from(String(a));
  const second = Buffer.from(String(b));

  return first.length === second.length &&
    crypto.timingSafeEqual(first, second);
}

function isDeviceOnline() {
  return !!(
    deviceSocket &&
    deviceSocket.readyState === WebSocket.OPEN &&
    Date.now() - lastDeviceSeen < DEVICE_TIMEOUT
  );
}

function send(ws, data) {
  if (!ws || ws.readyState !== WebSocket.OPEN) {
    return false;
  }

  ws.send(JSON.stringify(data));
  return true;
}

function sendToDevice(data) {
  if (!isDeviceOnline()) {
    return false;
  }

  lastDeviceSeen = Date.now();
  return send(deviceSocket, data);
}

function bridgeAuth(req, res, next) {
  const header = req.headers.authorization || "";
  const supplied = header.startsWith("Bearer ")
    ? header.slice(7)
    : "";

  if (!BRIDGE_TOKEN) {
    return res.status(503).json({
      ok: false,
      error: "bridge_token_not_configured"
    });
  }

  if (!safeEqual(supplied, BRIDGE_TOKEN)) {
    return res.status(401).json({
      ok: false,
      error: "unauthorized"
    });
  }

  next();
}

function updateDeviceStatus(data) {
  const allowed = [
    "light",
    "rope",
    "fan",
    "tv",
    "mode",
    "prepaidSeconds",
    "prepaidMinutes",
    "prepaidActive",
    "loadActive",
    "voltage",
    "current",
    "power",
    "energy",
    "temperature"
  ];

  for (const key of allowed) {
    if (Object.prototype.hasOwnProperty.call(data, key)) {
      latestStatus[key] = data[key];
    }
  }

  latestStatus.type = "status";
  latestStatus.deviceId = DEVICE_ID;
  latestStatus.online = true;
  lastDeviceSeen = Date.now();
}

function validateAppliance(device) {
  return ["light", "rope", "fan", "tv"].includes(device);
}

app.get("/", (req, res) => {
  res.json({
    ok: true,
    service: "RAMATECH HOME AI Cloud Gateway",
    deviceId: DEVICE_ID,
    deviceOnline: isDeviceOnline()
  });
});

app.get("/health", (req, res) => {
  res.json({
    ok: true,
    service: "RAMATECH HOME AI Cloud Gateway",
    deviceOnline: isDeviceOnline()
  });
});

// Get the latest ESP32 status.
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

// Send appliance, all-appliance, display or status commands.
app.post("/api/bridge/command", bridgeAuth, (req, res) => {
  const body = req.body || {};
  const type = body.type;

  if (!isDeviceOnline()) {
    return res.status(503).json({
      ok: false,
      error: "esp32_offline",
      message: "The ESP32 is not connected to the cloud gateway."
    });
  }

  let message;

  if (type === "command") {
    const device = String(body.device || "").toLowerCase();
    const state = String(body.state || "").toLowerCase();

    if (!validateAppliance(device)) {
      return res.status(400).json({
        ok: false,
        error: "invalid_device"
      });
    }

    if (!["on", "off"].includes(state)) {
      return res.status(400).json({
        ok: false,
        error: "invalid_state"
      });
    }

    message = {
      type: "command",
      device,
      state
    };
  } else if (type === "all") {
    const state = String(body.state || "").toLowerCase();

    if (!["on", "off"].includes(state)) {
      return res.status(400).json({
        ok: false,
        error: "invalid_state"
      });
    }

    message = {
      type: "all",
      state
    };
  } else if (type === "status_request") {
    message = { type: "status_request" };
  } else if (type === "display") {
    message = {
      type: "display",
      line1: String(body.line1 || "").slice(0, 16),
      line2: String(body.line2 || "").slice(0, 16)
    };
  } else {
    return res.status(400).json({
      ok: false,
      error: "unsupported_command"
    });
  }

  const accepted = sendToDevice(message);

  if (!accepted) {
    return res.status(503).json({
      ok: false,
      error: "command_not_sent"
    });
  }

  res.json({
    ok: true,
    accepted: true,
    deviceId: DEVICE_ID,
    command: message
  });
});

// Meter endpoints deliberately report that firmware support is not installed.
// They must not pretend to credit or activate prepaid power.
function meterNotImplemented(req, res) {
  res.status(501).json({
    ok: false,
    error: "meter_firmware_not_implemented",
    message: "The connected ESP32 firmware does not yet implement prepaid-meter functions."
  });
}

app.get("/api/bridge/meter/status", bridgeAuth, meterNotImplemented);
app.post("/api/bridge/meter/topup", bridgeAuth, meterNotImplemented);
app.post("/api/bridge/meter/mode", bridgeAuth, meterNotImplemented);

// WebSocket connections: ESP32 device and browser clients.
wss.on("connection", (ws) => {
  ws.isAuthenticated = false;
  ws.role = null;
  ws.isAlive = true;

  ws.on("pong", () => {
    ws.isAlive = true;
    if (ws === deviceSocket) {
      lastDeviceSeen = Date.now();
    }
  });

  ws.on("message", (raw) => {
    let data;

    try {
      data = JSON.parse(raw.toString());
    } catch {
      send(ws, {
        type: "error",
        message: "Invalid JSON"
      });
      return;
    }

    // Authentication must be the first message.
    if (!ws.isAuthenticated) {
      if (data.type !== "auth") {
        send(ws, {
          type: "auth_error",
          message: "Authentication required"
        });
        ws.close();
        return;
      }

      if (
        data.role === "device" &&
        data.deviceId === DEVICE_ID &&
        safeEqual(data.token, DEVICE_TOKEN)
      ) {
        if (deviceSocket && deviceSocket !== ws) {
          try {
            deviceSocket.close();
          } catch {}
        }

        deviceSocket = ws;
        ws.role = "device";
        ws.isAuthenticated = true;
        lastDeviceSeen = Date.now();

        send(ws, {
          type: "auth_ok",
          role: "device",
          deviceId: DEVICE_ID
        });

        return;
      }

      if (
        data.role === "browser" &&
        safeEqual(data.code, ACCESS_CODE)
      ) {
        ws.role = "browser";
        ws.isAuthenticated = true;

        send(ws, {
          type: "auth_ok",
          role: "browser",
          deviceOnline: isDeviceOnline()
        });

        send(ws, {
          type: "status",
          ...latestStatus,
          online: isDeviceOnline()
        });

        return;
      }

      send(ws, {
        type: "auth_error",
        message: "Invalid credentials"
      });

      ws.close();
      return;
    }

    // Messages received from the ESP32.
    if (ws.role === "device") {
      lastDeviceSeen = Date.now();

      if (data.type === "status") {
        updateDeviceStatus(data);

        for (const client of wss.clients) {
          if (client !== ws && client.isAuthenticated && client.role === "browser") {
            send(client, {
              type: "status",
              ...latestStatus,
              online: isDeviceOnline()
            });
          }
        }

        return;
      }

      if (data.type === "heartbeat") {
        send(ws, { type: "heartbeat_ack" });
        return;
      }

      return;
    }

    // Commands received from authenticated browser clients.
    if (ws.role === "browser") {
      if (!isDeviceOnline()) {
        send(ws, {
          type: "error",
          message: "ESP32 is offline"
        });
        return;
      }

      if (
        data.type === "command" &&
        validateAppliance(String(data.device || "").toLowerCase()) &&
        ["on", "off"].includes(String(data.state || "").toLowerCase())
      ) {
        sendToDevice({
          type: "command",
          device: String(data.device).toLowerCase(),
          state: String(data.state).toLowerCase()
        });
        return;
      }

      if (
        data.type === "all" &&
        ["on", "off"].includes(String(data.state || "").toLowerCase())
      ) {
        sendToDevice({
          type: "all",
          state: String(data.state).toLowerCase()
        });
        return;
      }

      if (data.type === "status_request") {
        sendToDevice({ type: "status_request" });
        return;
      }

      if (data.type === "display") {
        sendToDevice({
          type: "display",
          line1: String(data.line1 || "").slice(0, 16),
          line2: String(data.line2 || "").slice(0, 16)
        });
        return;
      }

      send(ws, {
        type: "error",
        message: "Unsupported or invalid command"
      });
    }
  });

  ws.on("close", () => {
    if (ws === deviceSocket) {
      deviceSocket = null;
      latestStatus.online = false;
    }
  });

  ws.on("error", (err) => {
    console.error("WebSocket error:", err.message);
  });
});

// Keep WebSocket connections alive.
const heartbeatTimer = setInterval(() => {
  for (const ws of wss.clients) {
    if (ws.isAlive === false) {
      if (ws === deviceSocket) {
        deviceSocket = null;
        latestStatus.online = false;
      }

      ws.terminate();
      continue;
    }

    ws.isAlive = false;
    ws.ping();
  }
}, 30000);

server.listen(PORT, () => {
  console.log(`RAMATECH HOME AI gateway running on port ${PORT}`);
  console.log(`Device ID: ${DEVICE_ID}`);
});

process.on("SIGTERM", () => {
  clearInterval(heartbeatTimer);
  server.close(() => process.exit(0));
});
