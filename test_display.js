const assert=require('node:assert/strict');
const {convert,stats,niceScale,tickLabel,calmSeries,timeAverage}=require('./web/math.js');
const near=(a,b)=>assert.ok(Math.abs(a-b)<1e-10,`${a} != ${b}`);
near(convert(2,'knots',3,1),7);
near(convert(2,'mph',3,1),7*1852/1609.344);
near(convert(2,'ms',3,1),7*1852/3600);
near(convert(2,'hz',3,1),2);
near(convert(2,'rpm',3,1),120);
assert.equal(convert(.1,'knots',1,-1),0);
assert.equal(convert(null,'knots'),null);
assert.equal(stats([]),null);
assert.deepEqual(stats([1,2,6,NaN,null]),{min:1,max:6,avg:3,count:3});
assert.deepEqual(stats([0]),{min:0,max:0,avg:0,count:1});
for(const maximum of [0,.0002,.03,.3,.9,1,2.1,13,91,1234,1e7]){
 const scale=niceScale(maximum,4);
 assert.ok(scale.max>0 && scale.max>=maximum);
 const base=10**Math.floor(Math.log10(scale.step));
 assert.ok([1,2,2.5,5,10].some(n=>Math.abs(scale.step/base-n)<1e-9));
 assert.equal(new Set(scale.ticks.map(t=>tickLabel(t,scale.step))).size,scale.ticks.length);
}
assert.equal(niceScale(13,4,true).max,13);
assert.equal(tickLabel(.25,.25),'0.25');
// Deduced calm: 2 s revolutions, then a 300 s stall in half A, then 2 s revolutions.
const rev=(t,a,b)=>({t,a,b,period:a+b,hz:1/(a+b),stream:'s'});
const pts=[rev(100,1,1),rev(102,1,1),rev(403,300,1),rev(405,1,1)];
const {items,spans}=calmSeries(pts,20);
assert.deepEqual(items.map(i=>[i.t,i.calm]),[[100,false],[102,false],[122,true],[402,true],[405,false]]);
assert.ok(items.filter(i=>i.calm).every(i=>i.hz===0));
// Revolutions / time in Hz: 4 revolutions from 98 to 405 = 307 s.
near(timeAverage(spans,0,1000).avg,4/307);
near(timeAverage(spans,0,1000).calm,280);
near(timeAverage(spans,150,350).avg,0);                     // inside the calm
assert.equal(timeAverage(spans,500,600),null);              // no data: gap, not zero
// Calm counts as 0 even with a positive calibration offset.
near(timeAverage(spans,150,350,hz=>convert(hz,'knots',2,0.5)).avg,0);
// Stall in half B: zero runs to the revolution's completing closure.
const b=calmSeries([rev(100,1,1),rev(131,1,30)],20).items;
assert.deepEqual(b.map(i=>[i.t,i.calm]),[[100,false],[121,true],[131,true]]);
// Both halves long: two separate calms. Slow-but-under-T halves are real points.
assert.equal(calmSeries([rev(100,25,25)],20).items.length,4);
assert.equal(calmSeries([rev(100,19,19)],20).items.length,1);
// Line-join rule (page): consecutive items join if dt <= max(20, 2*max(periods)). A long
// calm must be one segment from its start to its end, and join its neighbours.
const joined=list=>list.slice(1).every((p,i)=>p.t-list[i].t<=Math.max(20,2*Math.max(p.period,list[i].period)));
assert.ok(joined(calmSeries([rev(100,1,1),rev(500,397,1),rev(502,1,1)],20).items));
assert.ok(joined(calmSeries([rev(100,1,1),rev(400,100,200),rev(402,1,1)],20).items));
console.log('PASS: calibration, all units, zero clamp, sample statistics, preferred ticks and labels, deduced calm, time-weighted average');
