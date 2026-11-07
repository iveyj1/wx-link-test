import json
import tempfile
import threading
import time
import unittest
from pathlib import Path
from urllib.request import Request, urlopen
from urllib.error import HTTPError
from http.server import ThreadingHTTPServer
from monitor import decode
from web_server import Store, handler_for


def packet(revs=1, seq=1, age=328, session='abcd1234', uptime=100000):
    payload = f'W2,{session},{seq},{2*revs+1},{revs},32768,8192,24576,{age},{age},{uptime}'
    return decode(f'+RCV=1,{len(payload)},{payload},-28,13')


class WebTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.store = Store(Path(self.temp.name)/'history.db')

    def tearDown(self):
        self.store.db.close()
        self.temp.cleanup()

    def test_measurement_and_heartbeat(self):
        now = time.time()
        self.store.accept(packet(), now)
        self.store.accept(packet(seq=2, age=328+60*32768, uptime=3000000), now+60)
        result = self.store.query(after=0)
        self.assertEqual(len(result['points']), 1)
        self.assertEqual(result['points'][0]['hz'], 1)
        self.assertEqual(result['latest']['revolutions'], '1')
        self.assertAlmostEqual(result['latest']['measured_at'], now-328/32768)
        self.assertEqual(result['points'][0]['imbalance'], -50)
        self.store.accept(packet(revs=2, seq=3, uptime=4000000), now+61)
        self.assertEqual(len(self.store.query(after=result['cursor'])['points']), 1)

    def test_restart_and_persistence(self):
        now = time.time()
        self.store.accept(packet(), now)
        first = self.store.latest['stream']
        self.store.accept(packet(session='ffffffff'), now+5)
        self.assertNotEqual(first, self.store.latest['stream'])
        second = Store(Path(self.temp.name)/'history.db')
        try:
            second.accept(packet(session='ffffffff', seq=2), now+6)
            self.assertEqual(len(second.query(after=0)['points']), 2)
            self.assertFalse(second.connected)
        finally:
            second.db.close()

    def test_w3_packets_logged(self):
        now = time.time()
        self.store.accept(packet(), now)
        payload = 'W3,abcd1234,2,3,1,32768,8192,24576,1966408,1966408,2100000,1,1966080,9'
        self.store.accept(decode(f'+RCV=1,{len(payload)},{payload},-90,7'), now+60)
        rows = self.store.db.execute('SELECT version,seq,pin,pin_age,raw_edges,rssi FROM packets ORDER BY id').fetchall()
        self.assertEqual([tuple(r) for r in rows],
                         [('W2', 1, None, None, None, -28), ('W3', 2, 1, '1966080', '9', -90)])
        self.assertEqual(self.store.latest['raw_edges'], '9')
        self.assertEqual(self.store.latest['pin_age_s'], 60)
        self.assertEqual(len(self.store.query(after=0)['points']), 1)  # heartbeat: no new graph point

    def test_w4_clock_logged(self):
        payload = 'W4,abcd1234,1,3,1,32768,8192,24576,328,328,100000,0,1966080,9,1'
        self.store.accept(decode(f'+RCV=1,{len(payload)},{payload},-90,7'))
        self.assertEqual(self.store.db.execute('SELECT version,clock FROM packets').fetchone()[:], ('W4', 1))
        self.assertEqual(self.store.latest['clock'], 1)

    def w5(self, seq, tips, age=328, interval=0, received=None, session='abcd1234', count=3, uptime=100000):
        f = lambda n: format(n, 'x')
        rain_age = 2**64-1 if tips == 0 else age
        payload = (f'W5,{session},{f(seq)},{f(count)},2000,6000,148,148,{f(uptime)},1,'
                   f'ffffffffffffffff,{f(count)},0,{f(tips)},{f(interval)},{f(rain_age)},0,{f(tips+4)}')
        assert len(payload) <= 240
        self.store.accept(decode(f'+RCV=1,{len(payload)},{payload},-80,9'), received)

    def test_w5_rain_tips_and_loss_recovery(self):
        now = 1_800_000_000.0
        self.w5(1, 0, received=now)
        self.w5(2, 1, received=now+10)                      # first tip: no interval yet
        self.w5(5, 4, interval=32768*30, received=now+100)  # 2 packets lost: 3 tips recovered
        self.w5(6, 4, age=32768*60, interval=32768*30, received=now+160)  # heartbeat: no row
        rows = self.store.db.execute('SELECT t,tips,added,interval FROM rain ORDER BY id').fetchall()
        self.assertEqual([tuple(r) for r in rows],
                         [(now+10-328/32768, '1', 1, None), (now+100-328/32768, '4', 3, 30.0)])
        latest = self.store.latest
        self.assertEqual((latest['rain_tips'], latest['rain_interval_s'], latest['rain_raw_edges']), ('4', 30.0, '8'))
        self.assertAlmostEqual(latest['rain_at'], now+100-328/32768)
        self.assertEqual(latest['revolutions'], '1')
        self.assertEqual((latest['interval_a'], latest['period_s']), (0x2000, 1.0))  # hex fields
        result = self.store.rain(now, now+1000)
        self.assertEqual(sum(r['added'] for r in result['tips']), 4)
        self.assertEqual(self.store.rain(now+50, now+1000)['tips'][0]['added'], 3)
        # Sensor restart: count restarts from 0; tips since boot are all new.
        self.w5(1, 2, interval=32768, received=now+200, session='ffffffff', uptime=200000)
        self.assertEqual(sum(r['added'] for r in self.store.rain(now, now+1000)['tips']), 6)

    def test_rain_total_with_restart_and_gap(self):
        now = 1_800_000_000.0
        self.w5(1, 0, received=now)
        self.w5(2, 3, interval=32768, received=now + 100)
        self.w5(3, 5, interval=32768, received=now + 100 + 2 * 3600)   # 2 h without packets
        self.w5(1, 2, interval=32768, received=now + 9000, session='ffffffff', uptime=50000)  # reset
        total = self.store.rain_total(now + 50, now + 10000)
        self.assertEqual(total['tips'], 3 + 2 + 2)
        self.assertEqual(total['restarts'], [now + 9000])
        self.assertEqual(total['gaps'], [dict(start=now + 100, end=now + 100 + 7200)])
        self.assertEqual(total['rain_logging_since'], now)
        quiet = self.store.rain_total(now + 9500, now + 9600)
        self.assertEqual((quiet['tips'], quiet['restarts'], quiet['gaps']), (0, [], []))
        # Runs and gaps are rebuilt from the packet log when the tables start empty.
        self.store.db.execute('DELETE FROM runs'); self.store.db.execute('DELETE FROM gaps')
        self.store.backfill_runs()
        self.assertEqual(self.store.rain_total(now + 50, now + 10000)['gaps'], total['gaps'])
        self.assertEqual(self.store.rain_total(now + 50, now + 10000)['restarts'], [now + 9000])

    def test_w5_rain_only_report_is_not_a_wind_sample(self):
        self.w5(1, 0)
        self.w5(2, 1)
        self.assertEqual(len(self.store.query(after=0)['points']), 1)

    def test_w5_validation(self):
        good = 'W5,abcd1234,1,3,2000,6000,148,148,186a0,1,ffffffffffffffff,3,0,0,0,ffffffffffffffff,1,0'
        self.assertEqual(decode(f'+RCV=1,{len(good)},{good},-80,9')['rain_tips'], 0)
        for bad in (good.replace(',0,ffffffffffffffff,1,0', ',1,ffffffffffffffff,1,1'),  # tips but no age
                    good.replace('186a0', '186A0'), good.replace(',3,2000', ',2,2000'),
                    good + ',0', good.replace(',1,0', ',1,0x')):
            with self.assertRaises(ValueError, msg=bad):
                decode(f'+RCV=1,{len(bad)},{bad},-80,9')

    def test_w6_battery_logged(self):
        now = 1_800_000_000.0
        for i, mv in enumerate((4100, 4090, 4080)):
            p = (f'W6,abcd1234,{i+1:x},3,2000,6000,148,148,{100000 + i * 32768 * 60:x},1,ffffffffffffffff,3,0,'
                 f'0,0,ffffffffffffffff,1,0,{mv:x}')
            self.store.accept(decode(f'+RCV=1,{len(p)},{p},-80,9'), now + 60 * i)
        self.assertEqual(self.store.latest['vbat_v'], 4.08)
        pts = self.store.battery(now, now + 600, 600)['points']
        self.assertEqual((len(pts), pts[0]['mv'], pts[0]['lo'], pts[0]['hi'], pts[0]['n']), (1, 4090, 4080, 4100, 3))
        self.w5(9, 0, received=now + 300)       # W5 packets carry no battery value
        self.assertIsNone(self.store.latest['vbat_v'])

    def test_w7_weather_logged(self):
        now = 1_800_000_000.0
        def w7(i, pa, dk, t):
            p = (f'W7,abcd1234,{i:x},3,2000,6000,148,148,{100000 + i * 32768:x},1,ffffffffffffffff,3,0,'
                 f'0,0,ffffffffffffffff,1,0,1000,{pa:x},{dk:x}')
            self.store.accept(decode(f'+RCV=1,{len(p)},{p},-80,9'), t)
        w7(1, 101300, 2932, now)          # 20.0 C
        w7(2, 101310, 2934, now + 20)     # within WEATHER_STEP: packet only
        w7(3, 101200, 2942, now + 70)
        w7(4, 0, 0, now + 140)            # no barometer reading
        self.assertEqual(self.store.db.execute('SELECT COUNT(*) FROM weather').fetchone()[0], 2)
        self.assertEqual(tuple(self.store.db.execute('SELECT pressure_pa, temp_c FROM packets ORDER BY t').fetchall()[1]),
                         (101310, 20.2))
        self.assertIsNone(self.store.latest['pressure_pa'])
        pts = self.store.weather(now, now + 600, 600)['points']
        self.assertEqual((pts[0]['plo'], pts[0]['phi'], pts[0]['n']), (101200, 101300, 2))
        self.assertAlmostEqual(pts[0]['c'], 20.5)
        self.store.maintenance('start', now=now + 60); self.store.maintenance('stop', now=now + 100)
        self.assertEqual(self.store.weather(now, now + 600, 600)['points'][0]['n'], 1)

    def test_condensed_history(self):
        now = 1_800_000_000.0
        # 30 one-second revolutions in one 60 s bucket, then a 300 s calm revolution.
        for i in range(30):
            self.w5(i + 1, 0, received=now + i, count=2 * i + 3, uptime=100000 + i * 32768)
        payload_rows = self.store.db.execute('SELECT COUNT(*) FROM samples').fetchone()[0]
        self.assertEqual(payload_rows, 30)
        self.store.db.execute('UPDATE samples SET hz = 1.0 / 300, period = 300, a = 290, b = 10 '
                              'WHERE id = (SELECT MAX(id) FROM samples)')
        result = self.store.condensed(now - 60, now + 60, 60, 20)
        self.assertLessEqual(len(result['points']), 4)          # min and max per bucket
        self.assertIn(1.0 / 300, [p['hz'] for p in result['points']])  # calm revolution kept
        totals = [(b['n'], round(b['tsum'], 3), round(b['csum'], 3)) for b in result['buckets']]
        self.assertEqual(sum(t[0] for t in totals), 30)
        self.assertAlmostEqual(sum(t[2] for t in totals), 270)  # 290 - 20

    def test_unknown_period(self):
        payload = 'W2,abcd1234,1,0,0,0,0,0,18446744073709551615,18446744073709551615,50000'
        self.store.accept(decode(f'+RCV=1,{len(payload)},{payload},-28,13'))
        self.assertIsNone(self.store.latest['hz'])
        self.assertEqual(self.store.query(after=0)['points'], [])

    def test_maintenance_hides_samples_and_tips(self):
        now = 1_800_000_000.0
        # packet(revs) completes a 1 s revolution 328/32768 s before reception.
        self.store.accept(packet(revs=1, seq=1, uptime=100000), now)
        self.store.maintenance('start', 'takedown', now=now + 100)
        self.store.accept(packet(revs=2, seq=2, uptime=200000), now + 200)            # inside
        self.store.maintenance('stop', now=now + 300)
        self.store.maintenance('stop', now=now + 310)                                   # no-op
        self.store.accept(packet(revs=3, seq=3, uptime=300000), now + 300.5)          # revolution overlaps the end
        self.store.accept(packet(revs=4, seq=4, uptime=400000), now + 400)            # after
        self.assertEqual(len(self.store.db.execute('SELECT * FROM samples').fetchall()), 4)  # all recorded
        shown = [p['revs'] for p in self.store.query(start=now - 10, end=now + 1000)['points']]
        self.assertEqual(shown, ['1', '4'])
        live = self.store.query(after=0)
        self.assertEqual([p['revs'] for p in live['points']], ['1', '4'])
        self.assertNotIn('ok', live['points'][0])
        self.assertEqual(live['cursor'], self.store.db.execute('SELECT MAX(id) FROM samples').fetchone()[0])
        self.assertEqual(sum(b['n'] for b in self.store.condensed(now - 10, now + 1000, 100, 20)['buckets']), 2)
        windows = self.store.status()['maintenance']
        self.assertEqual([(w['start'], w['end'], w['note']) for w in windows if w['start'] == now + 100],
                         [(now + 100, now + 300, 'takedown')])
        with self.assertRaises(ValueError):
            self.store.maintenance('pause')

    def test_maintenance_excludes_rain(self):
        now = 1_800_000_000.0
        self.w5(1, 0, received=now)
        self.w5(2, 1, received=now + 10)
        self.store.maintenance('start', now=now + 50)
        self.assertIsNotNone(self.store.status()['maintenance_active'])
        self.w5(3, 6, interval=32768, received=now + 60)        # handling: 5 tips
        self.store.maintenance('stop', now=now + 70)
        self.w5(4, 7, interval=32768, received=now + 80)
        self.assertIsNone(self.store.status()['maintenance_active'])
        self.assertEqual(sum(r['added'] for r in self.store.rain(now, now + 100)['tips']), 2)
        total = self.store.rain_total(now, now + 100)
        self.assertEqual(total['tips'], 2)
        self.assertEqual(len(total['maintenance']), 1)
        self.assertEqual(self.store.rain_total(now + 75, now + 100)['maintenance'], [])

    def test_link_statistics(self):
        now = time.time()
        for seq in (1, 2, 5, 5, 6):                                   # 3 and 4 lost; 5 duplicated
            self.store.accept(packet(revs=seq, seq=seq, uptime=100000 * seq), now - 100 + seq)
        self.store.accept(packet(revs=1, seq=1, session='ffffffff', uptime=1000), now - 50)  # new session
        link = self.store.link(now)['h1']
        self.assertEqual((link['received'], link['missing']), (5, 2))
        self.assertEqual(link['rssi'], -28)
        self.assertIs(self.store.link(now + 5), self.store.link(now + 5))   # cached

    def test_maint_cli(self):
        import maint
        now = time.time()
        db = Path(self.temp.name) / 'history.db'
        self.w5(1, 0, received=now - 600)
        self.w5(2, 3, interval=32768, received=now - 300)
        self.store.db.commit()
        stamp = lambda t: time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(t))
        maint.main(['--db', str(db), 'add', stamp(now - 400), stamp(now - 200), '--note', 'bench'])
        self.assertEqual(self.store.rain_total(now - 1000, now)['tips'], 0)
        wid = self.store.windows()[0]['id']
        maint.main(['--db', str(db), 'remove', str(wid)])
        self.assertEqual(self.store.rain_total(now - 1000, now)['tips'], 3)
        maint.main(['--db', str(db), 'purge', stamp(now - 400), stamp(now - 200), '--yes'])
        self.assertEqual(self.store.rain_total(now - 1000, now)['tips'], 0)
        self.assertEqual(len(self.store.db.execute('SELECT * FROM packets').fetchall()), 1)
        backups = list((Path(self.temp.name) / 'backups').glob('*.sqlite3'))
        self.assertEqual(len(backups), 1)
        copy = __import__('sqlite3').connect(str(backups[0]))
        self.assertEqual(copy.execute('SELECT COUNT(*) FROM packets').fetchone()[0], 2)
        copy.close()
        self.assertAlmostEqual(maint.when('now'), time.time(), delta=2)
        with self.assertRaises(SystemExit):
            maint.main(['--db', str(db), 'add', stamp(now - 100), stamp(now - 200)])

    def test_http(self):
        self.store.accept(packet())
        server = ThreadingHTTPServer(('127.0.0.1',0), handler_for(self.store))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        base = f'http://127.0.0.1:{server.server_port}'
        try:
            with urlopen(base+'/api/live?after=0') as response:
                self.assertEqual(json.load(response)['points'][0]['hz'], 1)
            with urlopen(base+'/') as response:
                self.assertIn(b'Anemometer helicorder', response.read())
            with urlopen(base+'/math.js') as response:
                self.assertIn(b'niceScale', response.read())
                self.assertIn('javascript', response.headers['Content-Type'])
            with urlopen(base+'/api/weather?start=0&end=86400') as response:
                self.assertEqual(json.load(response)['points'], [])
            with self.assertRaises(HTTPError):
                urlopen(base+'/api/weather?start=0&end=99999999')
            with urlopen(base+'/api/battery?start=0&end=86400') as response:
                self.assertEqual(json.load(response)['points'], [])
            with urlopen(base+'/api/rain/total?start=0&end=86400') as response:
                self.assertEqual(json.load(response)['tips'], 0)
            with urlopen(base+'/api/rain?start=0&end=86400') as response:
                self.assertEqual(json.load(response)['tips'], [])
            with urlopen(base+'/api/history?start=0&end=86400&bucket=120&calm=20') as response:
                self.assertEqual(json.load(response)['buckets'], [])
            post = lambda body, kind='application/json': urlopen(Request(
                base + '/api/maintenance', data=body, headers={'Content-Type': kind}, method='POST'))
            with post(b'{"action":"start","note":"test"}') as response:
                self.assertEqual(json.load(response)['maintenance_active']['note'], 'test')
            with post(b'{"action":"stop"}') as response:
                self.assertIsNone(json.load(response)['maintenance_active'])
            for body, kind in ((b'action=start', 'application/x-www-form-urlencoded'), (b'{"action":"x"}', 'application/json'),
                               (b'[]', 'application/json'), (b'{"action":"start","note":5}', 'application/json')):
                with self.assertRaises(HTTPError):
                    post(body, kind)
            for query in ('/api/rain/total?start=x', '/api/rain/total?start=5&end=1', '/api/rain?start=0', '/api/rain?start=0&end=9999999', '/api/history?start=nan&end=1',
                          '/api/history?start=0&end=86400&bucket=0.5', '/api/history?start=0&end=86400', '/api/history?start=0&end=50000', '/api/live?after=-1', '/../../etc/passwd'):
                with self.assertRaises(HTTPError):
                    urlopen(base+query)
        finally:
            server.shutdown(); server.server_close(); thread.join()


if __name__ == '__main__':
    unittest.main()
