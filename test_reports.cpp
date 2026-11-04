#include "link/ReportSchedule.h"
#include "link/PulseCounter.h"
#include <cassert>
#include <cstdio>
int main() {
  const uint64_t hz = ReportSchedule::hz;
  ReportSchedule r;
  assert(r.due(0,0));
  r.finished(100,0,true);
  assert(!r.due(100+hz-1,1));
  assert(r.due(100+hz,1));
  assert(!r.due(100+59*hz,0));
  assert(r.due(100+60*hz,0));
  // Only a complete pair triggers, never the reference or the middle edge.
  PulseCounter p; p.begin(true);
  for (uint64_t i=1; i<=3; ++i) {
    p.edge(false,i*hz);
    p.timer(false,i*hz+327);
    assert(!r.due(i*hz+327,p.revolutions));
    p.timer(false,i*hz+328);
    assert(r.due(i*hz+328,p.revolutions) == (i==3));
    p.edge(true,i*hz+1000); p.timer(true,i*hz+1328);
  }
  assert(p.count==3 && p.revolutions==1 && p.revolutionPeriod==2*hz);
  r.finished(4*hz,1,true);
  // Coalesce even a final pulse that arrives during a send/cooldown.
  assert(!r.due(4*hz+100,2));
  assert(r.deadline(2)==5*hz);
  assert(r.due(5*hz,2));
  r.finished(5*hz,2,true);
  assert(r.deadline(2)==65*hz);
  // Failed transmission retains previous count and backs off.
  r.finished(6*hz,7,false);
  assert(r.sentCount==2 && !r.due(10*hz,7));
  assert(r.due(11*hz,7));
  // Rapid pulses: every snapshot is cumulative; no send faster than 1 s.
  uint64_t last=0, sends=0;
  for(uint64_t i=1;i<=1000;++i) {
    uint64_t now=20*hz+i*(hz/10);
    if(r.due(now,i)) {
      assert(now-last>=hz);
      r.finished(now,i,true); last=now; ++sends;
    }
  }
  assert(sends>80 && sends<=100);
  assert(r.due(last+hz,1000));
  r.finished(last+hz,1000,true);
  assert(r.sentCount==1000);
  // A failed forced/heartbeat report retries even when the count is unchanged.
  r.finished(last+2*hz,1000,false);
  assert(!r.due(last+6*hz,1000));
  assert(r.due(last+7*hz,1000));
  r.finished(last+7*hz,1000,true);
  assert(!r.retryPending && r.deadline(1000)==last+67*hz);
  puts("PASS: completed-revolution trigger, 1s rate limit, coalescing, in-flight pulses, 60s heartbeat, failure backoff");
}
