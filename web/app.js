// Dashboard logic for index.html (moved out of the page unchanged). Pure maths lives in math.js,
// which is tested by test_display.js.
'use strict';
const $=id=>document.getElementById(id), canvas=$('chart'), ctx=canvas.getContext('2d');
let duration=600, rows=6, stripHeight=140, units='knots', gain=1, calOffset=0, calibrated=false;
let points=[], cursor=0, state=null, live=true, fixedEnd=0, offset=0, loaded=false, generation=0, geometry=null;
const valueOf=hz=>WindMath.convert(hz,units,gain,calOffset);
const CALM_S=20; // no reed closure for this long = deduced calm, drawn as 0 (display only)
const itemValue=it=>it.calm?0:valueOf(it.hz);
const durText=s=>s<60?`${Math.round(s)} s`:s<3600?`${Math.floor(s/60)} m ${Math.round(s%60)} s`:`${Math.floor(s/3600)} h ${Math.round(s%3600/60)} m`;
const unitLabel=()=>WindMath.labels[units];
// Display precision: wind speed 0.01, RPM 0.1, Hz 0.001 (period 0.01 s, A/B 0.001 s, rain 0.1 mm).
const unitDec=()=>({hz:3,rpm:1})[units]??2;
// Big card numbers: pad with figure spaces (one digit wide, like the tabular digits) to a fixed
// number of integer digits, so the decimal point stays put as values change.
const FIG='\u2007';
const pad=(v,d,int)=>{if(v==null||!Number.isFinite(Number(v)))return '—';const t=Number(v).toFixed(d),w=int+(d?d+1:0);return t.length<w?FIG.repeat(w-t.length)+t:t;};
function saveSettings(){try{localStorage.setItem('windSettings',JSON.stringify({duration,rows,stripHeight,units,gain,calOffset,calibrated,ymax:$('ymax').value}));}catch(e){}}
function displayLabels(){
 const wind=!['hz','rpm'].includes(units);
 $('speedUnit').textContent=unitLabel();$('speedTitle').textContent=wind?'Wind speed':'Rotation speed';
 $('chartTitle').textContent=`${wind?'Wind':'Rotation'} speed · ${unitLabel()}`;
 $('yLabel').textContent=`Y maximum, ${unitLabel()}`;
 canvas.setAttribute('aria-label',`Live ${unitLabel()} history with min, average and max per strip, newest at top`);
 $('calibrationNote').className=calibrated?'caption':'warn';
 $('calibrationNote').textContent=calibrated?`Calibration: knots = max(0, ${gain} × Hz ${calOffset<0?'−':'+'} ${Math.abs(calOffset)}). ${wind?'Applied to the display.':'Not used for Hz/RPM.'}`:`UNCALIBRATED: provisional 1 knot/Hz. ${wind?'Set your calibration before interpreting wind speed.':'Hz/RPM are independent of wind calibration.'}`;
}
const clock=()=>Date.now()/1000+offset;
// Maintenance windows (from the server): data inside is recorded but not shown or totalled.
const windows=()=>state?.maintenance||[];
const winEnd=w=>w.end??clock();
const inMaint=t=>t!=null&&windows().some(w=>t>=w.start&&t<winEnd(w));
// Sensor faults (fault.py): one sensor's data is wrong, e.g. a disconnected wire reading calm.
const SENSOR_NAMES={wind:'Anemometer',rain:'Rain gauge',baro:'Barometer',air:'Air temperature/humidity'};
const faults=sensor=>(state?.faults||[]).filter(f=>f.sensor===sensor);
const activeFault=sensor=>(state?.faults_active||[]).find(f=>f.sensor===sensor);
const inFault=(sensor,t)=>t!=null&&faults(sensor).some(f=>t>=f.start&&t<winEnd(f));
const hm=t=>new Date(t*1000).toLocaleString([],{month:'short',day:'numeric',hour:'2-digit',minute:'2-digit',hour12:false});
function paintMaint(){
 const fa=state?.faults_active||[];
 $('faultBanner').hidden=!fa.length;
 $('faultBanner').textContent=fa.map(f=>`⚠ ${SENSOR_NAMES[f.sensor]} fault since ${hm(f.start)}${f.note?` (${f.note})`:''}: its readings are not trusted and are left out of its graphs and totals.`).join(' ');
 const a=state?.maintenance_active;
 $('maintBtn').textContent=a?'End maintenance':'Start maintenance';$('maintBtn').className=a?'active':'';
 $('maintBanner').hidden=!a;
 if(a)$('maintBanner').textContent=`Maintenance since ${hm(a.start)}${a.note?` (${a.note})`:''}: data is still recorded but hidden from graphs and rain totals (the cards still show live readings). Press End maintenance when the sensor is back in service.`;
}
// Shade maintenance windows, and this sensor's faults, overlapping [a,b) on a canvas row.
function shadeMaint(c,a,b,X,y0,h,sensor){
 const marks=[...windows().map(w=>[w,'rgba(228,178,108,.13)','#c9a46a','maintenance']),
  ...(sensor?faults(sensor):[]).map(f=>[f,'rgba(229,96,96,.16)','#ff9a9a',`${SENSOR_NAMES[sensor].toLowerCase()} fault${f.note?' · '+f.note:''}`])];
 for(const [w,fill,ink,label] of marks){const s=Math.max(a,w.start),e=Math.min(b,winEnd(w));if(e<=s)continue;
  const x0=X(s),x1=X(e);c.fillStyle=fill;c.fillRect(x0,y0,Math.max(1,x1-x0),h);
  if(x1-x0>70){c.fillStyle=ink;c.textAlign='left';c.font='11px system-ui';
   const text=c.measureText(label).width<=x1-x0-8?label:label.split(' · ')[0];c.fillText(text,x0+4,y0+10,x1-x0-8);}}
}
const PRESETS=[10,30,60,300,600,1800,3600,7200,10800,21600,43200,86400];
// Strip boundaries fall on local-time multiples of the duration (24 h strips start at midnight).
const tzOff=()=>new Date().getTimezoneOffset()*60;
const alignedEnd=t=>(Math.floor((t-tzOff())/duration)+1)*duration+tzOff();
const endTime=()=>live ? alignedEnd(clock()) : fixedEnd;
// Strips longer than 1 h load condensed history: per bucket, the slowest and fastest
// revolutions plus totals for the strip average (one sample per second would be ~1M points).
const condensed=()=>duration>3600, bucketSize=()=>duration/720;
let buckets=[], lastLoad=0;
const timeText=t=>new Date(t*1000).toLocaleTimeString([], {hour:'2-digit',minute:'2-digit',second:'2-digit',hour12:false});
const dateText=t=>new Date(t*1000).toLocaleString();
const fmt=(v,n=3)=>v==null?'—':Number(v).toFixed(n);
function settings(){
 const d=Number($('duration').value);
 if(!Number.isInteger(d)||d<10||d>86400){$('duration').reportValidity();return false;}
 duration=d;rows=Number($('rows').value);
 $('preset').value=PRESETS.includes(d)?String(d):'custom';
 saveSettings();
 return true;
}
async function get(url){const r=await fetch(url,{signal:AbortSignal.timeout(8000)});if(!r.ok)throw Error(`HTTP ${r.status}`);return r.json();}
function updateState(data){state=data;offset=data.now-Date.now()/1000;cursor=data.cursor;paintMetrics();}
async function load(){
 const gen=++generation;loaded=false;
 try{const end=endTime(), q=condensed()?`&bucket=${bucketSize()}&calm=${CALM_S}`:'';
  const data=await get(`/api/history?start=${end-rows*duration}&end=${end}${q}`);
  if(gen!==generation)return;points=data.points;buckets=data.buckets||[];lastLoad=Date.now();updateState(data);loaded=true;$('error').textContent='';draw();
 }catch(e){if(gen===generation){$('error').textContent=`Cannot load history: ${e.message}`;loaded=true;}}
}
// Calm start for the live reading, or null (link must be fresh: a stale link is not calm).
function liveCalm(now,chart=false){
 const p=state?.latest;
 if(!p||!state.connected||p.pulse_at==null||now-p.received>75||(chart&&state.maintenance_active)||activeFault('wind'))return null;
 const c=WindMath.liveCalmStart(p.pulse_at,p.count,now,CALM_S);
 // Calm cannot be drawn inside a maintenance window: start it where the last window ended.
 const ended=Math.max(0,...[...windows(),...faults('wind')].filter(w=>w.end!=null&&w.end<=now).map(w=>w.end));
 return c==null?null:Math.max(c,ended);
}
function paintMetrics(){
 if(!state)return;
 const p=state.latest, now=clock(), radioAge=p?Math.max(0,now-p.received):null;
 const age=p&&p.measured_at!=null?Math.max(0,now-p.measured_at):null;
 const stale=age==null||age>Math.max(20,2*(p?.period_s||0));
 let text='Receiving',good=true;
 if(!state.connected){text='Receiver disconnected';good=false;}
 else if(state.error){text=state.error;good=false;}
 else if(!p){text='Waiting for sensor';good=false;}
 else if(radioAge>75){text='Radio link stale';good=false;}
 else if(p.hz==null){text='Waiting for full revolution';good=false;}
 const calmFrom=['Receiving','Waiting for full revolution'].includes(text)?liveCalm(now):null, quiet=calmFrom==null?0:now-calmFrom+CALM_S;
 if(calmFrom!=null){text=`Calm · no closure for ${durText(quiet)}`;good=true;}
 else if(stale&&text==='Receiving'){text='Stopped / slow · last reading';good=false;}
 $('status').textContent=text;$('status').className='status '+(good?'ok':'warn');
 paintMaint();
 if(state.maintenance_active){$('status').textContent='Maintenance · '+text;$('status').className='status warn';}
 const wf=activeFault('wind');
 if(wf){$('status').textContent='Anemometer fault · '+$('status').textContent;$('status').className='status warn';}
 if(calmFrom!=null){
  // Same argument as the graph: no closure for over CALM_S s means < 0.5 rev / CALM_S, shown as 0.
  $('hz').textContent=pad(0,unitDec(),3);$('rpm').textContent=pad(0,1,3);
  const lb=pad(age??quiet,2,3);$('period').textContent='>'+(lb.startsWith(FIG)?lb.slice(1):lb);
  $('pair').textContent=`no reed closure for ${durText(quiet)}`;
  $('hz').style.opacity='1';
  $('freshness').textContent=`Deduced calm (no closure for over ${CALM_S} s) · last revolution ${durText(age??quiet)} ago${state.maintenance_active?' · maintenance: not graphed':''}`;
 } else {
 $('hz').textContent=pad(valueOf(p?.hz),unitDec(),3);$('rpm').textContent=pad(p?.hz==null?null:60*p.hz,1,3);
 $('period').textContent=p?.hz==null?'—':pad(p.period_s,2,3);
 $('pair').textContent=`A ${p?.hz==null?'—':fmt(p.a_s,3)+' s'} · B ${p?.hz==null?'—':fmt(p.b_s,3)+' s'}`;
 $('hz').style.opacity=good?'1':'.5';
 $('freshness').textContent=(age==null?'First accepted edge is reference; third completes a revolution':`${good?'Latest':'Last known'} measurement · ${age.toFixed(1)} s old`)+(state.maintenance_active?' · maintenance: not graphed':'')+(wf?' · ANEMOMETER FAULT: not trusted, not graphed':'');
 if(wf)$('hz').style.opacity='.35';
 }
 $('details').textContent=p?`Session ${p.session} · Sequence ${p.seq} · Completed revolutions ${p.revolutions} · Electrical pulses ${p.count}${p.raw_edges!=null?` · Raw D5 edges ${p.raw_edges}`:''}${p.rain_raw_edges!=null?` · Raw D10 edges ${p.rain_raw_edges}`:''}`:`${state.error||'Receiver open; no packets yet.'}`;
 paintDiag(p,radioAge);
}
// Link and sensor card: radio quality, packet loss, battery, clock and reed diagnostics.
const longDur=s=>s<86400?durText(s):`${Math.floor(s/86400)} d ${Math.floor(s%86400/3600)} h`;
function paintDiag(p,radioAge){
 const L=state.link||{}, h1=L.h1, h24=L.h24, esc=v=>String(v).replace(/[&<>"]/g,c=>`&#${c.charCodeAt(0)};`);
 const loss=w=>!w||!w.received?'—':`${w.missing} of ${w.received+w.missing} (${(100*w.missing/(w.received+w.missing)).toFixed(1)}%)`;
 const rows=[];
 const add=(k,v,warn=false)=>rows.push(`<dt>${k}</dt><dd${warn?' class="warn"':''}>${v}</dd>`);
 if(!p){add('Receiver',esc(state.error||'open, no packets yet'),true);}
 else{
  add('RSSI',`${p.rssi} dBm${h1?.rssi!=null?` · 1 h avg ${h1.rssi.toFixed(0)}, min ${h1.rssi_min}`:''}`,p.rssi<-110);
  add('SNR',`${p.snr} dB${h1?.snr!=null?` · 1 h avg ${h1.snr.toFixed(1)}, min ${h1.snr_min}`:''}`,p.snr<0);
  add('Last packet',`${radioAge.toFixed(1)} s ago · seq ${p.seq}`,radioAge>75);
  add('Missed, 1 h',loss(h1),h1&&h1.missing>0.05*(h1.received+h1.missing));
  add('Missed, 24 h',loss(h24));
  const w7=p.version>='W7';
  add('Barometer',!w7?'— (needs W7)':p.pressure_pa==null?'⚠ BMP180 not answering':`BMP180 · raw ${(p.pressure_pa/100).toFixed(2)} hPa · ${p.temp_c.toFixed(1)} °C`,w7&&p.pressure_pa==null);
  const w9=p.version>='W9';
  add('Air sensor',!w9?'— (needs W9)':p.air_temp_c==null?'⚠ SHT31 not answering':`SHT31 · ${p.air_temp_c.toFixed(1)} °C · ${p.rh_pct.toFixed(1)} % RH`,w9&&p.air_temp_c==null);
  add('Battery',p.vbat_v==null?'— (needs W6)':`${p.vbat_v.toFixed(2)} V`,p.vbat_v!=null&&p.vbat_v<3.5);
  add('Sleep',p.sleep==null?'— (needs W8)':p.sleep?'deep standby':'light (USB host seen; reset on battery for deep standby)',p.sleep===0);
  add('32 kHz clock',p.clock==null?'—':p.clock?'⚠ internal fallback (crystal stopped)':'crystal',!!p.clock);
  if(p.pin!=null)add('Wind reed D5',`${p.pin?'open':'closed'}${p.pin_age_s==null?'':` · unchanged ${longDur(p.pin_age_s)}`}`);
  if(p.rain_pin!=null)add('Rain reed D10',`${p.rain_pin?'open':'closed'} · ${p.rain_tips} tips since start`);
  add('Sensor',`${esc(p.version)} · session ${esc(p.session)} · up ${longDur(Number(p.uptime)/32768)}`);
 }
 add('Malformed frames',String(state.malformed),state.malformed>0);
 $('diag').innerHTML=rows.join('');
}
function draw(){
 // Plot fills the strip height; per-strip summary stacks in a column at the right.
 const width=canvas.clientWidth, left=94, right=width<550?92:124, top=22, stripH=stripHeight, plotH=stripH-30, height=top+rows*stripH+12;
 const ratio=window.devicePixelRatio||1;canvas.style.height=height+'px';canvas.width=Math.round(width*ratio);canvas.height=Math.round(height*ratio);ctx.setTransform(ratio,0,0,ratio,0,0);
 const end=endTime(), start=end-duration*rows, plotWidth=width-left-right;
 const visible=points.filter(p=>p.t>=start&&p.t<end).sort((a,b)=>a.t-b.t);
 let maximum=0;for(const p of visible)maximum=Math.max(maximum,valueOf(p.hz));
 // All loaded points: a calm-spanning revolution that completes after this page still draws its calm.
 const series=WindMath.calmSeries(points.slice().sort((a,b)=>a.t-b.t),CALM_S), items=series.items, spans=series.spans;
 const lp=state?.latest, now0=clock();
 const calmFrom=live?liveCalm(now0,true):null, liveCalmSpan=calmFrom==null?null:{start:calmFrom,end:now0,hz:0,calm:true};
 if(liveCalmSpan){
  // Live: no closure for CALM_S (certain; see liveCalmStart); provisional until the next revolution replaces it.
  items.push({t:calmFrom,hz:0,calm:true,provisional:true,stream:lp.stream,period:CALM_S},
             {t:now0,hz:0,calm:true,provisional:true,stream:lp.stream,period:Math.max(CALM_S,now0-calmFrom)});
  spans.push(liveCalmSpan);
 }
 const joinSlack=condensed()?2*bucketSize():0;
 const fixed=Number($('ymax').value), scale=WindMath.niceScale(fixed>0?fixed:maximum*1.1,Math.max(2,Math.floor(plotH/24)),fixed>0), ymax=scale.max;
 geometry={left,right,top,stripH,start,end,plotWidth,width,ymax,items};
 ctx.font='11px system-ui';ctx.textBaseline='middle';
 // Round tick spacing: 2–3 h strips in 20/30 min steps, 6–24 h in 1–4 h steps.
 const ticks=duration>=7200&&duration%6===0?(width<550?3:6):(width<550?4:5);
 for(let row=0;row<rows;row++){
  const y=top+row*stripH, rowStart=end-(row+1)*duration, base=y+6+plotH;
  ctx.fillStyle=row%2?'#132432':'#101e2b';ctx.fillRect(0,y,width,stripH);
  ctx.textAlign='left';ctx.fillStyle='#b8cbd8';ctx.fillText(timeText(rowStart),2,y+14);
  ctx.fillStyle='#7b95a9';ctx.fillText(new Date(rowStart*1000).toLocaleDateString([],{month:'short',day:'numeric'}),2,y+30);
  for(const tick of scale.ticks){
   // Zero line slightly emphasized: calm and dry periods sit on it.
   const yy=base-tick/ymax*plotH;ctx.strokeStyle=tick===0?'#55788f':'#294052';ctx.lineWidth=tick===0?1.6:1;ctx.beginPath();ctx.moveTo(left,yy);ctx.lineTo(width-right,yy);ctx.stroke();ctx.lineWidth=1;
   ctx.fillStyle='#839caf';ctx.textAlign='right';ctx.fillText(WindMath.tickLabel(tick,scale.step),left-5,yy,44);
  }
  for(let j=0;j<=ticks;j++){
   const x=left+plotWidth*j/ticks;ctx.strokeStyle='#223746';ctx.beginPath();ctx.moveTo(x,y+6);ctx.lineTo(x,base);ctx.stroke();
   ctx.fillStyle='#839caf';ctx.textAlign=j===0?'left':j===ticks?'right':'center';
   const elapsed=duration*j/ticks, [div,u]=duration<120?[1,'s']:duration<=10800?[60,'m']:[3600,'h'], value=elapsed/div;
   ctx.fillText(`${value.toFixed(Number.isInteger(value)?0:1)}${u}`,x,base+12);
  }
  shadeMaint(ctx,rowStart,rowStart+duration,t=>left+(t-rowStart)/duration*plotWidth,y+4,plotH+4,'wind');
  const rowPoints=[];let before=null,after=null;
  for(const p of items){if(p.t<rowStart)before=p;else if(p.t<rowStart+duration)rowPoints.push(p);else{after=p;break;}}
  if(before)rowPoints.unshift(before);if(after)rowPoints.push(after);
  const inRow=rowPoints.filter(p=>p.t>=rowStart&&p.t<rowStart+duration);
  const real=WindMath.stats(inRow.filter(p=>!p.calm).map(p=>valueOf(p.hz)));
  let tw;
  if(condensed()){
   // Bucket totals give the average; add any live calm still in progress.
   tw=WindMath.bucketAverage(buckets,rowStart,rowStart+duration,valueOf);
   const o=liveCalmSpan?Math.min(liveCalmSpan.end,rowStart+duration)-Math.max(liveCalmSpan.start,rowStart):0;
   if(o>0)tw=tw?{avg:tw.avg*tw.covered/(tw.covered+o),covered:tw.covered+o,calm:tw.calm+o,count:tw.count}:{avg:0,covered:o,calm:o,count:0};
  } else tw=WindMath.timeAverage(spans,rowStart,rowStart+duration,valueOf);
  const n=condensed()?(tw?tw.count:0):(real?real.count:0);
  const lines=tw?[['max',`${(real?real.max:0).toFixed(unitDec())} ${unitLabel()}`],['avg',tw.avg.toFixed(unitDec())],['min',(tw.calm>0?0:real?real.min:0).toFixed(unitDec())],['n',String(n)]].concat(tw.calm>=1?[['calm',durText(tw.calm)]]:[]):[['','no data']];
  ctx.font='11px system-ui';
  lines.forEach(([k,v],i)=>{const yy=y+14+i*15;if(yy>y+stripH-6)return;
   ctx.textAlign='left';ctx.fillStyle='#7b95a9';ctx.fillText(k,width-right+10,yy);
   ctx.textAlign='right';ctx.fillStyle='#b4c9d8';ctx.fillText(v,width-6,yy,right-44);});
  // Dots only while they fit (about one per 4 px); denser strips draw lines only.
  const dots=inRow.length<=plotWidth/4;
  ctx.save();ctx.beginPath();ctx.rect(left,y+4,plotWidth,plotH+4);ctx.clip();
  let prev=null;ctx.strokeStyle='#68e4b5';ctx.fillStyle='#68e4b5';ctx.lineWidth=1.6;
  for(const p of rowPoints){
   const x=left+(p.t-rowStart)/duration*plotWidth, yy=base-itemValue(p)/ymax*plotH;
   if(prev&&prev.p.stream===p.stream&&p.t-prev.p.t<=Math.max(20,2*Math.max(p.period,prev.p.period))+joinSlack){
    if(p.calm||prev.p.calm)ctx.setLineDash([4,3]);
    ctx.beginPath();ctx.moveTo(prev.x,prev.y);ctx.lineTo(x,yy);ctx.stroke();ctx.setLineDash([]);
   }
   if(dots){ctx.beginPath();ctx.arc(x,yy,p.calm?2.5:2,0,2*Math.PI);
    if(p.calm){ctx.lineWidth=1.2;ctx.stroke();ctx.lineWidth=1.6;}else ctx.fill();}
   prev={p,x,y:yy};
  }
  ctx.restore();
  const now=clock();if(live&&now>=rowStart&&now<rowStart+duration){const x=left+(now-rowStart)/duration*plotWidth;ctx.strokeStyle='#e4b26c';ctx.setLineDash([3,4]);ctx.beginPath();ctx.moveTo(x,y+4);ctx.lineTo(x,base);ctx.stroke();ctx.setLineDash([]);}
 }
 $('range').textContent=`${dateText(start)} — ${dateText(end)}${live?' · LIVE':' · HISTORY'}`;
 $('scale').textContent=`0–${WindMath.tickLabel(ymax,scale.step)} ${unitLabel()} on every strip · ${condensed()?`condensed: slowest and fastest revolution per ${durText(bucketSize())}`:`${visible.length} measurements`}`;
 $('live').className=live?'active':'';
}
// ---- Rain ----
const rainCanvas=$('rainChart'), rctx=rainCanvas.getContext('2d');
// Initial calibration: 2 ml per tip over a 49.5 × 109.5 mm funnel opening = 0.3690 mm/tip.
const RAIN_DEFAULT=Number((2000/(49.5*109.5)).toPrecision(4));
let mmPerTip=RAIN_DEFAULT, rainUnits='mm', rainCalibrated=true, rainDay=null, rainRows=[], rainKey=null, rainFetched=0, rainGen=0;
let rainDays=1, since={kind:'none'}, sinceData=null, sinceGen=0;
const midnight=t=>{const d=new Date(t*1000);d.setHours(0,0,0,0);return d.getTime()/1000;};
const nextMidnight=t=>{const d=new Date(t*1000);d.setHours(24,0,0,0);return d.getTime()/1000;};
const rainVal=mm=>rainUnits==='in'?mm/25.4:mm;
const rainFmt=mm=>mm==null?'—':rainVal(mm).toFixed(rainUnits==='in'?3:1);
const rainPad=(mm,int)=>mm==null?'—':pad(rainVal(mm),rainUnits==='in'?3:1,int);
function saveRain(){try{localStorage.setItem('rainSettings',JSON.stringify({mmPerTip,rainUnits,rainCalibrated,rainDays,since}));}catch(e){}}
function rainLabels(){
 document.querySelector('.rainUnit').textContent=rainUnits;document.querySelector('.rainUnit2').textContent=rainUnits+'/h';document.querySelector('.rainUnit3').textContent=rainUnits;
 $('rainNote').className=rainCalibrated?'caption':'warn';
 $('rainNote').textContent=rainCalibrated?mmPerTip===RAIN_DEFAULT?`Rain calibration: ${mmPerTip} mm per tip (initial: 2 ml per tip ÷ 49.5 × 109.5 mm funnel).`:`Rain calibration: ${mmPerTip} mm per tip (set in this browser).`:`UNCALIBRATED: provisional ${mmPerTip} mm per tip. Set the gauge's mm per tip.`;
}
async function loadRain(){
 const gen=++rainGen, days=rainDayList();
 try{const data=await get(`/api/rain?start=${days[0]}&end=${nextMidnight(days[days.length-1])}`);if(gen!==rainGen)return;
  rainRows=data.tips;rainFetched=Date.now();drawRain();}catch(e){if(gen===rainGen)$('rainScale').textContent=`Cannot load rain: ${e.message}`;}
}
// Local midnights of the days shown, oldest first; the last is the selected day (today by default).
function rainDayList(){
 const last=rainDay??midnight(clock()), list=[last];
 while(list.length<rainDays)list.unshift(midnight(list[0]-43200));
 return list;
}
// Start of the "since" total: a fixed mark, or a rolling/calendar preset (local time).
const SINCE_LABELS={h1:'Last 1 hour',h24:'Last 24 hours',h72:'Last 72 hours',d7:'Last 7 days',month:'Month to date',year:'Year to date',wy:'Water year (from Oct 1)'};
function sinceStart(now){
 if(since.kind==='fixed')return since.t;
 if(since.kind!=='preset')return null;
 const d=new Date(now*1000), k=since.key;
 if(k==='h1')return now-3600; if(k==='h24')return now-86400; if(k==='h72')return now-3*86400; if(k==='d7')return now-7*86400;
 if(k==='month')return new Date(d.getFullYear(),d.getMonth(),1).getTime()/1000;
 if(k==='year')return new Date(d.getFullYear(),0,1).getTime()/1000;
 if(k==='wy')return new Date(d.getMonth()>=9?d.getFullYear():d.getFullYear()-1,9,1).getTime()/1000;
 return null;
}
async function loadSince(){
 const gen=++sinceGen, now=clock(), start=sinceStart(now);
 if(start==null||start>=now){sinceData=null;paintSince();return;}
 try{const d=await get(`/api/rain/total?start=${start}&end=${now}`);if(gen!==sinceGen)return;sinceData=d;paintSince();}
 catch(e){if(gen===sinceGen){$('sinceInfo').textContent=`Cannot load total: ${e.message}`;}}
}
function paintSince(){
 $('sincePreset').value=since.kind==='preset'?since.key:since.kind;
 if(since.kind==='none'){$('sinceTitle').textContent='Since mark';$('sinceTotal').textContent='—';
  $('sinceInfo').textContent='No mark set: press Mark now, enter a date and time, or pick a preset';$('sinceWarn').textContent='';return;}
 const start=sinceStart(clock());
 $('sinceTitle').textContent=since.kind==='fixed'?`Since ${new Date(start*1000).toLocaleString([],{weekday:'short',month:'short',day:'numeric',hour:'2-digit',minute:'2-digit',hour12:false})}`:SINCE_LABELS[since.key];
 const d=sinceData;
 if(!d){$('sinceTotal').textContent='—';$('sinceInfo').textContent=start>=clock()?'Mark is in the future':'Loading…';$('sinceWarn').textContent='';return;}
 const total=d.tips*mmPerTip, hours=(d.end-d.start)/3600;
 $('sinceTotal').textContent=rainPad(total,4);
 $('sinceInfo').textContent=`${d.tips} tip${d.tips===1?'':'s'} · ${durText(d.end-d.start)} · average ${rainFmt(total/Math.max(hours,1e-9))} ${rainUnits}/h`;
 const warn=[], dt=t=>new Date(t*1000).toLocaleString([],{month:'short',day:'numeric',hour:'2-digit',minute:'2-digit',hour12:false});
 if(d.rain_logging_since==null||d.rain_logging_since>d.start)warn.push(d.rain_logging_since==null?'no rain logging yet':`rain logging began ${dt(d.rain_logging_since)}; nothing before that is counted`);
 if(d.restarts.length)warn.push(`sensor restarted ${d.restarts.length}× (${d.restarts.slice(0,3).map(dt).join(', ')}${d.restarts.length>3?', …':''}): tips not yet reported before a reset are lost`);
 if(d.maintenance?.length)warn.push(`${d.maintenance.length} maintenance window${d.maintenance.length>1?'s':''} excluded (${d.maintenance.slice(0,3).map(w=>`${dt(w.start)}–${w.end==null?'now':dt(w.end)}`).join(', ')}${d.maintenance.length>3?', …':''})`);
 if(d.faults?.length)warn.push(`${d.faults.length} rain gauge fault${d.faults.length>1?'s':''} excluded (${d.faults.slice(0,3).map(f=>`${dt(f.start)}–${f.end==null?'now':dt(f.end)}`).join(', ')}${d.faults.length>3?', …':''})`);
 if(d.gaps.length)warn.push(`no data ${d.gaps.slice(0,3).map(g=>`${dt(g.start)} for ${durText(g.end-g.start)}`).join(', ')}${d.gaps.length>3?', …':''}: tips are recovered unless the sensor restarted`);
 $('sinceWarn').textContent=warn.length?'⚠ '+warn.join(' · '):'';
}
function setSince(next){since=next;sinceData=null;saveRain();paintSince();loadSince();}
function paintRain(){
 const p=state?.latest, now=clock();
 if(!p||p.rain_tips==null){$('rainRate').textContent='—';$('rainLast').textContent='—';
  $('rainGauge').textContent='No rain data yet (needs sensor firmware W5)';$('rainRateNote').textContent='';return;}
 // Rate = calibration / max(last tip interval, time since last tip): decays during dry spells.
 const age=p.rain_at==null?null:Math.max(0,now-p.rain_at);
 let rate=null,note='needs two tips since sensor start';
 if(age!=null&&age>3600){rate=0;note='no tip for over 1 h';}
 else if(p.rain_interval_s){rate=mmPerTip*3600/Math.max(p.rain_interval_s,age);note=age>p.rain_interval_s?'decaying: no tip since; upper bound':'from the last two tips';}
 if(inMaint(p.rain_at))note+=' · last tip during maintenance: not in totals';
 if(inFault('rain',p.rain_at))note+=' · last tip during a gauge fault: not in totals';
 if(activeFault('rain'))note='RAIN GAUGE FAULT · '+note;
 $('rainRate').textContent=rate==null?'—':rainPad(rate,3);$('rainRateNote').textContent=note;
 $('rainLast').textContent=p.rain_at==null?'none since start':`${timeText(p.rain_at)} · ${age<120?age.toFixed(0)+' s':age<7200?(age/60).toFixed(0)+' min':(age/3600).toFixed(1)+' h'} ago`;
 $('rainGauge').textContent=`Gauge D10 ${p.rain_pin?'open':'closed'} · ${p.rain_tips} tips since sensor start · raw D10 edges ${p.rain_raw_edges}`;
}
function drawRain(){
 const days=rainDayList(), first=days[0], last=days[days.length-1], end=nextMidnight(last), span=end-first, today=rainDay==null;
 const width=rainCanvas.clientWidth, height=200, left=60, right=16, top=26, plotH=height-top-30, plotW=width-left-right;
 const ratio=window.devicePixelRatio||1;rainCanvas.style.height=height+'px';rainCanvas.width=Math.round(width*ratio);rainCanvas.height=Math.round(height*ratio);rctx.setTransform(ratio,0,0,ratio,0,0);
 const bounds=days.map((d,i)=>[d,i+1<days.length?days[i+1]:end]);
 const rows=rainRows.filter(r=>r.t>=first&&r.t<end), hours=new Array(Math.ceil(span/3600)).fill(0), dayTips=days.map(()=>0);
 for(const r of rows){hours[Math.min(hours.length-1,Math.floor((r.t-first)/3600))]+=r.added;dayTips[bounds.findIndex(([a,b])=>r.t>=a&&r.t<b)]+=r.added;}
 const peak=Math.max(...dayTips)*mmPerTip, scale=WindMath.niceScale(Math.max(rainVal(peak),rainVal(mmPerTip))*1.1,4,false), ymax=scale.max;
 const X=t=>left+(t-first)/span*plotW, Y=mm=>top+plotH-rainVal(mm)/ymax*plotH;
 rctx.fillStyle='#101e2b';rctx.fillRect(0,0,width,height);rctx.font='11px system-ui';rctx.textBaseline='middle';
 for(const tick of scale.ticks){const y=Y(rainUnits==='in'?tick*25.4:tick);rctx.strokeStyle=tick===0?'#55788f':'#294052';rctx.lineWidth=tick===0?1.6:1;rctx.beginPath();rctx.moveTo(left,y);rctx.lineTo(width-right,y);rctx.stroke();rctx.lineWidth=1;
  rctx.fillStyle='#839caf';rctx.textAlign='right';rctx.fillText(WindMath.tickLabel(tick,scale.step),left-5,y);}
 // Hour ticks: 3 h for one day, 6 h for two, 12 h for more (6/12/24 h on narrow screens).
 const step=(rainDays===1?3:rainDays===2?6:12)*(width<550?2:1);
 bounds.forEach(([a,b],i)=>{
  for(let h=0;a+h*3600<b;h+=step){const t=a+h*3600,x=X(t);rctx.strokeStyle=h===0?'#4a6b84':'#223746';rctx.beginPath();rctx.moveTo(x,top);rctx.lineTo(x,top+plotH);rctx.stroke();
   if(h===0&&i===0)continue;rctx.fillStyle='#839caf';rctx.textAlign='center';rctx.fillText(h===0?new Date(t*1000).toLocaleDateString([],{month:'short',day:'numeric'}):new Date(t*1000).toLocaleTimeString([],{hour:'2-digit',minute:'2-digit',hour12:false}),x,top+plotH+16);}
  rctx.fillStyle='#b8cbd8';rctx.textAlign='left';
  rctx.fillText(`${new Date(a*1000).toLocaleDateString([],{weekday:'short',month:'short',day:'numeric'})} · ${rainFmt(dayTips[i]*mmPerTip)} ${rainUnits}`,X(a)+4,top-12,X(b)-X(a)-8);
 });
 rctx.fillStyle='#839caf';rctx.textAlign='left';rctx.fillText('00:00',X(first),top+plotH+16);rctx.textAlign='right';rctx.fillText('24:00',X(end),top+plotH+16);
 shadeMaint(rctx,first,end,X,top,plotH,'rain');
 rctx.fillStyle='rgba(120,169,198,.35)';
 hours.forEach((n,h)=>{if(!n)return;const x0=X(first+h*3600),x1=X(Math.min(end,first+(h+1)*3600)),y=Y(n*mmPerTip);rctx.fillRect(x0+1,y,Math.max(1,x1-x0-2),top+plotH-y);});
 // Each day's line starts from 0 at its midnight.
 const now=clock();rctx.strokeStyle='#6cb8ff';rctx.lineWidth=2;
 bounds.forEach(([a,b])=>{
  if(a>now)return;const stop=Math.min(b,now);let cum=0,y=Y(0);
  rctx.beginPath();rctx.moveTo(X(a),y);
  for(const r of rows){if(r.t<a||r.t>=b)continue;const nx=X(r.t);rctx.lineTo(nx,y);cum+=r.added*mmPerTip;y=Y(cum);rctx.lineTo(nx,y);}
  rctx.lineTo(X(stop),y);rctx.stroke();
 });
 rctx.lineWidth=1;
 if(now>=first&&now<end){const nx=X(now);rctx.strokeStyle='#e4b26c';rctx.setLineDash([3,4]);rctx.beginPath();rctx.moveTo(nx,top);rctx.lineTo(nx,top+plotH);rctx.stroke();rctx.setLineDash([]);}
 const lastTips=dayTips[dayTips.length-1];
 $('rainTotal').textContent=rainPad(lastTips*mmPerTip,3);
 $('rainTips').textContent=`${lastTips} tip${lastTips===1?'':'s'} × ${mmPerTip} mm${today?' · so far today':` · ${new Date(last*1000).toLocaleDateString([],{month:'short',day:'numeric'})}`}`;
 const dayName=t=>new Date(t*1000).toLocaleDateString([],{weekday:'short',month:'short',day:'numeric'});
 $('rainDayText').textContent=rainDays===1?(today?'today':dayName(last)):`${dayName(first)} – ${today?'today':dayName(last)}`;
 $('rainScale').textContent=`${rainDays} day${rainDays>1?'s':''} · local time · max hour ${rainFmt(Math.max(0,...hours)*mmPerTip)} ${rainUnits}`;
 $('rainToday').className=today?'active':'';
}
function rainTick(){
 if(!state)return;paintRain();
 const key=`${state.latest?.rain_tips}|${midnight(clock())}`;
 if(key!==rainKey||Date.now()-rainFetched>60000){rainKey=key;loadRain();loadSince();}else drawRain();
}
$('rainOlder').onclick=()=>{rainDay=midnight((rainDay??midnight(clock()))-43200);loadRain();};
$('rainNewer').onclick=()=>{const next=nextMidnight((rainDay??midnight(clock()))+43200);rainDay=next>=midnight(clock())?null:midnight(next+3600);loadRain();};
$('rainToday').onclick=()=>{rainDay=null;loadRain();};
$('rainDays').addEventListener('change',()=>{rainDays=Number($('rainDays').value);saveRain();loadRain();});
$('markNow').onclick=()=>setSince({kind:'fixed',t:clock()});
$('sinceSet').onclick=()=>{const v=$('sinceAt').value;if(!v){$('sinceAt').reportValidity();return;}
 const t=new Date(v).getTime()/1000;if(!Number.isFinite(t))return;setSince({kind:'fixed',t});};
$('sinceClear').onclick=()=>setSince({kind:'none'});
$('sincePreset').addEventListener('change',()=>{const v=$('sincePreset').value;
 if(v==='none')setSince({kind:'none'});else if(v==='fixed'){if(since.kind!=='fixed')setSince({kind:'fixed',t:clock()});}else setSince({kind:'preset',key:v});});
$('applyRain').onclick=()=>{if(!$('mmPerTip').checkValidity()){$('mmPerTip').reportValidity();return;}
 mmPerTip=Number($('mmPerTip').value);rainUnits=$('rainUnits').value;rainCalibrated=true;saveRain();rainLabels();paintRain();drawRain();paintSince();};
$('rainUnits').addEventListener('change',()=>{rainUnits=$('rainUnits').value;saveRain();rainLabels();paintRain();drawRain();paintSince();});
try{const r=JSON.parse(localStorage.getItem('rainSettings'));
 if(r){if(r.rainCalibrated===true&&Number.isFinite(r.mmPerTip)&&r.mmPerTip>0&&r.mmPerTip<=1000){mmPerTip=r.mmPerTip;rainCalibrated=true;}
  if(['mm','in'].includes(r.rainUnits))rainUnits=r.rainUnits;
  if([1,2,3,4].includes(r.rainDays))rainDays=r.rainDays;
  const sv=r.since;
  if(sv&&sv.kind==='fixed'&&Number.isFinite(sv.t))since={kind:'fixed',t:sv.t};
  else if(sv&&sv.kind==='preset'&&Object.hasOwn(SINCE_LABELS,sv.key))since={kind:'preset',key:sv.key};}}catch(e){}
$('mmPerTip').value=mmPerTip;$('rainUnits').value=rainUnits;$('rainDays').value=String(rainDays);rainLabels();paintSince();
new ResizeObserver(drawRain).observe(rainCanvas.parentElement);
// ---- Battery (W6+) ----
const batCanvas=$('batChart'), bctx=batCanvas.getContext('2d');
let batRange=86400, batPoints=[], batFetched=0, batGen=0;
try{const r=Number(localStorage.getItem('batRange'));if([21600,86400,259200,604800].includes(r))batRange=r;}catch(e){}
$('batRange').value=String(batRange);
$('batRange').addEventListener('change',()=>{batRange=Number($('batRange').value);try{localStorage.setItem('batRange',String(batRange));}catch(e){}loadBat();});
async function loadBat(){
 const gen=++batGen, now=clock();
 try{const d=await get(`/api/battery?start=${now-batRange}&end=${now}`);if(gen!==batGen)return;batPoints=d.points;batFetched=Date.now();drawBat();}
 catch(e){if(gen===batGen)$('batScale').textContent=`Cannot load battery: ${e.message}`;}
}
// Least-squares slope (mV/h) over the last 6 h; needs at least 1 h of readings.
function batSlope(now){
 const pts=batPoints.filter(p=>p.t0>=now-21600);
 if(pts.length<3||pts[pts.length-1].t0-pts[0].t0<3600)return null;
 const n=pts.length,mx=pts.reduce((a,p)=>a+p.t0,0)/n,my=pts.reduce((a,p)=>a+p.mv,0)/n;
 let sxy=0,sxx=0;for(const p of pts){sxy+=(p.t0-mx)*(p.mv-my);sxx+=(p.t0-mx)**2;}
 return sxx?sxy/sxx*3600:null;
}
function paintBat(){
 const p=state?.latest, now=clock();
 $('batV').textContent=p?.vbat_v==null?'—':pad(p.vbat_v,2,1);
 $('batNote').textContent=p?.vbat_v==null?'Needs sensor firmware W6':`at last packet, ${durText(Math.max(0,now-p.received))} ago`;
 const sl=batSlope(now);
 $('batSlope').textContent=sl==null?'—':(sl>=0?'+':'−')+pad(Math.abs(sl),1,3).trimStart();
 $('batSlopeNote').textContent=sl==null?'needs at least 1 h of readings':sl<0?`about ${durText(Math.max(0,(batPoints[batPoints.length-1].mv-3500)/-sl*3600))} to 3.5 V at this rate (rough)`:'not discharging (charging, or flat)';
}
function drawBat(){
 const width=batCanvas.clientWidth, height=170, left=60, right=16, top=12, plotH=height-top-30, plotW=width-left-right;
 const ratio=window.devicePixelRatio||1;batCanvas.style.height=height+'px';batCanvas.width=Math.round(width*ratio);batCanvas.height=Math.round(height*ratio);bctx.setTransform(ratio,0,0,ratio,0,0);
 bctx.fillStyle='#101e2b';bctx.fillRect(0,0,width,height);bctx.font='11px system-ui';bctx.textBaseline='middle';
 const now=clock(), start=now-batRange, pts=batPoints;
 let lo=Infinity,hi=-Infinity;for(const p of pts){lo=Math.min(lo,p.lo);hi=Math.max(hi,p.hi);}
 if(!pts.length){lo=3500;hi=4200;}
 const vlo=lo/1000-0.02, vhi=hi/1000+0.02, span=vhi-vlo;
 const step=[0.01,0.02,0.05,0.1,0.2,0.5,1].find(s=>span/s<=5)||1;
 const X=t=>left+(t-start)/batRange*plotW, Y=v=>top+plotH-(v-vlo)/span*plotH;
 for(let v=Math.ceil(vlo/step)*step;v<=vhi+1e-9;v+=step){const y=Y(v);bctx.strokeStyle='#294052';bctx.beginPath();bctx.moveTo(left,y);bctx.lineTo(width-right,y);bctx.stroke();
  bctx.fillStyle='#839caf';bctx.textAlign='right';bctx.fillText(v.toFixed(step<0.1?2:1)+' V',left-5,y);}
 for(let j=0;j<=6;j++){const t=start+batRange*j/6,x=X(t);bctx.strokeStyle='#223746';bctx.beginPath();bctx.moveTo(x,top);bctx.lineTo(x,top+plotH);bctx.stroke();
  bctx.fillStyle='#839caf';bctx.textAlign=j===0?'left':j===6?'right':'center';
  bctx.fillText(batRange>86400?new Date(t*1000).toLocaleString([],{month:'short',day:'numeric',hour:'2-digit',hour12:false}):timeText(t).slice(0,5),x,top+plotH+16);}
 // Band and mean line, broken where buckets are missing (no packets).
 const gap=(a,b)=>b.t0-a.t0>3*Math.max(60,batRange/720);
 bctx.fillStyle='rgba(108,184,255,.18)';
 for(const p of pts){const x0=X(p.t0),x1=X(p.t0+Math.max(60,batRange/720));bctx.fillRect(x0,Y(p.hi/1000),Math.max(1,x1-x0),Math.max(1,Y(p.lo/1000)-Y(p.hi/1000)));}
 bctx.strokeStyle='#6cb8ff';bctx.lineWidth=1.6;bctx.beginPath();
 pts.forEach((p,i)=>{const x=X(p.t0),y=Y(p.mv/1000);if(i===0||gap(pts[i-1],p))bctx.moveTo(x,y);else bctx.lineTo(x,y);});
 bctx.stroke();bctx.lineWidth=1;
 $('batScale').textContent=pts.length?`${(lo/1000).toFixed(2)}–${(hi/1000).toFixed(2)} V over the last ${durText(batRange)}`:'No battery readings in this range yet (sensor firmware W6 needed)';
 paintBat();
}
new ResizeObserver(drawBat).observe(batCanvas.parentElement);
function batTick(){if(!state)return;paintBat();if(Date.now()-batFetched>60000)loadBat();}
$('maintBtn').onclick=async()=>{
 const active=!!state?.maintenance_active;let note='';
 if(!active){note=prompt('Start maintenance: data from now on is hidden from graphs and totals until you end it.\nOptional note (e.g. takedown, bench test):','');if(note===null)return;}
 else if(!confirm('End maintenance? Data from now on is shown and counted again.'))return;
 try{const r=await fetch('/api/maintenance',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action:active?'stop':'start',note}),signal:AbortSignal.timeout(8000)});
  if(!r.ok)throw Error(`HTTP ${r.status}`);updateState(Object.assign(await r.json(),{cursor}));load();rainKey=null;rainTick();}
 catch(e){$('error').textContent=`Cannot change maintenance: ${e.message}`;}
};
// ---- Pressure and temperature (W7+, BMP180) ----
// Defaults: station elevation 938 ft; no correction needed (Nov 6: sensor matched KARB's altimeter setting to 0.01 hPa).
const ALT_DEFAULT=285.9, POFF_DEFAULT=0;
let wxAir=[];
let wxRange=259200, wxPoints=[], wxFetched=0, wxGen=0, altitude=ALT_DEFAULT, pOffset=POFF_DEFAULT, tUnits='C';
try{const w=JSON.parse(localStorage.getItem('wxSettings'));if(w){if([21600,86400,259200,604800,1209600,2592000].includes(w.range))wxRange=w.range;
 if(Number.isFinite(w.altitude)&&w.altitude>=-500&&w.altitude<=9000)altitude=w.altitude;if(Number.isFinite(w.pOffset)&&Math.abs(w.pOffset)<=20)pOffset=w.pOffset;if(['C','F'].includes(w.tUnits))tUnits=w.tUnits;}}catch(e){}
const saveWx=()=>{try{localStorage.setItem('wxSettings',JSON.stringify({range:wxRange,altitude,pOffset,tUnits}));}catch(e){}};
$('wxRange').value=String(wxRange);$('altitude').value=altitude||'';$('pOffset').value=pOffset;$('tUnits').value=tUnits;
const SL=()=>altitude?Math.pow(1-altitude/44330.77,-5.25588):1;
const stationHpa=pa=>pa==null?null:pa/100+pOffset;  // corrected station pressure
const hPa=pa=>pa==null?null:stationHpa(pa)*SL();
const tVal=c=>c==null?null:tUnits==='F'?c*9/5+32:c;
async function loadWx(){
 const gen=++wxGen, now=clock();
 try{const d=await get(`/api/weather?start=${now-wxRange}&end=${now}`);if(gen!==wxGen)return;wxPoints=d.points;wxAir=d.air||[];wxFetched=Date.now();drawWx();}
 catch(e){if(gen===wxGen)$('wxScale').textContent=`Cannot load pressure: ${e.message}`;}
}
// Pressure 3 h ago, from the stored minute rows' buckets (needs history reaching back 3 h).
function pressureAgo(now,sec){
 let best=null;for(const p of wxPoints){const d=Math.abs(p.t0-(now-sec));if(d<=Math.max(900,wxRange/720)&&(!best||d<best.d))best={d,p};}
 return best?.p.p??null;
}
function paintWx(){
 const p=state?.latest, now=clock(), have=p?.pressure_pa!=null;
 $('pTitle').textContent=altitude?`Sea-level pressure (${altitude} m)`:'Station pressure';
 $('tUnit').textContent=tUnits==='F'?'°F':'°C';
 $('pNow').textContent=have?pad(hPa(p.pressure_pa),1,4):'—';
 $('tNow').textContent=have?pad(tVal(p.temp_c),1,3):'—';
 $('pNote').textContent=have?`at last packet, ${durText(Math.max(0,now-p.received))} ago${altitude?` · station ${stationHpa(p.pressure_pa).toFixed(1)} hPa`:''} · = ${(hPa(p.pressure_pa)/33.8639).toFixed(2)} inHg${pOffset?` · correction ${pOffset>0?'+':''}${pOffset} hPa`:''}`:p&&p.version>='W7'?'No reading: BMP180 not answering':'Needs sensor firmware W7 and a BMP180';
 if(activeFault('baro'))$('pNote').textContent='BAROMETER FAULT: not trusted · '+$('pNote').textContent;
 const air=p?.air_temp_c!=null;
 $('aUnit').textContent=tUnits==='F'?'°F':'°C';
 $('aNow').textContent=air?pad(tVal(p.air_temp_c),1,3):'—';
 $('hNow').textContent=air?pad(p.rh_pct,1,3):'—';
 $('aNote').textContent=air?`SHT31 at last packet, ${durText(Math.max(0,now-p.received))} ago`:p&&p.version>='W9'?'No reading: SHT31 not answering':'Needs sensor firmware W9 and an SHT31';
 if(activeFault('air'))$('aNote').textContent='AIR SENSOR FAULT: not trusted · '+$('aNote').textContent;
 const ago=pressureAgo(now,10800);
 if(have&&ago!=null){const d=hPa(p.pressure_pa)-hPa(ago);
  $('pTrend').textContent=(d>=0?'+':'−')+pad(Math.abs(d),1,2).trimStart();
  $('pTrendNote').textContent=Math.abs(d)<0.1?'steady':Math.abs(d)<1.5?(d>0?'rising slowly':'falling slowly'):Math.abs(d)<3.5?(d>0?'rising':'falling'):(d>0?'rising quickly':'falling quickly');}
 else{$('pTrend').textContent='—';$('pTrendNote').textContent='needs 3 h of readings';}
}
// One band-and-line chart: pts {t0, mean, lo, hi}; values already in display units.
function drawBand(cv,pts,start,span,fmt,label,color){
 const c=cv.getContext('2d'), width=cv.clientWidth, height=150, left=60, right=16, top=12, plotH=height-top-30, plotW=width-left-right;
 const ratio=window.devicePixelRatio||1;cv.style.height=height+'px';cv.width=Math.round(width*ratio);cv.height=Math.round(height*ratio);c.setTransform(ratio,0,0,ratio,0,0);
 c.fillStyle='#101e2b';c.fillRect(0,0,width,height);c.font='11px system-ui';c.textBaseline='middle';
 let lo=Infinity,hi=-Infinity;for(const p of pts){lo=Math.min(lo,p.lo);hi=Math.max(hi,p.hi);}
 if(!pts.length){lo=0;hi=1;}
 // Padded range on round steps (at least 0.5 units tall, so sensor noise doesn't fill the chart).
 const pad=Math.max(0.25,(hi-lo)*0.1), step=WindMath.niceScale(hi-lo+2*pad,4,false).step;
 const vlo=Math.floor((lo-pad)/step)*step, vhi=Math.ceil((hi+pad)/step)*step;
 const X=t=>left+(t-start)/span*plotW, Y=v=>top+plotH-(v-vlo)/(vhi-vlo)*plotH;
 for(let v=vlo;v<=vhi+step*1e-6;v+=step){const y=Y(v);c.strokeStyle='#294052';c.beginPath();c.moveTo(left,y);c.lineTo(width-right,y);c.stroke();
  c.fillStyle='#839caf';c.textAlign='right';c.fillText(WindMath.tickLabel(Number(v.toPrecision(12)),step),left-5,y);}
 for(let j=0;j<=6;j++){const t=start+span*j/6,x=X(t);c.strokeStyle='#223746';c.beginPath();c.moveTo(x,top);c.lineTo(x,top+plotH);c.stroke();
  c.fillStyle='#839caf';c.textAlign=j===0?'left':j===6?'right':'center';
  c.fillText(span>86400?new Date(t*1000).toLocaleString([],{month:'short',day:'numeric',hour:'2-digit',hour12:false}):timeText(t).slice(0,5),x,top+plotH+16);}
 shadeMaint(c,start,start+span,X,top,plotH,'baro');
 const w=Math.max(60,span/720), gap=(a,b)=>b.t0-a.t0>3*w;
 c.fillStyle=color+'2e';
 for(const p of pts){const x0=X(p.t0),x1=X(p.t0+w);c.fillRect(x0,Y(p.hi),Math.max(1,x1-x0),Math.max(1,Y(p.lo)-Y(p.hi)));}
 c.strokeStyle=color;c.lineWidth=1.6;c.beginPath();
 pts.forEach((p,i)=>{const x=X(p.t0+w/2),y=Y(p.mean);if(i===0||gap(pts[i-1],p))c.moveTo(x,y);else c.lineTo(x,y);});
 c.stroke();c.lineWidth=1;
 c.fillStyle='#b8cbd8';c.textAlign='left';c.fillText(label,left+6,top+8);
 return pts.length?`${fmt(lo)}–${fmt(hi)}`:null;
}
function drawWx(){
 const now=clock(), start=now-wxRange, pu=altitude?'hPa sea level':'hPa', tu=tUnits==='F'?'°F':'°C';
 const pr=drawBand($('pChart'),wxPoints.map(p=>({t0:p.t0,mean:hPa(p.p),lo:hPa(p.plo),hi:hPa(p.phi)})),start,wxRange,v=>v.toFixed(1),`Pressure, ${pu}`,'#b48cff');
 const tr=drawBand($('tChart'),wxPoints.map(p=>({t0:p.t0,mean:tVal(p.c),lo:tVal(p.clo),hi:tVal(p.chi)})),start,wxRange,v=>v.toFixed(1),`Enclosure temperature, ${tu}`,'#ff9f6c');
 drawBand($('aChart'),wxAir.map(p=>({t0:p.t0,mean:tVal(p.c),lo:tVal(p.clo),hi:tVal(p.chi)})),start,wxRange,v=>v.toFixed(1),`Air temperature (SHT31), ${tu}`,'#6cc4ff');
 drawBand($('hChart'),wxAir.map(p=>({t0:p.t0,mean:p.h,lo:p.hlo,hi:p.hhi})),start,wxRange,v=>v.toFixed(0),'Relative humidity, %','#5fd3a0');
 $('wxScale').textContent=pr?`Pressure ${pr} ${pu} · temperature ${tr} ${tu} · last ${durText(wxRange)}`:state?.maintenance_active&&state.latest?.pressure_pa!=null?'No readings outside maintenance in this range yet (readings during maintenance are hidden)':'No pressure readings in this range yet (sensor firmware W7 with a BMP180)';
 paintWx();
}
new ResizeObserver(drawWx).observe($('pChart').parentElement);
$('wxRange').addEventListener('change',()=>{wxRange=Number($('wxRange').value);saveWx();loadWx();});
$('applyWx').onclick=()=>{for(const id of ['altitude','pOffset'])if(!$(id).checkValidity()){$(id).reportValidity();return;}
 altitude=Number($('altitude').value)||0;pOffset=Number($('pOffset').value)||0;tUnits=$('tUnits').value;saveWx();drawWx();};
// Correction so that the latest reading, reduced at the station altitude, equals the given altimeter setting.
$('calFromAltimeter').onclick=()=>{
 const inHg=Number($('altimeter').value), pa=state?.latest?.pressure_pa;
 if(!$('altimeter').value||!$('altimeter').checkValidity()){$('altimeter').reportValidity();return;}
 if(pa==null){$('calResult').textContent='No current pressure reading';return;}
 if(!altitude){$('calResult').textContent='Set the station altitude first';return;}
 const expected=inHg*33.8639/SL(), off=Number((expected-pa/100).toFixed(2));
 if(Math.abs(off)>20){$('calResult').textContent=`Correction ${off} hPa is implausible; check the altitude and setting`;return;}
 pOffset=off;$('pOffset').value=off;saveWx();drawWx();
 $('calResult').textContent=`Correction set to ${off>0?'+':''}${off} hPa (expected station ${expected.toFixed(2)} hPa, sensor ${(pa/100).toFixed(2)} hPa)`;
};
function wxTick(){if(!state)return;paintWx();if(Date.now()-wxFetched>60000)loadWx();}
async function poll(){
 try{
  if(loaded){const gen=generation,data=await get(`/api/live?after=${cursor}`);
   if(gen===generation){updateState(data);rainTick();batTick();wxTick();
    if(live&&condensed()){if(Date.now()-lastLoad>30000)load();}  // condensed history: refresh, don't append raw points
    else if(live){const start=endTime()-rows*duration;points=points.concat(data.points).filter(p=>p.t>=start);}
    $('error').textContent='';draw();}
  }
 }catch(e){$('error').textContent=`Server connection lost: ${e.message}. Retrying…`;$('status').textContent='Web connection lost';$('status').className='status warn';}
 finally{setTimeout(poll,1000);}
}
$('duration').addEventListener('change',()=>{if(settings()){live=true;load();}});
$('rows').addEventListener('change',()=>{if(settings()){live=true;load();}});
$('preset').addEventListener('change',()=>{if($('preset').value!=='custom'){$('duration').value=$('preset').value;settings();live=true;load();}});
$('ymax').addEventListener('change',()=>{if($('ymax').value&&!$('ymax').checkValidity()){$('ymax').reportValidity();return;}saveSettings();draw();});
$('stripHeight').addEventListener('change',()=>{if(!$('stripHeight').value||!$('stripHeight').checkValidity()){$('stripHeight').reportValidity();return;}stripHeight=Number($('stripHeight').value);saveSettings();draw();});
$('units').addEventListener('change',()=>{units=$('units').value;$('ymax').value='';saveSettings();displayLabels();paintMetrics();draw();});
$('applyCalibration').onclick=()=>{
 for(const id of ['gain','calOffset']){if(!$(id).checkValidity()){$(id).reportValidity();return;}}
 gain=Number($('gain').value);calOffset=Number($('calOffset').value);calibrated=true;
 $('ymax').value='';saveSettings();displayLabels();paintMetrics();draw();
};
$('older').onclick=()=>{fixedEnd=endTime()-rows*duration;live=false;load();};
$('newer').onclick=()=>{fixedEnd=endTime()+rows*duration;live=fixedEnd>=alignedEnd(clock());load();};
$('live').onclick=()=>{live=true;load();};
canvas.addEventListener('pointermove',e=>{
 if(!geometry)return;const g=geometry,r=canvas.getBoundingClientRect(),x=e.clientX-r.left,y=e.clientY-r.top,row=Math.floor((y-g.top)/g.stripH);
 if(row<0||row>=rows||x<g.left){$('tooltip').style.display='none';return;}
 const rowStart=g.end-(row+1)*duration;
 const t=rowStart+(x-g.left)/g.plotWidth*duration;
 let best=null,dist=Infinity;for(const p of g.items){if(p.t<rowStart||p.t>=rowStart+duration)continue;const d=Math.abs(p.t-t);if(d<dist){best=p;dist=d;}}
 if(!best||dist>duration*12/g.plotWidth){$('tooltip').style.display='none';return;}
 $('tooltip').textContent=best.calm?`${dateText(best.t)}\nDeduced calm: 0 ${unitLabel()}\nNo reed closure for over ${CALM_S} s${best.provisional?' (provisional, live)':''}\nDisplay only; not a measurement`:`${dateText(best.t)}\n${valueOf(best.hz).toFixed(unitDec())} ${unitLabel()}${!calibrated&&!['hz','rpm'].includes(units)?' (uncalibrated)':''} · ${best.hz.toFixed(3)} Hz\nPeriod ${best.period.toFixed(2)} s\nA ${best.a.toFixed(3)} s · B ${best.b.toFixed(3)} s\nRevolution ${best.revs}`;
 $('tooltip').style.display='block';$('tooltip').style.left=Math.max(0,Math.min(e.clientX+12,innerWidth-280))+'px';$('tooltip').style.top=Math.max(0,Math.min(e.clientY+12,innerHeight-140))+'px';
});canvas.addEventListener('pointerleave',()=>$('tooltip').style.display='none');
try{
 const s=JSON.parse(localStorage.getItem('windSettings'));
 if(s){
  if(Number.isInteger(s.duration)&&s.duration>=10&&s.duration<=86400){duration=s.duration;$('duration').value=duration;}
  if([4,6,8,12].includes(s.rows)){rows=s.rows;$('rows').value=rows;}
  if(Number.isFinite(s.stripHeight)&&s.stripHeight>=100&&s.stripHeight<=400){stripHeight=s.stripHeight;$('stripHeight').value=stripHeight;}
  if(Object.hasOwn(WindMath.labels,s.units)){units=s.units;$('units').value=units;}
  if(s.calibrated===true&&Number.isFinite(s.gain)&&s.gain>=.000001&&s.gain<=1e6&&Number.isFinite(s.calOffset)&&Math.abs(s.calOffset)<=1e6){gain=s.gain;calOffset=s.calOffset;calibrated=true;}
  $('gain').value=gain;$('calOffset').value=calOffset;
  if(Number.isFinite(Number(s.ymax))&&Number(s.ymax)>0)$('ymax').value=s.ymax;
 }
}catch(e){}
settings();displayLabels();
new ResizeObserver(draw).observe(canvas.parentElement);
load().then(poll);
