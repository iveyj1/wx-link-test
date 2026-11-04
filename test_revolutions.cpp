// Revolution pairing in PulseCounter: reference edge, A/B halves, long gaps, 64-bit counts.
#include "link/PulseCounter.h"
#include <cassert>
#include <cstdio>
// One closure (high->low) at t, reopen 1000 ticks later; both debounced.
void closure(PulseCounter &p, uint64_t t) {
  p.edge(false,t); p.timer(false,t+328);
  p.edge(true,t+1000); p.timer(true,t+1328);
}
int main() {
  // Full-revolution reference: closure 1 is the reference, 2 is the middle, 3 completes.
  PulseCounter p; p.begin(true);
  closure(p,10000); assert(p.count==1 && p.revolutions==0 && p.revolutionPeriod==0);
  closure(p,20000); assert(p.count==2 && p.revolutions==0 && p.pendingA==10000);
  // Unequal spacing: A=10000, B=30000; the period is the full revolution, not a half.
  closure(p,50000); assert(p.revolutions==1 && p.intervalA==10000 && p.intervalB==30000);
  assert(p.revolutionPeriod==40000 && p.lastRevolution==50000);
  // Stable A/B pair: the same magnet spacing keeps A and B on the same labels.
  for (int i=0;i<5;++i) {
    uint64_t base=50000+uint64_t(i+1)*40000;
    closure(p,base-30000); closure(p,base);
    assert(p.intervalA==10000 && p.intervalB==30000 && p.revolutionPeriod==40000);
  }
  assert(p.revolutions==6 && p.count==13);
  // Long gap: hours between closures, across many 32-bit RTC wraps, still one revolution.
  uint64_t far=(uint64_t(1)<<40);
  closure(p,far); closure(p,far+40000);
  // Previous revolution completed at 250000; the long half is A, B stays 40000.
  assert(p.revolutions==7 && p.intervalA==far-250000 && p.intervalB==40000);
  assert(p.revolutionPeriod==(far+40000)-250000);
  // Phase swap: a sensor restart can take the other magnet as reference, swapping labels.
  PulseCounter q; q.begin(true);
  closure(q,0); closure(q,30000); closure(q,40000);
  assert(q.intervalA==30000 && q.intervalB==10000 && q.revolutionPeriod==40000);
  // 64-bit revolutions: counts past 2^32 keep incrementing.
  q.count=(uint64_t(1)<<33)+1; q.revolutions=uint64_t(1)<<32;
  closure(q,far); closure(q,far+20000);
  assert(q.revolutions==(uint64_t(1)<<32)+1);
  puts("PASS: full-revolution reference, unequal spacing, stable A/B pair, long gaps, phase swap, 64-bit revolutions");
}
