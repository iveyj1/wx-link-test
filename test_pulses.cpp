#include "link/PulseCounter.h"
#include <cassert>
#include <cstdio>
void level(PulseCounter &p, bool high, uint64_t t) {
  p.edge(high,t); p.timer(high,t+328);
}
int main() {
  PulseCounter p; p.begin(true);
  p.edge(false,100); p.timer(false,427); assert(p.count==0);
  p.edge(true,200); p.timer(true,528); assert(p.count==0);
  p.edge(false,600); p.edge(true,650); p.edge(false,700);
  p.timer(false,1027); assert(p.count==0);
  p.timer(false,1028); assert(p.count==1 && p.last==700 && p.period==0);
  p.timer(false,5000); assert(p.count==1); // held closed
  level(p,true,6000);
  level(p,false,700+327680); assert(p.count==2 && p.period==327680);
  level(p,true,400000);
  uint64_t longTime = (uint64_t(1)<<40) + 123;
  level(p,false,longTime); assert(p.count==3 && p.last==longTime);
  assert(p.period==longTime-(700+327680)); // many RTC wraps, no timeout
  PulseCounter fast; fast.begin(true);
  for (uint64_t i=0;i<10000;++i) {
    level(fast,false,1000+i*3277);
    level(fast,true,2500+i*3277);
  }
  assert(fast.count==10000 && fast.period==3277); // ~0.1 s
  PulseCounter bootClosed; bootClosed.begin(false);
  bootClosed.timer(false,1000000); assert(bootClosed.count==0);
  level(bootClosed,true,1000100); level(bootClosed,false,2000000);
  assert(bootClosed.count==1);
  fast.count=UINT32_MAX;
  level(fast,false,uint64_t(1)<<42);
  assert(fast.count==uint64_t(UINT32_MAX)+1);
  puts("PASS: bounce, 10ms boundary, held switch, 0.1s/10s periods, multi-wrap gaps, boot closed, 64-bit count");
}
