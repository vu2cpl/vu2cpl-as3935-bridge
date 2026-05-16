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
#a35tune .meta{color:var(--text); font-size:11px; margin-left:auto;
  font-family:var(--mono)}
#a35tune .counters{
  font-family:var(--mono); font-size:13px; letter-spacing:.5px;
}
#a35tune .calib{color:var(--muted); font-size:11px;
  font-family:var(--mono)}
#a35tune .vbat{
  font-family:var(--mono); font-size:13px;
  display:flex; gap:10px; align-items:baseline;
}
#a35tune .vbat #a35vbat{font-weight:700}
#a35tune .vbat.good #a35vbat{color:var(--green)}
#a35tune .vbat.warn #a35vbat{color:var(--amber)}
#a35tune .vbat.bad  #a35vbat{color:var(--red)}
#a35tune .vbat.absent #a35vbat{color:var(--muted)}
#a35tune .vbat .vbatpct{color:var(--text); font-size:11px}
#a35tune .vbat .vbatoffset{color:var(--muted); font-size:10px;
  margin-left:auto}
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

  <div class="card vbat" id="a35vbatcard">
    <span id="a35vbat">🔋 —</span>
    <span class="vbatpct" id="a35vbatpct"></span>
    <span class="vbatoffset" id="a35vbatoff"></span>
  </div>

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
    <div class="trow"><span class="lbl">AFE GB</span>
      <span class="val" id="a35afe">—</span>
      <button onclick="a35.toggleAfe()">toggle</button>
      <span class="rng">indoor/outdoor</span></div>
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
    <button onclick="a35.action('query_vbat')">Query Battery</button>
    <button class="warn" onclick="a35.confirm('Reboot the ESP32?', 'reboot')">Reboot ESP32</button>
    <button class="danger" onclick="a35.confirm('Erase stored WiFi creds and reboot into setup AP?', 'factory_reset_wifi')">Factory Reset WiFi</button>
  </div>

  <div class="card ack" id="a35ack">No commands yet</div>
</div>
"""

JS = """\
<script>
(function(scope){
  // Pattern B (matches chrony card): IIFE takes scope as parameter,
  // captured from the top-level closure where node-red-dashboard exposes
  // it. Don't rely on `this` — IIFE's this is window, not the directive's
  // scope, so `var scope = this;` would silently set scope=window.
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
        + ' · SRCO=' + (state.calib_srco || '?');
    }

    // Battery row. hb.vbat_mv lands every 30s (faster than the 5-min
    // status republish); status.vbat_mv fills in on connect/republish.
    // Prefer hb when both exist — it's fresher.
    var mv = (hb && hb.vbat_mv != null) ? hb.vbat_mv : state.vbat_mv;
    var vbcard = $('a35vbatcard');
    var vbel   = $('a35vbat');
    var vbpct  = $('a35vbatpct');
    var vboff  = $('a35vbatoff');
    if(vbcard && vbel){
      if(mv == null){
        vbel.textContent = '🔋 awaiting telemetry...';
        vbcard.className = 'card vbat absent';
        if(vbpct) vbpct.textContent = '';
      } else if(mv < 500){
        // Divider not wired (or no battery) — float read is <0.5 V.
        vbel.textContent = '🔋 — (divider not wired?)';
        vbcard.className = 'card vbat absent';
        if(vbpct) vbpct.textContent = '';
      } else {
        var v = (mv / 1000).toFixed(2);
        // Piecewise-linear SOC for an 18650.
        // 4.20→100, 3.95→80, 3.85→60, 3.75→40, 3.65→20, 3.50→10, 3.30→0.
        var pts = [[3300,0],[3500,10],[3650,20],[3750,40],
                   [3850,60],[3950,80],[4200,100]];
        var pct;
        if(mv <= pts[0][0])               pct = 0;
        else if(mv >= pts[pts.length-1][0]) pct = 100;
        else {
          for(var i=1;i<pts.length;i++){
            if(mv <= pts[i][0]){
              var a = pts[i-1], b = pts[i];
              pct = a[1] + (b[1]-a[1]) * (mv-a[0]) / (b[0]-a[0]);
              break;
            }
          }
        }
        vbel.textContent = '🔋 ' + v + ' V';
        if(vbpct) vbpct.textContent = '(≈ ' + Math.round(pct) + ' %)';
        // Colour thresholds: green ≥ 3.90 V, amber 3.70–3.90 V, red < 3.70 V.
        var cls = (mv >= 3900) ? 'good'
                : (mv >= 3700) ? 'warn'
                : 'bad';
        vbcard.className = 'card vbat ' + cls;
      }
    }
    if(vboff){
      vboff.textContent = (state.vbat_offset_mv != null
        && state.vbat_offset_mv !== 0)
        ? 'offset ' + (state.vbat_offset_mv>0?'+':'') + state.vbat_offset_mv + ' mV'
        : '';
    }
    if(state.nf      != null && $('a35nf'))     $('a35nf').textContent = state.nf;
    if(state.wdth    != null && $('a35wdth'))   $('a35wdth').textContent = state.wdth;
    if(state.srej    != null && $('a35srej'))   $('a35srej').textContent = state.srej;
    if(state.tun_cap != null && $('a35tuncap')) $('a35tuncap').textContent = state.tun_cap;
    if(state.mask_dist != null && $('a35mask')){
      $('a35mask').textContent = state.mask_dist ? 'ON' : 'OFF';
      $('a35mask').style.color = state.mask_dist ? 'var(--amber)' : 'var(--text)';
    }
    if(state.afe_gb && $('a35afe')){
      $('a35afe').textContent = state.afe_gb.toUpperCase();
      $('a35afe').style.color = state.afe_gb === 'outdoor' ? 'var(--green)' : 'var(--amber)';
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
    toggleAfe: function(){ this.set('afe_gb', state.afe_gb === 'outdoor' ? 'indoor' : 'outdoor'); },
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
})(scope);
</script>
"""

PANEL_FORMAT = CSS + HTML + JS

# ── Events panel (lightning / disturber / noise + retained last_event) ──
EVT_CSS = """\
<style>
#a35ev *{box-sizing:border-box;margin:0;padding:0}
#a35ev{
  font-family:var(--font); background:var(--bg); color:var(--text);
  padding:10px; display:flex; flex-direction:column; gap:8px;
  font-size:13px;
}
#a35ev .card{
  background:var(--card); border:1px solid var(--border);
  border-radius:8px; padding:10px 14px;
}
#a35ev .lastev{display:flex; align-items:center; gap:14px}
#a35ev .icon{font-size:30px; line-height:1; min-width:34px;
  text-align:center}
#a35ev .lastev .body{flex:1; display:flex; flex-direction:column; gap:3px}
#a35ev .lastev .label{
  font-size:10px; color:var(--muted);
  text-transform:uppercase; letter-spacing:1px;
}
#a35ev .lastev .summary{
  font-size:15px; font-weight:600; color:var(--text);
}
#a35ev .lastev .summary.lightning{color:var(--red)}
#a35ev .lastev .summary.disturber{color:var(--amber)}
#a35ev .lastev .summary.noise{color:var(--muted)}
#a35ev .lastev .when{
  font-family:var(--mono); font-size:11px; color:var(--muted);
}
#a35ev .countstrip{display:flex; gap:18px;
  font-family:var(--mono); font-size:13px}
#a35ev .cnt{display:flex; align-items:center; gap:5px}
#a35ev .cnt .v{font-weight:700; color:var(--text)}
#a35ev .cnt.lightning .v{color:var(--red)}
#a35ev .cnt.disturber .v{color:var(--amber)}
#a35ev .cnt.noise .v{color:var(--muted)}
#a35ev .log{
  font-family:var(--mono); font-size:11px;
  max-height:280px; overflow-y:auto;
  display:flex; flex-direction:column;
}
#a35ev .row{
  display:flex; gap:10px; padding:4px 0;
  border-bottom:1px solid var(--border);
}
#a35ev .row:last-of-type{border-bottom:none}
#a35ev .row.lightning .ev{color:var(--red)}
#a35ev .row.disturber .ev{color:var(--amber)}
#a35ev .row.noise .ev{color:var(--muted)}
#a35ev .ts{color:var(--muted); flex-shrink:0; width:78px}
#a35ev .ev{flex:1}
#a35ev .empty{color:var(--muted); text-align:center; padding:14px;
  font-family:var(--font); font-size:12px}
#a35ev .sect{
  font-size:10px; color:var(--muted);
  text-transform:uppercase; letter-spacing:1px; margin-bottom:6px;
}
</style>
"""

EVT_HTML = """\
<div id="a35ev">
  <div class="card lastev">
    <span class="icon" id="a35ev-icon">—</span>
    <div class="body">
      <span class="label">Last Event  (retained — survives restart)</span>
      <span class="summary" id="a35ev-summary">awaiting first event...</span>
      <span class="when" id="a35ev-when"></span>
    </div>
  </div>

  <div class="card">
    <div class="sect">Session counters  (since this browser opened)</div>
    <div class="countstrip">
      <span class="cnt lightning"><span>⚡ lightning</span><span class="v" id="a35ev-c-l">0</span></span>
      <span class="cnt disturber"><span>⚠ disturber</span><span class="v" id="a35ev-c-d">0</span></span>
      <span class="cnt noise"><span>📡 noise</span><span class="v" id="a35ev-c-n">0</span></span>
    </div>
  </div>

  <div class="card">
    <div class="sect">Recent events  (newest first · capped at 30)</div>
    <div class="log" id="a35ev-log">
      <div class="empty">no events yet</div>
    </div>
  </div>
</div>
"""

EVT_JS = """\
<script>
(function(scope){
  // Pattern B (matches tuning panel + chrony card): IIFE takes scope as
  // parameter from the top-level Angular controller. Don't shadow scope
  // with `var scope = this;` — IIFE's `this` is window.
  var log = [];                                // newest-first, cap 30
  var counts = { lightning:0, disturber:0, noise:0 };
  var lastEv = null;
  var lastEvWallMs = 0;

  function $(id){ return document.getElementById(id); }

  function fmtClock(ts){
    if(!ts) return '—';
    var s = String(ts);
    var i = s.indexOf('T');
    if(i >= 0) return s.slice(i+1).split('.')[0];   // ISO 8601 → HH:MM:SS
    return s;
  }
  function agoText(ms){
    if(!ms) return '';
    var age = Math.max(0, Math.floor((Date.now()-ms)/1000));
    if(age < 60)    return age + 's ago';
    if(age < 3600)  return Math.floor(age/60) + 'm ago';
    if(age < 86400) return Math.floor(age/3600) + 'h ago';
    return Math.floor(age/86400) + 'd ago';
  }
  function iconFor(ev){
    return ev === 'lightning' ? '⚡'
         : ev === 'disturber' ? '⚠'
         : ev === 'noise'     ? '📡'
         : '—';
  }
  function distText(p){
    // AS3935 reports distance 63 (0x3F) as the out-of-range sentinel.
    if(p.distance === 63) return 'out of range';
    if(p.distance == null) return '?';
    if(p.distance === 0)   return 'overhead';
    return p.distance + ' km';
  }

  function renderLast(){
    if(!lastEv){ return; }
    $('a35ev-icon').textContent = iconFor(lastEv.event);
    var line;
    if(lastEv.event === 'lightning'){
      line = '⚡ Lightning · ' + distText(lastEv) + ' · energy ' + (lastEv.energy||0);
    } else if(lastEv.event === 'disturber'){
      line = '⚠ Disturber  (man-made interference rejected by the chip)';
    } else if(lastEv.event === 'noise'){
      line = '📡 Noise floor too high  (raise NF if persistent)';
    } else {
      line = String(lastEv.event || 'unknown');
    }
    var sum = $('a35ev-summary');
    sum.textContent = line;
    sum.className = 'summary ' + (lastEv.event || '');
    $('a35ev-when').textContent =
      fmtClock(lastEv.timestamp) + '  ·  ' + agoText(lastEvWallMs);
  }
  function renderCounts(){
    $('a35ev-c-l').textContent = counts.lightning;
    $('a35ev-c-d').textContent = counts.disturber;
    $('a35ev-c-n').textContent = counts.noise;
  }
  function renderLog(){
    var el = $('a35ev-log');
    if(!log.length){
      el.innerHTML = '<div class="empty">no events yet</div>';
      return;
    }
    var out = '';
    for(var i=0; i<log.length; i++){
      var r = log[i];
      var detail = (r.event === 'lightning')
        ? '⚡ ' + distText(r) + '  ·  e=' + (r.energy||0)
        : (r.event === 'disturber') ? '⚠ disturber'
        : (r.event === 'noise')     ? '📡 noise'
        : r.event;
      out += '<div class="row ' + r.event + '">'
           + '<span class="ts">' + fmtClock(r.timestamp) + '</span>'
           + '<span class="ev">' + detail + '</span>'
           + '</div>';
    }
    el.innerHTML = out;
  }

  // Live event from lightning/as3935 — append to log, bump counters,
  // promote to Last Event card.
  function ingestLive(p){
    if(!p || !p.event) return;
    if(counts[p.event] != null) counts[p.event]++;
    log.unshift(p);
    if(log.length > 30) log.length = 30;
    lastEv = p;
    lastEvWallMs = Date.now();
    renderLast(); renderCounts(); renderLog();
  }

  // Retained last_event — rehydrate Last Event card only.
  // Do NOT touch the log (retained replay would conflict with live ingest).
  function ingestRetained(p){
    if(!p || !p.event) return;
    lastEv = p;
    lastEvWallMs = p.ts_epoch_ms || Date.now();
    renderLast();
  }

  scope.$watch('msg', function(msg){
    if(!msg || !msg.topic) return;
    var p = msg.payload;
    if(typeof p === 'string'){ try { p = JSON.parse(p); } catch(e){ return; } }
    if(msg.topic === 'lightning/as3935')                ingestLive(p);
    else if(msg.topic === 'lightning/as3935/last_event') ingestRetained(p);
  });

  // 1Hz tick — keep the "Xs ago" label fresh without server traffic.
  if(!scope._a35evTicker){
    scope._a35evTicker = setInterval(function(){
      if(lastEv) $('a35ev-when').textContent =
        fmtClock(lastEv.timestamp) + '  ·  ' + agoText(lastEvWallMs);
    }, 1000);
    scope.$on('$destroy', function(){
      clearInterval(scope._a35evTicker);
      scope._a35evTicker = null;
    });
  }

  renderLast(); renderCounts(); renderLog();
})(scope);
</script>
"""

EVT_PANEL_FORMAT = EVT_CSS + EVT_HTML + EVT_JS

# ── Flow JSON structure ──
FLOW = [
    {
        "id": "as3935_ctl_flow",
        "type": "tab",
        "label": "AS3935 Bridge",
        "disabled": False,
        "info": "MQTT control + status + events panels for the vu2cpl-as3935-bridge ESP32.\n\nTuning panel publishes to lightning/as3935/cmd, subscribes to status/hb/cmd/ack.\nEvents panel subscribes to lightning/as3935 (live stream) and lightning/as3935/last_event (retained — survives Node-RED restart).\n\nTest plan: see nodered/README.md → Comprehensive test plan."
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
        "id": "as3935_evt_grp",
        "type": "ui_group",
        "name": "AS3935 Events",
        "tab": "bcce4e07ac31b882",
        "order": 31,
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
        "wires": [["as3935_tuning_cache_status"]]
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
        "wires": [["as3935_tuning_cache_hb"]]
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
        "wires": [["as3935_tuning_cache_ack"]]
    },
    {
        "id": "as3935_tuning_cache_status",
        "type": "function",
        "z": "as3935_ctl_flow",
        "name": "Cache /status",
        "func": (
            "// Pass-through: cache msg.payload to flow.as3935_status so the\n"
            "// 5s replay tick can rehydrate the Control Panel within ~5s\n"
            "// of any page open (browser refresh or fresh tab).\n"
            "flow.set('as3935_status', msg.payload);\n"
            "return msg;\n"
        ),
        "outputs": 1,
        "timeout": 0,
        "noerr": 0,
        "initialize": "",
        "finalize": "",
        "libs": [],
        "x": 320, "y": 100,
        "wires": [["as3935_ctl_panel"]]
    },
    {
        "id": "as3935_tuning_cache_hb",
        "type": "function",
        "z": "as3935_ctl_flow",
        "name": "Cache /hb",
        "func": (
            "// Pass-through: cache msg.payload to flow.as3935_hb so the\n"
            "// 5s replay tick can rehydrate the Control Panel within ~5s\n"
            "// of any page open (browser refresh or fresh tab).\n"
            "flow.set('as3935_hb', msg.payload);\n"
            "return msg;\n"
        ),
        "outputs": 1,
        "timeout": 0,
        "noerr": 0,
        "initialize": "",
        "finalize": "",
        "libs": [],
        "x": 320, "y": 160,
        "wires": [["as3935_ctl_panel"]]
    },
    {
        "id": "as3935_tuning_cache_ack",
        "type": "function",
        "z": "as3935_ctl_flow",
        "name": "Cache /cmd_ack",
        "func": (
            "// Pass-through: cache msg.payload to flow.as3935_cmd_ack so the\n"
            "// 5s replay tick can rehydrate the Control Panel within ~5s\n"
            "// of any page open (browser refresh or fresh tab).\n"
            "flow.set('as3935_cmd_ack', msg.payload);\n"
            "return msg;\n"
        ),
        "outputs": 1,
        "timeout": 0,
        "noerr": 0,
        "initialize": "",
        "finalize": "",
        "libs": [],
        "x": 320, "y": 220,
        "wires": [["as3935_ctl_panel"]]
    },
    {
        "id": "as3935_tuning_replay_tick",
        "type": "inject",
        "z": "as3935_ctl_flow",
        "name": "Replay every 5s",
        "props": [{"p": "payload"}],
        "repeat": "5",
        "crontab": "",
        "once": True,
        "onceDelay": 1,
        "topic": "",
        "payload": "",
        "payloadType": "date",
        "x": 160, "y": 300,
        "wires": [["as3935_tuning_replay_fn", "as3935_evt_replay_fn"]]
    },
    {
        "id": "as3935_tuning_replay_fn",
        "type": "function",
        "z": "as3935_ctl_flow",
        "name": "Replay AS3935 state (5s tick)",
        "func": (
            "// Fires every 5s from the replay tick inject. Reads cached MQTT state\n"
            "// from flow context and re-emits to the Control Panel with original\n"
            "// msg.topic preserved (the Control Panel dispatches on msg.topic in\n"
            "// scope.$watch, no template change needed).\n"
            "//\n"
            "// Worst-case rehydration after opening the dashboard cold: 5s.\n"
            "// Background traffic when idle (no clients connected): same 5s tick,\n"
            "// emits up to 3 small JSON messages to the ui_template -- trivial load,\n"
            "// node-red-dashboard handles fan-out efficiently.\n"
            "//\n"
            "// Why a periodic tick instead of node-red-dashboard's ui_control event\n"
            "// node: ui_control isn't shipped in node-red-dashboard 3.6.6 (confirmed\n"
            "// by --force reinstall, files genuinely absent). Tick is the workable\n"
            "// substitute until either Dashboard 1 ships ui_control again or we\n"
            "// migrate widgets to Dashboard 2 (FlowFuse).\n"
            "const status = flow.get('as3935_status');\n"
            "const hb     = flow.get('as3935_hb');\n"
            "const ack    = flow.get('as3935_cmd_ack');\n"
            "\n"
            "let emitted = 0;\n"
            "if (status) { node.send({ topic: 'lightning/as3935/status',  payload: status }); emitted++; }\n"
            "if (hb)     { node.send({ topic: 'lightning/as3935/hb',      payload: hb     }); emitted++; }\n"
            "if (ack)    { node.send({ topic: 'lightning/as3935/cmd/ack', payload: ack    }); emitted++; }\n"
            "\n"
            "node.status({fill: emitted ? 'green' : 'grey', shape: 'dot',\n"
            "             text: 'replay tick · ' + emitted + ' msg(s)'});\n"
            "return null;\n"
        ),
        "outputs": 1,
        "timeout": 0,
        "noerr": 0,
        "initialize": "",
        "finalize": "",
        "libs": [],
        "x": 380, "y": 300,
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
    },

    # ── Events panel ─────────────────────────────────────────────────
    {
        "id": "as3935_evt_in",
        "type": "mqtt in",
        "z": "as3935_ctl_flow",
        "name": "AS3935 Event",
        "topic": "lightning/as3935",
        "qos": "0",
        "datatype": "json",
        "broker": "f4785be9863eab08",
        "nl": False, "rap": False, "rh": 0,
        "inputs": 0,
        "x": 160, "y": 440,
        "wires": [["as3935_evt_panel"]]
    },
    {
        "id": "as3935_evt_last_in",
        "type": "mqtt in",
        "z": "as3935_ctl_flow",
        "name": "AS3935 Last Event (retained)",
        "topic": "lightning/as3935/last_event",
        "qos": "1",
        "datatype": "json",
        "broker": "f4785be9863eab08",
        "nl": False, "rap": True, "rh": 0,
        "inputs": 0,
        "x": 210, "y": 500,
        "wires": [["as3935_evt_cache_last"]]
    },
    {
        "id": "as3935_evt_cache_last",
        "type": "function",
        "z": "as3935_ctl_flow",
        "name": "Cache /last_event",
        "func": (
            "// Cache the retained last_event payload to flow context. The 5s\n"
            "// replay tick re-emits it so the Last Event card rehydrates after\n"
            "// a page refresh even if no fresh event has arrived since.\n"
            "flow.set('as3935_last_event', msg.payload);\n"
            "return msg;\n"
        ),
        "outputs": 1,
        "timeout": 0,
        "noerr": 0,
        "initialize": "",
        "finalize": "",
        "libs": [],
        "x": 420, "y": 500,
        "wires": [["as3935_evt_panel"]]
    },
    {
        "id": "as3935_evt_replay_fn",
        "type": "function",
        "z": "as3935_ctl_flow",
        "name": "Replay last_event (5s tick)",
        "func": (
            "// Shares the as3935_tuning_replay_tick. Reads cached last_event\n"
            "// and re-emits with original topic preserved so the Events panel\n"
            "// Last Event card rehydrates within 5s of a page open.\n"
            "//\n"
            "// We deliberately do NOT replay live lightning/as3935 events --\n"
            "// the rolling log is session-only (replaying would double-count\n"
            "// and shuffle ordering on every tick).\n"
            "const last = flow.get('as3935_last_event');\n"
            "if (last) {\n"
            "    node.send({ topic: 'lightning/as3935/last_event', payload: last });\n"
            "    node.status({fill:'green', shape:'dot', text:'replay · last_event'});\n"
            "} else {\n"
            "    node.status({fill:'grey', shape:'ring', text:'replay · empty'});\n"
            "}\n"
            "return null;\n"
        ),
        "outputs": 1,
        "timeout": 0,
        "noerr": 0,
        "initialize": "",
        "finalize": "",
        "libs": [],
        "x": 420, "y": 560,
        "wires": [["as3935_evt_panel"]]
    },
    {
        "id": "as3935_evt_panel",
        "type": "ui_template",
        "z": "as3935_ctl_flow",
        "group": "as3935_evt_grp",
        "name": "AS3935 Events Panel",
        "order": 1,
        "width": "12",
        "height": "16",
        "format": EVT_PANEL_FORMAT,
        "storeOutMessages": True,
        "fwdInMessages": False,
        "resendOnRefresh": True,
        "templateScope": "local",
        "className": "",
        "x": 700, "y": 500,
        "wires": [[]]
    },

    # ── Test injects (publish fake events to lightning/as3935) ───────
    # Use these to exercise the Events panel end-to-end without the
    # ESP32. Each press publishes a single MQTT message; the panel
    # picks it up via its own mqtt-in subscription.
    {
        "id": "as3935_test_lightning_near",
        "type": "inject",
        "z": "as3935_ctl_flow",
        "name": "TEST · ⚡ lightning @ 5 km",
        "props": [{"p": "payload"}],
        "repeat": "",
        "crontab": "",
        "once": False,
        "onceDelay": 0.1,
        "topic": "",
        "payload": '{"event":"lightning","distance":5,"energy":850000,"timestamp":"TEST"}',
        "payloadType": "json",
        "x": 220, "y": 660,
        "wires": [["as3935_evt_test_out"]]
    },
    {
        "id": "as3935_test_lightning_far",
        "type": "inject",
        "z": "as3935_ctl_flow",
        "name": "TEST · ⚡ lightning @ 25 km",
        "props": [{"p": "payload"}],
        "repeat": "",
        "crontab": "",
        "once": False,
        "onceDelay": 0.1,
        "topic": "",
        "payload": '{"event":"lightning","distance":25,"energy":120000,"timestamp":"TEST"}',
        "payloadType": "json",
        "x": 220, "y": 700,
        "wires": [["as3935_evt_test_out"]]
    },
    {
        "id": "as3935_test_lightning_oor",
        "type": "inject",
        "z": "as3935_ctl_flow",
        "name": "TEST · ⚡ lightning out-of-range",
        "props": [{"p": "payload"}],
        "repeat": "",
        "crontab": "",
        "once": False,
        "onceDelay": 0.1,
        "topic": "",
        "payload": '{"event":"lightning","distance":63,"energy":40000,"timestamp":"TEST"}',
        "payloadType": "json",
        "x": 230, "y": 740,
        "wires": [["as3935_evt_test_out"]]
    },
    {
        "id": "as3935_test_disturber",
        "type": "inject",
        "z": "as3935_ctl_flow",
        "name": "TEST · ⚠ disturber",
        "props": [{"p": "payload"}],
        "repeat": "",
        "crontab": "",
        "once": False,
        "onceDelay": 0.1,
        "topic": "",
        "payload": '{"event":"disturber","timestamp":"TEST"}',
        "payloadType": "json",
        "x": 190, "y": 780,
        "wires": [["as3935_evt_test_out"]]
    },
    {
        "id": "as3935_test_noise",
        "type": "inject",
        "z": "as3935_ctl_flow",
        "name": "TEST · 📡 noise",
        "props": [{"p": "payload"}],
        "repeat": "",
        "crontab": "",
        "once": False,
        "onceDelay": 0.1,
        "topic": "",
        "payload": '{"event":"noise","timestamp":"TEST"}',
        "payloadType": "json",
        "x": 180, "y": 820,
        "wires": [["as3935_evt_test_out"]]
    },
    {
        "id": "as3935_evt_test_out",
        "type": "mqtt out",
        "z": "as3935_ctl_flow",
        "name": "TEST publish → lightning/as3935",
        "topic": "lightning/as3935",
        "qos": "0",
        "retain": "false",
        "respTopic": "",
        "contentType": "",
        "userProps": "",
        "correl": "",
        "expiry": "",
        "broker": "f4785be9863eab08",
        "x": 580, "y": 740,
        "wires": []
    },
    {
        "id": "as3935_test_comment",
        "type": "comment",
        "z": "as3935_ctl_flow",
        "name": "TEST INJECTS — exercise the Events panel without the ESP32",
        "info": "Each inject button publishes one MQTT message to lightning/as3935. The Events panel subscribes to that topic and renders the event.\n\nSee nodered/README.md → Comprehensive test plan for the full walk-through.",
        "x": 280, "y": 620,
        "wires": []
    }
]

OUT = "/Users/manoj/projects/vu2cpl-as3935-bridge/nodered/as3935-control-flow.json"
with open(OUT, "w") as f:
    json.dump(FLOW, f, indent=2)
print(f"Wrote {OUT} ({sum(1 for _ in open(OUT))} lines, "
      f"{len(open(OUT).read())} bytes)")
