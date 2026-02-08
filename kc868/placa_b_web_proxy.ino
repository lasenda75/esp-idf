/**************** PLACA B  WEB + PROXY HACIA PLACA A  *****************
   - Sirve la web desde SPIFFS (frontend)
   - Mantiene autenticacin (BasicAuth)
   - Expone /api/* para el frontend y reenva a A (proxy)
   - Compatible con tu web (scripts.js/config.js/credentials.js)
*************************************************************************/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>

#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

#include <SPIFFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <HTTPClient.h>          // Cliente HTTP para hablar con A

#include <ESPmDNS.h>

static String chipId4() {
  uint32_t id = (uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFF);
  char s[6]; snprintf(s, sizeof(s), "%04X", (uint16_t)id);
  return String(s);
}

// ----------------- Globals -----------------
AsyncWebServer server(80);
Preferences prefs;

// Credenciales (auth web en B)
String HTTP_USER = "admin";
String HTTP_PASS = "1234";

// Direccin de A (IP o host)
String A_BASE = "http://hardware_a.local"; // Valor por defecto mDNS

// ----------------- Auth -----------------
bool isAuth(AsyncWebServerRequest* r) {
  if (HTTP_USER.length() == 0) return true; // si quisieras permitir sin auth
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

// ----------------- Helper: URL Encode -----------------
// IMPORTANTE: Necesario para que espacios y smbolos pasen correctamente a A
String urlEncode(String str) {
  String encodedString = "";
  char c;
  char code0;
  char code1;
  for (int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (c == ' ') {
      encodedString += '+';
    } else if (isalnum(c)) {
      encodedString += c;
    } else {
      code1 = (c & 0xf) + '0';
      if ((c & 0xf) > 9) {
        code1 = (c & 0xf) - 10 + 'A';
      }
      c = (c >> 4) & 0xf;
      code0 = c + '0';
      if (c > 9) {
        code0 = c - 10 + 'A';
      }
      encodedString += '%';
      encodedString += code0;
      encodedString += code1;
    }
  }
  return encodedString;
}

// ----------------- Proxy helpers -----------------
static String urlJoin(const String& base, const String& pathAndQuery) {
  if (base.endsWith("/") && pathAndQuery.startsWith("/")) return base + pathAndQuery.substring(1);
  if (!base.endsWith("/") && !pathAndQuery.startsWith("/")) return base + "/" + pathAndQuery;
  return base + pathAndQuery;
}

static String buildQuery(AsyncWebServerRequest* r) {
  String query;
  for (uint8_t i = 0; i < r->params(); i++) {
    AsyncWebParameter* p = r->getParam(i);
    if (!p->isFile() && !p->isPost()) {
      if (query.length()) query += "&";
      query += p->name() + "=" + urlEncode(p->value());
    }
  }
  return query;
}

static bool proxyGET(const String& pathAndQuery, int& code, String& body, String& contentType) {
  HTTPClient http;
  String url = urlJoin(A_BASE, pathAndQuery);
  http.setTimeout(2500);
  if (!http.begin(url)) { code = 500; body = "HTTP begin fail"; contentType="text/plain"; return false; }

  code = http.GET();
  contentType = http.header("Content-Type");
  body = http.getString();
  http.end();
  return (code > 0);
}

// Reenva POST form-url-encoded construido a partir de params recibidos por B
static bool proxyPOSTForm(const String& path, AsyncWebServerRequest* r, int& code, String& body, String& contentType) {
  // Construye "a=b&c=d" con params del body y query
  String form;

  // 1) Query params
  for (uint8_t i = 0; i < r->params(); i++) {
    AsyncWebParameter* p = r->getParam(i);
    if (!p->isFile() && !p->isPost()) { // query
      if (form.length()) form += "&";
      form += p->name() + "=" + urlEncode(p->value());
    }
  }
  // 2) Body (POST) params
  for (uint8_t i = 0; i < r->params(); i++) {
    AsyncWebParameter* p = r->getParam(i);
    if (!p->isFile() && p->isPost()) { // post body
      if (form.length()) form += "&";
      form += p->name() + "=" + urlEncode(p->value());
    }
  }

  HTTPClient http;
  String url = urlJoin(A_BASE, path);
  http.setTimeout(2500);
  if (!http.begin(url)) { code = 500; body = "HTTP begin fail"; contentType="text/plain"; return false; }

  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  code = http.POST((uint8_t*)form.c_str(), form.length());
  contentType = http.header("Content-Type");
  body = http.getString();
  http.end();
  return (code > 0);
}

static void sendProxyResponse(AsyncWebServerRequest* r, int code, const String& ct, const String& body) {
  String ctype = ct.length() ? ct : "text/plain";
  r->send(code > 0 ? code : 500, ctype, body);
}

// ----------------- Files -----------------
void serveFiles() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r){
    if(!isAuth(r)) return;
    r->send(SPIFFS, "/index.html", "text/html");
  });
  server.on("/index.html", HTTP_GET, [](AsyncWebServerRequest* r){
    if(!isAuth(r)) return;
    r->send(SPIFFS, "/index.html", "text/html");
  });

  // Compat con tu web
  server.on("/config", HTTP_GET, [](AsyncWebServerRequest* r){
    if(!isAuth(r)) return;
    r->send(SPIFFS, "/config.html", "text/html");
  });

  // Static
  server.serveStatic("/styles.css", SPIFFS, "/styles.css");
  server.serveStatic("/scripts.js", SPIFFS, "/scripts.js").setCacheControl("no-cache, no-store, must-revalidate");
  server.serveStatic("/config.html", SPIFFS, "/config.html");
  server.serveStatic("/config.js",   SPIFFS, "/config.js").setCacheControl("no-cache, no-store, must-revalidate");
  server.serveStatic("/credentials.html", SPIFFS, "/credentials.html");
  server.serveStatic("/credentials.js",   SPIFFS, "/credentials.js").setCacheControl("no-cache, no-store, must-revalidate");

  // Imgenes del plano
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
    h += "<p>Sube <b>Hardware_Controller_A.ino.bin</b>.</p>";
    h += "<p><small>Destino: " + A_BASE + "/update</small></p>";
    h += "<form method='POST' action='" + A_BASE + "/update' enctype='multipart/form-data'><input type='file' name='update'><input type='submit' value='Actualizar A'></form></div>";

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
