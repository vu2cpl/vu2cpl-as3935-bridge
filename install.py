#!/usr/bin/env python3
"""
install.py — interactive bench-bring-up for vu2cpl-as3935-bridge.

Run from the repo root:
    python3 install.py

What it does:
  1. Installs PlatformIO if missing (pip3 install -U platformio).
  2. Prompts for your MQTT broker, captive-portal AP creds,
     timezone, serial port, and (optionally) Node-RED IDs.
  3. Patches src/main.cpp, platformio.ini, and nodered/build-flow.py
     in place. Regenerates nodered/as3935-control-flow.json.
  4. Runs `pio run` to verify the firmware compiles.

It does NOT flash the chip — you trigger `pio run -t upload`
yourself when you're ready (and the ESP32 isn't being
unplugged-and-replugged for port identification).

The patched files show up as a git diff. If you're forking this
for your own station, commit them. If you're just trying it out,
`git checkout -- .` restores defaults.
"""
import glob
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent
MAIN_CPP = REPO / "src" / "main.cpp"
PIO_INI = REPO / "platformio.ini"
FLOW_PY = REPO / "nodered" / "build-flow.py"

# ── Colours (no dep — plain ANSI) ───────────────────────────────────────
G = "\033[32m"   # green
Y = "\033[33m"   # amber
R = "\033[31m"   # red
B = "\033[1m"    # bold
D = "\033[2m"    # dim
N = "\033[0m"    # reset


def hdr(msg):
    print(f"\n{B}── {msg} ──{N}")


def ok(msg):
    print(f"  {G}✓{N} {msg}")


def warn(msg):
    print(f"  {Y}⚠{N} {msg}")


def err(msg):
    print(f"  {R}✗{N} {msg}")


def ask(prompt, default):
    raw = input(f"  {prompt} [{D}{default}{N}]: ").strip()
    return raw if raw else default


def confirm(prompt, default=True):
    d = "Y/n" if default else "y/N"
    raw = input(f"\n{prompt} [{d}]: ").strip().lower()
    if not raw:
        return default
    return raw.startswith("y")


# ── Read current values from the source so prompts default to them ─────
def grep(path, pattern, group=1):
    text = path.read_text()
    m = re.search(pattern, text)
    return m.group(group) if m else ""


# ── In-place patcher with a "what changed" report ──────────────────────
def patch(path, pattern, replacement, *, count=1, label=None):
    text = path.read_text()
    new_text, n = re.subn(pattern, replacement, text, count=count)
    if n == 0:
        warn(f"{label or pattern}: pattern not found in {path.name} (skipped)")
        return False
    if new_text == text:
        return True  # already at desired value
    path.write_text(new_text)
    ok(f"{label or pattern} → {path.name}")
    return True


def detect_serial_port():
    candidates = (
        sorted(glob.glob("/dev/cu.usbserial-*"))
        + sorted(glob.glob("/dev/cu.SLAB_USBtoUART*"))
        + sorted(glob.glob("/dev/cu.wchusbserial*"))
        + sorted(glob.glob("/dev/ttyUSB*"))
        + sorted(glob.glob("/dev/ttyACM*"))
    )
    return candidates[0] if candidates else None


def ensure_pio():
    if shutil.which("pio"):
        ok(f"PlatformIO already installed: {shutil.which('pio')}")
        return
    warn("PlatformIO not found.")
    if not confirm("Install via `pip3 install -U platformio`?", default=True):
        err("Aborted — PlatformIO is required.")
        sys.exit(1)
    rc = subprocess.run(
        [sys.executable, "-m", "pip", "install", "-U", "platformio"],
        check=False,
    ).returncode
    if rc != 0 or not shutil.which("pio"):
        err("PlatformIO install failed — try `pip3 install platformio` "
            "manually and re-run install.py.")
        sys.exit(1)
    ok("PlatformIO installed.")


def main():
    print(f"\n{B}═══ vu2cpl-as3935-bridge installer ═══{N}\n")
    print("This will customise firmware + Node-RED config for your")
    print("specific shack and verify the firmware compiles. It does")
    print("not flash the ESP32 — you run `pio run -t upload` yourself.\n")

    if not confirm("Continue?"):
        print("Aborted.")
        return

    hdr("Step 1/4 — PlatformIO")
    ensure_pio()

    hdr("Step 2/4 — Firmware config")
    cur_host = grep(MAIN_CPP, r'MQTT_HOST = "([^"]+)"') or "192.168.1.169"
    cur_port = grep(MAIN_CPP, r"MQTT_PORT = (\d+)") or "1883"
    cur_ssid = grep(MAIN_CPP, r'WIFI_AP_SSID = "([^"]+)"') or "vu2cpl-as3935-setup"
    cur_pass = grep(MAIN_CPP, r'WIFI_AP_PASS = "([^"]+)"') or "vu2cpl1234"
    cur_tz = grep(MAIN_CPP, r'configTzTime\("([^"]+)"') or "IST-5:30"

    mqtt_host = ask("MQTT broker host", cur_host)
    mqtt_port = ask("MQTT broker port", cur_port)
    wifi_ssid = ask("Captive-portal AP SSID", cur_ssid)
    wifi_pass = ask("Captive-portal AP password", cur_pass)
    tz = ask("Timezone (POSIX, e.g. IST-5:30, EST5EDT, UTC0)", cur_tz)

    hdr("Step 3/4 — Serial port for the ESP32")
    detected = detect_serial_port()
    if detected:
        print(f"  Detected USB-serial device: {G}{detected}{N}")
        print(f"  {D}(If you have multiple USB-serial devices — e.g. radio")
        print(f"  CAT cables — unplug the ESP32, run `ls /dev/cu.usb*`,")
        print(f"  plug it back in, and ls again to find the right one.){N}")
    else:
        print("  No USB-serial device detected. Plug in the ESP32 and")
        print("  press Enter to retry, or type the port path manually.")
    port = ask("Serial port", detected or "/dev/cu.usbserial-0001")

    hdr("Step 4/4 — Node-RED dashboard (optional)")
    print(f"  {D}If you don't use Node-RED, accept the defaults and skip{N}")
    print(f"  {D}the dashboard import. The firmware doesn't depend on it.{N}\n")
    cur_tab = grep(FLOW_PY, r'"tab": "([^"]+)"') or "bcce4e07ac31b882"
    cur_broker = grep(FLOW_PY, r'"broker": "([^"]+)"') or "f4785be9863eab08"
    nodered_tab = ask("Existing Node-RED dashboard tab ID", cur_tab)
    nodered_broker = ask("Existing Node-RED MQTT broker config ID", cur_broker)

    # ── Summary ───
    hdr("Summary")
    print(f"  MQTT broker        : {mqtt_host}:{mqtt_port}")
    print(f"  Captive-portal AP  : {wifi_ssid} / {wifi_pass}")
    print(f"  Timezone           : {tz}")
    print(f"  Serial port        : {port}")
    print(f"  Node-RED tab ID    : {nodered_tab}")
    print(f"  Node-RED broker ID : {nodered_broker}")

    if not confirm("Apply these changes?"):
        print("Aborted — no files modified.")
        return

    # ── Patches ───
    hdr("Patching")
    patch(MAIN_CPP,
          r'constexpr const char\* MQTT_HOST = "[^"]+";',
          f'constexpr const char* MQTT_HOST = "{mqtt_host}";',
          label="MQTT_HOST")
    patch(MAIN_CPP,
          r'constexpr uint16_t\s+MQTT_PORT = \d+;',
          f'constexpr uint16_t    MQTT_PORT = {mqtt_port};',
          label="MQTT_PORT")
    patch(MAIN_CPP,
          r'constexpr const char\* WIFI_AP_SSID = "[^"]+";',
          f'constexpr const char* WIFI_AP_SSID = "{wifi_ssid}";',
          label="WIFI_AP_SSID")
    patch(MAIN_CPP,
          r'constexpr const char\* WIFI_AP_PASS = "[^"]+";',
          f'constexpr const char* WIFI_AP_PASS = "{wifi_pass}";',
          label="WIFI_AP_PASS")
    patch(MAIN_CPP,
          r'configTzTime\("[^"]+",',
          f'configTzTime("{tz}",',
          label="Timezone")
    patch(PIO_INI,
          r"upload_port = .+",
          f"upload_port = {port}",
          label="upload_port")
    patch(PIO_INI,
          r"monitor_port = .+",
          f"monitor_port = {port}",
          label="monitor_port")
    patch(FLOW_PY,
          r'"tab": "[a-f0-9]{16}"',
          f'"tab": "{nodered_tab}"',
          label="Node-RED tab ID")
    patch(FLOW_PY,
          r'"broker": "[a-f0-9]{16}"',
          f'"broker": "{nodered_broker}"',
          count=0,  # all occurrences
          label="Node-RED broker ID (all instances)")

    # Regenerate the Node-RED flow JSON from the patched build-flow.py
    if shutil.which("python3"):
        hdr("Regenerating Node-RED flow JSON")
        rc = subprocess.run(
            [sys.executable, str(FLOW_PY)], cwd=REPO, check=False
        ).returncode
        if rc == 0:
            ok("nodered/as3935-control-flow.json updated")
        else:
            warn("build-flow.py failed — flow JSON may be stale")

    # ── Compile ───
    hdr("Building firmware (this will take a couple of minutes on first run)")
    rc = subprocess.run(["pio", "run"], cwd=REPO, check=False).returncode
    if rc != 0:
        err("Build failed. Check the output above; common causes:")
        print("    - Bad regex in build-flow.py (re-run install.py to fix)")
        print("    - Missing libraries (pio will download them automatically)")
        print("    - C++ syntax error in your patched main.cpp")
        sys.exit(1)
    ok("Firmware built successfully.")

    # ── Next steps ───
    hdr("Next steps")
    print(f"  1. Plug in the ESP32 via USB ({port}).")
    print(f"  2. {B}pio run -t upload{N}   (flash)")
    print(f"  3. {B}pio device monitor{N}  (watch boot log)")
    print(f"  4. On first boot, connect your phone to AP")
    print(f"     {B}{wifi_ssid}{N} (password {B}{wifi_pass}{N}) and pick")
    print(f"     your shack WiFi in the captive portal.")
    print(f"  5. Subscribe on your broker to verify it's publishing:")
    print(f"     {B}mosquitto_sub -h {mqtt_host} -t 'lightning/as3935/#' -v{N}\n")
    print(f"  Node-RED dashboard: import")
    print(f"     {B}nodered/as3935-control-flow.json{N}")
    print(f"  into Node-RED (Menu → Import → import to new flow → Deploy).")
    print(f"  See {B}nodered/README.md{N} and {B}BUILD.md{N} for details.\n")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nAborted.")
        sys.exit(130)
