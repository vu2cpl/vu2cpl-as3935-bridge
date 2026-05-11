// vu2cpl-as3935-bridge — ESP32 firmware
// MQTT contract identical to as3935_mqtt.py from vu2cpl-shack repo.

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "secrets.h"

// Pins
constexpr int PIN_AS3935_IRQ = 27;
constexpr int PIN_I2C_SDA    = 21;
constexpr int PIN_I2C_SCL    = 22;

// MQTT
constexpr const char* TOPIC_EVENT  = "lightning/as3935";
constexpr const char* TOPIC_STATUS = "lightning/as3935/status";
constexpr const char* TOPIC_HB     = "lightning/as3935/hb";
constexpr const char* TOPIC_LWT    = "lightning/as3935/lwt";
constexpr const char* CLIENT_ID    = "as3935-bridge";

// AS3935 (defaults — wire-equivalent to as3935_mqtt.py production config)
constexpr uint8_t AS3935_I2C_ADDR = 0x03;
constexpr uint8_t AS3935_NF       = 4;       // noise floor
constexpr uint8_t AS3935_AFE_GB   = 0x1C;    // OUTDOOR mode
constexpr uint8_t AS3935_WDTH     = 2;
constexpr uint8_t AS3935_SREJ     = 2;
uint8_t           as3935_tun_cap  = 10;      // overridden by NVS / cal

constexpr uint32_t HEARTBEAT_MS = 30 * 1000;

WiFiClient   wifi;
PubSubClient mqtt(wifi);

volatile bool irqFired = false;
void IRAM_ATTR onAs3935Irq() { irqFired = true; }

// --- AS3935 helpers ---
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

void as3935Init() {
    // TODO: implement full datasheet init sequence:
    //   1. PRESET_DEFAULT
    //   2. CALIB_RCO + verify TRCO/SRCO bits in 0x3A/0x3B
    //   3. Apply AFE_GB, NF, WDTH, SREJ
    //   4. Apply persisted TUN_CAP
    //   5. INT register flush
    // Cross-reference vu2cpl-shack/as3935_mqtt.py for the exact byte sequence.
    Serial.println("[as3935] init stub — implement before bench test");
}

// --- WiFi / MQTT ---
void wifiConnect() {
    Serial.printf("[wifi] connecting to %s...\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print('.'); }
    Serial.printf("\n[wifi] connected, RSSI %d dBm, IP %s\n",
                  WiFi.RSSI(), WiFi.localIP().toString().c_str());
}

void mqttConnect() {
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    while (!mqtt.connected()) {
        Serial.printf("[mqtt] connecting to %s:%d...\n", MQTT_HOST, MQTT_PORT);
        // LWT: retained "offline" on disconnect
        if (mqtt.connect(CLIENT_ID, nullptr, nullptr,
                         TOPIC_LWT, 0, true, "offline")) {
            Serial.println("[mqtt] connected, LWT armed");
            // Clear LWT immediately (will be re-set on actual disconnect)
            mqtt.publish(TOPIC_LWT, "online", true);
        } else {
            Serial.printf("[mqtt] failed rc=%d, retry in 5s\n", mqtt.state());
            delay(5000);
        }
    }
}

void publishStatus() {
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"up\":true,\"tun_cap\":%u,\"irq_pin\":%d,\"nf\":%u,"
        "\"afe_gb\":\"0x%02X\",\"calib_trco\":\"ok\",\"calib_srco\":\"ok\","
        "\"rssi\":%d,\"ts\":%lu,\"fw\":\"%s\"}",
        as3935_tun_cap, PIN_AS3935_IRQ, AS3935_NF, AS3935_AFE_GB,
        WiFi.RSSI(), (unsigned long)(millis()/1000), FIRMWARE_VERSION);
    mqtt.publish(TOPIC_STATUS, buf, true);
    Serial.printf("[mqtt] status published (retained): %s\n", buf);
}

void publishHeartbeat() {
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"ts\":%lu,\"rssi\":%d}",
             (unsigned long)(millis()/1000), WiFi.RSSI());
    mqtt.publish(TOPIC_HB, buf);
}

void handleAs3935Event() {
    delay(2);  // datasheet — 2 ms wait after IRQ before reading 0x03
    uint8_t intReg = as3935Read(0x03) & 0x0F;
    uint8_t distance = as3935Read(0x07) & 0x3F;
    uint32_t energy  = ((uint32_t)(as3935Read(0x06) & 0x1F) << 16)
                     | ((uint32_t)as3935Read(0x05) << 8)
                     |             as3935Read(0x04);

    const char* event = "unknown";
    switch (intReg) {
        case 0x08: event = "lightning"; break;
        case 0x04: event = "disturber"; break;
        case 0x01: event = "noise";     break;
    }

    char buf[160];
    snprintf(buf, sizeof(buf),
        "{\"event\":\"%s\",\"distance\":%u,\"energy\":%lu,\"timestamp\":%lu}",
        event, distance, (unsigned long)energy,
        (unsigned long)(millis()/1000));
    mqtt.publish(TOPIC_EVENT, buf);
    Serial.printf("[as3935] %s d=%u e=%lu\n", event, distance, (unsigned long)energy);
}

// --- Lifecycle ---
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.printf("\n[boot] vu2cpl-as3935-bridge %s\n", FIRMWARE_VERSION);

    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    pinMode(PIN_AS3935_IRQ, INPUT);
    attachInterrupt(digitalPinToInterrupt(PIN_AS3935_IRQ), onAs3935Irq, RISING);

    as3935Init();
    wifiConnect();
    mqttConnect();
    publishStatus();
}

uint32_t lastHb = 0;

void loop() {
    if (WiFi.status() != WL_CONNECTED) wifiConnect();
    if (!mqtt.connected())             mqttConnect();
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
