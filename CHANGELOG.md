# Changelog — vu2cpl-as3935-bridge

All notable firmware changes, newest first. Versions tagged via
`-DFIRMWARE_VERSION='"vX.Y.Z"'` in `platformio.ini` and surfaced in
the `fw` field of every `lightning/as3935/status` MQTT payload.

---

## v0.4.3 — 2026-10-14

Two safeguards for the one failure v0.4.1's self-heal can't see: lightning
detection stalling while MQTT and the heartbeat look healthy. Both disturber
counter freezes (2026-10-08, 2026-10-14) were this kind of stall. v0.4.2's
WiFi-off test showed the disturbers keep coming with the radio off, so a
frozen counter means the firmware stopped servicing the IRQ, not a quiet
source. No daily reboot (Manoj's call): these act in seconds, not up to 24 h.

### Stuck IRQ line

- The ISR fires on the RISING edge, and the AS3935 holds IRQ high until
  register 0x03 is read. A missed or failed read leaves the line latched with
  no new edge, ever. Now, if the line has been high for > 1 s with no ISR
  flag, the firmware reads 0x03 (handling the waiting event) and the line
  drops. Counted as **`irq_unlatch`** in the heartbeat.

### IRQ silence watchdog

- The deployed unit sees a steady ~400 IRQs/min. A per-minute EWMA of the IRQ
  count arms the watchdog only while it is >= 20/min. **5 min with no IRQ ⇒
  re-init the AS3935** (tunables, `CALIB_RCO`, clear INT; counted as
  **`as3935_reinit`**); **still none 5 min later ⇒ restart**
  (`boot_cause: irq_silence`). The restart needs a rate of roughly
  190/min or more before the silence (the EWMA decays while it waits); slower
  sensors only get the re-init.
- No restart loops: after a restart the EWMA starts at 0, so a sensor that is
  genuinely silent stays disarmed until IRQs flow again. Any `set` command and
  `calibrate_tun_cap` reset the watchdog, because they may change the rate on
  purpose (e.g. `mask_dist`).

### Build / test

- Flash 89.2% (1,169,365 bytes). `StuckLineWatch` and `SilenceWatch` live in
  `src/bridge_util.h`; the host test grows to 24 checks (normal IRQ pulses
  never trip; > 1 s latched does; steady 400/min never acts; silence ⇒ re-init
  at 5 min, restart at 10; IRQs resuming cancel the restart; a fresh watch on a
  silent sensor never acts in 3 h; 5/min with hour-long gaps never arms; the
  millis() wrap).

---

## v0.4.2 — 2026-10-14

A test to settle what causes the steady ~400/min disturbers. The v0.4.1
A/B/A test showed the per-event MQTT traffic isn't it (397.3/min throttled
vs 397.4/min publishing every event); the remaining suspect is the ESP32's
WiFi radio itself.

### Disturber test

- **`{"action":"wifi_off_test","secs":60}`** (10–120 s, default 60). Runs
  from `loop()` (not inside the MQTT callback): counts disturbers for `secs`
  with WiFi on, disconnects MQTT cleanly (so no LWT, the dashboards don't
  see "offline"), switches the radio fully off (`WiFi.mode(WIFI_OFF)`) for
  `secs`, rejoins with the stored credentials, counts `secs` more, and
  publishes, **retained**, on the new topic **`lightning/as3935/test`**:
  `{test:"wifi_off", ts, fw, secs, on_before:{disturber, noise, lightning,
  secs, per_min}, off:{…}, on_after:{…}, rejoin_s}`. Offline for about
  `secs` + 5 s. Heartbeats keep going in the two on windows.

### Lightning while offline

- A lightning event whose publish fails (MQTT down: the test's off window,
  or a flap) is held and published as soon as MQTT is back, with
  `"delayed":true`. Only the latest one is held; the counters count all.

### Build / test

- Flash 89.2% (1,168,709 bytes). Host check of the result message:
  valid JSON, 357 bytes with every counter at its maximum (MQTT buffer 512).

---

## v0.4.1 — 2026-10-14

Self-healing and a much quieter event stream, after two outages
(2026-10-08→10, 2026-10-14) where the MQTT link flapped every ~23 s for
hours and only a manual `reboot` command fixed it. Plus OTA, so later
fixes don't need a ladder. **On the deployed unit since 2026-10-14
13:54** (USB, since v0.4.0 has no OTA); an `./ota.sh` re-push at 13:55
came back with `boot_cause: ota`.

### Self-heal

- **5 MQTT reconnects inside 10 min ⇒ restart** (`boot_cause:
  mqtt_churn`). The existing watchdog only fires after 10 min with no
  *successful* publish, and while the link flaps, upstream publishes
  keep succeeding, so it never fired. At the flapping cadence the new
  check restarts after ~90 s instead of waiting for a human.
- **`boot_cause` in status**: why the last restart happened. Hardware
  reasons (`power_on`, `brownout`, `panic`, `task_wdt`, …) come from
  `esp_reset_reason()`; the firmware's own restarts (`mqtt_churn`,
  `publish_wdt`, `mqtt_fail`, `wifi_timeout`, `cmd_reboot`,
  `cmd_wifi_reset`, `portal`, `portal_timeout`, `ota`) leave their
  reason in RTC memory via the new `restartWith()`.
- Heartbeat gains `mqtt_reconnects` (since boot).

### Events

- **Disturber and noise events are throttled**: one publish per
  `evt_interval_s` (new NVS tunable, default 60, range 0–3600; `0` =
  every event, the v0.4.0 behaviour). The publish carries `count`, the
  number of events it stands for. Lightning is never throttled.
  Disturbers had been running at ~6/s, i.e. ~12 MQTT publishes a second
  with `last_event`, keeping the radio transmitting almost nonstop.
- Heartbeat gains `rate_per_min:{disturber, noise}` (since the previous
  heartbeat), so the true rate stays visible with the throttle on.
- `last_event` follows the throttle (at most one disturber/noise update
  per interval).

### OTA

- `{"action":"ota_open"}` starts an HTTP update server on port 80 for
  10 min (re-sending extends it). Upload with basic auth, user `ota`,
  password = the MQTT password in NVS. Refused if no MQTT password is
  set. New `ota.sh` does the whole round trip.
- HTTP push rather than ArduinoOTA, because the IoT VLAN can't open
  connections back to the uploader.
- **Rollback:** `verifyRollbackLater()` keeps a new image on probation
  until it has connected to MQTT and published its status; a restart
  before that makes the bootloader return to the previous image. The
  hook needs `extern "C"` to replace the core's weak C default (checked
  with `nm`: without it the core's version was linked and rollback would
  silently have done nothing).

### WiFi

- `modem_sleep` accepts `none` (`WIFI_PS_NONE`) besides `max` / `min`.
  New NVS key `ps_mode`; a unit upgraded from v0.4.0 keeps its `ps_max`
  setting, and `ps_max` is still written so a downgrade reads back
  something sensible.

### Build / test

- Built with the pioarduino platform 55.03.311 (Arduino core 3.3.11):
  Flash 89.0% (1,166,597 of 1,310,720 bytes), RAM 16.1%. The partition
  table already has two 1.25 MB app slots, so OTA needs no repartition.
- `src/bridge_util.h` holds the churn and throttle logic with no Arduino
  dependencies; `test/host/test_bridge_util.cpp` checks it on the host
  (14 checks: the 23 s flapping cadence, rare reconnects, the window
  edge, the millis() wrap, throttle counts).
- `ota.sh` tested against a mock HTTP server: basic auth from stdin with
  a password containing a space, `"` and `\`, multipart field `firmware`.

---

## v0.4.0 — 2026-08-21

MQTT authentication support. The shack broker (`192.168.1.169`) is
being migrated off anonymous access to per-role accounts with ACLs;
this bridge uses the ACL-restricted **`iot`** account. Until now the
firmware connected anonymously (`mqtt.connect(CLIENT_ID, nullptr,
nullptr, …)`) and would have been dropped the moment the broker set
`allow_anonymous false`.

### MQTT

- `mqttConnect()` now passes a username/password to `mqtt.connect()`.
  Empty username ⇒ anonymous connect (backward-compatible with a
  broker that still allows anonymous), so a device flashed with
  v0.4.0 but not yet provisioned keeps working until the broker flips.
- The connect log line now shows the account:
  `[mqtt] connecting to …:1883 as iot (attempt …)`.

### Provisioning (`WiFiManager` portal)

- Two new captive-portal fields — **MQTT username** and **MQTT
  password** — shown alongside the WiFi fields in both portal paths
  (`wifiSetup()` auto-portal and the BOOT-hold re-provision in
  `checkBootButtonForReset()`).
- Username field defaults to `iot`; the **password is entered in the
  portal and never compiled into the firmware or committed to git**
  (keeps the repo's no-secrets-in-source convention).
- Values persist in NVS (namespace `as3935`, keys `mqtt_user` /
  `mqtt_pass`) and reload on boot; only re-saved when the operator
  actually changes a field.

### Operational

- **Requires a USB re-flash** (no OTA in this firmware): `./flash.sh`,
  then hold BOOT to open the portal and enter the `iot` password.
  Boot log prints `[nvs] mqtt_user=iot mqtt_pass=(set)` to confirm.

### Firmware (`src/main.cpp`)

- New NVS keys `NVS_K_MQTT_USER` / `NVS_K_MQTT_PASS`; `mqttUser[64]` /
  `mqttPass[64]` buffers loaded in `loadTunables()`.
- New `saveMqttCreds()`, `attachMqttParams()`,
  `commitMqttParamsIfChanged()` helpers.

---

## v0.3.0 — 2026-05-17

Battery voltage telemetry over MQTT. Required for outdoor deploy —
the operator needs to see battery state from the shack dashboard
without opening the sealed enclosure.

### Hardware mod (new)

External 1:2 resistive divider feeding ADC1_CH6 (GPIO 34):

- R1 = 100 kΩ from TP4056 OUT+ to GPIO 34
- R2 = 100 kΩ from GPIO 34 to GND
- C1 = 100 nF ceramic at the pin (S/H impedance shore-up)
- Bleed current 21 µA at 4.2 V (~0.5 mWh/day, trivial)

Full schematic + BOM + tap-point rationale (why OUT+ not B+) in
[`WIRING.md`](WIRING.md). Without the divider the firmware still
runs but reports ~0 V — visual cue the mod isn't done.

### MQTT surface additions

- `lightning/as3935/hb` now includes `"vbat_mv":NNNN` (every 30 s).
- `lightning/as3935/status` now includes `"vbat_mv":NNNN` and
  `"vbat_offset_mv":SS` (on connect + every 5 min).
- New `cmd` action `{"action":"query_vbat"}` — fresh one-shot
  reading. Acks with `cmd:"action:query_vbat vbat_mv=NNNN"` and
  republishes status for atomic dashboard update.
- New `cmd` tunable `{"set":"vbat_offset_mv","value":-500..500}` —
  per-chip Vref trim, persisted to NVS under key `vbat_off_mv`.

### Firmware (`src/main.cpp`)

- New `vbatInit()` calls `esp_adc_cal_characterize()` once at boot;
  logs the calibration source (eFuse-Vref / eFuse-two-point /
  default-Vref fallback).
- New `readVbat_mV()` averages 32 ADC samples, runs them through
  `esp_adc_cal_raw_to_voltage()`, multiplies by `VBAT_DIVIDER_RATIO`
  (= 2), adds NVS-trimmed `vbat_offset_mv`, clamps non-negative.
- `Tunables` struct gains `int16_t vbat_offset_mv = 0`; NVS load /
  save extended; `handleSet` gains case + range check; `handleAction`
  gains `query_vbat`.
- `publishHeartbeat` buffer grew 256 → 320 bytes to fit the new field.
- `vbatInit()` runs from `setup()` before WiFi (no network needed
  for eFuse Vref read).

### Dashboard (`nodered/build-flow.py`)

- New 🔋 row between Calib and Tunables: `🔋 X.XX V (≈ NN %)`,
  green ≥ 3.90 V, amber 3.70–3.90 V, red < 3.70 V. Shows
  `(divider not wired?)` when reading < 500 mV. Right edge shows
  `offset ±NN mV` when `vbat_offset_mv != 0`.
- New **Query Battery** action button next to Republish Status.
- SOC % computed client-side from a piecewise-linear LUT (firmware
  doesn't carry battery chemistry knowledge — easier to update one
  place when the chemistry changes).

### Why not a fuel-gauge IC

MAX17048 / DS2438 / similar would give true %SOC via coulomb
counting. Voltage-only is "good enough" for a sensor node — catches
the failure modes that matter (dead panel, cell drift, cold-weather
drop) without adding I²C parts to the bus. If field experience
shows it's misleading often, fuel-gauge IC is a clean drop-in
upgrade — wire changes are zero (sits on the existing I²C bus),
firmware reads SOC from the chip instead of doing ADC math.

### Build verification

`pio run` clean — no warnings, RAM 14.6 % (47964 / 327680), Flash
72.2 % (946021 / 1310720). Bench-flash + DMM cross-check is the
next physical step.

---

## Post-v0.2.0 — retained `last_event` topic for dashboard rehydration — 2026-05-14

New retained MQTT topic so the shack's Node-RED Master Dashboard can
show a correct "LAST SEEN" age on Node-RED restart without waiting
for the next live AS3935 event.

- New constant `TOPIC_LAST_EVENT = "lightning/as3935/last_event"`.
- New helper `publishLastEvent(event, distance, energy)` — emits
  `{event, distance, energy, timestamp, ts_epoch_ms}` with
  `retain=true`. `ts_epoch_ms = time(nullptr) * 1000` (the device's
  NTP-synced wall clock at publish time, in milliseconds since
  epoch — chosen instead of bridge `millis()` because the dashboard
  needs a stable reference point that survives ESP32 reboot too,
  and `time(nullptr)` already drives the existing ISO `ts` field).
- Three new call sites in `handleAS3935Irq()`: one after each of the
  existing `mqtt.publish(TOPIC_EVENT, …)` calls for lightning,
  disturber, and noise. Same `event` string, same distance/energy
  for lightning, `0/0` for disturber and noise (the chip's
  distance/energy registers for non-lightning events aren't
  physically meaningful; dashboard treats them accordingly).

**Why retained.** Broker stores the latest payload per topic when
`retain=true`. Any subscriber connecting after publication gets the
payload immediately on subscribe — so on Node-RED restart, the
shack dashboard's Master Dashboard re-paints LAST SEEN within
~100 ms instead of staying blank until the next disturber.

**Sequencing.** The shack repo (`vu2cpl-shack`) ships the consuming
mqtt-in + dashboard handler in commit `f75e147`. That side is a
silent subscriber until this firmware is flashed; this firmware
side is harmless until the shack side is deployed. Either order
works, both already on `origin/main`.

**Build / flash.** Verified to compile on this Mac (PlatformIO 6,
ESP32-WROOM-32). Flash with `./flash.sh` over USB to the live
bridge ESP32. Status topic's `fw` field still reads `v0.2.0` after
flash — no version bump for this addition, same rationale as the
2026-05-12 bench-refinements set above.

---

## Post-v0.2.0 bench refinements — 2026-05-12

Small follow-ups validated on the bench after v0.2.0 was deployed.
No firmware version bump — these are minor enough that bumping to
v0.2.1 would be noisy.

- **`esp_log_level_set("wifi", ESP_LOG_ERROR)`** in `setup()`.
  Hides the ESP32 WiFi driver's W-level chatter — CCMP-replay
  detections and the AUTH_FAIL / AUTH_EXPIRE retries during
  WiFiManager-stored-creds reconnect. Real errors still print.
- **Reboot delay bumped 500 ms → 1500 ms.** Discovered when the
  first-ever NVS commit after a fresh partition couldn't finish
  before `ESP.restart()` fired (test: set a tunable from default,
  reboot, observe the default get reloaded). Once the namespace
  exists, subsequent commits finish well under 500 ms — but the
  extra second is cheap insurance against full-flash-erase edge
  cases.
- **Node-RED tuning dashboard** added in
  [`nodered/as3935-control-flow.json`](nodered/as3935-control-flow.json).
  Single `ui_template` styled to match the existing Master
  Dashboard (GitHub-dark palette, vanilla DOM, LED indicator,
  rounded-card layout). Imports as a new flow into the shack's
  Node-RED, drops into the **Shack Monitoring tools** tab as the
  "AS3935 Tuning" group. Source HTML/CSS/JS is in
  [`nodered/build-flow.py`](nodered/build-flow.py) — edit there,
  re-run, re-import. See [`nodered/README.md`](nodered/README.md).

---

## v0.2.0 — 2026-05-12

Adds a full MQTT control surface for live tuning, on-device LC-tank
calibration, modem sleep, and reliability watchdogs. Existing
status/hb/event payload keys are preserved — the Node-RED
`Lightning Antenna Protector` flow keeps working unchanged.

### MQTT command surface

- **New subscribe topic** `lightning/as3935/cmd` accepting JSON
  commands of two shapes:
  - `{"set":"<key>","value":<v>}` for tunables.
  - `{"action":"<name>"}` for actions.
- **New publish topic** `lightning/as3935/cmd/ack` returning
  `{ok, cmd, error?, ts}` for every received command (success and
  validation error both produce an ack).
- All `set` operations validate range/type, apply to the AS3935
  immediately, and persist to NVS (`Preferences` namespace
  `as3935`) — values survive reboot.

Tunable keys: `nf` (0–7), `wdth` (0–15), `srej` (0–15), `tun_cap`
(0–15), `mask_dist` (bool), `min_num_lightning` (1/5/9/16), `afe_gb`
(`indoor`/`outdoor`), `modem_sleep` (`max`/`min`).

Actions: `republish_status`, `calibrate_tun_cap`, `reboot`,
`factory_reset_wifi`.

See [`nodered/README.md`](nodered/README.md) for the dashboard
wiring guide and [`nodered/cmd-examples.sh`](nodered/cmd-examples.sh)
for `mosquitto_pub` one-liners.

### On-device TUN_CAP calibration

`calibrateTunCap()` is a direct port of the Python `as3935_tune.py`
LCO sweep: set `LCO_FDIV=3` (÷128), enable `DISP_LCO`, sweep
`TUN_CAP` 0..15 with a 2 s edge-counting window per value, pick the
cap whose frequency lands closest to 500 kHz ± 3.5 %. Persists the
winning cap to NVS, applies it, and re-publishes status.

Runs from the cmd topic — no recompile, no re-flash, no enclosure
opening. MQTT keepalive is pumped during the sweep so the ~35 s
runtime doesn't drop the broker connection.

### Status payload extended

Adds `fw`, `ip`, `rssi`, and the current live value of every
tunable to the retained status. The Python-era keys
(`noise_floor`, `antenna`, `tun_cap`, `irq_pin`, `calib_*`) are
preserved as aliases so existing subscribers don't break.

Heartbeat payload also gains `rssi` so RSSI is visible every 30 s
without waiting for a status republish.

### Reliability watchdogs

- **MQTT reconnect bounded**: after 60 failed connect attempts
  (5 min total), `ESP.restart()` instead of looping forever.
- **No-successful-publish watchdog**: if `mqtt.publish()` hasn't
  returned `true` in 10 min, `ESP.restart()`. Catches the case
  where the lib thinks it's connected but the broker silently
  dropped us.
- The existing ESP32 task watchdog on `loopTask` (5 s) is
  unchanged — `delay()` / `yield()` everywhere keeps it fed.

### WiFi modem sleep

`WiFi.setSleep(WIFI_PS_MAX_MODEM)` enabled by default (toggleable
via the `modem_sleep` cmd). Cuts average idle current from the
~100-150 mA measured on the bench at v0.1.1 down to ~30-50 mA.
First sustained-LAN measurement still TBD — see HANDOVER.

### Other

- AS3935 cold-bus first-read `0xFF` glitch is now retried once
  before the warning fires, cutting noise in the boot log.
- PubSubClient buffer raised from default 256 to 512 bytes to fit
  the larger status payload.
- Added `bblanchon/ArduinoJson @^7.0` for clean cmd parsing / status
  serialisation. Flash cost ~16 KB; total now at 71.5 %.

## v0.1.1 — 2026-05-11

- **Periodic status republish.** The ESP32 now republishes its
  retained `status` message every 5 min, in addition to publishing
  on every MQTT (re)connect. This self-heals stale retained values —
  e.g. when another publisher cleanly hands off the topic, its
  farewell `event:"offline"` no longer persists indefinitely.
- `publishStatus()` is the single source of truth for the
  `lastStatusMs` timer, so the reconnect path and the periodic path
  can't double-publish.

## v0.1.0 — 2026-05-11

Initial firmware. Replaces the indoor wired Pi daemon
`as3935_mqtt.py` from the `vu2cpl-shack` sister repo. MQTT contract
is wire-identical to that daemon so the Node-RED
`Lightning Antenna Protector` flow needs zero changes.

### AS3935 init (`as3935Init()`)

Mirrors the Python daemon byte-for-byte, in the same order:

1. Self-test read of `CFG0` (warn if `0x00` or `0xFF`).
2. Antenna mode → `REG_CFG0 = (CFG0 & 0xC1) | 0x1C` (outdoor
   `AFE_GB = 0x0E`).
3. Noise floor → `REG_CFG1 bits [6:4] = NF` (NF=4).
4. `TUN_CAP` → `REG 0x08 low nibble`, loaded from NVS, default 10.
5. `CALIB_RCO` (`0x96 → 0x3D`), wait 5 ms, verify `DONE=1` /
   `NOK=0` in `0x3A` (TRCO) and `0x3B` (SRCO).
6. Flush pending INT (read `0x03`).

`WDTH` and `SREJ` are intentionally left at chip defaults to match
the Python daemon exactly. `PRESET_DEFAULT` is skipped for the same
reason.

### MQTT contract

Three retained / non-retained topics on broker `192.168.1.169:1883`:

| Topic | Retain | Payload |
|-------|--------|---------|
| `lightning/as3935/status` | retained | `{event:"ready"\|"offline", ts, noise_floor, antenna, tun_cap, irq_pin, calib_trco, calib_srco, fw}` |
| `lightning/as3935/hb` | retained | `{alive:true, ts, uptime_s, counters:{lightning,disturber,noise,irq}}` every 30 s |
| `lightning/as3935` | not retained | Lightning: `{event:"lightning", distance, energy, timestamp}`. Disturber/Noise: `{event, timestamp}`. |

LWT lives on the **`status` topic** (not a separate `/lwt`),
payload `{event:"offline", ts:<boot-time>}`, retained — same as
Python.

`ts` and `timestamp` are local-IST ISO 8601 strings
(`YYYY-MM-DDTHH:MM:SS`), generated via `configTzTime("IST-5:30", …)`
SNTP sync at boot. No re-sync polling in the loop yet.

### WiFi via captive portal

WiFi credentials are **not** baked into the firmware. On first boot
the ESP32 raises its own AP `vu2cpl-as3935-setup` (password
`vu2cpl1234`) and `tzapu/WiFiManager` serves a captive portal at
`http://192.168.4.1` for one-time provisioning. Credentials persist
in the ESP32 WiFi stack's NVS.

Hold the **BOOT button** on the NodeMCU for 3 s at power-on to erase
stored credentials and re-enter the portal.

### Toolchain

- PlatformIO + Arduino, board `esp32dev`.
- Libraries: `knolleary/PubSubClient ^2.8`,
  `tzapu/WiFiManager v2.0.17`.
- AS3935 driver intentionally not a library — direct `Wire` register
  ops in `main.cpp` keep the contract with the Python daemon
  inspectable.
- `upload_port` and `monitor_port` pinned to
  `/dev/cu.usbserial-0001` so PlatformIO never grabs one of the
  bench's FTDI radio-CAT cables by accident.

---

## Bring-up gotchas (2026-05-11)

Hit-and-fixed during the bench bring-up session. Recorded here so
they don't bite a future build.

1. **Silkscreen pin labels on the AS3935 module are abbreviated.**
   This module labels the I²C pins **`D`** (data) and **`C`**
   (clock) rather than `SDA` / `SCL`. Cost a debug round of
   `i2cWriteReadNonStop returned Error -1` and `CFG0=0xFF`. See
   [`WIRING.md`](WIRING.md).
2. **Multiple FTDI cables on the same Mac bench fight over the
   port.** This bench has at least two FTDI radio-CAT cables and
   pio auto-picks the first one it sees, which either fails with
   "Resource busy" or, worse, would scribble firmware onto a radio
   interface. Fix: pinned `upload_port` / `monitor_port` to the
   ESP32's `usbserial-0001` (CH340) in `platformio.ini`.
3. **The Pi daemon's clean-shutdown handler races the new
   publisher.** When `as3935.service` was stopped on `noderedpi4`,
   its `SIGTERM` handler published `event:"offline"` retained, which
   overwrote the ESP32's `event:"ready"`. The ESP32 only republishes
   `status` on MQTT reconnect, so the broker carried the Pi's
   farewell until the ESP32 was reset. Fixed in v0.1.1 (periodic
   status republish every 5 min).
4. **First I²C transaction after cold power-up returns `0xFF`.**
   Subsequent reads succeed. Harmless cold-bus glitch — the firmware
   logs a `CFG0 suspicious` warning but doesn't gate on it. Real
   evidence the chip is alive is `CALIB_RCO TRCO=OK SRCO=OK` two
   reads later.
5. **WiFi credentials baked at compile time are wrong for a sealed
   outdoor box.** Original scaffold used `secrets.h` with
   `WIFI_SSID` / `WIFI_PASS` defines. Re-flashing means cracking the
   IP65 box open. Switched to `WiFiManager` captive portal so
   credentials provision over WiFi from a phone.
