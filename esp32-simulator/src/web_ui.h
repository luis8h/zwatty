#pragma once
#include <pgmspace.h>

// Single-page control UI, served from flash. Talks to /meta, /state, /set and /do.
static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>FTMS simulator</title>
<style>
:root{--bg:#eef0f2;--card:#fff;--fg:#161a1f;--mute:#65707d;--line:#d9dde2;--acc:#0a6bd6;--bad:#b3261e;--ok:#17803a}
@media (prefers-color-scheme:dark){:root{--bg:#101418;--card:#181e24;--fg:#e7ebef;--mute:#8b96a3;--line:#29313a;--acc:#5aa6ff;--bad:#ff8a80;--ok:#6fdc8c}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.45 system-ui,-apple-system,"Segoe UI",sans-serif}
header{max-width:900px;margin:0 auto;padding:16px 12px 8px}
h1{font-size:20px;margin:0 0 8px}
h2{font-size:15px;margin:0 0 10px}
main{max-width:900px;margin:0 auto;padding:0 12px 40px;display:grid;gap:12px}
section{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px}
.pills{display:flex;gap:6px;flex-wrap:wrap}
.pill{font-size:13px;padding:2px 10px;border-radius:99px;border:1px solid var(--line);color:var(--mute)}
.pill.on{color:var(--ok);border-color:var(--ok)}
.pill.warn{color:var(--bad);border-color:var(--bad)}
.tiles{display:grid;grid-template-columns:repeat(auto-fill,minmax(120px,1fr));gap:8px}
.tile{border-left:3px solid var(--acc);padding:2px 10px}
.tile span{display:block;font-size:13px;color:var(--mute)}
.tile b{font-size:21px;font-weight:600;font-variant-numeric:tabular-nums}
.ctl{display:grid;grid-template-columns:1fr auto;align-items:center;gap:2px 10px;margin-bottom:12px}
.ctl output{font-variant-numeric:tabular-nums;color:var(--mute)}
.ctl input[type=range]{grid-column:1/3;width:100%;accent-color:var(--acc)}
.ctl input[type=checkbox]{width:20px;height:20px;accent-color:var(--acc)}
.cols{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:0 28px}
.btns{display:flex;gap:8px;flex-wrap:wrap}
button{font:inherit;padding:8px 12px;border-radius:8px;border:1px solid var(--line);background:var(--card);color:var(--fg);cursor:pointer}
button:hover{border-color:var(--acc)}
button:focus-visible,input:focus-visible{outline:2px solid var(--acc);outline-offset:2px}
button.danger{color:var(--bad);border-color:var(--bad)}
#toast{min-height:1.4em;color:var(--mute);font-size:14px;margin:10px 0 0}
#log{margin:0;padding:0;list-style:none;font:13px/1.6 ui-monospace,Menlo,Consolas,monospace;max-height:280px;overflow:auto}
#log li{display:flex;gap:12px;border-bottom:1px solid var(--line);padding:1px 0}
#log time{color:var(--mute);flex:none;width:58px;text-align:right}
</style>
</head>
<body>
<header>
  <h1>FTMS trainer simulator</h1>
  <div class="pills">
    <span class="pill" id="p_conn">Connecting…</span>
    <span class="pill" id="p_ble">No app connected</span>
    <span class="pill" id="p_sub">Data off</span>
    <span class="pill" id="p_ctl">No control</span>
    <span class="pill" id="p_run">Stopped</span>
    <span class="pill warn" id="p_stall" style="display:none">Data stalled</span>
  </div>
</header>
<main>
  <section>
    <h2>What your app sees</h2>
    <div class="tiles" id="tiles"></div>
  </section>
  <section>
    <h2>Rider and trainer model</h2>
    <div class="cols" id="controls"></div>
  </section>
  <section>
    <h2>Fault injection</h2>
    <div class="btns">
      <button class="danger" onclick="act('drop')">Disconnect app</button>
      <button onclick="act('stall',10)">Stop data for 10 s</button>
      <button onclick="act('inject','short')">Send short packet</button>
      <button onclick="act('inject','garbage')">Send garbage packet</button>
      <button onclick="act('inject','zero')">Send all-zero packet</button>
      <button onclick="act('inject','max')">Send max-value packet</button>
      <button onclick="act('inject','negative')">Send negative power</button>
    </div>
    <p id="toast"></p>
  </section>
  <section>
    <h2>Session</h2>
    <div class="btns">
      <button onclick="act('reset')">Reset session</button>
      <button onclick="act('defaults')">Restore default settings</button>
    </div>
  </section>
  <section>
    <h2>Activity</h2>
    <ul id="log"></ul>
  </section>
</main>
<script>
const $=s=>document.querySelector(s);
const fmt=t=>{t=Math.round(t);return String(Math.floor(t/60)).padStart(2,'0')+':'+String(t%60).padStart(2,'0')};
const TILES=[
 ['mode','Mode',s=>s.mode],
 ['power','Power',s=>s.power+' W'],
 ['cadence','Cadence',s=>s.cadence+' rpm'],
 ['speed','Speed',s=>s.speed_kmh+' km/h'],
 ['dist','Distance',s=>(s.distance_m/1000).toFixed(2)+' km'],
 ['time','Elapsed',s=>fmt(s.elapsed_s)],
 ['tp','ERG target',s=>s.mode==='erg'?s.target_power+' W':'-'],
 ['grade','Grade',s=>s.mode==='sim'?s.grade+' %':'-'],
 ['wind','Wind',s=>s.mode==='sim'?s.wind+' m/s':'-'],
 ['crr','Crr',s=>s.mode==='sim'?s.crr:'-'],
 ['cw','Cw',s=>s.mode==='sim'?s.cw+' kg/m':'-'],
 ['res','Resistance',s=>s.mode==='resistance'?(s.resistance/10).toFixed(1):'-']
];
let meta=[];const timers={};
async function api(path){try{const r=await fetch(path,{cache:'no-store'});return await r.json()}catch(e){return null}}
function pill(id,cls,text){const e=$('#'+id);e.className='pill'+(cls?' '+cls:'');e.textContent=text}

function buildUI(){
  $('#tiles').innerHTML=TILES.map(t=>`<div class="tile"><span>${t[1]}</span><b id="t_${t[0]}">-</b></div>`).join('');
  const box=$('#controls');
  meta.forEach(m=>{
    const toggle=m.min===0&&m.max===1;
    const d=document.createElement('div');d.className='ctl';
    d.innerHTML=`<label for="c_${m.key}">${m.label}</label>`+(toggle?'':`<output id="o_${m.key}"></output>`)+
      `<input id="c_${m.key}" type="${toggle?'checkbox':'range'}" min="${m.min}" max="${m.max}" step="${m.step}">`;
    box.appendChild(d);
    const el=d.querySelector('input');
    el.addEventListener('input',()=>{
      const v=toggle?(el.checked?1:0):el.value;
      const o=$('#o_'+m.key);if(o)o.textContent=v+' '+m.unit;
      clearTimeout(timers[m.key]);
      timers[m.key]=setTimeout(()=>api('/set?'+m.key+'='+v),150);
    });
  });
}

async function act(cmd,arg){
  const r=await api('/do?cmd='+cmd+(arg!==undefined?'&arg='+arg:''));
  $('#toast').textContent=r?r.msg:'Request failed. Is the Wi-Fi connection still up?';
  poll();
}

async function poll(){
  const s=await api('/state');
  if(!s){pill('p_conn','warn','Simulator unreachable');return}
  pill('p_conn','on','Simulator online');
  pill('p_ble',s.connected?'on':'',s.connected?'App connected':'No app connected');
  pill('p_sub',s.subscribed?'on':'',s.subscribed?'Data on':'Data off');
  pill('p_ctl',s.control?'on':'',s.control?'App has control':'No control');
  pill('p_run',s.running?'on':'',s.running?'Running':'Stopped');
  $('#p_stall').style.display=s.stalled?'':'none';
  if(s.stalled)$('#p_stall').className='pill warn';
  TILES.forEach(t=>{$('#t_'+t[0]).textContent=t[2](s)});
  meta.forEach(m=>{
    const el=$('#c_'+m.key),v=s.cfg[m.key];
    if(document.activeElement!==el){if(el.type==='checkbox')el.checked=v>=0.5;else el.value=v}
    const o=$('#o_'+m.key);if(o)o.textContent=el.value+' '+m.unit;
  });
  $('#log').replaceChildren(...s.log.slice().reverse().map(e=>{
    const li=document.createElement('li'),t=document.createElement('time'),m=document.createElement('span');
    t.textContent=(e.t/1000).toFixed(1)+'s';m.textContent=e.m;li.append(t,m);return li}));
}

(async()=>{meta=(await api('/meta'))||[];buildUI();await poll();setInterval(poll,500)})();
</script>
</body>
</html>
)HTML";
