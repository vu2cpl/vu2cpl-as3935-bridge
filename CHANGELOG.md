# Changelog — vu2cpl-as3935-bridge

All notable firmware changes, newest first. Versions tagged via
`-DFIRMWARE_VERSION='"vX.Y.Z"'` in `platformio.ini` and surfaced in
the `fw` field of every `lightning/as3935/status` MQTT payload.

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
