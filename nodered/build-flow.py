#!/usr/bin/env python3
"""Build the AS3935 control flow JSON, embedding the panel HTML/CSS/JS.

Run once to regenerate nodered/as3935-control-flow.json. Edit the
TEMPLATE constants below; Python handles the JSON escaping.
"""
import json

# ── Palette + style — matches Lightning Protection / Master Dashboard ──
CSS = """\
<style>
:root{
  --bg:#0d1117; --card:#161b22; --border:#21262d;
  --accent:#238636; --danger:#da3633; --warn:#e3b341;
  --muted:#8b949e; --text:#c9d1d9;
  --green:#3fb950; --red:#f85149; --amber:#e3b341;
  --font:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;
  --mono:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;
}
#a35tune *{box-sizing:border-box;margin:0;padding:0}
#a35tune{
  font-family:var(--font); background:var(--bg); color:var(--text);
  padding:10px; display:flex; flex-direction:column; gap:8px;
  font-size:13px;
}
#a35tune .card{
  background:var(--card); border:1px solid var(--border);
  border-radius:8px; padding:10px 14px;
}
#a35tune .hdr{display:flex;align-items:center;gap:10px}
#a35tune .led{
  display:inline-block; width:12px; height:12px; border-radius:50%;
  background:var(--muted); box-shadow:0 0 6px currentColor;
}
#a35tune .title{font-weight:700; font-size:14px; letter-spacing:.3px}
#a35tune .meta{color:var(--muted); font-size:11px; margin-left:auto;
  font-family:var(--mono)}
#a35tune .counters{
  font-family:var(--mono); font-size:13px; letter-spacing:.5px;
}
#a35tune .calib{color:var(--muted); font-size:11px;
  font-family:var(--mono)}
#a35tune .tune{display:flex; flex-direction:column; gap:6px}
#a35tune .trow{
  display:flex; align-items:center; gap:8px; font-size:12px;
}
#a35tune .lbl{width:100px; color:var(--muted)}
#a35tune .val{
  display:inline-block; min-width:34px; text-align:center;
  background:var(--bg); border:1px solid var(--border);
  border-radius:4px; padding:2px 8px;
  font-family:var(--mono); font-weight:700;
}
#a35tune .rng{color:var(--muted); font-size:10px; margin-left:auto;
  font-family:var(--mono)}
#a35tune button{
  font-family:var(--font); background:var(--card); color:var(--text);
  border:1px solid var(--border); border-radius:6px;
  padding:5px 10px; font-size:12px; cursor:pointer;
  transition:background 100ms, border-color 100ms;
}
#a35tune button:hover{background:#21262d; border-color:#3a414b}
#a35tune button:active{transform:translateY(1px)}
#a35tune button:disabled{opacity:.35; cursor:not-allowed}
#a35tune .trow button{padding:2px 9px; min-width:26px;
  font-family:var(--mono); font-weight:700; font-size:14px;
  line-height:1}
#a35tune select{
  font-family:var(--font); background:var(--bg); color:var(--text);
  border:1px solid var(--border); border-radius:4px;
  padding:3px 6px; font-size:12px;
}
#a35tune .actions{display:flex; flex-wrap:wrap; gap:6px}
#a35tune button.primary{
  background:var(--accent); border-color:var(--accent);
  color:white; font-weight:600;
}
#a35tune button.primary:hover{background:#2ea043; border-color:#2ea043}
#a35tune button.warn{
  background:transparent; border-color:var(--warn); color:var(--warn);
}
#a35tune button.warn:hover{background:rgba(227,179,65,.1)}
#a35tune button.danger{
  background:transparent; border-color:var(--danger); color:var(--red);
}
#a35tune button.danger:hover{background:rgba(218,54,51,.1)}
#a35tune .ack{
  font-family:var(--mono); font-size:11px; color:var(--muted);
}
#a35tune .ack.ok{color:var(--green)}
#a35tune .ack.bad{color:var(--red)}
#a35tune .sect{font-size:10px; color:var(--muted);
  text-transform:uppercase; letter-spacing:1px; margin-bottom:4px}
</style>
"""

HTML = """\
<div id="a35tune">
  <div class="card hdr">
    <span class="led" id="a35led"></span>
    <span class="title">AS3935 Bridge</span>
    <span class="meta" id="a35meta">awaiting status...</span>
  </div>

  <div class="card counters" id="a35counters">awaiting heartbeat...</div>

  <div class="card calib" id="a35calib">Calib: —</div>

  <div class="card tune">
    <div class="sect">Tunables</div>
    <div class="trow"><span class="lbl">NF</span>
      <span class="val" id="a35nf">—</span>
      <button onclick="a35.nudge('nf',-1,0,7)">−</button>
      <button onclick="a35.nudge('nf',+1,0,7)">+</button>
      <span class="rng">0..7</span></div>
    <div class="trow"><span class="lbl">WDTH</span>
      <span class="val" id="a35wdth">—</span>
      <button onclick="a35.nudge('wdth',-1,0,15)">−</button>
      <button onclick="a35.nudge('wdth',+1,0,15)">+</button>
      <span class="rng">0..15</span></div>
    <div class="trow"><span class="lbl">SREJ</span>
      <span class="val" id="a35srej">—</span>
      <button onclick="a35.nudge('srej',-1,0,15)">−</button>
      <button onclick="a35.nudge('srej',+1,0,15)">+</button>
      <span class="rng">0..15</span></div>
    <div class="trow"><span class="lbl">TUN_CAP</span>
      <span class="val" id="a35tuncap">—</span>
      <button onclick="a35.nudge('tun_cap',-1,0,15)">−</button>
      <button onclick="a35.nudge('tun_cap',+1,0,15)">+</button>
      <span class="rng">0..15</span></div>
    <div class="trow"><span class="lbl">Mask dist</span>
      <span class="val" id="a35mask">—</span>
      <button onclick="a35.toggle('mask_dist')">toggle</button>
      <span class="rng">on/off</span></div>
    <div class="trow"><span class="lbl">Min strikes</span>
      <select id="a35mnl" onchange="a35.set('min_num_lightning', parseInt(this.value))">
        <option value="1">1</option><option value="5">5</option>
        <option value="9">9</option><option value="16">16</option>
      </select>
      <span class="rng">storm filter</span></div>
    <div class="trow"><span class="lbl">Modem sleep</span>
      <select id="a35ms" onchange="a35.set('modem_sleep', this.value)">
        <option value="max">max</option><option value="min">min</option>
      </select>
      <span class="rng">WiFi PS</span></div>
  </div>

  <div class="card actions">
    <div class="sect" style="width:100%">Actions</div>
    <button class="primary" onclick="a35.action('calibrate_tun_cap')">Calibrate TUN_CAP (~35s)</button>
    <button onclick="a35.action('republish_status')">Republish Status</button>
    <button class="warn" onclick="a35.confirm('Reboot the ESP32?', 'reboot')">Reboot ESP32</button>
    <button class="danger" onclick="a35.confirm('Erase stored WiFi creds and reboot into setup AP?', 'factory_reset_wifi')">Factory Reset WiFi</button>
  </div>

  <div class="card ack" id="a35ack">No commands yet</div>
</div>
"""

JS = """\
<script>
(function(){
  // 'scope' is the AngularJS controller scope that Node-RED's ui_template
  // exposes inside this <script> block. Do NOT shadow it with
  // 'var scope = this;' — the IIFE's `this` is undefined/window and we'd
  // lose access to scope.send / scope.$watch. The Master Dashboard
  // template uses the same closure-from-controller pattern.
  var state = {};
  var hb = {counters:{}};

  function $(id){return document.getElementById(id);}
  function fmtUptime(s){
    if(s==null) return '?';
    var h=Math.floor(s/3600), m=Math.floor((s%3600)/60), x=s%60;
    return (h?h+'h ':'') + (m?m+'m ':'') + x+'s';
  }

  function render(){
    var led = $('a35led');
    if(led){
      var ok = state.event==='ready'
        && state.calib_trco==='OK' && state.calib_srco==='OK';
      var off = state.event==='offline';
      led.style.background = off ? 'var(--red)'
        : ok ? 'var(--green)' : 'var(--amber)';
      led.style.color      = off ? 'var(--red)'
        : ok ? 'var(--green)' : 'var(--amber)';
    }
    var meta = $('a35meta');
    if(meta){
      meta.textContent =
        'FW ' + (state.fw || '—')
        + ' · IP ' + (state.ip || '?')
        + ' · RSSI ' + ((hb.rssi != null ? hb.rssi : state.rssi) || '?') + ' dBm'
        + ' · up ' + fmtUptime(hb.uptime_s);
    }
    var c = hb.counters || {};
    var cn = $('a35counters');
    if(cn){
      cn.textContent =
        '⚡ ' + (c.lightning || 0)
        + '   ⚠ ' + (c.disturber || 0)
        + '   📡 ' + (c.noise || 0)
        + '   IRQ ' + (c.irq || 0);
    }
    var cal = $('a35calib');
    if(cal){
      cal.textContent =
        'Calib: TRCO=' + (state.calib_trco || '?')
        + ' · SRCO=' + (state.calib_srco || '?')
        + ' · afe_gb=' + (state.afe_gb || '?');
    }
    if(state.nf      != null && $('a35nf'))     $('a35nf').textContent = state.nf;
    if(state.wdth    != null && $('a35wdth'))   $('a35wdth').textContent = state.wdth;
    if(state.srej    != null && $('a35srej'))   $('a35srej').textContent = state.srej;
    if(state.tun_cap != null && $('a35tuncap')) $('a35tuncap').textContent = state.tun_cap;
    if(state.mask_dist != null && $('a35mask')){
      $('a35mask').textContent = state.mask_dist ? 'ON' : 'OFF';
      $('a35mask').style.color = state.mask_dist ? 'var(--amber)' : 'var(--text)';
    }
    if(state.min_num_lightning != null && $('a35mnl'))
      $('a35mnl').value = String(state.min_num_lightning);
    if(state.modem_sleep && $('a35ms'))
      $('a35ms').value = state.modem_sleep;
  }

  window.a35 = {
    nudge: function(key, delta, lo, hi){
      var v = (state[key] == null ? 0 : state[key]) + delta;
      if(v < lo || v > hi) return;
      this.set(key, v);
    },
    toggle: function(key){ this.set(key, !state[key]); },
    set: function(key, value){
      scope.send({ payload: { set: key, value: value } });
    },
    action: function(name){
      scope.send({ payload: { action: name } });
    },
    confirm: function(msg, action){
      if(confirm(msg)) this.action(action);
    }
  };

  scope.$watch('msg', function(msg){
    if(!msg || !msg.topic) return;
    var p = msg.payload;
    if(typeof p === 'string'){ try { p = JSON.parse(p); } catch(e){ return; } }
    if(msg.topic === 'lightning/as3935/status'){ state = p || {}; }
    else if(msg.topic === 'lightning/as3935/hb'){ hb = p || {counters:{}}; }
    else if(msg.topic === 'lightning/as3935/cmd/ack' && $('a35ack')){
      var t = (p && p.ts || '').slice(11);
      $('a35ack').textContent =
        (p.ok ? '✓ ' : '✗ ') + (p.cmd || '?')
        + (p.error ? ' — ' + p.error : '') + (t ? '  @ ' + t : '');
      $('a35ack').className = 'card ack ' + (p.ok ? 'ok' : 'bad');
    }
    render();
  });

  render();
})();
</script>
"""

PANEL_FORMAT = CSS + HTML + JS

# ── Flow JSON structure ──
FLOW = [
    {
        "id": "as3935_ctl_flow",
        "type": "tab",
        "label": "AS3935 Tuning",
        "disabled": False,
        "info": "MQTT control + live status panel for the vu2cpl-as3935-bridge ESP32.\n\nPublishes to lightning/as3935/cmd. Subscribes to status/hb/cmd/ack.\n\nDashboard group: Shack Monitoring tools > AS3935 Tuning."
    },
    {
        "id": "as3935_ctl_grp",
        "type": "ui_group",
        "name": "AS3935 Tuning",
        "tab": "bcce4e07ac31b882",
        "order": 30,
        "disp": False,
        "width": "12",
        "collapse": False,
        "className": ""
    },
    {
        "id": "as3935_ctl_status_in",
        "type": "mqtt in",
        "z": "as3935_ctl_flow",
        "name": "AS3935 Status",
        "topic": "lightning/as3935/status",
        "qos": "0",
        "datatype": "json",
        "broker": "f4785be9863eab08",
        "nl": False, "rap": True, "rh": 0,
        "inputs": 0,
        "x": 160, "y": 100,
        "wires": [["as3935_ctl_panel"]]
    },
    {
        "id": "as3935_ctl_hb_in",
        "type": "mqtt in",
        "z": "as3935_ctl_flow",
        "name": "AS3935 Heartbeat",
        "topic": "lightning/as3935/hb",
        "qos": "0",
        "datatype": "json",
        "broker": "f4785be9863eab08",
        "nl": False, "rap": True, "rh": 0,
        "inputs": 0,
        "x": 170, "y": 160,
        "wires": [["as3935_ctl_panel"]]
    },
    {
        "id": "as3935_ctl_ack_in",
        "type": "mqtt in",
        "z": "as3935_ctl_flow",
        "name": "AS3935 Cmd Ack",
        "topic": "lightning/as3935/cmd/ack",
        "qos": "0",
        "datatype": "json",
        "broker": "f4785be9863eab08",
        "nl": False, "rap": True, "rh": 0,
        "inputs": 0,
        "x": 160, "y": 220,
        "wires": [["as3935_ctl_panel"]]
    },
    {
        "id": "as3935_ctl_panel",
        "type": "ui_template",
        "z": "as3935_ctl_flow",
        "group": "as3935_ctl_grp",
        "name": "AS3935 Control Panel",
        "order": 1,
        "width": "12",
        "height": "18",
        "format": PANEL_FORMAT,
        "storeOutMessages": True,
        "fwdInMessages": False,
        "resendOnRefresh": True,
        "templateScope": "local",
        "className": "",
        "x": 460, "y": 160,
        "wires": [["as3935_ctl_cmd_out"]]
    },
    {
        "id": "as3935_ctl_cmd_out",
        "type": "mqtt out",
        "z": "as3935_ctl_flow",
        "name": "AS3935 Cmd",
        "topic": "lightning/as3935/cmd",
        "qos": "0",
        "retain": "false",
        "respTopic": "",
        "contentType": "",
        "userProps": "",
        "correl": "",
        "expiry": "",
        "broker": "f4785be9863eab08",
        "x": 720, "y": 160,
        "wires": []
    }
]

OUT = "/Users/manoj/projects/vu2cpl-as3935-bridge/nodered/as3935-control-flow.json"
with open(OUT, "w") as f:
    json.dump(FLOW, f, indent=2)
print(f"Wrote {OUT} ({sum(1 for _ in open(OUT))} lines, "
      f"{len(open(OUT).read())} bytes)")
