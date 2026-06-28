# WIRING — vu2cpl-as3935-bridge

One picture of the whole bridge: ESP32 + AS3935 + 18650 power chain
+ solar input. Build it on the bench first, exactly like this, then
move it into the IP65 enclosure.

---

## The whole thing

```
        ┌───────────────────────────┐
        │     Solar panel 5 V       │
        │      1 – 2 W              │
        └────────┬──────────────┬───┘
                 │ +            │ −
                 ▼              ▼
        ┌─────────────────────────────┐
        │  TP4056 with protection IC  │
        │  (DW01A + FS8205A variant)  │
        │                             │
        │   IN+   IN−   B+  B−   OUT+  OUT− │
        └────┬─────┬────┬───┬─────┬─────┬───┘
             │     │    │   │     │     │
        (from panel)    │   │     │     │
                        ▼   ▼     │     │
                  ┌──────────┐    │     │
                  │  18650   │    │     │
                  │  Li-ion  │    │     │
                  │ 3000 mAh │    │     │
                  └──────────┘    │     │
                                  │     │
                                  ▼     ▼
                        ┌──────────────────────────────┐
                        │      ESP32 NodeMCU (38-pin)  │
                        │                              │
                        │   VIN ────────── + (from OUT+)│
                        │   GND ────────── − (from OUT−)│
                        │                              │
                        │   3V3 ──┐                    │
                        │   GND ──┼─┐                  │
                        │   G21 ──┼─┼─┐                │
                        │   G22 ──┼─┼─┼─┐              │
                        │   G27 ──┼─┼─┼─┼─┐            │
                        └─────────┼─┼─┼─┼─┼────────────┘
                                  │ │ │ │ │
                                  │ │ │ │ │
                        ┌─────────┼─┼─┼─┼─┼────────┐
                        │  AS3935 │ │ │ │ │ module │
                        │         ▼ ▼ ▼ ▼ ▼        │
                        │   VCC GND SDA SCL IRQ    │
                        │                          │
                        │   I²C mode jumpers:      │
                        │     SI  → VCC  (I²C on)  │
                        │     A0  → GND  ┐         │
                        │     A1  → GND  ┘ addr 0x03│
                        │                          │
                        │   [ ferrite loop antenna ] │
                        └──────────────────────────┘
```

---

## Wire list

### ESP32 ↔ AS3935

| AS3935 pin (datasheet) | Silkscreen label on this module | ESP32 NodeMCU pin | Wire colour suggestion |
|------------------------|----------------------------------|-------------------|------------------------|
| VCC        | `VCC`           | 3V3               | red                    |
| GND        | `GND`           | GND               | black                  |
| **SDA**    | **`D`**         | GPIO21            | blue                   |
| **SCL**    | **`C`**         | GPIO22            | yellow                 |
| IRQ        | `IRQ`           | GPIO27            | green                  |
| MOSI / MISO / CS | (NC on I²C-mode boards) | — (NC) | leave unconnected      |

> **Read the silkscreen carefully — the I²C pins are abbreviated.** On
> the CJMCU-3935 / GY-AS3935 modules used here the data line is
> labelled **`D`** (not `SDA`) and the clock line is labelled **`C`**
> (not `SCL`). It's an easy 30-minute debug detour. `D → GPIO21`,
> `C → GPIO22`.

> **Address-select switches (SI / A0 / A1) — leave at factory default.**
> These are **on-board DIP / solder switches**, not wires you run to
> the ESP32. The factory default position selects I²C mode + address
> **`0x03`**, which is what the firmware expects
> (`AS3935_I2C_ADDR = 0x03` in `main.cpp`). Don't touch them.
> Sanity check after first power-on: if `[as3935] CFG0=0x00` or
> `0xFF` keeps appearing on the serial monitor, one of the switches
> has been flipped — power down, restore to default, retry.

### Power chain

| Wire | From | To | Notes |
|------|------|----|-------|
| Solar + | Panel + | TP4056 **IN+** | 5 V panel; TP4056 accepts 4.5 – 5.5 V |
| Solar − | Panel − | TP4056 **IN−** | |
| Battery + | 18650 + (top, button) | TP4056 **B+** | |
| Battery − | 18650 − (bottom, flat) | TP4056 **B−** | |
| Load + | TP4056 **OUT+** | ESP32 **VIN** | NodeMCU regulates to 3.3 V on-board |
| Load − | TP4056 **OUT−** | ESP32 **GND** | common ground with everything else |

> **Critical: use a TP4056 board with protection.** The bare TP4056
> chip only charges — it does **not** protect against over-discharge.
> Buy the variant labelled "TP4056 with protection" / "TP4056 + DW01A
> + FS8205A". Visually it has **four pads** on the battery side
> (B+/B−/OUT+/OUT−), not just two. The unprotected version has only
> B+/B− and will eventually kill your 18650 by letting it discharge
> below ~2.5 V.

### Battery voltage divider (v0.3.0+, required for outdoor deploy)

Hardware add-on so the firmware can publish `vbat_mv` over MQTT and
the dashboard can show 🔋 voltage + a derived %SOC. Without this
mod the firmware still boots and runs — it just reports ~0 V for
the battery, which is the visual cue that the divider isn't wired.

```
                        TP4056 OUT+ ──┬── ESP32 VIN  (existing wire)
                                      │
                                      │
                            R1 = 100 kΩ
                                      │
                                      ├────┬─── ESP32 GPIO 34   ← ADC tap
                                      │    │      (silkscreen: G34)
                            R2 = 100 kΩ   C1 = 100 nF
                                      │    │
                                      └────┴─── ESP32 GND   (existing common)
```

> **Read the silkscreen carefully — GPIO 34 is `G34` on this board**,
> not `A0`. ESP32 NodeMCU boards label ADC pins by GPIO number; the
> `A0` convention is ESP8266-only. On the bench's WROOM-32 38-pin
> module `G34` is the **5th pin down on the column opposite the USB
> connector**, just below `VN`. Sister cluster of input-only ADC1
> pins on the same edge: `VP` (= GPIO 36) · `VN` (= GPIO 39) ·
> **`G34`** · `G35`. Other clones may print it as `IO34`, `D34`, or
> bare `34` — all the same pin. The number is what matters.

| Component | Part | Notes |
|-----------|------|-------|
| R1, R2    | 100 kΩ 1/8 W 1% (metal film) | 1% so the divider ratio is predictable; ±5% carbon also works if you accept a ~50 mV reporting error |
| C1        | 100 nF ceramic (X7R 50 V)    | Mounted **at the GPIO 34 pin**, not at the divider — it shores up the ADC's S/H input impedance |
| Wire      | 22 AWG silicone | Short — keep the tap < 5 cm from the ESP32 |

**Tap point.** Tap **from TP4056 OUT+** (which goes to ESP32 VIN),
*not* from the battery's B+ directly. Reasons:

- OUT+ = battery voltage *minus* the protection MOSFET's R<sub>DS(on)</sub>
  drop (~20 mV at 100 mA, negligible) — close enough to true battery
  voltage for monitoring.
- B+ tapped directly would still read fine **except** during a
  protection-IC trip (over-discharge / over-current / short) when
  the MOSFET disconnects OUT from B. After a trip, B+ shows correct
  battery voltage but the ESP32 is dead because OUT is disconnected
  — so a B+ reading "looks fine" while the load can't see the
  battery. OUT+ tap gives the operationally meaningful number.

**Voltage range vs ADC linear region:**

| Battery state | BAT+ voltage | Voltage at GPIO 34 (post-divider) |
|---------------|--------------|-----------------------------------|
| Full          | 4.20 V       | 2.10 V                            |
| Nominal       | 3.70 V       | 1.85 V                            |
| Low alarm     | 3.30 V       | 1.65 V                            |
| Cutoff (DW01A) | 2.50 V      | 1.25 V                            |

All four are inside the ESP32 ADC's linear region at 11 dB
attenuation (~150 mV – 2.45 V). Below ~150 mV and above ~2.45 V the
ADC pinches non-linearly and `esp_adc_cal_raw_to_voltage()` returns
saturated values.

**Bleed current.** R1 + R2 = 200 kΩ. At 4.2 V: 21 µA. Over 24 h
that's 0.5 mWh — negligible against the 18650's ~10 Wh. Resistors
that small don't load the protection IC enough to affect anything.

**Why GPIO 34.** Input-only (so firmware can never accidentally
drive it as an output), ADC1 (WiFi-safe — ADC2 reads return
`ESP_ERR_TIMEOUT` while WiFi is active, which is always). Free on
this board — no other peripheral uses it.

**Calibration after install.** Solder the divider, flash v0.3.0,
measure BAT+ with a known-good DMM, compare to the dashboard's
`vbat_mv` reading. Typical delta: < 30 mV (most ESP32s have
factory-calibrated eFuse Vref). If the delta is > 50 mV, set it via
the `cmd` channel:

```sh
H=192.168.1.169
# Example: firmware reports 3.92 V but DMM measures 3.85 V → offset = -70
mosquitto_pub -h $H -t lightning/as3935/cmd \
    -m '{"set":"vbat_offset_mv","value":-70}'
```

The offset persists in NVS and is also exposed on `/status` as a
field for sanity-check.

---

## First-boot WiFi setup

The firmware does not bake WiFi creds in. On first power-on:

1. The ESP32 raises its own AP: **`vu2cpl-as3935-setup`** (password
   `vu2cpl1234`).
2. Connect your phone to that AP. The captive portal usually opens
   automatically; if not, browse to `http://192.168.4.1`.
3. Tap **Configure WiFi**, pick your shack AP, type the password,
   save. The ESP32 reboots and joins it.

To re-configure later (changed AP, new password): hold the
NodeMCU's **BOOT button** for 3 s at power-on. Stored credentials
are erased and the setup AP comes back up.

---

## Bench checklist

Before you seal anything:

1. **Power it from USB first.** Skip the battery / TP4056 / panel
   entirely. ESP32 NodeMCU's micro-USB → laptop. Verify the serial
   log shows `[as3935] CALIB_RCO TRCO=OK SRCO=OK`. If it doesn't,
   the I²C wiring is wrong — fix that before touching the power
   chain.
2. **Add the battery + TP4056 next.** Disconnect USB, plug the
   battery in, confirm the ESP32 boots from battery alone.
3. **Add the solar panel last.** With battery installed, connect the
   panel. In sunlight (or under a desk lamp close-up), TP4056's
   **red LED** lights = charging; **blue LED** = full. If neither
   lights with the panel in good light, the panel polarity or
   voltage is off.
4. **Verify on the broker side** before sealing the enclosure:
   ```sh
   mosquitto_sub -h 192.168.1.169 -t 'lightning/as3935/#' -v
   ```
   You should see the retained `status` message immediately, then
   `hb` every 30 s.

---

## Outdoor deployment — two-box topology (required)

The bench rig (everything in one box, sensor on the same PCB
cluster as the ESP32 + TP4056) **works fine indoors as a development
setup**, but **fails as a deployed sensor** — the AS3935's
ferrite-loop antenna picks up the ESP32's own digital switching
activity (CPU at 240 MHz, WiFi TX bursts every status republish,
CP2102 USB-serial harmonics, regulator transients) at higher field
strength than a 5 km storm. The result is **false `lightning`
events firing at ~1 km distance**, often correlated with the
firmware's 5-min `STATUS_REPUBLISH_MS` WiFi transmit cycle.
Documented from the field on 2026-06-28 — every successful AS3935
field deployment in published write-ups uses some variant of this
two-box pattern.

The fix is structural — physical separation, not strictness tuning:

```
                  ┌─────────────────────────┐
                  │  SENSOR BOX  (plastic)  │
                  │                         │
                  │   AS3935 module         │
                  │   + optional 100 nF     │
                  │     decoupling on VCC   │
                  │                         │
                  │   [ferrite antenna] ↕   │  ← vertical
                  └────┬────┬────┬────┬─────┘
                       │    │    │    │
                       │   ≥ 10 cm cable away from
                       │   anything else in the
                       │   control box. Run the
                       │   cable AWAY from the
                       │   antenna, not over it.
                       │
                       │   4 conductors:
                       │     VCC (3V3)
                       │     GND
                       │     SDA (D / GPIO 21)
                       │     SCL (C / GPIO 22)
                       │     IRQ (GPIO 27)
                       ▼
                  ┌─────────────────────────┐
                  │  CONTROL BOX  (plastic) │
                  │                         │
                  │   ESP32 NodeMCU         │
                  │   TP4056 + 18650 cell   │
                  │   Battery divider       │
                  │     (R1+R2+C1 → G34)    │
                  │                         │
                  │   NO antenna in here    │
                  └─────────────────────────┘
                  │
                  ▼
            (cable to solar panel)
```

**What goes in each box:**

| Sensor box | Control box |
|------------|-------------|
| AS3935 module only | ESP32 NodeMCU |
| Optional 100 nF VCC decoupling cap | TP4056 + 18650 |
| Nothing else electrically active | Battery divider |
|                                   | All other wiring |

**Inter-box cable:**

| Property | Spec | Notes |
|----------|------|-------|
| Length   | **30 – 50 cm typical**, up to ~80 cm | I²C at default 100 kHz tolerates long runs; keep shorter if you can |
| Conductors | 5 (VCC, GND, SDA, SCL, IRQ) | A 6-pin Dupont connector with one spare is convenient |
| Type     | Twisted-pair preferred, shielded for runs > 50 cm | Shield to GND **at the control-box end only** |
| Routing  | Away from the antenna, not over it | The ferrite loop's null axis is perpendicular to its plane — orient the cable along the null when possible |

**Why both boxes plastic.** Metal sensor box would Faraday-cage the
antenna and kill sensitivity completely. Metal control box would be
fine for shielding the noisy bits, **but** it would also block WiFi
unless you ran an external antenna for the ESP32 — usually not worth
the complexity. Plastic for both is the simpler win.

**Re-calibrate TUN_CAP after moving the AS3935 board** into its own
box. The new mechanical environment has different parasitic
capacitance and the LC tank's resonance shifts. One click in the
dashboard (Tuning panel → Actions → Calibrate TUN_CAP) or
`mosquitto_pub -h $H -t lightning/as3935/cmd -m '{"action":"calibrate_tun_cap"}'`.
Documented as Step 1 of the field-deploy troubleshooting flow.

**Symptom signature** if the sensor is still too close to digital
circuitry after deploy:

- False `event:"lightning"` events firing repeatedly in clear
  weather
- **Distance value of `1` (or `0` / "overhead")** on most or all
  of them — chip is reading saturated front-end as "very close"
- **Periodic cadence near 5 min**, matching the firmware's status
  republish WiFi-TX cycle (give or take a few seconds)

If you see that pattern after deployment, increase the inter-box
cable length and / or relocate the sensor box further from any
metal in the surrounding structure (gutters, wall flashing, the
solar panel's frame). Tuning knobs (WDTH, min_num_lightning, AFE_GB)
mask the symptom but don't fix the cause — the chip is still being
deafened by local noise.

---

## Where to mount

- **In shade.** Direct Bengaluru sun → enclosure hits 60 – 70 °C →
  18650 either won't charge (TP4056 cutoff above ~45 °C) or
  degrades fast. A tree-shaded balcony or a roof-eave-shaded
  south wall is fine.
- **Antenna orientation: vertical.** The AS3935's ferrite bar
  picks up best omnidirectionally when its long axis is vertical.
- **WiFi RSSI ≥ −70 dBm at the mounting spot** before sealing.
  Check with a phone WiFi-analyzer app held at the install
  location; if it's marginal there, the ESP32's internal antenna
  will be worse.
- **Sensor box ≥ 10 cm from the control box**, per the two-box
  topology above — and the cable run away from the antenna's
  sensitive axis, not draped over it.

---

*73 de VU2CPL*
