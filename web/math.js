/* Pure display mathematics; raw history always remains in revolution Hz. */
(function(root) {
  const labels = {knots:'kn', mph:'mph', ms:'m/s', hz:'Hz', rpm:'RPM'};
  function convert(hz, unit, gain=1, offset=0) {
    if (hz == null || !Number.isFinite(hz)) return null;
    if (unit === 'hz') return hz;
    if (unit === 'rpm') return hz * 60;
    const knots = Math.max(0, gain * hz + offset);
    if (unit === 'knots') return knots;
    if (unit === 'mph') return knots * 1852 / 1609.344;
    if (unit === 'ms') return knots * 1852 / 3600;
    throw Error('Unknown unit');
  }
  function stats(values) {
    const finite = values.filter(Number.isFinite);
    if (!finite.length) return null;
    let min=Infinity, max=-Infinity, sum=0;
    for (const v of finite) { min=Math.min(min,v); max=Math.max(max,v); sum+=v; }
    return {min, max, avg:sum/finite.length, count:finite.length};
  }
  function niceScale(maximum, intervals=4, fixed=false) {
    maximum = Number.isFinite(maximum) && maximum>0 ? maximum : 1;
    const raw=maximum/Math.max(1,intervals), power=10**Math.floor(Math.log10(raw));
    const factor=[1,2,2.5,5,10].find(v=>v*power>=raw*(1-1e-12));
    const step=factor*power;
    const rounded=Number((Math.ceil(maximum/step-1e-12)*step).toPrecision(12));
    const max=fixed?maximum:Math.max(maximum,rounded);
    const ticks=[];
    for(let i=0;i<=Math.floor(max/step+1e-10);i++) ticks.push(Number((i*step).toPrecision(12)));
    return {max,step,ticks};
  }
  function tickLabel(value, step) {
    if (value===0) return '0';
    if (Math.abs(value)>=1e6 || Math.abs(value)<1e-4) return value.toExponential(1);
    const decimals=Math.max(0,Math.min(8,-Math.floor(Math.log10(step))+1));
    return Number(value.toFixed(decimals)).toString();
  }
  // Deduced calm (display only; stored data stays as measured). A half-revolution longer
  // than T seconds means no closure for T: average speed < 0.5/T rev/s, below resolution,
  // drawn as 0 from (half start + T) to the closure that ended it. The calm-spanning
  // revolution's own near-zero average is not drawn. Points need t (completion), a, b,
  // period, hz, stream; sorted by t. Returns drawing items and time spans for averages.
  function calmSeries(points, T=20) {
    const items=[], spans=[];
    for (const p of points) {
      const start=p.t-p.a-p.b;
      const calms=[[start,p.t-p.b],[p.t-p.b,p.t]].filter(([s,e])=>e-s>T).map(([s,e])=>[s+T,e]);
      if (!calms.length) {
        items.push(Object.assign({},p,{calm:false}));
        spans.push({start,end:p.t,hz:p.hz,calm:false});
        continue;
      }
      // One revolution happened during the time not declared calm.
      const moving=p.period-calms.reduce((n,[s,e])=>n+e-s,0);
      let cursor=start;
      for (const [s,e] of calms) {
        if (s>cursor) spans.push({start:cursor,end:s,hz:1/moving,calm:false});
        spans.push({start:s,end:e,hz:0,calm:true});
        // period drives the page's line-join rule: the end's period = calm length, so a calm
        // is always one continuous segment however long it lasts.
        items.push({t:s,hz:0,calm:true,stream:p.stream,period:T,source:p},
                   {t:e,hz:0,calm:true,stream:p.stream,period:Math.max(T,e-s),source:p});
        cursor=e;
      }
      if (p.t>cursor) spans.push({start:cursor,end:p.t,hz:1/moving,calm:false});
    }
    return {items,spans};
  }
  // Time-weighted strip average over [from,to): revolutions / time in Hz; calm counts as 0
  // (not value(0), which a positive calibration offset would lift). Gaps are excluded.
  function timeAverage(spans, from, to, value=v=>v) {
    let covered=0, sum=0, calm=0;
    for (const s of spans) {
      const o=Math.min(s.end,to)-Math.max(s.start,from);
      if (o<=0) continue;
      sum+=(s.calm?0:value(s.hz))*o; covered+=o; if (s.calm) calm+=o;
    }
    return covered?{avg:sum/covered,covered,calm}:null;
  }
  const api={labels,convert,stats,niceScale,tickLabel,calmSeries,timeAverage};
  if(typeof module!=='undefined' && module.exports) module.exports=api;
  else root.WindMath=api;
})(typeof globalThis!=='undefined'?globalThis:this);
