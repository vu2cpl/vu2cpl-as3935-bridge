# Session Handover — vu2cpl-as3935-bridge

**Operator:** Manoj VU2CPL · MK83TE · Bengaluru
**Started:** 2026-05-11

---

## Status

**v0.2.0 live on the bench, 2026-05-12.** Firmware now exposes the
full AS3935 register surface as a runtime MQTT control channel
(`lightning/as3935/cmd` + `cmd/ack`), NVS-persisted across reboots,
range-validated per key. On-device TUN_CAP calibration runs as a
`cmd` action — no re-flash, no enclosure-opening required for
post-install re-tune. WiFi modem sleep enabled by default
(`WIFI_PS_MAX_MODEM`). MQTT reconnect bounded + no-publish watchdog
(10 min) → `ESP.restart()`. See [`CHANGELOG.md`](CHANGELOG.md).

The Node-RED **AS3935 Control Panel** is live on `noderedpi4` since
2026-05-12 — a `ui_template` (group `as3935_ctl_grp`, flow tab
`fe70cfdcdfa19aa4` in vu2cpl-shack) wired to one mqtt-out for cmds and
three mqtt-ins for status / hb / ack. Provides NF / WDTH / SREJ /
TUN_CAP / Mask dist / AFE GB / Min strikes / Modem sleep knobs and the
four actions. Styled to the GitHub-dark palette used by the rest of the
shack dashboard. Source HTML/CSS/JS in
[`nodered/build-flow.py`](nodered/build-flow.py); generated flow JSON
in [`nodered/as3935-control-flow.json`](nodered/as3935-control-flow.json).

The example flow now ships a **second panel** — the **AS3935 Events
Panel** (group `as3935_evt_grp`) — alongside Tuning. Subscribes to
`lightning/as3935` (live stream) and `lightning/as3935/last_event`
(retained, so the Last Event card survives Node-RED + browser
refresh, rehydrates within 5 s via the same cache-and-replay tick as
Tuning). Card colour-codes by event type (⚡ red / ⚠ amber /
📡 muted), shows session counters, and a 30-row rolling event log.
**Five TEST inject buttons** in the flow publish fake
`lightning/as3935` events so the panel can be exercised end-to-end
without the ESP32. The bridge's `nodered/README.md` includes a 5-phase
comprehensive test plan (Tuning sanity → Events via TEST injects →
retained-rehydration via `mosquitto_pub -r` → live ESP32 → regression
smoke).

**Downstream consumer:** [`vu2cpl-shack`](https://github.com/vu2cpl/vu2cpl-shack)'s
`Trigger Disconnect` (also rebuilt 2026-05-12) now uses this bridge's
`lightning/as3935` events as the primary strike source for a 3×3
distance-graded decision matrix (AS3935 close/medium/far × Open-Meteo
cold/lit/severe). Strikes from this firmware are the *only* thing that
fires the disconnect chain — Open-Meteo became a corroboration signal,
not a trigger. Documented in `vu2cpl-shack/CLAUDE.md` "Lightning
Antenna Protector → Distance-graded disconnect".

**Bench-verified end-to-end** (v0.1.1 onwards, still holds at v0.2.0):
piezo-lighter sparks produce `event:"disturber"` events on
`lightning/as3935`, counters increment correctly, status republishes
every 5 min, hb publishes every 30 s, ack publishes per command, and
the shack flow's Lightning Antenna Protector consumes everything
unchanged. Indoor Pi daemon `as3935.service` on `noderedpi4` stopped
and disabled — the ESP32 is the sole publisher.

**Previous milestone — v0.1.1, 2026-05-11:** AS3935 wired, calibrated
(`TRCO=OK SRCO=OK`), I²C solid at `0x03`, MQTT pipe live to the shack
broker, WiFi auto-rejoins via WiFiManager-saved creds. Retained status
refreshes every 5 min.

**Outstanding for v0.3.0+:**
- Verify modem-sleep current drop on the bench (expected ~30-50 mA avg
  vs the 100-200 mA measured at v0.1.1).
- Power chain (TP4056 + 18650 + solar), with **panel mounted in sun**
  even if enclosure is in shade (long cable).
- Enclosure seal, field install with in-situ TUN_CAP re-tune (now
  reachable from the shack via `{"action":"calibrate_tun_cap"}` — no
  enclosure-opening needed).
- Eventual deep-sleep + EXT0 wake on AS3935 IRQ (~10 µA between events).
- OTA updates (ArduinoOTA) so a sealed-box re-flash doesn't require
  USB access.

See [`CHANGELOG.md`](CHANGELOG.md) for the version-by-version log and
2026-05-11/12 bring-up gotchas.

---

## Known gotchas

**Dual CP2102 ports collide on the same `/dev/cu.usbserial-0001`.**
This ESP32 board and the sibling [`esp8266-gps-ntp`](https://github.com/vu2cpl/esp8266-gps-ntp)
NodeMCU both ship with Silicon Labs CP2102 USB-serial chips
carrying the factory-locked default serial number `0001`. macOS
(and Linux) hand `/dev/cu.usbserial-0001` to whichever board
enumerated first, so a hard-coded `upload_port` in
`platformio.ini` silently flashes the wrong board whenever both
are connected.

Reprogramming the CP2102 EEPROM to a unique serial number was
investigated on 2026-05-12 from both macOS Sequoia (Apple Silicon)
and Raspberry Pi OS using `cp210x-cfg` (DiUS/cp210x-cfg). The
vendor-OUT control transfer returns success at the libusb level on
both hosts with no error, but the chip's EEPROM is unchanged on the
next enumeration — `iSerial` stays `0001`. Chip markings confirm
genuine Silicon Labs CP2102 (not a CH9102X clone), so the write
isn't being rejected at the protocol layer — the EEPROM was
**factory-locked** by whoever assembled the dev board and the
lock is permanent. No software path forward.

Workaround in place since 2026-05-12: `flash.sh` and `monitor.sh`
at the repo root enumerate the visible USB-serial devices and use
bash's `select` to prompt for the right one when more than one is
present. `platformio.ini` intentionally leaves `upload_port` /
`monitor_port` unset so the wrappers are the single source of truth.

```sh
./flash.sh       # build + upload, prompts when >1 port present
./monitor.sh     # serial monitor, same prompt
```

The same scripts and convention live in the sibling
`esp8266-gps-ntp` repo. The rule *"ESP firmware projects use a
`flash.sh`/`monitor.sh` picker, not a pinned `upload_port`"* is
captured in `~/.claude/CLAUDE.md` so future ESP repos pick it up
automatically.

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

## 2026-05-12 — v0.2.0 shipped: cmd channel, NVS persistence, TUN_CAP sweep

### What landed

The firmware grew a runtime control surface so the indoor / outdoor
gain decision (and every other AS3935 tunable) can be flipped without
a re-flash. Builds on v0.1.1's bench bring-up.

**New MQTT topics:**

| Topic                            | Direction | Notes |
|----------------------------------|-----------|-------|
| `lightning/as3935/cmd`           | Node-RED → ESP32 | JSON command in. Single-message dispatch — no batching. |
| `lightning/as3935/cmd/ack`       | ESP32 → Node-RED | Per-message ack. Not retained — failure detail (`error` field) only useful in the moment. |
| `lightning/as3935/status`        | ESP32 → Node-RED | Existing topic; v0.2.0 now republishes it after every successful `set`/`action` so subscribers see fresh state immediately. |

**Command payload shapes:**

```jsonc
// Tunables — written through to AS3935 register, then to NVS
{"set": "nf",                "value": 0..7}
{"set": "wdth",              "value": 0..15}
{"set": "srej",              "value": 0..15}
{"set": "tun_cap",           "value": 0..15}
{"set": "mask_dist",         "value": true|false}
{"set": "min_num_lightning", "value": 1|5|9|16}     // datasheet-quantised
{"set": "afe_gb",            "value": "indoor"|"outdoor"}  // string enum, not raw hex
{"set": "modem_sleep",       "value": "max"|"min"}

// Actions — side effects, no value
{"action": "republish_status"}
{"action": "calibrate_tun_cap"}   // ~35 s sweep, MQTT kept alive inside
{"action": "reboot"}
{"action": "factory_reset_wifi"}
```

**Ack shape:**

```jsonc
{"ok": true,  "cmd": "set:afe_gb", "ts": "2026-05-12T..."}
{"ok": false, "cmd": "set:afe_gb", "error": "afe_gb must be 'indoor' or 'outdoor'", "ts": "..."}
```

Every `set` succeeds → ack + status republish, in that order. Every
failed `set` → ack only (no register write, no NVS write, no status).

### Decisions taken in v0.2.0

| # | Decision | Reason |
|---|----------|--------|
| 11 | **NVS-backed tunables**, defaults in `struct Tunables tun` overridden by `Preferences` on boot | Settings survive reboot; deep-sleep iteration in the future doesn't need a different persistence layer |
| 12 | **String enums on the wire** for `afe_gb` (`"indoor"`/`"outdoor"`) and `modem_sleep` (`"max"`/`"min"`) | Node-RED operators shouldn't need to know `AFE_GB = 0x0E` vs `0x12`; firmware translates to register bits |
| 13 | **Range-validated**, per-key, in `handleSet` | A bad command publishes a descriptive ack failure rather than corrupting the chip's register |
| 14 | **Republish retained `status` after every successful command** | UIs that just subscribe to `status` get the current state without needing to interpret acks |
| 15 | **`cmd/ack` is NOT retained** | Late-attaching subscribers shouldn't see stale per-command success/failure of someone else's command; for "current persistent state," subscribe to `status` instead |
| 16 | **TUN_CAP calibration is an `action`, not a tunable** | It's a procedure with a side-effecting result (whatever value won the sweep), not a value the operator sets directly. Result lands in `tun.tun_cap` + NVS, status republished |
| 17 | **MQTT keepalive pumped inside the calibration loop** | The sweep takes ~35 s. Without `mqtt.loop()` calls in the sample wait, PubSubClient drops the connection on the broker's keepalive timeout, and the ESP32 reboots before publishing the result |
| 18 | **Boot-time `loadTunables()` happens before `as3935Init()`** | `as3935Init()` reads `tun.*` to apply settings — wrong order would write the defaults to the chip and then silently desync from NVS until the next command |
| 19 | **WiFi log level pinned to `ESP_LOG_ERROR`** | Chatty CCMP-replay / AUTH_FAIL info noise drowns out the genuinely interesting log lines |
| 20 | **No-publish watchdog: 10 min without a successful publish → `ESP.restart()`** | Field-deploy failure mode: WiFi reconnects but MQTT silently doesn't. Restart is the only reliable recovery from "looks fine, isn't publishing" |
| 21 | **MQTT fail counter: 60 × 5 s = 5 min of connect failures → `ESP.restart()`** | Bounded retry, avoid the "wedged forever" mode |

### Open questions / for v0.3.0+

| # | Question | Notes |
|---|----------|-------|
| E | Sleep-on-IRQ mode for battery operation | v0.1.0/0.2.0 keeps WiFi always-on. Once the sensor is on solar+18650 outdoors and `as3935-bridge` is the only thing running on the ESP32, `esp_sleep_enable_ext0_wakeup(PIN_AS3935_IRQ, HIGH)` lets the chip wake on real strikes. Power budget changes from ~150 mA average to ~5 mA. Needs a re-think of heartbeat cadence (don't wake just to publish "I'm fine"). |
| F | OTA updates (Q-C from v0.1.0) | Still open. With NVS persistence settled, OTA is the next step — climbing on the roof to USB-flash a sealed box is the obvious motivation. ArduinoOTA on the same WiFi is the simplest path |
| G | Whether to expose `noise_floor_raw` / per-event metadata on the status topic | Currently status carries human-friendly `nf`, `wdth`, `srej`. A diagnostic mode could also report `REG_CFG0/1/2` raw values for debugging |
| H | Boot-button mapping during operation | Currently `BOOT` held 3 s → erase WiFi creds + portal. Could overload it to also trigger TUN_CAP calibration with a different hold duration (e.g. 5 s) so the box is field-recalibratable without MQTT |

### Lessons learned during the v0.2.0 build

- **`as3935Modify(reg, mask, bits)`** as a primitive turned out to be a force multiplier — every parameter writer is one line. Avoids the bug class of "I forgot to preserve the other bits in this byte."
- **ArduinoJson v7 `JsonDocument`** (no template size) is the right call for variable-size payloads; the v6-style `StaticJsonDocument<N>` doesn't compose with optional fields.
- **PubSubClient's 256-byte default buffer is too small** for the status payload (which has ~12 fields). `mqtt.setBufferSize(512)` fixes silent dropped publishes. The failure mode is `publish()` returning true but the broker seeing nothing — easy to miss.
- **NVS namespace must be opened read-only when only reading** (`p.begin(NS, true)`) and read/write when writing (`p.begin(NS, false)`). Mixing causes silent write failures on the first commit after a fresh flash.
- **First-ever NVS commit on a fresh namespace takes ~1 s longer than steady-state.** When a command path ends with `ESP.restart()` (the `reboot` action), the firmware needs `delay(1500)` after `publishAck` to let NVS flush before the restart hits.
- **`calibrateTunCap()` must `detachInterrupt` the normal AS3935 ISR first** and re-attach at the end. Otherwise the high-rate LCO edges trigger `handleAs3935Event()` on every pulse and the I²C bus is hammered with status reads that compete with the calibration's own reads.

### Source of truth (unchanged from v0.1.x)

Node-RED Lightning Antenna Protector flow (`vu2cpl-shack` repo, tab id
`75e2cac8ab96f556`) is the contract consumer. Any rename of
`status` / `hb` / `event` keys breaks the dashboard — extend, don't
rename. `cmd` / `cmd/ack` are new in v0.2.0 and only consumed by
admin-side widgets, so additions there are safer.

---

## 2026-05-15 — example flow caught up to shack + comprehensive test plan

No firmware change. Pure tooling under `nodered/` — the example flow
that ships in this repo was lagging behind what's actually deployed
on `noderedpi4`. Two waves of catch-up this session.

### Wave 1 — Tuning panel parity with shack

Five edits to bring the imported panel (`build-flow.py` →
`as3935-control-flow.json`) up to what's live in
`vu2cpl-shack/flows.json`:

- **AFE GB tunable row** added to the panel with its own toggle —
  chip goes green on `outdoor`, amber on `indoor`. Previously AFE GB
  was only readable from the Calib line.
- **Calib line drops the `afe_gb=…` segment** since AFE GB now has
  its own row.
- **`.meta` colour brightened** from `--muted` (#8b949e) to
  `--text` (#c9d1d9) for legibility.
- **JS IIFE switched to Pattern B** `(function(scope){…})(scope)`
  matching the chrony card — explicit `scope` parameter instead of
  relying on closure capture from the surrounding controller. The
  closure-capture form worked but the parameter form is the pattern
  used elsewhere in the shack dashboard.
- **`a35.toggleAfe()` method** wired to the new AFE GB toggle
  button.

Plus the **cache-and-replay rehydration pattern** the shack ships:
three `Cache /status` · `Cache /hb` · `Cache /cmd_ack` function
nodes stash MQTT payloads in flow context, a 5-second
`Replay every 5s` inject re-emits them with `msg.topic` preserved.
Worst-case rehydration after opening the dashboard cold is now 5 s
without needing to hit *Republish Status* on the ESP32. The replay
path is the workaround for `ui_control` being absent from
`node-red-dashboard 3.6.6` (confirmed by `--force` reinstall, files
genuinely missing).

### Wave 2 — Events panel + 5-phase test plan

A second `ui_template` (group `as3935_evt_grp`, sibling to the
Tuning group on tab id `bcce4e07ac31b882`):

- **Last Event card** — large icon, colour-coded summary
  (red / amber / muted), ISO timestamp + live `Xs ago` (1 Hz
  client-side tick). Backed by retained
  `lightning/as3935/last_event` (the topic added in commit
  `f895d71`), so it **survives Node-RED + browser refresh** with
  ≤5 s rehydration via a new `Cache /last_event` +
  `Replay last_event (5s tick)` pair that shares the same replay
  tick as the Tuning panel.
- **Session counters** — `⚡/⚶/📡` per-tab tallies (browser-session
  scoped, intentionally not persisted).
- **Recent events log** — newest-first, capped at 30, with
  distance (incl. `out of range` for the chip's sentinel
  `distance=63` and `overhead` for `distance=0`) and raw energy on
  lightning rows.

Plus **five TEST inject buttons** wired to a single
`TEST publish → lightning/as3935` mqtt out: `⚡ @ 5 km`,
`⚡ @ 25 km`, `⚡ OOR (distance=63)`, `⚠ disturber`, `📡 noise`.
Each press publishes one fake event so the Events panel can be
exercised without the ESP32.

`nodered/README.md` now carries a **5-phase comprehensive test
plan** (~10 min total): Tuning sanity → Events via TEST injects →
retained-rehydration via `mosquitto_pub -r` (with the exact
one-liner) → live ESP32 end-to-end → regression smoke checklist.

### Why this matters (and why now)

The example flow in this repo is what downstream consumers
*import* — it should mirror what's actually running, otherwise
people who clone the repo get an out-of-date starter. The shack
flow had drifted ahead during the 2026-05-13/14/15 dashboard
work (commits `45d4e17`, `b2aa5d2`, `6dab37f`, `f75e147`,
`027e3b4`) and this catches the example back up to the same
codebase.

### Open

- The Open-Meteo CAPE integration, antenna-disconnect matrix,
  threshold logic, JSONL persistence (Pi-side filesystem with
  hardcoded paths), and bypass handler from the shack flow are
  **not** ported into the example — they're shack-specific policy /
  hardware control, not bridge-level concerns. A clone of this repo
  building its own dashboard shouldn't inherit them.
- The rolling event log in the Events panel is intentionally
  session-only (browser refresh clears it). If a future consumer
  wants persistent history they can either (a) copy the JSONL
  pattern from `vu2cpl-shack/flows.json` or (b) point Node-RED at
  an InfluxDB / similar — out of scope for the bridge example.

### Source of truth

The example flow source remains `nodered/build-flow.py`. JSON in
`nodered/as3935-control-flow.json` is generated — never hand-edit.
Run `python3 nodered/build-flow.py` to regenerate after any panel
edit. Re-import or hot-reload in Node-RED to see the change.

---

*73 de VU2CPL*
