/**************** PLACA B - WEB + PROXY HTTP HACIA PLACA A ***************
   - Sirve la web desde SPIFFS (frontend)
   - Mantiene autenticación (BasicAuth)
   - Expone /api/* para el frontend y reenvía a A por HTTP
   - OTA local para B + panel para OTA de A por navegador
***************************************************************************/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>

#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

#include <SPIFFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <HTTPClient.h>

#include <ESPmDNS.h>

static String chipId4() {
  uint32_t id = (uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFF);
  char s[6]; snprintf(s, sizeof(s), "%04X", (uint16_t)id);
  return String(s);
}

// ----------------- Globals -----------------
AsyncWebServer server(80);
Preferences prefs;

String HTTP_USER = "admin";
String HTTP_PASS = "1234";
String A_BASE = "http://hardware_a.local";

// ----------------- Auth -----------------
bool isAuth(AsyncWebServerRequest* r) {
  if (HTTP_USER.length() == 0) return true;
  if (!r->authenticate(HTTP_USER.c_str(), HTTP_PASS.c_str())) {
    r->requestAuthentication();
    return false;
  }
  return true;
}

// ----------------- Preferences -----------------
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

// ----------------- URL helpers -----------------
String urlEncode(const String& str) {
  String out;
    if (c == ' ') out += '+';
    else if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') out += c;
    else {
      code1 = (c & 0x0f) + '0';
      if ((c & 0x0f) > 9) code1 = (c & 0x0f) - 10 + 'A';
      c = (c >> 4) & 0x0f;
      if (c > 9) code0 = c - 10 + 'A';
      out += '%'; out += code0; out += code1;
  return out;
String joinUrl(const String& base, const String& pathAndQuery) {
  if (base.endsWith("/") && pathAndQuery.startsWith("/")) return base + pathAndQuery.substring(1);
  if (!base.endsWith("/") && !pathAndQuery.startsWith("/")) return base + "/" + pathAndQuery;
  return base + pathAndQuery;
String buildQuery(AsyncWebServerRequest* r, bool includePost) {
  String q;
    if (p->isFile()) continue;
    if (p->isPost() != includePost) continue;
    if (q.length()) q += '&';
    q += p->name();
    q += '=';
    q += urlEncode(p->value());
  return q;
String mergeParams(const String& a, const String& b) {
void sendProxyResponse(AsyncWebServerRequest* r, int code, const String& ct, const String& body) {
  String contentType = ct.length() ? ct : "text/plain";
  r->send((code > 0) ? code : 500, contentType, body);
}

bool proxyGET(const String& path, AsyncWebServerRequest* req, int& code, String& ct, String& body) {
  HTTPClient http;
  String q = buildQuery(req, false);
  String target = path;
  if (q.length()) target += "?" + q;
  String url = joinUrl(A_BASE, target);
  http.setTimeout(3000);
  if (!http.begin(url)) {
    code = 500; ct = "text/plain"; body = "HTTP begin fail";
    return false;
  code = http.GET();
  ct = http.header("Content-Type");
  body = http.getString();
  http.end();
  return code > 0;
}
bool proxyPOSTForm(const String& path, AsyncWebServerRequest* req, int& code, String& ct, String& body) {
  HTTPClient http;
  String form = mergeParams(buildQuery(req, false), buildQuery(req, true));
  String url = joinUrl(A_BASE, path);
  http.setTimeout(3000);
  if (!http.begin(url)) {
    code = 500; ct = "text/plain"; body = "HTTP begin fail";
    return false;

  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  code = http.POST((uint8_t*)form.c_str(), form.length());
  ct = http.header("Content-Type");
  body = http.getString();
  http.end();
  return code > 0;

  server.serveStatic("/config.js", SPIFFS, "/config.js").setCacheControl("no-cache, no-store, must-revalidate");
  server.serveStatic("/credentials.js", SPIFFS, "/credentials.js").setCacheControl("no-cache, no-store, must-revalidate");
  server.serveStatic("/overlay_salida.png", SPIFFS, "/overlay_salida.png");
  server.serveStatic("/overlay_bypass.png", SPIFFS, "/overlay_bypass.png");
// ----------------- API -----------------
    d["ota_b_url"] = String("http://") + WiFi.localIP().toString() + "/update";
    if (!v.startsWith("http://") && !v.startsWith("https://")) v = "http://" + v;
    r->send(200, "text/plain", "OK");
  auto proxyGet = [&](const char* path){
      int code; String ct; String body;
      proxyGET(path, r, code, ct, body);
      sendProxyResponse(r, code, ct, body);
  auto proxyPost = [&](const char* path){
      int code; String ct; String body;
      proxyPOSTForm(path, r, code, ct, body);
      sendProxyResponse(r, code, ct, body);
  proxyGet("/api/estados");
  proxyGet("/api/state");
  proxyPost("/api/valvula/mode");
  proxyPost("/api/valvula/open");
  proxyPost("/api/valvula/close");
  proxyPost("/api/valvula/auto");
  proxyPost("/api/motor/mode");
  proxyPost("/api/motor/start");
  proxyPost("/api/motor/stop");
  proxyPost("/api/motor/auto");
  proxyPost("/api/unblock");
  proxyGet("/api/config");
  proxyPost("/api/config");
  proxyGet("/api/ctrl");
  proxyPost("/api/ctrl");
  proxyGet("/api/prot");
  proxyPost("/api/prot");
  proxyGet("/api/flow");
  proxyPost("/api/flow");
  proxyGet("/api/cred");
    int code; String ct; String body;
    proxyPOSTForm("/api/cred", r, code, ct, body);
    sendProxyResponse(r, (code > 0 ? code : 200), ct.length() ? ct : "text/plain", body.length() ? body : "OK");
  proxyGet("/api/tg");
  proxyPost("/api/tg");
  proxyPost("/api/tg/test");
  proxyPost("/api/relay/set");
  proxyPost("/api/relay/pulse");
  proxyGet("/api/debug/di");
  proxyGet("/api/debug/di8");
  proxyGet("/api/vfd/read");
  proxyGet("/api/relmap");
  proxyPost("/api/relmap");
  proxyPost("/api/relpolarity");
  // OTA en B
  // Panel OTA
    h += "<p>Sube <b>firmware B</b> o <b>SPIFFS</b>.</p>";

    h += "<p>Sube <b>firmware A</b>.</p>";
    h += "<br><a href='/'>← Volver</a></body></html>";
  String host = "web-gw-" + chipId4();
    if (!newA.startsWith("http://") && !newA.startsWith("https://")) newA = "http://" + newA;
  Serial.print("Target A: "); Serial.println(A_BASE);

  // asíncrono
genes del plano
  server.serveStatic("/plano.png", SPIFFS, "/plano.png");
  server.serveStatic("/overlay_entrada.png", SPIFFS, "/overlay_entrada.png");
  server.serveStatic("/overlay_salida.png",  SPIFFS, "/overlay_salida.png");
  server.serveStatic("/overlay_bypass.png",  SPIFFS, "/overlay_bypass.png");
  server.serveStatic("/favicon.ico", SPIFFS, "/favicon.ico");
}

// ----------------- API (proxy + algunos endpoints locales) -----------------
void setupAPI() {
  // Endpoint til para ver a qu A apunta B
  server.on("/api/b/info", HTTP_GET, [](AsyncWebServerRequest* r){
    if(!isAuth(r)) return;
    DynamicJsonDocument d(256);
    d["a_base"] = A_BASE;
    d["host"] = WiFi.getHostname() ? WiFi.getHostname() : "";
    d["ip"] = WiFi.localIP().toString();
    String s; serializeJson(d, s);
    r->send(200, "application/json", s);
  });

  auto proxyGet = [&](const char* path){
    server.on(path, HTTP_GET, [=](AsyncWebServerRequest* r){
      if(!isAuth(r)) return;
      String query = buildQuery(r);
      String urlDest = String(path);
      if (query.length()) urlDest += "?" + query;

      int code; String body; String ct;
      proxyGET(urlDest, code, body, ct);
      sendProxyResponse(r, code, ct, body);
    });
  };

  auto proxyPost = [&](const char* path){
    server.on(path, HTTP_POST, [=](AsyncWebServerRequest* r){
      if(!isAuth(r)) return;
      int code; String body; String ct;
      proxyPOSTForm(path, r, code, body, ct);
      sendProxyResponse(r, code, ct, body);
    });
  };

  // Estados (scripts.js usa /api/estados y fallback /api/state)
  proxyGet("/api/estados");
  proxyGet("/api/state");

  // Control (index.html/config.js)
  proxyPost("/api/valvula/mode");
  proxyPost("/api/valvula/open");
  proxyPost("/api/valvula/close");

  proxyPost("/api/motor/mode");
  proxyPost("/api/motor/start");
  proxyPost("/api/motor/stop");

  proxyPost("/api/unblock");

  // Configuracin completa
  proxyGet("/api/config");
  proxyPost("/api/config");

  proxyGet("/api/ctrl");
  proxyPost("/api/ctrl");

  proxyGet("/api/prot");
  proxyPost("/api/prot");

  proxyGet("/api/flow");
  proxyPost("/api/flow");

  proxyGet("/api/cred");
  server.on("/api/cred", HTTP_POST, [](AsyncWebServerRequest* r){
    if(!isAuth(r)) return;

    // 1) Guardar credenciales en B (para login web)
    if (r->hasParam("user", true)) HTTP_USER = r->getParam("user", true)->value();
    if (r->hasParam("pass", true)) HTTP_PASS = r->getParam("pass", true)->value();
    saveB();

    // 2) Reenviar tambin a A (para mantener sincrona si quieres)
    int code; String body; String ct;
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
