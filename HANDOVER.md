# Session Handover — vu2cpl-as3935-bridge

**Operator:** Manoj VU2CPL · MK83TE · Bengaluru
**Started:** 2026-05-11

---

## Status

**Planning.** Hardware on hand, no firmware code yet. This handover
captures the design decisions made during the 2026-05-11 evening
planning session in `vu2cpl-shack` (closes-out conversation around
HANDOVER #1 over there).

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

*73 de VU2CPL*
