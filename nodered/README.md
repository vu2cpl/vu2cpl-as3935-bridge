# Node-RED control + dashboard for the AS3935 bridge

A "small dashboard" guide for adding tuning controls to your shack's
Node-RED. You already have the broker connection (`f4785be9863eab08`)
and the `Lightning Antenna Protector` tab. This adds a new tab,
**AS3935 Tuning**, with sliders + buttons that publish to
`lightning/as3935/cmd` and a live status pane that subscribes to
`lightning/as3935/status` and `.../hb`.

Two delivery options:

- **Terminal-only**: use [`cmd-examples.sh`](cmd-examples.sh) with
  `mosquitto_pub` — no Node-RED changes. Best for quick bench
  tweaks.
- **Dashboard**: hand-wire the few nodes described below. Roughly
  10 minutes in the Node-RED editor. No flow JSON to import —
  Node-RED dashboard widgets are easier to tune in the GUI than in
  JSON.

---

## Quick reference — MQTT topics

| Topic | Direction | Purpose |
|-------|-----------|---------|
| `lightning/as3935/cmd` | publish → ESP32 | Send a tuning command (`set` or `action`) |
| `lightning/as3935/cmd/ack` | ESP32 → subscribe | One-line ack/error for each cmd |
| `lightning/as3935/status` | ESP32 → subscribe (retained) | Full state snapshot — live tunable values + calib + IP + RSSI |
| `lightning/as3935/hb` | ESP32 → subscribe (retained) | Every 30 s: uptime, counters, RSSI |
| `lightning/as3935` | ESP32 → subscribe | Lightning / disturber / noise events |

---

## Command schema

JSON. Two shapes:

```json
{"set": "<key>", "value": <number|bool|string>}
{"action": "<action_name>"}
```

### Tunable keys

| Key | Range | Effect |
|-----|-------|--------|
| `nf` | 0–7 | Noise-floor threshold (raise on noisy days) |
| `wdth` | 0–15 | Watchdog threshold (pre-amp amplitude gate) |
| `srej` | 0–15 | Spike rejection (post-demod waveform strictness) |
| `tun_cap` | 0–15 | LC tank tuning capacitor index (~8 pF/step) |
| `mask_dist` | true/false | Suppress disturber-class events from being published |
| `min_num_lightning` | 1 / 5 / 9 / 16 | Strikes required before IRQ asserts (storm-only filter) |
| `afe_gb` | `"indoor"` / `"outdoor"` | Analog front-end gain. Keep at outdoor for this bridge. |
| `modem_sleep` | `"max"` / `"min"` | WiFi modem sleep aggressiveness |

### Actions

| Action | Effect |
|--------|--------|
| `republish_status` | Re-publish retained `status` immediately |
| `calibrate_tun_cap` | Run the LCO sweep (port of `as3935_tune.py`) — takes ~35 s, picks the best `tun_cap`, persists it, re-publishes status |
| `reboot` | `ESP.restart()` |
| `factory_reset_wifi` | Erase stored WiFi creds and restart into the captive portal |

All `set` commands persist to NVS automatically — values survive
reboot. Defaults are still hardcoded in `main.cpp` for first boot.

Every cmd receives an ack on `lightning/as3935/cmd/ack`:

```json
{"ok": true,  "cmd": "set:nf",            "ts": "2026-05-12T10:23:01"}
{"ok": false, "cmd": "set:nf", "error": "nf out of range 0..7", "ts": "..."}
```

---

## Hand-wired Node-RED tab — minimal layout

Add a new tab called **AS3935 Tuning** with two `ui_group` columns:
**Controls** (width 6) and **Status** (width 6). Then add these
nodes.

### One shared MQTT out node

```
type:   mqtt out
broker: <your existing broker config>  (likely f4785be9863eab08)
topic:  lightning/as3935/cmd
qos:    0
retain: false
name:   AS3935 Cmd
```

Every control widget below funnels into this one MQTT out node.

### One shared "build set/action payload" function node

Drop a `function` node named **build-cmd** between every UI widget
and the MQTT-out. Body:

```js
// Pulls msg.cmdKey or msg.cmdAction out of the widget's preset
// msg, builds the JSON payload, returns it ready for MQTT out.
if (msg.cmdAction) {
    msg.payload = { action: msg.cmdAction };
} else if (msg.cmdKey !== undefined) {
    msg.payload = { set: msg.cmdKey, value: msg.payload };
}
return msg;
```

### Sliders (one per numeric tunable)

For each of `nf`, `wdth`, `srej`, `tun_cap`:

```
type:    ui_slider
group:   Controls
label:   NF (0..7)            ← match the key
min/max: 0 / 7                ← see range table above
step:    1
output:  only on release
topic:   <leave blank>
```

After the slider, drop a `change` node:

```
type:   change
action: set
target: msg.cmdKey
to:     "nf"   ← string, the key name
```

Wire: slider → change(cmdKey=nf) → build-cmd → AS3935 Cmd.

### Switch (`mask_dist`)

```
type:     ui_switch
label:    Mask disturbers
on:       true
off:      false
```

Wire: switch → change(cmdKey=mask_dist) → build-cmd → AS3935 Cmd.

### Dropdowns (`min_num_lightning`, `afe_gb`, `modem_sleep`)

`ui_dropdown` widgets. For `min_num_lightning` the options are
`1 / 5 / 9 / 16` (numeric). For `afe_gb` the options are
`indoor / outdoor` (string). For `modem_sleep`, `max / min` (string).

### Action buttons

```
type:   ui_button
label:  Calibrate TUN_CAP    (or Republish, Reboot, Factory-reset WiFi)
```

After each button drop a `change` node setting `msg.cmdAction` to
the action name (e.g. `calibrate_tun_cap`). Wire to build-cmd → MQTT
Cmd.

> **Tip for `factory_reset_wifi`** — wrap it in a `ui_toast` confirm
> dialog or a second "Are you sure?" button so a stray tap doesn't
> wipe your portal creds.

### Status pane

Two `mqtt in` nodes:

| name | topic | output |
|------|-------|--------|
| AS3935 Status | `lightning/as3935/status` | parsed JSON object |
| AS3935 Heartbeat | `lightning/as3935/hb` | parsed JSON object |

Pipe each into one **parse-status** function node:

```js
const p = msg.payload;
if (msg.topic.endsWith('/status')) {
    flow.set('as3935', p);
    msg.payload =
        `FW ${p.fw}   IP ${p.ip}   RSSI ${p.rssi} dBm\n` +
        `NF=${p.nf}  WDTH=${p.wdth}  SREJ=${p.srej}  TUN_CAP=${p.tun_cap}\n` +
        `mask_dist=${p.mask_dist}  min_lt=${p.min_num_lightning}  afe_gb=${p.afe_gb}\n` +
        `calib  TRCO=${p.calib_trco}  SRCO=${p.calib_srco}\n` +
        `modem_sleep=${p.modem_sleep}`;
} else if (msg.topic.endsWith('/hb')) {
    const c = p.counters || {};
    msg.payload =
        `uptime ${p.uptime_s}s  RSSI ${p.rssi} dBm\n` +
        `lightning=${c.lightning}  disturber=${c.disturber}  noise=${c.noise}  irq=${c.irq}`;
}
return msg;
```

Wire each to a `ui_text` widget (multi-line) in the Status group.

### Ack toaster (optional but useful)

```
mqtt in  → topic: lightning/as3935/cmd/ack → ui_toast
```

`ui_toast` raw mode shows the ack JSON briefly when each cmd
applies. Quick visual feedback while you're tuning.

---

## Terminal-only path (`mosquitto_pub`)

If you don't want the dashboard, every cmd is one line. See
[`cmd-examples.sh`](cmd-examples.sh) for the full set. The most
common ones:

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
