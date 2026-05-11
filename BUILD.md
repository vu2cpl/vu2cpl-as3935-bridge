# BUILD — vu2cpl-as3935-bridge

Stage-by-stage build + flash + field install. Follow once on the
bench, again at install location.

---

## Stage 1 — Bill of materials

| Item | Qty | Notes |
|------|-----|-------|
| ESP-WROOM-32 NodeMCU dev board | 1 | The common 30- or 38-pin module |
| AS3935 module | 1 | I²C variant (CJMCU-3935 or similar); ferrite antenna onboard |
| 18650 Li-ion cell + holder | 1 | 2500-3000 mAh, button-top |
| TP4056 charging module | 1 | **Get the BMS-equipped variant** (overcharge / over-discharge protection) |
| Solar panel | 1 | 5 V / 1-2 W; output regulated or via TP4056's input range |
| 3.3 V LDO | 0-1 | NodeMCU has onboard AMS1117; skip unless wiring bare WROOM module |
| IP65 enclosure | 1 | ABS plastic, gasket-sealed; ~15 × 10 × 7 cm minimum |
| Antenna cable + grounding strap | — | For mounting AS3935 board away from ESP32's RF noise inside enclosure |
| Cable gland | 1 | For solar input cable into enclosure |
| Mounting hardware | — | Bracket / pole clamp for shade mounting |

---

## Stage 2 — Wiring

| AS3935 pin | ESP32 GPIO | Notes |
|------------|------------|-------|
| VCC | 3V3 | From ESP32 regulator |
| GND | GND | Common |
| SDA | GPIO21 | ESP32 default I²C SDA |
| SCL | GPIO22 | ESP32 default I²C SCL |
| IRQ | GPIO27 | RTC-capable GPIO (required for future EXT0 wake) |
| MOSI/MISO/CS | — | Tied to GND or VCC per module's I²C-mode jumpers |

**Power chain wiring:**

```
Solar panel  →  TP4056 IN+/IN-
TP4056 OUT+  →  18650 +     (TP4056 charges/protects the cell)
18650 +      →  ESP32 VIN   (NodeMCU onboard regulator drops to 3.3 V)
18650 -      →  ESP32 GND   (common ground with TP4056)
```

Verify the TP4056 module you have **includes a protection IC** (DW01A
+ FS8205A is the common combo). The standalone TP4056 without
protection will over-discharge the cell.

---

## Stage 3 — Firmware build & flash

Requires PlatformIO CLI or the VS Code extension.

```sh
git clone https://github.com/vu2cpl/vu2cpl-as3935-bridge.git
cd vu2cpl-as3935-bridge
pio run -t upload
pio device monitor -b 115200
```

There is no `secrets.h` to edit. WiFi credentials are entered via a
captive portal on first boot (see Stage 3a below); the MQTT broker
host is hardcoded to `192.168.1.169:1883` in `src/main.cpp`.

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
2. On your phone, connect to the WiFi network `vu2cpl-as3935-setup`
   (password `vu2cpl1234`).
3. The captive portal usually opens automatically. If not, open a
   browser and visit `http://192.168.4.1`.
4. Tap **Configure WiFi**, pick your shack AP, type the password, save.
5. The ESP32 reboots into normal mode and stays connected to your AP
   from now on. The setup AP is gone.

To re-configure WiFi later (changed AP, new password, moved house) —
hold the **BOOT button** on the NodeMCU for 3 seconds at power-on.
This erases stored credentials and re-opens the setup AP.

### Expected boot output (after WiFi is configured)

```
[boot] vu2cpl-as3935-bridge v0.1.0
[wifi] auto-connect; if no stored creds, AP=vu2cpl-as3935-setup pass=vu2cpl1234
[wifi] connected, RSSI -52 dBm, IP 192.168.1.xxx
[ntp] synced, bootEpoch=1762885200
[mqtt] connecting to 192.168.1.169:1883...
[mqtt] connected, LWT armed
[as3935] CFG0=0x24 (i2c addr 0x03)
[as3935] antenna=outdoor CFG0=0x1C
[as3935] NF=4 CFG1=0x42
[as3935] TUN_CAP=10 (~80 pF) REG0x08=0x0A
[as3935] CALIB_RCO TRCO=OK (0x80) SRCO=OK (0x80)
[as3935] cleared pending INT: 0x0
[mqtt] status: {...}
[loop] entering main loop, IRQ on GPIO27
```

On the shack Pi side, verify:

```sh
mosquitto_sub -h 192.168.1.169 -t 'lightning/as3935/#' -v
```

Should see the retained `status` immediately, then `hb` every 30 s.

---

## Stage 4 — TUN_CAP calibration (one-shot per physical install)

Hold BOOT button at power-on (or use serial command `cal` once
implemented) to enter calibration mode. The firmware then:

1. Sets AS3935 INT to mode 3 (LCO output on IRQ pin).
2. Sweeps `TUN_CAP` from 0 to 15.
3. For each value, gates the IRQ pin for 100 ms and counts edges.
4. Each edge ≈ 16 LCO cycles; target frequency is **500 kHz ± 3.5 %**.
5. Picks the `TUN_CAP` value closest to target, persists to NVS.
6. Switches INT back to normal lightning mode and continues.

Re-run this any time the physical layout changes (enclosure resealed,
antenna repositioned, etc.). The indoor daemon's `as3935_tune.py` is
the reference algorithm.

---

## Stage 5 — Field install

1. Mount enclosure in **shade**. South-facing window-sill is fine if
   shaded by a roof eave; a tree-shaded balcony is better; direct sun
   is fatal to the 18650 over a Bengaluru summer.
2. Antenna orientation: vertical typically best for the AS3935
   ferrite, but worth a quick 2-3 day comparison if you can rotate.
3. Run solar panel cable through the cable gland; seal with silicone.
4. Verify WiFi RSSI at the install location **before sealing** —
   ideally better than -70 dBm. If marginal, an external antenna ESP32
   module + pigtail is worth the upgrade.
5. Re-run Stage 4 (TUN_CAP cal) after sealing.
6. Watch the Node-RED dashboard's AS3935 panel for the first 24 h —
   should see `READY · NF=4 · up Nm`. If the panel goes muted
   ("offline"), the LWT fired — check WiFi, battery voltage, MQTT.

---

## Stage 6 — Failure-mode reference

| Symptom | Likely cause | Check |
|---------|--------------|-------|
| Sensor offline in dashboard | LWT fired | Power, WiFi RSSI, MQTT reachability from sensor |
| Constant noise events | NF too low for environment OR uncalibrated TUN_CAP | Rerun Stage 4; bump NF to 5-6 if local noise floor is high |
| Battery drains overnight | Solar undersized OR cell ageing | Measure mid-day TP4056 IN voltage; verify cell ≥ 4.0 V after a sunny day |
| WiFi reconnects flapping | Marginal RSSI OR AP DHCP lease issues | Bump enclosure higher, or static-lease the ESP32 in your router |
| AS3935 reports `distance=63` continuously | Storm overhead OR LC tank far off frequency | Wait for clear weather, rerun TUN_CAP cal |
