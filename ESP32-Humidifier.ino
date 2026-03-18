#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Update.h>

#define SIG_PIN   6
#define PULSE_MS  50

const char* AP_SSID = "HumidifierTimer";
const char* AP_PASS = "humidifier123";

const byte DNS_PORT = 53;
DNSServer dnsServer;
IPAddress apIP(192,168,4,1);
IPAddress netMsk(255,255,255,0);

const uint32_t ON_MIN_MS  = 500;
const uint32_t ON_MAX_MS  = 60000;
const uint32_t OFF_MIN_MS = 500;
const uint32_t OFF_MAX_MS = 600000;
const uint32_t AUTOOFF_MIN_MS = 1800000;
const uint32_t AUTOOFF_MAX_MS = 86400000;
const uint8_t PATTERN_REGULAR = 0;
const uint8_t PATTERN_HEARTBEAT = 1;
const uint8_t PATTERN_MACHINEGUN = 2;
const uint8_t PATTERN_TRAIN = 3;
const uint8_t PATTERN_MAX = PATTERN_TRAIN;
const uint32_t TRAIN_TICK_MS = 50; // base tick for train pattern
const uint8_t TRAIN_PATTERN_LEN = 35;
const uint8_t TRAIN_PATTERN[TRAIN_PATTERN_LEN] = {
  // pair 1
  1,0,0,0,1,
  // inter-pair gap
  0,0,0,0,0,
  // pair 2
  1,0,0,0,1,
  // big gap (20 zeros)
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

Preferences prefs;
volatile uint32_t onMs  = 1000;
volatile uint32_t offMs = 3000;
volatile uint32_t autoOffMs = 3600000;
volatile bool humidifierEnabled = true;
volatile bool autoOffEnabled = false;
volatile uint8_t patternMode = PATTERN_REGULAR;
volatile uint8_t patternPhase = 0;
volatile uint8_t trainIdx = 0;

WebServer server(80);
uint32_t lastChange = 0;
uint32_t autoOffStart = 0;
bool mistOn = false;

static uint32_t clampU32(uint32_t v, uint32_t lo, uint32_t hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

void pulseFallingEdge() {
  digitalWrite(SIG_PIN, LOW);
  delay(PULSE_MS);
  digitalWrite(SIG_PIN, HIGH);
}

void forceMistOff() {
  if (mistOn) {
    pulseFallingEdge();
    mistOn = false;
  }
}

void applyNow() {
  if (!humidifierEnabled) {
    forceMistOff();
  }
  lastChange = millis();
  patternPhase = 0;
  trainIdx = 0;
  if (humidifierEnabled && autoOffEnabled) {
    autoOffStart = lastChange;
  }
}

void loadSettings() {
  prefs.begin("humid", true);
  onMs  = clampU32(prefs.getUInt("onMs",  1000), ON_MIN_MS,  ON_MAX_MS);
  offMs = clampU32(prefs.getUInt("offMs", 3000), OFF_MIN_MS, OFF_MAX_MS);
  autoOffMs = clampU32(prefs.getUInt("autoOffMs", 3600000), AUTOOFF_MIN_MS, AUTOOFF_MAX_MS);
  humidifierEnabled = prefs.getBool("enabled", true);
  autoOffEnabled = prefs.getBool("autoOffEnabled", false);
  patternMode = (uint8_t)clampU32(prefs.getUInt("pattern", 0), PATTERN_REGULAR, PATTERN_MAX);
  patternPhase = 0;
  trainIdx = 0;
  prefs.end();
}

void saveSettings() {
  prefs.begin("humid", false);
  prefs.putUInt("onMs", onMs);
  prefs.putUInt("offMs", offMs);
  prefs.putUInt("autoOffMs", autoOffMs);
  prefs.putBool("enabled", humidifierEnabled);
  prefs.putBool("autoOffEnabled", autoOffEnabled);
  prefs.putUInt("pattern", patternMode);
  prefs.end();
}

String pageHtml() {
  String s;
  s.reserve(9000);

  s += "<!doctype html><html><head><meta charset='utf-8'>";
  s += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  s += "<title>Humidifier Timer</title>";
  s += "<style>";
  s += ":root{--bg:#222222;--surface:#2a2a2a;--surfaceAlt:#333333;--border:#444444;--borderStrong:#555555;--text:#ffffff;--muted:rgba(255,255,255,0.68)}";
  s += "body{font-family:system-ui;margin:24px;max-width:620px;background:var(--bg);color:var(--text)}";
  s += "h2{margin-bottom:12px}";
  s += ".card{border:1px solid var(--border);border-radius:16px;padding:16px;background:var(--surface);box-shadow:0 12px 28px rgba(0,0,0,0.18)}";
  s += ".row{margin-top:16px}";
  s += "button{padding:14px 16px;font-size:20px;border-radius:14px;border:1px solid var(--borderStrong);background:var(--surfaceAlt);color:var(--text)}";
  s += "button:active{transform:scale(0.98)}";
  s += "input[type=number]{padding:12px 12px;font-size:20px;border-radius:14px;border:1px solid var(--borderStrong);background:var(--surfaceAlt);color:var(--text);width:140px}";
  s += "select{padding:12px;font-size:18px;border-radius:14px;border:1px solid var(--borderStrong);background:var(--surfaceAlt);color:var(--text)}";
  s += ".inline{display:flex;gap:10px;align-items:center;flex-wrap:wrap}";
  s += ".muted{color:var(--muted);font-size:13px;margin-top:10px}";
  s += "code{background:var(--surfaceAlt);padding:2px 6px;border-radius:8px;border:1px solid var(--border)}";
  s += "a{color:var(--text)}";
  s += ".powerBlock{display:flex;flex-direction:column;align-items:flex-start;gap:8px;margin-top:18px}";
  s += ".autoBlock{display:flex;flex-direction:column;align-items:flex-start;gap:8px;margin-top:10px;margin-bottom:12px}";
  s += ".powerLabel{font-size:18px;font-weight:600}";
  s += ".sectionTitle{margin-bottom:8px;display:block}";
  s += ".switch{position:relative;display:inline-block;width:56px;height:32px;flex:0 0 auto}";
  s += ".switch input{opacity:0;width:0;height:0}";
  s += ".slider{position:absolute;cursor:pointer;inset:0;background:#4b5563;border:1px solid var(--borderStrong);transition:.2s;border-radius:999px}";
  s += ".slider:before{position:absolute;content:'';height:24px;width:24px;left:3px;top:3px;background:var(--text);transition:.2s;border-radius:50%}";
  s += "input:checked+.slider{background:#0f766e}";
  s += "input:checked+.slider:before{transform:translateX(24px)}";
  s += "</style></head><body>";

  s += "<h2>Humidifier Timer</h2>";
  s += "<div class='card'>";
  s += "<div class='muted'><b>Wi-Fi:</b> " + String(AP_SSID) + " | <b>UI:</b> <code>http://192.168.4.1</code> | <a href='/update'>OTA</a></div>";
  s += "<div class='powerBlock'>";
  s += "<div class='powerLabel'>Humidifier Power</div>";
  s += "<label class='switch'><input id='enabled' type='checkbox'><span class='slider'></span></label>";
  s += "</div>";

  s += "<div class='row'>";
  s += "<div class='sectionTitle' style='font-size:18px'><b>OFF interval</b></div>";
  s += "<div class='inline'>";
  s += "<button type='button' id='offMinus'>-</button>";
  s += "<input id='off' type='number' inputmode='decimal' step='0.5' min='0.5' max='600'>";
  s += "<div style='font-size:18px'>s</div>";
  s += "<button type='button' id='offPlus'>+</button>";
  s += "</div>";
  s += "<div class='muted'>Time between sprays</div>";
  s += "</div>";

  s += "<div class='row'>";
  s += "<div class='sectionTitle' style='font-size:18px'><b>ON duration</b></div>";
  s += "<div class='inline'>";
  s += "<button type='button' id='onMinus'>-</button>";
  s += "<input id='on' type='number' inputmode='decimal' step='0.5' min='0.5' max='60'>";
  s += "<div style='font-size:18px'>s</div>";
  s += "<button type='button' id='onPlus'>+</button>";
  s += "</div>";
  s += "<div class='muted'>How long it sprays</div>";
  s += "</div>";

  s += "<div class='row'>";
  s += "<div style='font-size:18px'><b>Auto-off timer</b></div>";
  s += "<div class='autoBlock'>";
  s += "<label class='switch'><input id='autoOffEnabled' type='checkbox'><span class='slider'></span></label>";
  s += "</div>";
  s += "<div class='inline'>";
  s += "<button type='button' id='autoOffMinus'>-</button>";
  s += "<input id='autoOff' type='number' inputmode='decimal' step='30' min='30' max='1440'>";
  s += "<div style='font-size:18px'>min</div>";
  s += "<button type='button' id='autoOffPlus'>+</button>";
  s += "</div>";
  s += "<div class='muted'>Turns humidifier off after the set time.</div>";
  s += "</div>";

  s += "<div class='row'>";
  s += "<div class='sectionTitle' style='font-size:18px'><b>Patterns</b></div>";
  s += "<div class='inline'>";
  s += "<select id='pattern'>";
  s += "<option value='0'>Regular</option>";
  s += "<option value='1'>Heartbeat</option>";
  s += "<option value='2'>Machine gun</option>";
  s += "<option value='3'>Train Tracks</option>";
  s += "</select>";
  s += "</div>";
  s += "<div class='muted'>Regular uses your OFF/ON values. Heartbeat doubles short pulses with a rest. Machine gun pulses very rapidly.</div>";
  s += "</div>";

  s += "<div class='row inline'>";
  s += "<button type='button' onclick='applyNow()'><b>Apply</b></button>";
  s += "<button type='button' onclick='refreshVals()'>Refresh</button>";
  s += "<button type='button' onclick='hardReset()'>Hard Reset</button>";
  s += "</div>";

  s += "<div class='row' id='st' style='font-size:14px;opacity:.85'></div>";
  s += "</div>";

  s += "<script>";
  s += "const $=id=>document.getElementById(id);";
  s += "function round1(x){return Math.round(x*10)/10;}";
  s += "function clamp(x,min,max){return Math.max(min,Math.min(max,x));}";
  s += "function step(which,delta){";
  s += "  const el=$(which);";
  s += "  const v=parseFloat(el.value||'0');";
  s += "  const mn=parseFloat(el.min); const mx=parseFloat(el.max);";
  s += "  el.value=round1(clamp(v+delta,mn,mx));";
  s += "}";
  s += "function attachHold(btnId, fn){";
  s += "  const b=$(btnId);";
  s += "  let t=null; let i=null;";
  s += "  const start=()=>{";
  s += "    fn();";
  s += "    t=setTimeout(()=>{";
  s += "      i=setInterval(fn,80);";
  s += "    },350);";
  s += "  };";
  s += "  const stop=()=>{";
  s += "    if(t){clearTimeout(t); t=null;}";
  s += "    if(i){clearInterval(i); i=null;}";
  s += "  };";
  s += "  b.addEventListener('pointerdown', (e)=>{e.preventDefault(); start();});";
  s += "  b.addEventListener('pointerup', stop);";
  s += "  b.addEventListener('pointercancel', stop);";
  s += "  b.addEventListener('pointerleave', stop);";
  s += "};";
  s += "function blockSigns(id){";
  s += "  const el=$(id);";
  s += "  el.addEventListener('keydown', e=>{";
  s += "    if(['e','E','+','-'].includes(e.key)) e.preventDefault();";
  s += "  });";
  s += "  el.addEventListener('input', e=>{";
  s += "    const cleaned=el.value.replace(/[eE\\+\\-]/g,'');";
  s += "    if(cleaned!==el.value) el.value=cleaned;";
  s += "  });";
  s += "}";
  s += "blockSigns('off');";
  s += "blockSigns('on');";
  s += "blockSigns('autoOff');";

  s += "attachHold('offMinus', ()=>step('off',-0.5));";
  s += "attachHold('offPlus',  ()=>step('off', 0.5));";
  s += "attachHold('onMinus',  ()=>step('on', -0.5));";
  s += "attachHold('onPlus',   ()=>step('on',  0.5));";
  s += "attachHold('autoOffMinus', ()=>step('autoOff', -30));";
  s += "attachHold('autoOffPlus',  ()=>step('autoOff',  30));";

  s += "async function refreshVals(){";
  s += "  const r=await fetch('/json',{cache:'no-store'});";
  s += "  const j=await r.json();";
  s += "  $('off').value=round1(j.off/1000);";
  s += "  $('on').value=round1(j.on/1000);";
  s += "  $('enabled').checked=!!j.enabled;";
  s += "  $('autoOff').value=round1(j.autoOff/60000);";
  s += "  $('autoOffEnabled').checked=!!j.autoOffEnabled;";
  s += "  $('pattern').value=j.patternMode;";
  s += "  $('st').textContent='Refreshed';";
  s += "}";
  s += "async function hardReset(){";
  s += "  $('st').textContent='Resetting...';";
  s += "  const r=await fetch('/reset',{cache:'no-store'});";
  s += "  $('st').textContent=await r.text();";
  s += "  await refreshVals();";
  s += "}";
  s += "async function applyNow(){";
  s += "  $('st').textContent='Applying...';";
  s += "  const offMs=Math.round(parseFloat($('off').value)*1000);";
  s += "  const onMs=Math.round(parseFloat($('on').value)*1000);";
  s += "  const enabled=$('enabled').checked?1:0;";
  s += "  const autoOffMs=Math.round(parseFloat($('autoOff').value)*60000);";
  s += "  const autoEnabled=$('autoOffEnabled').checked?1:0;";
  s += "  const pattern=$('pattern').value;";
  s += "  const r=await fetch(`/set?off=${offMs}&on=${onMs}&enabled=${enabled}&autoOff=${autoOffMs}&autoOffEnabled=${autoEnabled}&pattern=${pattern}`,{cache:'no-store'});";
  s += "  $('st').textContent=await r.text();";
  s += "}";
  s += "$('enabled').addEventListener('change', applyNow);";
  s += "$('autoOffEnabled').addEventListener('change', applyNow);";
  s += "refreshVals();";
  s += "</script>";

  s += "</body></html>";
  return s;
}

void redirectToRoot() {
  server.sendHeader("Location", String("http://") + apIP.toString() + "/", true);
  server.send(302, "text/plain", "Redirecting...");
}

void handleRoot() { server.send(200, "text/html", pageHtml()); }

void handleJson() {
  String s = "{";
  s += "\"off\":" + String(offMs) + ",";
  s += "\"on\":" + String(onMs) + ",";
  s += "\"autoOff\":" + String(autoOffMs) + ",";
  s += "\"enabled\":";
  s += humidifierEnabled ? "true" : "false";
  s += ",\"autoOffEnabled\":";
  s += autoOffEnabled ? "true" : "false";
  s += ",\"patternMode\":";
  s += String(patternMode);
  s += "}";
  server.send(200, "application/json", s);
}

void handleReset() {
  onMs = 1000;
  offMs = 60000;
  autoOffMs = 3600000;
  autoOffEnabled = false;
  humidifierEnabled = true;
  patternMode = PATTERN_REGULAR;
  patternPhase = 0;
  trainIdx = 0;

  mistOn = true;
  forceMistOff();

  lastChange = millis();
  autoOffStart = lastChange;

  saveSettings();
  applyNow();

  server.send(200, "text/plain",
    String("Reset: ") +
    "OFF=" + String(offMs) + "ms ON=" + String(onMs) + "ms" +
    " | AutoOff=off");
}

void handleSet() {
  if (server.hasArg("off")) offMs = (uint32_t)server.arg("off").toInt();
  if (server.hasArg("on"))  onMs  = (uint32_t)server.arg("on").toInt();
  if (server.hasArg("enabled")) humidifierEnabled = server.arg("enabled") != "0";
  if (server.hasArg("autoOff")) autoOffMs = (uint32_t)server.arg("autoOff").toInt();
  if (server.hasArg("autoOffEnabled")) autoOffEnabled = server.arg("autoOffEnabled") != "0";
  if (server.hasArg("pattern")) patternMode = (uint8_t)server.arg("pattern").toInt();
  patternPhase = 0;
  trainIdx = 0;

  offMs = clampU32(offMs, OFF_MIN_MS, OFF_MAX_MS);
  onMs  = clampU32(onMs,  ON_MIN_MS,  ON_MAX_MS);
  autoOffMs = clampU32(autoOffMs, AUTOOFF_MIN_MS, AUTOOFF_MAX_MS);

  saveSettings();
  applyNow();

  server.send(200, "text/plain",
    String(humidifierEnabled ? "Enabled" : "Disabled") +
    " | OFF=" + String(offMs) + "ms ON=" + String(onMs) + "ms" +
    " | AutoOff=" + (autoOffEnabled ? String(autoOffMs/60000) + "min" : String("off")));
}

String otaPageHtml() {
  String s;
  s.reserve(3500);
  s += "<!doctype html><html><head><meta charset='utf-8'>";
  s += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  s += "<title>OTA Update</title>";
  s += "<style>";
  s += ":root{--bg:#222222;--surface:#2a2a2a;--surfaceAlt:#333333;--border:#444444;--borderStrong:#555555;--text:#ffffff;--muted:rgba(255,255,255,0.68)}";
  s += "body{font-family:system-ui;margin:24px;max-width:620px;background:var(--bg);color:var(--text)}";
  s += "h2{margin-bottom:12px}";
  s += ".card{border:1px solid var(--border);border-radius:16px;padding:16px;background:var(--surface);box-shadow:0 12px 28px rgba(0,0,0,0.18)}";
  s += "input,button{font-size:18px}";
  s += "input[type=file]{width:100%;box-sizing:border-box;padding:12px;border-radius:14px;border:1px solid var(--borderStrong);background:var(--surfaceAlt);color:var(--text)}";
  s += "button{padding:12px 16px;border-radius:14px;border:1px solid var(--borderStrong);background:var(--surfaceAlt);color:var(--text)}";
  s += ".alert{background:rgba(127,29,29,0.22);border:1px solid rgba(185,28,28,0.45);color:#ffffff;padding:12px 14px;border-radius:12px;margin-top:12px}";
  s += ".success{background:rgba(20,83,45,0.22);border:1px solid rgba(34,197,94,0.45);color:#ffffff;padding:12px 14px;border-radius:12px;margin-top:10px}";
  s += ".muted{color:var(--muted);font-size:13px;margin-top:10px}";
  s += "a{color:var(--text)}";
  s += "</style></head><body>";
  s += "<h2>OTA Update</h2>";
  s += "<div class='card'>";
  s += "<div class='alert'><b>Warning:</b> Only upload authentic firmware built for this item. Using the wrong <b>.bin</b> can fail or leave the device <b>bricked</b>.</div>";
  s += "<div class='success'>Recommended <b>.bin</b> files and future updates: <a href='https://github.com/Crucial-hash/ESP32-Humidifier' target='_blank' rel='noopener noreferrer'>github.com/Crucial-hash/ESP32-Humidifier</a></div>";
  s += "<form method='POST' action='/update' enctype='multipart/form-data' style='margin-top:16px' onsubmit=\"document.getElementById('st').textContent='Uploading...';return true;\">";
  s += "<input type='file' name='firmware' accept='.bin' required>";
  s += "<div style='margin-top:12px;display:flex;gap:10px;flex-wrap:wrap'>";
  s += "<button type='submit'><b>Upload & Update</b></button>";
  s += "<a href='/' style='text-decoration:none'><button type='button'>Back</button></a>";
  s += "</div>";
  s += "</form>";
  s += "<div id='st' class='muted'></div>";
  s += "</div></body></html>";
  return s;
}

void handleUpdatePage() {
  server.send(200, "text/html", otaPageHtml());
}

void handleUpdateUpload() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    Update.begin(UPDATE_SIZE_UNKNOWN);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    Update.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    Update.end(true);
  }
}

void handleUpdateDone() {
  if (Update.hasError()) {
    server.send(500, "text/plain", "Update failed.");
  } else {
    server.send(200, "text/plain", "Update OK. Rebooting...");
    delay(300);
    ESP.restart();
  }
}

void setupCaptiveEndpoints() {
  server.on("/generate_204", redirectToRoot);
  server.on("/gen_204", redirectToRoot);
  server.on("/hotspot-detect.html", redirectToRoot);
  server.on("/library/test/success.html", redirectToRoot);
  server.on("/ncsi.txt", redirectToRoot);
  server.on("/connecttest.txt", redirectToRoot);
  server.onNotFound([]() { redirectToRoot(); });
}

void setup() {
  pinMode(SIG_PIN, OUTPUT);
  digitalWrite(SIG_PIN, HIGH);
  mistOn = true;
  forceMistOff();

  loadSettings();

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, netMsk);
  WiFi.softAP(AP_SSID, AP_PASS);

  dnsServer.start(DNS_PORT, "*", apIP);

  server.on("/", handleRoot);
  server.on("/json", handleJson);
  server.on("/set", handleSet);
  server.on("/reset", handleReset);

  server.on("/update", HTTP_GET, handleUpdatePage);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);

  setupCaptiveEndpoints();
  server.begin();

  lastChange = millis();
  applyNow();
}

uint32_t heartbeatDuration(bool stateOn, uint8_t phase) {
  // phase cycles 0..3: off-short, on-short, off-long, on-short
  if (stateOn) {
    return (phase % 4 == 1 || phase % 4 == 3) ? 300 : 300;
  }
  return (phase % 4 == 0) ? 400 : 2500;
}

uint32_t machineGunDuration(bool stateOn) {
  return stateOn ? 120 : 120;
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();

  uint32_t now = millis();
  uint32_t localOn  = onMs;
  uint32_t localOff = offMs;
  bool localEnabled = humidifierEnabled;
  uint32_t localAutoOff = autoOffMs;
  bool localAutoEnabled = autoOffEnabled;
  uint8_t localPattern = patternMode;

  if (!localEnabled) {
    return;
  }

  if (localAutoEnabled && (now - autoOffStart >= localAutoOff)) {
    humidifierEnabled = false;
    saveSettings();
    applyNow();
    return;
  }

  if (mistOn && (now - lastChange > localOn + 5000)) {
    mistOn = false;
    lastChange = now;
    pulseFallingEdge();
  }

  uint32_t targetDuration = 0;
  if (localPattern == PATTERN_HEARTBEAT) {
    targetDuration = heartbeatDuration(mistOn, patternPhase);
    if (now - lastChange >= targetDuration) {
      mistOn = !mistOn;
      lastChange = now;
      pulseFallingEdge();
      patternPhase = (patternPhase + 1) % 4;
    }
  } else if (localPattern == PATTERN_MACHINEGUN) {
    targetDuration = machineGunDuration(mistOn);
    if (now - lastChange >= targetDuration) {
      mistOn = !mistOn;
      lastChange = now;
      pulseFallingEdge();
    }
  } else if (localPattern == PATTERN_TRAIN) {
    if (now - lastChange >= TRAIN_TICK_MS) {
      lastChange = now;
      uint8_t desired = TRAIN_PATTERN[trainIdx];
      if (mistOn != desired) {
        mistOn = desired;
        pulseFallingEdge();
      }
      trainIdx = (trainIdx + 1) % TRAIN_PATTERN_LEN;
    }
  } else { // regular
    targetDuration = mistOn ? localOn : localOff;
    if (now - lastChange >= targetDuration) {
      mistOn = !mistOn;
      lastChange = now;
      pulseFallingEdge();
    }
  }
}

