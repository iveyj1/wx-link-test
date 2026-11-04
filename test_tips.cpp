#include "link/TipCounter.h"
#include "link/ReportSchedule.h"
#include <cassert>
#include <cstdio>
void level(TipCounter &t, bool high, uint64_t at) { t.edge(high,at); t.timer(high,at+328); }
// One tip: ~100 ms closure (3277 ticks), then reopen.
void tip(TipCounter &t, uint64_t at) { level(t,false,at); level(t,true,at+3277); }
int main() {
  TipCounter t; t.begin(true);
  // Bounce shorter than 10 ms, and a bounce that returns to the old level, count nothing.
  t.edge(false,100); t.timer(false,427); assert(t.tips==0);
  t.edge(true,200); t.timer(true,528); assert(t.tips==0);
  // Closure counts; reopening does not. First tip has no interval.
  level(t,false,1000); assert(t.tips==1 && t.interval==0 && t.last==1000);
  level(t,true,1000+3277); assert(t.tips==1);
  tip(t,1000+32768); assert(t.tips==2 && t.interval==32768);
  // Chatter on closing and on reopening counts once.
  t.edge(false,100000); t.edge(true,100050); t.edge(false,100100);
  t.timer(false,100427); assert(t.tips==2);
  t.timer(false,100428); assert(t.tips==3 && t.last==100100 && t.interval==100100-(1000+32768));
  t.edge(true,103400); t.edge(false,103420); t.edge(true,103460); t.timer(true,103788);
  assert(t.tips==3 && t.stable);
  t.timer(true,999999); assert(t.tips==3);
  // Boot while closed: the reopening is not a tip; the next closure is.
  TipCounter b; b.begin(false);
  level(b,true,5000); assert(b.tips==0);
  tip(b,9000); assert(b.tips==1);
  // Hours between tips across RTC wraps.
  uint64_t later = (uint64_t(1)<<40) + 5;
  tip(t,later); assert(t.tips==4 && t.interval==later-100100);
  // Reports trigger on a tip even when no revolution happened (key = revolutions + tips).
  ReportSchedule r; r.finished(0,3,true);
  assert(!r.due(ReportSchedule::hz,3));
  assert(r.due(ReportSchedule::hz,4));
  puts("PASS: rain tip per ~100ms closure, reopen ignored, debounce, chatter, boot closed, interval, multi-wrap, report trigger");
}
