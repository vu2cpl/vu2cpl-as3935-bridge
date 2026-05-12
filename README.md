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
| `lightning/as3935/status` | publish (retained) | Full state: `{event, ts, fw, ip, rssi, nf, wdth, srej, tun_cap, mask_dist, min_num_lightning, afe_gb, modem_sleep, irq_pin, calib_trco, calib_srco, …}` on (re)connect and every 5 min. LWT publishes the same topic with `event:"offline"` retained. |
| `lightning/as3935/hb` | publish (retained) | `{alive:true, ts, uptime_s, rssi, counters:{lightning, disturber, noise, irq}}` every 30 s. |
| `lightning/as3935/cmd` | **subscribe** | `{"set":"<key>","value":<v>}` or `{"action":"<name>"}` — live tuning. See [`nodered/README.md`](nodered/README.md). |
| `lightning/as3935/cmd/ack` | publish | `{ok, cmd, error?, ts}` for every received command. |

`ts` / `timestamp` are local-IST ISO 8601 strings (`YYYY-MM-DDTHH:MM:SS`),
matching the Python daemon. The ESP32 SNTP-syncs at boot.

## Hardware

| Item | Detail |
|------|--------|
| MCU | ESP-WROOM-32 (NodeMCU dev board) |
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

Project planning. See [`HANDOVER.md`](HANDOVER.md) for current state.

## License

MIT — see [LICENSE](LICENSE).
