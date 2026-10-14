#!/usr/bin/env python3
"""Trigger the AS3935 bridge's wifi_off_test via HA mqtt.publish and print the ack, status changes and result."""
import json, os, subprocess, sys, time, urllib.request

SECS = int(sys.argv[1]) if len(sys.argv) > 1 else 60
env = {}
for l in open(os.path.expanduser("~/.config/vu2cpl-shack.env")):
    l = l.strip()
    if "=" in l and not l.startswith("#"):
        k, v = l.split("=", 1)
        env[k.replace("export ", "").strip()] = v.strip().strip("\"'")

def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)

sub = subprocess.Popen(["mosquitto_sub", "-h", env.get("MQTT_BROKER", "192.168.1.169"), "-u", env["MQTT_USER"],
                        "-P", env["MQTT_PASS"], "-R", "-v", "-t", "lightning/as3935/cmd/ack",
                        "-t", "lightning/as3935/status", "-t", "lightning/as3935/test",
                        "-t", "lightning/as3935"], stdout=subprocess.PIPE, text=True)
time.sleep(1)
body = json.dumps({"topic": "lightning/as3935/cmd",
                   "payload": json.dumps({"action": "wifi_off_test", "secs": SECS}), "retain": False}).encode()
r = urllib.request.Request(env["HA_URL"].rstrip("/") + "/api/services/mqtt/publish", data=body, method="POST",
                           headers={"Authorization": "Bearer " + env["HA_TOKEN"], "Content-Type": "application/json"})
log("sent wifi_off_test secs=%d via HA: HTTP %d" % (SECS, urllib.request.urlopen(r, timeout=10).status))
t0 = time.time()
for line in sub.stdout:
    topic, _, payload = line.strip().partition(" ")
    try:
        d = json.loads(payload)
    except ValueError:
        continue
    if topic.endswith("/ack"):
        log("ack:", d.get("ok"), d.get("cmd"), d.get("error") or "")
    elif topic.endswith("/status"):
        log("status:", d.get("event"), "fw", d.get("fw"), "boot_cause", d.get("boot_cause"))
    elif topic == "lightning/as3935" and d.get("event") == "lightning":
        log("LIGHTNING:", payload)
    elif topic.endswith("/test"):
        log("RESULT:", json.dumps(d))
        for w in ("on_before", "off", "on_after"):
            x = d[w]
            print(f"   {w:9}  disturbers {x['disturber']:6}  in {x['secs']:5.1f} s  = {x['per_min']:6.1f}/min"
                  f"   noise {x['noise']}  lightning {x['lightning']}")
        print(f"   rejoin {d['rejoin_s']} s")
        break
    if time.time() - t0 > SECS * 3 + 360:
        log("timed out waiting for the result")
        break
sub.terminate()
