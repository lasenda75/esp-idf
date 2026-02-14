/**************** KC868-A6  HARDWARE (A) + HTTP API + OTA *****************
 *  Basado en el firmware completo original, pero sin web/SPIFFS.
 *  - Expone /api/* para que la placa B haga proxy.
 *  - OTA por /update.
 *  - mDNS: hardware_a.local
 ***************************************************************************/
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <NetworkClientSecure.h>
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
#include <stdlib.h>

static String chipId4() {
  uint32_t id = (uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFF);
  char s[6];
  snprintf(s, sizeof(s), "%04X", (uint16_t)id);
  return String(s);
}

SemaphoreHandle_t g_i2cMutex = nullptr;
volatile uint32_t g_i2cQuietUntil = 0;
inline bool i2cLock(uint32_t timeout_ms = 100) {
  if (!g_i2cMutex) return true;
  return xSemaphoreTake(g_i2cMutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
inline void i2cUnlock() {
  if (g_i2cMutex) xSemaphoreGive(g_i2cMutex);
}

/* ---------- Telegram ---------- */
NetworkClientSecure tgClient;
UniversalTelegramBot* tgBot = nullptr;
String TG_TOKEN, TG_CHAT;

bool tgEnsure() {
  tgClient.setInsecure();
  if (!tgBot && TG_TOKEN.length()) {
    tgBot = new UniversalTelegramBot(TG_TOKEN, tgClient);
  }
  return tgBot != nullptr;
}

bool tgSendDbg(const String& msg, String* out = nullptr) {
  if (!tgEnsure()) {
    if (out) *out = "sin bot/token";
    return false;
  }
  if (!TG_CHAT.length()) {
    if (out) *out = "CHAT_ID vaco";
    return false;
  }
  bool ok = tgBot->sendMessage(TG_CHAT, msg, "Markdown");
  if (out) *out = ok ? "OK" : "FALLO";
  return ok;
}

void tgNotify(const String& msg) {
  tgSendDbg(msg, nullptr);
}

/* ---------- I2C  KC868-A6 ---------- */
PCF8574 PCF_DO(0x24, 4, 15);
const uint8_t DO_PIN[6] = { P0, P1, P2, P3, P4, P5 };

PCF8574 PCF_DI(0x22, 4, 15);
const uint8_t DI_PIN[6] = { P5, P4, P3, P2, P1, P0 };
uint8_t       g_DI_BIT[6] = { 5, 4, 3, 2, 1, 0 };

constexpr bool DI_ACTIVE_LOW = true;
bool g_relays_active_high = false;

volatile bool g_relayDirty[6]   = {false, false, false, false, false, false};
bool          g_relayDesired[6] = {false, false, false, false, false, false};

inline void relayRequest(uint8_t ch, bool on) {
  if (ch >= 6) return;
  g_relayDesired[ch] = on;
  g_relayDirty[ch]   = true;
  g_i2cQuietUntil    = millis() + 200;
}

inline bool diActive(uint8_t ch) {
  if (ch >= 6) return false;
  if (!i2cLock()) return false;

  Wire.beginTransmission(0x22);
  Wire.write(0xFF);
  (void)Wire.endTransmission();
  delayMicroseconds(120);

  Wire.requestFrom((uint8_t)0x22, (uint8_t)1);
  if (Wire.available() < 1) {
    i2cUnlock();
    return false;
  }
  uint8_t port = Wire.read();
  i2cUnlock();

  bool low = ((port & (1 << g_DI_BIT[ch])) == 0);
  return DI_ACTIVE_LOW ? low : !low;
}

void relayProcess();
void relayWrite(uint8_t ch, bool on);

volatile uint8_t g_doShadow = 0xFF;

inline bool doRawWrite0x24(uint8_t val) {
  if (!i2cLock()) return false;
  Wire.beginTransmission(0x24);
  Wire.write(val);
  uint8_t err = Wire.endTransmission();
  i2cUnlock();
  return (err == 0);
}

inline void relayWrite(uint8_t ch, bool on) {
  if (ch >= 6) return;
  const uint8_t bit  = (uint8_t)DO_PIN[ch];
  const uint8_t mask = (uint8_t)(1u << bit);
  const bool driveLow = on ? !g_relays_active_high : g_relays_active_high;

  const bool isLowNow = ((g_doShadow & mask) == 0);
  if (isLowNow == driveLow) return;

  if (driveLow) g_doShadow &= (uint8_t)~mask;
  else          g_doShadow |= (uint8_t) mask;

  doRawWrite0x24(g_doShadow);
}

void relayProcess() {
  for (uint8_t ch = 0; ch < 6; ch++) {
    if (g_relayDirty[ch]) {
      g_relayDirty[ch] = false;
      relayWrite(ch, g_relayDesired[ch]);
    }
  }
}

#define DI_PRESOSTATO   0
#define DI_NIVEL_MAX    1
#define DI_NIVEL_MIN    2
#define DI_NIVEL_OPT    3
#define DI_HUMEDAD      4
#define DI_RESERVA      5

/* ---------- RS485 / SU-600 ---------- */
#define RS485_UART          Serial2
#define RS485_RX_PIN        14
#define RS485_TX_PIN        27
#define PIN_RS485_DIR       -1
#define RS485_BAUD          9600

struct VFDMap {
  uint16_t reg_cmd   = 0x2000;
  uint16_t reg_setf  = 0x2001;
  uint16_t reg_r_set = 0x2102;
  uint16_t reg_r_out = 0x2103;
  uint16_t reg_press = 0x2107;
  uint16_t reg_flags = 0x2101;
  uint16_t reg_stat2 = 0x2116;
  float    scale_hz  = 0.01f;
  float    scale_bar = 0.01f;
} VFD;

volatile bool g_vfd_ready = false;
uint32_t g_boot_mute_until = 0;
AsyncWebServer server(80);
Preferences prefs;
ModbusMaster mb;

/* ---------- Caudalmetros IO1/IO2 ---------- */
volatile uint32_t flow_in_pulses  = 0;
volatile uint32_t g_inhibit_until = 0;
/* ---------- Config / Estado ---------- */
  String   http_user = "admin";
  String   http_pass = "1234";
  uint8_t  ev_mode  = 0;
  uint8_t  mot_mode = 0;
  uint8_t  vfd_id   = 1;
  uint32_t vfd_baud = RS485_BAUD;
  float    vfd_set_hz_def = 25.0;
  float    vfd_fmax_hz    = 50.0f;
  float    flow_k_in  = 450.0f;
  float    flow_k_out = 450.0f;
  uint8_t  ev_ch   = 0;
  uint8_t  achq_ch = 1;
  bool     relays_active_high = false;
  bool     bloqueo_activo = false;

  bool   bloqueado  = false;
  bool   bypass     = false;
  float  presion_bar = 0.0;
  float  vfd_set_hz = 0.0;
  float  vfd_out_hz = 0.0;
  float  lps_in     = 0.0;
  float  lps_out    = 0.0;
  bool   flujo_in   = false;
  bool   flujo_out  = false;
  String presostato = "-";
  bool presion_ok = false;
  bool   nivel_max  = false;
  bool   nivel_min  = false;
  bool   nivel_opt  = false;
  bool   humedad_wet = false;
  uint16_t humedad_adc = 0;
  String ev_txt     = "Desconocida";
  bool   ev_abierta = false;
  bool   motor_run  = false;

struct Ctrl {
  float    p_start;
  float    p_stop;
  float    hz_min;
  uint16_t dwell_ms;
} CTRL;
void loadCtrl() {
  CTRL.p_start  = prefs.getFloat ("p_start", 2.0f);
  CTRL.p_stop   = prefs.getFloat ("p_stop",  2.5f);
  CTRL.hz_min   = prefs.getFloat ("hz_min", 25.0f);
}
void saveCtrl() {
  prefs.begin("ctrl", false);
  prefs.putFloat ("p_start", CTRL.p_start);
  prefs.putFloat ("p_stop",  CTRL.p_stop);
  prefs.putFloat ("hz_min",  CTRL.hz_min);
  prefs.putUShort("dwell",   CTRL.dwell_ms);
  prefs.end();
}
void leerDigitales();
void aplicarEV();
void aplicarMotor();

bool isAuth(AsyncWebServerRequest* r) {
  if (!r->authenticate(CFG.http_user.c_str(), CFG.http_pass.c_str())) {
    r->requestAuthentication();
    return false;
  }
  return true;
}

void loadCfg() {
  prefs.begin("cfg", true);
  CFG.http_user = prefs.getString("u", CFG.http_user);
  CFG.http_pass = prefs.getString("p", CFG.http_pass);
  CFG.ev_mode   = prefs.getUChar("evm", CFG.ev_mode);
  CFG.mot_mode  = prefs.getUChar("mm",  CFG.mot_mode);
  CFG.vfd_id    = prefs.getUChar("vid", CFG.vfd_id);
  CFG.vfd_baud  = prefs.getUInt("vbd", CFG.vfd_baud);
  CFG.vfd_set_hz_def = prefs.getFloat("vset", CFG.vfd_set_hz_def);
  CFG.vfd_fmax_hz    = prefs.getFloat("vmax", 50.0f);
  CFG.bloqueo_activo = prefs.getBool("blk", CFG.bloqueo_activo);
  TG_TOKEN = prefs.getString("tg_t", "");
  TG_CHAT  = prefs.getString("tg_c", "");
  CFG.flow_k_in  = prefs.getFloat("fk_in",  CFG.flow_k_in);
  CFG.flow_k_out = prefs.getFloat("fk_out", CFG.flow_k_out);
  CFG.ev_ch      = prefs.getUChar("evch",  CFG.ev_ch);
  CFG.achq_ch    = prefs.getUChar("aqch",  CFG.achq_ch);
  CFG.relays_active_high = prefs.getBool("rah", CFG.relays_active_high);
  uint16_t r; float f;
  r = prefs.getUShort("reg_run",  VFD.reg_cmd);   VFD.reg_cmd   = r;
  r = prefs.getUShort("reg_setf", VFD.reg_setf);  VFD.reg_setf  = r;
  r = prefs.getUShort("reg_outf", VFD.reg_r_out); VFD.reg_r_out = r;
  r = prefs.getUShort("reg_pres", VFD.reg_press); VFD.reg_press = r;
  f = prefs.getFloat ("scale_f",  VFD.scale_hz);  VFD.scale_hz  = f;
  f = prefs.getFloat ("scale_p",  VFD.scale_bar); VFD.scale_bar = f;
  g_relays_active_high = CFG.relays_active_high;
  prefs.putString("u", CFG.http_user);
  prefs.putString("p", CFG.http_pass);
  prefs.putUChar("evm", CFG.ev_mode);
  prefs.putUChar("mm",  CFG.mot_mode);
  prefs.putUChar("vid", CFG.vfd_id);
  prefs.putUInt("vbd", CFG.vfd_baud);
  prefs.putFloat("vset", CFG.vfd_set_hz_def);
  prefs.putFloat("vmax", CFG.vfd_fmax_hz);
  prefs.putBool("blk", CFG.bloqueo_activo);
  prefs.putString("tg_t", TG_TOKEN);
  prefs.putString("tg_c", TG_CHAT);
  prefs.putFloat("fk_in",  CFG.flow_k_in);
  prefs.putFloat("fk_out", CFG.flow_k_out);
  prefs.putUChar("evch",   CFG.ev_ch);
  prefs.putUChar("aqch",   CFG.achq_ch);
  prefs.putBool("rah",     CFG.relays_active_high);
  prefs.putUShort("reg_run",  VFD.reg_cmd);
  prefs.putUShort("reg_setf", VFD.reg_setf);
  prefs.putUShort("reg_outf", VFD.reg_r_out);
  prefs.putUShort("reg_pres", VFD.reg_press);
  prefs.putFloat ("scale_f",  VFD.scale_hz);
  prefs.putFloat ("scale_p",  VFD.scale_bar);
  g_relays_active_high = CFG.relays_active_high;
void preTx() {
  if (PIN_RS485_DIR >= 0) {
    digitalWrite(PIN_RS485_DIR, HIGH);
    delayMicroseconds(2);
  }
}
void postTx() {
  if (PIN_RS485_DIR >= 0) {
    delayMicroseconds(2);
    digitalWrite(PIN_RS485_DIR, LOW);
  }
}

void vfdBegin() {
  if (PIN_RS485_DIR >= 0) {
    pinMode(PIN_RS485_DIR, OUTPUT);
    digitalWrite(PIN_RS485_DIR, LOW);
  }
  RS485_UART.begin(CFG.vfd_baud, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
  mb.begin(CFG.vfd_id, RS485_UART);
  mb.preTransmission(preTx);
  mb.postTransmission(postTx);
struct Protec {
  uint16_t in_s  = 10;
  uint16_t out_s = 2;
} PROT;

void loadProt() {
  prefs.begin("prot", true);
  PROT.in_s  = prefs.getUShort("in_s", 10);
  PROT.out_s = prefs.getUShort("out_s", 2);
  prefs.end();
}
  prefs.putUShort("in_s",  PROT.in_s);
  prefs.end();
}

bool vfdWrite06(uint16_t reg, uint16_t val) {
  for (int i = 0; i < 2; i++) {
    if (mb.writeSingleRegister(reg, val) == mb.ku8MBSuccess) return true;
    delay(5);
  }
  return false;
bool vfdRead03(uint16_t reg, uint16_t &val) {
  for (int i = 0; i < 2; i++) {
    if (mb.readHoldingRegisters(reg, 1) == mb.ku8MBSuccess) {
      val = mb.getResponseBuffer(0);
      return true;
    }
    delay(5);
  return false;
bool vfdRunFwd() {
  ST.motor_run = true;
  return vfdWrite06(VFD.reg_cmd, 0x0012);
bool vfdStop() {
  ST.motor_run = false;
  return vfdWrite06(VFD.reg_cmd, 0x0001);
bool vfdSetHz(float hz) {
  if (hz < 0) hz = 0;
  float pct = 100.0f * hz / CFG.vfd_fmax_hz;
  if (pct > 100.0f) pct = 100.0f;
  uint16_t raw = (uint16_t)(pct * 100.0f + 0.5f);
  return vfdWrite06(VFD.reg_setf, raw);

  uint16_t v = 0;
  if (vfdRead03(VFD.reg_r_set, v)) ST.vfd_set_hz = v * VFD.scale_hz;
  if (vfdRead03(VFD.reg_r_out, v)) ST.vfd_out_hz = v * VFD.scale_hz;
  bool okp = vfdRead03(VFD.reg_press, v);
  if (okp) {
    ST.presion_bar = v * VFD.scale_bar;
    g_vfd_ready = true;
  if (vfdRead03(VFD.reg_flags, v)) ST.motor_run = (v & 0x0001);
}

void leerDigitales() {
  bool pres_ok = diActive(DI_PRESOSTATO);
  ST.presion_ok = pres_ok;
  ST.presostato = pres_ok ? "Presin OK" : "Sin presin";
  ST.nivel_max = diActive(DI_NIVEL_MAX);
  ST.nivel_min = diActive(DI_NIVEL_MIN);
  ST.nivel_opt = diActive(DI_NIVEL_OPT);
  ST.humedad_wet = diActive(DI_HUMEDAD);
  ST.humedad_adc = ST.humedad_wet ? 4095 : 0;

  static bool prevBlk = CFG.bloqueo_activo;
  if (!CFG.bloqueo_activo) {
    if (ST.humedad_wet) {
      CFG.bloqueo_activo = true;
      saveCfg();
      tgNotify(" *BLOQUEO ACTIVADO* por inundacin.\nEV cerrada y motor bloqueado.");
    }
  ST.bloqueado = CFG.bloqueo_activo;
  if (prevBlk != ST.bloqueado) {
    prevBlk = ST.bloqueado;
    if (!ST.bloqueado) tgNotify("? *Bloqueo liberado*");
  static bool prevNivelMin = false;
  if (ST.nivel_min && !prevNivelMin) {
    tgNotify(" *Nivel bajo detectado*: motor detenido en AUTO, EV permanece abierta para rellenar.");
    vfdStop();
  if (!ST.nivel_min && prevNivelMin) {
    tgNotify("? *Nivel recuperado*: motor habilitado de nuevo en modo AUTO.");
  }
  prevNivelMin = ST.nivel_min;

  ST.ev_txt = ST.ev_abierta ? "Electrovlvula: Abierta" : "Electrovlvula: Cerrada";
  static bool last_run_cmd = false;
  static uint32_t t_ok = 0;
  if (!g_vfd_ready) {
    if (last_run_cmd) {
      vfdStop();
      last_run_cmd = false;
    }
    return;
  }

    if (!last_run_cmd) {
      vfdSetHz(CFG.vfd_set_hz_def);
      vfdRunFwd();
      last_run_cmd = true;
    }
    if (last_run_cmd) {
      vfdStop();
      last_run_cmd = false;
    }
    if (last_run_cmd) {
      vfdStop();
      last_run_cmd = false;
    }
    t_ok = 0;
    return;
  }

  if (millis() < g_inhibit_until) {
    if (last_run_cmd) {
      vfdStop();
      last_run_cmd = false;
    }
  const float p = ST.presion_bar;
  if (!last_run_cmd && p < CTRL.p_start) {
    vfdSetHz(CTRL.hz_min);
    vfdRunFwd();
    last_run_cmd = true;
    t_ok = 0;
    return;

  if (last_run_cmd && p > CTRL.p_stop) {
    if (t_ok == 0) t_ok = millis();
    if (millis() - t_ok >= CTRL.dwell_ms) {
      vfdStop();
      last_run_cmd = false;
      t_ok = 0;
    }
    return;
  }

  t_ok = 0;
}

void aplicarEV() {
  static int8_t last_ch = -1;
  static bool   last_on = false;

  uint8_t mode = CFG.ev_mode;
  bool abrir = (mode == 1) ? true
               : (mode == 2) ? false
               : (!ST.bloqueado && !ST.nivel_max);

  if (last_ch != (int)CFG.ev_ch || last_on != abrir) {
    relayWrite(CFG.ev_ch, abrir);
    last_ch = CFG.ev_ch;
    last_on = abrir;
  }

  ST.ev_abierta = abrir;
  ST.ev_txt = abrir ? "Electrovlvula: Abierta" : "Electrovlvula: Cerrada";
  DynamicJsonDocument d(896);
  d["bloqueado"]   = ST.bloqueado;
  d["bypass"]      = ST.bypass;
  d["vfd_set_hz"]  = ST.vfd_set_hz;
  d["vfd_out_hz"]  = ST.vfd_out_hz;
  d["lps_in"]      = ST.lps_in;
  d["lps_out"]     = ST.lps_out;
  d["flujo_in"]    = ST.flujo_in;
  d["flujo_out"]   = ST.flujo_out;
  d["presostato"]  = ST.presostato;
  d["nivel_max"]   = ST.nivel_max;
  d["nivel_min"]   = ST.nivel_min;
  d["nivel_opt"]   = ST.nivel_opt;
  d["ev_txt"]      = ST.ev_txt;
  d["ev_abierta"]  = ST.ev_abierta;
  d["motor_run"]   = ST.motor_run;
  d["ev_mode"]      = CFG.ev_mode;
  d["mot_mode"]     = CFG.mot_mode;
  d["ev_mode_txt"]  = (CFG.ev_mode == 0) ? "AUTO"
                      : (CFG.ev_mode == 1) ? "MANUAL_OPEN" : "MANUAL_CLOSE";
  d["mot_mode_txt"] = (CFG.mot_mode == 0) ? "AUTO"
                      : (CFG.mot_mode == 1) ? "MANUAL_ON"  : "MANUAL_OFF";
  d["humedad"]     = ST.humedad_wet ? "ALERTA" : "OK";
  d["ev_ch"]       = CFG.ev_ch;
  d["achq_ch"]     = CFG.achq_ch;
  d["rah"]         = CFG.relays_active_high;
  d["prot_in_s"]  = PROT.in_s;
void apiEndpoints() {
  server.on("/api/debug/di8", HTTP_GET, [](AsyncWebServerRequest * r) {
    if (!isAuth(r)) return;
    if (!i2cLock()) {
      r->send(503, "text/plain", "I2C BUSY");
      return;
    }

    Wire.beginTransmission(0x22);
    Wire.write(0xFF);
    uint8_t errW = Wire.endTransmission();
    delayMicroseconds(200);

    Wire.requestFrom((uint8_t)0x22, (uint8_t)1);
    if (Wire.available() < 1) {
      i2cUnlock();
      r->send(500, "text/plain", "no data");
      return;
    }
    uint8_t port = Wire.read();

    i2cUnlock();

    DynamicJsonDocument d(200);
    d["errW"] = errW;
    d["port"] = port;
    JsonObject P = d.createNestedObject("P");
    for (int b = 0; b < 8; b++) P[String(b)] = (bool)((port >> b) & 1);

    String s; serializeJson(d, s);
    r->send(200, "application/json", s);
  server.on("/api/ctrl", HTTP_GET, [](AsyncWebServerRequest * r) {
    if (!isAuth(r)) return;
    DynamicJsonDocument d(200);
    d["p_start"] = CTRL.p_start; d["p_stop"] = CTRL.p_stop;
    d["hz_min"] = CTRL.hz_min;   d["dwell_ms"] = CTRL.dwell_ms;
    String s; serializeJson(d, s); r->send(200, "application/json", s);
  server.on("/api/ctrl", HTTP_POST, [](AsyncWebServerRequest * r) {
    if (!isAuth(r)) return;
    if (r->hasParam("p_start", true))  CTRL.p_start  = r->getParam("p_start", true)->value().toFloat();
    if (r->hasParam("p_stop",  true))  CTRL.p_stop   = r->getParam("p_stop",  true)->value().toFloat();
    if (r->hasParam("hz_min",  true))  CTRL.hz_min   = r->getParam("hz_min",  true)->value().toFloat();
    if (r->hasParam("dwell_ms", true))  CTRL.dwell_ms = r->getParam("dwell_ms", true)->value().toInt();
    saveCtrl();
    r->send(200, "application/json", "{\"ok\":true}");
  server.on("/api/cred", HTTP_GET, [](AsyncWebServerRequest * r) {
    if (!isAuth(r)) return; DynamicJsonDocument d(160);
    d["user"] = CFG.http_user; d["pass"] = CFG.http_pass;
  server.on("/api/cred", HTTP_POST, [](AsyncWebServerRequest * r) {
    if (!isAuth(r)) return;
    if (r->hasParam("user", true)) CFG.http_user = r->getParam("user", true)->value();
    if (r->hasParam("pass", true)) CFG.http_pass = r->getParam("pass", true)->value();
  server.on("/api/prot", HTTP_GET, [](AsyncWebServerRequest * r) {
    if (!isAuth(r)) return;
    DynamicJsonDocument d(128);
    d["in_s"]  = PROT.in_s;
    d["out_s"] = PROT.out_s;
    String s; serializeJson(d, s);
    r->send(200, "application/json", s);
  server.on("/api/prot", HTTP_POST, [](AsyncWebServerRequest * r) {
    if (!isAuth(r)) return;
    if (r->hasParam("in_s",   true)) PROT.in_s  = (uint16_t)r->getParam("in_s",   true)->value().toInt();
    if (r->hasParam("out_s",  true)) PROT.out_s = (uint16_t)r->getParam("out_s",  true)->value().toInt();
    if (r->hasParam("vacio_s", true)) PROT.out_s = (uint16_t)r->getParam("vacio_s", true)->value().toInt();
    saveProt();
    r->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/tg", HTTP_GET, [](AsyncWebServerRequest * r) {
    if (!isAuth(r)) return;
    DynamicJsonDocument d(160);
    d["token_ok"] = TG_TOKEN.length() > 10;
    d["chat"]     = TG_CHAT;
    String s; serializeJson(d, s);
    r->send(200, "application/json", s);
  server.on("/api/tg", HTTP_POST, [](AsyncWebServerRequest * r) {
    if (!isAuth(r)) return;

    bool changed = false;

    if (r->hasParam("token", true)) {
      String t = r->getParam("token", true)->value();
      if (t.length() > 10) {
        TG_TOKEN = t;
        changed = true;
      }
    }
    if (r->hasParam("chat", true)) {
      String c = r->getParam("chat", true)->value();
      if (c.length() > 0) {
        TG_CHAT = c;
        changed = true;
      }
    }

    if (changed) {
      saveCfg();
      delete tgBot; tgBot = nullptr;
    }

    r->send(200, "text/plain", "OK");

  server.on("/api/tg/test", HTTP_POST, [](AsyncWebServerRequest * r) {
    if (!isAuth(r)) return;
    String resp;
    bool ok = tgSendDbg(" *Prueba Telegram* desde Sala Mquinas", &resp);
    DynamicJsonDocument d(256);
    d["token_ok"] = TG_TOKEN.length() > 10;
    d["chat_ok"]  = TG_CHAT.length() > 0;
    d["ok"]       = ok;
    d["response"] = resp;
    String s; serializeJson(d, s);
    r->send(ok ? 200 : 500, "application/json", s);
  server.on("/api/flow", HTTP_GET, [](AsyncWebServerRequest * r) {
    if (!isAuth(r)) return; DynamicJsonDocument d(128);
  server.on("/api/flow", HTTP_POST, [](AsyncWebServerRequest * r) {
    if (!isAuth(r)) return;
    if (r->hasParam("k_in",  true)) CFG.flow_k_in  = r->getParam("k_in",  true)->value().toFloat();
  server.on("/api/estados", HTTP_GET, [](AsyncWebServerRequest * r) {
    if (!isAuth(r)) return;
    r->send(200, "application/json", jsonEstados());
  server.on("/api/state",   HTTP_GET, [](AsyncWebServerRequest * r) {
    if (!isAuth(r)) return;
    r->send(200, "application/json", jsonEstados());
  });

  server.on("/api/valvula/mode", HTTP_POST, [](AsyncWebServerRequest * r) {
    if (!isAuth(r)) return;
    if (!r->hasParam("m", true)) {
      r->send(400, "text/plain", "falta m");
      return;
    }
    String m = r->getParam("m", true)->value(); m.toLowerCase();
    if      (m == "auto")  CFG.ev_mode = 0;
    else if (m == "open")  CFG.ev_mode = 1;
    else if (m == "close") CFG.ev_mode = 2;
    saveCfg(); aplicarEV();
  server.on("/api/valvula/auto",  HTTP_POST, [](AsyncWebServerRequest * r) {
    if (!isAuth(r)) return;
    CFG.ev_mode = 0;
    saveCfg();
    aplicarEV();
    r->send(200, "text/plain", "OK");
  });
  server.on("/api/valvula/open",  HTTP_POST, [](AsyncWebServerRequest * r) {
    if (!isAuth(r)) return;
    CFG.ev_mode = 1;
    saveCfg();
    aplicarEV();
    r->send(200, "text/plain", "OK");
  });
  server.on("/api/valvula/close", HTTP_POST, [](AsyncWebServerRequest * r) {
    if (!isAuth(r)) return;
    CFG.ev_mode = 2;
    saveCfg();
    aplicarEV();
    r->send(200, "text/plain", "OK");
  server.on("/api/motor/mode", HTTP_POST, [](AsyncWebServerRequest * r) {
    if (!isAuth(r)) return;
    if (!r->hasParam("m", true)) {
      r->send(400, "text/plain", "falta m");
      return;
    }
    String m = r->getParam("m", true)->value(); m.toLowerCase();
    if      (m == "auto") CFG.mot_mode = 0;
    else if (m == "on")   CFG.mot_mode = 1;
    else if (m == "off")  CFG.mot_mode = 2;
    saveCfg(); aplicarMotor();
    r->send(200, "text/plain", "OK");
  });
  server.on("/api/motor/auto",    HTTP_POST, [](AsyncWebServerRequest * r) {
    if (!isAuth(r)) return;
    CFG.mot_mode = 0;
    saveCfg();
    aplicarMotor();
    r->send(200, "text/plain", "OK");
  });
  server.on("/api/motor/start",   HTTP_POST, [](AsyncWebServerRequest * r) {
    if (!isAuth(r)) return;
    CFG.mot_mode = 1;
    saveCfg();
    aplicarMotor();
    r->send(200, "text/plain", "OK");
  });
  server.on("/api/motor/stop",    HTTP_POST, [](AsyncWebServerRequest * r) {
    if (!isAuth(r)) return;
    CFG.mot_mode = 2;
    saveCfg();
    aplicarMotor();
    r->send(200, "text/plain", "OK");
  });

  server.on("/api/unblock", HTTP_POST, [](AsyncWebServerRequest * r) {
    if (!isAuth(r)) return;
    bool cond_ok = (!ST.humedad_wet) && (!ST.nivel_min || ST.nivel_opt);
    if (cond_ok) {
      CFG.bloqueo_activo = false;
      saveCfg();
      tgNotify(" Desbloqueo manual ejecutado");
      r->send(200, "text/plain", "OK");
    }
    else {
      r->send(423, "text/plain", "Condicin insegura");
    }
  });

  server.on("/api/relay/set", HTTP_POST, [](AsyncWebServerRequest * r) {
    if (!isAuth(r)) return;
    if (!r->hasParam("ch", true) || !r->hasParam("on", true)) {
      r->send(400, "text/plain", "falta ch/on");
      return;
    }


    if (ch < 0 || ch > 5) {
      r->send(400, "text/plain", "ch fuera de rango");
      return;
    }

    g_i2cQuietUntil = millis() + 200;


  server.on("/api/relay/pulse", HTTP_POST, [](AsyncWebServerRequest * r) {
    if (!isAuth(r)) return;
    if (!r->hasParam("ch", true)) {
      r->send(400, "text/plain", "falta ch");
      return;
    }
    if (ch < 0 || ch > 5) {
      r->send(400, "text/plain", "ch fuera de rango");
      return;
    }
  server.on("/api/relmap", HTTP_GET, [](AsyncWebServerRequest * r) {
    if (!isAuth(r)) return; DynamicJsonDocument d(128);
    d["ev_ch"] = CFG.ev_ch; d["achq_ch"] = CFG.achq_ch; d["rah"] = CFG.relays_active_high;
  server.on("/api/relmap", HTTP_POST, [](AsyncWebServerRequest * r) {
    if (!isAuth(r)) return;
    if (r->hasParam("ev",  true)) {
      int v = r->getParam("ev",  true)->value().toInt();
      if (v >= 0 && v <= 5) CFG.ev_ch = v;
    }
    if (r->hasParam("ach", true)) {
      int v = r->getParam("ach", true)->value().toInt();
      if (v >= 0 && v <= 5) CFG.achq_ch = v;
    }
    saveCfg(); r->send(200, "text/plain", "OK");
  });
  server.on("/api/relpolarity", HTTP_POST, [](AsyncWebServerRequest * r) {
    if (!isAuth(r)) return;
    if (!r->hasParam("rah", true)) {
      r->send(400, "text/plain", "falta rah");
      return;
    }
    CFG.relays_active_high = (r->getParam("rah", true)->value().toInt() != 0);
    saveCfg();
    r->send(200, "text/plain", "OK");
  });

  server.on("/api/debug/di", HTTP_GET, [](AsyncWebServerRequest * r) {
    if (!isAuth(r)) return;

    if (!i2cLock()) {
      r->send(503, "text/plain", "I2C BUSY");
      return;
    }

    Wire.beginTransmission(0x22);
    Wire.write(0xFF);
    uint8_t errW = Wire.endTransmission();
    delayMicroseconds(150);

    Wire.requestFrom((uint8_t)0x22, (uint8_t)1);
    if (Wire.available() < 1) {
      i2cUnlock();
      r->send(500, "text/plain", "no data");
      return;
    }
    uint8_t port = Wire.read();
    i2cUnlock();

    uint8_t raw = 0;
    for (int i = 0; i < 6; i++) {
      bool active = ((port & (1 << g_DI_BIT[i])) == 0);
      if (active) raw |= (1 << i);
    }

    DynamicJsonDocument d(200);
    d["errW"] = errW;
    d["port"] = port;
    d["raw"]  = raw;
    JsonObject bits = d.createNestedObject("bits");
    bits["D1"] = (bool)(raw & (1 << 0));
    bits["D2"] = (bool)(raw & (1 << 1));
    bits["D3"] = (bool)(raw & (1 << 2));
    bits["D4"] = (bool)(raw & (1 << 3));
    bits["D5"] = (bool)(raw & (1 << 4));
    bits["D6"] = (bool)(raw & (1 << 5));

    String s; serializeJson(d, s);
    r->send(200, "application/json", s);
  });

  server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest * r) {
    if (!isAuth(r)) return;
    DynamicJsonDocument d(512);
    d["vfd_id"]   = CFG.vfd_id;
    d["vfd_baud"] = CFG.vfd_baud;
    d["vfd_par"]  = "N";
    d["reg_run"]  = VFD.reg_cmd;
    d["reg_setf"] = VFD.reg_setf;
    d["reg_outf"] = VFD.reg_r_out;
    d["reg_pres"] = VFD.reg_press;
    d["scale_f"]  = VFD.scale_hz;
    d["scale_p"]  = VFD.scale_bar;
    d["k_in"]     = CFG.flow_k_in;
    d["k_out"]    = CFG.flow_k_out;
    d["vfd_fmax_hz"] = CFG.vfd_fmax_hz;
    String s; serializeJson(d, s);
    r->send(200, "application/json", s);
  });

  server.on("/api/config", HTTP_POST, [](AsyncWebServerRequest * r) {
    if (!isAuth(r)) return;

    if (r->hasParam("vfd_id",   true)) CFG.vfd_id   = (uint8_t) r->getParam("vfd_id",   true)->value().toInt();
    if (r->hasParam("vfd_baud", true)) CFG.vfd_baud = (uint32_t)r->getParam("vfd_baud", true)->value().toInt();

    if (r->hasParam("reg_run",  true)) VFD.reg_cmd   = (uint16_t)r->getParam("reg_run",  true)->value().toInt();
    if (r->hasParam("reg_setf", true)) VFD.reg_setf  = (uint16_t)r->getParam("reg_setf", true)->value().toInt();
    if (r->hasParam("reg_outf", true)) VFD.reg_r_out = (uint16_t)r->getParam("reg_outf", true)->value().toInt();
    if (r->hasParam("reg_pres", true)) VFD.reg_press = (uint16_t)r->getParam("reg_pres", true)->value().toInt();

    if (r->hasParam("scale_f",  true)) VFD.scale_hz  = r->getParam("scale_f",  true)->value().toFloat();
    if (r->hasParam("scale_p",  true)) VFD.scale_bar = r->getParam("scale_p",  true)->value().toFloat();

    if (r->hasParam("k_in",  true))    CFG.flow_k_in  = r->getParam("k_in",  true)->value().toFloat();
    if (r->hasParam("k_out", true))    CFG.flow_k_out = r->getParam("k_out", true)->value().toFloat();
    if (r->hasParam("vfd_fmax_hz", true))
      CFG.vfd_fmax_hz = r->getParam("vfd_fmax_hz", true)->value().toFloat();
    saveCfg();
    vfdBegin();
    r->send(200, "text/plain", "OK");
  });

  server.on("/api/vfd/read", HTTP_GET, [](AsyncWebServerRequest * r) {
    if (!isAuth(r)) return;
    if (!r->hasParam("reg")) {
      r->send(400, "text/plain", "falta reg");
      return;
    }
    uint16_t val = 0; bool ok = vfdRead03(reg, val);
    d["ok"] = ok; d["reg"] = reg; d["val"] = val; d["hex"] = String("0x") + String(reg, 16);
    String s; serializeJson(d, s);
    r->send(ok ? 200 : 500, "application/json", s);
  server.on("/update", HTTP_POST, [](AsyncWebServerRequest *r){
    if (!isAuth(r)) return;
  }, [](AsyncWebServerRequest*, String, size_t idx, uint8_t *data, size_t len, bool final){
    if(!idx) Update.begin(UPDATE_SIZE_UNKNOWN);
    if(final) Update.end(true);
void i2cScanBoot() {
  uint8_t found = 0;
  for (uint8_t a = 0x20; a <= 0x27; a++) {
    Wire.beginTransmission(a);
    uint8_t err = Wire.endTransmission();
    if (err == 0) {
      found++;
    }
  }
  (void)found;
}
void setup() {
  Serial.begin(115200); delay(50);
  esp_task_wdt_config_t twdt_cfg = { .timeout_ms = 20000, .trigger_panic = false };
  esp_task_wdt_init(20, false);
  loadCfg();
  loadCtrl();
  loadProt();

  Wire.setClock(100000);
  i2cScanBoot();
  PCF_DI.begin();
  for (uint8_t i = 0; i < 6; i++) {
    PCF_DI.pinMode(DI_PIN[i], INPUT);
    PCF_DI.digitalWrite(DI_PIN[i], HIGH);
  }
  PCF_DO.begin();
  for (uint8_t p = 0; p < 8; p++) PCF_DO.pinMode(p, OUTPUT);
  g_doShadow = 0xFF;
  doRawWrite0x24(g_doShadow);
  pinMode(FLOW_IN_PIN,  INPUT);
  pinMode(FLOW_OUT_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(FLOW_IN_PIN),  isr_flow_in,  RISING);
  attachInterrupt(digitalPinToInterrupt(FLOW_OUT_PIN), isr_flow_out, RISING);

  ST.ev_abierta = false;
  ST.ev_txt = "Electrovlvula: Cerrada";

  WiFi.mode(WIFI_STA);
  String host = "hardware_a" + chipId4();
  WiFiManager wm;
  wm.setClass("invert");
  bool ok = wm.autoConnect("Sala_de_Maquinas", "claveSegura123");
  (void)ok;

  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setSleep(false);
    delay(500);

    MDNS.end();
    if (MDNS.begin("hardware_a")) {
      MDNS.setInstanceName("hardware_a");
      MDNS.addService("http", "tcp", 80);
      MDNS.addServiceTxt("http", "tcp", "path", "/");
    }

  configTime(0, 0, "pool.ntp.org");
  tgClient.setTimeout(8000);

  if (TG_TOKEN.length()) {
    tgClient.setInsecure();
    tgBot = new UniversalTelegramBot(TG_TOKEN, tgClient);
  }

  vfdBegin();
  vfdSetHz(CFG.vfd_set_hz_def);
  apiEndpoints();
  server.begin();
  g_boot_mute_until = millis() + 10000;
uint32_t tPoll = 0, tFlow = 0;

  uint32_t now = millis();
  if (now - tPoll >= 500) {
    tPoll = now;

    bool quiet = (now < g_i2cQuietUntil);

    if (!quiet) {
      leerDigitales();
      aplicarEV();
    }
    vfdPoll();
    aplicarMotor();

    if (!quiet) {
      static bool last_achq = false;
      bool achq = (CFG.bloqueo_activo && ST.humedad_wet);
      if (achq != last_achq) {
        relayWrite(CFG.achq_ch, achq);
        last_achq = achq;
      }
    }
  }
  if (now - tFlow >= 1000) {
    tFlow = now;
    uint32_t p_in = flow_in_pulses, p_out = flow_out_pulses;
    flow_in_pulses = 0; flow_out_pulses = 0;
    ST.lps_in  = (CFG.flow_k_in  > 0.0f) ? (p_in  / CFG.flow_k_in)  : 0.0f;
    ST.lps_out = (CFG.flow_k_out > 0.0f) ? (p_out / CFG.flow_k_out) : 0.0f;
    ST.flujo_in  = (p_in  > 0);
    ST.flujo_out = (p_out > 0);
  }
  static uint32_t tNoIn = 0, tNoOut = 0;
  static bool avisadoIn = false;
  if (now < g_boot_mute_until) {
    tNoIn = tNoOut = 0;
    avisadoIn = false;
  }

  if (ST.motor_run) {
    if (!ST.flujo_in && !ST.presion_ok) {
      if (!tNoIn) tNoIn = now;
      if (!avisadoIn && (now - tNoIn >= (uint32_t)PROT.in_s * 1000UL)) {
        tgNotify(" Agua de la calle cortada");
        avisadoIn = true;
      }
    } else {
      tNoIn = 0;
      avisadoIn = false;
    if (!ST.flujo_out) {
      if (!tNoOut) tNoOut = now;
      if (now - tNoOut >= (uint32_t)PROT.out_s * 1000UL) {
        vfdStop();
        tgNotify(" Paro por vaco (sin caudal de salida)");
        tNoOut = 0;
        g_inhibit_until = now + 10000UL;
      }
    } else {
      tNoOut = 0;
    }
  } else {
    tNoIn = tNoOut = 0; avisadoIn = false;

  esp_task_wdt_reset();

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
