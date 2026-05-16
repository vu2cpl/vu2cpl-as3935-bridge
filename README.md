# vu2cpl-as3935-bridge

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

> ESP32 firmware bridging an AS3935 lightning sensor outdoors to the
> VU2CPL shack's existing MQTT broker. Battery + solar + WiFi, in a
> sealed enclosure mounted in shade.

Sister project to [`vu2cpl-shack`](https://github.com/vu2cpl/vu2cpl-shack)
(Node-RED shack automation) — replaces the wired indoor AS3935
attached to `noderedpi4` with a wireless outdoor sensor, regaining the
chip's rated ~40 km range.

VU2CPL · Manoj · Bengaluru · MK83TE · Licensed 1993

---

## Heads-up for forks

This repo is **opinionated for VU2CPL's specific shack** — it hard-codes:

- MQTT broker at `192.168.1.169:1883`, plain, no auth, LAN-only.
- Captive-portal AP `vu2cpl-as3935-setup` / `vu2cpl1234` for first
  WiFi setup (5-min window after factory reset).
- IST timezone for ISO timestamps (`configTzTime("IST-5:30", …)`).
- The existing Node-RED broker config ID `f4785be9863eab08`
  from `vu2cpl-shack/flows.json`.

If you're forking this for your own station, edit `src/main.cpp`
constants near the top (`MQTT_HOST`, `WIFI_AP_SSID`, `WIFI_AP_PASS`)
and re-point the Node-RED broker in [`nodered/build-flow.py`](nodered/build-flow.py)
before rebuilding the flow JSON. The captive-portal password is
intentionally simple because the AP is only up briefly during
on-site setup — change it if your install is somewhere physically
accessible to strangers.

---

## Why this exists

The AS3935 sensor at VU2CPL was previously wired via I²C + IRQ on
GPIO4 to `noderedpi4` indoors, with the antenna also indoors. Indoor
operation requires `AFE_GB=0x12` (high gain) and stray capacitance
limits effective range to ~few km. Moving the sensor outdoors
recovers the rated ~40 km range with `AFE_GB=0x1C` and a clean
re-tuned LC tank (`TUN_CAP` sweep on the new physical layout).

I²C is too short-distance to run from the shack out to a true
outdoor location. The ESP32 hosts the sensor locally (short I²C run
inside the enclosure) and bridges to the shack LAN over WiFi.

## What it talks to

Existing MQTT broker: **Mosquitto on `192.168.1.169:1883`** (plain,
no auth, LAN only).

The MQTT contract is **identical to the previous
`as3935_mqtt.py` Python daemon** — same topics, same JSON shape, same
heartbeat cadence, same LWT semantics — so the Node-RED Lightning
Antenna Protector flow needs **zero changes**:

| Topic | Direction | Payload |
|-------|-----------|---------|
| `lightning/as3935` | publish | Lightning: `{event:"lightning", distance, energy, timestamp}`. Disturber: `{event:"disturber", timestamp}`. Noise: `{event:"noise", timestamp}`. |
| `lightning/as3935/status` | publish (retained) | Full state: `{event, ts, fw, ip, rssi, nf, wdth, srej, tun_cap, mask_dist, min_num_lightning, afe_gb, modem_sleep, vbat_mv, vbat_offset_mv, irq_pin, calib_trco, calib_srco, …}` on (re)connect and every 5 min. LWT publishes the same topic with `event:"offline"` retained. |
| `lightning/as3935/hb` | publish (retained) | `{alive:true, ts, uptime_s, rssi, vbat_mv, counters:{lightning, disturber, noise, irq}}` every 30 s. |
| `lightning/as3935/cmd` | **subscribe** | `{"set":"<key>","value":<v>}` or `{"action":"<name>"}` — live tuning. See [`nodered/README.md`](nodered/README.md). |
| `lightning/as3935/cmd/ack` | publish | `{ok, cmd, error?, ts}` for every received command. |
| `lightning/as3935/last_event` | publish (retained) | `{event, distance, energy, timestamp, ts_epoch_ms}` — most recent disturber/noise/lightning event. Retained so the shack dashboard's "LAST SEEN" survives Node-RED restart. `ts_epoch_ms` is `time(nullptr) * 1000` (ms since epoch). `distance`/`energy` are `0` for disturber/noise (non-lightning events don't have physically meaningful values). |

`ts` / `timestamp` are local-IST ISO 8601 strings (`YYYY-MM-DDTHH:MM:SS`),
matching the Python daemon. The ESP32 SNTP-syncs at boot.

## Hardware

| Item | Detail |
|------|--------|
| MCU | ESP-WROOM-32 (NodeMCU dev board) — minimum 4 MB flash. ESP32-S2/S3/C3/C6 also work in principle; see [BUILD.md § Compatible ESP32 variants](BUILD.md#compatible-esp32-variants). ESP8266 and ESP32-H2 are not supported. |
| Sensor | AS3935 lightning detector (I²C variant) |
| Battery | 18650 Li-ion (3000 mAh) |
| Charger | TP4056 (Li-ion, 5 V solar input, BMS-equipped variant recommended) |
| Solar | 5 V / 1 W or 2 W panel (sized for net-positive daily) |
| Enclosure | IP65 sealed plastic, mounted in **shade** (Li-ion + heat is fatal) |
| WiFi | 2.4 GHz, shack AP, verified RSSI at install location before sealing |

## Build & flash

**Quick path** — interactive installer:

```sh
git clone https://github.com/vu2cpl/vu2cpl-as3935-bridge.git
cd vu2cpl-as3935-bridge
python3 install.py
```

`install.py` installs PlatformIO if needed, prompts for your MQTT
broker, captive-portal AP creds, timezone, serial port, and
(optionally) Node-RED IDs, patches the source in place, and builds
the firmware. You then run `pio run -t upload` to flash. See
[`BUILD.md`](BUILD.md) for the manual step-by-step.

WiFi credentials are not baked into the firmware — on first boot
the ESP32 raises a captive portal AP (`vu2cpl-as3935-setup` /
`vu2cpl1234` by default; the installer lets you change these) so
you can pick the shack AP and enter its password from your phone.
Hold the BOOT button for 3 s at power-on to clear stored
credentials.

## Wiring

See [`WIRING.md`](WIRING.md) for the connection diagram and bench
bring-up checklist.

## Status

**v0.3.0 — battery voltage telemetry (2026-05-17).** Firmware now
publishes `vbat_mv` on `/hb` (every 30 s) and `/status` (on connect
+ every 5 min). New `cmd` action `query_vbat` returns a one-shot
fresh reading on demand. Per-chip Vref delta correctable via the new
`vbat_offset_mv` NVS tunable (±500 mV bracket, settable over `cmd`).
Hardware: 100 kΩ + 100 kΩ + 100 nF divider on GPIO 34 — see
[`WIRING.md § Battery voltage divider`](WIRING.md#battery-voltage-divider-v030-required-for-outdoor-deploy).
Without the divider the firmware still boots and runs; it just
reports ~0 V, which is the visual cue the mod hasn't been done.

**v0.2.0 live on the bench since 2026-05-12.** Firmware exposes the
full AS3935 register surface over an MQTT command channel
(`lightning/as3935/cmd` in, `lightning/as3935/cmd/ack` out), NVS-backed,
range-validated. On-device TUN_CAP calibration runs as a `cmd`
action — no enclosure-opening for re-tune after physical install.
WiFi modem sleep enabled by default. MQTT reconnect bounded + no-publish
watchdog → auto-restart.

A Node-RED dashboard flow ([`nodered/`](nodered/)) is **live** on the
shack Pi (`noderedpi4`) since 2026-05-12 and ships two `ui_template`
panels:

- **AS3935 Tuning** — knobs for every tunable + actions (Calibrate /
  Republish / **Query Battery** / Reboot / Factory Reset WiFi). Status /
  heartbeat / ack render in real time. v0.3.0 added a **🔋 battery
  row** with mV reading, derived %SOC from a piecewise-linear LUT,
  green / amber / red colour bands (≥ 3.90 V / 3.70–3.90 V / < 3.70 V),
  and a "(divider not wired?)" hint when the reading is < 500 mV.
- **AS3935 Events** — Last Event card (backed by retained
  `lightning/as3935/last_event` so it survives Node-RED + browser
  refresh within 5 s), session counters, 30-row rolling event log.

Plus 5 TEST inject buttons that publish fake events for end-to-end
exercise without the ESP32, and a 5-phase comprehensive test plan in
[`nodered/README.md`](nodered/README.md). The shack's distance-graded
disconnect logic in [`vu2cpl-shack`](https://github.com/vu2cpl/vu2cpl-shack)
consumes this bridge's `lightning/as3935` event stream as its primary
strike source.

**Outstanding** (per [`HANDOVER.md`](HANDOVER.md) for full detail):
solder the v0.3.0 battery divider + flash + verify on bench against
a DMM, verify modem-sleep current drop, build the power chain
(TP4056 + 18650 + solar), seal the enclosure and do the field install
with in-situ TUN_CAP recalibration over MQTT, then eventually
deep-sleep + EXT0 wake on the AS3935 IRQ. Shack-side Telegram alert
on low `vbat_mv` is being tracked separately in
[`vu2cpl-shack`](https://github.com/vu2cpl/vu2cpl-shack).

See [`CHANGELOG.md`](CHANGELOG.md) for version-by-version history.

## License

MIT — see [LICENSE](LICENSE).
