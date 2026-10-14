#!/usr/bin/env bash
# ota.sh — build the firmware and push it to the AS3935 bridge over WiFi (needs v0.4.1+ on the device).
#
#   ./ota.sh            build, open the OTA window over MQTT, upload, wait for the new version to report
#   ./ota.sh --no-build push the existing .pio/build/esp32dev/firmware.bin
#
# Asks once for the MQTT password of the `iot` account. It is also the OTA upload password, so nothing
# secret lives in the repo. The password is never echoed; curl reads it from stdin, not argv
# (mosquitto_pub/sub do take it as -P, visible to `ps` for those few seconds).
#
# Flow: subscribe to cmd/ack → publish {"action":"ota_open"} → read the URL from the ack → POST the image
# → wait for lightning/as3935/status to report the new fw. The device reboots into the new image on
# probation: if it can't reach MQTT before its next restart, the bootloader rolls back to the old one.
#
# Env: MQTT_HOST (default 192.168.1.169), OTA_MQTT_USER (default iot), PIO (path to pio).
set -euo pipefail
cd "$(dirname "$0")"

BROKER=${MQTT_HOST:-192.168.1.169}
MUSER=${OTA_MQTT_USER:-iot}
BIN=.pio/build/esp32dev/firmware.bin
WANT=$(sed -n "s/.*-DFIRMWARE_VERSION='\"\([^\"]*\)\"'.*/\1/p" platformio.ini)

# A Homebrew `pio` can be an old build on an unsupported Python; prefer PlatformIO's own venv.
if [ -z "${PIO:-}" ]; then
    if [ -x "$HOME/.platformio/penv/bin/pio" ]; then PIO="$HOME/.platformio/penv/bin/pio"; else PIO=pio; fi
fi
for t in mosquitto_pub mosquitto_sub curl; do
    command -v "$t" >/dev/null || { echo "missing $t (macOS: brew install mosquitto curl; Pi: sudo apt install mosquitto-clients curl)" >&2; exit 2; }
done

if [ "${1:-}" != "--no-build" ]; then
    "$PIO" run -e esp32dev
fi
[ -f "$BIN" ] || { echo "no $BIN — build first" >&2; exit 2; }
echo "image: $BIN ($(wc -c < "$BIN" | tr -d ' ') bytes), version $WANT"

read -r -s -p "MQTT password for '$MUSER' (also the OTA password): " PW; echo
[ -n "$PW" ] || { echo "empty password" >&2; exit 2; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

# 1. Open the window. Subscribe first so the ack can't be missed.
mosquitto_sub -h "$BROKER" -u "$MUSER" -P "$PW" -t lightning/as3935/cmd/ack -C 1 -W 20 > "$TMP/ack" 2>"$TMP/ack.err" &
SUB=$!
sleep 1
mosquitto_pub -h "$BROKER" -u "$MUSER" -P "$PW" -t lightning/as3935/cmd -m '{"action":"ota_open"}'
wait "$SUB" || true
ACK=$(cat "$TMP/ack")
if [ -z "$ACK" ]; then
    echo "no ack within 20 s ($(cat "$TMP/ack.err"))." >&2
    echo "Is the sensor online and on v0.4.1+? Check: mosquitto_sub -t lightning/as3935/status -C 1" >&2
    exit 1
fi
echo "ack: $ACK"
URL=$(printf '%s' "$ACK" | sed -n 's/.*url=\(http:[^ "]*\).*/\1/p')
[ -n "$URL" ] || { echo "the device refused ota_open (see the ack above)" >&2; exit 1; }

# 2. Upload. Watch for the new version from before the upload, so a quick reboot can't be missed.
mosquitto_sub -h "$BROKER" -u "$MUSER" -P "$PW" -t lightning/as3935/status -W 180 > "$TMP/status" 2>/dev/null &
WATCH=$!
echo "uploading to $URL …"
PWQ=${PW//\\/\\\\}; PWQ=${PWQ//\"/\\\"}   # escape for curl's quoted config syntax
if ! printf 'user = "ota:%s"\n' "$PWQ" | curl -sS --fail-with-body --max-time 240 -K - \
        -F "firmware=@$BIN" "$URL"; then
    kill "$WATCH" 2>/dev/null || true
    echo "upload failed" >&2
    exit 1
fi

# 3. Wait for the device to come back reporting the new version.
echo "waiting up to 3 min for the device to report $WANT …"
for _ in $(seq 1 180); do
    if grep -q "\"fw\":\"$WANT\"" "$TMP/status" 2>/dev/null; then
        kill "$WATCH" 2>/dev/null || true
        LINE=$(grep "\"fw\":\"$WANT\"" "$TMP/status" | tail -1)
        echo "OK: running $WANT, boot_cause=$(printf '%s' "$LINE" | sed -n 's/.*"boot_cause":"\([^"]*\)".*/\1/p')"
        exit 0
    fi
    kill -0 "$WATCH" 2>/dev/null || break
    sleep 1
done
kill "$WATCH" 2>/dev/null || true
echo "did not see $WANT within 3 min. If status shows the old fw, the new image rolled back." >&2
exit 1
