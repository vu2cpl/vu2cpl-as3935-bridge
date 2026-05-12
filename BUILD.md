# BUILD — vu2cpl-as3935-bridge

Stage-by-stage build → flash → tune → install. Read top-to-bottom
for the first build; later you can jump straight to whichever stage
you need (re-calibration, re-flash, etc.).

| Stage | What | When |
|-------|------|------|
| [0](#stage-0--prerequisites) | Toolchain on your Mac | Once per builder machine |
| [1](#stage-1--bill-of-materials) | Buy parts | Once |
| [2](#stage-2--wiring) | Solder / breadboard | Once |
| [3](#stage-3--firmware-build--flash) | Build & flash | Each firmware change |
| [3a](#stage-3a--first-boot-wifi-setup-captive-portal) | WiFi captive portal | First boot after flash |
| [4](#stage-4--tun_cap-calibration) | LC tank tuning | After every physical layout change |
| [4a](#stage-4a--node-red-dashboard-import) | Live tuning UI | Once on the shack Pi |
| [5](#stage-5--field-install) | Mount + seal | Once |
| [6](#stage-6--failure-mode-reference) | Troubleshoot | When something breaks |

---

## Stage 0 — Prerequisites

A macOS or Linux machine with:

- **Python 3.9+** — already on macOS via Homebrew (`brew install python`) or built-in on most Linuxes.
- **PlatformIO CLI**:
  ```sh
  pip3 install -U platformio
  ```
  Installs `pio` and `platformio` into `/opt/homebrew/bin` (macOS) or
  `~/.local/bin` (Linux). First `pio run` will download the ESP32
  toolchain (~500 MB) — give it a couple of minutes.
- **A data-capable USB cable** for the ESP32 dev board. A surprising
  number of "USB cables" are power-only; if the board doesn't appear
  on a `ls /dev/cu.usb*` after plugging in, try a different cable
  before suspecting anything else.
- **macOS CH340 driver** — most NodeMCU-32 dev boards ship with a CH340
  USB-serial chip. macOS 13 (Ventura) and later include the driver in
  the kernel — no install needed. Older macOS may need
  [WCH's macOS CH340 driver](https://www.wch.cn/downloads/CH341SER_MAC_ZIP.html).
  After plugging in the ESP32, you should see `/dev/cu.usbserial-XXXX`
  appear within ~2 s.
- **A way to flash the broker** (`mosquitto-clients` for the test
  commands): `brew install mosquitto` on macOS, `apt install mosquitto-clients` on Debian/Ubuntu/Raspberry Pi OS.

> **If your Mac already has FTDI radio CAT cables connected**, the
> ESP32 will *not* be the only `/dev/cu.usbserial-*` device on the
> machine. Identify which one is the ESP32 by unplugging it,
> running `ls /dev/cu.usb*`, plugging it back in, and `ls`-ing
> again — the one that newly appeared is yours. The repo's
> `platformio.ini` pins `/dev/cu.usbserial-0001`; if your ESP32
> enumerates differently, edit `upload_port` and `monitor_port` in
> `platformio.ini` to match. CH340 boards typically appear as
> `usbserial-0001`; FTDI boards as `usbserial-XXXXXXXX` (8 hex
> chars from the chip's serial number).

---

## Stage 1 — Bill of materials

| Item | Qty | Notes |
|------|-----|-------|
| ESP-WROOM-32 NodeMCU dev board | 1 | The common 30- or 38-pin module; CH340 USB-serial preferred over CP210x for macOS plug-and-play |
| AS3935 module | 1 | I²C variant (CJMCU-3935 / GY-AS3935); ferrite antenna onboard |
| 18650 Li-ion cell + holder | 1 | 2500-3000 mAh, button-top |
| TP4056 charging module | 1 | **Get the BMS-equipped variant** (DW01A + FS8205A, four pads on the battery side, not two — the bare TP4056 will over-discharge the cell) |
| Solar panel | 1 | 5 V / 1-2 W; mount in **direct sun**, even if the enclosure is in shade — use a long cable |
| IP65 enclosure | 1 | ABS plastic, gasket-sealed; ~15 × 10 × 7 cm minimum |
| Antenna cable + grounding strap | — | For mounting AS3935 board away from ESP32's RF noise |
| Cable gland | 1 | For solar input cable into the enclosure |
| Mounting hardware | — | Bracket / pole clamp for shade mounting |

See [WIRING.md](WIRING.md) for the full picture, wire-by-wire table,
and silkscreen-label gotchas (the AS3935 module labels the I²C pins
**`D`** for SDA and **`C`** for SCL).

### Compatible ESP32 variants

The bench is built on a vanilla **ESP-WROOM-32 NodeMCU dev board**
and that's what every doc here is validated against. The firmware
itself only needs these minimums:

| Requirement | Min | Why |
|---|---|---|
| Chip family | ESP32, S2, S3, C3, or C6 | WiFi 2.4 GHz + Arduino-ESP32 framework |
| Flash | **4 MB** | Firmware is ~937 KB; default partition table needs a 1.3 MB app slot |
| SRAM | 320 KB | Firmware uses ~48 KB; every ESP32 chip has ≥ 320 KB |
| Free GPIOs | 3 + BOOT | I²C SDA + SCL + AS3935 IRQ; BOOT for factory-reset |
| RTC-capable pin for IRQ | yes | Reserved for future deep-sleep EXT0 wake. Current GPIO27 is RTC-capable on all ESP32 variants |
| USB-serial chip | CH340 / CP210x / FTDI / native USB-OTG | First-flash + serial monitor |
| 3.3 V LDO onboard | yes | To power AS3935 from VIN |

Not required: Bluetooth, PSRAM, dual core, display, USB-OTG host.

**Recommended**: stick with ESP-WROOM-32 / NodeMCU-32. ~₹250-400 in
India, CH340 plug-and-play on macOS 13+, plenty of flash headroom
for future OTA.

**Possible alternatives** (untested — try at your own bench):
- **ESP32-C3-DevKitM-1**: cheaper, smaller, RISC-V. Pin map differs
  (no GPIO21/22 by default) — would need `PIN_I2C_SDA` / `PIN_I2C_SCL`
  remapped in `main.cpp`. Lower idle current than ESP32 classic, so a
  good candidate for v0.3 deep-sleep work.
- **ESP32-S3**: more GPIOs, native USB, fine but no real advantage
  for this project.

**Explicitly NOT supported**:
- **ESP8266** — different SDK (no `Preferences`, no EXT0 wake, no
  GPIO27), weaker WiFi. HANDOVER decision #1 ruled it out.
- **ESP32-H2** — no WiFi (Thread/BLE only), so no MQTT.

---

## Stage 2 — Wiring

See [WIRING.md](WIRING.md) for the ASCII connection diagram and the
full wire table.

Summary:

| AS3935 (silkscreen) | ESP32 GPIO | Notes |
|---------------------|------------|-------|
| `VCC` | 3V3 | From ESP32 regulator |
| `GND` | GND | Common |
| `D` (= SDA) | GPIO21 | ESP32 default I²C SDA |
| `C` (= SCL) | GPIO22 | ESP32 default I²C SCL |
| `IRQ` | GPIO27 | RTC-capable GPIO (room for future EXT0 wake) |
| `SI` | VCC | Selects I²C mode (vs SPI) |
| `A0` + `A1` | GND | Selects I²C address `0x03` |
| `MOSI` / `MISO` / `CS` | — | Leave unconnected |

**Power chain:**

```
Solar panel → TP4056 IN+/IN-
TP4056 B+/B- ↔ 18650 +/-       (battery sits between B pads)
TP4056 OUT+/OUT- → ESP32 VIN/GND
```

Use a TP4056 with **protection** (4 pads on the battery side).

---

## Stage 3 — Firmware build & flash

Requires Stage 0 prerequisites and the ESP32 plugged in via USB.

### Path A — interactive installer (recommended for forks)

```sh
git clone https://github.com/vu2cpl/vu2cpl-as3935-bridge.git
cd vu2cpl-as3935-bridge
python3 install.py
```

`install.py` prompts for your MQTT broker, captive-portal AP
credentials, timezone, serial port, and (optionally) Node-RED
IDs. It patches `src/main.cpp`, `platformio.ini`, and
`nodered/build-flow.py` in place, regenerates the Node-RED flow
JSON, and builds the firmware (but does **not** flash — you trigger
that yourself). Re-run any time you want to change settings.

Then to flash:

```sh
pio run -t upload
pio device monitor
```

### Path B — manual config (for the maintainer / bench rebuilds)

If you've already cloned and don't need to change defaults:

```sh
pio run -t upload
pio device monitor
```

- `pio run -t upload` — builds and writes the firmware. First run
  downloads the ESP32 toolchain (~500 MB, takes a few min).
- `pio device monitor` — opens a serial monitor at 115200 baud.
  Press `Ctrl+]` to exit.

There is no `secrets.h` to edit. WiFi credentials are entered via a
captive portal on first boot (Stage 3a). The MQTT broker host is
hardcoded to `192.168.1.169:1883` in `src/main.cpp` — change there
or rerun `install.py` if you have a different LAN broker.

### Stage 3a — First-boot WiFi setup (captive portal)

On first power-on the ESP32 raises its own WiFi AP because it has no
stored credentials yet:

| Field | Value |
|-------|-------|
| AP SSID | `vu2cpl-as3935-setup` |
| AP password | `vu2cpl1234` |
| Captive portal URL | `http://192.168.4.1` (most phones auto-open it) |

Steps:

1. Power on the ESP32 (USB is fine for first setup).
2. On your phone, connect to the WiFi network
   `vu2cpl-as3935-setup` (password `vu2cpl1234`).
3. The captive portal usually opens automatically. If not, open a
   browser and visit `http://192.168.4.1`.
4. Tap **Configure WiFi**, pick your shack AP, type the password,
   save.
5. The ESP32 reboots into normal mode and stays connected to your
   AP from now on. The setup AP is gone.

To re-configure WiFi later (changed AP, new password, moved house):

- Hold the **BOOT button** on the NodeMCU for 3 s at power-on.
- *Or* publish `{"action":"factory_reset_wifi"}` to
  `lightning/as3935/cmd`.

Both erase stored credentials and re-open the setup AP.

### Expected boot output (v0.2.0)

```
ets Jul 29 2019 12:21:46
rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)
...
[boot] vu2cpl-as3935-bridge v0.2.0
[nvs] nf=4 wdth=2 srej=2 tun_cap=10 mask=0 min_lt=0 indoor=0 ps_max=1
[wifi] auto-connect; if no stored creds, AP=vu2cpl-as3935-setup pass=vu2cpl1234
[wifi] connected, RSSI -57 dBm, IP 192.168.1.xxx
[wifi] modem sleep = max
[ntp] synced, bootEpoch=1778557344
[mqtt] connecting to 192.168.1.169:1883 (attempt 1/60)
[mqtt] connected, LWT armed
[mqtt] subscribed to lightning/as3935/cmd
[as3935] CFG0=0x1C (i2c addr 0x03)
[as3935] applied: nf=4 wdth=2 srej=2 tun_cap=10 mask_dist=0 min_lt=0 afe_gb=outdoor
[as3935] CALIB_RCO TRCO=OK (0xA3) SRCO=OK (0xA4)
[as3935] cleared pending INT: 0x0
[mqtt] status: {"event":"ready", ... ,"fw":"v0.2.0", ...}
[loop] entering main loop, IRQ on GPIO27
```

On the shack Pi side, verify:

```sh
mosquitto_sub -h 192.168.1.169 -t 'lightning/as3935/#' -v
```

Should see the retained `status` immediately, then `hb` every 30 s.

---

## Stage 4 — TUN_CAP calibration

The AS3935's LC tank antenna needs trimming with the internal `TUN_CAP`
capacitor (0..15, ~8 pF/step) to land at 500 kHz ± 3.5 %. Stray
capacitance from your wiring + enclosure shifts the optimum cap value,
so calibration is **physical-layout-dependent** — re-run any time you
re-seal the box or reposition the antenna.

**Run it remotely from the shack:**

```sh
mosquitto_pub -h 192.168.1.169 -t lightning/as3935/cmd \
    -m '{"action":"calibrate_tun_cap"}'
```

…or from the Node-RED dashboard (Stage 4a) — click the
**Calibrate TUN_CAP (~35s)** button.

The ESP32 spends ~35 s sweeping. Watch the serial monitor:

```
[calib] starting TUN_CAP sweep
[calib] cap= 0 edges=  3768 freq=241152 Hz err=-51.77%
[calib] cap= 1 edges=  4892 freq=313088 Hz err=-37.38%
...
[calib] cap=11 edges=  7820 freq=500480 Hz err=+0.10%
...
[calib] cap=15 edges=  8624 freq=551936 Hz err=+10.39%
[calib] best cap=11 freq=500480 Hz err=+0.10% (in spec)
```

The winning cap value is automatically persisted to NVS, applied to
the chip, and reflected in the next status publish. **No re-flash, no
opening the enclosure.**

`(OUT OF SPEC)` on the best line means no cap setting got within
±3.5 %. Likely causes:

- Antenna ferrite damaged or wrong inductance.
- Long wires between antenna and chip adding stray capacitance.
- Strong RF / power coupling into the LC tank.

### All other tunables work the same way

`nf`, `wdth`, `srej`, `mask_dist`, `min_num_lightning`, `afe_gb`,
`modem_sleep` are all live-tunable over the same MQTT cmd topic.
Each value is validated, applied immediately, persisted to NVS, and
re-published in the next status. See
[`nodered/README.md`](nodered/README.md) for the full schema and
[`nodered/cmd-examples.sh`](nodered/cmd-examples.sh) for ready-to-paste
one-liners.

---

## Stage 4a — Node-RED dashboard import

The dashboard panel lives at
[`nodered/as3935-control-flow.json`](nodered/as3935-control-flow.json).
It styles itself to match the existing Lightning Protection panel
(GitHub-dark palette, LED indicator, rounded cards) and drops into
the **Shack Monitoring tools** tab as the "AS3935 Tuning" group.

On `noderedpi4`:

```sh
# If the repo is cloned on the Pi:
cd ~/vu2cpl-as3935-bridge && git pull

# Otherwise scp the single file from your laptop:
scp nodered/as3935-control-flow.json vu2cpl@noderedpi4:/tmp/
```

In Node-RED (browser at `http://noderedpi4:1880`):

1. Menu (☰, top-right) → **Import**.
2. **select a file to import** → pick `as3935-control-flow.json`,
   *or* paste the JSON contents into the textbox.
3. Choose **import to new flow** → **Import** → **Deploy**.

Open the dashboard (`http://noderedpi4:1880/ui` or your custom path)
and navigate to **Shack Monitoring tools > AS3935 Tuning**. The panel
should populate with the retained `status` and `hb` values within
~1 s. LED goes green when `event:"ready"` and both calibrations are
`OK`.

See [`nodered/README.md`](nodered/README.md) for layout, command
schema, and editing instructions (HTML/CSS/JS source is in
`nodered/build-flow.py`).

---

## Stage 5 — Field install

1. **Mount enclosure in shade.** Direct Bengaluru sun → enclosure
   hits 60-70 °C → 18650 won't charge (TP4056 cutoff above ~45 °C)
   and degrades fast. Tree-shaded balcony or roof-eave-shaded south
   wall is fine.
2. **Mount the solar panel itself in direct sun**, even if the
   enclosure is in shade. Long cable from panel to enclosure. A
   shaded panel can't keep up with 24 h × 100-150 mA draw.
3. **Antenna orientation: vertical** for best omnidirectional
   reception with the AS3935 ferrite. Worth a 2-3 day comparison if
   you can rotate.
4. **Verify WiFi RSSI at the install location *before sealing***.
   Mount the assembled board, power it up, watch the retained
   status on the broker — `rssi` should be better than `-70 dBm`.
   If it's marginal, the ESP32's internal antenna will be worse
   inside the sealed enclosure; consider an external-antenna ESP32
   variant + pigtail.
5. **Run solar panel cable through the cable gland**; seal with
   silicone before mounting.
6. **Re-run TUN_CAP calibration after sealing** — stray
   capacitance shifts noticeably when the enclosure closes. From the
   shack:
   ```sh
   mosquitto_pub -h 192.168.1.169 -t lightning/as3935/cmd \
       -m '{"action":"calibrate_tun_cap"}'
   ```
   Or click **Calibrate TUN_CAP** in the dashboard.
7. **Watch for the first 24 h.** Both panels should be green:
   - **Lightning Protection** (main shack tab) — operational
     view, shows live strikes.
   - **AS3935 Tuning** (Monitoring tools tab) — RSSI, uptime, IRQ
     counters, calib status. If the LED stays amber/red, dig into
     the failure-mode table below.

---

## Stage 6 — Failure-mode reference

| Symptom | Likely cause | Check |
|---------|--------------|-------|
| Sensor offline in dashboard | LWT fired — ESP32 lost broker connection | Power (battery voltage), WiFi RSSI from install location, MQTT reachability from the bridge's IP |
| `i2cWriteReadNonStop returned Error -1` on every read; `CFG0=0xFF` | AS3935 not responding on I²C | Swap SDA/SCL (silkscreen `D`/`C`, not `SDA`/`SCL`); verify SI is tied to VCC; verify VCC has 3.3 V vs GND with a meter |
| Constant noise events (`noise` counter climbing fast) | NF too low for environment | From shack: `mosquitto_pub -t lightning/as3935/cmd -m '{"set":"nf","value":5}'` (or higher up to 7) |
| Constant disturber events | Local RF (ham gear nearby, motor brushes, switching PSU) | Mask them: `{"set":"mask_dist","value":true}`. Or raise `wdth` / `srej`. |
| `distance=63` reported continuously | "Out of range" code — storm overhead OR LC tank far off frequency | Wait for clear weather, then re-run `calibrate_tun_cap` |
| Battery drains overnight | Solar undersized / panel in shade / cell ageing | Measure mid-day TP4056 IN voltage; cell should sit ≥ 4.0 V after a sunny day. Verify panel itself is in direct sun, not just the enclosure |
| WiFi reconnects flapping | Marginal RSSI (-70 dBm or worse), or AP DHCP lease churn | Bump enclosure higher; static-lease the ESP32 in your router |
| `pio run -t upload` fails "Resource busy" | Another process holds the serial port (often `pio device monitor` in another terminal) | `lsof /dev/cu.usbserial-XXXX` to find the holder, kill it, retry |
| `pio run -t upload` fails "Could not open ... doesn't exist" | Wrong port pinned in `platformio.ini`, or USB cable is power-only | `ls /dev/cu.usb*` with the board unplugged then plugged in; whichever device newly appears is the right one. Update `upload_port` and `monitor_port` in `platformio.ini` |
| Dashboard panel stuck at "awaiting status..." / "awaiting heartbeat..." | ui_template's `scope.$watch` not firing — usually a Node-RED `ui_template` import issue | Verify the imported template did not shadow `scope`. Hard-refresh the dashboard (Cmd+Shift+R). Check browser DevTools console for JS errors. |
| cmd ack topic stays silent | Command's JSON malformed by shell quoting (most common: missing single quotes around the JSON), or wrong topic name | Wrap the cmd payload in single quotes: `mosquitto_pub … -m '{"action":"republish_status"}'`. Verify ESP32 boot log shows `[mqtt] subscribed to lightning/as3935/cmd` |
| Status JSON appears retained as "indoor offline" | Old Pi-side `as3935_mqtt.py` daemon is still publishing | On `noderedpi4`: `sudo systemctl stop as3935.service && sudo systemctl disable as3935.service` |

For anything else, the serial monitor on the ESP32 is the ground
truth — boot log + `[cmd]` / `[as3935]` / `[mqtt]` traces tell you
exactly what the bridge is doing.
