"""spindown.py recovers known friction/drag from simulated spins sampled like the sensor reports."""
import io
import sqlite3
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

import spindown


def simulate(f0, a, b, dt=1e-4):
    """Completion time and duration of each full revolution while coasting from f0 to rest."""
    t = theta = 0.0; f = f0; last = 0.0; revs = []
    while f > 0:
        f -= (a + b * f * f) * dt
        theta += max(f, 0) * dt; t += dt
        if theta >= len(revs) + 1:
            revs.append((t, t - last)); last = t
    return revs


def build(path, spins, a, b, t0=1_800_000_000.0, rest=30.0):
    db = sqlite3.connect(path)
    db.execute('CREATE TABLE samples (id INTEGER PRIMARY KEY, t REAL, stream TEXT, hz REAL, period REAL, '
               'a REAL, b REAL, imbalance REAL, revs TEXT, pulses TEXT)')
    db.execute('CREATE TABLE packets (id INTEGER PRIMARY KEY, t REAL, session TEXT, pulses TEXT, revs TEXT)')
    total, t = 0, t0
    for f0 in spins:
        # The first revolution after a rest straddles the stop: one long period, as the sensor sees it.
        revs = simulate(f0, a, b)
        # Like ReportSchedule: a completed revolution is reported at once unless within 1 s of the
        # last report; then the latest one is reported when the cooldown ends (none are dropped).
        allowed, i = -1e9, 0
        while i < len(revs):
            when = max(revs[i][0], allowed)
            while i + 1 < len(revs) and revs[i + 1][0] <= when:
                i += 1
            tc, period = revs[i]
            n = total + i + 1
            if i == 0:
                period = rest + period
            db.execute('INSERT INTO samples(t,stream,hz,period,a,b,imbalance,revs,pulses) VALUES(?,?,?,?,0,0,0,?,?)',
                       (t + tc, 'abcd1234-1', 1 / period, period, str(n), str(2 * n + 1)))
            db.execute('INSERT INTO packets(t,session,pulses,revs) VALUES(?,?,?,?)', (t + when, 'abcd1234', str(2 * n + 1), str(n)))
            allowed, i = when + 1.0, i + 1
        total += len(revs)
        db.execute('INSERT INTO packets(t,session,pulses,revs) VALUES(?,?,?,?)', (t + revs[-1][0] + 2, 'abcd1234', str(2 * total + 2), str(total)))
        t += revs[-1][0] + rest
    db.commit(); db.close()
    return t0, t


class SpinDown(unittest.TestCase):
    def test_recovers_friction_and_drag(self):
        a, b = 0.12, 0.02  # rev/s^2, 1/rev
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'w.sqlite3'
            t0, t1 = build(path, [6.0, 4.5, 7.5], a, b)
            db = sqlite3.connect(path)
            spins = spindown.split(spindown.load(db, t0, t1 + 60), 10.0, 20.0, 1.3)
            self.assertEqual(len(spins), 3)
            for i, spin in enumerate(spins):
                nxt = spins[i + 1][0]['t'] - spins[i + 1][0]['period'] if i + 1 < len(spins) else t1 + 60
                r = spindown.analyse(spin, db, 3.0, 1.0, nxt)
                self.assertAlmostEqual(r['a'], a, delta=0.1 * a)
                self.assertAlmostEqual(r['b'], b, delta=0.1 * b)
                self.assertGreater(r['r2'], 0.99)
                self.assertEqual(r['extra_half'], 1)

    def test_cli_smoke(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'w.sqlite3'
            t0, t1 = build(path, [5.0, 5.5], 0.1, 0.015)
            import datetime as dt
            fmt = lambda t: dt.datetime.fromtimestamp(t).strftime('%Y-%m-%d %H:%M:%S')
            out = io.StringIO()
            with redirect_stdout(out):
                spindown.main(['--db', str(path), '--since', fmt(t0 - 5), '--until', fmt(t1 + 60),
                               '--label', 'test', '--csv', str(Path(d) / 'p.csv')])
            text = out.getvalue()
            self.assertIn('mean ± sd', text)
            self.assertIn('test', text)
            self.assertTrue((Path(d) / 'p.csv').read_text().startswith('label,spin'))


if __name__ == '__main__':
    unittest.main()
