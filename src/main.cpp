// vu2cpl-as3935-bridge — ESP32 firmware
//
// MQTT contract is wire-compatible with as3935_mqtt.py from
// vu2cpl-shack repo (status/hb/event keys preserved). v0.2.0 adds:
//   - cmd subscribe topic (lightning/as3935/cmd) for live tuning
//   - all AS3935 tunables exposed + persisted in NVS
//   - on-device TUN_CAP calibration (port of as3935_tune.py)
//   - WiFi modem sleep (WIFI_PS_MAX_MODEM by default)
//   - bounded MQTT reconnect + no-publish watchdog → ESP.restart()
//
// Any divergence from the Python daemon's status/hb shape breaks the
// Node-RED Lightning Antenna Protector flow — extend, don't rename.

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <esp_log.h>
#include <time.h>

// ── Broker / portal ──────────────────────────────────────────────────────
constexpr const char* MQTT_HOST = "192.168.1.169";
constexpr uint16_t    MQTT_PORT = 1883;

constexpr const char* WIFI_AP_SSID = "vu2cpl-as3935-setup";
constexpr const char* WIFI_AP_PASS = "vu2cpl1234";
constexpr uint16_t    WIFI_PORTAL_TIMEOUT_S       = 300;
constexpr uint32_t    WIFI_RECONNECT_TIMEOUT_MS   = 5 * 60 * 1000;

// ── Pins ─────────────────────────────────────────────────────────────────
constexpr int PIN_AS3935_IRQ  = 27;
constexpr int PIN_I2C_SDA     = 21;
constexpr int PIN_I2C_SCL     = 22;
constexpr int PIN_BOOT_BUTTON = 0;
constexpr uint32_t BOOT_HOLD_MS = 3000;

// ── MQTT topics ──────────────────────────────────────────────────────────
constexpr const char* TOPIC_EVENT  = "lightning/as3935";
constexpr const char* TOPIC_STATUS = "lightning/as3935/status";
constexpr const char* TOPIC_HB     = "lightning/as3935/hb";
constexpr const char* TOPIC_CMD    = "lightning/as3935/cmd";
constexpr const char* TOPIC_ACK    = "lightning/as3935/cmd/ack";
constexpr const char* CLIENT_ID    = "as3935-bridge";

constexpr uint32_t HEARTBEAT_MS        = 30 * 1000;
constexpr uint32_t STATUS_REPUBLISH_MS = 5  * 60 * 1000;
constexpr uint32_t NTP_WAIT_MS         = 10 * 1000;
constexpr uint8_t  MQTT_MAX_FAILS      = 60;             // × 5 s = 5 min before restart
constexpr uint32_t PUBLISH_WDT_MS      = 10 * 60 * 1000; // no successful publish ⇒ restart

// ── AS3935 ──────────────────────────────────────────────────────────────
constexpr uint8_t AS3935_I2C_ADDR  = 0x03;
constexpr uint8_t REG_CFG0         = 0x00;   // [5:1]=AFE_GB, [0]=PWD
constexpr uint8_t REG_CFG1         = 0x01;   // [6:4]=NF, [3:0]=WDTH
constexpr uint8_t REG_CFG2         = 0x02;   // [6]=CL_STAT, [5:4]=MIN_NUM_LIGH, [3:0]=SREJ
constexpr uint8_t REG_INT          = 0x03;   // [7:6]=LCO_FDIV, [5]=MASK_DIST, [3:0]=INT
constexpr uint8_t REG_ENERGY_L     = 0x04;
constexpr uint8_t REG_ENERGY_M     = 0x05;
constexpr uint8_t REG_ENERGY_H     = 0x06;
constexpr uint8_t REG_DISTANCE     = 0x07;
constexpr uint8_t REG_TUN_CAP      = 0x08;   // [7]=DISP_LCO, [3:0]=TUN_CAP
constexpr uint8_t REG_CALIB_RCO    = 0x3D;
constexpr uint8_t REG_CALIB_TRCO   = 0x3A;
constexpr uint8_t REG_CALIB_SRCO   = 0x3B;
constexpr uint8_t CMD_CALIB_RCO    = 0x96;

constexpr uint8_t INT_LIGHTNING = 0x08;
constexpr uint8_t INT_DISTURBER = 0x04;
constexpr uint8_t INT_NOISE     = 0x01;

// AFE_GB raw nibble (the chip stores these in CFG0[5:1])
constexpr uint8_t AFE_GB_OUTDOOR = 0x0E;
constexpr uint8_t AFE_GB_INDOOR  = 0x12;

// ── Tunables (defaults; overridden from NVS at boot) ─────────────────────
struct Tunables {
    uint8_t nf                = 4;       // 0..7
    uint8_t wdth              = 2;       // 0..15
    uint8_t srej              = 2;       // 0..15
    uint8_t tun_cap           = 10;      // 0..15
    uint8_t min_num_lightning = 0;       // datasheet code: 0=1, 1=5, 2=9, 3=16
    bool    mask_dist         = false;
    bool    indoor            = false;   // false = outdoor
    bool    modem_sleep_max   = true;    // WIFI_PS_MAX_MODEM
};
Tunables tun;

// NVS keys
constexpr const char* NVS_NAMESPACE = "as3935";
constexpr const char* NVS_K_NF      = "nf";
constexpr const char* NVS_K_WDTH    = "wdth";
constexpr const char* NVS_K_SREJ    = "srej";
constexpr const char* NVS_K_TUN_CAP = "tun_cap";
constexpr const char* NVS_K_MIN_LT  = "min_lt";
constexpr const char* NVS_K_MASK    = "mask_dist";
constexpr const char* NVS_K_INDOOR  = "indoor";
constexpr const char* NVS_K_PS_MAX  = "ps_max";

// ── Runtime state ────────────────────────────────────────────────────────
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

struct Counters { uint32_t lightning = 0, disturber = 0, noise = 0, irq = 0; };
Counters counters;

volatile bool     irqFired   = false;
volatile uint32_t calibEdges = 0;
void IRAM_ATTR onAs3935Irq() { irqFired = true; }
void IRAM_ATTR onCalibEdge() { calibEdges++; }

const char* calib_trco = "UNKNOWN";
const char* calib_srco = "UNKNOWN";

uint32_t bootEpoch               = 0;
uint32_t lastStatusMs            = 0;
uint32_t lastHb                  = 0;
uint32_t lastSuccessfulPublishMs = 0;
uint8_t  mqttFailCount           = 0;

char     lwtPayload[64];

// ── Time helpers ─────────────────────────────────────────────────────────
bool waitForNtpSync(uint32_t timeoutMs) {
    configTzTime("IST-5:30", "pool.ntp.org", "time.google.com");
    uint32_t deadline = millis() + timeoutMs;
    struct tm timeinfo;
    while (millis() < deadline) {
        if (getLocalTime(&timeinfo, 100) && timeinfo.tm_year > (2020 - 1900)) {
            return true;
        }
    }
    return false;
}
void isoNow(char* out, size_t n) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 50)) {
        strftime(out, n, "%Y-%m-%dT%H:%M:%S", &timeinfo);
    } else {
        snprintf(out, n, "1970-01-01T00:00:00");
    }
}
uint32_t uptimeSeconds() {
    if (bootEpoch == 0) return millis() / 1000;
    time_t now = time(nullptr);
    return (now > (time_t)bootEpoch) ? (uint32_t)(now - bootEpoch) : 0;
}

// ── I²C helpers ──────────────────────────────────────────────────────────
uint8_t as3935Read(uint8_t reg) {
    Wire.beginTransmission(AS3935_I2C_ADDR);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom(AS3935_I2C_ADDR, (uint8_t)1);
    return Wire.read();
}
void as3935Write(uint8_t reg, uint8_t v) {
    Wire.beginTransmission(AS3935_I2C_ADDR);
    Wire.write(reg);
    Wire.write(v);
    Wire.endTransmission();
}
void as3935Modify(uint8_t reg, uint8_t mask, uint8_t bits) {
    uint8_t v = (as3935Read(reg) & ~mask) | (bits & mask);
    as3935Write(reg, v);
}

// ── AS3935 parameter writers (one register field each) ───────────────────
void writeNF(uint8_t v)         { as3935Modify(REG_CFG1, 0x70, (v & 0x07) << 4); }
void writeWDTH(uint8_t v)       { as3935Modify(REG_CFG1, 0x0F, v & 0x0F); }
void writeSREJ(uint8_t v)       { as3935Modify(REG_CFG2, 0x0F, v & 0x0F); }
void writeTunCap(uint8_t v)     { as3935Modify(REG_TUN_CAP, 0x0F, v & 0x0F); }
void writeMaskDist(bool on)     { as3935Modify(REG_INT, 0x20, on ? 0x20 : 0); }
void writeMinNumLightning(uint8_t code) { as3935Modify(REG_CFG2, 0x30, (code & 0x03) << 4); }
void writeAFEGB(bool indoor) {
    uint8_t bits = (indoor ? AFE_GB_INDOOR : AFE_GB_OUTDOOR) << 1;
    as3935Modify(REG_CFG0, 0x3E, bits);
}

void applyAllTunables() {
    writeAFEGB(tun.indoor);
    writeNF(tun.nf);
    writeWDTH(tun.wdth);
    writeSREJ(tun.srej);
    writeTunCap(tun.tun_cap);
    writeMaskDist(tun.mask_dist);
    writeMinNumLightning(tun.min_num_lightning);
}

// ── NVS persistence ──────────────────────────────────────────────────────
void loadTunables() {
    Preferences p;
    p.begin(NVS_NAMESPACE, true);
    tun.nf                = p.getUChar(NVS_K_NF,      tun.nf);
    tun.wdth              = p.getUChar(NVS_K_WDTH,    tun.wdth);
    tun.srej              = p.getUChar(NVS_K_SREJ,    tun.srej);
    tun.tun_cap           = p.getUChar(NVS_K_TUN_CAP, tun.tun_cap);
    tun.min_num_lightning = p.getUChar(NVS_K_MIN_LT,  tun.min_num_lightning);
    tun.mask_dist         = p.getBool (NVS_K_MASK,    tun.mask_dist);
    tun.indoor            = p.getBool (NVS_K_INDOOR,  tun.indoor);
    tun.modem_sleep_max   = p.getBool (NVS_K_PS_MAX,  tun.modem_sleep_max);
    p.end();
}
void saveTunables() {
    Preferences p;
    p.begin(NVS_NAMESPACE, false);
    p.putUChar(NVS_K_NF,      tun.nf);
    p.putUChar(NVS_K_WDTH,    tun.wdth);
    p.putUChar(NVS_K_SREJ,    tun.srej);
    p.putUChar(NVS_K_TUN_CAP, tun.tun_cap);
    p.putUChar(NVS_K_MIN_LT,  tun.min_num_lightning);
    p.putBool (NVS_K_MASK,    tun.mask_dist);
    p.putBool (NVS_K_INDOOR,  tun.indoor);
    p.putBool (NVS_K_PS_MAX,  tun.modem_sleep_max);
    p.end();
}

// ── AS3935 init ──────────────────────────────────────────────────────────
void as3935Init() {
    uint8_t cfg0 = as3935Read(REG_CFG0);
    Serial.printf("[as3935] CFG0=0x%02X (i2c addr 0x%02X)\n", cfg0, AS3935_I2C_ADDR);
    if (cfg0 == 0x00 || cfg0 == 0xFF) {
        // Cold-bus first-read glitch — retry once.
        delay(5);
        cfg0 = as3935Read(REG_CFG0);
        Serial.printf("[as3935] CFG0 retry=0x%02X\n", cfg0);
        if (cfg0 == 0x00 || cfg0 == 0xFF) {
            Serial.println("[as3935] WARNING: chip not responding on I²C");
        }
    }

    applyAllTunables();
    Serial.printf("[as3935] applied: nf=%u wdth=%u srej=%u tun_cap=%u "
                  "mask_dist=%d min_lt=%u afe_gb=%s\n",
                  tun.nf, tun.wdth, tun.srej, tun.tun_cap,
                  tun.mask_dist, tun.min_num_lightning,
                  tun.indoor ? "indoor" : "outdoor");

    // CALIB_RCO: write 0x96 → 0x3D, wait ≥2 ms, verify DONE=1 NOK=0 in 0x3A / 0x3B.
    as3935Write(REG_CALIB_RCO, CMD_CALIB_RCO);
    delay(5);
    uint8_t trco = as3935Read(REG_CALIB_TRCO);
    uint8_t srco = as3935Read(REG_CALIB_SRCO);
    bool trcoOk = (trco & 0x80) && !(trco & 0x40);
    bool srcoOk = (srco & 0x80) && !(srco & 0x40);
    calib_trco = trcoOk ? "OK" : "FAIL";
    calib_srco = srcoOk ? "OK" : "FAIL";
    Serial.printf("[as3935] CALIB_RCO TRCO=%s (0x%02X) SRCO=%s (0x%02X)\n",
                  calib_trco, trco, calib_srco, srco);

    uint8_t pending = as3935Read(REG_INT) & 0x0F;
    Serial.printf("[as3935] cleared pending INT: 0x%X\n", pending);
}

// ── WiFi / portal ────────────────────────────────────────────────────────
void checkBootButtonForReset() {
    pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
    delay(50);
    if (digitalRead(PIN_BOOT_BUTTON) != LOW) return;
    Serial.printf("[wifi] BOOT pressed — hold %lus to erase WiFi creds\n",
                  (unsigned long)(BOOT_HOLD_MS / 1000));
    uint32_t start = millis();
    while (digitalRead(PIN_BOOT_BUTTON) == LOW) {
        if (millis() - start >= BOOT_HOLD_MS) {
            Serial.println("[wifi] BOOT held — erasing creds, entering portal");
            WiFiManager wm;
            wm.resetSettings();
            wm.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT_S);
            wm.startConfigPortal(WIFI_AP_SSID, WIFI_AP_PASS);
            ESP.restart();
        }
        delay(50);
    }
}
void applyModemSleep() {
    WiFi.setSleep(tun.modem_sleep_max ? WIFI_PS_MAX_MODEM : WIFI_PS_MIN_MODEM);
    Serial.printf("[wifi] modem sleep = %s\n", tun.modem_sleep_max ? "max" : "min");
}
void wifiSetup() {
    WiFi.mode(WIFI_STA);
    WiFiManager wm;
    wm.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT_S);
    wm.setConnectTimeout(20);
    Serial.printf("[wifi] auto-connect; if no stored creds, AP=%s pass=%s\n",
                  WIFI_AP_SSID, WIFI_AP_PASS);
    if (!wm.autoConnect(WIFI_AP_SSID, WIFI_AP_PASS)) {
        Serial.println("[wifi] portal timeout / failed — restarting");
        delay(1000);
        ESP.restart();
    }
    Serial.printf("[wifi] connected, RSSI %d dBm, IP %s\n",
                  WiFi.RSSI(), WiFi.localIP().toString().c_str());
}
void wifiWaitForReconnect() {
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > WIFI_RECONNECT_TIMEOUT_MS) {
            Serial.println("[wifi] no reconnect within timeout — restarting");
            delay(500);
            ESP.restart();
        }
        delay(500);
    }
    Serial.printf("[wifi] reconnected, RSSI %d dBm\n", WiFi.RSSI());
}

// ── MQTT ─────────────────────────────────────────────────────────────────
// Forward decls for the command path.
void publishStatus(const char* event);
void publishHeartbeat();
void publishAck(bool ok, const char* what, const char* err = nullptr);
void handleCmd(const char* payload, size_t length);

void onMqttMessage(char* topic, uint8_t* payload, unsigned int length) {
    if (strcmp(topic, TOPIC_CMD) == 0) {
        handleCmd((const char*)payload, length);
    }
}
void mqttConnect() {
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setBufferSize(512);
    mqtt.setCallback(onMqttMessage);
    while (!mqtt.connected()) {
        Serial.printf("[mqtt] connecting to %s:%d (attempt %u/%u)\n",
                      MQTT_HOST, MQTT_PORT, mqttFailCount + 1, MQTT_MAX_FAILS);
        if (mqtt.connect(CLIENT_ID, nullptr, nullptr,
                         TOPIC_STATUS, 1, true, lwtPayload)) {
            Serial.println("[mqtt] connected, LWT armed");
            mqtt.subscribe(TOPIC_CMD);
            Serial.printf("[mqtt] subscribed to %s\n", TOPIC_CMD);
            mqttFailCount = 0;
            return;
        }
        mqttFailCount++;
        if (mqttFailCount >= MQTT_MAX_FAILS) {
            Serial.println("[mqtt] max failures reached, restarting");
            delay(500);
            ESP.restart();
        }
        Serial.printf("[mqtt] failed rc=%d, retry in 5s\n", mqtt.state());
        delay(5000);
    }
}

// ── Publish ──────────────────────────────────────────────────────────────
void publishStatus(const char* event) {
    char ts[24]; isoNow(ts, sizeof(ts));
    static const uint16_t minNumMap[] = {1, 5, 9, 16};

    JsonDocument doc;
    doc["event"]             = event;
    doc["ts"]                = ts;
    doc["fw"]                = FIRMWARE_VERSION;
    doc["ip"]                = WiFi.localIP().toString();
    doc["rssi"]              = WiFi.RSSI();
    doc["irq_pin"]           = PIN_AS3935_IRQ;
    doc["nf"]                = tun.nf;
    doc["wdth"]              = tun.wdth;
    doc["srej"]              = tun.srej;
    doc["tun_cap"]           = tun.tun_cap;
    doc["mask_dist"]         = tun.mask_dist;
    doc["min_num_lightning"] = minNumMap[tun.min_num_lightning & 0x03];
    doc["afe_gb"]            = tun.indoor ? "indoor" : "outdoor";
    doc["modem_sleep"]       = tun.modem_sleep_max ? "max" : "min";
    // Back-compat aliases for any subscriber that still reads the Python-style keys.
    doc["noise_floor"]       = tun.nf;
    doc["antenna"]           = tun.indoor ? "indoor" : "outdoor";
    doc["calib_trco"]        = calib_trco;
    doc["calib_srco"]        = calib_srco;

    char buf[512];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    bool ok = mqtt.publish(TOPIC_STATUS, (const uint8_t*)buf, n, true);
    if (ok) {
        lastSuccessfulPublishMs = millis();
        lastStatusMs            = millis();
    }
    Serial.printf("[mqtt] status: %s\n", buf);
}
void publishHeartbeat() {
    char ts[24]; isoNow(ts, sizeof(ts));
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"alive\":true,\"ts\":\"%s\",\"uptime_s\":%lu,\"rssi\":%d,"
        "\"counters\":{\"lightning\":%lu,\"disturber\":%lu,"
        "\"noise\":%lu,\"irq\":%lu}}",
        ts, (unsigned long)uptimeSeconds(), WiFi.RSSI(),
        (unsigned long)counters.lightning, (unsigned long)counters.disturber,
        (unsigned long)counters.noise,     (unsigned long)counters.irq);
    if (mqtt.publish(TOPIC_HB, buf, true)) {
        lastSuccessfulPublishMs = millis();
    }
}
void publishAck(bool ok, const char* what, const char* err) {
    char ts[24]; isoNow(ts, sizeof(ts));
    JsonDocument doc;
    doc["ok"]  = ok;
    doc["cmd"] = what;
    if (err) doc["error"] = err;
    doc["ts"]  = ts;
    char buf[192];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    mqtt.publish(TOPIC_ACK, (const uint8_t*)buf, n, false);
    Serial.printf("[mqtt] ack: %s\n", buf);
}

// ── Event handler ────────────────────────────────────────────────────────
void handleAs3935Event() {
    delay(3);  // datasheet ≥2 ms after IRQ before reading 0x03
    uint8_t intReg = as3935Read(REG_INT) & 0x0F;
    counters.irq++;
    char ts[24]; isoNow(ts, sizeof(ts));
    char buf[160];

    if (intReg == INT_LIGHTNING) {
        uint8_t  distance = as3935Read(REG_DISTANCE) & 0x3F;
        uint32_t energy   = ((uint32_t)(as3935Read(REG_ENERGY_H) & 0x1F) << 16)
                          | ((uint32_t) as3935Read(REG_ENERGY_M)         <<  8)
                          |              as3935Read(REG_ENERGY_L);
        counters.lightning++;
        snprintf(buf, sizeof(buf),
            "{\"event\":\"lightning\",\"distance\":%u,\"energy\":%lu,"
            "\"timestamp\":\"%s\"}",
            distance, (unsigned long)energy, ts);
        mqtt.publish(TOPIC_EVENT, buf);
        Serial.printf("[as3935] ⚡ lightning d=%ukm e=%lu\n",
                      distance, (unsigned long)energy);
    } else if (intReg == INT_DISTURBER) {
        counters.disturber++;
        snprintf(buf, sizeof(buf),
            "{\"event\":\"disturber\",\"timestamp\":\"%s\"}", ts);
        mqtt.publish(TOPIC_EVENT, buf);
        Serial.println("[as3935] ⚠ disturber");
    } else if (intReg == INT_NOISE) {
        counters.noise++;
        snprintf(buf, sizeof(buf),
            "{\"event\":\"noise\",\"timestamp\":\"%s\"}", ts);
        mqtt.publish(TOPIC_EVENT, buf);
        Serial.println("[as3935] 📡 noise");
    } else {
        Serial.printf("[as3935] spurious IRQ, INT=0x%X\n", intReg);
    }
}

// ── Command dispatcher ───────────────────────────────────────────────────
bool handleSet(const char* key, JsonVariant value, char* err, size_t errlen) {
    if (strcmp(key, "nf") == 0) {
        int v = value.as<int>();
        if (v < 0 || v > 7) { snprintf(err, errlen, "nf out of range 0..7"); return false; }
        tun.nf = (uint8_t)v; writeNF(tun.nf);
    } else if (strcmp(key, "wdth") == 0) {
        int v = value.as<int>();
        if (v < 0 || v > 15) { snprintf(err, errlen, "wdth out of range 0..15"); return false; }
        tun.wdth = (uint8_t)v; writeWDTH(tun.wdth);
    } else if (strcmp(key, "srej") == 0) {
        int v = value.as<int>();
        if (v < 0 || v > 15) { snprintf(err, errlen, "srej out of range 0..15"); return false; }
        tun.srej = (uint8_t)v; writeSREJ(tun.srej);
    } else if (strcmp(key, "tun_cap") == 0) {
        int v = value.as<int>();
        if (v < 0 || v > 15) { snprintf(err, errlen, "tun_cap out of range 0..15"); return false; }
        tun.tun_cap = (uint8_t)v; writeTunCap(tun.tun_cap);
    } else if (strcmp(key, "mask_dist") == 0) {
        tun.mask_dist = value.as<bool>();
        writeMaskDist(tun.mask_dist);
    } else if (strcmp(key, "min_num_lightning") == 0) {
        int v = value.as<int>();
        uint8_t code;
        if      (v == 1)  code = 0;
        else if (v == 5)  code = 1;
        else if (v == 9)  code = 2;
        else if (v == 16) code = 3;
        else { snprintf(err, errlen, "min_num_lightning must be 1, 5, 9, or 16"); return false; }
        tun.min_num_lightning = code; writeMinNumLightning(code);
    } else if (strcmp(key, "afe_gb") == 0) {
        const char* s = value.as<const char*>();
        if      (s && strcmp(s, "indoor")  == 0) tun.indoor = true;
        else if (s && strcmp(s, "outdoor") == 0) tun.indoor = false;
        else { snprintf(err, errlen, "afe_gb must be 'indoor' or 'outdoor'"); return false; }
        writeAFEGB(tun.indoor);
    } else if (strcmp(key, "modem_sleep") == 0) {
        const char* s = value.as<const char*>();
        if (s && strcmp(s, "max") == 0)      tun.modem_sleep_max = true;
        else if (s && strcmp(s, "min") == 0) tun.modem_sleep_max = false;
        else { snprintf(err, errlen, "modem_sleep must be 'max' or 'min'"); return false; }
        applyModemSleep();
    } else {
        snprintf(err, errlen, "unknown key: %s", key);
        return false;
    }
    Serial.printf("[cmd] set %s applied\n", key);
    return true;
}

void calibrateTunCap();  // forward decl

void handleAction(const char* action) {
    if (strcmp(action, "republish_status") == 0) {
        publishStatus("ready");
        publishAck(true, "action:republish_status");
    } else if (strcmp(action, "calibrate_tun_cap") == 0) {
        publishAck(true, "action:calibrate_tun_cap");
        calibrateTunCap();
        saveTunables();
        publishStatus("ready");
    } else if (strcmp(action, "reboot") == 0) {
        publishAck(true, "action:reboot");
        delay(1500);   // first-ever NVS commit on a fresh namespace needs time
        ESP.restart();
    } else if (strcmp(action, "factory_reset_wifi") == 0) {
        publishAck(true, "action:factory_reset_wifi");
        delay(1500);
        WiFiManager wm;
        wm.resetSettings();
        ESP.restart();
    } else {
        char err[64]; snprintf(err, sizeof(err), "unknown action: %s", action);
        publishAck(false, "action:?", err);
    }
}

void handleCmd(const char* payload, size_t length) {
    JsonDocument doc;
    DeserializationError jerr = deserializeJson(doc, payload, length);
    if (jerr) {
        publishAck(false, "?", "json parse error");
        return;
    }
    const char* setKey = doc["set"];
    const char* action = doc["action"];
    if (setKey) {
        char what[48]; snprintf(what, sizeof(what), "set:%s", setKey);
        char errbuf[80];
        if (handleSet(setKey, doc["value"], errbuf, sizeof(errbuf))) {
            saveTunables();
            publishAck(true, what);
            publishStatus("ready");
        } else {
            publishAck(false, what, errbuf);
        }
    } else if (action) {
        handleAction(action);
    } else {
        publishAck(false, "?", "no 'set' or 'action' field");
    }
}

// ── TUN_CAP calibration (port of as3935_tune.py) ─────────────────────────
// Sweep TUN_CAP 0..15, count edges on IRQ pin while DISP_LCO routes
// the LC tank oscillator (÷128) to IRQ. Pick the cap value whose
// resulting frequency is closest to 500 kHz (±3.5% spec).
//
// Takes ~35 s (16 caps × 2 s + setup). The MQTT keepalive is pumped
// inside the sample loop to avoid getting dropped by the broker.
void calibrateTunCap() {
    Serial.println("[calib] starting TUN_CAP sweep");
    constexpr uint8_t  LCO_FDIV       = 3;
    constexpr uint16_t DIV            = 16 << LCO_FDIV;   // 128
    constexpr uint32_t TARGET_HZ      = 500000;
    constexpr float    TOLERANCE_PCT  = 3.5f;
    constexpr uint32_t SAMPLE_MS      = 2000;

    detachInterrupt(digitalPinToInterrupt(PIN_AS3935_IRQ));
    as3935Modify(REG_INT, 0xC0, LCO_FDIV << 6);  // LCO_FDIV bits [7:6]

    uint8_t bestCap   = 0;
    int32_t bestErrHz = INT32_MAX;
    float   bestFreq  = 0;

    for (uint8_t cap = 0; cap <= 15; cap++) {
        as3935Modify(REG_TUN_CAP, 0x0F, cap);
        as3935Modify(REG_TUN_CAP, 0x80, 0x80);  // DISP_LCO = 1
        delay(50);                              // settle LC tank

        calibEdges = 0;
        attachInterrupt(digitalPinToInterrupt(PIN_AS3935_IRQ), onCalibEdge, RISING);
        uint32_t start = millis();
        while (millis() - start < SAMPLE_MS) {
            mqtt.loop();  // keep MQTT alive over ~35 s sweep
            delay(20);
        }
        detachInterrupt(digitalPinToInterrupt(PIN_AS3935_IRQ));

        uint32_t edges = calibEdges;
        float freq = (edges * (float)DIV * 1000.0f) / (float)SAMPLE_MS;
        int32_t errHz = (int32_t)freq - (int32_t)TARGET_HZ;

        Serial.printf("[calib] cap=%2u edges=%6u freq=%.0f Hz err=%+.2f%%\n",
                      cap, edges, freq, (errHz * 100.0f) / (float)TARGET_HZ);

        if (abs(errHz) < abs(bestErrHz)) {
            bestErrHz = errHz;
            bestCap   = cap;
            bestFreq  = freq;
        }
    }

    // Cleanup: DISP_LCO off, LCO_FDIV → 0, apply winning cap, reattach normal ISR.
    as3935Modify(REG_TUN_CAP, 0x80, 0);
    as3935Modify(REG_INT, 0xC0, 0);
    writeTunCap(bestCap);
    tun.tun_cap = bestCap;

    float bestErrPct = (bestErrHz * 100.0f) / (float)TARGET_HZ;
    Serial.printf("[calib] best cap=%u freq=%.0f Hz err=%+.2f%% %s\n",
                  bestCap, bestFreq, bestErrPct,
                  fabsf(bestErrPct) <= TOLERANCE_PCT ? "(in spec)" : "(OUT OF SPEC)");

    delay(10);
    as3935Read(REG_INT);  // flush any pending INT
    irqFired = false;
    attachInterrupt(digitalPinToInterrupt(PIN_AS3935_IRQ), onAs3935Irq, RISING);
}

// ── Watchdog ─────────────────────────────────────────────────────────────
void checkPublishWatchdog() {
    if (lastSuccessfulPublishMs == 0) return;  // not armed until first success
    if (millis() - lastSuccessfulPublishMs > PUBLISH_WDT_MS) {
        Serial.println("[wdt] no successful publish in 10 min — restarting");
        delay(500);
        ESP.restart();
    }
}

// ── Lifecycle ────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.printf("\n[boot] vu2cpl-as3935-bridge %s\n", FIRMWARE_VERSION);

    // Suppress the WiFi driver's chatty warnings (CCMP-replay,
    // AUTH_FAIL retries, etc.). Real errors (E level) still print.
    esp_log_level_set("wifi", ESP_LOG_ERROR);

    loadTunables();
    Serial.printf("[nvs] nf=%u wdth=%u srej=%u tun_cap=%u mask=%d "
                  "min_lt=%u indoor=%d ps_max=%d\n",
                  tun.nf, tun.wdth, tun.srej, tun.tun_cap, tun.mask_dist,
                  tun.min_num_lightning, tun.indoor, tun.modem_sleep_max);

    checkBootButtonForReset();

    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    pinMode(PIN_AS3935_IRQ, INPUT);
    attachInterrupt(digitalPinToInterrupt(PIN_AS3935_IRQ), onAs3935Irq, RISING);

    wifiSetup();
    applyModemSleep();

    if (waitForNtpSync(NTP_WAIT_MS)) {
        bootEpoch = (uint32_t)time(nullptr);
        Serial.printf("[ntp] synced, bootEpoch=%lu\n", (unsigned long)bootEpoch);
    } else {
        Serial.println("[ntp] WARNING: not synced within timeout");
    }

    char ts[24]; isoNow(ts, sizeof(ts));
    snprintf(lwtPayload, sizeof(lwtPayload),
             "{\"event\":\"offline\",\"ts\":\"%s\"}", ts);

    mqttConnect();
    as3935Init();
    publishStatus("ready");

    Serial.printf("[loop] entering main loop, IRQ on GPIO%d\n", PIN_AS3935_IRQ);
}

void loop() {
    if (WiFi.status() != WL_CONNECTED) wifiWaitForReconnect();
    if (!mqtt.connected()) {
        mqttConnect();
        publishStatus("ready");
    }
    mqtt.loop();

    if (irqFired) {
        irqFired = false;
        handleAs3935Event();
    }

    uint32_t now = millis();
    if (now - lastHb >= HEARTBEAT_MS) {
        lastHb = now;
        publishHeartbeat();
    }
    if (now - lastStatusMs >= STATUS_REPUBLISH_MS) {
        publishStatus("ready");
    }

    checkPublishWatchdog();
}
