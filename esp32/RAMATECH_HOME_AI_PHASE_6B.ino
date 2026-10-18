
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <time.h>

// ======================================================
// RAMATECH HOME AI
// ESP32 + 4 RELAYS + 1602 I2C LCD
// LOCAL WEB CONTROL + CLOUD WEBSOCKET + IPHONE VOICE
// Adjustable speech voice, accent, rate and pitch
// ======================================================

// ---------------- WIFI SETTINGS -----------------------
const char* WIFI_SSID     = "Ramatechcode";
const char* WIFI_PASSWORD = "Ramadan@14";

// ---------------- CLOUD SETTINGS ---------------------
const char* CLOUD_HOST  = "ramatech-home-ai.onrender.com";
const uint16_t CLOUD_PORT = 443;
const char* CLOUD_PATH  = "/ws";

const char* DEVICE_ID    = "RAMATECH_HOME_01";
const char* DEVICE_TOKEN = "RMT_DEVICE_HOME";

// ---------------- RELAY PINS -------------------------
const int RELAY_LIGHT = 25;
const int RELAY_ROPE  = 26;
const int RELAY_FAN   = 27;
const int RELAY_TV    = 33;

// Active LOW relay module
const int RELAY_ON  = LOW;
const int RELAY_OFF = HIGH;

// ---------------- I2C LCD ----------------------------
const int LCD_SDA = 21;
const int LCD_SCL = 22;
LiquidCrystal_I2C lcd(0x27, 16, 2); // Common I2C address; some modules use 0x3F
String lcdMessage1 = "";
String lcdMessage2 = "";
unsigned long lcdMessageUntil = 0;
unsigned long lastLcdRefresh = 0;

// ---------------- SERVERS -----------------------------
WebServer server(80);
WebSocketsClient cloudSocket;

// ---------------- DEVICE STATE ------------------------
bool lightState = false;
bool ropeState  = false;
bool fanState   = false;
bool tvState    = false;

bool cloudAuthenticated = false;

unsigned long lastWiFiAttempt = 0;
unsigned long lastCloudAttempt = 0;
unsigned long lastHeartbeat = 0;
unsigned long lastCloudStatus = 0;

const unsigned long WIFI_RETRY_MS = 10000;
const unsigned long CLOUD_RETRY_MS = 5000;
const unsigned long HEARTBEAT_MS = 15000;
const unsigned long STATUS_MS = 5000;

// ======================================================
// STATUS JSON
// ======================================================

String getStatusJson() {
  StaticJsonDocument<256> doc;

  doc["type"] = "status";
  doc["deviceId"] = DEVICE_ID;
  doc["online"] = true;
  doc["light"] = lightState;
  doc["rope"] = ropeState;
  doc["fan"] = fanState;
  doc["tv"] = tvState;

  String output;
  serializeJson(doc, output);
  return output;
}

void sendCloudStatus() {
  if (!cloudAuthenticated) return;

  // Keep this as a named String for WebSockets library
  String statusJson = getStatusJson();
  cloudSocket.sendTXT(statusJson);
}

// ======================================================
// LCD STATUS DISPLAY
// ======================================================

String fit16(String value) {
  if (value.length() > 16) value = value.substring(0, 16);
  while (value.length() < 16) value += " ";
  return value;
}

void displayMessage(String line1, String line2 = "") {
  lcdMessage1 = line1;
  lcdMessage2 = line2;
  lcdMessageUntil = millis() + 4000;
  lcd.setCursor(0, 0);
  lcd.print(fit16(lcdMessage1));
  lcd.setCursor(0, 1);
  lcd.print(fit16(lcdMessage2));
}

void showStatus() {
  lcd.setCursor(0, 0);
  lcd.print(fit16("L:" + String(lightState ? "ON" : "OFF") +
                  " R:" + String(ropeState ? "ON" : "OFF")));
  lcd.setCursor(0, 1);
  lcd.print(fit16("F:" + String(fanState ? "ON" : "OFF") +
                  " TV:" + String(tvState ? "ON" : "OFF")));
}

void refreshLCD() {
  if (millis() - lastLcdRefresh < 1000) return;
  lastLcdRefresh = millis();
  if (lcdMessageUntil && (long)(millis() - lcdMessageUntil) < 0) return;
  lcdMessageUntil = 0;
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 10)) {
    char dateTime[17], status[17];
    strftime(dateTime, sizeof(dateTime), "%d %b %H:%M", &timeinfo);
    snprintf(status, sizeof(status), "L%s R%s F%s T%s",
      lightState ? "1" : "0", ropeState ? "1" : "0",
      fanState ? "1" : "0", tvState ? "1" : "0");
    lcd.setCursor(0, 0); lcd.print(fit16(String(dateTime)));
    lcd.setCursor(0, 1); lcd.print(fit16(String(status)));
  } else {
    showStatus();
  }
}

String currentDateTimeJson() {
  struct tm timeinfo;
  StaticJsonDocument<192> doc;
  if (getLocalTime(&timeinfo, 100)) {
    char dateBuf[32], timeBuf[20];
    strftime(dateBuf, sizeof(dateBuf), "%A, %d %B %Y", &timeinfo);
    strftime(timeBuf, sizeof(timeBuf), "%I:%M %p", &timeinfo);
    doc["valid"] = true;
    doc["date"] = dateBuf;
    doc["time"] = timeBuf;
  } else {
    doc["valid"] = false;
    doc["date"] = "Date unavailable";
    doc["time"] = "Time unavailable";
  }
  String out; serializeJson(doc, out); return out;
}

void updateOutputs() {
  digitalWrite(RELAY_LIGHT, lightState ? RELAY_ON : RELAY_OFF);
  digitalWrite(RELAY_ROPE,  ropeState  ? RELAY_ON : RELAY_OFF);
  digitalWrite(RELAY_FAN,   fanState   ? RELAY_ON : RELAY_OFF);
  digitalWrite(RELAY_TV,    tvState    ? RELAY_ON : RELAY_OFF);

  showStatus();
}

void setAll(bool turnOn) {
  lightState = turnOn;
  ropeState  = turnOn;
  fanState   = turnOn;
  tvState    = turnOn;

  updateOutputs();
  displayMessage(turnOn ? "ALL APPLIANCES ON" : "ALL APPLIANCES OFF", "Command completed");
  sendCloudStatus();
}

bool setDeviceState(String device, bool turnOn) {
  device.toLowerCase();

  if (device == "light" || device == "lights") {
    lightState = turnOn;
  } else if (device == "rope" ||
             device == "rope light" ||
             device == "ropelight") {
    ropeState = turnOn;
  } else if (device == "fan") {
    fanState = turnOn;
  } else if (device == "tv" ||
             device == "television") {
    tvState = turnOn;
  } else if (device == "all") {
    setAll(turnOn);
    return true;
  } else {
    return false;
  }

  updateOutputs();
  displayMessage(device + (turnOn ? " TURNED ON" : " TURNED OFF"), "Command completed");
  sendCloudStatus();
  return true;
}

// ======================================================
// LOCAL WEB SERVER ROUTES
// ======================================================

void handleControl() {
  if (!server.hasArg("device") || !server.hasArg("state")) {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"Missing device or state\"}");
    return;
  }

  String device = server.arg("device");
  String state = server.arg("state");
  state.toLowerCase();

  if (state != "on" && state != "off") {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"State must be on or off\"}");
    return;
  }

  bool success = setDeviceState(device, state == "on");

  if (!success) {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"Unknown device\"}");
    return;
  }

  server.send(200, "application/json", getStatusJson());
}

void handleAll() {
  if (!server.hasArg("state")) {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"Missing state\"}");
    return;
  }

  String state = server.arg("state");
  state.toLowerCase();

  if (state != "on" && state != "off") {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"State must be on or off\"}");
    return;
  }

  setAll(state == "on");
  server.send(200, "application/json", getStatusJson());
}

void handleStatus() {
  server.send(200, "application/json", getStatusJson());
}

void handleDisplayCommand() {
  if (!server.hasArg("text")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Missing text\"}");
    return;
  }
  displayMessage("VOICE COMMAND:", server.arg("text"));
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleTime() {
  server.send(200, "application/json", currentDateTimeJson());
}

// ======================================================
// WEB DASHBOARD WITH VOICE SETTINGS
// ======================================================

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport"
 content="width=device-width, initial-scale=1, maximum-scale=1">
<meta name="theme-color" content="#0f172a">
<title>RAMATECH HOME AI</title>

<style>
:root {
  --bg:#0f172a;
  --panel:#172554;
  --cyan:#38bdf8;
  --text:#f8fafc;
  --muted:#cbd5e1;
  --green:#22c55e;
  --red:#ef4444;
}
* {box-sizing:border-box}
body {
  margin:0;
  padding:16px;
  font-family:Arial,Helvetica,sans-serif;
  background:var(--bg);
  color:var(--text);
}
main {max-width:760px;margin:auto}
h1 {font-size:25px;margin:8px 0;color:var(--cyan)}
h2 {font-size:19px;margin-top:0}
p {line-height:1.5}
.panel {
  background:var(--panel);
  padding:16px;
  margin:14px 0;
  border:1px solid #334155;
  border-radius:15px;
}
.status {
  padding:10px;
  background:#0f172a;
  border-radius:9px;
  margin:8px 0;
  color:var(--muted);
  overflow-wrap:anywhere;
}
.grid {
  display:grid;
  grid-template-columns:repeat(2,minmax(0,1fr));
  gap:10px;
}
button,select,input[type=range] {width:100%}
button,select {
  border:0;
  border-radius:9px;
  padding:12px;
  font-size:15px;
}
button {
  background:var(--cyan);
  color:#082f49;
  font-weight:bold;
  cursor:pointer;
  min-height:44px;
}
button:active {opacity:.8}
.off {background:#475569;color:white}
.danger {background:#f87171;color:#450a0a}
label {
  display:block;
  margin:15px 0 6px;
  font-weight:bold;
}
select {
  background:#f8fafc;
  color:#0f172a;
}
input[type=range] {accent-color:var(--cyan)}
.small {font-size:13px;color:var(--muted)}
.value {color:var(--cyan);font-weight:bold}
.full {margin-top:10px}
#commandInput {
  width:100%;
  padding:12px;
  border-radius:8px;
  border:0;
  font-size:16px;
}
</style>
</head>

<body>
<main>
  <h1>RAMATECH HOME AI</h1>
  <p>Smart home assistant designed, programmed and developed by
     Ramatechcode Lab.</p>

  <section class="panel">
    <h2>Device Status</h2>
    <div class="status" id="connectionStatus">Connecting...</div>
    <div class="status" id="deviceStatus">Loading appliance status...</div>
  </section>

  <section class="panel">
    <h2>Appliance Control</h2>
    <div class="grid">
      <button id="lightBtn" onclick="toggleDevice('light')">Light: OFF</button>
      <button id="ropeBtn" onclick="toggleDevice('rope')">Rope Light: OFF</button>
      <button id="fanBtn" onclick="toggleDevice('fan')">Fan: OFF</button>
      <button id="tvBtn" onclick="toggleDevice('tv')">TV: OFF</button>
      <button onclick="controlAll('on')">ALL ON</button>
      <button class="danger" onclick="controlAll('off')">ALL OFF</button>
    </div>
  </section>

  <section class="panel">
    <h2>Voice Assistant</h2>
    <p class="small">Use your voice to control appliances.
       Voice recognition support depends on your browser.</p>

    <button onclick="startListening()">🎙 Start Voice Command</button>
    <button class="off full" onclick="stopListening()">Stop Listening</button>
    <button class="full" onclick="welcomeSpeech()">Welcome Message</button>

    <div class="status" id="voiceStatus">Voice assistant ready.</div>

    <label for="commandInput">Type a command</label>
    <input id="commandInput" placeholder="Example: turn on the fan">
    <button class="full" onclick="runTypedCommand()">Send Command</button>
  </section>

  <section class="panel">
    <h2>Text-to-Speech Settings</h2>

    <label for="accentSelect">English accent</label>
    <select id="accentSelect" onchange="previewSettings()">
      <option value="any">Automatic / any English accent</option>
      <option value="en-US">American English (US)</option>
      <option value="en-GB">British English (UK)</option>
    </select>

    <label for="genderSelect">Voice preference</label>
    <select id="genderSelect" onchange="previewSettings()">
      <option value="any">Any voice</option>
      <option value="female">Female-sounding voice</option>
      <option value="male">Male-sounding voice</option>
    </select>

    <label for="voiceSelect">Available voice on this device</label>
    <select id="voiceSelect" onchange="previewSettings()">
      <option value="">Automatic voice selection</option>
    </select>
    <p class="small">
      The list contains voices supplied by your iPhone or browser.
      Gender labels are approximate because voice names are not always
      labelled by gender.
    </p>

    <label for="rateRange">
      Speaking speed: <span id="rateValue" class="value">0.70x</span>
    </label>
    <input id="rateRange" type="range" min="0.45" max="1.25"
      step="0.05" value="0.70" oninput="updateLabels()">

    <label for="pitchRange">
      Voice pitch: <span id="pitchValue" class="value">1.00</span>
    </label>
    <input id="pitchRange" type="range" min="0.60" max="1.60"
      step="0.05" value="1.00" oninput="updateLabels()">

    <div class="grid full">
      <button onclick="testVoice()">Test Voice</button>
      <button onclick="saveVoiceSettings()">Save Settings</button>
    </div>
    <button class="off full" onclick="resetVoiceSettings()">Reset Defaults</button>

    <p class="small">
      Rate below 1.0 is slower. Pitch below 1.0 is lower;
      pitch above 1.0 is higher. Your selected settings are saved
      in this browser on this device.
    </p>
  </section>

  <section class="panel">
    <h2>About</h2>
    <p>RAMATECH HOME AI supports local appliance control, cloud status,
       speech recognition and spoken responses. Available voice options
       depend on your browser and installed system voices.</p>
  </section>
</main>

<script>
const STORAGE_KEY = "ramatechHomeAIVoiceSettings";

let currentStatus = {
  light:false, rope:false, fan:false, tv:false
};

let availableVoices = [];
let recognition = null;
let isListening = false;

const $ = id => document.getElementById(id);

function updateLabels() {
  $("rateValue").textContent =
    Number($("rateRange").value).toFixed(2) + "x";
  $("pitchValue").textContent =
    Number($("pitchRange").value).toFixed(2);
}

function getSettings() {
  return {
    accent: $("accentSelect").value,
    gender: $("genderSelect").value,
    voiceURI: $("voiceSelect").value,
    rate: Number($("rateRange").value),
    pitch: Number($("pitchRange").value)
  };
}

function loadSavedSettings() {
  try {
    const saved = JSON.parse(localStorage.getItem(STORAGE_KEY) || "null");
    if (!saved) return;

    if (saved.accent) $("accentSelect").value = saved.accent;
    if (saved.gender) $("genderSelect").value = saved.gender;
    if (saved.rate) $("rateRange").value = saved.rate;
    if (saved.pitch) $("pitchRange").value = saved.pitch;
  } catch (e) {
    console.log("Voice settings could not be loaded.");
  }
  updateLabels();
}

function saveVoiceSettings() {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(getSettings()));
    $("voiceStatus").textContent = "Voice settings saved on this device.";
  } catch (e) {
    $("voiceStatus").textContent =
      "Browser storage unavailable. Settings may not persist.";
  }
  updateVoiceList();
}

function resetVoiceSettings() {
  $("accentSelect").value = "any";
  $("genderSelect").value = "any";
  $("rateRange").value = "0.70";
  $("pitchRange").value = "1.00";
  $("voiceSelect").value = "";
  saveVoiceSettings();
  updateLabels();
  say("Voice settings have been reset. I will speak more slowly.");
}

function guessGender(voice) {
  const n = (voice.name || "").toLowerCase();

  if (/female|woman|samantha|victoria|karen|moira|tessa|fiona|zira|ava|susan|serena|kate|allison|joanna|salli|amy|emma|libby|sonia|aria|jenny|natasha/.test(n)) {
    return "female";
  }

  if (/male|man|daniel|alex|fred|david|james|george|oliver|thomas|arthur|guy|brian|ryan|lee|aaron/.test(n)) {
    return "male";
  }

  return "unknown";
}

function updateVoiceList() {
  if (!("speechSynthesis" in window)) {
    $("voiceStatus").textContent =
      "Text-to-speech is not supported by this browser.";
    return;
  }

  const previous = $("voiceSelect").value;
  availableVoices = window.speechSynthesis.getVoices();

  const select = $("voiceSelect");
  select.innerHTML = "";

  const automatic = document.createElement("option");
  automatic.value = "";
  automatic.textContent = "Automatic voice selection";
  select.appendChild(automatic);

  availableVoices.forEach((voice, index) => {
    const option = document.createElement("option");
    option.value = voice.voiceURI || String(index);
    option.textContent = voice.name + " (" + voice.lang + ")";
    select.appendChild(option);
  });

  const settings = (() => {
    try { return JSON.parse(localStorage.getItem(STORAGE_KEY) || "null"); }
    catch(e) { return null; }
  })();

  const wanted = previous || (settings && settings.voiceURI) || "";
  if (wanted && Array.from(select.options).some(o => o.value === wanted)) {
    select.value = wanted;
  }
}

if ("speechSynthesis" in window) {
  updateVoiceList();
  window.speechSynthesis.onvoiceschanged = updateVoiceList;
}

function selectVoice(settings) {
  if (!availableVoices.length) {
    availableVoices = window.speechSynthesis.getVoices();
  }

  if (settings.voiceURI) {
    const manual = availableVoices.find(v =>
      (v.voiceURI || "") === settings.voiceURI
    );
    if (manual) return manual;
  }

  let candidates = availableVoices.filter(v =>
    (v.lang || "").toLowerCase().startsWith("en")
  );

  if (settings.accent === "en-US") {
    candidates = candidates.filter(v =>
      (v.lang || "").toLowerCase().startsWith("en-us")
    );
  } else if (settings.accent === "en-GB") {
    candidates = candidates.filter(v =>
      (v.lang || "").toLowerCase().startsWith("en-gb")
    );
  }

  if (settings.gender !== "any") {
    const preferred = candidates.filter(v =>
      guessGender(v) === settings.gender
    );
    if (preferred.length) candidates = preferred;
  }

  return candidates[0] || availableVoices[0] || null;
}

function previewSettings() {
  updateLabels();
}

function say(message) {
  if (!("speechSynthesis" in window) ||
      !("SpeechSynthesisUtterance" in window)) {
    $("voiceStatus").textContent =
      "Text-to-speech is not supported by this browser.";
    return;
  }

  // Stop previous speech so messages do not overlap.
  window.speechSynthesis.cancel();

  const settings = getSettings();
  const utterance = new SpeechSynthesisUtterance(message);

  utterance.rate = settings.rate;
  utterance.pitch = settings.pitch;
  utterance.volume = 1;

  const voice = selectVoice(settings);
  if (voice) {
    utterance.voice = voice;
    utterance.lang = voice.lang || "en-US";
  } else {
    utterance.lang =
      settings.accent === "any" ? "en-US" : settings.accent;
  }

  utterance.onstart = () => {
    $("voiceStatus").textContent = "Speaking...";
  };
  utterance.onend = () => {
    $("voiceStatus").textContent = "Ready.";
  };
  utterance.onerror = (event) => {
    $("voiceStatus").textContent =
      "Speech error: " + (event.error || "unavailable");
  };

  window.speechSynthesis.speak(utterance);
}

function testVoice() {
  say("Hello Boss. This is RAMATECH HOME AI. I am speaking slowly so you can understand me clearly.");
}

function welcomeSpeech() {
  say("Welcome Boss. RAMATECH HOME AI is ready. You can control your light, rope light, fan and television using your voice or the control buttons.");
}

// ---------------- LOCAL CONTROL ----------------------

async function sendControl(device, state) {
  try {
    const response = await fetch(
      "/control?device=" + encodeURIComponent(device) +
      "&state=" + encodeURIComponent(state),
      {cache:"no-store"}
    );

    const data = await response.json();

    if (!response.ok || data.ok === false) {
      throw new Error(data.error || "Control request failed");
    }

    updateFromStatus(data);
    await refreshStatus();
    return true;
  } catch (e) {
    $("connectionStatus").textContent =
      "Local control error: " + e.message;
    return false;
  }
}

async function toggleDevice(device) {
  const current = !!currentStatus[device];
  const next = current ? "off" : "on";

  const ok = await sendControl(device, next);
  if (ok) {
    say(deviceName(device) + " turned " + next + ".");
  }
}

async function controlAll(state) {
  try {
    const response = await fetch("/all?state=" + state, {
      cache:"no-store"
    });
    const data = await response.json();

    if (!response.ok || data.ok === false) {
      throw new Error(data.error || "Command failed");
    }

    updateFromStatus(data);
    await refreshStatus();
    say(state === "on"
      ? "All appliances turned on."
      : "All appliances turned off.");
  } catch (e) {
    $("connectionStatus").textContent = "Control error: " + e.message;
  }
}

function deviceName(device) {
  const names = {
    light:"Light",
    rope:"Rope light",
    fan:"Fan",
    tv:"Television"
  };
  return names[device] || device;
}

function updateFromStatus(data) {
  if (!data) return;

  ["light", "rope", "fan", "tv"].forEach(key => {
    if (typeof data[key] !== "undefined") {
      currentStatus[key] = data[key] === true ||
        data[key] === 1 ||
        String(data[key]).toLowerCase() === "on" ||
        String(data[key]).toLowerCase() === "true";
    }
  });

  $("lightBtn").textContent =
    "Light: " + (currentStatus.light ? "ON" : "OFF");
  $("ropeBtn").textContent =
    "Rope Light: " + (currentStatus.rope ? "ON" : "OFF");
  $("fanBtn").textContent =
    "Fan: " + (currentStatus.fan ? "ON" : "OFF");
  $("tvBtn").textContent =
    "TV: " + (currentStatus.tv ? "ON" : "OFF");

  $("deviceStatus").textContent =
    "Light: " + (currentStatus.light ? "ON" : "OFF") +
    " | Rope: " + (currentStatus.rope ? "ON" : "OFF") +
    " | Fan: " + (currentStatus.fan ? "ON" : "OFF") +
    " | TV: " + (currentStatus.tv ? "ON" : "OFF");
}

async function refreshStatus() {
  try {
    const response = await fetch("/status", {cache:"no-store"});
    if (!response.ok) throw new Error("HTTP " + response.status);

    updateFromStatus(await response.json());
    $("connectionStatus").textContent = "ESP32 local connection: OK";
  } catch (e) {
    $("connectionStatus").textContent =
      "Cannot reach ESP32 local dashboard.";
  }
}

// ---------------- VOICE RECOGNITION ------------------

function startListening() {
  const SpeechRecognition =
    window.SpeechRecognition || window.webkitSpeechRecognition;

  if (!SpeechRecognition) {
    $("voiceStatus").textContent =
      "Speech recognition is unavailable in this browser. Try Safari or type a command.";
    say("Speech recognition is not available in this browser. You can type your command.");
    return;
  }

  try {
    recognition = new SpeechRecognition();
    recognition.lang =
      $("accentSelect").value === "any"
        ? "en-US"
        : $("accentSelect").value;
    recognition.continuous = false;
    recognition.interimResults = false;
    recognition.maxAlternatives = 1;

    recognition.onstart = () => {
      isListening = true;
      $("voiceStatus").textContent = "Listening. Speak your command.";
    };

    recognition.onresult = event => {
      const transcript = event.results[0][0].transcript;
      $("voiceStatus").textContent = "You said: " + transcript;
      processVoiceCommand(transcript);
    };

    recognition.onerror = event => {
      $("voiceStatus").textContent =
        "Voice recognition error: " + event.error;
    };

    recognition.onend = () => {
      isListening = false;
    };

    recognition.start();
  } catch (e) {
    $("voiceStatus").textContent =
      "Could not start listening. Try tapping the button again.";
  }
}

function stopListening() {
  if (recognition) {
    try { recognition.stop(); } catch(e) {}
  }
  isListening = false;
  $("voiceStatus").textContent = "Voice recognition stopped.";
}

function runTypedCommand() {
  const input = $("commandInput").value.trim();
  if (!input) {
    $("voiceStatus").textContent = "Enter a command first.";
    return;
  }
  processVoiceCommand(input);
}

async function displayCommandOnLCD(command) {
  try { await fetch("/display?text=" + encodeURIComponent(command), {cache:"no-store"}); }
  catch (e) {}
}

async function speakCurrentDateTime(which) {
  try {
    const response = await fetch("/time", {cache:"no-store"});
    const data = await response.json();
    if (!data.valid) {
      say("Sorry Boss, the clock is not synchronised yet. Please check Wi-Fi.");
      return;
    }
    if (which === "date") say("Today is " + data.date + ".");
    else if (which === "time") say("The current time is " + data.time + ".");
    else say("Today is " + data.date + ". The current time is " + data.time + ".");
    await displayCommandOnLCD(which === "date" ? data.date : which === "time" ? data.time : data.date + " " + data.time);
  } catch (e) {
    say("Sorry Boss, I could not retrieve the date and time. Connect your phone to the same Wi-Fi as the ESP32.");
  }
}

function processVoiceCommand(raw) {
  displayCommandOnLCD(raw);
  const text = raw.toLowerCase().replace(/[.,!?]/g, " ").trim();

  if ((text.includes("date") && text.includes("time")) ||
      text.includes("what is the date and time")) {
    speakCurrentDateTime("both"); return;
  }
  if ((text.includes("today") && (text.includes("date") || text.includes("day"))) ||
      text.includes("what is today's date") || text.includes("what is todays date")) {
    speakCurrentDateTime("date"); return;
  }
  if (text.includes("what time") || text.includes("current time") ||
      text === "time" || text.includes("tell me the time")) {
    speakCurrentDateTime("time"); return;
  }

  if (text.includes("how are you")) {
    say("I am working properly, Boss. Thank you for asking.");
    return;
  }

  if (text.includes("your name") || text.includes("who are you")) {
    say("My name is RAMATECH HOME AI, your smart home assistant.");
    return;
  }

  if (text.includes("who made you") ||
      text.includes("who developed you") ||
      text.includes("who programmed you")) {
    say("I was designed, programmed and developed by Ramatechcode Lab.");
    return;
  }

  if (text.includes("thank you") || text.includes("thanks")) {
    say("You are welcome, Boss.");
    return;
  }

  if (text.includes("hello") || text.includes("good morning") ||
      text.includes("good afternoon") || text.includes("good evening")) {
    say("Hello Boss. RAMATECH HOME AI is ready.");
    return;
  }

  if (text.includes("status") || text.includes("what is on") ||
      text.includes("what is off")) {
    refreshStatus();
    say("Here is the current appliance status. " +
        "Light " + (currentStatus.light ? "on" : "off") + ". " +
        "Rope light " + (currentStatus.rope ? "on" : "off") + ". " +
        "Fan " + (currentStatus.fan ? "on" : "off") + ". " +
        "Television " + (currentStatus.tv ? "on" : "off") + ".");
    return;
  }

  const wantsOn = /\b(on|open|start|activate|switch on|turn on)\b/.test(text);
  const wantsOff = /\b(off|close|stop|deactivate|switch off|turn off)\b/.test(text);

  if ((text.includes("all") || text.includes("everything")) &&
      (wantsOn || wantsOff)) {
    controlAll(wantsOn ? "on" : "off");
    return;
  }

  let device = "";

  if (text.includes("rope light") || text.includes("rope")) {
    device = "rope";
  } else if (text.includes("light")) {
    device = "light";
  } else if (text.includes("fan")) {
    device = "fan";
  } else if (text.includes("television") || /\btv\b/.test(text)) {
    device = "tv";
  }

  if (device && (wantsOn || wantsOff)) {
    const state = wantsOn ? "on" : "off";
    sendControl(device, state).then(ok => {
      if (ok) say("Alright Boss. " + deviceName(device) +
                  " turned " + state + ".");
    });
    return;
  }

  say("Sorry Boss, I did not understand that command. Please try again.");
}

// ---------------- STARTUP -----------------------------

loadSavedSettings();
updateVoiceList();
updateLabels();
refreshStatus();

setInterval(refreshStatus, 4000);
</script>
</body>
</html>
)rawliteral";

// ======================================================
// CLOUD MESSAGE HANDLING
// ======================================================

bool parseOnOff(JsonVariant value, bool defaultValue = false) {
  if (value.is<bool>()) return value.as<bool>();
  if (value.is<int>()) return value.as<int>() != 0;

  String text = value.as<String>();
  text.toLowerCase();

  if (text == "on" || text == "true" || text == "1") return true;
  if (text == "off" || text == "false" || text == "0") return false;

  return defaultValue;
}

void processCloudMessage(String message) {
  StaticJsonDocument<512> doc;

  DeserializationError error = deserializeJson(doc, message);
  if (error) {
    Serial.print("Cloud JSON error: ");
    Serial.println(error.c_str());
    return;
  }

  String type = doc["type"] | "";

  if (type == "auth_ok") {
    cloudAuthenticated = true;
    Serial.println("Cloud authentication successful.");
    sendCloudStatus();
    return;
  }

  if (type == "auth_error") {
    cloudAuthenticated = false;
    Serial.println("Cloud authentication rejected. Check token and device ID.");
    return;
  }

  if (type == "status_request") {
    sendCloudStatus();
    return;
  }

  if (type == "heartbeat") {
    return;
  }

  // Additive: show browser/cloud voice commands on the physical I2C LCD.
  if (type == "display") {
    String line1 = doc["line1"] | "VOICE COMMAND:";
    String line2 = doc["line2"] | "";
    displayMessage(line1, line2);
    return;
  }

  if (type == "all") {
    String stateText = doc["state"] | "";
    stateText.toLowerCase();

    if (stateText == "on" || stateText == "off") {
      setAll(stateText == "on");
    }
    return;
  }

  if (type == "command") {
    String device = doc["device"] | "";
    if (device.length() == 0) {
      device = doc["appliance"] | "";
    }

    bool turnOn = parseOnOff(doc["state"], false);

    if (device.length() > 0) {
      setDeviceState(device, turnOn);
    }
    return;
  }
}

void webSocketEvent(WStype_t event,
                    uint8_t* payload,
                    size_t length) {
  switch (event) {
    case WStype_DISCONNECTED:
      cloudAuthenticated = false;
      Serial.println("Cloud WebSocket disconnected.");
      break;

    case WStype_CONNECTED: {
      Serial.println("Cloud WebSocket connected.");

      StaticJsonDocument<192> doc;
      doc["type"] = "auth";
      doc["role"] = "device";
      doc["deviceId"] = DEVICE_ID;
      doc["token"] = DEVICE_TOKEN;

      String authMessage;
      serializeJson(doc, authMessage);
      cloudSocket.sendTXT(authMessage);
      break;
    }

    case WStype_TEXT: {
      String message;
      message.reserve(length + 1);

      for (size_t i = 0; i < length; i++) {
        message += (char)payload[i];
      }

      Serial.print("Cloud message: ");
      Serial.println(message);
      processCloudMessage(message);
      break;
    }

    case WStype_ERROR:
      Serial.println("Cloud WebSocket error.");
      break;

    default:
      break;
  }
}

// ======================================================
// WIFI AND CLOUD CONNECTION
// ======================================================

void connectWiFi() {
  Serial.print("Connecting to Wi-Fi: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Wi-Fi connected.");
    Serial.print("ESP32 IP address: ");
    Serial.println(WiFi.localIP());

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi Connected");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP().toString());
    delay(1800);
    showStatus();
  } else {
    Serial.println("Wi-Fi not connected. Will retry.");
  }
}

void startCloud() {
  if (WiFi.status() != WL_CONNECTED) return;

  Serial.println("Starting secure cloud WebSocket...");

  cloudSocket.onEvent(webSocketEvent);
  cloudSocket.setReconnectInterval(CLOUD_RETRY_MS);
  cloudSocket.enableHeartbeat(15000, 3000, 2);

  cloudSocket.beginSSL(CLOUD_HOST, CLOUD_PORT, CLOUD_PATH);

  lastCloudAttempt = millis();
}

// ======================================================
// SETUP
// ======================================================

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(RELAY_LIGHT, OUTPUT);
  pinMode(RELAY_ROPE, OUTPUT);
  pinMode(RELAY_FAN, OUTPUT);
  pinMode(RELAY_TV, OUTPUT);

  // Ensure all active-LOW relays start OFF
  digitalWrite(RELAY_LIGHT, RELAY_OFF);
  digitalWrite(RELAY_ROPE, RELAY_OFF);
  digitalWrite(RELAY_FAN, RELAY_OFF);
  digitalWrite(RELAY_TV, RELAY_OFF);

  lightState = false;
  ropeState = false;
  fanState = false;
  tvState = false;

  Wire.begin(LCD_SDA, LCD_SCL);
  lcd.init();
  lcd.backlight();
  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("RAMATECH HOME AI");
  lcd.setCursor(0, 1);
  lcd.print("Starting...");
  delay(1500);

  connectWiFi();
  // Nigeria / West Africa Time (WAT = UTC+1).
  configTime(3600, 0, "pool.ntp.org", "time.google.com", "time.nist.gov");

  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", INDEX_HTML);
  });

  server.on("/control", HTTP_GET, handleControl);
  server.on("/all", HTTP_GET, handleAll);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/display", HTTP_GET, handleDisplayCommand);
  server.on("/time", HTTP_GET, handleTime);

  server.onNotFound([]() {
    server.send(404, "text/plain", "Not found");
  });

  server.begin();
  Serial.println("Local web server started on port 80.");

  if (WiFi.status() == WL_CONNECTED) {
    startCloud();
  }

  updateOutputs();
}

// ======================================================
// MAIN LOOP
// ======================================================

void loop() {
  server.handleClient();
  refreshLCD();

  if (WiFi.status() == WL_CONNECTED) {
    cloudSocket.loop();

    if (lastHeartbeat == 0 ||
        millis() - lastHeartbeat >= HEARTBEAT_MS) {
      if (cloudAuthenticated) {
        StaticJsonDocument<128> doc;
        doc["type"] = "heartbeat";
        doc["deviceId"] = DEVICE_ID;

        String heartbeatMessage;
        serializeJson(doc, heartbeatMessage);
        cloudSocket.sendTXT(heartbeatMessage);
      }

      lastHeartbeat = millis();
    }

    if (cloudAuthenticated &&
        millis() - lastCloudStatus >= STATUS_MS) {
      sendCloudStatus();
      lastCloudStatus = millis();
    }
  } else {
    cloudAuthenticated = false;

    if (millis() - lastWiFiAttempt >= WIFI_RETRY_MS) {
      lastWiFiAttempt = millis();
      Serial.println("Wi-Fi disconnected. Reconnecting...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  }

  if (WiFi.status() == WL_CONNECTED &&
      !cloudAuthenticated &&
      millis() - lastCloudAttempt >= CLOUD_RETRY_MS) {
    // WebSocketsClient also handles its own reconnect attempts.
    lastCloudAttempt = millis();
  }

  delay(2);
}
