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
`as3935_mqtt.py` Python daemon** — same topic, same JSON shape, same
heartbeat cadence, same LWT semantics — so the Node-RED Lightning
Antenna Protector flow needs **zero changes**:

| Topic | Direction | Payload |
|-------|-----------|---------|
| `lightning/as3935` | publish | `{event, distance, energy, timestamp}` per strike/disturber/noise event |
| `lightning/as3935/status` | publish (retained) | `{up, tun_cap, irq_pin, nf, afe_gb, calib_trco, calib_srco, rssi, ts}` once at boot |
| `lightning/as3935/hb` | publish | `{ts, rssi}` every 30 s |
| `lightning/as3935/lwt` | LWT (retained) | `"offline"` set on broker disconnect |

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

See [`BUILD.md`](BUILD.md).

## Status

Project planning. See [`HANDOVER.md`](HANDOVER.md) for current state.

## License

MIT — see [LICENSE](LICENSE).
