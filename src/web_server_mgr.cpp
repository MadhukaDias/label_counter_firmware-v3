#include "web_server_mgr.h"
#include "config.h"
#include "imu_detector.h"
#include <WebServer.h>
#include <ArduinoJson.h>

static WebServer server(80);
static AppConfig* _cfg     = nullptr;
static bool _updated       = false;
static bool _mqttOk        = false;
static bool _vibActive     = false;

void webServerSetMqttOk(bool ok)    { _mqttOk   = ok; }
void webServerSetVibActive(bool va) { _vibActive = va; }

// ─────────────────────────────────────────────────────────────────────────────
//  Inline HTML — no LittleFS required, zero filesystem dependency
// ─────────────────────────────────────────────────────────────────────────────
static const char INDEX_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Label Counter</title>
<style>
@import url('https://fonts.googleapis.com/css2?family=Share+Tech+Mono&family=Barlow:wght@400;600;700&display=swap');
:root{
  --bg:#0d0f12;--surf:#161a1f;--brd:#2a2f38;
  --acc:#00e5a0;--acc2:#ff6b35;--txt:#e0e6ef;--dim:#5a6475;
  --mono:'Share Tech Mono',monospace;--sans:'Barlow',sans-serif;
}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--txt);font-family:var(--sans);min-height:100vh}
body::before{content:'';position:fixed;inset:0;background:repeating-linear-gradient(0deg,transparent,transparent 2px,rgba(0,0,0,.06) 2px,rgba(0,0,0,.06) 4px);pointer-events:none;z-index:9}
header{background:var(--surf);border-bottom:1px solid var(--brd);padding:16px 24px;display:flex;align-items:center;gap:12px}
.logo{width:34px;height:34px;background:var(--acc);display:flex;align-items:center;justify-content:center;clip-path:polygon(0 0,100% 0,100% 75%,75% 100%,0 100%)}
.logo svg{width:18px;height:18px;fill:#0d0f12}
.brand{font-size:1rem;font-weight:700;letter-spacing:.08em;text-transform:uppercase}
.brand span{color:var(--acc)}
.did{margin-left:auto;font-family:var(--mono);font-size:.7rem;color:var(--dim);border:1px solid var(--brd);padding:3px 8px}
main{max-width:680px;margin:0 auto;padding:28px 18px 60px}

/* Stats */
.stats{display:grid;grid-template-columns:repeat(4,1fr);gap:10px;margin-bottom:28px}
.scard{background:var(--surf);border:1px solid var(--brd);padding:12px 14px;position:relative;overflow:hidden}
.scard::after{content:'';position:absolute;top:0;left:0;width:3px;height:100%;background:var(--acc)}
.slbl{font-family:var(--mono);font-size:.6rem;color:var(--dim);text-transform:uppercase;letter-spacing:.12em;margin-bottom:5px}
.sval{font-family:var(--mono);font-size:1.4rem;color:var(--acc)}
.sval.warn{color:var(--acc2)}

/* Chart */
.chart-wrap{background:var(--surf);border:1px solid var(--brd);padding:16px;margin-bottom:28px}
.chart-hdr{display:flex;align-items:center;justify-content:space-between;margin-bottom:10px}
.chart-title{font-family:var(--mono);font-size:.7rem;color:var(--dim);text-transform:uppercase;letter-spacing:.15em}
.thr-line-lbl{font-family:var(--mono);font-size:.65rem;color:var(--acc2)}
canvas{width:100%!important;display:block;image-rendering:pixelated}

/* Section heading */
.sec{font-size:.68rem;font-family:var(--mono);color:var(--dim);text-transform:uppercase;letter-spacing:.2em;margin-bottom:14px;display:flex;align-items:center;gap:10px}
.sec::after{content:'';flex:1;height:1px;background:var(--brd)}

/* Fields */
.fg{background:var(--surf);border:1px solid var(--brd);padding:18px;margin-bottom:2px;transition:border-color .2s}
.fg:hover{border-color:var(--acc)}
.fhdr{display:flex;align-items:baseline;justify-content:space-between;margin-bottom:8px}
.fname{font-weight:600;font-size:.88rem}
.funit{font-family:var(--mono);font-size:.68rem;color:var(--dim);border:1px solid var(--brd);padding:2px 6px}
.fdesc{font-size:.76rem;color:var(--dim);margin-bottom:12px;line-height:1.5}
.srow{display:flex;align-items:center;gap:12px}
input[type=range]{flex:1;-webkit-appearance:none;height:4px;background:var(--brd);outline:none}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:17px;height:17px;background:var(--acc);cursor:pointer;clip-path:polygon(50% 0%,100% 50%,50% 100%,0% 50%);transition:transform .15s}
input[type=range]::-webkit-slider-thumb:hover{transform:scale(1.3)}
input[type=number]{width:86px;background:var(--bg);border:1px solid var(--brd);color:var(--acc);font-family:var(--mono);font-size:.95rem;padding:5px 9px;text-align:right;outline:none;transition:border-color .2s}
input[type=number]:focus{border-color:var(--acc)}

.brow{display:flex;gap:10px;margin-top:10px}
button{flex:1;padding:13px;font-family:var(--sans);font-size:.82rem;font-weight:700;letter-spacing:.1em;text-transform:uppercase;cursor:pointer;border:none;transition:all .2s}
.bsave{background:var(--acc);color:#0d0f12;clip-path:polygon(0 0,100% 0,100% 80%,96% 100%,0 100%)}
.bsave:hover{background:#00ffb3;transform:translateY(-1px)}
.breset{background:transparent;color:var(--acc2);border:1px solid var(--acc2)}
.breset:hover{background:rgba(255,107,53,.12)}

/* Count control */
.cctl{background:var(--surf);border:1px solid var(--brd);padding:18px;display:flex;align-items:center;justify-content:space-between;margin-top:8px}
.cbig{font-family:var(--mono);font-size:2.6rem;color:var(--acc)}
.bcnt{background:transparent;color:var(--acc2);border:1px solid var(--acc2);padding:10px 18px;font-family:var(--mono);font-size:.78rem;cursor:pointer;text-transform:uppercase;letter-spacing:.1em;transition:all .2s;flex:none}
.bcnt:hover{background:rgba(255,107,53,.15)}

/* Toast */
#toast{position:fixed;bottom:22px;right:22px;background:var(--acc);color:#0d0f12;font-family:var(--mono);font-size:.78rem;padding:11px 18px;transform:translateY(70px);opacity:0;transition:all .32s cubic-bezier(.23,1,.32,1);font-weight:700;letter-spacing:.05em}
#toast.show{transform:translateY(0);opacity:1}

@media(max-width:500px){.stats{grid-template-columns:1fr 1fr}}
</style>
</head>
<body>
<header>
  <div class="logo"><svg viewBox="0 0 24 24" stroke-width="0"><path d="M12 2L2 7l10 5 10-5-10-5zM2 17l10 5 10-5M2 12l10 5 10-5"/></svg></div>
  <div class="brand">Label<span>Counter</span></div>
  <div class="did" id="did">ID: --</div>
</header>

<main>
  <!-- Live stats -->
  <div class="stats">
    <div class="scard"><div class="slbl">Count</div><div class="sval" id="lc">--</div></div>
    <div class="scard"><div class="slbl">Vibration</div><div class="sval" id="lv">--</div></div>
    <div class="scard"><div class="slbl">State</div><div class="sval" id="ls">--</div></div>
    <div class="scard"><div class="slbl">MQTT</div><div class="sval" id="lm">--</div></div>
  </div>

  <!-- Vibration plot -->
  <div class="chart-wrap">
    <div class="chart-hdr">
      <div style="display:flex;align-items:center;gap:10px">
        <span class="chart-title">&#9640; Live Vibration Magnitude</span>
        <label style="display:flex;align-items:center;gap:4px;cursor:pointer">
          <input type="checkbox" id="plot-toggle" checked style="accent-color:var(--acc)">
          <span style="font-family:var(--mono);font-size:0.6rem;color:var(--dim);text-transform:uppercase">Plot Data</span>
        </label>
      </div>
      <span class="thr-line-lbl" id="thr-lbl">THR: --</span>
    </div>
    <canvas id="chart" height="110"></canvas>
    <div id="live-thr-stats" style="font-size:0.8rem; color:var(--acc); text-align:center; padding-top:8px;">
      Temp Start: <span id="c-st">--</span> | Temp Stop: <span id="c-sp">--</span>
    </div>
  </div>

  <!-- Threshold params -->
  <div class="sec">Vibration Parameters</div>
  <form id="cfg-form">
    <div class="fg">
      <div class="fhdr">
        <span class="fname">Vibration Threshold</span>
        <button type="button" class="bcnt" style="margin-left:auto; font-size:0.7rem; padding:4px 8px; width:auto; border-color:var(--acc); color:var(--acc);" onclick="autoCalibrate()">Auto-Calibrate</button>
        <span class="funit" style="margin-left:10px;">raw Δ</span>
      </div>
      <div class="fdesc">
        <span>Minimum 3-axis vibration delta above idle.</span>
        <div style="margin-top:5px; color:var(--acc); font-size:0.85rem;">
          Stop Thr: <span id="s-sthr">--</span> | cMax: <span id="s-cmax">--</span> | cMin: <span id="s-cmin">--</span> | Spike Thr: <span id="s-spikethr">--</span> | Lock Peak: <span id="s-lpk">--</span>
        </div>
      </div>
      <div class="srow">
        <input type="range" id="s-thr" min="100" max="8000" step="50">
        <input type="number" id="n-thr" min="100" max="8000" step="50">
      </div>
    </div>
    <div class="fg">
      <div class="fhdr"><span class="fname">Min Sewing Duration</span><span class="funit">ms</span></div>
      <div class="fdesc">Vibration must persist this long to be confirmed. Prevents false triggers from bumps or table knocks.</div>
      <div class="srow">
        <input type="range" id="s-dur" min="100" max="3000" step="50">
        <input type="number" id="n-dur" min="100" max="3000" step="50">
      </div>
    </div>
    <div class="fg">
      <div class="fhdr"><span class="fname">Silence Window</span><span class="funit">ms</span></div>
      <div class="fdesc">Quiet time after sewing stops before count triggers. Prevents double-counting on a single label.</div>
      <div class="srow">
        <input type="range" id="s-sil" min="200" max="5000" step="50">
        <input type="number" id="n-sil" min="200" max="5000" step="50">
      </div>
    </div>
    <div class="fg">
      <div class="fhdr"><span class="fname">MQTT Publish Interval</span><span class="funit">seconds</span></div>
      <div class="fdesc">How often to push count + status to MQTT broker. A count event always publishes immediately regardless of this interval.</div>
      <div class="srow">
        <input type="range" id="s-mqi" min="5" max="300" step="5">
        <input type="number" id="n-mqi" min="5" max="300" step="5">
      </div>
    </div>
    <div class="fg" style="display:flex;align-items:center;gap:10px;padding:14px 18px">
      <input type="checkbox" id="mqtt-toggle" style="width:18px;height:18px;accent-color:var(--acc)">
      <label for="mqtt-toggle" class="fname">Enable MQTT Publishing</label>
    </div>
    <div class="brow">
      <button type="submit" class="bsave">&#9654; Save All</button>
      <button type="button" class="breset" onclick="resetDefs()">Reset Defaults</button>
    </div>
  </form>

  <div class="sec" style="margin-top:28px">Count Control</div>
  <div class="cctl">
    <div>
      <div style="font-size:.7rem;color:var(--dim);margin-bottom:3px;font-family:var(--mono)">CURRENT COUNT</div>
      <div class="cbig" id="cnt-big">--</div>
    </div>
    <button class="bcnt" onclick="resetCount()">&#9744; Reset to Zero</button>
  </div>
</main>

  <!-- Calibration Modal -->
  <div id="calib-modal" style="display:none; position:fixed; top:0; left:0; width:100%; height:100%; background:rgba(0,0,0,0.85); z-index:9999; justify-content:center; align-items:center;">
    <div style="background:var(--bg); border:1px solid var(--border); padding:20px; border-radius:8px; width:90%; max-width:400px; text-align:center; box-shadow:0 10px 30px rgba(0,0,0,0.5);">
      <h3 style="margin-top:0; color:var(--acc);">System Calibration</h3>
      <div id="calib-step-1" style="margin:15px 0;">
        <div style="font-weight:bold; font-size:1.1rem; margin-bottom:5px;">Step 1: Noise Profiling</div>
        <div style="color:var(--dim); font-size:0.9rem;">Please do not touch the machine for 3 seconds...</div>
        <div style="margin-top:10px; width:100%; height:8px; background:var(--panel); border-radius:4px; overflow:hidden;">
          <div id="calib-progress" style="width:0%; height:100%; background:var(--acc); transition:width 0.3s;"></div>
        </div>
      </div>
      <div id="calib-step-2" style="margin:15px 0; display:none;">
        <div style="font-weight:bold; font-size:1.1rem; margin-bottom:5px;">Step 2: Solenoid Detection</div>
        <div style="color:var(--dim); font-size:0.9rem; margin-bottom:10px;">Please actuate the lock solenoid manually.</div>
        <div style="font-size:1.5rem; font-family:var(--mono); color:var(--acc); margin-bottom:15px;">
          <span id="calib-locks">0</span> / 3
        </div>
        <button onclick="skipCalibPhase2()" style="padding:6px 12px; background:transparent; border:1px solid var(--dim); color:var(--dim); border-radius:4px; font-size:0.85rem; cursor:pointer;">Skip (No Solenoid)</button>
      </div>
      <div id="calib-step-3" style="margin:15px 0; display:none; color:#00e5a0; font-weight:bold; font-size:1.1rem;">
        Calibration Complete!
      </div>
    </div>
  </div>

<div id="toast"></div>

<script>
// ── Chart setup ────────────────────────────────────────────────────────────────
const canvas = document.getElementById('chart');
const ctx    = canvas.getContext('2d');
const W = 640, H = 110;
canvas.width  = W;
canvas.height = H;

const HIST   = 80;   // samples to keep
const magBuf = new Array(HIST).fill(0);
const actBuf = new Array(HIST).fill(false);
const pendingBuf = new Array(HIST).fill(false);
let   curThr = 800;
let   lastCount = null;

function drawChart() {
  const maxVal = Math.max(curThr * 1.4, ...magBuf, 100);
  ctx.clearRect(0, 0, W, H);

  // Grid lines
  ctx.strokeStyle = '#2a2f38';
  ctx.lineWidth = 1;
  for (let i = 0; i <= 4; i++) {
    const y = Math.round(H - (i / 4) * H);
    ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(W, y); ctx.stroke();
    ctx.fillStyle = '#5a6475';
    ctx.font = '9px Share Tech Mono, monospace';
    ctx.fillText(Math.round((i / 4) * maxVal), 3, y - 2);
  }

  // X-axis moving stamps (every sample = 300ms)
  ctx.beginPath();
  const step = W / (HIST - 1);
  for (let i = 0; i < HIST; i++) {
    const x = i * step;
    ctx.moveTo(x, H);
    ctx.lineTo(x, H - 4);
  }
  ctx.strokeStyle = '#5a6475';
  ctx.stroke();

  // Threshold line
  const ty = H - (curThr / maxVal) * H;
  ctx.strokeStyle = '#ffaa00';
  ctx.setLineDash([4, 4]);
  ctx.lineWidth = 1.5;
  ctx.beginPath(); ctx.moveTo(0, ty); ctx.lineTo(W, ty); ctx.stroke();
  
  // Stop Threshold line
  if (typeof curStopThr !== 'undefined') {
    const sy = H - (curStopThr / maxVal) * H;
    ctx.strokeStyle = '#00ffff';
    ctx.setLineDash([2, 2]);
    ctx.beginPath(); ctx.moveTo(0, sy); ctx.lineTo(W, sy); ctx.stroke();
  }
  ctx.setLineDash([]);

  // Fill under curve
  ctx.beginPath();
  ctx.moveTo(0, H);
  for (let i = 0; i < HIST; i++) {
    const x = i * step;
    const y = H - (magBuf[i] / maxVal) * H;
    ctx.lineTo(x, y);
  }
  ctx.lineTo(W, H);
  ctx.closePath();
  ctx.fillStyle = 'rgba(0,229,160,0.08)';
  ctx.fill();

  // Line segments with state colors
  ctx.lineWidth   = 2;
  ctx.lineJoin    = 'round';
  for (let i = 1; i < HIST; i++) {
    ctx.beginPath();
    const x0 = (i - 1) * step;
    const y0 = H - (magBuf[i - 1] / maxVal) * H;
    const x1 = i * step;
    const y1 = H - (magBuf[i] / maxVal) * H;
    ctx.moveTo(x0, y0);
    ctx.lineTo(x1, y1);
    ctx.strokeStyle = (actBuf[i] || actBuf[i-1]) ? '#ff6b35' : '#00e5a0';
    ctx.stroke();
  }

  // Current value dot
  const lastY = H - (magBuf[HIST-1] / maxVal) * H;
  ctx.fillStyle = magBuf[HIST-1] >= curThr ? '#ff6b35' : '#00e5a0';
  ctx.beginPath();
  ctx.arc(W - 1, lastY, 4, 0, Math.PI * 2);
  ctx.fill();
}

// ── Slider ↔ number sync ───────────────────────────────────────────────────────
let curStopThr = 0;
[['s-thr', 'n-thr'], ['s-dur', 'n-dur'], ['s-sil', 'n-sil'], ['s-mqi', 'n-mqi']]
  .forEach(([sid, nid]) => {
    const s = document.getElementById(sid);
    const n = document.getElementById(nid);
    s.addEventListener('input', () => { 
      n.value = s.value; 
      if (sid === 's-thr') { 
        curThr = +s.value; 
        curStopThr = Math.max(0, curThr - 100);
        document.getElementById('thr-lbl').textContent = 'THR: ' + s.value; 
        document.getElementById('c-st').textContent = curThr;
        document.getElementById('c-sp').textContent = curStopThr;
      } 
    });
    n.addEventListener('input', () => { 
      s.value = n.value; 
      if (nid === 'n-thr') { 
        curThr = +n.value; 
        curStopThr = Math.max(0, curThr - 100);
        document.getElementById('thr-lbl').textContent = 'THR: ' + n.value; 
        document.getElementById('c-st').textContent = curThr;
        document.getElementById('c-sp').textContent = curStopThr;
      } 
    });
  });

// ── Load config ────────────────────────────────────────────────────────────────
async function loadConfig() {
  try {
    const d = await (await fetch('/api/config')).json();
    document.getElementById('s-thr').value = d.threshold;
    document.getElementById('n-thr').value = d.threshold;
    document.getElementById('s-dur').value = d.minDur;
    document.getElementById('n-dur').value = d.minDur;
    document.getElementById('s-sil').value = d.silence;
    document.getElementById('n-sil').value = d.silence;
    // mqttInterval stored in seconds in UI, ms on device
    const mqtts = Math.round(d.mqttInterval / 1000);
    document.getElementById('s-mqi').value = mqtts;
    document.getElementById('n-mqi').value = mqtts;
    document.getElementById('mqtt-toggle').checked = d.mqttEnabled;
    document.getElementById('did').textContent = 'ID: ' + (d.deviceId || '--');
    curThr = d.threshold;
    curStopThr = d.tempStop;
    document.getElementById('thr-lbl').textContent = 'THR: ' + curThr;
    document.getElementById('c-st').textContent = d.threshold;
    document.getElementById('c-sp').textContent = d.tempStop;
    
    document.getElementById('s-sthr').textContent = d.tempStop;
    document.getElementById('s-cmax').textContent = d.lastCalibMax;
    document.getElementById('s-cmin').textContent = d.lastCalibMin;
    document.getElementById('s-spikethr').textContent = d.lastSpikeThr;
    document.getElementById('s-lpk').textContent = d.lastLockPeak;
  } catch(e){ console.error(e); }
}

  // ── Poll status ────────────────────────────────────────────────────────────────
  const STATES = ['IDLE', 'VIBRATING', 'SEWING', 'COOLING'];
  let confirmedUntil = 0;
async function poll() {
  try {
    const d = await (await fetch('/api/status')).json();
    
    if (isPlotting) {
      magBuf.shift(); magBuf.push(d.mag || 0);
      pendingBuf.shift(); pendingBuf.push(d.vibActive || false);
      actBuf.shift(); actBuf.push(false);

      if (lastCount !== null && d.count !== undefined && d.count > lastCount) {
        let foundTrue = false;
        let falseCount = 0;
        for (let i = HIST - 1; i >= 0; i--) {
          if (pendingBuf[i]) {
            actBuf[i] = true;
            pendingBuf[i] = false;
            foundTrue = true;
            for (let j = 1; j <= falseCount; j++) {
              if (i + j < HIST) actBuf[i + j] = true;
            }
            falseCount = 0;
          } else if (foundTrue) {
            falseCount++;
              if (falseCount > 20) break; // ~6 seconds gap tolerance
            }
          }
          confirmedUntil = Date.now() + 1000;
        }
        drawChart();
    }
    if (d.count !== undefined) lastCount = d.count;

    document.getElementById('lc').textContent = d.count ?? '--';
    document.getElementById('cnt-big').textContent = d.count ?? '--';

    const vEl = document.getElementById('lv');
    vEl.textContent = d.mag ?? '--';
    vEl.className   = 'sval' + (d.vibActive ? ' warn' : '');

      let stateText = STATES[d.state] || '--';
      if (Date.now() < confirmedUntil) stateText = 'CONFIRMED';

      const sEl = document.getElementById('ls');
      sEl.textContent = stateText;
      sEl.className = 'sval' + ((d.state > 0 || stateText === 'CONFIRMED') ? ' warn' : '');

    const mEl = document.getElementById('lm');
    mEl.textContent = d.mqtt ? 'OK' : 'OFF';
    mEl.className   = 'sval' + (d.mqtt ? '' : ' warn');
  } catch(e){}
}

// ── Save ───────────────────────────────────────────────────────────────────────
document.getElementById('cfg-form').addEventListener('submit', async e => {
  e.preventDefault();
  const body = {
    threshold: +document.getElementById('n-thr').value,
    minDur:    +document.getElementById('n-dur').value,
    silence:   +document.getElementById('n-sil').value,
    mqttInterval: +document.getElementById('n-mqi').value * 1000,
    mqttEnabled: document.getElementById('mqtt-toggle').checked
  };
  const r = await fetch('/api/config', {
    method:'POST', headers:{'Content-Type':'application/json'},
    body: JSON.stringify(body)
  });
  toast(r.ok ? 'Configuration saved' : 'Save failed');
});

function resetDefs() {
  fetch('/api/reset-config',{method:'POST'}).then(()=>{ loadConfig(); toast('Defaults restored'); });
}
function resetCount() {
  if (!confirm('Reset count to zero?')) return;
  fetch('/api/reset-count',{method:'POST'}).then(()=> toast('Count reset'));
}
function toast(msg) {
  const t = document.getElementById('toast');
  t.textContent = '✓ ' + msg;
  t.classList.add('show');
  setTimeout(()=> t.classList.remove('show'), 2800);
}

let calibInterval = null;
let calibTimer = 0;

function autoCalibrate() {
  if (!confirm('Ensure the machine is ON but IDLE (not sewing). Continue?')) return;
  
  const modal = document.getElementById('calib-modal');
  const s1 = document.getElementById('calib-step-1');
  const s2 = document.getElementById('calib-step-2');
  const s3 = document.getElementById('calib-step-3');
  const pbar = document.getElementById('calib-progress');
  const lcnt = document.getElementById('calib-locks');
  
  modal.style.display = 'flex';
  s1.style.display = 'block';
  s2.style.display = 'none';
  s3.style.display = 'none';
  pbar.style.width = '0%';
  lcnt.textContent = '0';
  calibTimer = 0;
  
  fetch('/api/calibrate', { method: 'POST' }).then(() => {
    if (calibInterval) clearInterval(calibInterval);
    
    calibInterval = setInterval(async () => {
      try {
        const r = await fetch('/api/calib_status');
        const d = await r.json();
        
        // d.state: 0=IDLE, 1=SAMPLING, 2=NOISE_DONE, 3=LOCK_WAITING
        if (d.state === 1 || d.state === 2) {
          calibTimer += 300;
          pbar.style.width = Math.min(100, (calibTimer / 3000) * 100) + '%';
        } else if (d.state === 3) {
          s1.style.display = 'none';
          s2.style.display = 'block';
          lcnt.textContent = d.lockCount;
        } else if (d.state === 0 && calibTimer > 0) {
          // Finished
          clearInterval(calibInterval);
          s2.style.display = 'none';
          s3.style.display = 'block';
          loadConfig();
          setTimeout(() => {
            modal.style.display = 'none';
            toast('Calibration complete!');
          }, 1500);
        }
      } catch(e) {}
    }, 300);
  });
}

function skipCalibPhase2() {
  fetch('/api/calib_skip', { method: 'POST' });
}

let isPlotting = true;
let pollTimer = setInterval(poll, 300);

document.getElementById('plot-toggle').addEventListener('change', (e) => {
  isPlotting = e.target.checked;
  clearInterval(pollTimer);
  pollTimer = setInterval(poll, isPlotting ? 300 : 2000);
});

loadConfig();
</script>
</body>
</html>
)rawhtml";

// ─────────────────────────────────────────────────────────────────────────────
//  Route handlers
// ─────────────────────────────────────────────────────────────────────────────

static uint8_t _sewState = 0;   // exposed from main via setter

void webServerSetSewState(uint8_t s) { _sewState = s; }

static void handleRoot() {
    server.send_P(200, "text/html", INDEX_HTML);
}

static void handleGetConfig() {
    JsonDocument doc;
    doc["threshold"]    = _cfg->vib.threshold;
    doc["minDur"]       = _cfg->vib.minDurationMs;
    doc["silence"]      = _cfg->vib.silenceMs;
    doc["mqttInterval"] = _cfg->mqttIntervalMs;
    doc["mqttEnabled"]  = _cfg->mqttEnabled;
    doc["count"]        = _cfg->count;
    doc["deviceId"]     = _cfg->deviceId;
    doc["lastCalibMax"] = _cfg->lastCalibMax;
    doc["lastCalibMin"] = _cfg->lastCalibMin;
    doc["lastSpikeThr"] = _cfg->lastSpikeThr;
    doc["lastLockPeak"] = _cfg->lastLockPeak;
    
    doc["tempStop"]     = _cfg->vib.stopThreshold;
    
    String out; serializeJson(doc, out);
    server.send(200, "application/json", out);
}

static void handlePostConfig() {
    if (!server.hasArg("plain")) { server.send(400, "text/plain", "No body"); return; }
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain"))) { server.send(400, "text/plain", "Bad JSON"); return; }

    if (doc["threshold"].is<int>()) {
        _cfg->vib.threshold     = constrain((int)doc["threshold"], 100, 8000);
        int32_t stopThr = _cfg->vib.threshold > 100 ? _cfg->vib.threshold - 100 : 0;
        if (stopThr < (int32_t)_cfg->lastCalibMax) stopThr = _cfg->lastCalibMax + 50;
        _cfg->vib.stopThreshold = stopThr;
    }
    if (doc["minDur"].is<int>())
        _cfg->vib.minDurationMs = constrain((int)doc["minDur"], 100, 3000);
    if (doc["silence"].is<int>())
        _cfg->vib.silenceMs     = constrain((int)doc["silence"], 200, 5000);
    if (doc["mqttInterval"].is<int>())
        _cfg->mqttIntervalMs    = constrain((int)doc["mqttInterval"], 5000, 300000);
    if (doc["mqttEnabled"].is<bool>())
        _cfg->mqttEnabled       = doc["mqttEnabled"];

    cfgSave(*_cfg);
    _updated = true;
    server.send(200, "application/json", "{\"ok\":true}");
    Serial.println("[WEB] Config updated via portal.");
}

static void handleResetConfig() {
    cfgReset(*_cfg);
    _updated = true;
    server.send(200, "application/json", "{\"ok\":true}");
}

static void handleResetCount() {
    _cfg->count = 0;
    cfgSaveCount(0);
    server.send(200, "application/json", "{\"ok\":true}");
}

extern void startCalibration();
static void handleCalibrate() {
    startCalibration();
    server.send(200, "application/json", "{\"ok\":true}");
}

extern void skipCalibPhase2();
static void handleCalibSkip() {
    skipCalibPhase2();
    server.send(200, "application/json", "{\"ok\":true}");
}

extern void getCalibStatus(int& state, int& lockCount);
static void handleCalibStatus() {
    int state, lockCount;
    getCalibStatus(state, lockCount);
    JsonDocument doc;
    doc["state"] = state;
    doc["lockCount"] = lockCount;
    String out; serializeJson(doc, out);
    server.send(200, "application/json", out);
}

static void handleStatus() {
    JsonDocument doc;
    doc["count"]     = _cfg->count;
    doc["mag"]       = imuGetMagnitude();
    doc["vibActive"] = _vibActive;
    doc["state"]     = _sewState;
    doc["mqtt"]      = _mqttOk;
    String out; serializeJson(doc, out);
    server.send(200, "application/json", out);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Public API
// ─────────────────────────────────────────────────────────────────────────────

void webServerInit(AppConfig* cfg) {
    _cfg = cfg;
    server.on("/",                HTTP_GET,  handleRoot);
    server.on("/api/config",      HTTP_GET,  handleGetConfig);
    server.on("/api/config",      HTTP_POST, handlePostConfig);
    server.on("/api/reset-config",HTTP_POST, handleResetConfig);
    server.on("/api/reset-count", HTTP_POST, handleResetCount);
    server.on("/api/calibrate",   HTTP_POST, handleCalibrate);
    server.on("/api/calib_skip",  HTTP_POST, handleCalibSkip);
    server.on("/api/calib_status",HTTP_GET,  handleCalibStatus);
    server.on("/api/status",      HTTP_GET,  handleStatus);
    server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
    server.begin();
    Serial.println("[WEB] HTTP server started on port 80");
}

void webServerLoop()           { server.handleClient(); }
bool webServerHasUpdate()      { return _updated; }
void webServerClearUpdate()    { _updated = false; }
