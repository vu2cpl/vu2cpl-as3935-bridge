# Session Handover — vu2cpl-as3935-bridge

**Operator:** Manoj VU2CPL · MK83TE · Bengaluru
**Started:** 2026-05-11

---

## Status

**v0.2.0 live on the bench, 2026-05-12.** Firmware now exposes the
full AS3935 register surface as a runtime MQTT control channel
(`lightning/as3935/cmd` + `cmd/ack`), NVS-persisted across reboots,
range-validated per key. On-device TUN_CAP calibration runs as a
`cmd` action — no re-flash, no enclosure-opening required for
post-install re-tune. WiFi modem sleep enabled by default
(`WIFI_PS_MAX_MODEM`). MQTT reconnect bounded + no-publish watchdog
(10 min) → `ESP.restart()`. See [`CHANGELOG.md`](CHANGELOG.md).

The Node-RED **AS3935 Control Panel** is live on `noderedpi4` since
2026-05-12 — a single `ui_template` (group `as3935_ctl_grp`, flow tab
`fe70cfdcdfa19aa4` in vu2cpl-shack) wired to one mqtt-out for cmds and
three mqtt-ins for status / hb / ack. Provides NF / WDTH / SREJ /
TUN_CAP / Mask dist / AFE GB / Min strikes / Modem sleep knobs and the
four actions. Styled to the GitHub-dark palette used by the rest of the
shack dashboard. Source HTML/CSS/JS in
[`nodered/build-flow.py`](nodered/build-flow.py); generated flow JSON
in [`nodered/as3935-control-flow.json`](nodered/as3935-control-flow.json).

**Downstream consumer:** [`vu2cpl-shack`](https://github.com/vu2cpl/vu2cpl-shack)'s
`Trigger Disconnect` (also rebuilt 2026-05-12) now uses this bridge's
`lightning/as3935` events as the primary strike source for a 3×3
distance-graded decision matrix (AS3935 close/medium/far × Open-Meteo
cold/lit/severe). Strikes from this firmware are the *only* thing that
fires the disconnect chain — Open-Meteo became a corroboration signal,
not a trigger. Documented in `vu2cpl-shack/CLAUDE.md` "Lightning
Antenna Protector → Distance-graded disconnect".

**Bench-verified end-to-end** (v0.1.1 onwards, still holds at v0.2.0):
piezo-lighter sparks produce `event:"disturber"` events on
`lightning/as3935`, counters increment correctly, status republishes
every 5 min, hb publishes every 30 s, ack publishes per command, and
the shack flow's Lightning Antenna Protector consumes everything
unchanged. Indoor Pi daemon `as3935.service` on `noderedpi4` stopped
and disabled — the ESP32 is the sole publisher.

**Previous milestone — v0.1.1, 2026-05-11:** AS3935 wired, calibrated
(`TRCO=OK SRCO=OK`), I²C solid at `0x03`, MQTT pipe live to the shack
broker, WiFi auto-rejoins via WiFiManager-saved creds. Retained status
refreshes every 5 min.

**Outstanding for v0.3.0+:**
- Verify modem-sleep current drop on the bench (expected ~30-50 mA avg
  vs the 100-200 mA measured at v0.1.1).
- Power chain (TP4056 + 18650 + solar), with **panel mounted in sun**
  even if enclosure is in shade (long cable).
- Enclosure seal, field install with in-situ TUN_CAP re-tune (now
  reachable from the shack via `{"action":"calibrate_tun_cap"}` — no
  enclosure-opening needed).
- Eventual deep-sleep + EXT0 wake on AS3935 IRQ (~10 µA between events).
- OTA updates (ArduinoOTA) so a sealed-box re-flash doesn't require
  USB access.

See [`CHANGELOG.md`](CHANGELOG.md) for the version-by-version log and
2026-05-11/12 bring-up gotchas.

---

## Decisions taken

| # | Decision | Reason |
|---|----------|--------|
| 1 | MCU = **ESP-WROOM-32**, not ESP8266 | EXT0 wake-on-GPIO with deep-sleep, dual-core, better WiFi stability, more flash for OTA |
| 2 | Framework = **PlatformIO + Arduino** | Same toolchain as common AS3935 / PubSubClient libraries; cross-platform on Mac |
| 3 | Power topology = **solar → TP4056 → 18650 → ESP32** | Standard cheap chain, no exotic ICs; TP4056 handles 5 V solar input and Li-ion charge/protect (use BMS-equipped TP4056 variant) |
| 4 | AS3935 stays powered continuously | Avoids losing calibration on each ESP wake; chip draws microamps idle, negligible budget impact |
| 5 | WiFi = **always-on** in v0.1.0, deep-sleep added in a later iteration | Simpler firmware to ship first; 18650 + solar covers always-on draw with ~20+ h standalone runtime even without sun |
| 6 | **MQTT contract identical** to old `as3935_mqtt.py` | Zero Node-RED changes; flow tab `Lightning Antenna Protector` keeps working as-is |
| 7 | `AFE_GB = 0x1C` (outdoor mode) | Indoor `0x12` was a workaround for the indoor location; outdoor regains rated range |
| 8 | `TUN_CAP` re-sweep on the new physical layout | Stray capacitance shifts; port `as3935_tune.py` sweep logic into a firmware calibration mode |
| 9 | Enclosure mounted in **shade** | Sealed boxes in direct Bengaluru sun reach 60-70 °C; Li-ion degrades fast above 45 °C and won't charge above ~45 °C |
| 10 | NF=4 production noise floor | Same as the pre-existing Python daemon's tuned default |

---

## Open questions

| # | Question | Notes |
|---|----------|-------|
| A | Antenna mounting orientation | The AS3935 ferrite antenna is directional; vertical orientation typically gives best omnidirectional reception, but worth a one-day test post-install |
| B | Heartbeat interval | Python daemon used 30 s. With WiFi always-on, can keep at 30 s. If we move to deep-sleep mode later, may want to stretch to 60-120 s |
| C | OTA updates | Worth wiring in early — climbing on the roof to re-flash via USB is painful. ESP32 has `ArduinoOTA` built in. Defer to v0.2.0? |
| D | Onboard LED status | A bicolor or single LED with blink-pattern status (WiFi up, MQTT up, last event) helps field debugging; cheap to add |

---

## Next steps

1. **Wire it up on the bench first** — ESP32 + AS3935 on breadboard, USB power. Get the I²C link working before adding the battery/solar layer.
2. **Get WiFi + MQTT connecting** — to `192.168.1.169:1883`. Verify `mosquitto_sub -t lightning/as3935 -v` on the Pi sees a heartbeat.
3. **Match the MQTT contract** — first thing the flow will see when the new firmware comes up. Cross-check against
   `vu2cpl-shack/as3935_mqtt.py` for payload shape, retained flags, LWT semantics.
4. **Port the `as3935_tune.py` sweep** — TUN_CAP 0..15, settle 100 ms, count edges from the ANT pin via INT trigger mode 3 (LCO output on IRQ pin), pick the best. Run it once after the final physical install.
5. **Bench test the wake-on-IRQ path** even if v0.1.0 is always-on — proves the IRQ wiring works.
6. **Build the power chain** — TP4056 + 18650 + ESP32, verify the ESP starts on battery alone and that the panel tops up under simulated sun (or an actual sunny day on the bench).
7. **Field install** — fix antenna orientation, seal the enclosure, mount in shade. Re-tune TUN_CAP after install (stray cap changes when the box closes).

---

## Lessons inherited from the indoor Python daemon

The `as3935_mqtt.py` daemon in `vu2cpl-shack` repo evolved over a few
years and learned several things the hard way. Port the lessons into
firmware:

- **CALIB_RCO at startup** — the AS3935's internal RCO needs explicit
  calibration after power-up. Without it, all subsequent timing is
  off and false noise events flood the IRQ. Datasheet sequence is
  "CALIB_RCO command, wait 2 ms, read REG 0x3A/0x3B for results".
- **INT register flush** after every config write — a stale interrupt
  bit can mask the next real event.
- **Heartbeat carries useful diagnostics** — Python daemon's status
  message includes `tun_cap`, `irq_pin`, `calib_trco`, `calib_srco`,
  uptime. Reproduce in firmware.
- **LWT is critical** — the Node-RED dashboard goes "offline" only
  when the broker sees the LWT. Without it, a silent sensor looks
  identical to a quiet day. Set LWT on connect: topic
  `lightning/as3935/lwt`, payload `"offline"`, retained.
- **AS3935 distance `63` means "out of range"** — the Node-RED flow
  treats it as "always disconnect" (storm overhead). Firmware should
  publish `63` as-is and let the flow keep its semantics.

---

## Source of truth for the MQTT contract

The Python daemon `as3935_mqtt.py` in
[`vu2cpl-shack`](https://github.com/vu2cpl/vu2cpl-shack) repo.
Cross-reference its publish calls + payload shapes when implementing
the firmware. The Node-RED tab that consumes the MQTT topics is
`Lightning Antenna Protector` (id `75e2cac8ab96f556`), `Parse AS3935`
function (id `0a664ba977970e17`).

---

## 2026-05-12 — v0.2.0 shipped: cmd channel, NVS persistence, TUN_CAP sweep

### What landed

The firmware grew a runtime control surface so the indoor / outdoor
gain decision (and every other AS3935 tunable) can be flipped without
a re-flash. Builds on v0.1.1's bench bring-up.

**New MQTT topics:**

| Topic                            | Direction | Notes |
|----------------------------------|-----------|-------|
| `lightning/as3935/cmd`           | Node-RED → ESP32 | JSON command in. Single-message dispatch — no batching. |
| `lightning/as3935/cmd/ack`       | ESP32 → Node-RED | Per-message ack. Not retained — failure detail (`error` field) only useful in the moment. |
| `lightning/as3935/status`        | ESP32 → Node-RED | Existing topic; v0.2.0 now republishes it after every successful `set`/`action` so subscribers see fresh state immediately. |

**Command payload shapes:**

```jsonc
// Tunables — written through to AS3935 register, then to NVS
{"set": "nf",                "value": 0..7}
{"set": "wdth",              "value": 0..15}
{"set": "srej",              "value": 0..15}
{"set": "tun_cap",           "value": 0..15}
{"set": "mask_dist",         "value": true|false}
{"set": "min_num_lightning", "value": 1|5|9|16}     // datasheet-quantised
{"set": "afe_gb",            "value": "indoor"|"outdoor"}  // string enum, not raw hex
{"set": "modem_sleep",       "value": "max"|"min"}

// Actions — side effects, no value
{"action": "republish_status"}
{"action": "calibrate_tun_cap"}   // ~35 s sweep, MQTT kept alive inside
{"action": "reboot"}
{"action": "factory_reset_wifi"}
```

**Ack shape:**

```jsonc
{"ok": true,  "cmd": "set:afe_gb", "ts": "2026-05-12T..."}
{"ok": false, "cmd": "set:afe_gb", "error": "afe_gb must be 'indoor' or 'outdoor'", "ts": "..."}
```

Every `set` succeeds → ack + status republish, in that order. Every
failed `set` → ack only (no register write, no NVS write, no status).

### Decisions taken in v0.2.0

| # | Decision | Reason |
|---|----------|--------|
| 11 | **NVS-backed tunables**, defaults in `struct Tunables tun` overridden by `Preferences` on boot | Settings survive reboot; deep-sleep iteration in the future doesn't need a different persistence layer |
| 12 | **String enums on the wire** for `afe_gb` (`"indoor"`/`"outdoor"`) and `modem_sleep` (`"max"`/`"min"`) | Node-RED operators shouldn't need to know `AFE_GB = 0x0E` vs `0x12`; firmware translates to register bits |
| 13 | **Range-validated**, per-key, in `handleSet` | A bad command publishes a descriptive ack failure rather than corrupting the chip's register |
| 14 | **Republish retained `status` after every successful command** | UIs that just subscribe to `status` get the current state without needing to interpret acks |
| 15 | **`cmd/ack` is NOT retained** | Late-attaching subscribers shouldn't see stale per-command success/failure of someone else's command; for "current persistent state," subscribe to `status` instead |
| 16 | **TUN_CAP calibration is an `action`, not a tunable** | It's a procedure with a side-effecting result (whatever value won the sweep), not a value the operator sets directly. Result lands in `tun.tun_cap` + NVS, status republished |
| 17 | **MQTT keepalive pumped inside the calibration loop** | The sweep takes ~35 s. Without `mqtt.loop()` calls in the sample wait, PubSubClient drops the connection on the broker's keepalive timeout, and the ESP32 reboots before publishing the result |
| 18 | **Boot-time `loadTunables()` happens before `as3935Init()`** | `as3935Init()` reads `tun.*` to apply settings — wrong order would write the defaults to the chip and then silently desync from NVS until the next command |
| 19 | **WiFi log level pinned to `ESP_LOG_ERROR`** | Chatty CCMP-replay / AUTH_FAIL info noise drowns out the genuinely interesting log lines |
| 20 | **No-publish watchdog: 10 min without a successful publish → `ESP.restart()`** | Field-deploy failure mode: WiFi reconnects but MQTT silently doesn't. Restart is the only reliable recovery from "looks fine, isn't publishing" |
| 21 | **MQTT fail counter: 60 × 5 s = 5 min of connect failures → `ESP.restart()`** | Bounded retry, avoid the "wedged forever" mode |

### Open questions / for v0.3.0+

| # | Question | Notes |
|---|----------|-------|
| E | Sleep-on-IRQ mode for battery operation | v0.1.0/0.2.0 keeps WiFi always-on. Once the sensor is on solar+18650 outdoors and `as3935-bridge` is the only thing running on the ESP32, `esp_sleep_enable_ext0_wakeup(PIN_AS3935_IRQ, HIGH)` lets the chip wake on real strikes. Power budget changes from ~150 mA average to ~5 mA. Needs a re-think of heartbeat cadence (don't wake just to publish "I'm fine"). |
| F | OTA updates (Q-C from v0.1.0) | Still open. With NVS persistence settled, OTA is the next step — climbing on the roof to USB-flash a sealed box is the obvious motivation. ArduinoOTA on the same WiFi is the simplest path |
| G | Whether to expose `noise_floor_raw` / per-event metadata on the status topic | Currently status carries human-friendly `nf`, `wdth`, `srej`. A diagnostic mode could also report `REG_CFG0/1/2` raw values for debugging |
| H | Boot-button mapping during operation | Currently `BOOT` held 3 s → erase WiFi creds + portal. Could overload it to also trigger TUN_CAP calibration with a different hold duration (e.g. 5 s) so the box is field-recalibratable without MQTT |

### Lessons learned during the v0.2.0 build

- **`as3935Modify(reg, mask, bits)`** as a primitive turned out to be a force multiplier — every parameter writer is one line. Avoids the bug class of "I forgot to preserve the other bits in this byte."
- **ArduinoJson v7 `JsonDocument`** (no template size) is the right call for variable-size payloads; the v6-style `StaticJsonDocument<N>` doesn't compose with optional fields.
- **PubSubClient's 256-byte default buffer is too small** for the status payload (which has ~12 fields). `mqtt.setBufferSize(512)` fixes silent dropped publishes. The failure mode is `publish()` returning true but the broker seeing nothing — easy to miss.
- **NVS namespace must be opened read-only when only reading** (`p.begin(NS, true)`) and read/write when writing (`p.begin(NS, false)`). Mixing causes silent write failures on the first commit after a fresh flash.
- **First-ever NVS commit on a fresh namespace takes ~1 s longer than steady-state.** When a command path ends with `ESP.restart()` (the `reboot` action), the firmware needs `delay(1500)` after `publishAck` to let NVS flush before the restart hits.
- **`calibrateTunCap()` must `detachInterrupt` the normal AS3935 ISR first** and re-attach at the end. Otherwise the high-rate LCO edges trigger `handleAs3935Event()` on every pulse and the I²C bus is hammered with status reads that compete with the calibration's own reads.

### Source of truth (unchanged from v0.1.x)

Node-RED Lightning Antenna Protector flow (`vu2cpl-shack` repo, tab id
`75e2cac8ab96f556`) is the contract consumer. Any rename of
`status` / `hb` / `event` keys breaks the dashboard — extend, don't
rename. `cmd` / `cmd/ack` are new in v0.2.0 and only consumed by
admin-side widgets, so additions there are safer.

---

*73 de VU2CPL*
