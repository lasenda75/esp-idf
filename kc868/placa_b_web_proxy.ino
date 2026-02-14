/**************** PLACA B  WEB + UART HACIA PLACA A  *****************
   - Sirve la web desde SPIFFS (frontend)
   - Mantiene autenticacin (BasicAuth)
   - Expone /api/* para el frontend y reenva por UART a A
   - OTA local para B + panel que enva a A por HTTP (/update)
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

#include <ESPmDNS.h>

// Forward declaration para evitar fallo del auto-prototipado de Arduino
struct SerialReply;

static String chipId4() {
  uint32_t id = (uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFF);
  char s[6]; snprintf(s, sizeof(s), "%04X", (uint16_t)id);
  return String(s);
}

// ----------------- UART hacia A -----------------
#define PIN_COM_RX 18
#define PIN_COM_TX 19
#define COM_BAUD   115200
#define COM_TIMEOUT_MS 160
HardwareSerial SerialCom(1);

// ----------------- Globals -----------------
AsyncWebServer server(80);
Preferences prefs;

// Credenciales (auth web en B)
String HTTP_USER = "admin";
String HTTP_PASS = "1234";

// Direccin de A (para OTA en el navegador)
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

// ----------------- Helper: URL Encode -----------------
static String buildForm(AsyncWebServerRequest* r) {
    if (!p->isFile() && p->isPost()) {
  }
  return form;
}

struct SerialReply {
  int code = 500;
  String body;
};

SerialReply serialRequest(const String& method, const String& path, const String& params) {
  SerialReply reply;
  String line = method + " " + path;
  if (params.length()) line += " " + params;
  SerialCom.println(line);

  String resp = SerialCom.readStringUntil('\n');
  resp.trim();
  if (!resp.length()) {
    reply.code = 504;
    reply.body = "timeout";
    return reply;
  }

  if (resp == "OK") {
    reply.code = 200;
    reply.body = "";
    return reply;
  }
  if (resp.startsWith("OK ")) {
    reply.code = 200;
    reply.body = resp.substring(3);
static String mergeParams(const String& a, const String& b) {
  if (!a.length()) return b;
  if (!b.length()) return a;
  return a + "&" + b;
}


  while (SerialCom.available()) {
    (void)SerialCom.read();
  }

    return reply;
  }
  if (resp.startsWith("ERR")) {
    int first = resp.indexOf(' ');
    int second = resp.indexOf(' ', first + 1);
    if (second > 0) {
      reply.code = resp.substring(first + 1, second).toInt();
      reply.body = resp.substring(second + 1);
    } else {
      reply.code = 500;
      reply.body = resp;
    return reply;
  reply.code = 500;
  reply.body = resp;
  return reply;
static void sendProxyResponse(AsyncWebServerRequest* r, int code, const String& body) {
  String trimmed = body;
  trimmed.trim();
  String ctype = (trimmed.startsWith("{") || trimmed.startsWith("[")) ? "application/json" : "text/plain";

// ----------------- API (UART) -----------------
    d["uart_rx"] = PIN_COM_RX;
    d["uart_tx"] = PIN_COM_TX;
    d["uart_baud"] = COM_BAUD;
  server.on("/api/b/ota", HTTP_GET, [](AsyncWebServerRequest* r){
    if(!isAuth(r)) return;
    DynamicJsonDocument d(256);
    d["ota_b_url"] = "/update";
    d["ota_a_url"] = A_BASE + "/update";
    d["ota_panel"] = "/ota";
    String s; serializeJson(d, s);
    r->send(200, "application/json", s);
  });

  server.on("/api/b/a_base", HTTP_POST, [](AsyncWebServerRequest* r){
    if(!isAuth(r)) return;
    if (!r->hasParam("a_base", true)) {
      r->send(400, "text/plain", "falta a_base");
      return;
    }
    String v = r->getParam("a_base", true)->value();
    v.trim();
    if (!v.length()) {
      r->send(400, "text/plain", "a_base vacio");
      return;
    }
    if (!v.startsWith("http")) v = "http://" + v;
    A_BASE = v;
    saveB();
    r->send(200, "application/json", "{\"ok\":true}");
  });

  auto serialGet = [&](const char* path){
      SerialReply rep = serialRequest("GET", path, query);
      sendProxyResponse(r, rep.code, rep.body);
  auto serialPost = [&](const char* path){
      String query = buildQuery(r);
      String form = buildForm(r);
      SerialReply rep = serialRequest("POST", path, mergeParams(query, form));
      sendProxyResponse(r, rep.code, rep.body);
  serialGet("/api/estados");
  serialGet("/api/state");

  serialPost("/api/valvula/mode");
  serialPost("/api/valvula/open");
  serialPost("/api/valvula/close");
  serialPost("/api/valvula/auto");
  serialPost("/api/motor/mode");
  serialPost("/api/motor/start");
  serialPost("/api/motor/stop");
  serialPost("/api/motor/auto");
  serialPost("/api/unblock");
  serialGet("/api/config");
  serialPost("/api/config");
  serialGet("/api/ctrl");
  serialPost("/api/ctrl");
  serialGet("/api/prot");
  serialPost("/api/prot");
  serialGet("/api/flow");
  serialPost("/api/flow");
  serialGet("/api/cred");
    String form = buildForm(r);
    SerialReply rep = serialRequest("POST", "/api/cred", form);
    sendProxyResponse(r, rep.code, rep.body.length() ? rep.body : "OK");
  serialGet("/api/tg");
  serialPost("/api/tg");
  serialPost("/api/tg/test");
  serialPost("/api/relay/set");
  serialPost("/api/relay/pulse");
  serialGet("/api/debug/di");
  serialGet("/api/debug/di8");
  serialGet("/api/vfd/read");

  serialGet("/api/relmap");
  serialPost("/api/relmap");
  serialPost("/api/relpolarity");
  });
    if(!isAuth(r)) return;

  SerialCom.begin(COM_BAUD, SERIAL_8N1, PIN_COM_RX, PIN_COM_TX);
  SerialCom.setTimeout(120);

  Serial.print("Target OTA A: "); Serial.println(A_BASE);

  // El servidor web es as
ncrono
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
