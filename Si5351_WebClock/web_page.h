// web_page.h — single-page UI served from flash
#pragma once
#include <pgmspace.h>

static const char INDEX_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Si5351A Clock Generator</title>
<style>
:root{--bg:#0f1216;--card:#181d24;--line:#2a313b;--fg:#e6e9ee;--mut:#8b95a3;--acc:#3fb68b;--err:#e5534b;--warn:#d9a441}
@media (prefers-color-scheme:light){:root{--bg:#f3f5f8;--card:#fff;--line:#dde2e8;--fg:#1b2027;--mut:#66707d}}
*{box-sizing:border-box}body{margin:0;font:15px/1.4 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;background:var(--bg);color:var(--fg)}
header{padding:18px 20px 6px;max-width:1100px;margin:auto;display:flex;flex-wrap:wrap;gap:8px 16px;align-items:baseline}
h1{font-size:20px;margin:0;font-weight:650}#status{color:var(--mut);font-size:13px}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px;background:var(--mut)}
.ok .dot{background:var(--acc)}.bad .dot{background:var(--err)}
main{max-width:1100px;margin:auto;padding:10px 20px 30px;display:grid;gap:14px;grid-template-columns:repeat(auto-fit,minmax(300px,1fr))}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:16px}
.card h2{margin:0;font-size:16px;display:flex;justify-content:space-between;align-items:center}
.read{font:600 26px/1.2 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;margin:12px 0 2px;letter-spacing:.5px;word-break:break-all}
.off .read{color:var(--mut)}.unit{font-size:15px;color:var(--mut);margin-left:4px}
.sub{color:var(--mut);font-size:12.5px;min-height:34px}
.row{display:flex;gap:8px;margin-top:10px}
input,select,button{font:inherit;color:var(--fg);background:var(--bg);border:1px solid var(--line);border-radius:8px;padding:8px 10px}
input{flex:1;min-width:0;font-family:ui-monospace,Menlo,Consolas,monospace}
button{background:var(--acc);border-color:var(--acc);color:#fff;font-weight:600;cursor:pointer}
button.sec{background:transparent;color:var(--fg);border-color:var(--line)}
label.l{font-size:12px;color:var(--mut);display:block;margin-top:10px}
.sw{position:relative;width:42px;height:24px;flex:none}.sw input{opacity:0;width:0;height:0}
.sw span{position:absolute;inset:0;border-radius:24px;background:var(--line);transition:.15s;cursor:pointer}
.sw span:before{content:"";position:absolute;width:18px;height:18px;left:3px;top:3px;border-radius:50%;background:#fff;transition:.15s}
.sw input:checked+span{background:var(--acc)}.sw input:checked+span:before{transform:translateX(18px)}
.chips{display:flex;flex-wrap:wrap;gap:6px;margin-top:10px}
.chips button{padding:3px 9px;font-size:12px;font-weight:500;background:transparent;color:var(--mut);border-color:var(--line)}
#msg{max-width:1100px;margin:0 auto;padding:0 20px}
#msg div{border-radius:10px;padding:10px 14px;margin-top:8px;font-size:14px}
.e{background:rgba(229,83,75,.12);border:1px solid var(--err)}.i{background:rgba(63,182,139,.12);border:1px solid var(--acc)}
.full{grid-column:1/-1}.grid2{display:grid;grid-template-columns:1fr 1fr;gap:0 12px}
small{color:var(--mut)}
</style></head><body>
<header><h1>Si5351A Clock Generator</h1><span id="status"><span class="dot"></span>connecting…</span></header>
<div id="msg"></div>
<main id="clocks"></main>
<main>
 <section class="card full">
  <h2>Reference &amp; calibration</h2>
  <div class="grid2">
   <div><label class="l">Crystal frequency (Hz) — enter the measured value to calibrate</label>
    <div class="row"><input id="xtal" inputmode="numeric"></div></div>
   <div><label class="l">Crystal load capacitance</label>
    <div class="row"><select id="load"><option value="6">6 pF</option><option value="8">8 pF</option><option value="10">10 pF</option></select>
    <button onclick="saveRef()">Save</button></div></div>
  </div>
  <p><small>Range 2.5 kHz – 200 MHz per output. Outputs above 112.5 MHz use an integer divider and need their own PLL;
  the planner shares the two PLLs automatically and shows the exact frequency produced. Changing a clock may briefly
  glitch other clocks on the same PLL. Calibrate: set CLK0 to 10 MHz, measure it, then enter
  <code>crystal × measured / 10 000 000</code>.</small></p>
  <div id="pll" class="sub"></div>
 </section>
</main>
<script>
const U={Hz:1,kHz:1e3,MHz:1e6},$=s=>document.querySelector(s);let st=null,busy=false;
const PRE=[32768,1e6,10e6,25e6,100e6,200e6];
function fmt(hz,dp){if(hz>=1e6)return[(hz/1e6).toFixed(dp??6),'MHz'];if(hz>=1e3)return[(hz/1e3).toFixed(dp??3),'kHz'];return[(+hz).toFixed(dp??3),'Hz']}
function short(hz){let[v,u]=fmt(hz,6);v=v.replace(/\.?0+$/,'');return v+' '+u}
function msg(t,cls){$('#msg').innerHTML=t?`<div class="${cls||'e'}">${t}</div>`:''}
function build(){let h='';for(let i=0;i<3;i++)h+=`
 <section class="card" id="c${i}"><h2>CLK${i}<label class="sw"><input type="checkbox" id="en${i}" onchange="send({['en${i}']:this.checked?1:0})"><span></span></label></h2>
 <div class="read" id="r${i}">—</div><div class="sub" id="s${i}"></div>
 <div class="row"><input id="f${i}" inputmode="decimal" placeholder="frequency" onkeydown="if(event.key=='Enter')setF(${i})">
 <select id="u${i}"><option>Hz</option><option>kHz</option><option selected>MHz</option></select>
 <button onclick="setF(${i})">Set</button></div>
 <label class="l">Drive strength</label><div class="row"><select id="d${i}" onchange="send({['d${i}']:this.value})">
 <option value="2">2 mA</option><option value="4">4 mA</option><option value="6">6 mA</option><option value="8">8 mA</option></select></div>
 <div class="chips">${PRE.map(p=>`<button onclick="send({f${i}:${p},en${i}:1})">${short(p)}</button>`).join('')}</div></section>`;
 $('#clocks').innerHTML=h}
function render(s){st=s;const S=$('#status');S.className=s.chip?'ok':'bad';
 S.innerHTML=`<span class="dot"></span>${s.chip?'Si5351 detected':'Si5351 NOT detected (check wiring)'} · ${s.net}`;
 s.clk.forEach((c,i)=>{$('#c'+i).classList.toggle('off',!c.en);$('#en'+i).checked=c.en;$('#d'+i).value=c.drive;
  const[v,u]=fmt(c.en?c.actual:c.f);$('#r'+i).innerHTML=`${v}<span class="unit">${u}</span>`;
  if(document.activeElement!==$('#f'+i)){const[fv,fu]=fmt(c.f);$('#f'+i).value=fv.replace(/\.?0+$/,'');$('#u'+i).value=fu}
  const e=c.actual-c.f;
  $('#s'+i).innerHTML=c.en?`PLL ${c.pll} · MultiSynth ${c.ms}${c.r>1?' · R ÷'+c.r:''} (${c.mode})<br>target ${short(c.f)} · error ${Math.abs(e)<5e-4?'0':e.toFixed(3)} Hz`:'output off';});
 if(document.activeElement!==$('#xtal'))$('#xtal').value=s.xtal;$('#load').value=s.load;
 $('#pll').innerHTML=s.pll.map((p,g)=>`PLL ${'AB'[g]}: `+(p.used?`VCO ${(p.vco/1e6).toFixed(6)} MHz = XTAL × ${p.mult}`:'unused')).join(' &nbsp;·&nbsp; ')}
async function send(o){if(busy)return;busy=true;try{
 const r=await fetch('/api/set',{method:'POST',body:new URLSearchParams(o)});const j=await r.json();
 if(!j.ok){msg(j.error);if(st)render(st)}else{msg('');render(j)}}catch(e){msg('Request failed: '+e)}busy=false}
function setF(i){const v=parseFloat($('#f'+i).value.replace(',','.'));if(!isFinite(v)){msg('Enter a number');return}
 const hz=Math.round(v*U[$('#u'+i).value]);send({['f'+i]:hz,['en'+i]:1})}
function saveRef(){send({xtal:$('#xtal').value.trim(),load:$('#load').value})}
async function poll(){try{const r=await fetch('/api/state');render(await r.json())}catch(e){}}
build();poll();setInterval(()=>{if(!busy)poll()},5000);
</script></body></html>)HTML";
