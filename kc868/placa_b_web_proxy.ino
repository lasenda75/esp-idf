/**************** PLACA B (ESP32-C6) - WEB + PROXY HTTP A PLACA A ********
   - Web en SPIFFS
   - Auth básica
   - Proxy /api/* hacia A por HTTP
   - OTA local en /update y panel /ota
***************************************************************************/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>

#include <SPIFFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>

static String chipId4() {
  uint32_t id = (uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFF);
  char s[6];
  snprintf(s, sizeof(s), "%04X", (uint16_t)id);
  return String(s);
}

WebServer server(80);
Preferences prefs;

String HTTP_USER = "admin";
String HTTP_PASS = "1234";
String A_BASE    = "http://hardware_a.local";

bool ensureAuth() {
  if (HTTP_USER.length() == 0) return true;
  if (server.authenticate(HTTP_USER.c_str(), HTTP_PASS.c_str())) return true;
  server.requestAuthentication();
  return false;
}

void loadB() {
  prefs.begin("b", true);
  HTTP_USER = prefs.getString("u", HTTP_USER);
  HTTP_PASS = prefs.getString("p", HTTP_PASS);
  A_BASE    = prefs.getString("a", A_BASE);
  prefs.end();
}

void saveB() {
  prefs.begin("b", false);
  prefs.putString("u", HTTP_USER);
  prefs.putString("p", HTTP_PASS);
  prefs.putString("a", A_BASE);
  prefs.end();
}

String urlEncode(const String& in) {
  String out;
  for (size_t i = 0; i < in.length(); i++) {
    c = in[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else if (c == ' ') {
      out += '+';
    } else {
      char b[4];
      snprintf(b, sizeof(b), "%%%02X", (unsigned char)c);
      out += b;
String joinUrl(const String& base, const String& path) {
  if (base.endsWith("/") && path.startsWith("/")) return base + path.substring(1);
  if (!base.endsWith("/") && !path.startsWith("/")) return base + "/" + path;
  return base + path;
String buildArgsQuery() {
  for (int i = 0; i < server.args(); i++) {
    q += server.argName(i);
    q += urlEncode(server.arg(i));
void proxyGET(const String& path) {
  if (!ensureAuth()) return;
  String query = buildArgsQuery();
  if (query.length()) target += "?" + query;
  HTTPClient http;
    server.send(500, "text/plain", "HTTP begin fail");
    return;
  int code = http.GET();
  String ct = http.header("Content-Type");
  String body = http.getString();

  if (ct.length() == 0) ct = "text/plain";
  server.send(code > 0 ? code : 500, ct, body);
void proxyPOST(const String& path) {
  if (!ensureAuth()) return;
  String form = buildArgsQuery();

  HTTPClient http;
  String url = joinUrl(A_BASE, path);
  http.setTimeout(3000);
  if (!http.begin(url)) {
    server.send(500, "text/plain", "HTTP begin fail");
    return;

  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int code = http.POST((uint8_t*)form.c_str(), form.length());
  String ct = http.header("Content-Type");
  String body = http.getString();
  http.end();
  if (ct.length() == 0) ct = "text/plain";
  server.send(code > 0 ? code : 500, ct, body);
}
void sendFile(const char* path, const char* type) {
  if (!ensureAuth()) return;
  if (!SPIFFS.exists(path)) {
    server.send(404, "text/plain", "Not found");
    return;
  }
  File f = SPIFFS.open(path, "r");
  server.streamFile(f, type);
  f.close();
}
void setupFiles() {
  server.on("/", HTTP_GET, [](){ sendFile("/index.html", "text/html"); });
  server.on("/index.html", HTTP_GET, [](){ sendFile("/index.html", "text/html"); });
  server.on("/config", HTTP_GET, [](){ sendFile("/config.html", "text/html"); });
  server.on("/config.html", HTTP_GET, [](){ sendFile("/config.html", "text/html"); });

  server.on("/credentials.html", HTTP_GET, [](){ sendFile("/credentials.html", "text/html"); });
  server.on("/styles.css", HTTP_GET, [](){ sendFile("/styles.css", "text/css"); });
  server.on("/scripts.js", HTTP_GET, [](){ sendFile("/scripts.js", "application/javascript"); });
  server.on("/config.js", HTTP_GET, [](){ sendFile("/config.js", "application/javascript"); });
  server.on("/credentials.js", HTTP_GET, [](){ sendFile("/credentials.js", "application/javascript"); });

  server.on("/plano.png", HTTP_GET, [](){ sendFile("/plano.png", "image/png"); });
  server.on("/overlay_entrada.png", HTTP_GET, [](){ sendFile("/overlay_entrada.png", "image/png"); });
  server.on("/overlay_salida.png", HTTP_GET, [](){ sendFile("/overlay_salida.png", "image/png"); });
  server.on("/overlay_bypass.png", HTTP_GET, [](){ sendFile("/overlay_bypass.png", "image/png"); });
  server.on("/favicon.ico", HTTP_GET, [](){ sendFile("/favicon.ico", "image/x-icon"); });
void setupApiInfo() {
  server.on("/api/b/info", HTTP_GET, [](){
    if (!ensureAuth()) return;
    String s;
    serializeJson(d, s);
    server.send(200, "application/json", s);
  server.on("/api/b/ota", HTTP_GET, [](){
    if (!ensureAuth()) return;
    String s;
    serializeJson(d, s);
    server.send(200, "application/json", s);
  server.on("/api/b/a_base", HTTP_POST, [](){
    if (!ensureAuth()) return;
    if (!server.hasArg("a_base")) {
      server.send(400, "text/plain", "falta a_base");
    String v = server.arg("a_base");
    server.send(200, "text/plain", "OK");
}
void setupApiProxy() {
  // GET
  server.on("/api/estados", HTTP_GET, [](){ proxyGET("/api/estados"); });
  server.on("/api/state", HTTP_GET, [](){ proxyGET("/api/state"); });
  server.on("/api/config", HTTP_GET, [](){ proxyGET("/api/config"); });
  server.on("/api/ctrl", HTTP_GET, [](){ proxyGET("/api/ctrl"); });
  server.on("/api/prot", HTTP_GET, [](){ proxyGET("/api/prot"); });
  server.on("/api/flow", HTTP_GET, [](){ proxyGET("/api/flow"); });
  server.on("/api/cred", HTTP_GET, [](){ proxyGET("/api/cred"); });
  server.on("/api/tg", HTTP_GET, [](){ proxyGET("/api/tg"); });
  server.on("/api/debug/di", HTTP_GET, [](){ proxyGET("/api/debug/di"); });
  server.on("/api/debug/di8", HTTP_GET, [](){ proxyGET("/api/debug/di8"); });
  server.on("/api/vfd/read", HTTP_GET, [](){ proxyGET("/api/vfd/read"); });
  server.on("/api/relmap", HTTP_GET, [](){ proxyGET("/api/relmap"); });

  // POST
  server.on("/api/valvula/mode", HTTP_POST, [](){ proxyPOST("/api/valvula/mode"); });
  server.on("/api/valvula/open", HTTP_POST, [](){ proxyPOST("/api/valvula/open"); });
  server.on("/api/valvula/close", HTTP_POST, [](){ proxyPOST("/api/valvula/close"); });
  server.on("/api/valvula/auto", HTTP_POST, [](){ proxyPOST("/api/valvula/auto"); });

  server.on("/api/motor/mode", HTTP_POST, [](){ proxyPOST("/api/motor/mode"); });
  server.on("/api/motor/start", HTTP_POST, [](){ proxyPOST("/api/motor/start"); });
  server.on("/api/motor/stop", HTTP_POST, [](){ proxyPOST("/api/motor/stop"); });
  server.on("/api/motor/auto", HTTP_POST, [](){ proxyPOST("/api/motor/auto"); });

  server.on("/api/unblock", HTTP_POST, [](){ proxyPOST("/api/unblock"); });

  server.on("/api/config", HTTP_POST, [](){ proxyPOST("/api/config"); });
  server.on("/api/ctrl", HTTP_POST, [](){ proxyPOST("/api/ctrl"); });
  server.on("/api/prot", HTTP_POST, [](){ proxyPOST("/api/prot"); });
  server.on("/api/flow", HTTP_POST, [](){ proxyPOST("/api/flow"); });

  // cred: guarda en B y sincroniza a A
  server.on("/api/cred", HTTP_POST, [](){
    if (!ensureAuth()) return;

    if (server.hasArg("user")) HTTP_USER = server.arg("user");
    if (server.hasArg("pass")) HTTP_PASS = server.arg("pass");
    proxyPOST("/api/cred");
  server.on("/api/tg", HTTP_POST, [](){ proxyPOST("/api/tg"); });
  server.on("/api/tg/test", HTTP_POST, [](){ proxyPOST("/api/tg/test"); });
  server.on("/api/relay/set", HTTP_POST, [](){ proxyPOST("/api/relay/set"); });
  server.on("/api/relay/pulse", HTTP_POST, [](){ proxyPOST("/api/relay/pulse"); });
  server.on("/api/relmap", HTTP_POST, [](){ proxyPOST("/api/relmap"); });
  server.on("/api/relpolarity", HTTP_POST, [](){ proxyPOST("/api/relpolarity"); });
}
void setupOta() {
  server.on("/update", HTTP_POST, []() {
    if (!ensureAuth()) return;
    server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK");
    delay(150);
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      if (!ensureAuth()) return;
      Update.begin(UPDATE_SIZE_UNKNOWN);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      Update.write(upload.buf, upload.currentSize);
    } else if (upload.status == UPLOAD_FILE_END) {
      Update.end(true);
    }

  server.on("/ota", HTTP_GET, []() {
    if (!ensureAuth()) return;

    h += "<meta name='viewport' content='width=device-width, initial-scale=1'><style>body{font-family:sans-serif;padding:20px;background:#f0f2f5}.card{background:white;padding:20px;margin-bottom:20px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,.1)}input{display:block;margin:10px 0}</style></head><body>";
    h += "<div class='card'><h2>1) Actualizar B</h2>";
    h += "<div class='card'><h2>2) Actualizar A</h2>";

    server.send(200, "text/html", h);

  char aBuf[96];
  snprintf(aBuf, sizeof(aBuf), "%s", A_BASE.c_str());
  WiFiManagerParameter pA("a_base", "A_BASE (ej: http://hardware_a.local)", aBuf, sizeof(aBuf) - 1);
  setupFiles();
  setupApiInfo();
  setupApiProxy();
  setupOta();

  server.onNotFound([]() {
    if (!ensureAuth()) return;
    server.send(404, "text/plain", "Not found");
  });
  server.begin();
  Serial.println("WEB GATEWAY START (C6)");
  server.handleClient();
    proxyPOSTForm("/api/cred", r, code, body, ct);

    sendProxyResponse(r, (code>0?code:200), ct.length()?ct:"text/plain", body.length()?body:"OK");
  });

  proxyGet("/api/tg");
  proxyPost("/api/tg");
  proxyPost("/api/tg/test");

  // Relay set
  server.on("/api/relay/set", HTTP_POST, [](AsyncWebServerRequest* r){
    if(!isAuth(r)) return;
    int code; String body; String ct;
    proxyPOSTForm("/api/relay/set", r, code, body, ct);
    sendProxyResponse(r, code, ct, body);
  });
  // Compatibilidad adicional
  proxyPost("/api/relay/pulse");

  // Debug
  proxyGet("/api/debug/di");
  proxyGet("/api/vfd/read");

  // ---- OTA EN B (auto-actualizacin) ----
  server.on("/update", HTTP_POST, [](AsyncWebServerRequest *r){
    if(!isAuth(r)) return;
    r->send(200, "text/plain", Update.hasError() ? "FAIL" : "OK");
    ESP.restart();
  }, [](AsyncWebServerRequest *r, String fn, size_t idx, uint8_t *data, size_t len, bool final){
    if(!idx) Update.begin(UPDATE_SIZE_UNKNOWN);
    Update.write(data, len);
    if(final) Update.end(true);
  });

  // ---- OTA PANEL (Para elegir actualizar A o B) ----
  server.on("/ota", HTTP_GET, [](AsyncWebServerRequest *r){
    if(!isAuth(r)) return;
    String h = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>OTA Center</title>";
    h += "<meta name='viewport' content='width=device-width, initial-scale=1'><style>body{font-family:sans-serif;padding:20px;background:#f0f2f5}.card{background:white;padding:20px;margin-bottom:20px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1)}input{display:block;margin:10px 0}</style></head><body>";
    h += "<h1>Centro OTA</h1>";

    // Tarjeta 1: Actualizar ESTA placa (Web Gateway B)
    h += "<div class='card'><h2>1. Actualizar Web Gateway (B)</h2>";
    h += "<p>Sube <b>Web_Gateway_B.ino.bin</b> o el FS spiffs.bin</p>";
    h += "<form method='POST' action='/update' enctype='multipart/form-data'><input type='file' name='update'><input type='submit' value='Actualizar B'></form></div>";

    // Tarjeta 2: Actualizar HARDWARE (Parte A)
    h += "<div class='card'><h2>2. Actualizar Hardware (A)</h2>";
  SerialCom.setTimeout(COM_TIMEOUT_MS);
  // El servidor web es asincrono

    h += "<br><a href='/'>&larr; Volver al Panel</a></body></html>";
    r->send(200, "text/html", h);
  });

  // 404
  server.onNotFound([](AsyncWebServerRequest* r){
    if (r->url() == "/" || r->url() == "/index.html") {
      if(!isAuth(r)) return;
    }
    r->send(404, "text/plain", "Not found");
  });
}

// ----------------- Setup/Loop -----------------
void setup() {
  Serial.begin(115200);
  delay(50);

  loadB();

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS mount FAIL");
  }

  // WiFiManager + parmetro para A_BASE
  WiFiManager wm;
  String host = "hardware-b-" + chipId4();
  WiFi.setHostname(host.c_str());

  // Permite editar A_BASE en el portal
  char aBuf[96]; snprintf(aBuf, sizeof(aBuf), "%s", A_BASE.c_str());
  WiFiManagerParameter pA("a_base", "A_BASE (ej: http://hardware_a.local)", aBuf, sizeof(aBuf)-1);
  wm.addParameter(&pA);

  if (!wm.autoConnect(host.c_str())) ESP.restart();

  // Si cambi el parmetro, guardarlo
  String newA = String(pA.getValue());
  newA.trim();
  if (newA.length() > 0 && newA != A_BASE) {
    if (!newA.startsWith("http")) newA = "http://" + newA;
    A_BASE = newA;
    saveB();
  }

  if (MDNS.begin("web_gateway")) {
    Serial.printf("mDNS: http://web_gateway.local/\n");
  }

  serveFiles();
  setupAPI();
  server.begin();

  Serial.println("WEB GATEWAY START");
  Serial.print("IP: "); Serial.println(WiFi.localIP());
  Serial.print("Target Hardware A: "); Serial.println(A_BASE);
}

void loop() {
  // El servidor web es asncrono
}
