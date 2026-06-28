# Node-RED control + dashboard for the AS3935 bridge

Live tuning + events + test injects for the ESP32 lightning bridge.
Two paths:

1. **Importable flow** (recommended): `as3935-control-flow.json` —
   **two `ui_template` panels** (Tuning + Events), 5 `mqtt in`, 4
   cache function nodes, a 5s replay tick that feeds two replay
   functions, 5 TEST inject buttons, 2 `mqtt out`. Matches the
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

After deploy, two dashboard groups appear under the **Shack
Monitoring tools** tab.

### Group 1 — AS3935 Tuning

- **Status header** — colour-coded LED, FW / IP / RSSI / uptime.
- **Counters strip** — `⚡ N · ⚠ N · 📡 N · IRQ N`.
- **Calib line** — `TRCO=OK · SRCO=OK`.
- **Battery row** (v0.3.0) — `🔋 V.VV V  (≈ NN %)`. Green when
  ≥ 3.90 V, amber 3.70–3.90 V, red < 3.70 V. Shows `(divider not
  wired?)` when the reading is < 500 mV. `vbat_offset_mv` is shown
  on the right edge when non-zero (per-chip Vref trim).
- **Tunables block** — NF, WDTH, SREJ, TUN_CAP with `−` / `+` nudge
  buttons; Mask-dist toggle; AFE GB toggle (indoor/outdoor — chip
  goes green when outdoor, amber when indoor); Min-strikes and
  Modem-sleep dropdowns.
- **Actions block** — Calibrate TUN_CAP (~35 s), Republish,
  **Query Battery** (v0.3.0 — one-shot fresh `vbat_mv` reading
  outside the 30 s heartbeat cadence), Reboot, Factory Reset WiFi
  (the destructive two require a confirm dialog).
- **Ack footer** — last command result, green tick / red cross.

The three MQTT in nodes fan through `Cache /status` · `Cache /hb` ·
`Cache /cmd_ack` function nodes that stash each payload in flow
context, and a 5-second `Replay every 5s` inject re-emits the cached
values to the panel. **Worst-case rehydration after opening the
dashboard cold is 5 s** — no need to hit *Republish Status* on the
ESP32 when you open a fresh tab. The replay path is a workaround for
`ui_control` being absent from `node-red-dashboard 3.6.6`.

Every nudge / toggle / dropdown change publishes a single
`{"set":"<key>","value":...}` payload to `lightning/as3935/cmd`. The
ESP32 acks on `.../cmd/ack` and re-publishes status — the panel
re-renders within ~100 ms.

### Group 2 — AS3935 Events

A second panel directly below Tuning shows live strike / disturber /
noise traffic from the chip:

- **Last Event card** — large icon (⚡ / ⚠ / 📡), event summary in
  red / amber / muted, ISO timestamp + a live `Xs ago` counter
  (updated 1 Hz client-side). Backed by the retained
  `lightning/as3935/last_event` topic, so it **survives Node-RED
  restart and browser refresh** with at-most-5 s rehydration delay.
- **Session counters** — `⚡ N · ⚠ N · 📡 N`, reset when the
  browser tab opens. Reflects what *this* tab has seen.
- **Recent events log** — newest-first, capped at 30 rows; for
  lightning rows shows distance (or `out of range` / `overhead`) and
  raw `energy=`. Session-scoped — refreshing the browser clears it.

Live events arrive via `mqtt in lightning/as3935`. The retained
last_event flows `mqtt in → Cache /last_event → panel`, and a
`Replay last_event (5s tick)` function shares the same 5 s tick as
the Tuning panel — both panels rehydrate together.

### TEST inject buttons (no ESP32 required)

The flow ships **five inject nodes** wired to an `mqtt out
lightning/as3935`. Pressing any of them publishes a single fake
event that the Events panel renders exactly as a real one:

| Button | Payload |
|--------|---------|
| `TEST · ⚡ lightning @ 5 km` | `{"event":"lightning","distance":5,"energy":850000,"timestamp":"TEST"}` |
| `TEST · ⚡ lightning @ 25 km` | `{"event":"lightning","distance":25,"energy":120000,...}` |
| `TEST · ⚡ lightning out-of-range` | `{"event":"lightning","distance":63,...}` (chip sentinel) |
| `TEST · ⚠ disturber` | `{"event":"disturber","timestamp":"TEST"}` |
| `TEST · 📡 noise` | `{"event":"noise","timestamp":"TEST"}` |

These do **not** touch the bridge or its retained last_event — they
only exercise the Events panel's live `lightning/as3935`
subscription. To also exercise the retained-rehydration path, use
the `mosquitto_pub` recipe in the test plan below.

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
that shows live strikes. **That stays untouched** — this new flow
is a separate tuning + events UI on the Monitoring tools tab so the
main operational dashboard isn't cluttered with maintenance controls.

---

## Comprehensive test plan

Walks through every code path in the flow without needing a live
storm. Estimated time: ~10 minutes. Assumes the bridge ESP32 is
powered, on Wi-Fi, and publishing to the same MQTT broker that
Node-RED talks to.

Set up two extra terminals on `noderedpi4` (or any host that can
reach the broker):

```sh
# Terminal A — watch every AS3935 topic
mosquitto_sub -h 192.168.1.169 -t 'lightning/as3935/#' -v

# Terminal B — for the mosquitto_pub one-liners below
H=192.168.1.169
```

Open the dashboard at `http://noderedpi4:1880/ui` and navigate to
**Shack Monitoring tools**. You should see both groups (Tuning above,
Events below).

### Phase 1 — Tuning panel sanity

| # | Action | Pass criterion |
|---|--------|----------------|
| 1.1 | **Open the dashboard cold** (close all tabs, reopen). | Within 5 s, Tuning panel populates: LED green/amber (not grey), FW + IP + RSSI shown, counter strip non-zero, calib `TRCO=OK · SRCO=OK`. *Validates the cache + replay rehydration path.* |
| 1.2 | Click `NF` **`+`** once. | Within ~200 ms: ack footer shows `✓ set:nf @ HH:MM:SS` in green, `NF` value increments. Terminal A shows one `lightning/as3935/cmd` and one `cmd/ack`. |
| 1.3 | Click `NF` **`−`** to restore. | Symmetric, value returns to start. |
| 1.4 | Click `Mask dist` **toggle**. | Value flips `OFF` ↔ `ON`, ack green. |
| 1.5 | Click `AFE GB` **toggle**. | Chip flips `INDOOR` (amber) ↔ `OUTDOOR` (green). Ack green. |
| 1.6 | Pick **Min strikes = 9** from dropdown. | Ack green, value persists across browser refresh (NVS-backed on the ESP32). |
| 1.7 | Click **Republish Status**. | Ack green, all status fields visibly re-render at ~the same instant. |
| 1.8 | **Out-of-range guard**: push `NF +` repeatedly. | At `NF=7` the next `+` should *not* publish anything (the panel's `nudge` guard kicks in before MQTT). Terminal A confirms no extra cmd. |
| 1.9 | **Confirm dialog**: click **Reboot ESP32**, **Cancel** in the popup. | No cmd published. Re-click and **OK** — cmd flies, panel goes amber/grey for ~10 s while ESP32 reboots, then comes back green. |

### Phase 1b — Battery telemetry (v0.3.0)

Requires the GPIO 34 divider per [`WIRING.md § Battery voltage
divider`](../WIRING.md#battery-voltage-divider-v030-required-for-outdoor-deploy).
If the divider isn't soldered yet, steps 1b.1–1b.3 still run — the
panel will show `🔋 — (divider not wired?)` and the heartbeat will
carry `vbat_mv` close to 0 (whatever the floating ADC reads). 1b.4
onwards needs the hardware.

| # | Action | Pass criterion |
|---|--------|----------------|
| 1b.1 | Watch Terminal A for the next heartbeat (≤ 30 s). | Payload includes `"vbat_mv":NNNN`. Without the divider, `NNNN` is < 500. |
| 1b.2 | Check the battery row in the Tuning panel. | If divider wired: shows `🔋 X.XX V (≈ NN %)` with green/amber/red colour. If not: shows `🔋 — (divider not wired?)` in muted grey. |
| 1b.3 | Click **Query Battery** in the Actions block. | Ack footer: `✓ action:query_vbat vbat_mv=NNNN @ HH:MM:SS` (green tick). Status republishes ~immediately — the battery row updates without waiting for the next heartbeat. |
| 1b.4 | **DMM cross-check** (divider required). Measure TP4056 OUT+ to GND with a known-good multimeter. | Reading should match the dashboard's mV value within ±50 mV. Most ESP32s have factory-calibrated eFuse Vref so the delta is typically < 30 mV. |
| 1b.5 | If delta > 50 mV: apply the trim. Example for firmware reading 3920 mV vs DMM 3850 mV → offset = −70. | `mosquitto_pub -h $H -t lightning/as3935/cmd -m '{"set":"vbat_offset_mv","value":-70}'` — ack green, panel battery row updates within ~200 ms, `offset −70 mV` appears at the right edge of the row. NVS-persisted (survives reboot). |
| 1b.6 | Reboot the ESP32 (`{"action":"reboot"}` or power-cycle). | Battery reading returns to within ±10 mV of the pre-reboot value (offset persists). Confirms NVS round-trip for the new key. |
| 1b.7 | **Threshold colour check** (no hardware needed — just observe). | Battery rendering: ≥ 3.90 V green, 3.70–3.90 V amber, < 3.70 V red. If your battery happens to sit in one band, you can verify other bands by temporarily setting `vbat_offset_mv` to push the reading across thresholds. Remember to reset it afterwards. |

### Phase 2 — Events panel via TEST injects

In the Node-RED editor (not the dashboard), open the **AS3935 Bridge** tab.
The five TEST inject nodes have arrow buttons on their left edge — that's
how you fire them.

| # | Action | Pass criterion |
|---|--------|----------------|
| 2.1 | Click ▶ on `TEST · ⚡ lightning @ 5 km`. | Events panel: Last Event card flips to red `⚡ Lightning · 5 km · energy 850000`. Counter `⚡` → 1. New row appears at the top of Recent events. |
| 2.2 | Click ▶ on `TEST · ⚡ lightning @ 25 km`. | Last Event updates, `⚡` counter → 2, second row prepended. |
| 2.3 | Click ▶ on `TEST · ⚡ lightning out-of-range`. | Distance renders as `out of range` (chip sentinel 63 handled), counter → 3. |
| 2.4 | Click ▶ on `TEST · ⚠ disturber`. | Last Event card flips amber, `⚠` counter → 1. |
| 2.5 | Click ▶ on `TEST · 📡 noise`. | Last Event card flips muted-grey, `📡` counter → 1. |
| 2.6 | Click `TEST · ⚡ lightning @ 5 km` **10 times in a row**. | Recent events log grows to 10 rows; counters update each press; no flicker, no lag. |
| 2.7 | Refresh the **dashboard** (not the editor). | Session counters reset to 0, Recent events clears — **but the Last Event card stays populated**. *Confirms it's reading the retained topic, not the live stream.* |

### Phase 3 — Retained `last_event` rehydration

This is the path that survives Node-RED restart. The TEST injects do
**not** populate the retained topic; we have to publish it manually.

```sh
# Pretend the bridge just saw a 3 km strike — set the retained marker.
mosquitto_pub -h $H -t lightning/as3935/last_event -r -m '{
  "event":"lightning","distance":3,"energy":1234567,
  "timestamp":"2026-05-15T10:00:00","ts_epoch_ms":1747303200000
}'
```

| # | Action | Pass criterion |
|---|--------|----------------|
| 3.1 | Run the `mosquitto_pub -r` above. | Within 5 s, Events panel Last Event card flips to red `⚡ Lightning · 3 km · energy 1234567`. *(Retained delivery on subscribe via `rap: true` — almost instant in practice.)* |
| 3.2 | Refresh the dashboard. | Last Event card **re-populates within 5 s** of the page rendering. *Validates `Cache /last_event` + `Replay last_event (5s tick)`.* |
| 3.3 | In the Node-RED editor, **Stop / Start** the AS3935 Bridge tab (top-right menu → Disable then Enable). | After re-enable, Last Event card re-populates within 5 s without anyone touching the bridge. *Validates retained delivery on Node-RED's MQTT (re)subscribe.* |
| 3.4 | Clear the retained marker: `mosquitto_pub -h $H -t lightning/as3935/last_event -r -n` (`-n` = empty/null payload). | Card stays showing the prior value — the panel doesn't clear on a null retained delete (by design). A real `lightning/as3935/last_event` from the bridge will overwrite it. |

### Phase 4 — End-to-end with the live ESP32

Cover the bridge antenna or rub a piezo lighter near it to trigger
real interrupts.

| # | Action | Pass criterion |
|---|--------|----------------|
| 4.1 | Wait for a real `disturber` IRQ (very common with man-made noise). | Terminal A: `lightning/as3935  {"event":"disturber",...}` plus a retained `.../last_event` and an updated `.../hb` (counters.disturber++). Events panel: Last Event card amber. |
| 4.2 | Watch `.../hb` ticks. | One every ~10 s; `counters.irq` strictly non-decreasing; uptime monotonic. Tuning panel counter strip updates. |
| 4.3 | **Power-cycle the ESP32**. | Tuning panel LED → red, then back to green ~15 s after reboot. NVS-persisted tunables (NF, WDTH, AFE GB, etc.) survive the cycle. The retained `last_event` from before the reboot is still shown on the Events panel — that's the whole point of `r=true` on the bridge side. |
| 4.4 | **Sensor-proximity sanity check** (commissioning a new deploy). After mounting, leave the bridge running for 15–30 min in clear weather, then look at the Events panel's rolling log. | All `lightning` events should be reasonable distances (5+ km is typical for real strikes; closer is rare). **If most events report `distance=1`** ("overhead") and arrive at a periodic cadence near 5 min, the AS3935 board is too close to the ESP32 — see [`HANDOVER.md § Known gotchas`](../HANDOVER.md#known-gotchas) and [`WIRING.md § Outdoor deployment`](../WIRING.md#outdoor-deployment--two-box-topology-required) for the two-box fix. Strictness knobs (WDTH ↑, min_num_lightning ↑) mask the symptom but don't address the cause. |

### Phase 5 — Regression smoke

After any panel edit + `python3 nodered/build-flow.py` + re-import:

- [ ] Both panels render without console errors (Cmd-Option-J in the
      browser, Console tab — should be silent).
- [ ] Replay tick status node in the Node-RED editor shows
      `replay tick · 3 msg(s)` (Tuning side) and `replay · last_event`
      (Events side) green-dot every 5 s.
- [ ] All five TEST inject nodes still fire (single-message publish
      per click).
- [ ] No orphan wires (the build script JSON-validates; missing IDs
      will surface as red bars in the Node-RED editor at import time).

---

## Quick reference — MQTT topics

| Topic | Direction | Purpose |
|-------|-----------|---------|
| `lightning/as3935/cmd` | publish → ESP32 | Send a tuning command |
| `lightning/as3935/cmd/ack` | ESP32 → subscribe | One-line ack/error |
| `lightning/as3935/status` | ESP32 → subscribe (retained) | Full state — incl. `vbat_mv` + `vbat_offset_mv` (v0.3.0) |
| `lightning/as3935/hb` | ESP32 → subscribe (retained) | uptime + counters + `vbat_mv` (v0.3.0) |
| `lightning/as3935` | ESP32 → subscribe | lightning/disturber/noise events |
| `lightning/as3935/last_event` | ESP32 → subscribe (retained) | Latest event with `ts_epoch_ms` — backs the Events panel's Last Event card across restart |

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
| `vbat_offset_mv` | −500 .. +500 | Per-chip Vref trim for the battery ADC. Add this many millivolts to every `vbat_mv` reading. Most ESP32s need 0 or close to it. |

### Actions

| Action | Effect |
|--------|--------|
| `republish_status` | Re-publish retained status |
| `calibrate_tun_cap` | ~35 s LCO sweep, picks best cap, persists |
| `query_vbat` | One-shot fresh battery reading; acks `action:query_vbat vbat_mv=NNNN` and republishes status so the dashboard updates atomically |
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
