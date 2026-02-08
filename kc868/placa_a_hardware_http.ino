/**************** PLACA A: CONTROL (KC868-A6) - HTTP SLAVE *****************
 * HW: Rel3, RS485 (VFD), sensores, caudal y Telegram.
 * COM: API HTTP (sin web) + OTA por /update.
 * mDNS: hardware_a.local
 ***************************************************************************/
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <ModbusMaster.h>
#include <Wire.h>
#include <PCF8574.h>
#include <UniversalTelegramBot.h>
#include <ESPmDNS.h>
#include "esp_task_wdt.h"

// --- RS485 (KC868-A6) ---
#define RS485_UART      Serial2
#define RS485_RX_PIN    14
#define RS485_TX_PIN    27
#define PIN_RS485_DIR   -1 // KC868-A6 suele usar control automtico o pin 4
#define RS485_BAUD      9600

// --- HARDWARE I2C ---
PCF8574 PCF_DO(0x24, 4, 15);
const uint8_t DO_PIN[6] = { 0, 1, 2, 3, 4, 5 }; // P0..P5
PCF8574 PCF_DI(0x22, 4, 15);
const uint8_t DI_PIN[6] = { 5, 4, 3, 2, 1, 0 }; // Mapeo inverso habitual
uint8_t g_DI_BIT[6]     = { 5, 4, 3, 2, 1, 0 };

#define DI_PRESOSTATO 0
#define DI_NIVEL_MAX  1
#define DI_NIVEL_MIN  2
#define DI_NIVEL_OPT  3
#define DI_HUMEDAD    4
#define DI_RESERVA    5

// --- CAUDALMETROS ---
#define FLOW_IN_PIN   32
#define FLOW_OUT_PIN  33
volatile uint32_t flow_in_pulses = 0;
volatile uint32_t flow_out_pulses = 0;
void IRAM_ATTR isr_flow_in()  { flow_in_pulses++; }
void IRAM_ATTR isr_flow_out() { flow_out_pulses++; }

// --- OBJETOS ---
Preferences prefs;
ModbusMaster mb;
NetworkClientSecure tgClient;
UniversalTelegramBot* tgBot = nullptr;
SemaphoreHandle_t g_i2cMutex = nullptr;
AsyncWebServer server(80);

// --- CONFIGURACIN Y ESTADO ---
String TG_TOKEN, TG_CHAT;
String HTTP_USER = "admin";
String HTTP_PASS = "1234";

struct Config {
  uint8_t ev_mode = 0; uint8_t mot_mode = 0;
  uint8_t vfd_id = 1;  uint32_t vfd_baud = 9600;
  float vfd_set_hz_def = 25.0; float vfd_fmax_hz = 50.0;
  float flow_k_in = 450.0; float flow_k_out = 450.0;
  uint8_t ev_ch = 0; uint8_t achq_ch = 1;
  bool relays_active_high = false; bool bloqueo_activo = false;
} CFG;

struct Estado {
  bool bloqueado=false; bool bypass=false;
  float presion_bar=0.0; float vfd_set_hz=0.0; float vfd_out_hz=0.0;
  float lps_in=0.0; float lps_out=0.0;
  bool flujo_in=false; bool flujo_out=false;
  String presostato="-"; bool presion_ok=false;
  bool nivel_max=false; bool nivel_min=false; bool nivel_opt=false;
  bool humedad_wet=false; uint16_t humedad_adc=0;
  String ev_txt="Init"; bool ev_abierta=false; bool motor_run=false;
} ST;

struct VFDMap {
  uint16_t reg_cmd=0x2000; uint16_t reg_setf=0x2001;
  uint16_t reg_r_set=0x2102; uint16_t reg_r_out=0x2103;
  uint16_t reg_press=0x2107; uint16_t reg_flags=0x2101;
  float scale_hz=0.01f; float scale_bar=0.01f;
} VFD;

struct Ctrl { float p_start; float p_stop; float hz_min; uint16_t dwell_ms; } CTRL;
struct Protec { uint16_t in_s=10; uint16_t out_s=2; } PROT;

// --- FUNCIONES AUXILIARES ---
inline bool i2cLock() { return g_i2cMutex ? xSemaphoreTake(g_i2cMutex, pdMS_TO_TICKS(100)) : true; }
inline void i2cUnlock() { if (g_i2cMutex) xSemaphoreGive(g_i2cMutex); }

void loadCfg() {
  prefs.begin("cfg", true);
  CFG.ev_mode = prefs.getUChar("evm", 0); CFG.mot_mode = prefs.getUChar("mm", 0);
  CFG.vfd_id = prefs.getUChar("vid", 1); CFG.vfd_baud = prefs.getUInt("vbd", 9600);
  CFG.vfd_set_hz_def = prefs.getFloat("vset", 25.0); CFG.vfd_fmax_hz = prefs.getFloat("vmax", 50.0);
  CFG.flow_k_in = prefs.getFloat("fk_in", 450.0); CFG.flow_k_out = prefs.getFloat("fk_out", 450.0);
  CFG.ev_ch = prefs.getUChar("evch", 0); CFG.achq_ch = prefs.getUChar("achq", 1);
  CFG.relays_active_high = prefs.getBool("rah", false);
  CFG.bloqueo_activo = prefs.getBool("blk", false);
  TG_TOKEN = prefs.getString("tg_t", ""); TG_CHAT = prefs.getString("tg_c", "");
  HTTP_USER = prefs.getString("user", HTTP_USER);
  HTTP_PASS = prefs.getString("pass", HTTP_PASS);
  prefs.end();

  prefs.begin("ctrl", true);
  CTRL.p_start = prefs.getFloat("p_start", 2.0);
  CTRL.p_stop = prefs.getFloat("p_stop", 2.5);
  CTRL.hz_min = prefs.getFloat("hz_min", 25.0);
  CTRL.dwell_ms = prefs.getUShort("dwell", 1500);
  prefs.end();

  prefs.begin("prot", true);
  PROT.in_s = prefs.getUShort("in_s", 10);
  PROT.out_s = prefs.getUShort("out_s", 2);
  prefs.end();
}

void saveCfg() {
  prefs.begin("cfg", false);
  prefs.putUChar("evm", CFG.ev_mode); prefs.putUChar("mm", CFG.mot_mode);
  prefs.putUChar("vid", CFG.vfd_id); prefs.putUInt("vbd", CFG.vfd_baud);
  prefs.putFloat("vset", CFG.vfd_set_hz_def); prefs.putFloat("vmax", CFG.vfd_fmax_hz);
  prefs.putFloat("fk_in", CFG.flow_k_in); prefs.putFloat("fk_out", CFG.flow_k_out);
  prefs.putUChar("evch", CFG.ev_ch); prefs.putUChar("achq", CFG.achq_ch);
  prefs.putBool("rah", CFG.relays_active_high); prefs.putBool("blk", CFG.bloqueo_activo);
  prefs.putString("tg_t", TG_TOKEN); prefs.putString("tg_c", TG_CHAT);
  prefs.putString("user", HTTP_USER); prefs.putString("pass", HTTP_PASS);
  prefs.end();
}

void saveCtrl() {
  prefs.begin("ctrl", false);
  prefs.putFloat("p_start", CTRL.p_start);
  prefs.putFloat("p_stop", CTRL.p_stop);
  prefs.putFloat("hz_min", CTRL.hz_min);
  prefs.putUShort("dwell", CTRL.dwell_ms);
  prefs.end();
}

void saveProt() {
  prefs.begin("prot", false);
  prefs.putUShort("in_s", PROT.in_s);
  prefs.putUShort("out_s", PROT.out_s);
  prefs.end();
}

// --- TELEGRAM ---
bool tgEnsure() {
  tgClient.setInsecure();
  if (!tgBot && TG_TOKEN.length() > 5) tgBot = new UniversalTelegramBot(TG_TOKEN, tgClient);
  return tgBot != nullptr;
}
void tgNotify(const String& msg) {
  if (WiFi.status() == WL_CONNECTED && tgEnsure() && TG_CHAT.length()) {
    esp_task_wdt_reset();
    tgBot->sendMessage(TG_CHAT, msg, "Markdown");
    esp_task_wdt_reset();
  }
}

// --- HARDWARE CONTROL ---
void relayWrite(uint8_t ch, bool on) {
  if (ch >= 6 || !i2cLock()) return;
  bool level = CFG.relays_active_high ? on : !on; // activo-bajo
  PCF_DO.digitalWrite(DO_PIN[ch], level);
  i2cUnlock();
}

bool diActive(uint8_t ch) {
  if (ch >= 6 || !i2cLock()) return false;
  Wire.beginTransmission(0x22);
  Wire.write(0xFF);
  Wire.endTransmission();
  Wire.requestFrom((uint8_t)0x22, (uint8_t)1);
  uint8_t port = Wire.available() ? Wire.read() : 0xFF;
  i2cUnlock();
  bool low = ((port & (1 << g_DI_BIT[ch])) == 0);
  return low; // activo bajo
}

// --- MODBUS ---
bool vfdWrite(uint16_t reg, uint16_t val) {
  return mb.writeSingleRegister(reg, val) == mb.ku8MBSuccess;
}
void vfdPoll() {
  uint16_t v;
  if (mb.readHoldingRegisters(VFD.reg_press, 1) == mb.ku8MBSuccess) {
    ST.presion_bar = mb.getResponseBuffer(0) * VFD.scale_bar;
  }
  if (mb.readHoldingRegisters(VFD.reg_r_out, 1) == mb.ku8MBSuccess) {
    ST.vfd_out_hz = mb.getResponseBuffer(0) * VFD.scale_hz;
  }
  if (mb.readHoldingRegisters(VFD.reg_flags, 1) == mb.ku8MBSuccess) {
    ST.motor_run = (mb.getResponseBuffer(0) & 0x0001);
  }
}

void aplicarEV() {
  bool abrir = (CFG.ev_mode == 1) ? true
               : (CFG.ev_mode == 2) ? false
               : (!ST.bloqueado && !ST.nivel_max);
  if (ST.ev_abierta != abrir) {
    relayWrite(CFG.ev_ch, abrir);
    ST.ev_abierta = abrir;
    ST.ev_txt = abrir ? "Abierta" : "Cerrada";
  }
}

void aplicarMotor() {
  if (CFG.mot_mode == 1) {
    vfdWrite(VFD.reg_cmd, 0x0012);
    ST.motor_run = true;
    return;
  }
  if (CFG.mot_mode == 2) {
    vfdWrite(VFD.reg_cmd, 0x0001);
    ST.motor_run = false;
    return;
  }

  if (ST.bloqueado || ST.nivel_min) {
    vfdWrite(VFD.reg_cmd, 0x0001);
    ST.motor_run = false;
    return;
  }

  if (ST.presion_bar < CTRL.p_start) {
    vfdWrite(VFD.reg_cmd, 0x0012);
    ST.motor_run = true;
  } else if (ST.presion_bar > CTRL.p_stop) {
    vfdWrite(VFD.reg_cmd, 0x0001);
    ST.motor_run = false;
  }
}

String jsonEstados() {
  DynamicJsonDocument d(768);
  d["bloqueado"] = ST.bloqueado;
  d["bypass"] = ST.bypass;
  d["presion_bar"] = ST.presion_bar;
  d["vfd_set_hz"] = ST.vfd_set_hz;
  d["vfd_out_hz"] = ST.vfd_out_hz;
  d["lps_in"] = ST.lps_in;
  d["lps_out"] = ST.lps_out;
  d["flujo_in"] = ST.flujo_in;
  d["flujo_out"] = ST.flujo_out;
  d["presostato"] = ST.presostato;
  d["nivel_max"] = ST.nivel_max;
  d["nivel_min"] = ST.nivel_min;
  d["nivel_opt"] = ST.nivel_opt;
  d["ev_txt"] = ST.ev_txt;
  d["ev_abierta"] = ST.ev_abierta;
  d["motor_run"] = ST.motor_run;
  d["ev_mode"] = CFG.ev_mode;
  d["mot_mode"] = CFG.mot_mode;
  d["humedad_wet"] = ST.humedad_wet;
  d["humedad_adc"] = ST.humedad_adc;
  d["ev_ch"] = CFG.ev_ch;
  d["achq_ch"] = CFG.achq_ch;
  d["rah"] = CFG.relays_active_high;
  d["prot_in_s"] = PROT.in_s;
  d["prot_out_s"] = PROT.out_s;
  String s; serializeJson(d, s); return s;
}

void setupApi() {
  server.on("/api/estados", HTTP_GET, [](AsyncWebServerRequest* r){
    r->send(200, "application/json", jsonEstados());
  });
  server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest* r){
    r->send(200, "application/json", jsonEstados());
  });

  server.on("/api/valvula/mode", HTTP_POST, [](AsyncWebServerRequest* r){
    if (!r->hasParam("m", true)) { r->send(400, "text/plain", "falta m"); return; }
    String m = r->getParam("m", true)->value(); m.toLowerCase();
    if (m == "auto") CFG.ev_mode = 0;
    else if (m == "open") CFG.ev_mode = 1;
    else if (m == "close") CFG.ev_mode = 2;
    saveCfg(); aplicarEV();
    r->send(200, "text/plain", "OK");
  });
  server.on("/api/valvula/open", HTTP_POST, [](AsyncWebServerRequest* r){ CFG.ev_mode = 1; saveCfg(); aplicarEV(); r->send(200, "text/plain", "OK"); });
  server.on("/api/valvula/close", HTTP_POST, [](AsyncWebServerRequest* r){ CFG.ev_mode = 2; saveCfg(); aplicarEV(); r->send(200, "text/plain", "OK"); });
  server.on("/api/valvula/auto", HTTP_POST, [](AsyncWebServerRequest* r){ CFG.ev_mode = 0; saveCfg(); aplicarEV(); r->send(200, "text/plain", "OK"); });

  server.on("/api/motor/mode", HTTP_POST, [](AsyncWebServerRequest* r){
    if (!r->hasParam("m", true)) { r->send(400, "text/plain", "falta m"); return; }
    String m = r->getParam("m", true)->value(); m.toLowerCase();
    if (m == "auto") CFG.mot_mode = 0;
    else if (m == "on") CFG.mot_mode = 1;
    else if (m == "off") CFG.mot_mode = 2;
    saveCfg(); aplicarMotor();
    r->send(200, "text/plain", "OK");
  });
  server.on("/api/motor/start", HTTP_POST, [](AsyncWebServerRequest* r){ CFG.mot_mode = 1; saveCfg(); aplicarMotor(); r->send(200, "text/plain", "OK"); });
  server.on("/api/motor/stop", HTTP_POST, [](AsyncWebServerRequest* r){ CFG.mot_mode = 2; saveCfg(); aplicarMotor(); r->send(200, "text/plain", "OK"); });
  server.on("/api/motor/auto", HTTP_POST, [](AsyncWebServerRequest* r){ CFG.mot_mode = 0; saveCfg(); aplicarMotor(); r->send(200, "text/plain", "OK"); });

  server.on("/api/unblock", HTTP_POST, [](AsyncWebServerRequest* r){
    CFG.bloqueo_activo = false; saveCfg();
    r->send(200, "text/plain", "OK");
  });

  server.on("/api/cred", HTTP_GET, [](AsyncWebServerRequest* r){
    DynamicJsonDocument d(128);
    d["user"] = HTTP_USER; d["pass"] = HTTP_PASS;
    String s; serializeJson(d, s); r->send(200, "application/json", s);
  });
  server.on("/api/cred", HTTP_POST, [](AsyncWebServerRequest* r){
    if (r->hasParam("user", true)) HTTP_USER = r->getParam("user", true)->value();
    if (r->hasParam("pass", true)) HTTP_PASS = r->getParam("pass", true)->value();
    saveCfg(); r->send(200, "text/plain", "OK");
  });

  server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest* r){
    DynamicJsonDocument d(512);
    d["vfd_id"] = CFG.vfd_id;
    d["vfd_baud"] = CFG.vfd_baud;
    d["reg_run"] = VFD.reg_cmd;
    d["reg_setf"] = VFD.reg_setf;
    d["reg_outf"] = VFD.reg_r_out;
    d["reg_pres"] = VFD.reg_press;
    d["scale_f"] = VFD.scale_hz;
    d["scale_p"] = VFD.scale_bar;
    d["k_in"] = CFG.flow_k_in;
    d["k_out"] = CFG.flow_k_out;
    d["vfd_fmax_hz"] = CFG.vfd_fmax_hz;
    String s; serializeJson(d, s); r->send(200, "application/json", s);
  });
  server.on("/api/config", HTTP_POST, [](AsyncWebServerRequest* r){
    if (r->hasParam("vfd_id", true)) CFG.vfd_id = (uint8_t)r->getParam("vfd_id", true)->value().toInt();
    if (r->hasParam("vfd_baud", true)) CFG.vfd_baud = (uint32_t)r->getParam("vfd_baud", true)->value().toInt();
    if (r->hasParam("reg_run", true)) VFD.reg_cmd = (uint16_t)r->getParam("reg_run", true)->value().toInt();
    if (r->hasParam("reg_setf", true)) VFD.reg_setf = (uint16_t)r->getParam("reg_setf", true)->value().toInt();
    if (r->hasParam("reg_outf", true)) VFD.reg_r_out = (uint16_t)r->getParam("reg_outf", true)->value().toInt();
    if (r->hasParam("reg_pres", true)) VFD.reg_press = (uint16_t)r->getParam("reg_pres", true)->value().toInt();
    if (r->hasParam("scale_f", true)) VFD.scale_hz = r->getParam("scale_f", true)->value().toFloat();
    if (r->hasParam("scale_p", true)) VFD.scale_bar = r->getParam("scale_p", true)->value().toFloat();
    if (r->hasParam("k_in", true)) CFG.flow_k_in = r->getParam("k_in", true)->value().toFloat();
    if (r->hasParam("k_out", true)) CFG.flow_k_out = r->getParam("k_out", true)->value().toFloat();
    if (r->hasParam("vfd_fmax_hz", true)) CFG.vfd_fmax_hz = r->getParam("vfd_fmax_hz", true)->value().toFloat();
    saveCfg();
    r->send(200, "text/plain", "OK");
  });

  server.on("/api/ctrl", HTTP_GET, [](AsyncWebServerRequest* r){
    DynamicJsonDocument d(200);
    d["p_start"] = CTRL.p_start; d["p_stop"] = CTRL.p_stop;
    d["hz_min"] = CTRL.hz_min; d["dwell_ms"] = CTRL.dwell_ms;
    String s; serializeJson(d, s); r->send(200, "application/json", s);
  });
  server.on("/api/ctrl", HTTP_POST, [](AsyncWebServerRequest* r){
    if (r->hasParam("p_start", true)) CTRL.p_start = r->getParam("p_start", true)->value().toFloat();
    if (r->hasParam("p_stop", true)) CTRL.p_stop = r->getParam("p_stop", true)->value().toFloat();
    if (r->hasParam("hz_min", true)) CTRL.hz_min = r->getParam("hz_min", true)->value().toFloat();
    if (r->hasParam("dwell_ms", true)) CTRL.dwell_ms = r->getParam("dwell_ms", true)->value().toInt();
    saveCtrl(); r->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/prot", HTTP_GET, [](AsyncWebServerRequest* r){
    DynamicJsonDocument d(128);
    d["in_s"] = PROT.in_s; d["out_s"] = PROT.out_s;
    String s; serializeJson(d, s); r->send(200, "application/json", s);
  });
  server.on("/api/prot", HTTP_POST, [](AsyncWebServerRequest* r){
    if (r->hasParam("in_s", true)) PROT.in_s = (uint16_t)r->getParam("in_s", true)->value().toInt();
    if (r->hasParam("out_s", true)) PROT.out_s = (uint16_t)r->getParam("out_s", true)->value().toInt();
    saveProt(); r->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/flow", HTTP_GET, [](AsyncWebServerRequest* r){
    DynamicJsonDocument d(128);
    d["k_in"] = CFG.flow_k_in; d["k_out"] = CFG.flow_k_out;
    String s; serializeJson(d, s); r->send(200, "application/json", s);
  });
  server.on("/api/flow", HTTP_POST, [](AsyncWebServerRequest* r){
    if (r->hasParam("k_in", true)) CFG.flow_k_in = r->getParam("k_in", true)->value().toFloat();
    if (r->hasParam("k_out", true)) CFG.flow_k_out = r->getParam("k_out", true)->value().toFloat();
    saveCfg(); r->send(200, "text/plain", "OK");
  });

  server.on("/api/tg", HTTP_GET, [](AsyncWebServerRequest* r){
    DynamicJsonDocument d(160);
    d["token_ok"] = TG_TOKEN.length() > 10;
    d["chat"] = TG_CHAT;
    String s; serializeJson(d, s); r->send(200, "application/json", s);
  });
  server.on("/api/tg", HTTP_POST, [](AsyncWebServerRequest* r){
    bool changed = false;
    if (r->hasParam("token", true)) { String t = r->getParam("token", true)->value(); if (t.length() > 10) { TG_TOKEN = t; changed = true; } }
    if (r->hasParam("chat", true)) { String c = r->getParam("chat", true)->value(); if (c.length() > 0) { TG_CHAT = c; changed = true; } }
    if (changed) { saveCfg(); delete tgBot; tgBot = nullptr; }
    r->send(200, "text/plain", "OK");
  });
  server.on("/api/tg/test", HTTP_POST, [](AsyncWebServerRequest* r){
    bool ok = false;
    if (tgEnsure() && TG_CHAT.length()) ok = tgBot->sendMessage(TG_CHAT, "\ud83d\udd14 *Prueba Telegram* desde Placa A", "Markdown");
    DynamicJsonDocument d(256);
    d["ok"] = ok; d["token_ok"] = TG_TOKEN.length() > 10; d["chat_ok"] = TG_CHAT.length() > 0;
    String s; serializeJson(d, s); r->send(ok ? 200 : 500, "application/json", s);
  });

  server.on("/api/relay/set", HTTP_POST, [](AsyncWebServerRequest* r){
    if (!r->hasParam("ch", true) || !r->hasParam("on", true)) { r->send(400, "text/plain", "falta ch/on"); return; }
    int ch = r->getParam("ch", true)->value().toInt();
    int on = r->getParam("on", true)->value().toInt();
    if (ch < 0 || ch > 5) { r->send(400, "text/plain", "ch fuera de rango"); return; }
    relayWrite((uint8_t)ch, on != 0);
    r->send(200, "text/plain", "OK");
  });
  server.on("/api/relay/pulse", HTTP_POST, [](AsyncWebServerRequest* r){
    if (!r->hasParam("ch", true)) { r->send(400, "text/plain", "falta ch"); return; }
    int ch = r->getParam("ch", true)->value().toInt();
    int ms = r->hasParam("ms", true) ? r->getParam("ms", true)->value().toInt() : 500;
    if (ch < 0 || ch > 5) { r->send(400, "text/plain", "ch fuera de rango"); return; }
    relayWrite((uint8_t)ch, true); delay(ms); relayWrite((uint8_t)ch, false);
    r->send(200, "text/plain", "OK");
  });

  server.on("/api/debug/di", HTTP_GET, [](AsyncWebServerRequest* r){
    DynamicJsonDocument d(128);
    d["D1"] = diActive(0); d["D2"] = diActive(1); d["D3"] = diActive(2);
    d["D4"] = diActive(3); d["D5"] = diActive(4); d["D6"] = diActive(5);
    String s; serializeJson(d, s); r->send(200, "application/json", s);
  });
  server.on("/api/vfd/read", HTTP_GET, [](AsyncWebServerRequest* r){
    if (!r->hasParam("reg")) { r->send(400, "text/plain", "falta reg"); return; }
    String sreg = r->getParam("reg")->value();
    uint16_t reg = sreg.startsWith("0x")
                   ? (uint16_t)strtoul(sreg.c_str(), nullptr, 16)
                   : (uint16_t)sreg.toInt();
    uint16_t val = 0;
    bool ok = mb.readHoldingRegisters(reg, 1) == mb.ku8MBSuccess;
    if (ok) val = mb.getResponseBuffer(0);
    DynamicJsonDocument d(160);
    d["ok"] = ok; d["reg"] = reg; d["val"] = val;
    String s; serializeJson(d, s); r->send(ok ? 200 : 500, "application/json", s);
  });

  server.on("/update", HTTP_POST, [](AsyncWebServerRequest* r){
    r->send(200, "text/plain", Update.hasError() ? "FAIL" : "OK");
    ESP.restart();
  }, [](AsyncWebServerRequest*, String, size_t idx, uint8_t* data, size_t len, bool final){
    if (!idx) Update.begin(UPDATE_SIZE_UNKNOWN);
    Update.write(data, len);
    if (final) Update.end(true);
  });
}

void setup() {
  Serial.begin(115200);
  delay(50);

#if ESP_IDF_VERSION_MAJOR >= 5
  esp_task_wdt_deinit();
  esp_task_wdt_config_t twdt_cfg = { .timeout_ms = 15000, .trigger_panic = false };
  esp_task_wdt_init(&twdt_cfg);
#else
  esp_task_wdt_init(15, false);
#endif
  esp_task_wdt_add(NULL);

  g_i2cMutex = xSemaphoreCreateMutex();
  Wire.begin(4, 15);
  PCF_DO.begin(); PCF_DI.begin();

  pinMode(PIN_RS485_DIR, OUTPUT);
  digitalWrite(PIN_RS485_DIR, LOW);
  RS485_UART.begin(RS485_BAUD, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
  mb.begin(CFG.vfd_id, RS485_UART);

  pinMode(FLOW_IN_PIN, INPUT); attachInterrupt(FLOW_IN_PIN, isr_flow_in, RISING);
  pinMode(FLOW_OUT_PIN, INPUT); attachInterrupt(FLOW_OUT_PIN, isr_flow_out, RISING);

  loadCfg();

  WiFiManager wm;
  String host = "hardware_a";
  WiFi.setHostname(host.c_str());
  if (!wm.autoConnect(host.c_str())) ESP.restart();

  if (MDNS.begin("hardware_a")) {
    MDNS.addService("http", "tcp", 80);
  }

  setupApi();
  server.begin();

  Serial.println("A (Hardware) listo.");
}

void loop() {
  esp_task_wdt_reset();

  static uint32_t lastLoop = 0;
  if (millis() - lastLoop > 500) {
    lastLoop = millis();

    ST.nivel_max = diActive(DI_NIVEL_MAX);
    ST.nivel_min = diActive(DI_NIVEL_MIN);
    ST.nivel_opt = diActive(DI_NIVEL_OPT);
    ST.humedad_wet = diActive(DI_HUMEDAD);
    ST.humedad_adc = ST.humedad_wet ? 4095 : 0;

    noInterrupts();
    ST.lps_in = (CFG.flow_k_in > 0) ? (float)flow_in_pulses / CFG.flow_k_in : 0.0f;
    ST.lps_out = (CFG.flow_k_out > 0) ? (float)flow_out_pulses / CFG.flow_k_out : 0.0f;
    flow_in_pulses = 0;
    flow_out_pulses = 0;
    interrupts();

    if (!ST.bloqueado && ST.humedad_wet) {
      CFG.bloqueo_activo = true;
      saveCfg();
      tgNotify("\u26a0\ufe0f *INUNDACI\u00d3N DETECTADA* - Sistema Bloqueado");
    }
    ST.bloqueado = CFG.bloqueo_activo;

    aplicarEV();
    vfdPoll();
    aplicarMotor();
  }
}
