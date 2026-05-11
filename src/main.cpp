// vu2cpl-as3935-bridge — ESP32 firmware
//
// MQTT contract is wire-identical to as3935_mqtt.py from vu2cpl-shack repo:
// the Node-RED "Lightning Antenna Protector" tab consumes status/hb/event
// payloads by exact field names. Any divergence breaks the dashboard.

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <time.h>

// ── MQTT broker (LAN, no auth) ───────────────────────────────────────────
constexpr const char* MQTT_HOST = "192.168.1.169";
constexpr uint16_t    MQTT_PORT = 1883;

// ── WiFi config-portal (used on first boot or when stored creds fail) ────
constexpr const char* WIFI_AP_SSID = "vu2cpl-as3935-setup";
constexpr const char* WIFI_AP_PASS = "vu2cpl1234";
constexpr uint16_t    WIFI_PORTAL_TIMEOUT_S = 300;  // 5 min
constexpr uint32_t    WIFI_RECONNECT_TIMEOUT_MS = 5 * 60 * 1000;

// ── Pins ─────────────────────────────────────────────────────────────────
constexpr int PIN_AS3935_IRQ  = 27;  // RTC-capable, needed for future EXT0 wake
constexpr int PIN_I2C_SDA     = 21;
constexpr int PIN_I2C_SCL     = 22;
constexpr int PIN_BOOT_BUTTON = 0;   // hold during boot → erase WiFi creds
constexpr uint32_t BOOT_HOLD_MS = 3000;

// ── MQTT topics ──────────────────────────────────────────────────────────
constexpr const char* TOPIC_EVENT  = "lightning/as3935";
constexpr const char* TOPIC_STATUS = "lightning/as3935/status";
constexpr const char* TOPIC_HB     = "lightning/as3935/hb";
constexpr const char* CLIENT_ID    = "as3935-bridge";

// ── AS3935 ──────────────────────────────────────────────────────────────
// Matches as3935_mqtt.py production config. Antenna is outdoor on this
// bridge — AFE_GB nibble is 0x0E, which sits in REG_CFG0 bits [5:1] as
// the mask 0x1C. (Python's set_antenna_mode writes the same 0x1C, OR'd
// with reserved bits 7:6 and PWD bit 0.)
constexpr uint8_t AS3935_I2C_ADDR        = 0x03;
constexpr uint8_t AS3935_NF              = 4;       // 0..7, lower = more sensitive
constexpr uint8_t AS3935_AFE_GB_CFG0     = 0x1C;    // CFG0 bits for outdoor (AFE_GB=0x0E)
constexpr uint8_t AS3935_TUN_CAP_DEFAULT = 10;      // 0..15, ~8 pF/step; overridden by NVS
constexpr const char* ANTENNA_STR        = "outdoor";

// AS3935 registers
constexpr uint8_t REG_CFG0       = 0x00;
constexpr uint8_t REG_CFG1       = 0x01;
constexpr uint8_t REG_INT        = 0x03;
constexpr uint8_t REG_ENERGY_L   = 0x04;
constexpr uint8_t REG_ENERGY_M   = 0x05;
constexpr uint8_t REG_ENERGY_H   = 0x06;
constexpr uint8_t REG_DISTANCE   = 0x07;
constexpr uint8_t REG_TUN_CAP    = 0x08;
constexpr uint8_t REG_CALIB_RCO  = 0x3D;
constexpr uint8_t REG_CALIB_TRCO = 0x3A;
constexpr uint8_t REG_CALIB_SRCO = 0x3B;
constexpr uint8_t CMD_CALIB_RCO  = 0x96;

constexpr uint8_t INT_LIGHTNING = 0x08;
constexpr uint8_t INT_DISTURBER = 0x04;
constexpr uint8_t INT_NOISE     = 0x01;

constexpr uint32_t HEARTBEAT_MS  = 30 * 1000;
constexpr uint32_t NTP_WAIT_MS   = 10 * 1000;

// ── Runtime state ────────────────────────────────────────────────────────
WiFiClient   wifi;
PubSubClient mqtt(wifi);
Preferences  prefs;

uint8_t as3935_tun_cap = AS3935_TUN_CAP_DEFAULT;
const char* calib_trco = "UNKNOWN";
const char* calib_srco = "UNKNOWN";

struct Counters { uint32_t lightning = 0, disturber = 0, noise = 0, irq = 0; };
Counters counters;

volatile bool irqFired = false;
void IRAM_ATTR onAs3935Irq() { irqFired = true; }

uint32_t bootEpoch  = 0;   // unix seconds at SNTP sync (for uptime_s)
char     lwtPayload[64];   // must outlive mqtt.connect() — keep in BSS

// ── Time ─────────────────────────────────────────────────────────────────
// IST-5:30 = POSIX TZ for IST (UTC+5:30). Python daemon uses Pi's system
// localtime; the Pi is IST. Match.
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

// ── AS3935 init ──────────────────────────────────────────────────────────
// Sequence is the Python daemon's, in the same order:
//   self-test → antenna mode → noise floor → TUN_CAP → CALIB_RCO → INT flush
// WDTH/SREJ are intentionally left at chip defaults to match the Python
// daemon byte-for-byte. PRESET_DEFAULT is also skipped for the same reason.
void as3935Init() {
    uint8_t cfg0 = as3935Read(REG_CFG0);
    Serial.printf("[as3935] CFG0=0x%02X (i2c addr 0x%02X)\n", cfg0, AS3935_I2C_ADDR);
    if (cfg0 == 0x00 || cfg0 == 0xFF) {
        Serial.println("[as3935] WARNING: CFG0 suspicious — chip may not be responding");
    }

    // Antenna mode (CFG0 bits [5:1] = AFE_GB). Preserve bits 7:6 (reserved) + 0 (PWD).
    uint8_t v = (as3935Read(REG_CFG0) & 0xC1) | AS3935_AFE_GB_CFG0;
    as3935Write(REG_CFG0, v);
    Serial.printf("[as3935] antenna=%s CFG0=0x%02X\n", ANTENNA_STR, v);

    // Noise floor (CFG1 bits [6:4]). Preserve bit 7 (PWD-related) + [3:0] (WDTH).
    v = (as3935Read(REG_CFG1) & 0x8F) | ((AS3935_NF & 0x07) << 4);
    as3935Write(REG_CFG1, v);
    Serial.printf("[as3935] NF=%u CFG1=0x%02X\n", AS3935_NF, v);

    // TUN_CAP (REG 0x08 low nibble). Preserve DISP bits [7:5] + reserved bit 4.
    v = (as3935Read(REG_TUN_CAP) & 0xF0) | (as3935_tun_cap & 0x0F);
    as3935Write(REG_TUN_CAP, v);
    Serial.printf("[as3935] TUN_CAP=%u (~%u pF) REG0x08=0x%02X\n",
                  as3935_tun_cap, as3935_tun_cap * 8, v);

    // CALIB_RCO: write 0x96 to 0x3D, wait ≥2 ms, verify DONE=1 NOK=0 in TRCO/SRCO.
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
    if (!trcoOk || !srcoOk) {
        Serial.println("[as3935] WARNING: RC oscillator calibration failed");
    }

    // Flush any pending INT left from config writes.
    uint8_t pending = as3935Read(REG_INT) & 0x0F;
    Serial.printf("[as3935] cleared pending INT: 0x%X\n", pending);
}

// ── WiFi / SNTP / MQTT ───────────────────────────────────────────────────
// On first boot (or after BOOT-held reset, or when stored creds fail to
// connect) WiFiManager raises an AP named WIFI_AP_SSID. Connect your
// phone, the captive portal page lets you pick your home AP and enter
// the password. Creds are then persisted by the ESP32 WiFi stack and
// the AP closes on the next reboot.
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

void mqttConnect() {
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    while (!mqtt.connected()) {
        Serial.printf("[mqtt] connecting to %s:%d...\n", MQTT_HOST, MQTT_PORT);
        // LWT on STATUS topic (retained JSON) — matches Python daemon.
        if (mqtt.connect(CLIENT_ID, nullptr, nullptr,
                         TOPIC_STATUS, 1, true, lwtPayload)) {
            Serial.println("[mqtt] connected, LWT armed");
        } else {
            Serial.printf("[mqtt] failed rc=%d, retry in 5s\n", mqtt.state());
            delay(5000);
        }
    }
}

// ── Publish ──────────────────────────────────────────────────────────────
void publishStatus(const char* event) {
    char ts[24]; isoNow(ts, sizeof(ts));
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"event\":\"%s\",\"ts\":\"%s\","
        "\"noise_floor\":%u,\"antenna\":\"%s\","
        "\"tun_cap\":%u,\"irq_pin\":%d,"
        "\"calib_trco\":\"%s\",\"calib_srco\":\"%s\","
        "\"fw\":\"%s\"}",
        event, ts,
        AS3935_NF, ANTENNA_STR,
        as3935_tun_cap, PIN_AS3935_IRQ,
        calib_trco, calib_srco,
        FIRMWARE_VERSION);
    mqtt.publish(TOPIC_STATUS, buf, true);
    Serial.printf("[mqtt] status: %s\n", buf);
}

void publishHeartbeat() {
    char ts[24]; isoNow(ts, sizeof(ts));
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"alive\":true,\"ts\":\"%s\",\"uptime_s\":%lu,"
        "\"counters\":{\"lightning\":%lu,\"disturber\":%lu,"
        "\"noise\":%lu,\"irq\":%lu}}",
        ts, (unsigned long)uptimeSeconds(),
        (unsigned long)counters.lightning, (unsigned long)counters.disturber,
        (unsigned long)counters.noise,     (unsigned long)counters.irq);
    mqtt.publish(TOPIC_HB, buf, true);
}

void handleAs3935Event() {
    delay(3);  // datasheet: wait ≥2 ms after IRQ before reading 0x03
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

// ── Lifecycle ────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.printf("\n[boot] vu2cpl-as3935-bridge %s\n", FIRMWARE_VERSION);

    // Load persisted TUN_CAP from NVS (written by calibration mode).
    prefs.begin("as3935", true);
    as3935_tun_cap = prefs.getUChar("tun_cap", AS3935_TUN_CAP_DEFAULT);
    prefs.end();

    checkBootButtonForReset();

    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    pinMode(PIN_AS3935_IRQ, INPUT);
    attachInterrupt(digitalPinToInterrupt(PIN_AS3935_IRQ), onAs3935Irq, RISING);

    wifiSetup();

    if (waitForNtpSync(NTP_WAIT_MS)) {
        bootEpoch = (uint32_t)time(nullptr);
        Serial.printf("[ntp] synced, bootEpoch=%lu\n", (unsigned long)bootEpoch);
    } else {
        Serial.println("[ntp] WARNING: not synced within timeout; timestamps will be bogus");
    }

    // Compose LWT now that the clock is set, so a future broker-side
    // disconnect carries a meaningful boot-time timestamp (Python's quirk:
    // LWT ts is frozen at script start, not at disconnect time).
    char ts[24]; isoNow(ts, sizeof(ts));
    snprintf(lwtPayload, sizeof(lwtPayload),
             "{\"event\":\"offline\",\"ts\":\"%s\"}", ts);

    mqttConnect();
    as3935Init();
    publishStatus("ready");

    Serial.printf("[loop] entering main loop, IRQ on GPIO%d\n", PIN_AS3935_IRQ);
}

uint32_t lastHb = 0;

void loop() {
    if (WiFi.status() != WL_CONNECTED) wifiWaitForReconnect();
    if (!mqtt.connected()) {
        mqttConnect();
        publishStatus("ready");  // re-publish on every reconnect (matches Python)
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
}
