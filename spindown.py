#!/usr/bin/env python3
"""Spin-down analysis: split hand spins out of the revolution log and fit bearing friction.

Spin the rotor by hand, let it stop completely, wait ~20 s, repeat. Each completed
revolution with period <= --max-period becomes a speed point (f = 1/period, at the middle
of that revolution). Spins starting below --min-start (1.5 rev/s) are ignored as wind. A spin ends at a stop (a revolution longer than --max-period, or no
revolution for --gap s) or at a re-spin (speed jumps up).

Model: df/dt = -a - b*f^2 (f in rev/s). a = constant (Coulomb) friction deceleration in
rev/s^2, mostly the bearing; b = quadratic drag in 1/rev, mostly air on the cups. Because
revolutions are counted exactly, the fit uses d(f^2)/dn = -2a - 2b*f^2 (n = revolutions),
a straight line in f^2: no time derivatives needed. Lower a = freer bearing.

Spins start at different speeds, so compare a, b and the fixed-band metrics
(coast from --hi to --lo Hz, revolutions from --hi Hz to the stop), not total revolutions.

  spindown.py --since "2026-11-09 10:00" [--until ...] [--label "608ZZ stock"] [--csv points.csv]
Times are local ("HH:MM", "YYYY-MM-DD HH:MM[:SS]", or "now"); data comes from the dashboard's
database, including samples hidden by a maintenance window.
"""
import argparse
import csv
import math
import sqlite3
import statistics
import sys
from pathlib import Path

from maint import DB, show, when


def load(db, start, end):
    rows = db.execute('SELECT t, stream, period, revs, pulses FROM samples WHERE t >= ? AND t < ? ORDER BY t',
                      (start, end)).fetchall()
    return [dict(t=t, stream=s, period=p, revs=int(r), pulses=int(q)) for t, s, p, r, q in rows]


def split(samples, max_period, gap, jump):
    """Lists of consecutive revolutions belonging to one spin."""
    spins, cur = [], []
    for s in samples:
        boundary = (not cur or s['stream'] != cur[-1]['stream'] or s['period'] > max_period
                    or s['t'] - cur[-1]['t'] > gap or s['revs'] <= cur[-1]['revs']
                    or 1 / s['period'] > jump * (1 / cur[-1]['period']) + 0.2)
        if boundary:
            if len(cur) >= 3:
                spins.append(cur)
            cur = []
        if s['period'] <= max_period:
            cur.append(s)
    if len(cur) >= 3:
        spins.append(cur)
    return spins


def linfit(xs, ys, ws):
    sw = sum(ws); mx = sum(w * x for w, x in zip(ws, xs)) / sw; my = sum(w * y for w, y in zip(ws, ys)) / sw
    sxx = sum(w * (x - mx) ** 2 for w, x in zip(ws, xs))
    if sxx <= 0:
        return my, 0.0, float('nan')
    slope = sum(w * (x - mx) * (y - my) for w, x, y in zip(ws, xs, ys)) / sxx
    icpt = my - slope * mx
    syy = sum(w * (y - my) ** 2 for w, y in zip(ws, ys))
    res = sum(w * (y - icpt - slope * x) ** 2 for w, x, y in zip(ws, xs, ys))
    return icpt, slope, 1 - res / syy if syy > 0 else float('nan')


def crossing(points, level):
    """(time, revolution index) where speed first falls through level, interpolated."""
    for (t1, n1, f1), (t2, n2, f2) in zip(points, points[1:]):
        if f1 >= level > f2:
            k = (f1 - level) / (f1 - f2)
            return t1 + k * (t2 - t1), n1 + k * (n2 - n1)
    return None


def analyse(spin, db, hi, lo, until):
    # Speed point: average over the revolution that just completed, placed at its middle.
    pts = [(s['t'] - s['period'] / 2, s['revs'] - 0.5, 1 / s['period']) for s in spin]
    xs, ys, ws = [], [], []
    for (_, n1, f1), (_, n2, f2) in zip(pts, pts[1:]):
        dn = n2 - n1
        xs.append((f1 * f1 + f2 * f2) / 2); ys.append((f2 * f2 - f1 * f1) / dn); ws.append(dn)
    c0, c1, r2 = linfit(xs, ys, ws)
    first, last = spin[0], spin[-1]
    start = first['t'] - first['period']
    # Half-turns after the last full revolution, up to the next spin: rotor coasting past
    # one more closure, or rocking on the magnet as it stops.
    extra = db.execute('SELECT MAX(CAST(pulses AS INTEGER)) FROM packets WHERE session = ? AND t > ? AND t < ? '
                       'AND CAST(revs AS INTEGER) = ?',
                       (first['stream'].split('-')[0], last['t'], until, last['revs'])).fetchone()[0]
    extra = max(0, (extra or last['pulses']) - last['pulses'])
    up, down = crossing(pts, hi), crossing(pts, lo)
    return dict(start=start, f0=pts[0][2], f_end=pts[-1][2], revs=last['revs'] - first['revs'] + 1,
                extra_half=extra, duration=last['t'] - start, a=-c0 / 2, b=-c1 / 2, r2=r2, n=len(pts),
                coast=down[0] - up[0] if up and down else None,
                revs_hi=last['revs'] - up[1] + extra / 2 if up else None, points=pts)


def fmt(v, spec):
    return '—' if v is None or (isinstance(v, float) and math.isnan(v)) else format(v, spec)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--db', type=Path, default=DB)
    p.add_argument('--since', type=when, required=True)
    p.add_argument('--until', type=when, default=when('now'))
    p.add_argument('--label', default='')
    p.add_argument('--hi', type=float, default=3.0, help='upper speed for the fixed band, rev/s (default 3)')
    p.add_argument('--lo', type=float, default=1.0, help='lower speed for the fixed band, rev/s (default 1)')
    p.add_argument('--max-period', type=float, default=10.0, help='longer revolutions count as stopped (s)')
    p.add_argument('--gap', type=float, default=20.0, help='no revolution for this long ends a spin (s)')
    p.add_argument('--min-start', type=float, default=1.5, help='ignore spins starting slower (wind), rev/s (default 1.5)')
    p.add_argument('--csv', type=Path, help='write every speed point here')
    args = p.parse_args(argv)
    db = sqlite3.connect(f'file:{args.db}?mode=ro', uri=True)
    spins = [s for s in split(load(db, args.since, args.until), args.max_period, args.gap, jump=1.3)
             if 1 / s[0]['period'] >= args.min_start]
    if not spins:
        sys.exit(f'No spins between {show(args.since)} and {show(args.until)}.')
    results = []
    for i, spin in enumerate(spins):
        nxt = spins[i + 1][0]['t'] - spins[i + 1][0]['period'] if i + 1 < len(spins) else args.until
        results.append(analyse(spin, db, args.hi, args.lo, nxt))
    title = f' · {args.label}' if args.label else ''
    print(f'Spins {show(args.since)} → {show(args.until)}{title}')
    print(f'  #  start     f0 Hz  revs(+½) dur s   a rev/s²  b 1/rev   R²   pts  {args.hi:g}→{args.lo:g} Hz s  revs {args.hi:g} Hz→stop')
    for i, r in enumerate(results, 1):
        print(f'{i:3}  {show(r["start"])[11:]}  {r["f0"]:5.2f}  {r["revs"]:5}+{r["extra_half"]}   {r["duration"]:6.1f}  '
              f'{r["a"]:8.4f}  {r["b"]:8.5f}  {fmt(r["r2"], "4.2f")}  {r["n"]:3}  {fmt(r["coast"], "10.1f")}  '
              f'{fmt(r["revs_hi"], "14.1f")}')
    if len(results) > 1:
        def ms(key):
            v = [r[key] for r in results if r[key] is not None and not math.isnan(r[key])]
            return '—' if not v else f'{statistics.mean(v):.4g} ± {statistics.stdev(v):.2g} (n={len(v)})' if len(v) > 1 else f'{v[0]:.4g} (n=1)'
        print(f'  mean ± sd: a {ms("a")} rev/s² · b {ms("b")} 1/rev · coast {ms("coast")} s · revs {args.hi:g} Hz→stop {ms("revs_hi")}')
    print('  a = constant friction deceleration (bearing), b = drag ∝ speed² (cups); lower a = freer bearing.')
    if args.csv:
        with args.csv.open('w', newline='') as fh:
            w = csv.writer(fh); w.writerow(['label', 'spin', 'seconds', 'revolution', 'rev_per_s'])
            for i, r in enumerate(results, 1):
                for t, n, f in r['points']:
                    w.writerow([args.label, i, round(t - r['start'], 4), n, round(f, 5)])
        print(f'  Points written to {args.csv}')


if __name__ == '__main__':
    main()
