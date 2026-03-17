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

const uint32_t ON_MIN_MS  = 200;
const uint32_t ON_MAX_MS  = 60000;     // 60s
const uint32_t OFF_MIN_MS = 500;
const uint32_t OFF_MAX_MS = 600000;    // 10min

Preferences prefs;
volatile uint32_t onMs  = 1000;
volatile uint32_t offMs = 3000;

WebServer server(80);
uint32_t lastChange = 0;
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

void applyNow() {
  lastChange = millis();
}

void loadSettings() {
  prefs.begin("humid", true);
  onMs  = clampU32(prefs.getUInt("onMs",  1000), ON_MIN_MS,  ON_MAX_MS);
  offMs = clampU32(prefs.getUInt("offMs", 3000), OFF_MIN_MS, OFF_MAX_MS);
  prefs.end();
}

void saveSettings() {
  prefs.begin("humid", false);
  prefs.putUInt("onMs", onMs);
  prefs.putUInt("offMs", offMs);
  prefs.end();
}

String pageHtml() {
  String s;
  s.reserve(9000);

  s += "<!doctype html><html><head><meta charset='utf-8'>";
  s += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  s += "<title>Humidifier Timer</title>";
  s += "<style>";
  s += "body{font-family:system-ui;margin:24px;max-width:620px}";
  s += ".card{border:1px solid #ccc;border-radius:16px;padding:16px}";
  s += ".row{margin-top:16px}";
  s += "button{padding:14px 16px;font-size:20px;border-radius:14px;border:1px solid #bbb;background:#fff}";
  s += "button:active{transform:scale(0.99)}";
  s += "input[type=number]{padding:12px 12px;font-size:20px;border-radius:14px;border:1px solid #bbb;width:140px}";
  s += ".inline{display:flex;gap:10px;align-items:center;flex-wrap:wrap}";
  s += ".muted{opacity:.7;font-size:13px;margin-top:10px}";
  s += "code{background:#f2f2f2;padding:2px 6px;border-radius:8px}";
  s += "a{color:inherit}";
  s += "</style></head><body>";

  s += "<h2>Humidifier Timer</h2>";
  s += "<div class='card'>";
  s += "<div class='muted'><b>Wi-Fi:</b> " + String(AP_SSID) + " | <b>UI:</b> <code>http://192.168.4.1</code> | <a href='/update'>OTA</a></div>";

  // OFF
  s += "<div class='row'>";
  s += "<div style='font-size:18px'><b>OFF interval</b></div>";
  s += "<div class='inline'>";
  s += "<button type='button' id='offMinus'>−</button>";
  s += "<input id='off' type='number' inputmode='decimal' step='0.5' min='0.5' max='600'>";
  s += "<div style='font-size:18px'>s</div>";
  s += "<button type='button' id='offPlus'>+</button>";
  s += "</div>";
  s += "<div class='muted'>Time between sprays</div>";
  s += "</div>";

  // ON
  s += "<div class='row'>";
  s += "<div style='font-size:18px'><b>ON duration</b></div>";
  s += "<div class='inline'>";
  s += "<button type='button' id='onMinus'>−</button>";
  s += "<input id='on' type='number' inputmode='decimal' step='0.1' min='0.2' max='60'>";
  s += "<div style='font-size:18px'>s</div>";
  s += "<button type='button' id='onPlus'>+</button>";
  s += "</div>";
  s += "<div class='muted'>How long it sprays</div>";
  s += "</div>";

  s += "<div class='row inline'>";
  s += "<button type='button' onclick='applyNow()'><b>Apply</b></button>";
  s += "<button type='button' onclick='refreshVals()'>Refresh</button>";
  s += "</div>";

  s += "<div class='row' id='st' style='font-size:14px;opacity:.85'></div>";
  s += "</div>";

  // JS: hold-to-repeat steppers
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
  s += "      i=setInterval(fn,80);";  // repeat rate
  s += "    },350);";                // hold delay
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

  s += "attachHold('offMinus', ()=>step('off',-0.5));";
  s += "attachHold('offPlus',  ()=>step('off', 0.5));";
  s += "attachHold('onMinus',  ()=>step('on', -0.1));";
  s += "attachHold('onPlus',   ()=>step('on',  0.1));";

  s += "async function refreshVals(){";
  s += "  const r=await fetch('/json',{cache:'no-store'});";
  s += "  const j=await r.json();";
  s += "  $('off').value=round1(j.off/1000);";
  s += "  $('on').value=round1(j.on/1000);";
  s += "  $('st').textContent='Refreshed';";
  s += "}";
  s += "async function applyNow(){";
  s += "  $('st').textContent='Applying…';";
  s += "  const offMs=Math.round(parseFloat($('off').value)*1000);";
  s += "  const onMs=Math.round(parseFloat($('on').value)*1000);";
  s += "  const r=await fetch(`/set?off=${offMs}&on=${onMs}`,{cache:'no-store'});";
  s += "  $('st').textContent=await r.text();";
  s += "}";
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
  s += "\"on\":" + String(onMs);
  s += "}";
  server.send(200, "application/json", s);
}

void handleSet() {
  if (server.hasArg("off")) offMs = (uint32_t)server.arg("off").toInt();
  if (server.hasArg("on"))  onMs  = (uint32_t)server.arg("on").toInt();

  offMs = clampU32(offMs, OFF_MIN_MS, OFF_MAX_MS);
  onMs  = clampU32(onMs,  ON_MIN_MS,  ON_MAX_MS);

  saveSettings();
  applyNow();

  server.send(200, "text/plain",
    "Applied: OFF=" + String(offMs) + "ms ON=" + String(onMs) + "ms");
}

String otaPageHtml() {
  String s;
  s.reserve(3500);
  s += "<!doctype html><html><head><meta charset='utf-8'>";
  s += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  s += "<title>OTA Update</title>";
  s += "<style>";
  s += "body{font-family:system-ui;margin:24px;max-width:620px}";
  s += ".card{border:1px solid #ccc;border-radius:16px;padding:16px}";
  s += "input,button{font-size:18px}";
  s += "button{padding:12px 16px;border-radius:14px;border:1px solid #bbb;background:#fff}";
  s += ".muted{opacity:.7;font-size:13px;margin-top:10px}";
  s += "</style></head><body>";
  s += "<h2>OTA Update</h2>";
  s += "<div class='card'>";
  s += "<form method='POST' action='/update' enctype='multipart/form-data' onsubmit=\"document.getElementById('st').textContent='Uploading…';return true;\">";
  s += "<input type='file' name='firmware' accept='.bin' required>";
  s += "<div style='margin-top:12px;display:flex;gap:10px;flex-wrap:wrap'>";
  s += "<button type='submit'><b>Upload & Update</b></button>";
  s += "<a href='/' style='text-decoration:none'><button type='button'>Back</button></a>";
  s += "</div>";
  s += "</form>";
  s += "<div id='st' class='muted'></div>";
  s += "<div class='muted'>Upload the <b>.bin</b> you compiled for this ESP32-C3.</div>";
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
  // Android
  server.on("/generate_204", redirectToRoot);
  server.on("/gen_204", redirectToRoot);

  // iOS/macOS
  server.on("/hotspot-detect.html", redirectToRoot);
  server.on("/library/test/success.html", redirectToRoot);

  // Windows
  server.on("/ncsi.txt", redirectToRoot);
  server.on("/connecttest.txt", redirectToRoot);

  // Anything else -> redirect
  server.onNotFound([]() { redirectToRoot(); });
}

void setup() {
  pinMode(SIG_PIN, OUTPUT);
  digitalWrite(SIG_PIN, HIGH);

  loadSettings();

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, netMsk);
  WiFi.softAP(AP_SSID, AP_PASS);

  dnsServer.start(DNS_PORT, "*", apIP);

  server.on("/", handleRoot);
  server.on("/json", handleJson);
  server.on("/set", handleSet);

  server.on("/update", HTTP_GET, handleUpdatePage);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);

  setupCaptiveEndpoints();
  server.begin();

  lastChange = millis();
  applyNow();
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();

  uint32_t now = millis();
  uint32_t localOn  = onMs;
  uint32_t localOff = offMs;

  if (!mistOn) {
    if (now - lastChange >= localOff) {
      mistOn = true;
      lastChange = now;
      pulseFallingEdge(); // toggle ON
    }
  } else {
    if (now - lastChange >= localOn) {
      mistOn = false;
      lastChange = now;
      pulseFallingEdge(); // toggle OFF
    }
  }
}
