// vu2cpl-as3935-bridge — ESP32 firmware
//
// MQTT contract is wire-compatible with as3935_mqtt.py from
// vu2cpl-shack repo (status/hb/event keys preserved). v0.2.0 adds:
//   - cmd subscribe topic (lightning/as3935/cmd) for live tuning
//   - all AS3935 tunables exposed + persisted in NVS
//   - on-device TUN_CAP calibration (port of as3935_tune.py)
//   - WiFi modem sleep (WIFI_PS_MAX_MODEM by default)
//   - bounded MQTT reconnect + no-publish watchdog → ESP.restart()
// v0.3.0 adds:
//   - battery voltage telemetry on GPIO 34 (ADC1_CH6) via 1:2 divider,
//     published as vbat_mv on status + hb, queryable via cmd action,
//     per-chip Vref delta correctable via vbat_offset_mv NVS tunable
// v0.4.1 adds (after the 2026-10-10 / 10-14 keepalive-flapping outages):
//   - self-heal: 5 MQTT reconnects inside 10 min ⇒ restart (the publish
//     watchdog can't see that churn — upstream publishes keep succeeding)
//   - boot_cause in status (why the last restart happened)
//   - disturber/noise events throttled to one per evt_interval_s (default
//     60 s, count of coalesced events in the payload); per-minute rates and
//     the reconnect count ride the heartbeat
//   - OTA over HTTP, opened for 10 min by the ota_open cmd action; app
//     rollback if a new image doesn't reach MQTT before its first reboot
//   - modem_sleep "none" (WIFI_PS_NONE) as a third option
// v0.4.2 adds:
//   - wifi_off_test cmd action: counts disturbers for N s with WiFi on, N s
//     with the radio off, N s on again, and publishes the three counts
//     (retained) on lightning/as3935/test — is the ESP32 the disturber source?
//   - a lightning event whose publish fails is held and sent once MQTT is
//     back (the test takes the sensor offline on purpose; so does a flap)
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
#include <WebServer.h>
#include <Update.h>
#include <esp_log.h>
#include <esp_adc_cal.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <esp_ota_ops.h>
#include <time.h>
#include "bridge_util.h"

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

// Battery voltage divider on ADC1_CH6 (GPIO 34, input-only, WiFi-safe).
// External 100 kΩ + 100 kΩ divider from BAT+ to GND, tap to PIN_VBAT.
// Without the divider the pin floats and the reading is meaningless —
// the dashboard renders whatever comes out, so a missing divider is
// visually obvious (typically <100 mV at the pin → <200 mV reported).
constexpr int      PIN_VBAT             = 34;
constexpr int      VBAT_DIVIDER_RATIO   = 2;   // R1 == R2
constexpr int      VBAT_SAMPLES         = 32;
constexpr uint32_t VBAT_VREF_DEFAULT_MV = 1100;  // used if eFuse Vref absent

// ── MQTT topics ──────────────────────────────────────────────────────────
constexpr const char* TOPIC_EVENT      = "lightning/as3935";
constexpr const char* TOPIC_STATUS     = "lightning/as3935/status";
constexpr const char* TOPIC_HB         = "lightning/as3935/hb";
constexpr const char* TOPIC_CMD        = "lightning/as3935/cmd";
constexpr const char* TOPIC_ACK        = "lightning/as3935/cmd/ack";
constexpr const char* TOPIC_LAST_EVENT = "lightning/as3935/last_event";
constexpr const char* TOPIC_TEST       = "lightning/as3935/test";   // v0.4.2, retained
constexpr const char* CLIENT_ID    = "as3935-bridge";

constexpr uint32_t HEARTBEAT_MS        = 30 * 1000;
constexpr uint32_t STATUS_REPUBLISH_MS = 5  * 60 * 1000;
constexpr uint32_t NTP_WAIT_MS         = 10 * 1000;
constexpr uint8_t  MQTT_MAX_FAILS      = 60;             // × 5 s = 5 min before restart
constexpr uint32_t PUBLISH_WDT_MS      = 10 * 60 * 1000; // no successful publish ⇒ restart
constexpr uint8_t  CHURN_MAX_RECONNECTS = 5;               // this many MQTT reconnects …
constexpr uint32_t CHURN_WINDOW_MS      = 10 * 60 * 1000;  // … inside this window ⇒ restart

// ── OTA (HTTP upload, opened on demand by the ota_open cmd action) ───────
// Basic auth: user "ota", password = the MQTT password in NVS, so no secret is
// compiled in (the repo is public). An `iot` credential holder can already
// spoof lightning events and reboot the sensor; OTA adds nothing it can't do.
constexpr uint16_t    OTA_PORT      = 80;
constexpr uint32_t    OTA_WINDOW_MS = 10 * 60 * 1000;
constexpr const char* OTA_USER      = "ota";

// ── WiFi-off disturber test (v0.4.2) ─────────────────────────────────────
// Three windows of `secs`: on, off, on. 3 × 120 s + reconnect stays well
// inside PUBLISH_WDT_MS (heartbeats keep flowing in the on windows anyway).
constexpr uint16_t WIFI_TEST_DEFAULT_S = 60;
constexpr uint16_t WIFI_TEST_MIN_S     = 10;
constexpr uint16_t WIFI_TEST_MAX_S     = 120;
constexpr uint32_t WIFI_TEST_REJOIN_MS = 30 * 1000;   // then the normal 5-min reconnect path

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
// WiFi power-save modes, in NVS order (ps_mode key).
constexpr uint8_t PS_NONE = 0;   // WIFI_PS_NONE — radio always on (mains-powered units)
constexpr uint8_t PS_MIN  = 1;   // WIFI_PS_MIN_MODEM
constexpr uint8_t PS_MAX  = 2;   // WIFI_PS_MAX_MODEM
const char* psName(uint8_t m) { return m == PS_NONE ? "none" : m == PS_MIN ? "min" : "max"; }

struct Tunables {
    uint8_t nf                = 4;       // 0..7
    uint8_t wdth              = 2;       // 0..15
    uint8_t srej              = 2;       // 0..15
    uint8_t tun_cap           = 10;      // 0..15
    uint8_t min_num_lightning = 0;       // datasheet code: 0=1, 1=5, 2=9, 3=16
    bool    mask_dist         = false;
    bool    indoor            = false;   // false = outdoor
    uint8_t modem_sleep       = PS_MAX;  // PS_NONE / PS_MIN / PS_MAX
    int16_t vbat_offset_mv    = 0;       // ±500 mV trim for per-chip Vref delta
    uint16_t evt_interval_s   = 60;      // disturber/noise publish throttle; 0 = every event
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
constexpr const char* NVS_K_PS_MAX  = "ps_max";    // ≤ v0.4.0: bool; still written for downgrades
constexpr const char* NVS_K_PS_MODE = "ps_mode";   // v0.4.1: PS_NONE / PS_MIN / PS_MAX
constexpr const char* NVS_K_EVT_IVL = "evt_ivl_s";
constexpr const char* NVS_K_VBAT_OFFSET = "vbat_off_mv";
constexpr const char* NVS_K_MQTT_USER   = "mqtt_user";
constexpr const char* NVS_K_MQTT_PASS   = "mqtt_pass";

// ── Runtime state ────────────────────────────────────────────────────────
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

// MQTT credentials — provisioned via the WiFi portal, persisted in NVS.
// Empty username ⇒ anonymous connect (backward-compatible with a broker
// that still allows anonymous). Username defaults to "iot", the shack
// broker's ACL-restricted IoT account; the password is entered in the
// captive portal and never compiled into the firmware / committed to git.
char mqttUser[64] = "iot";
char mqttPass[64] = "";

// ADC calibration handle for the battery divider. Characterised once at
// boot in vbatInit(). esp_adc_cal_characterize() reads eFuse Vref if
// the chip was factory-calibrated (most ESP32s are) and falls back to
// VBAT_VREF_DEFAULT_MV otherwise.
esp_adc_cal_characteristics_t adcCalVbat;
bool adcCalReady = false;

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

ChurnTracker<CHURN_MAX_RECONNECTS> churn(CHURN_WINDOW_MS);
uint32_t      mqttReconnects = 0;     // since boot, reported in the heartbeat
EventThrottle thrDisturber, thrNoise;
Counters      hbPrevCounters;         // counters at the previous heartbeat, for rate_per_min
uint32_t      hbPrevMs = 0;
uint16_t      wifiTestPendingSecs = 0; // set by the cmd callback, run from loop()

// A lightning event that couldn't be published (MQTT down) is kept and sent
// once the connection is back, so a strike during an outage isn't lost.
// Only the latest one is kept; the counters still count them all.
struct PendingLightning { bool set = false; uint8_t distance = 0; uint32_t energy = 0; char ts[24] = ""; };
PendingLightning pendingLightning;

// ── Restart bookkeeping ──────────────────────────────────────────────────
// Every deliberate restart goes through restartWith(), which leaves its cause
// in RTC memory; that survives a software reset (not a power cut), so the next
// boot can report *why* in status.boot_cause — e.g. "mqtt_churn" proves the
// self-heal fired, "brownout" points at the supply.
RTC_NOINIT_ATTR uint32_t rtcCauseMagic;
RTC_NOINIT_ATTR char     rtcCause[16];
constexpr uint32_t CAUSE_MAGIC = 0xA5935041;
char bootCause[16] = "unknown";

void restartWith(const char* cause) {
    strncpy(rtcCause, cause, sizeof(rtcCause) - 1);
    rtcCause[sizeof(rtcCause) - 1] = '\0';
    rtcCauseMagic = CAUSE_MAGIC;
    Serial.printf("[boot] restarting: %s\n", cause);
    delay(500);
    ESP.restart();
}
void resolveBootCause() {
    esp_reset_reason_t r = esp_reset_reason();
    const char* c;
    switch (r) {
        case ESP_RST_POWERON:   c = "power_on";  break;
        case ESP_RST_EXT:       c = "external";  break;
        case ESP_RST_SW:        c = "sw";        break;
        case ESP_RST_PANIC:     c = "panic";     break;
        case ESP_RST_INT_WDT:   c = "int_wdt";   break;
        case ESP_RST_TASK_WDT:  c = "task_wdt";  break;
        case ESP_RST_WDT:       c = "wdt";       break;
        case ESP_RST_BROWNOUT:  c = "brownout";  break;
        case ESP_RST_DEEPSLEEP: c = "deepsleep"; break;
        default:                c = "other";     break;
    }
    if (r == ESP_RST_SW && rtcCauseMagic == CAUSE_MAGIC) {
        rtcCause[sizeof(rtcCause) - 1] = '\0';
        c = rtcCause;
    }
    strncpy(bootCause, c, sizeof(bootCause) - 1);
    bootCause[sizeof(bootCause) - 1] = '\0';
    rtcCauseMagic = 0;   // consume, so a later unexplained reset isn't misattributed
}

// Keep a freshly OTA-flashed image on probation until it has reached MQTT:
// the Arduino core would otherwise mark it valid at boot. If it restarts for
// any reason before markAppValidIfPending() runs, the bootloader rolls back
// to the previous image. A USB-flashed image is never pending, so this is a
// no-op for it. extern "C": the core's weak default is a C function, and a
// C++-mangled definition would silently not replace it (checked with nm).
extern "C" bool verifyRollbackLater() { return true; }
bool appValidated = false;
void markAppValidIfPending() {
    if (appValidated) return;
    appValidated = true;
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (running && esp_ota_get_state_partition(running, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        Serial.println("[ota] new image reached MQTT — marked valid, rollback cancelled");
    }
}

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
    // ps_mode wins; a unit upgraded from ≤ v0.4.0 only has the ps_max bool.
    if (p.isKey(NVS_K_PS_MODE)) {
        tun.modem_sleep = p.getUChar(NVS_K_PS_MODE, tun.modem_sleep);
        if (tun.modem_sleep > PS_MAX) tun.modem_sleep = PS_MAX;
    } else {
        tun.modem_sleep = p.getBool(NVS_K_PS_MAX, tun.modem_sleep == PS_MAX) ? PS_MAX : PS_MIN;
    }
    tun.evt_interval_s    = p.getUShort(NVS_K_EVT_IVL, tun.evt_interval_s);
    tun.vbat_offset_mv    = p.getShort(NVS_K_VBAT_OFFSET, tun.vbat_offset_mv);

    // MQTT credentials — String form keeps the compiled-in default
    // (mqttUser="iot", mqttPass="") when the NVS key is absent.
    String mu = p.getString(NVS_K_MQTT_USER, mqttUser);
    String mp = p.getString(NVS_K_MQTT_PASS, mqttPass);
    strncpy(mqttUser, mu.c_str(), sizeof(mqttUser) - 1); mqttUser[sizeof(mqttUser) - 1] = '\0';
    strncpy(mqttPass, mp.c_str(), sizeof(mqttPass) - 1); mqttPass[sizeof(mqttPass) - 1] = '\0';
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
    p.putUChar(NVS_K_PS_MODE, tun.modem_sleep);
    p.putBool (NVS_K_PS_MAX,  tun.modem_sleep == PS_MAX);   // what v0.4.0 would read back
    p.putUShort(NVS_K_EVT_IVL, tun.evt_interval_s);
    p.putShort(NVS_K_VBAT_OFFSET, tun.vbat_offset_mv);
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

// ── Battery voltage (ADC on GPIO 34) ─────────────────────────────────────
// 1:2 external divider (100 kΩ + 100 kΩ) brings 18650 voltage range
// (3.0–4.2 V) into the ADC's linear region at 11 dB attenuation.
// Without the divider, GPIO 34 floats and readings are meaningless; the
// dashboard will show ~0 V and the operator immediately knows the
// hardware mod hasn't been done.
//
// esp_adc_cal_characterize() uses the eFuse-stored Vref if present
// (most ESP32s are factory-calibrated, ~30 mV typical delta from
// nominal 1100 mV). If not present, it falls back to the supplied
// default — readings will be ±50 mV off, correctable via the
// vbat_offset_mv NVS tunable.
void vbatInit() {
    analogReadResolution(12);
    analogSetPinAttenuation(PIN_VBAT, ADC_11db);
    esp_adc_cal_value_t src = esp_adc_cal_characterize(
        ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12,
        VBAT_VREF_DEFAULT_MV, &adcCalVbat);
    adcCalReady = true;
    const char* srcStr =
        (src == ESP_ADC_CAL_VAL_EFUSE_VREF)  ? "eFuse Vref" :
        (src == ESP_ADC_CAL_VAL_EFUSE_TP)    ? "eFuse two-point" :
        (src == ESP_ADC_CAL_VAL_DEFAULT_VREF)? "default Vref" : "?";
    Serial.printf("[vbat] ADC characterised, cal source: %s\n", srcStr);
}

// Returns battery voltage in millivolts, including the NVS-trimmed
// offset. Averages VBAT_SAMPLES samples to suppress ADC noise.
uint32_t readVbat_mV() {
    if (!adcCalReady) return 0;
    uint32_t acc = 0;
    for (int i = 0; i < VBAT_SAMPLES; i++) acc += analogRead(PIN_VBAT);
    uint32_t raw = acc / VBAT_SAMPLES;
    uint32_t mv_at_pin = esp_adc_cal_raw_to_voltage(raw, &adcCalVbat);
    int32_t mv = (int32_t)mv_at_pin * VBAT_DIVIDER_RATIO + tun.vbat_offset_mv;
    if (mv < 0) mv = 0;
    return (uint32_t)mv;
}

// ── MQTT credential portal fields (persisted in NVS) ─────────────────────
// Shown in the WiFiManager captive portal alongside the WiFi fields, so
// the operator sets the broker username/password at provisioning time.
WiFiManagerParameter g_mqttUserParam("mqttuser", "MQTT username", "", 63);
WiFiManagerParameter g_mqttPassParam("mqttpass", "MQTT password", "", 63);

void saveMqttCreds(const char* u, const char* pw) {
    strncpy(mqttUser, u,  sizeof(mqttUser) - 1); mqttUser[sizeof(mqttUser) - 1] = '\0';
    strncpy(mqttPass, pw, sizeof(mqttPass) - 1); mqttPass[sizeof(mqttPass) - 1] = '\0';
    Preferences p;
    p.begin(NVS_NAMESPACE, false);
    p.putString(NVS_K_MQTT_USER, mqttUser);
    p.putString(NVS_K_MQTT_PASS, mqttPass);
    p.end();
    Serial.printf("[mqtt] credentials saved to NVS (user=%s)\n", mqttUser);
}
// Pre-fill the portal fields with the current values, then register them.
void attachMqttParams(WiFiManager& wm) {
    g_mqttUserParam.setValue(mqttUser, sizeof(mqttUser) - 1);
    g_mqttPassParam.setValue(mqttPass, sizeof(mqttPass) - 1);
    wm.addParameter(&g_mqttUserParam);
    wm.addParameter(&g_mqttPassParam);
}
// After the portal closes, persist only if the operator changed a field.
// Version-robust: no dependency on setSaveParamsCallback.
void commitMqttParamsIfChanged() {
    const char* u  = g_mqttUserParam.getValue();
    const char* pw = g_mqttPassParam.getValue();
    if (strcmp(u, mqttUser) != 0 || strcmp(pw, mqttPass) != 0) {
        saveMqttCreds(u, pw);
    }
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
            attachMqttParams(wm);
            wm.startConfigPortal(WIFI_AP_SSID, WIFI_AP_PASS);
            commitMqttParamsIfChanged();
            restartWith("portal");
        }
        delay(50);
    }
}
void applyModemSleep() {
    WiFi.setSleep(tun.modem_sleep == PS_NONE ? WIFI_PS_NONE :
                  tun.modem_sleep == PS_MIN  ? WIFI_PS_MIN_MODEM : WIFI_PS_MAX_MODEM);
    Serial.printf("[wifi] modem sleep = %s\n", psName(tun.modem_sleep));
}
void wifiSetup() {
    WiFi.mode(WIFI_STA);
    WiFiManager wm;
    wm.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT_S);
    wm.setConnectTimeout(20);
    attachMqttParams(wm);
    Serial.printf("[wifi] auto-connect; if no stored creds, AP=%s pass=%s\n",
                  WIFI_AP_SSID, WIFI_AP_PASS);
    if (!wm.autoConnect(WIFI_AP_SSID, WIFI_AP_PASS)) {
        Serial.println("[wifi] portal timeout / failed — restarting");
        delay(500);
        restartWith("portal_timeout");
    }
    commitMqttParamsIfChanged();
    Serial.printf("[wifi] connected, RSSI %d dBm, IP %s\n",
                  WiFi.RSSI(), WiFi.localIP().toString().c_str());
}
void wifiWaitForReconnect() {
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > WIFI_RECONNECT_TIMEOUT_MS) {
            Serial.println("[wifi] no reconnect within timeout — restarting");
            restartWith("wifi_timeout");
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
void publishLastEvent(const char* event, uint8_t distance, uint32_t energy);
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
    const char* user = (mqttUser[0] != '\0') ? mqttUser : nullptr;
    const char* pass = (mqttPass[0] != '\0') ? mqttPass : nullptr;
    while (!mqtt.connected()) {
        Serial.printf("[mqtt] connecting to %s:%d as %s (attempt %u/%u)\n",
                      MQTT_HOST, MQTT_PORT, user ? user : "(anonymous)",
                      mqttFailCount + 1, MQTT_MAX_FAILS);
        if (mqtt.connect(CLIENT_ID, user, pass,
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
            restartWith("mqtt_fail");
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
    doc["modem_sleep"]       = psName(tun.modem_sleep);
    doc["evt_interval_s"]    = tun.evt_interval_s;
    doc["boot_cause"]        = bootCause;
    doc["vbat_mv"]           = readVbat_mV();
    doc["vbat_offset_mv"]    = tun.vbat_offset_mv;
    // Back-compat aliases for any subscriber that still reads the Python-style keys.
    doc["noise_floor"]       = tun.nf;
    doc["antenna"]           = tun.indoor ? "indoor" : "outdoor";
    doc["calib_trco"]        = calib_trco;
    doc["calib_srco"]        = calib_srco;

    char buf[512];
    if (measureJson(doc) >= sizeof(buf)) {   // would truncate into invalid JSON
        Serial.printf("[mqtt] status too big (%u bytes), not published\n", (unsigned)measureJson(doc));
        return;
    }
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
    // Events per minute since the previous heartbeat (since boot for the first).
    // With events throttled on the wire, this is where the real rate shows.
    uint32_t now = millis();
    float mins = (now - hbPrevMs) / 60000.0f;
    if (mins <= 0) mins = 1.0f / 60000.0f;
    float distRate  = (counters.disturber - hbPrevCounters.disturber) / mins;
    float noiseRate = (counters.noise     - hbPrevCounters.noise)     / mins;
    char buf[384];
    snprintf(buf, sizeof(buf),
        "{\"alive\":true,\"ts\":\"%s\",\"uptime_s\":%lu,\"rssi\":%d,"
        "\"vbat_mv\":%lu,"
        "\"counters\":{\"lightning\":%lu,\"disturber\":%lu,"
        "\"noise\":%lu,\"irq\":%lu},"
        "\"rate_per_min\":{\"disturber\":%.1f,\"noise\":%.1f},"
        "\"mqtt_reconnects\":%lu}",
        ts, (unsigned long)uptimeSeconds(), WiFi.RSSI(),
        (unsigned long)readVbat_mV(),
        (unsigned long)counters.lightning, (unsigned long)counters.disturber,
        (unsigned long)counters.noise,     (unsigned long)counters.irq,
        distRate, noiseRate, (unsigned long)mqttReconnects);
    hbPrevCounters = counters;
    hbPrevMs       = now;
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

// Retained "last event" topic. Survives Node-RED restart so the
// dashboard's LAST SEEN field can rehydrate within ~100 ms of subscribe
// instead of waiting for the next live disturber / noise / lightning
// strike. Distance and energy are only meaningful for lightning events;
// for disturber/noise pass 0 (dashboard renders them as 0 if shown,
// or '—' depending on the widget's null-check).
void publishLastEvent(const char* event, uint8_t distance, uint32_t energy) {
    char ts[24]; isoNow(ts, sizeof(ts));
    uint64_t ts_ms = (uint64_t)time(nullptr) * 1000ULL;
    char buf[224];
    int n = snprintf(buf, sizeof(buf),
        "{\"event\":\"%s\",\"distance\":%u,\"energy\":%lu,"
        "\"timestamp\":\"%s\",\"ts_epoch_ms\":%llu}",
        event, distance, (unsigned long)energy, ts,
        (unsigned long long)ts_ms);
    if (n > 0 && n < (int)sizeof(buf)) {
        mqtt.publish(TOPIC_LAST_EVENT, (const uint8_t*)buf, n, true);
    }
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
        if (mqtt.publish(TOPIC_EVENT, buf)) {
            publishLastEvent("lightning", distance, energy);
        } else {
            pendingLightning.set      = true;
            pendingLightning.distance = distance;
            pendingLightning.energy   = energy;
            strncpy(pendingLightning.ts, ts, sizeof(pendingLightning.ts) - 1);
            Serial.println("[as3935] lightning NOT published (MQTT down) — held for reconnect");
        }
        Serial.printf("[as3935] ⚡ lightning d=%ukm e=%lu\n",
                      distance, (unsigned long)energy);
    } else if (intReg == INT_DISTURBER) {
        // Throttled (v0.4.1): one publish per evt_interval_s, with "count" =
        // events it stands for. The full tally is counters.disturber in the hb.
        counters.disturber++;
        uint32_t n = thrDisturber.onEvent(millis(), tun.evt_interval_s * 1000UL);
        if (n) {
            snprintf(buf, sizeof(buf),
                "{\"event\":\"disturber\",\"timestamp\":\"%s\",\"count\":%lu}",
                ts, (unsigned long)n);
            mqtt.publish(TOPIC_EVENT, buf);
            publishLastEvent("disturber", 0, 0);
            Serial.printf("[as3935] ⚠ disturber (×%lu)\n", (unsigned long)n);
        }
    } else if (intReg == INT_NOISE) {
        counters.noise++;
        uint32_t n = thrNoise.onEvent(millis(), tun.evt_interval_s * 1000UL);
        if (n) {
            snprintf(buf, sizeof(buf),
                "{\"event\":\"noise\",\"timestamp\":\"%s\",\"count\":%lu}",
                ts, (unsigned long)n);
            mqtt.publish(TOPIC_EVENT, buf);
            publishLastEvent("noise", 0, 0);
            Serial.printf("[as3935] 📡 noise (×%lu)\n", (unsigned long)n);
        }
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
        if      (s && strcmp(s, "max")  == 0) tun.modem_sleep = PS_MAX;
        else if (s && strcmp(s, "min")  == 0) tun.modem_sleep = PS_MIN;
        else if (s && strcmp(s, "none") == 0) tun.modem_sleep = PS_NONE;
        else { snprintf(err, errlen, "modem_sleep must be 'max', 'min' or 'none'"); return false; }
        applyModemSleep();
    } else if (strcmp(key, "evt_interval_s") == 0) {
        // Disturber/noise publish throttle. 0 = publish every event (v0.4.0).
        int v = value.as<int>();
        if (v < 0 || v > 3600) { snprintf(err, errlen, "evt_interval_s out of range 0..3600"); return false; }
        tun.evt_interval_s = (uint16_t)v;
    } else if (strcmp(key, "vbat_offset_mv") == 0) {
        // Per-chip Vref trim. ±500 mV bracket is generous — eFuse-calibrated
        // chips need <50 mV typically; the wider range accommodates uncalibrated
        // chips and the small drift from the divider resistors' ±1% tolerance.
        int v = value.as<int>();
        if (v < -500 || v > 500) {
            snprintf(err, errlen, "vbat_offset_mv out of range -500..500");
            return false;
        }
        tun.vbat_offset_mv = (int16_t)v;
    } else {
        snprintf(err, errlen, "unknown key: %s", key);
        return false;
    }
    Serial.printf("[cmd] set %s applied\n", key);
    return true;
}

void calibrateTunCap();  // forward decl
bool otaOpen();          // forward decl

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
        restartWith("cmd_reboot");
    } else if (strcmp(action, "factory_reset_wifi") == 0) {
        publishAck(true, "action:factory_reset_wifi");
        delay(1500);
        WiFiManager wm;
        wm.resetSettings();
        restartWith("cmd_wifi_reset");
    } else if (strcmp(action, "ota_open") == 0) {
        // Opens the HTTP update server for OTA_WINDOW_MS (re-sending extends it).
        // Refused without an MQTT password: it doubles as the upload password.
        if (otaOpen()) {
            char ackbuf[96];
            snprintf(ackbuf, sizeof(ackbuf), "action:ota_open url=http://%s/update for %lus",
                     WiFi.localIP().toString().c_str(), (unsigned long)(OTA_WINDOW_MS / 1000));
            publishAck(true, ackbuf);
        } else {
            publishAck(false, "action:ota_open", "no MQTT password in NVS (it is the OTA password)");
        }
    } else if (strcmp(action, "query_vbat") == 0) {
        // One-shot snapshot — useful when the dashboard wants a fresh reading
        // outside the heartbeat cadence (e.g. after solar panel inspection).
        // Republishes the full status so the meta + vbat row both update
        // atomically; the ack carries the mV value as well for callers that
        // only want one line back.
        uint32_t mv = readVbat_mV();
        char ackbuf[80];
        snprintf(ackbuf, sizeof(ackbuf), "action:query_vbat vbat_mv=%lu",
                 (unsigned long)mv);
        publishAck(true, ackbuf);
        publishStatus("ready");
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
    } else if (action && strcmp(action, "wifi_off_test") == 0) {
        // Runs from loop(), not here: it disconnects MQTT, which must not
        // happen inside PubSubClient's own callback.
        int secs = doc["secs"] | (int)WIFI_TEST_DEFAULT_S;
        if (secs < WIFI_TEST_MIN_S || secs > WIFI_TEST_MAX_S) {
            publishAck(false, "action:wifi_off_test", "secs out of range 10..120");
        } else {
            wifiTestPendingSecs = (uint16_t)secs;
            char what[112];
            snprintf(what, sizeof(what),
                     "action:wifi_off_test secs=%d: on/off/on, ~%ds offline, result on %s",
                     secs, secs + 5, TOPIC_TEST);
            publishAck(true, what);
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

// ── OTA over HTTP ────────────────────────────────────────────────────────
// Closed by default. `{"action":"ota_open"}` starts a WebServer on OTA_PORT for
// OTA_WINDOW_MS; then (see ota.sh)
//   curl -u ota:<mqtt password> -F firmware=@firmware.bin http://<ip>/update
// The Mac (Trusted VLAN) can open connections into the IoT VLAN, but IoT can't
// call back out, which is why this is a push to the device and not ArduinoOTA
// (whose device side connects back to the uploader). A successfully written
// image reboots straight into probation (see markAppValidIfPending()).
WebServer* otaServer   = nullptr;
uint32_t   otaOpenedMs = 0;
bool       otaDenied   = false;   // this upload failed auth: don't touch flash
bool       otaWritten  = false;   // Update.end(true) succeeded
bool       otaReboot   = false;

bool otaAuthorised() {
    if (otaServer->authenticate(OTA_USER, mqttPass)) return true;
    otaServer->requestAuthentication(BASIC_AUTH, "as3935-ota");
    return false;
}

bool otaOpen() {
    if (mqttPass[0] == '\0') return false;
    otaOpenedMs = millis();
    if (otaServer) return true;   // already open: window extended
    otaServer = new WebServer(OTA_PORT);
    otaServer->on("/", HTTP_GET, []() {
        if (!otaAuthorised()) return;
        otaServer->send(200, "text/plain",
            String("vu2cpl-as3935-bridge ") + FIRMWARE_VERSION +
            " - POST firmware.bin as form field 'firmware' to /update\n");
    });
    otaServer->on("/update", HTTP_POST,
        []() {   // after the body: report and, on success, reboot
            if (!otaAuthorised()) return;
            if (otaWritten) {
                otaServer->send(200, "text/plain", "OK - rebooting into the new image\n");
                otaReboot = true;
            } else {
                otaServer->send(500, "text/plain",
                    String("FAIL: ") + (Update.hasError() ? Update.errorString() : "no firmware received") + "\n");
            }
        },
        []() {   // body chunks
            HTTPUpload& up = otaServer->upload();
            if (up.status == UPLOAD_FILE_START) {
                otaWritten = false;
                otaDenied  = !otaServer->authenticate(OTA_USER, mqttPass);
                if (otaDenied) { Serial.println("[ota] upload refused: bad credentials"); return; }
                Serial.printf("[ota] receiving %s\n", up.filename.c_str());
                if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
            } else if (otaDenied) {
                return;
            } else if (up.status == UPLOAD_FILE_WRITE) {
                if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
            } else if (up.status == UPLOAD_FILE_END) {
                otaWritten = Update.end(true);
                if (otaWritten) Serial.printf("[ota] %u bytes written, image OK\n", (unsigned)up.totalSize);
                else            Update.printError(Serial);
            } else if (up.status == UPLOAD_FILE_ABORTED) {
                Update.abort();
                Serial.println("[ota] upload aborted");
            }
        });
    otaServer->begin();
    Serial.printf("[ota] open on :%u for %lus\n", OTA_PORT, (unsigned long)(OTA_WINDOW_MS / 1000));
    return true;
}

void otaLoop() {
    if (!otaServer) return;
    otaServer->handleClient();
    if (otaReboot) {
        delay(300);   // let the HTTP response flush
        restartWith("ota");
    }
    if (millis() - otaOpenedMs > OTA_WINDOW_MS) {
        otaServer->stop();
        delete otaServer;
        otaServer = nullptr;
        Serial.println("[ota] window closed");
    }
}

// ── WiFi-off disturber test (v0.4.2) ─────────────────────────────────────
// Is the ESP32's own WiFi the disturber source? The 2026-10-14 A/B/A test
// ruled out the per-event MQTT traffic; this switches the radio off entirely.
struct WinCount { uint32_t dist, noise, light; float secs; };
void flushPendingLightning();   // below

// Services AS3935 IRQs for `ms`; while online also keeps MQTT and the
// heartbeat going. Returns the counter deltas over the window.
WinCount countWindow(uint32_t ms, bool online) {
    Counters c0 = counters;
    uint32_t t0 = millis();
    while (millis() - t0 < ms) {
        if (online) {
            mqtt.loop();
            flushPendingLightning();
            if (millis() - lastHb >= HEARTBEAT_MS) { lastHb = millis(); publishHeartbeat(); }
        }
        if (irqFired) { irqFired = false; handleAs3935Event(); }
        delay(5);
    }
    return { counters.disturber - c0.disturber, counters.noise - c0.noise,
             counters.lightning - c0.lightning, (millis() - t0) / 1000.0f };
}

int fmtWin(char* out, size_t n, const char* name, const WinCount& w) {
    return snprintf(out, n, "\"%s\":{\"disturber\":%lu,\"noise\":%lu,\"lightning\":%lu,\"secs\":%.1f,\"per_min\":%.1f}",
                    name, (unsigned long)w.dist, (unsigned long)w.noise, (unsigned long)w.light,
                    w.secs, w.secs > 0 ? w.dist * 60.0f / w.secs : 0.0f);
}

void runWifiOffTest(uint16_t secs) {
    const uint32_t ms = secs * 1000UL;
    Serial.printf("[test] wifi_off_test: %us on, %us off, %us on\n", secs, secs, secs);
    WinCount before = countWindow(ms, true);

    mqtt.disconnect();          // clean DISCONNECT: the broker does not fire the LWT
    delay(100);
    WiFi.mode(WIFI_OFF);        // radio fully off (driver stopped)
    Serial.println("[test] WiFi off");
    WinCount off = countWindow(ms, false);

    uint32_t tr = millis();
    WiFi.mode(WIFI_STA);
    WiFi.begin();               // stored credentials (saved by WiFiManager)
    while (WiFi.status() != WL_CONNECTED && millis() - tr < WIFI_TEST_REJOIN_MS) {
        if (irqFired) { irqFired = false; handleAs3935Event(); }
        delay(50);
    }
    if (WiFi.status() != WL_CONNECTED) wifiWaitForReconnect();   // 5 min, then restart
    applyModemSleep();
    mqttConnect();
    publishStatus("ready");
    flushPendingLightning();    // a strike during the off window goes out now
    float rejoin = (millis() - tr) / 1000.0f;
    Serial.printf("[test] back online after %.1fs\n", rejoin);

    WinCount after = countWindow(ms, true);

    char ts[24]; isoNow(ts, sizeof(ts));
    char buf[480];
    int n = snprintf(buf, sizeof(buf), "{\"test\":\"wifi_off\",\"ts\":\"%s\",\"fw\":\"%s\",\"secs\":%u,",
                     ts, FIRMWARE_VERSION, secs);
    n += fmtWin(buf + n, sizeof(buf) - n, "on_before", before);  buf[n++] = ',';
    n += fmtWin(buf + n, sizeof(buf) - n, "off", off);           buf[n++] = ',';
    n += fmtWin(buf + n, sizeof(buf) - n, "on_after", after);
    n += snprintf(buf + n, sizeof(buf) - n, ",\"rejoin_s\":%.1f}", rejoin);
    if (n > 0 && n < (int)sizeof(buf)) {
        bool ok = mqtt.publish(TOPIC_TEST, (const uint8_t*)buf, n, true);
        Serial.printf("[test] result %s: %s\n", ok ? "published" : "NOT published", buf);
    }
}

void flushPendingLightning() {
    if (!pendingLightning.set || !mqtt.connected()) return;
    char buf[200];
    snprintf(buf, sizeof(buf),
        "{\"event\":\"lightning\",\"distance\":%u,\"energy\":%lu,\"timestamp\":\"%s\",\"delayed\":true}",
        pendingLightning.distance, (unsigned long)pendingLightning.energy, pendingLightning.ts);
    if (mqtt.publish(TOPIC_EVENT, buf)) {
        publishLastEvent("lightning", pendingLightning.distance, pendingLightning.energy);
        pendingLightning.set = false;
        Serial.println("[as3935] held lightning event published");
    }
}

// ── Watchdog ─────────────────────────────────────────────────────────────
void checkPublishWatchdog() {
    if (lastSuccessfulPublishMs == 0) return;  // not armed until first success
    if (millis() - lastSuccessfulPublishMs > PUBLISH_WDT_MS) {
        Serial.println("[wdt] no successful publish in 10 min — restarting");
        restartWith("publish_wdt");
    }
}

// ── Lifecycle ────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.printf("\n[boot] vu2cpl-as3935-bridge %s\n", FIRMWARE_VERSION);
    resolveBootCause();
    Serial.printf("[boot] cause: %s\n", bootCause);

    // Suppress the WiFi driver's chatty warnings (CCMP-replay,
    // AUTH_FAIL retries, etc.). Real errors (E level) still print.
    esp_log_level_set("wifi", ESP_LOG_ERROR);

    loadTunables();
    Serial.printf("[nvs] nf=%u wdth=%u srej=%u tun_cap=%u mask=%d "
                  "min_lt=%u indoor=%d ps=%s vbat_off=%d evt_ivl=%us\n",
                  tun.nf, tun.wdth, tun.srej, tun.tun_cap, tun.mask_dist,
                  tun.min_num_lightning, tun.indoor, psName(tun.modem_sleep),
                  tun.vbat_offset_mv, tun.evt_interval_s);
    Serial.printf("[nvs] mqtt_user=%s mqtt_pass=%s\n",
                  mqttUser, mqttPass[0] ? "(set)" : "(empty)");

    checkBootButtonForReset();

    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    pinMode(PIN_AS3935_IRQ, INPUT);
    attachInterrupt(digitalPinToInterrupt(PIN_AS3935_IRQ), onAs3935Irq, RISING);

    // ADC characterisation runs before WiFi to keep it simple — eFuse Vref
    // doesn't need network. After this, readVbat_mV() is safe to call.
    vbatInit();

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
    if (lastSuccessfulPublishMs) markAppValidIfPending();

    Serial.printf("[loop] entering main loop, IRQ on GPIO%d\n", PIN_AS3935_IRQ);
}

void loop() {
    if (WiFi.status() != WL_CONNECTED) wifiWaitForReconnect();
    if (!mqtt.connected()) {
        mqttReconnects++;
        if (churn.record(millis())) {
            Serial.printf("[mqtt] %u reconnects within %lu min — restarting\n",
                          CHURN_MAX_RECONNECTS, (unsigned long)(CHURN_WINDOW_MS / 60000));
            restartWith("mqtt_churn");
        }
        mqttConnect();
        publishStatus("ready");
    }
    mqtt.loop();
    if (!appValidated && lastSuccessfulPublishMs) markAppValidIfPending();
    otaLoop();
    flushPendingLightning();
    if (wifiTestPendingSecs) {
        uint16_t secs = wifiTestPendingSecs;
        wifiTestPendingSecs = 0;
        runWifiOffTest(secs);
    }

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
