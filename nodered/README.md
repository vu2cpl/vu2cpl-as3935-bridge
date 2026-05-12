# Node-RED control + dashboard for the AS3935 bridge

Live tuning + status panel for the ESP32 lightning bridge. Two paths:

1. **Importable flow** (recommended): `as3935-control-flow.json` —
   one `ui_template` + 3 `mqtt in` + 1 `mqtt out`. Matches the
   existing dashboard's GitHub-dark palette (#0d1117 / #161b22 /
   #238636 / #f85149 / etc.), references the existing
   `f4785be9863eab08` MQTT broker config, drops itself into the
   **Shack Monitoring tools** dashboard tab.
2. **Terminal-only** via `cmd-examples.sh` — no Node-RED changes,
   `mosquitto_pub` one-liners for every supported cmd.

---

## Import the flow

On `noderedpi4`:

1. Pull the latest from this repo, OR download just
   [`as3935-control-flow.json`](as3935-control-flow.json) to the Pi.
2. Open Node-RED in a browser → top-right menu → **Import**.
3. Click **select a file to import** and pick
   `as3935-control-flow.json`. Choose **import to new flow**.
4. Click **Deploy**.

The new dashboard panel appears at **Shack Monitoring tools >
AS3935 Tuning** with:

- **Status header** — colour-coded LED, FW / IP / RSSI / uptime.
- **Counters strip** — `⚡ N · ⚠ N · 📡 N · IRQ N`.
- **Calib line** — `TRCO=OK SRCO=OK afe_gb=outdoor`.
- **Tunables block** — NF, WDTH, SREJ, TUN_CAP with `−` / `+` nudge
  buttons; Mask-dist toggle; Min-strikes and Modem-sleep dropdowns.
- **Actions block** — Calibrate TUN_CAP (~35 s), Republish, Reboot,
  Factory Reset WiFi (the destructive two require a confirm dialog).
- **Ack footer** — last command result, green tick / red cross.

Every nudge / toggle / dropdown change publishes a single
`{"set":"<key>","value":...}` payload to `lightning/as3935/cmd`. The
ESP32 acks on `.../cmd/ack` and re-publishes status — the panel
re-renders within ~100 ms.

> **Broker reference.** The flow references the existing
> `f4785be9863eab08` broker config ("Tasmota MQTT Broker",
> 192.168.1.169:1883) already present in `vu2cpl-shack/flows.json`.
> If you import into a Node-RED that lacks that config, the MQTT
> nodes will show as un-configured — re-point them at your broker.

### Editing the panel

The panel HTML / CSS / JS lives in the `format` field of the
`ui_template` node, which is messy to hand-edit inside JSON.
**Edit [`build-flow.py`](build-flow.py) instead** — the `CSS`,
`HTML`, and `JS` constants at the top contain the readable source.
Regenerate the JSON with:

```sh
python3 nodered/build-flow.py
```

The script overwrites `nodered/as3935-control-flow.json`. Re-import
or hot-reload in Node-RED to see the change.

### Note on the existing Lightning Protection group

The shack's main dashboard tab already has a **Lightning
Protection** group with the read-only `Master Dashboard` template
that shows live strikes. **That stays untouched** — this new panel
is a separate tuning UI on the Monitoring tools tab so the main
operational dashboard isn't cluttered with maintenance controls.

---

## Quick reference — MQTT topics

| Topic | Direction | Purpose |
|-------|-----------|---------|
| `lightning/as3935/cmd` | publish → ESP32 | Send a tuning command |
| `lightning/as3935/cmd/ack` | ESP32 → subscribe | One-line ack/error |
| `lightning/as3935/status` | ESP32 → subscribe (retained) | Full state |
| `lightning/as3935/hb` | ESP32 → subscribe (retained) | uptime + counters |
| `lightning/as3935` | ESP32 → subscribe | lightning/disturber/noise events |

---

## Command schema

```json
{"set": "<key>", "value": <number|bool|string>}
{"action": "<action_name>"}
```

### Tunable keys

| Key | Range | Effect |
|-----|-------|--------|
| `nf` | 0–7 | Noise-floor threshold |
| `wdth` | 0–15 | Watchdog threshold (pre-amp amplitude gate) |
| `srej` | 0–15 | Spike rejection (post-demod waveform strictness) |
| `tun_cap` | 0–15 | LC tank tuning capacitor index (~8 pF/step) |
| `mask_dist` | true/false | Suppress disturber events |
| `min_num_lightning` | 1 / 5 / 9 / 16 | Strikes before IRQ asserts |
| `afe_gb` | `"indoor"` / `"outdoor"` | Analog front-end gain |
| `modem_sleep` | `"max"` / `"min"` | WiFi modem sleep aggressiveness |

### Actions

| Action | Effect |
|--------|--------|
| `republish_status` | Re-publish retained status |
| `calibrate_tun_cap` | ~35 s LCO sweep, picks best cap, persists |
| `reboot` | `ESP.restart()` |
| `factory_reset_wifi` | Erase WiFi creds, restart into captive portal |

All `set` commands persist to NVS automatically — values survive
reboot.

Every cmd receives an ack on `lightning/as3935/cmd/ack`:

```json
{"ok":true,  "cmd":"set:nf",            "ts":"2026-05-12T10:23:01"}
{"ok":false, "cmd":"set:nf", "error":"nf out of range 0..7", "ts":"..."}
```

---

## Terminal-only path (`mosquitto_pub`)

If you don't want the dashboard, every cmd is one line. See
[`cmd-examples.sh`](cmd-examples.sh) for the full set. Common ones:

```sh
H=192.168.1.169

# Raise noise floor
mosquitto_pub -h $H -t lightning/as3935/cmd \
    -m '{"set":"nf","value":5}'

# Mask disturbers (useful when running noisy ham gear nearby)
mosquitto_pub -h $H -t lightning/as3935/cmd \
    -m '{"set":"mask_dist","value":true}'

# Re-tune LC tank (takes ~35 s; watch serial log for sweep table)
mosquitto_pub -h $H -t lightning/as3935/cmd \
    -m '{"action":"calibrate_tun_cap"}'

# Reboot
mosquitto_pub -h $H -t lightning/as3935/cmd \
    -m '{"action":"reboot"}'
```

Subscribe in another terminal to watch acks + status fly by:

```sh
mosquitto_sub -h 192.168.1.169 -t 'lightning/as3935/#' -v
```
