#!/usr/bin/env python3
"""AS3935 disturber A/B/A test: does publishing every disturber (evt_interval_s 0) raise the disturber rate
compared with the throttle (60)? Phases: A 60 min @60, B 60 min @0, A2 30 min @60 (also restores the setting).
Rates come from the device's own counter (hb counters.disturber deltas), not from what gets published.
Logs every heartbeat to CSV; commands go through HA's mqtt.publish (svc is read-only on lightning/#)."""
import csv, json, os, statistics, subprocess, sys, threading, time, urllib.request

OUT = sys.argv[1]
PHASES = [("A", 60, 60 * 60), ("B", 0, 60 * 60), ("A2", 60, 30 * 60)]

env = {}
for f in ("/etc/default/vu2cpl-shack", os.path.expanduser("~/.config/vu2cpl-shack.env")):
    try:
        for l in open(f):
            l = l.strip()
            if "=" in l and not l.startswith("#"):
                k, v = l.split("=", 1)
                env[k.replace("export ", "").strip()] = v.strip().strip("\"'")
    except OSError:
        pass

def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)

lock = threading.Lock()
rows, acks, statuses = [], [], []
phase = {"name": "A"}

def reader():
    p = subprocess.Popen(["mosquitto_sub", "-h", env.get("MQTT_BROKER", "192.168.1.169"), "-u", env["MQTT_USER"],
                          "-P", env["MQTT_PASS"], "-R", "-v", "-t", "lightning/as3935/hb",
                          "-t", "lightning/as3935/cmd/ack", "-t", "lightning/as3935/status",
                          "-t", "lightning/as3935"], stdout=subprocess.PIPE, text=True)
    for line in p.stdout:
        topic, _, payload = line.strip().partition(" ")
        now = time.time()
        try:
            d = json.loads(payload)
        except ValueError:
            continue
        with lock:
            if topic.endswith("/hb"):
                rows.append({"t": now, "phase": phase["name"], "uptime": d["uptime_s"],
                             "dist": d["counters"]["disturber"], "noise": d["counters"]["noise"],
                             "light": d["counters"]["lightning"], "rate": d.get("rate_per_min", {}).get("disturber"),
                             "reconn": d.get("mqtt_reconnects"), "rssi": d.get("rssi")})
            elif topic.endswith("/ack"):
                acks.append((now, d))
            elif topic.endswith("/status"):
                statuses.append((now, d))
                if d.get("event") == "ready" and d.get("boot_cause") not in (None,):
                    pass
            elif topic == "lightning/as3935" and d.get("event") == "lightning":
                log("LIGHTNING event:", payload)

threading.Thread(target=reader, daemon=True).start()

def set_interval(v):
    body = json.dumps({"topic": "lightning/as3935/cmd",
                       "payload": json.dumps({"set": "evt_interval_s", "value": v}), "retain": False}).encode()
    for attempt in range(1, 4):
        t0 = time.time()
        r = urllib.request.Request(env["HA_URL"].rstrip("/") + "/api/services/mqtt/publish", data=body, method="POST",
                                   headers={"Authorization": "Bearer " + env["HA_TOKEN"], "Content-Type": "application/json"})
        urllib.request.urlopen(r, timeout=10)
        while time.time() - t0 < 15:
            time.sleep(0.5)
            with lock:
                ok = [s for (t, s) in statuses if t >= t0 and s.get("evt_interval_s") == v]
            if ok:
                log(f"evt_interval_s = {v} confirmed by status (attempt {attempt})")
                return True
        log(f"no confirmation for evt_interval_s={v}, attempt {attempt}")
    return False

time.sleep(3)
for name, ivl, secs in PHASES:
    with lock:
        phase["name"] = name
    if name != "A" and not set_interval(ivl):
        log("ABORT: could not set evt_interval_s; restoring 60 and stopping")
        set_interval(60)
        break
    log(f"phase {name}: evt_interval_s={ivl} for {secs // 60} min")
    time.sleep(secs)

# Make sure we end throttled whatever happened.
with lock:
    last_status = statuses[-1][1] if statuses else {}
if last_status.get("evt_interval_s") != 60:
    set_interval(60)

with lock:
    data = list(rows)
with open(OUT, "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=["t", "phase", "uptime", "dist", "noise", "light", "rate", "reconn", "rssi"])
    w.writeheader(); w.writerows(data)

log(f"wrote {len(data)} heartbeats to {OUT}")
print("\nphase  hb   minutes  disturbers  per_min  median_hb_rate  min..max hb rate  reconnects  restarts  mean_rssi")
for name, ivl, _ in PHASES:
    ph = [r for r in data if r["phase"] == name]
    if len(ph) < 3:
        print(f"{name:5}  too few heartbeats ({len(ph)})"); continue
    total, mins, restarts = 0, 0.0, 0
    for a, b in zip(ph, ph[1:]):
        if b["uptime"] < a["uptime"]:      # reboot: skip the interval that spans it
            restarts += 1; continue
        total += b["dist"] - a["dist"]; mins += (b["t"] - a["t"]) / 60
    rates = [r["rate"] for r in ph[1:] if r["rate"] is not None]
    print(f"{name:5}  {len(ph):3}  {mins:7.1f}  {total:10}  {total / mins:7.1f}  {statistics.median(rates):14.1f}"
          f"  {min(rates):6.1f}..{max(rates):<7.1f}  {ph[-1]['reconn'] - ph[0]['reconn']:10}  {restarts:8}"
          f"  {statistics.mean(r['rssi'] for r in ph):9.1f}")
lt = [r for r in data]
if lt:
    print(f"\nlightning counter {lt[0]['light']} -> {lt[-1]['light']}, noise {lt[0]['noise']} -> {lt[-1]['noise']}")
with lock:
    bc = [(time.strftime('%H:%M:%S', time.localtime(t)), s.get('boot_cause')) for (t, s) in statuses
          if s.get("event") == "ready"]
print("status ready events:", bc[:20])
