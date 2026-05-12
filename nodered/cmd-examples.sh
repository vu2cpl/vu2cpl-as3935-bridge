#!/usr/bin/env bash
# AS3935 bridge — every supported MQTT command as a one-liner.
# Run individual lines to tune the bridge from the shack Pi or any
# host on the LAN. Set BROKER if it's not the default.
set -euo pipefail

BROKER="${BROKER:-192.168.1.169}"
TOPIC="lightning/as3935/cmd"

pub() {
    echo "→ $1"
    mosquitto_pub -h "$BROKER" -t "$TOPIC" -m "$1"
}

# ── Numeric tunables ────────────────────────────────────────────────────
# pub '{"set":"nf","value":4}'                   # noise floor 0..7 (default 4)
# pub '{"set":"wdth","value":2}'                 # watchdog threshold 0..15 (default 2)
# pub '{"set":"srej","value":2}'                 # spike rejection 0..15 (default 2)
# pub '{"set":"tun_cap","value":10}'             # LC tank tune 0..15 (default 10)

# ── Boolean / enum tunables ─────────────────────────────────────────────
# pub '{"set":"mask_dist","value":true}'         # suppress disturber events
# pub '{"set":"mask_dist","value":false}'
# pub '{"set":"min_num_lightning","value":1}'    # 1, 5, 9, or 16
# pub '{"set":"afe_gb","value":"outdoor"}'       # "indoor" or "outdoor"
# pub '{"set":"modem_sleep","value":"max"}'      # "max" or "min"

# ── Actions ─────────────────────────────────────────────────────────────
# pub '{"action":"republish_status"}'            # refresh retained status now
# pub '{"action":"calibrate_tun_cap"}'           # ~35 s LC-tank sweep, picks + persists best cap
# pub '{"action":"reboot"}'                      # ESP.restart()
# pub '{"action":"factory_reset_wifi"}'          # WIPE WIFI CREDS — captive portal on next boot

echo "(uncomment the line you want to run, then re-execute)"
