# TODO — vu2cpl-as3935-bridge

Living list of open work, roughly in priority order. Full
rationale + context for each item lives in
[`HANDOVER.md`](HANDOVER.md) — this file is a scannable index so
future-Manoj (or a fork) can see "what's left" at a glance.

Newly-completed items are moved to **Recently resolved** at the
bottom (they're not just deleted so the work log stays intact).

---

## Near-term operational (bench + install)

- [ ] **Modem-sleep current-draw bench measurement.** Expected
  ~30–50 mA average with `modem_sleep=max` vs ~100–200 mA
  measured at v0.1.1. Needs a bench DMM in series with VIN
  averaged over a few minutes. Validates the v0.3.0+ power
  budget for outdoor operation.
- [ ] **Seal both plastic enclosures.** IP65 gasket + cable
  glands on the control box + sensor box + inter-box harness.
  Two-box topology is validated for behaviour
  ([`WIRING.md § Outdoor deployment`](WIRING.md#outdoor-deployment--two-box-topology-required));
  physical seal is separate work.
- [ ] **Field-mount at target location.** Currently on the
  bench in the outdoor-verified two-box config. Target: shaded
  wall / roof-eave (per WIRING § Where to mount), with the
  solar panel in sun via long cable if needed. Post-mount,
  re-run `calibrate_tun_cap` at the new location (parasitic
  capacitance will shift again).

## Cross-repo — vu2cpl-shack

- [ ] **Shack-side Telegram alert on low `vbat_mv`.** Debounced
  low-voltage warning + self-clearing recovery message. Full
  brief spawned as a background task in FleetView; picks up
  from the bridge's `lightning/as3935/hb` topic. Skips alerts
  when `vbat_mv < 500` (visual cue that the divider isn't
  wired, not a real low-battery event).

## v0.3.1 firmware (small)

- [ ] **OTA updates (ArduinoOTA).** Highest-value firmware
  addition post-field-mount. Climbing to the sealed enclosure
  to USB-flash is the failure mode this eliminates. ArduinoOTA
  on the existing WiFi connection is the simplest path — no
  changes to the MQTT surface, just a background task that
  listens for OTA pushes. Open Q C (v0.1.0) / Open Q F (v0.2.0)
  in HANDOVER.

## v0.4.0 firmware (deep-sleep milestone)

- [ ] **Deep-sleep + EXT0 wake on AS3935 IRQ.** Power budget
  changes from ~150 mA average to ~5 mA — the difference between
  "runs 3 days on 18650 in monsoon overcast" and "runs 2 weeks."
  Wakes on real strikes via `esp_sleep_enable_ext0_wakeup
  (PIN_AS3935_IRQ, HIGH)`. Open Q E in HANDOVER.
- [ ] **Heartbeat cadence stretch when in deep-sleep.** Current
  30 s cadence is fine while WiFi is always-on; wakes the radio
  every 30 s in deep-sleep mode which defeats the purpose.
  Stretch to 60–120 s or make it wake-only-on-event. Open Q B
  in HANDOVER.

## Design questions (unscheduled — decide before building)

- [ ] **Boot-button overload for TUN_CAP recalibration.** Currently
  `BOOT` held 3 s → erase WiFi creds + portal. Could overload
  with a 5 s hold → run `calibrate_tun_cap` locally, so the
  sealed box is field-recalibratable without MQTT. Open Q H.
- [ ] **Expose raw AS3935 registers via a diagnostic status
  mode.** Currently status carries human-friendly `nf`, `wdth`,
  `srej`. A diag mode (opt-in via `{"set":"diag","value":true}`?)
  could also report raw `REG_CFG0/1/2` values for deep debugging.
  Open Q G.
- [ ] **Onboard LED status pattern.** A single LED with blink
  patterns (WiFi up / MQTT up / last event age) helps field
  debugging without needing a laptop. Cheap to add. Open Q D
  (v0.1.0).

---

## Recently resolved

Newest first. Kept short so the file stays a work log, not a
graveyard.

- [x] **v0.3.0 field-stable** — 2026-06-29 · 24 h two-box outdoor
  watch passed clean. README + HANDOVER updated in commit
  [`be38db7`](https://github.com/vu2cpl/vu2cpl-as3935-bridge/commit/be38db7).
- [x] **Two-box outdoor topology validated + documented** —
  2026-06-28 · WIRING.md now has "Outdoor deployment — two-box
  topology (required)" as the canonical reference. Commit
  [`8705f69`](https://github.com/vu2cpl/vu2cpl-as3935-bridge/commit/8705f69).
- [x] **Battery divider soldered, DMM-cross-checked, offset
  trimmed** — 2026-06-28 · `vbat_offset_mv = -56` (chip-side
  eFuse Vref delta of 56 mV against a 4.20 V DMM reading).
  Persisted to NVS, survives reboot.
- [x] **TUN_CAP re-calibrated for sensor box** — 2026-06-28 ·
  `calibrate_tun_cap` action run after the AS3935 board moved
  into its own plastic enclosure.
- [x] **v0.3.0 shipped: battery voltage telemetry** — 2026-05-17 ·
  firmware + WIRING + dashboard + comprehensive test plan Phase 1b.
  Commit [`8084ad9`](https://github.com/vu2cpl/vu2cpl-as3935-bridge/commit/8084ad9).
