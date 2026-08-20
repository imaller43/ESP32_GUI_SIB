// ============================================================
//  ESP32 Dashboard  —  Waveshare ESP32-S3-POE-ETH-8DI-8DO
//  DO: PCF8574 I2C expander (active-LOW driver, addr 0x20)
//  DI: GPIO direct read (active-LOW opto, INPUT_PULLUP)
// ============================================================

#include "FS.h"
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <ETH.h>
#include <HTTPClient.h>
#include <HardwareSerial.h>
#include <LittleFS.h>
#include <PubSubClient.h>
#include <UniversalTelegramBot.h>
#include <Update.h>
#include <WebServer.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>

#define FIRMWARE_VERSION 1.1
#define FS_VERSION 1.1

// ─── WebSocket bridge for cloud MQTT ─────────────────────────────
#define WSBRIDGE_RX_SIZE 1024
class WsClientBridge : public Client {
  WebSocketsClient &_ws;
  uint8_t _buf[WSBRIDGE_RX_SIZE];
  uint32_t _head = 0, _tail = 0;
  bool _connected = false;
  uint32_t _avail() const { return _head - _tail; }

public:
  explicit WsClientBridge(WebSocketsClient &ws) : _ws(ws) {}
  void push(const uint8_t *d, size_t n) {
    for (size_t i = 0; i < n && _avail() < WSBRIDGE_RX_SIZE - 1; i++)
      _buf[(_head + i) % WSBRIDGE_RX_SIZE] = d[i];
    _head += n;
  }
  void setConnected(bool c) {
    _connected = c;
    if (!c) {
      _head = _tail = 0;
    }
  }
  int connect(IPAddress, uint16_t) override { return (int)_connected; }
  int connect(const char *, uint16_t) override { return (int)_connected; }
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t *buf, size_t sz) override {
    _ws.sendBIN(buf, sz);
    return sz;
  }
  int available() override {
    if (!_avail())
      _ws.loop();
    return (int)_avail();
  }
  int read() override {
    unsigned long t = millis();
    while (!_avail()) {
      _ws.loop();
      if (millis() - t > 3000)
        return -1;
    }
    return _buf[_tail++ % WSBRIDGE_RX_SIZE];
  }
  int read(uint8_t *buf, size_t sz) override {
    int n = 0;
    while ((size_t)n < sz) {
      int b = read();
      if (b < 0)
        break;
      buf[n++] = (uint8_t)b;
    }
    return n;
  }
  int peek() override {
    if (!_avail())
      _ws.loop();
    return _avail() ? _buf[_tail % WSBRIDGE_RX_SIZE] : -1;
  }
  void flush() override {}
  void stop() override {
    _ws.disconnect();
    setConnected(false);
  }
  uint8_t connected() override { return _connected; }
  operator bool() override { return _connected; }
};

// ================= NETWORK ================
const char *ssid = "SMC";
const char *password = "Smcw62021";
bool eth_connected = false;

// ================= DIGITAL IO =================
// DI: INPUT_PULLUP, active-LOW opto (read LOW = input ON)
// DO: PCF8574 at 0x20, active-LOW driver
//     outputState bit=1 → logical ON → write 0 to expander pin
const int inputPins[8] = {4, 5, 6, 7, 8, 9, 10, 11};
uint8_t outputState = 0; // logical: bit=1 means channel ON
#define EXIO_ADDR 0x20

void writeOutputs() {
  Wire.beginTransmission(EXIO_ADDR);
  Wire.write(0x01);         // Output Port Register (TCA9554)
  Wire.write(~outputState); // invert for active-LOW driver
  Wire.endTransmission();
}

// ================= DECLARATIONS NEEDED BY setOutput() =================
struct PinConfig {
  char topic[50];
  uint32_t pubIntervalMs; // 0 = on-change only, >0 = periodic heartbeat
};
PinConfig inputConfig[8];
PinConfig outputConfig[8];
char deviceName[30] = "esp32";

WiFiClient tcpClient;
WiFiClientSecure mqttTlsClient;
WebSocketsClient wsClient;
WsClientBridge wsBridge(wsClient);
PubSubClient mqtt(tcpClient);

SemaphoreHandle_t ioMutex = NULL;

void setOutput(int ch, bool state) {
  if (ch < 0 || ch > 7)
    return;
  if (ioMutex)
    xSemaphoreTake(ioMutex, portMAX_DELAY);
  bool cur = (outputState >> ch) & 1;
  if (cur != state) {
    if (state)
      outputState |= (1 << ch);
    else
      outputState &= ~(1 << ch);
    writeOutputs();
    // publish DO state change if topic configured
    if (strlen(outputConfig[ch].topic) > 0 && mqtt.connected())
      mqtt.publish(outputConfig[ch].topic, state ? "1" : "0");
  }
  if (ioMutex)
    xSemaphoreGive(ioMutex);
}

// ================= CONFIG STRUCTS =================
struct MqttConfig {
  char mode[8];
  char proto[8];
  char host[100];
  int port;
  char path[40];
  char user[33];
  char pass[65];
} mqttCfg;

// ================= DO LOGIC RULES =================
// Each rule has:  doIdx (0-7), enabled, conditions[]
// Condition: diIdx(0-7), state(0=OFF,1=ON), op(0=first/IF, 1=AND, 2=OR)
#define MAX_DO_RULES 16
#define MAX_RULE_COND 8

struct RuleCond {
  uint8_t diIdx; // 0-7
  uint8_t state; // 0=OFF, 1=ON
  uint8_t op;    // 0=IF(first), 1=AND, 2=OR
};

struct DoRule {
  uint8_t doIdx;
  bool enabled;
  int condCount;
  RuleCond conds[MAX_RULE_COND];
};

DoRule doRules[MAX_DO_RULES];
int doRuleCount = 0;

// ================= diCurrentlyOn needed by evaluateDoRules() =================
bool diCurrentlyOn[8] = {false};

// Evaluate all enabled rules against current DI states
// Called from the DI scan after any state change
void evaluateDoRules() {
  for (int r = 0; r < doRuleCount; r++) {
    DoRule &rule = doRules[r];
    if (!rule.enabled || rule.condCount == 0)
      continue;
    // Evaluate left-to-right with AND/OR precedence (simple linear evaluation)
    bool result =
        (diCurrentlyOn[rule.conds[0].diIdx] ? 1 : 0) == rule.conds[0].state;
    for (int c = 1; c < rule.condCount; c++) {
      bool cv =
          (diCurrentlyOn[rule.conds[c].diIdx] ? 1 : 0) == rule.conds[c].state;
      if (rule.conds[c].op == 1)
        result = result && cv; // AND
      else
        result = result || cv; // OR
    }
    setOutput(rule.doIdx, result);
  }
}

void loadDoRules() {
  doRuleCount = 0;
  if (!LittleFS.exists(F("/do_rules.json")))
    return;
  File f = LittleFS.open(F("/do_rules.json"), "r");
  if (!f)
    return;
  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, f)) {
    f.close();
    return;
  }
  f.close();
  for (JsonObject obj : doc.as<JsonArray>()) {
    if (doRuleCount >= MAX_DO_RULES)
      break;
    DoRule &rule = doRules[doRuleCount];
    rule.doIdx = obj["doIdx"] | 0;
    rule.enabled = obj["enabled"] | false;
    rule.condCount = 0;
    for (JsonObject c : obj["conds"].as<JsonArray>()) {
      if (rule.condCount >= MAX_RULE_COND)
        break;
      rule.conds[rule.condCount].diIdx = c["diIdx"] | 0;
      rule.conds[rule.condCount].state = c["state"] | 1;
      rule.conds[rule.condCount].op = c["op"] | 0;
      rule.condCount++;
    }
    doRuleCount++;
  }
}

void saveDoRules() {
  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < doRuleCount; i++) {
    DoRule &rule = doRules[i];
    JsonObject obj = arr.createNestedObject();
    obj["doIdx"] = rule.doIdx;
    obj["enabled"] = rule.enabled;
    JsonArray ca = obj.createNestedArray("conds");
    for (int c = 0; c < rule.condCount; c++) {
      JsonObject co = ca.createNestedObject();
      co["diIdx"] = rule.conds[c].diIdx;
      co["state"] = rule.conds[c].state;
      co["op"] = rule.conds[c].op;
    }
  }
  File f = LittleFS.open(F("/do_rules.json"), "w");
  if (!f)
    return;
  serializeJson(doc, f);
  f.close();
}

// ================= MACHINE STATE =================
#define MACHINE_STATE_TOPIC "esp32/machine/state"
uint8_t machineStateIdx = 1;
const char *MACHINE_LABELS[] = {"UNKNOWN", "RUNNING", "STOPPED"};

// ================= METRICS =================
unsigned long triggerCount[8] = {0};
unsigned long totalOnTimeMs[8] = {0};
unsigned long lastOnStartMs[8] = {0};
unsigned long cycleStartMs = 0;
unsigned long lastCycleTimeMs = 0;
bool cycleActive = false;
unsigned long rejectCount = 0;
unsigned long runtimeMs = 0;
unsigned long downtimeMs = 0;
unsigned long lastMetricTickMs = 0;
unsigned long lastMetricAccMs = 0;
unsigned long lastMetricsSaveMs = 0;
float customEffPct = -1.0f;

// ================= DI DEBOUNCE =================
#define DEBOUNCE_MS 50
int lastRawState[8];
unsigned long debounceTimer[8];

// ================= OTA FLAGS =================
bool shouldReboot = false;

// ================= TIMING =================
unsigned long lastDiScanMs = 0;

// ── Per-topic heartbeat timers (DI/DO) ────────────────────────────
unsigned long diLastPubMs[8] = {0};
unsigned long doLastPubMs[8] = {0};

// ================= WEB SERVER / MQTT =================
WebServer server(80);
unsigned long lastMqttRetry = 0;
const unsigned long mqttRetryInterval = 5000;

// ================= SESSION =================
char activeToken[33] = "";

// ================= TELEGRAM =================
struct TelegramConfig {
  char botToken[128];
  char chatId[24];
  bool enabled;
  int rejectThreshold;
} tgConfig;

WiFiClientSecure tgSecureClient;
UniversalTelegramBot tgBot("", tgSecureClient);
bool tgInitialized = false;

QueueHandle_t tgOutQueue = NULL;
TaskHandle_t tgTaskHandle = NULL;
volatile bool tgResetMetricsFlag = false;
const unsigned long TG_POLL_MS = 2500;

// ================= USER ACCOUNTS =================
struct UserAccount {
  char username[30];
  char password[30];
};
UserAccount users[10];
int userCount = 0;

// ===========================================================
//  HELPERS
// ===========================================================
static String fmtMsBot(unsigned long ms) {
  unsigned long s = ms / 1000, m = s / 60, h = m / 60;
  char buf[12];
  snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", h, m % 60, s % 60);
  return String(buf);
}

unsigned long getOnTime(int ch) {
  unsigned long t = totalOnTimeMs[ch];
  if (diCurrentlyOn[ch])
    t += millis() - lastOnStartMs[ch];
  return t;
}

// ===========================================================
//  OTA AUTO UPDATE (GITHUB)
// ===========================================================
TaskHandle_t otaTaskHandle = NULL;
int lastOtaDay = -1;

bool downloadAndFlash(WiFiClientSecure *client, String url, int command) {
  HTTPClient http;
  http.begin(*client, url);
  int httpCode = http.GET();
  bool success = false;
  if (httpCode == HTTP_CODE_OK) {
    int contentLength = http.getSize();
    if (Update.begin(contentLength, command)) {
      WiFiClient *stream = http.getStreamPtr();
      size_t written = Update.writeStream(*stream);
      if (written == (size_t)contentLength && Update.end() &&
          Update.isFinished()) {
        success = true;
      }
    }
  }
  http.end();
  return success;
}

void performGitHubOTA() {
  tgSend(F("*OTA Update Check Started*"));
  Serial.println(F("Checking GitHub for updates..."));

  WiFiClientSecure *client = new WiFiClientSecure;
  if (!client)
    return;
  client->setInsecure();

  HTTPClient http;
  http.begin(*client, "https://raw.githubusercontent.com/imaller43/"
                      "ESP32_GUI_SIB/main/version.json");
  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    http.end();
    delete client;
    return;
  }
  String payload = http.getString();
  http.end();

  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, payload)) {
    delete client;
    return;
  }

  float newFwVer = doc["firmware_version"] | 1.0f;
  float newFsVer = doc["fs_version"] | 1.0f;
  String fwUrl = doc["firmware_url"] | "";
  String fsUrl = doc["fs_url"] | "";

  bool rebootNeeded = false;

  if (newFwVer > FIRMWARE_VERSION && fwUrl.length() > 0) {
    tgSend("*Downloading firmware v" + String(newFwVer) + "...*");
    if (downloadAndFlash(client, fwUrl, U_FLASH)) {
      tgSend(F("Firmware updated successfully!"));
      rebootNeeded = true;
    } else {
      tgSend(F("Firmware update failed."));
    }
  }

  if (newFsVer > FS_VERSION && fsUrl.length() > 0) {
    tgSend("*Downloading filesystem v" + String(newFsVer) + "...*");
    // Note: U_SPIFFS is the command used for both SPIFFS and LittleFS in the
    // Update library
    if (downloadAndFlash(client, fsUrl, U_SPIFFS)) {
      tgSend(F("Filesystem updated successfully!"));
      rebootNeeded = true;
    } else {
      tgSend(F("Filesystem update failed."));
    }
  }

  if (rebootNeeded) {
    tgSend(F("*OTA Update Complete!* Rebooting..."));
    delay(1000);
    ESP.restart();
  } else if (newFwVer <= FIRMWARE_VERSION && newFsVer <= FS_VERSION) {
    Serial.println(F("Already up to date."));
  }

  delete client;
}

void otaTask(void *pv) {
  performGitHubOTA();
  vTaskDelete(NULL);
}

void triggerOTA() {
  if (otaTaskHandle == NULL || eTaskGetState(otaTaskHandle) == eDeleted) {
    xTaskCreatePinnedToCore(otaTask, "OTATask", 8192, NULL, 1, &otaTaskHandle,
                            0);
  }
}

void checkDailyOTA() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 10))
    return;

  // Check between 13:15 and 14:00 (1:15pm - 2:00pm)
  if (timeinfo.tm_hour == 13 && timeinfo.tm_min >= 15 && timeinfo.tm_min < 60) {
    if (lastOtaDay != timeinfo.tm_yday) {
      lastOtaDay = timeinfo.tm_yday;
      triggerOTA();
    }
  }
}

// ===========================================================
//  TELEGRAM
// ===========================================================
void loadTelegramConfig() {
  memset(&tgConfig, 0, sizeof(tgConfig));
  tgConfig.rejectThreshold = 10;
  if (!LittleFS.exists(F("/telegram.json")))
    return;
  File f = LittleFS.open(F("/telegram.json"), "r");
  if (!f)
    return;
  StaticJsonDocument<384> doc;
  if (!deserializeJson(doc, f)) {
    strlcpy(tgConfig.botToken, doc["botToken"] | "", sizeof(tgConfig.botToken));
    strlcpy(tgConfig.chatId, doc["chatId"] | "", sizeof(tgConfig.chatId));
    tgConfig.enabled = doc["enabled"] | false;
    tgConfig.rejectThreshold = doc["rejectThreshold"] | 10;
  }
  f.close();
}
void saveTelegramConfig() {
  StaticJsonDocument<384> doc;
  doc["botToken"] = tgConfig.botToken;
  doc["chatId"] = tgConfig.chatId;
  doc["enabled"] = tgConfig.enabled;
  doc["rejectThreshold"] = tgConfig.rejectThreshold;
  File f = LittleFS.open(F("/telegram.json"), "w");
  if (!f)
    return;
  serializeJson(doc, f);
  f.close();
}
void initTelegramBot() {
  tgInitialized = false;
  if (strlen(tgConfig.botToken) < 10)
    return;
  tgSecureClient.setInsecure();
  tgSecureClient.setTimeout(8);
  tgBot = UniversalTelegramBot(tgConfig.botToken, tgSecureClient);
  tgInitialized = true;
}
void tgSend(const String &msg) {
  if (!tgInitialized || !tgConfig.enabled)
    return;
  if (!strlen(tgConfig.chatId) || tgOutQueue == NULL)
    return;
  char buf[256] = {0};
  msg.substring(0, 254).toCharArray(buf, sizeof(buf));
  xQueueSend(tgOutQueue, buf, pdMS_TO_TICKS(0));
}

String buildStatusMsg() {
  char buf[512];
  int p = 0;
  String ip =
      eth_connected ? ETH.localIP().toString() : WiFi.localIP().toString();
  p += snprintf(buf + p, sizeof(buf) - p,
                "*Status*\nMachine: *%s*\nIP: `%s`\n\n*Inputs:*\n",
                MACHINE_LABELS[machineStateIdx], ip.c_str());
  for (int i = 0; i < 8; i++)
    p += snprintf(buf + p, sizeof(buf) - p, "DI%d: %s\n", i + 1,
                  diCurrentlyOn[i] ? "ON" : "OFF");
  p += snprintf(buf + p, sizeof(buf) - p, "\n*Outputs:*\n");
  for (int i = 0; i < 8; i++)
    p += snprintf(buf + p, sizeof(buf) - p, "DO%d: %s\n", i + 1,
                  ((outputState >> i) & 1) ? "ON" : "OFF");
  return String(buf);
}
String buildMetricsMsg() {
  unsigned long now = millis(), rt = runtimeMs, dt = downtimeMs;
  if (machineStateIdx == 1)
    rt += now - lastMetricTickMs;
  else if (machineStateIdx == 2)
    dt += now - lastMetricTickMs;
  float eff = (rt + dt > 0) ? (float)rt / (float)(rt + dt) * 100.0f : 0;
  char buf[256];
  snprintf(buf, sizeof(buf),
           "*Metrics*\nRuntime: `%s`\nDowntime: `%s`\nCycle: `%s`\nRejects: "
           "`%lu`\nEfficiency: `%.1f%%`",
           fmtMsBot(rt).c_str(), fmtMsBot(dt).c_str(),
           lastCycleTimeMs > 0
               ? (String(lastCycleTimeMs / 1000.0, 2) + " s").c_str()
               : "--",
           rejectCount, eff);
  return String(buf);
}
String buildHealthMsg() {
  unsigned long upS = millis() / 1000, upM = upS / 60, upH = upM / 60,
                upD = upH / 24;
  char up[32], buf[256];
  if (upD > 0)
    snprintf(up, sizeof(up), "%lud %02lu:%02lu:%02lu", upD, upH % 24, upM % 60,
             upS % 60);
  else
    snprintf(up, sizeof(up), "%02lu:%02lu:%02lu", upH, upM % 60, upS % 60);
  snprintf(buf, sizeof(buf),
           "*Health*\nTemp: `%.1f C`\nHeap: `%lu KB`\nNet: `%s`\nCPU: `%u "
           "MHz`\nUp: `%s`",
           temperatureRead(), (unsigned long)(ESP.getFreeHeap() / 1024),
           eth_connected ? "Ethernet" : (String(WiFi.RSSI()) + " dBm").c_str(),
           getCpuFrequencyMhz(), up);
  return String(buf);
}

void handleTelegramMessage(const telegramMessage &m) {
  if (strlen(tgConfig.chatId) > 0 && m.chat_id != String(tgConfig.chatId)) {
    tgBot.sendMessage(m.chat_id, "Unauthorized.", "");
    return;
  }
  String text = m.text;
  text.trim();
  if (text == "/help") {
    tgBot.sendMessage(
        m.chat_id,
        "*ESP32 Bot Commands*\n\n"
        "/status — DI/DO states\n/metrics — Runtime, downtime, rejects\n"
        "/health — Temp, heap, WiFi, uptime\n"
        "/do on N — Turn DO N ON (1-8)\n/do off N — Turn DO N OFF (1-8)\n"
        "/resetmetrics — Reset all counters\n/help — Show this menu",
        "Markdown");
  } else if (text == "/status")
    tgBot.sendMessage(m.chat_id, buildStatusMsg(), "Markdown");
  else if (text == "/metrics")
    tgBot.sendMessage(m.chat_id, buildMetricsMsg(), "Markdown");
  else if (text == "/health")
    tgBot.sendMessage(m.chat_id, buildHealthMsg(), "Markdown");
  else if (text.startsWith("/do ")) {
    String sub = text.substring(4);
    sub.trim();
    bool on = false;
    int ch = -1;
    if (sub.startsWith("on ")) {
      on = true;
      ch = sub.substring(3).toInt() - 1;
    } else if (sub.startsWith("off ")) {
      on = false;
      ch = sub.substring(4).toInt() - 1;
    }
    if (ch >= 0 && ch <= 7) {
      setOutput(ch, on);
      tgBot.sendMessage(
          m.chat_id, "DO" + String(ch + 1) + " turned " + (on ? "ON" : "OFF"),
          "");
    } else
      tgBot.sendMessage(m.chat_id, "Usage: /do on N  or  /do off N  (N=1-8)",
                        "");
  } else if (text == "/resetmetrics") {
    tgResetMetricsFlag = true;
    tgBot.sendMessage(m.chat_id, "Reset requested.", "");
  } else
    tgBot.sendMessage(m.chat_id, "Unknown command. Send /help for the list.",
                      "");
}

void telegramTask(void *pv) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(TG_POLL_MS));
    if (!tgInitialized || !tgConfig.enabled)
      continue;
    if (WiFi.status() != WL_CONNECTED && !eth_connected)
      continue;
    char outMsg[256];
    if (xQueueReceive(tgOutQueue, outMsg, 0) == pdTRUE) {
      tgBot.sendMessage(tgConfig.chatId, outMsg, "Markdown");
      continue;
    }
    int n = tgBot.getUpdates(tgBot.last_message_received + 1);
    for (int i = 0; i < n; i++)
      handleTelegramMessage(tgBot.messages[i]);
  }
}

// ===========================================================
//  MACHINE STATE
// ===========================================================
void setMachineState(uint8_t idx) {
  if (machineStateIdx == idx)
    return;
  machineStateIdx = idx;
  if (idx == 1)
    tgSend(F("Machine: *RUNNING*"));
  else if (idx == 2)
    tgSend(F("Machine: *STOPPED*"));
}
void autoUpdateMachineState(int ch, int state) {
  if (ch == 0 && state == 0) {
    setMachineState(machineStateIdx == 1 ? 2 : 1);
    return;
  }
  if (ch == 1 && state == 1) {
    setMachineState(1);
    return;
  }
  if (ch == 2 && state == 1) {
    setMachineState(2);
    return;
  }
}

// ===========================================================
//  METRICS PERSIST
// ===========================================================
void saveMetrics() {
  StaticJsonDocument<576> doc;
  doc["rejects"] = rejectCount;
  doc["runtime"] = runtimeMs;
  doc["downtime"] = downtimeMs;
  doc["machineState"] = machineStateIdx;
  doc["customEff"] = customEffPct;
  JsonArray tc = doc.createNestedArray("tc"), ot = doc.createNestedArray("ot");
  for (int i = 0; i < 8; i++) {
    tc.add(triggerCount[i]);
    ot.add(totalOnTimeMs[i]);
  }
  File f = LittleFS.open(F("/metrics.json"), "w");
  if (f) {
    serializeJson(doc, f);
    f.close();
  }
}
void loadMetrics() {
  if (!LittleFS.exists(F("/metrics.json")))
    return;
  File f = LittleFS.open(F("/metrics.json"), "r");
  if (!f)
    return;
  StaticJsonDocument<576> doc;
  if (!deserializeJson(doc, f)) {
    rejectCount = doc["rejects"] | (unsigned long)0;
    runtimeMs = doc["runtime"] | (unsigned long)0;
    downtimeMs = doc["downtime"] | (unsigned long)0;
    machineStateIdx = doc["machineState"] | (uint8_t)1;
    customEffPct = doc["customEff"] | -1.0f;
    for (int i = 0; i < 8; i++) {
      triggerCount[i] = doc["tc"][i] | (unsigned long)0;
      totalOnTimeMs[i] = doc["ot"][i] | (unsigned long)0;
    }
  }
  f.close();
}

// ===========================================================
//  CONFIG LOAD / SAVE
// ===========================================================
void loadConfig() {
  // Defaults
  strlcpy(mqttCfg.mode, "Not Set", sizeof(mqttCfg.mode));
  strlcpy(mqttCfg.proto, "Not Set", sizeof(mqttCfg.proto));
  strlcpy(
      mqttCfg.host,
      "1b88e7df-d8af-4d46-ac2b-43963d26bdf2-00-if445lcva84n.pike.replit.dev",
      sizeof(mqttCfg.host));
  mqttCfg.port = 0;
  strlcpy(mqttCfg.path, "/mqtt", sizeof(mqttCfg.path));
  mqttCfg.user[0] = '\0';
  mqttCfg.pass[0] = '\0';
  for (int i = 0; i < 8; i++) {
    inputConfig[i].pubIntervalMs = 0;
    outputConfig[i].pubIntervalMs = 0;
  }

  if (!LittleFS.exists(F("/config.json")))
    return;
  File f = LittleFS.open(F("/config.json"), "r");
  if (!f)
    return;
  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, f)) {
    f.close();
    return;
  }
  f.close();
  strlcpy(deviceName, doc["deviceName"] | "esp32", sizeof(deviceName));
  strlcpy(mqttCfg.mode, doc["mqttMode"] | "Not Set", sizeof(mqttCfg.mode));
  strlcpy(mqttCfg.proto, doc["mqttProto"] | "Not Set", sizeof(mqttCfg.proto));
  strlcpy(mqttCfg.host, doc["mqttHost"] | "", sizeof(mqttCfg.host));
  mqttCfg.port = doc["mqttPort"] | 0;
  strlcpy(mqttCfg.path, doc["mqttPath"] | "/mqtt", sizeof(mqttCfg.path));
  strlcpy(mqttCfg.user, doc["mqttUser"] | "", sizeof(mqttCfg.user));
  strlcpy(mqttCfg.pass, doc["mqttPass"] | "", sizeof(mqttCfg.pass));

  for (int i = 0; i < 8; i++) {
    strlcpy(inputConfig[i].topic, doc["inputs"][i] | "", 50);
    strlcpy(outputConfig[i].topic, doc["outputs"][i] | "", 50);
    inputConfig[i].pubIntervalMs = doc["inputPubMs"][i] | (uint32_t)0;
    outputConfig[i].pubIntervalMs = doc["outputPubMs"][i] | (uint32_t)0;
  }

  // Load users embedded in config.json (fallback if users.json absent)
  if (doc.containsKey("users")) {
    userCount = 0;
    for (JsonObject u : doc["users"].as<JsonArray>()) {
      if (userCount >= 10)
        break;
      strlcpy(users[userCount].username, u["username"] | "", 30);
      strlcpy(users[userCount].password, u["password"] | "", 30);
      userCount++;
    }
  }
}

void saveConfig() {
  DynamicJsonDocument doc(3072);
  doc["deviceName"] = deviceName;
  doc["mqttMode"] = mqttCfg.mode;
  doc["mqttProto"] = mqttCfg.proto;
  doc["mqttHost"] = mqttCfg.host;
  doc["mqttPort"] = mqttCfg.port;
  doc["mqttPath"] = mqttCfg.path;
  doc["mqttUser"] = mqttCfg.user;
  doc["mqttPass"] = mqttCfg.pass;
  for (int i = 0; i < 8; i++) {
    doc["inputs"][i] = inputConfig[i].topic;
    doc["outputs"][i] = outputConfig[i].topic;
    doc["inputPubMs"][i] = inputConfig[i].pubIntervalMs;
    doc["outputPubMs"][i] = outputConfig[i].pubIntervalMs;
  }

  File f = LittleFS.open(F("/config.json"), "w");
  if (!f)
    return;
  serializeJson(doc, f);
  f.close();
}

// ===========================================================
//  USER MANAGEMENT + AUTH
// ===========================================================
void loadUsers() {
  userCount = 0;
  if (!LittleFS.exists(F("/users.json")))
    return;
  File f = LittleFS.open(F("/users.json"), "r");
  if (!f)
    return;
  StaticJsonDocument<768> doc;
  if (deserializeJson(doc, f)) {
    f.close();
    return;
  }
  f.close();
  for (JsonObject u : doc.as<JsonArray>()) {
    if (userCount >= 10)
      break;
    strlcpy(users[userCount].username, u["username"] | "", 30);
    strlcpy(users[userCount].password, u["password"] | "", 30);
    userCount++;
  }
}
bool checkCredentials(const String &u, const String &p) {
  for (int i = 0; i < userCount; i++)
    if (strcmp(users[i].username, u.c_str()) == 0 &&
        strcmp(users[i].password, p.c_str()) == 0)
      return true;
  return false;
}
void generateToken(char *buf) {
  randomSeed(micros() ^ millis());
  for (int i = 0; i < 32; i++) {
    int r = random(16);
    buf[i] = r < 10 ? '0' + r : 'a' + r - 10;
  }
  buf[32] = '\0';
}
bool isAuthenticated() {
  if (activeToken[0] == '\0')
    return false;
  String c = server.header("Cookie");
  int idx = c.indexOf(F("session="));
  if (idx == -1)
    return false;
  String v = c.substring(idx + 8);
  int e = v.indexOf(';');
  if (e != -1)
    v = v.substring(0, e);
  v.trim();
  return strcmp(activeToken, v.c_str()) == 0;
}
bool requireAuth() {
  if (!isAuthenticated()) {
    server.sendHeader(F("Location"), F("/login"));
    server.send(302);
    return false;
  }
  return true;
}
void serveStatic(const char *path, const char *mime, bool noCache = false) {
  File f = LittleFS.open(path, "r");
  if (!f) {
    server.send(404, F("text/plain"), F("Not Found"));
    return;
  }
  if (!noCache)
    server.sendHeader(F("Cache-Control"), F("public, max-age=86400"));
  server.streamFile(f, mime);
  f.close();
}

// ===========================================================
//  MQTT TRANSPORT
// ===========================================================
void applyMqttTransport() {
  mqtt.disconnect();
  wsBridge.setConnected(false);
  wsClient.disconnect();
  if (mqttCfg.host[0] == '\0')
    return; // no broker configured yet
  if (strcmp(mqttCfg.mode, "cloud") == 0) {
    mqtt.setClient(wsBridge);
    if (strcmp(mqttCfg.proto, "wss") == 0)
      wsClient.beginSSL(mqttCfg.host, mqttCfg.port, mqttCfg.path, "", "mqtt");
    else
      wsClient.begin(mqttCfg.host, mqttCfg.port, mqttCfg.path, "mqtt");
    wsClient.onEvent(wsEventHandler);
    wsClient.setReconnectInterval(5000);
  } else {
    if (strcmp(mqttCfg.proto, "ssl") == 0) {
      mqttTlsClient.setInsecure();
      mqtt.setClient(mqttTlsClient);
    } else
      mqtt.setClient(tcpClient);
  }
  mqtt.setServer(mqttCfg.host, mqttCfg.port);
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(15);
  lastMqttRetry = 0;
}
void wsEventHandler(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
  case WStype_CONNECTED:
    wsBridge.setConnected(true);
    break;
  case WStype_DISCONNECTED:
    wsBridge.setConnected(false);
    break;
  case WStype_BIN:
    wsBridge.push(payload, length);
    break;
  default:
    break;
  }
}
void mqttCallback(char *topic, byte *payload, unsigned int length) {
  if (strcmp(topic, MACHINE_STATE_TOPIC) == 0) {
    char msg[12] = {0};
    memcpy(msg, payload, min(length, (unsigned int)10));
    if (strcmp(msg, "RUNNING") == 0)
      setMachineState(1);
    else if (strcmp(msg, "STOPPED") == 0)
      setMachineState(2);
    return;
  }
  if (strcmp(topic, "esp32/sd/cmd") == 0) {
    StaticJsonDocument<128> cmd;
    if (!deserializeJson(cmd, payload, length)) {
      const char *c = cmd["cmd"] | "";
    }
    return;
  }
  if (strcmp(topic, "esp32/triggercount/set") == 0 ||
      strcmp(topic, "esp32/triggercount/reset") == 0) {
    StaticJsonDocument<128> doc;
    if (!deserializeJson(doc, payload, length)) {
      if (doc.containsKey("sensor")) {
        int sensor = doc["sensor"];
        if (sensor >= 0 && sensor < 8) {
          if (strcmp(topic, "esp32/triggercount/set") == 0) {
            triggerCount[sensor] = doc["count"] | 0UL;
          } else {
            triggerCount[sensor] = 0;
          }
          if (sensor == 6) {
            rejectCount = triggerCount[sensor];
          }
        }
      }
    }
    return;
  }
  if (strcmp(topic, "esp32/ota_trigger") == 0) {
    triggerOTA();
    return;
  }

  char msg[4] = {0};
  memcpy(msg, payload, min(length, (unsigned int)3));
  for (int i = 0; i < 8; i++)
    if (strlen(outputConfig[i].topic) > 0 &&
        strcmp(topic, outputConfig[i].topic) == 0) {
      setOutput(i, strcmp(msg, "1") == 0);
      break;
    }
}
void reconnectMQTT() {
  if (mqttCfg.host[0] == '\0')
    return; // no broker configured — skip silently
  if (strcmp(mqttCfg.mode, "cloud") == 0 && !wsBridge.connected())
    return;

  bool connected = false;
  if (strlen(mqttCfg.user) > 0) {
    connected = mqtt.connect(deviceName, mqttCfg.user, mqttCfg.pass);
  } else {
    connected = mqtt.connect(deviceName);
  }

  if (connected) {
    for (int i = 0; i < 8; i++)
      if (strlen(outputConfig[i].topic) > 0)
        mqtt.subscribe(outputConfig[i].topic);
    mqtt.subscribe(MACHINE_STATE_TOPIC);
    mqtt.subscribe("esp32/sd/cmd");
    mqtt.subscribe("esp32/triggercount/set");
    mqtt.subscribe("esp32/triggercount/reset");
    mqtt.subscribe("esp32/ota_trigger");
  }
}

// ===========================================================
//  HTTP LOGIN / LOGOUT
// ===========================================================
void handleLogin() {
  if (server.method() == HTTP_GET) {
    serveStatic("/login.html", "text/html", true);
  } else {
    if (checkCredentials(server.arg("username"), server.arg("password"))) {
      generateToken(activeToken);
      server.sendHeader(F("Set-Cookie"),
                        String(F("session=")) + activeToken + F("; Path=/"));
      server.sendHeader(F("Location"), F("/"));
      server.send(302);
    } else {
      server.sendHeader(F("Location"), F("/login?error=1"));
      server.send(302);
    }
  }
}
void handleLogout() {
  activeToken[0] = '\0';
  server.sendHeader(F("Set-Cookie"), F("session=; Path=/; Max-Age=0"));
  server.sendHeader(F("Location"), F("/login"));
  server.send(302);
}

// ===========================================================
//  API HANDLERS
// ===========================================================
void handleToggle() {
  if (!requireAuth())
    return;
  int ch = server.arg("ch").toInt();
  if (ch < 0 || ch > 7) {
    server.send(400, F("text/plain"), F("Invalid channel"));
    return;
  }
  bool ns = !((outputState >> ch) & 1);
  setOutput(ch, ns);
  server.send(200, F("text/plain"), ns ? "1" : "0");
}

void handleStatus() {
  if (!requireAuth())
    return;
  char buf[280];
  int pos = 0;
  pos += snprintf(buf + pos, sizeof(buf) - pos, "{\"inputs\":[");
  for (int i = 0; i < 8; i++) {
    buf[pos++] = diCurrentlyOn[i] ? '1' : '0';
    if (i < 7)
      buf[pos++] = ',';
  }
  pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"outputs\":[");
  for (int i = 0; i < 8; i++) {
    buf[pos++] = (outputState & (1 << i)) ? '1' : '0';
    if (i < 7)
      buf[pos++] = ',';
  }
  String ip =
      eth_connected ? ETH.localIP().toString() : WiFi.localIP().toString();
  snprintf(buf + pos, sizeof(buf) - pos,
           "],\"ip\":\"%s\",\"network\":\"%s\",\"machineState\":\"%s\"}",
           ip.c_str(), eth_connected ? "Ethernet" : "WiFi",
           MACHINE_LABELS[machineStateIdx]);
  server.sendHeader(F("Cache-Control"), F("no-cache"));
  server.send(200, F("application/json"), buf);
}

void handleGetMetrics() {
  if (!requireAuth())
    return;
  unsigned long now = millis(), rt = runtimeMs, dt = downtimeMs;
  if (machineStateIdx == 1)
    rt += now - lastMetricTickMs;
  else if (machineStateIdx == 2)
    dt += now - lastMetricTickMs;
  StaticJsonDocument<640> doc;
  JsonArray trig = doc.createNestedArray("triggers"),
            ont = doc.createNestedArray("onTimes");
  for (int i = 0; i < 8; i++) {
    trig.add(triggerCount[i]);
    ont.add(getOnTime(i));
  }
  doc["cycleTime"] = lastCycleTimeMs;
  doc["rejects"] = rejectCount;
  doc["runtime"] = rt;
  doc["downtime"] = dt;
  String out;
  serializeJson(doc, out);
  server.sendHeader(F("Cache-Control"), F("no-cache"));
  server.send(200, F("application/json"), out);
}

void handleResetMetrics() {
  if (!requireAuth())
    return;
  String target = server.arg("target");
  unsigned long now = millis();
  if (target == "all") {
    for (int i = 0; i < 8; i++) {
      triggerCount[i] = 0;
      totalOnTimeMs[i] = 0;
      if (diCurrentlyOn[i])
        lastOnStartMs[i] = now;
    }
    rejectCount = 0;
    runtimeMs = 0;
    downtimeMs = 0;
    lastCycleTimeMs = 0;
    lastMetricTickMs = now;
  } else if (target.startsWith("di")) {
    int ch = target.substring(2).toInt();
    if (ch >= 0 && ch < 8) {
      triggerCount[ch] = 0;
      totalOnTimeMs[ch] = 0;
      if (diCurrentlyOn[ch])
        lastOnStartMs[ch] = now;
      if (ch == 6)
        rejectCount = 0;
    }
  } else if (target == "rejects")
    rejectCount = 0;
  else if (target == "runtime") {
    runtimeMs = 0;
    downtimeMs = 0;
    lastMetricTickMs = now;
  } else if (target == "cycle")
    lastCycleTimeMs = 0;
  saveMetrics();
  server.send(200, F("text/plain"), F("OK"));
}

void handleGetConfig() {
  if (!requireAuth())
    return;
  StaticJsonDocument<1024> doc;
  doc["deviceName"] = deviceName;
  doc["mqttMode"] = mqttCfg.mode;
  doc["mqttProto"] = mqttCfg.proto;
  doc["mqttHost"] = mqttCfg.host;
  doc["mqttPort"] = mqttCfg.port;
  doc["mqttPath"] = mqttCfg.path;
  doc["mqttUser"] = mqttCfg.user;
  doc["mqttPass"] = mqttCfg.pass;
  doc["mqttConnected"] = mqtt.connected();
  for (int i = 0; i < 8; i++) {
    doc["inputs"][i] = inputConfig[i].topic;
    doc["outputs"][i] = outputConfig[i].topic;
    doc["inputPubMs"][i] = inputConfig[i].pubIntervalMs;
    doc["outputPubMs"][i] = outputConfig[i].pubIntervalMs;
  }
  String out;
  serializeJson(doc, out);
  server.send(200, F("application/json"), out);
}

void handleGetHealth() {
  if (!requireAuth())
    return;
  StaticJsonDocument<192> doc;
  doc["temp"] = temperatureRead();
  doc["freeHeap"] = (uint32_t)ESP.getFreeHeap();
  doc["rssi"] = eth_connected ? 0 : (int)WiFi.RSSI();
  doc["cpuFreq"] = getCpuFrequencyMhz();
  doc["uptime"] = millis();
  doc["fw_ver"] = FIRMWARE_VERSION;
  doc["fs_ver"] = FS_VERSION;
  String out;
  serializeJson(doc, out);
  server.sendHeader(F("Cache-Control"), F("no-cache"));
  server.send(200, F("application/json"), out);
}

void handleSave() {
  if (!requireAuth())
    return;
  String mode = server.arg("mqttMode"), proto = server.arg("mqttProto"),
         host = server.arg("mqttHost"), port = server.arg("mqttPort"),
         path = server.arg("mqttPath"), user = server.arg("mqttUser"),
         pass = server.arg("mqttPass");
  bool mqttChanged = false;
  if (mode.length() > 0) {
    mqttChanged =
        (strcmp(mode.c_str(), mqttCfg.mode) != 0 ||
         strcmp(proto.c_str(), mqttCfg.proto) != 0 ||
         (host.length() > 0 && strcmp(host.c_str(), mqttCfg.host) != 0) ||
         (port.length() > 0 && port.toInt() != mqttCfg.port));
    strlcpy(mqttCfg.mode, mode.c_str(), sizeof(mqttCfg.mode));
    strlcpy(mqttCfg.proto, proto.c_str(), sizeof(mqttCfg.proto));
    if (host.length() > 0)
      strlcpy(mqttCfg.host, host.c_str(), sizeof(mqttCfg.host));
    if (port.length() > 0)
      mqttCfg.port = port.toInt();
    if (path.length() > 0)
      strlcpy(mqttCfg.path, path.c_str(), sizeof(mqttCfg.path));
    else if (strcmp(mqttCfg.mode, "local") == 0)
      mqttCfg.path[0] = '\0';

    // Always overwrite user/pass because they might have been cleared
    strlcpy(mqttCfg.user, user.c_str(), sizeof(mqttCfg.user));
    strlcpy(mqttCfg.pass, pass.c_str(), sizeof(mqttCfg.pass));
    mqttChanged =
        true; // force reconnect if they changed, though we'll just set it
  }
  for (int i = 0; i < 8; i++) {
    strlcpy(inputConfig[i].topic, server.arg("in" + String(i)).c_str(), 50);
    strlcpy(outputConfig[i].topic, server.arg("out" + String(i)).c_str(), 50);
    String inPubMs = server.arg("inPubMs" + String(i));
    String outPubMs = server.arg("outPubMs" + String(i));
    if (inPubMs.length() > 0)
      inputConfig[i].pubIntervalMs =
          (uint32_t)constrain(inPubMs.toInt(), 0, 60000);
    if (outPubMs.length() > 0)
      outputConfig[i].pubIntervalMs =
          (uint32_t)constrain(outPubMs.toInt(), 0, 60000);
  }
  saveConfig();
  if (mqttChanged)
    applyMqttTransport();
  server.send(200, F("text/plain"), F("OK"));
}

// ── DO Rules API ──────────────────────────────────────────────────
void handleGetDoRules() {
  if (!requireAuth())
    return;
  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < doRuleCount; i++) {
    DoRule &rule = doRules[i];
    JsonObject obj = arr.createNestedObject();
    obj["doIdx"] = rule.doIdx;
    obj["enabled"] = rule.enabled;
    JsonArray ca = obj.createNestedArray("conds");
    for (int c = 0; c < rule.condCount; c++) {
      JsonObject co = ca.createNestedObject();
      co["diIdx"] = rule.conds[c].diIdx;
      co["state"] = rule.conds[c].state;
      co["op"] = rule.conds[c].op;
    }
  }
  String out;
  serializeJson(doc, out);
  server.sendHeader(F("Cache-Control"), F("no-cache"));
  server.send(200, F("application/json"), out);
}

void handleSaveDoRules() {
  if (!requireAuth())
    return;
  String body = server.arg("plain");
  if (!body.length()) {
    server.send(400, F("text/plain"), F("Empty body"));
    return;
  }
  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, body)) {
    server.send(400, F("text/plain"), F("JSON error"));
    return;
  }
  doRuleCount = 0;
  for (JsonObject obj : doc.as<JsonArray>()) {
    if (doRuleCount >= MAX_DO_RULES)
      break;
    DoRule &rule = doRules[doRuleCount];
    rule.doIdx = obj["doIdx"] | 0;
    rule.enabled = obj["enabled"] | false;
    rule.condCount = 0;
    for (JsonObject c : obj["conds"].as<JsonArray>()) {
      if (rule.condCount >= MAX_RULE_COND)
        break;
      rule.conds[rule.condCount].diIdx = c["diIdx"] | 0;
      rule.conds[rule.condCount].state = c["state"] | 1;
      rule.conds[rule.condCount].op = c["op"] | 0;
      rule.condCount++;
    }
    doRuleCount++;
  }
  saveDoRules();
  server.send(200, F("text/plain"), F("OK"));
}

void handleSetCustomEff() {
  if (!requireAuth())
    return;
  String v = server.arg("eff");
  customEffPct = v.length() > 0 ? v.toFloat() : -1.0f;
  server.send(200, F("text/plain"), F("OK"));
}

// ── Telegram API ──────────────────────────────────────────────────
void handleGetTelegramConfig() {
  if (!requireAuth())
    return;
  StaticJsonDocument<256> doc;
  bool ht = strlen(tgConfig.botToken) > 8;
  doc["tokenMasked"] =
      ht ? String(tgConfig.botToken).substring(0, 8) + "........" : "";
  doc["hasToken"] = ht;
  doc["chatId"] = tgConfig.chatId;
  doc["enabled"] = tgConfig.enabled;
  doc["rejectThreshold"] = tgConfig.rejectThreshold;
  doc["botReady"] = tgInitialized;
  String out;
  serializeJson(doc, out);
  server.send(200, F("application/json"), out);
}
void handleSaveTelegramConfig() {
  if (!requireAuth())
    return;
  String token = server.arg("botToken"), chatId = server.arg("chatId"),
         en = server.arg("enabled"), thresh = server.arg("rejectThreshold");
  if (token.length() > 8 && !token.endsWith("........"))
    strlcpy(tgConfig.botToken, token.c_str(), sizeof(tgConfig.botToken));
  if (chatId.length() > 0)
    strlcpy(tgConfig.chatId, chatId.c_str(), sizeof(tgConfig.chatId));
  tgConfig.enabled = (en == "1" || en == "true");
  if (thresh.length() > 0)
    tgConfig.rejectThreshold = constrain(thresh.toInt(), 0, 9999);
  saveTelegramConfig();
  initTelegramBot();
  server.send(200, F("text/plain"), F("OK"));
}
void handleTelegramTest() {
  if (!requireAuth())
    return;
  if (!tgInitialized || !tgConfig.enabled || !strlen(tgConfig.chatId)) {
    server.send(400, F("text/plain"), F("Bot not configured or disabled"));
    return;
  }

  saveConfig();
  saveTelegramConfig();
  saveMetrics();
  tgSend(F("*Test OK!* All configs saved. Bot is live."));
  server.send(200, F("text/plain"), F("Queued — check Telegram in ~3 seconds"));
}

//  NETWORK EVENT
// ===========================================================
void WiFiEvent(WiFiEvent_t event) {
  if (event == ARDUINO_EVENT_ETH_GOT_IP) {
    eth_connected = true;
    Serial.print(F("ETH IP: "));
    Serial.println(ETH.localIP());
  }
}

// ===========================================================
//  SETUP
// ===========================================================
void setup() {
  Serial.begin(115200);
  Serial.println(F("Booting..."));

  for (int i = 0; i < 8; i++) {
    pinMode(inputPins[i], INPUT_PULLUP);
    lastRawState[i] = HIGH;
    debounceTimer[i] = 0;
  }

  Wire.begin(42, 41);

  // Configure TCA9554 all pins as outputs
  Wire.beginTransmission(EXIO_ADDR);
  Wire.write(0x03); // Configuration Register
  Wire.write(0x00); // 0 = Output, 1 = Input (All Outputs)
  Wire.endTransmission();

  writeOutputs(); // initialise all DO OFF

  if (!LittleFS.begin(true))
    Serial.println(F("LittleFS failed!"));
  else
    Serial.println(F("LittleFS OK"));

  loadConfig();
  loadUsers();
  loadTelegramConfig();
  loadMetrics();

  loadDoRules();

  // Network
  WiFi.onEvent(WiFiEvent);
  ETH.begin();
  WiFi.begin(ssid, password);
  WiFi.setSleep(false);
  unsigned long cs = millis();
  while (!eth_connected && WiFi.status() != WL_CONNECTED) {
    if (millis() - cs > 15000)
      break;
    Serial.print(F("."));
    delay(200);
  }
  Serial.println();
  if (eth_connected) {
    Serial.print(F("ETH IP: "));
    Serial.println(ETH.localIP());
  } else if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("WiFi IP: "));
    Serial.println(WiFi.localIP());
  } else
    Serial.println(F("No network"));

  lastMetricTickMs = millis();
  lastMetricAccMs = millis();
  lastMetricsSaveMs = millis();

  configTime(8 * 3600, 0, "pool.ntp.org", "time.cloudflare.com");
  struct tm ntpCheck;
  for (int i = 0; i < 5; i++)
    if (getLocalTime(&ntpCheck, 1000))
      break;

  applyMqttTransport();

  // FreeRTOS primitives
  ioMutex = xSemaphoreCreateMutex();

  tgOutQueue = xQueueCreate(6, 256);
  initTelegramBot();
  xTaskCreatePinnedToCore(telegramTask, "TGTask", 12288, NULL, 1, &tgTaskHandle,
                          0);

  // HTTP routes
  const char *headerKeys[] = {"Cookie"};
  server.collectHeaders(headerKeys, 1);

  server.on("/login", handleLogin);
  server.on("/logout", handleLogout);

  // Static assets
  server.on("/style.css", HTTP_GET,
            []() { serveStatic("/style.css", "text/css"); });
  server.on("/SIB_Favicon.png", HTTP_GET,
            []() { serveStatic("/SIB_Favicon.png", "image/png"); });
  server.on("/shrdc_logo.png", HTTP_GET,
            []() { serveStatic("/shrdc_logo.png", "image/png"); });
  server.on("/SIB1_logo.png", HTTP_GET,
            []() { serveStatic("/SIB1_logo.png", "image/png"); });
  server.on("/SIB_logo.png", HTTP_GET,
            []() { serveStatic("/SIB_logo.png", "image/png"); });
  server.on("/variables.json", HTTP_GET, []() {
    if (!requireAuth())
      return;
    serveStatic("/variables.json", "application/json");
  });

  // Pages
  server.on("/", HTTP_GET, []() {
    if (!requireAuth())
      return;
    serveStatic("/index.html", "text/html", true);
  });
  server.on("/iot", HTTP_GET, []() {
    if (!requireAuth())
      return;
    serveStatic("/iot.html", "text/html", true);
  });

  server.on("/telegram", HTTP_GET, []() {
    if (!requireAuth())
      return;
    serveStatic("/telegram.html", "text/html", true);
  });

  // Legacy redirects
  server.on("/mqtt", HTTP_GET, []() {
    server.sendHeader("Location", "/iot");
    server.send(301);
  });

  server.on("/monitoring", HTTP_GET, []() {
    server.sendHeader("Location", "/");
    server.send(301);
  });

  // Updates
  server.on("/update", HTTP_GET, []() {
    if (!requireAuth())
      return;
    serveStatic("/update.html", "text/html", true);
  });

  server.on(
      "/update/firmware", HTTP_POST,
      []() {
        if (!isAuthenticated()) {
          server.send(401, "text/plain", "Unauthorized");
          return;
        }
        server.sendHeader("Connection", "close");
        if (Update.hasError()) {
          server.send(500, "text/plain", "FAIL");
        } else {
          server.send(200, "text/plain", "OK");
          shouldReboot = true;
        }
      },
      []() {
        if (!isAuthenticated())
          return;
        HTTPUpload &upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
          Serial.setDebugOutput(true);
          Serial.printf("Update FW: %s\n", upload.filename.c_str());
          if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
            Update.printError(Serial);
          }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
          if (Update.write(upload.buf, upload.currentSize) !=
              upload.currentSize) {
            Update.printError(Serial);
          }
        } else if (upload.status == UPLOAD_FILE_END) {
          if (Update.end(true)) {
            Serial.printf("Update FW Success: %u\n", upload.totalSize);
          } else {
            Update.printError(Serial);
          }
          Serial.setDebugOutput(false);
        }
      });

  server.on(
      "/update/filesystem", HTTP_POST,
      []() {
        if (!isAuthenticated()) {
          server.send(401, "text/plain", "Unauthorized");
          return;
        }
        server.sendHeader("Connection", "close");
        if (Update.hasError()) {
          server.send(500, "text/plain", "FAIL");
        } else {
          server.send(200, "text/plain", "OK");
          shouldReboot = true;
        }
      },
      []() {
        if (!isAuthenticated())
          return;
        HTTPUpload &upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
          Serial.setDebugOutput(true);
          Serial.printf("Update FS: %s\n", upload.filename.c_str());
          if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS)) {
            Update.printError(Serial);
          }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
          if (Update.write(upload.buf, upload.currentSize) !=
              upload.currentSize) {
            Update.printError(Serial);
          }
        } else if (upload.status == UPLOAD_FILE_END) {
          if (Update.end(true)) {
            Serial.printf("Update FS Success: %u\n", upload.totalSize);
          } else {
            Update.printError(Serial);
          }
          Serial.setDebugOutput(false);
        }
      });

  // API
  server.on("/toggle", handleToggle);
  server.on("/status", handleStatus);
  server.on("/getConfig", handleGetConfig);
  server.on("/getMetrics", handleGetMetrics);
  server.on("/getHealth", handleGetHealth);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/resetMetrics", HTTP_POST, handleResetMetrics);
  server.on("/getDoRules", HTTP_GET, handleGetDoRules);
  server.on("/saveDoRules", HTTP_POST, handleSaveDoRules);
  server.on("/setCustomEff", HTTP_POST, handleSetCustomEff);

  server.on("/getTelegramConfig", HTTP_GET, handleGetTelegramConfig);
  server.on("/saveTelegramConfig", HTTP_POST, handleSaveTelegramConfig);
  server.on("/telegramTest", HTTP_POST, handleTelegramTest);

  ArduinoOTA.setHostname("ESP32_GUI");
  ArduinoOTA.begin();

  server.begin();
  Serial.println(F("HTTP server started"));
  String ip =
      eth_connected ? ETH.localIP().toString() : WiFi.localIP().toString();
  tgSend("*ESP32 Online*\nIP: `" + ip + "`");
}

// ===========================================================
//  LOOP
// ===========================================================
void loop() {
  if (shouldReboot) {
    delay(500);
    ESP.restart();
  }

  // Auto Update check once a minute
  static unsigned long lastOtaCheckMs = 0;
  if (millis() - lastOtaCheckMs >= 60000) {
    lastOtaCheckMs = millis();
    checkDailyOTA();
  }

  server.handleClient();
  ArduinoOTA.handle();
  if (strcmp(mqttCfg.mode, "cloud") == 0)
    wsClient.loop();
  mqtt.loop();

  if (!mqtt.connected()) {
    unsigned long now = millis();
    if (now - lastMqttRetry > mqttRetryInterval) {
      lastMqttRetry = now;
      reconnectMQTT();
    }
  }

  unsigned long now = millis();

  // Telegram reset flag
  if (tgResetMetricsFlag) {
    tgResetMetricsFlag = false;
    for (int i = 0; i < 8; i++) {
      triggerCount[i] = 0;
      totalOnTimeMs[i] = 0;
      if (diCurrentlyOn[i])
        lastOnStartMs[i] = now;
    }
    rejectCount = 0;
    runtimeMs = 0;
    downtimeMs = 0;
    lastCycleTimeMs = 0;
    lastMetricTickMs = now;
    saveMetrics();
  }

  // Runtime/Downtime accumulation (100 ms tick)
  if (now - lastMetricAccMs >= 100) {
    unsigned long elapsed = now - lastMetricTickMs;
    if (machineStateIdx == 1)
      runtimeMs += elapsed;
    else if (machineStateIdx == 2)
      downtimeMs += elapsed;
    lastMetricTickMs = now;
    lastMetricAccMs = now;
  }

  // Periodic metrics save every 5 min
  if (now - lastMetricsSaveMs >= 300000UL) {
    lastMetricsSaveMs = now;
    saveMetrics();
  }

  // Per-topic periodic MQTT heartbeat for DI
  if (mqtt.connected()) {
    for (int i = 0; i < 8; i++) {
      if (inputConfig[i].pubIntervalMs > 0 &&
          strlen(inputConfig[i].topic) > 0) {
        if (now - diLastPubMs[i] >= inputConfig[i].pubIntervalMs) {
          diLastPubMs[i] = now;
          mqtt.publish(inputConfig[i].topic, diCurrentlyOn[i] ? "1" : "0");
        }
      }
      if (outputConfig[i].pubIntervalMs > 0 &&
          strlen(outputConfig[i].topic) > 0) {
        if (now - doLastPubMs[i] >= outputConfig[i].pubIntervalMs) {
          doLastPubMs[i] = now;
          mqtt.publish(outputConfig[i].topic,
                       ((outputState >> i) & 1) ? "1" : "0");
        }
      }
    }
  }

  // DI scan every 5 ms
  if (now - lastDiScanMs < 5)
    return;
  lastDiScanMs = now;
  bool anyDiChanged = false;

  for (int i = 0; i < 8; i++) {
    int raw = digitalRead(inputPins[i]);
    if (raw != lastRawState[i]) {
      lastRawState[i] = raw;
      debounceTimer[i] = now;
    }
    if ((now - debounceTimer[i]) >= DEBOUNCE_MS) {
      // Active-LOW opto: LOW = input ON, HIGH = input OFF
      int logical = (raw == LOW) ? 1 : 0;
      if (logical != (int)diCurrentlyOn[i]) {
        diCurrentlyOn[i] = (logical == 1);
        anyDiChanged = true;
        if (logical == 1) {
          static const int diMultipliers[8] = {
              1, 1, 1, 1, 1, 1, 1, 1}; // modify the counter multiplier
          triggerCount[i] += diMultipliers[i];
          lastOnStartMs[i] = now;
          if (i == 3) {
            cycleStartMs = now;
            cycleActive = true;
          }
          if (i == 5 && cycleActive) {
            lastCycleTimeMs = now - cycleStartMs;
            cycleActive = false;
          }
          if (i == 6) {
            rejectCount += 1;
            if (tgConfig.rejectThreshold > 0 &&
                (rejectCount % (unsigned long)tgConfig.rejectThreshold == 0))
              tgSend("*Reject Alert!* Total: *" + String(rejectCount) + "*");
          }
        } else {
          totalOnTimeMs[i] += now - lastOnStartMs[i];
        }
        autoUpdateMachineState(i, logical);
        // On-change MQTT publish for DI
        if (strlen(inputConfig[i].topic) > 0 && mqtt.connected())
          mqtt.publish(inputConfig[i].topic, logical ? "1" : "0");
      }
    }
  }

  // Re-evaluate all DO logic rules whenever any DI changed
  if (anyDiChanged)
    evaluateDoRules();
}