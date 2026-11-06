#!/usr/bin/env python3
"""LAN anemometer dashboard: one serial reader, SQLite history, read-only HTTP API."""
import argparse
import json
import math
from pathlib import Path
import sqlite3
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit, parse_qs

import serial
from monitor import RECEIVER, HZ, UNKNOWN, decode

ROOT = Path(__file__).resolve().parent
RETENTION = 7 * 86400
RAIN_RETENTION = 400 * 86400  # rain rows are tiny; keep over a year of daily totals
GAP = 3600  # no packet for this long is recorded as a data gap


class Store:
    def __init__(self, path):
        self.lock = threading.RLock()
        self.db = sqlite3.connect(str(path), check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        self.db.execute('PRAGMA journal_mode=WAL')
        self.db.executescript('''
          CREATE TABLE IF NOT EXISTS samples (
            id INTEGER PRIMARY KEY AUTOINCREMENT, t REAL NOT NULL,
            stream TEXT NOT NULL, hz REAL NOT NULL, period REAL NOT NULL,
            a REAL NOT NULL, b REAL NOT NULL, imbalance REAL NOT NULL,
            revs TEXT NOT NULL, pulses TEXT NOT NULL);
          CREATE INDEX IF NOT EXISTS sample_time ON samples(t);
          CREATE TABLE IF NOT EXISTS state (key TEXT PRIMARY KEY, value TEXT NOT NULL);
          -- Every received packet, including heartbeats: link continuity and W3 reed diagnostics.
          -- uint64 values are TEXT; pin/pin_age/raw_edges are NULL for W2.
          CREATE TABLE IF NOT EXISTS packets (
            id INTEGER PRIMARY KEY AUTOINCREMENT, t REAL NOT NULL, version TEXT NOT NULL,
            session TEXT NOT NULL, seq INTEGER NOT NULL, pulses TEXT NOT NULL, revs TEXT NOT NULL,
            uptime TEXT NOT NULL, pin INTEGER, pin_age TEXT, raw_edges TEXT,
            rssi INTEGER NOT NULL, snr INTEGER NOT NULL);
          CREATE INDEX IF NOT EXISTS packet_time ON packets(t);
          -- Rain (W5+): one row per packet whose cumulative tip count rose. tips = cumulative
          -- count since sensor boot (TEXT uint64); added = new tips since the previous packet
          -- (several if packets were lost: the cumulative count recovers them). t = estimated
          -- time of the latest of those tips; earlier ones in the same row are attributed to it.
          CREATE TABLE IF NOT EXISTS rain (
            id INTEGER PRIMARY KEY AUTOINCREMENT, t REAL NOT NULL, session TEXT NOT NULL,
            tips TEXT NOT NULL, added INTEGER NOT NULL, interval REAL);
          CREATE INDEX IF NOT EXISTS rain_time ON rain(t);
          -- Long-lived context for rain totals (packets are kept only 7 days): one row per sensor
          -- run (a reset starts a new session, and tips not yet reported before it are lost),
          -- and every stretch of more than GAP seconds without packets.
          CREATE TABLE IF NOT EXISTS runs (
            session TEXT PRIMARY KEY, version TEXT NOT NULL, first_t REAL NOT NULL, last_t REAL NOT NULL);
          CREATE TABLE IF NOT EXISTS gaps (start REAL NOT NULL, end REAL NOT NULL);
          CREATE INDEX IF NOT EXISTS gap_time ON gaps(start);
        ''')
        if not self.db.execute('SELECT 1 FROM runs LIMIT 1').fetchone():
            self.backfill_runs()
        if 'clock' not in [r[1] for r in self.db.execute('PRAGMA table_info(packets)')]:
            # W4: 32 kHz source, 0 = crystal, 1 = internal fallback. NULL for W2/W3.
            self.db.execute('ALTER TABLE packets ADD COLUMN clock INTEGER')
        row = self.db.execute("SELECT value FROM state WHERE key='latest'").fetchone()
        self.latest = json.loads(row[0]) if row else None
        self.connected = False
        self.error = 'Opening receiver'
        self.last_serial = None
        self.malformed = 0
        self.last_prune = 0

    def backfill_runs(self):
        """Seed runs/gaps from the packet log the first time (it holds the last 7 days)."""
        with self.lock:
            self.db.execute('INSERT OR IGNORE INTO runs SELECT session, MAX(version), MIN(t), MAX(t) '
                            'FROM packets GROUP BY session')
            prev = None
            for (t,) in self.db.execute('SELECT t FROM packets ORDER BY t').fetchall():
                if prev is not None and t - prev > GAP:
                    self.db.execute('INSERT INTO gaps VALUES(?,?)', (prev, t))
                prev = t
            self.db.commit()

    def connection(self, connected, error=None):
        with self.lock:
            self.connected, self.error = connected, error

    def accept(self, packet, received=None):
        received = time.time() if received is None else received
        if packet['version'] not in ('W2', 'W3', 'W4', 'W5'):
            with self.lock:
                self.error = 'Legacy W1 firmware: full-revolution frequency unavailable'
            return
        with self.lock:
            old = self.latest
            restart = old is None or packet['session'] != old['session'] or packet['uptime'] < old['uptime']
            stream = f"{packet['session']}-{received:.6f}" if restart else old['stream']
            period = packet['period'] / HZ
            age = None if packet['rev_age'] == UNKNOWN else packet['rev_age'] / HZ
            pulse_age = None if packet['age'] == UNKNOWN else packet['age'] / HZ
            sample_time = received - age if age is not None else None
            latest = dict(packet, received=received, stream=stream, period_s=period,
                          hz=1 / period if period else None, measured_at=sample_time,
                          pulse_at=received - pulse_age if pulse_age is not None else None,
                          a_s=packet['interval_a'] / HZ, b_s=packet['interval_b'] / HZ,
                          imbalance=100 * (packet['interval_a'] - packet['interval_b']) / packet['period'] if period else None)
            # Counts stay strings in JSON/SQLite; JavaScript numbers cannot represent all uint64s.
            latest['count'] = str(packet['count'])
            latest['revolutions'] = str(packet['revolutions'])
            if old and received - old['received'] > GAP:
                self.db.execute('INSERT INTO gaps VALUES(?,?)', (old['received'], received))
            self.db.execute('INSERT INTO runs VALUES(?,?,?,?) ON CONFLICT(session) DO UPDATE SET '
                            'last_t=excluded.last_t, version=excluded.version',
                            (packet['session'], packet['version'], received, received))
            w3 = packet['version'] in ('W3', 'W4', 'W5')
            if 'rain_tips' in packet:
                tips = packet['rain_tips']
                latest['rain_tips'] = str(tips)
                latest['rain_raw_edges'] = str(packet['rain_raw_edges'])
                latest['rain_interval_s'] = packet['rain_interval'] / HZ if packet['rain_interval'] else None
                latest['rain_at'] = None if packet['rain_age'] == UNKNOWN else received - packet['rain_age'] / HZ
                # New sensor session (or first rain-capable packet): every tip since boot is new.
                previous = None if restart or not old or 'rain_tips' not in old else int(old['rain_tips'])
                added = tips - previous if previous is not None and tips >= previous else tips
                if added > 0:
                    self.db.execute('INSERT INTO rain(t,session,tips,added,interval) VALUES(?,?,?,?,?)',
                                    (latest['rain_at'], packet['session'], str(tips), added,
                                     latest['rain_interval_s']))
                elif old and not restart and old.get('rain_at') is not None:
                    latest['rain_at'] = old['rain_at']  # keep the first estimate, as for wind
            if w3:
                latest['raw_edges'] = str(packet['raw_edges'])
                latest['pin_age_s'] = None if packet['pin_age'] == UNKNOWN else packet['pin_age'] / HZ
            self.db.execute('INSERT INTO packets(t,version,session,seq,pulses,revs,uptime,pin,pin_age,raw_edges,rssi,snr,clock) '
                            'VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)',
                            (received, packet['version'], packet['session'], packet['seq'], latest['count'],
                             latest['revolutions'], str(packet['uptime']), packet['pin'] if w3 else None,
                             str(packet['pin_age']) if w3 else None, latest['raw_edges'] if w3 else None,
                             packet['rssi'], packet['snr'], packet.get('clock')))
            new_measurement = period > 0 and (restart or latest['revolutions'] != old['revolutions'])
            if new_measurement:
                self.db.execute('INSERT INTO samples(t,stream,hz,period,a,b,imbalance,revs,pulses) VALUES(?,?,?,?,?,?,?,?,?)',
                                (sample_time, stream, latest['hz'], period, latest['a_s'], latest['b_s'],
                                 latest['imbalance'], latest['revolutions'], latest['count']))
            elif old and not restart and period:
                # A heartbeat repeats the same measurement, not a new graph point.
                latest['measured_at'] = old['measured_at']
            self.latest = latest
            self.error = None
            self.db.execute("INSERT OR REPLACE INTO state VALUES('latest',?)", (json.dumps(latest),))
            if received - self.last_prune > 3600:
                self.db.execute('DELETE FROM samples WHERE t < ?', (received - RETENTION,))
                self.db.execute('DELETE FROM packets WHERE t < ?', (received - RETENTION,))
                self.db.execute('DELETE FROM rain WHERE t < ?', (received - RAIN_RETENTION,))
                self.db.execute('DELETE FROM runs WHERE last_t < ?', (received - RAIN_RETENTION,))
                self.db.execute('DELETE FROM gaps WHERE end < ?', (received - RAIN_RETENTION,))
                self.last_prune = received
            self.db.commit()

    def status(self):
        return dict(connected=self.connected, error=self.error, latest=self.latest,
                    last_serial=self.last_serial, malformed=self.malformed, now=time.time())

    def condensed(self, start, end, bucket, calm):
        """Long ranges: per time bucket, the slowest and fastest real samples (so extremes and
        calm-spanning revolutions survive) plus totals for a time-weighted strip average:
        n revolutions, tsum = their summed period, csum = the part of it deduced calm
        (each half longer than `calm` seconds contributes half - calm)."""
        with self.lock:
            cursor = self.db.execute('SELECT COALESCE(MAX(id),0) FROM samples').fetchone()[0]
            base = 'SELECT *, CAST((t - :s) / :b AS INTEGER) AS bk FROM samples WHERE t >= :s AND t < :e'
            args = dict(s=start, e=end, b=bucket, c=calm)
            rows = self.db.execute(
                f'SELECT id,t,stream,hz,period,a,b,imbalance,revs,pulses FROM ('
                f'SELECT *, ROW_NUMBER() OVER (PARTITION BY bk ORDER BY hz) AS lo, '
                f'ROW_NUMBER() OVER (PARTITION BY bk ORDER BY hz DESC) AS hi FROM ({base})) '
                f'WHERE lo = 1 OR hi = 1 ORDER BY t', args).fetchall()
            buckets = self.db.execute(
                f'SELECT :s + bk * :b AS t0, COUNT(*) AS n, SUM(period) AS tsum, '
                f'SUM(MAX(a - :c, 0) + MAX(b - :c, 0)) AS csum FROM ({base}) GROUP BY bk ORDER BY bk',
                args).fetchall()
            return dict(self.status(), cursor=cursor, bucket=bucket, points=[dict(r) for r in rows],
                        buckets=[dict(r) for r in buckets])

    def query(self, after=None, start=None, end=None):
        with self.lock:
            cursor = self.db.execute('SELECT COALESCE(MAX(id),0) FROM samples').fetchone()[0]
            if after is not None:
                rows = self.db.execute('SELECT * FROM samples WHERE id > ? ORDER BY id LIMIT 2000', (after,)).fetchall()
                if rows:
                    cursor = rows[-1]['id']
            else:
                rows = self.db.execute('SELECT * FROM samples WHERE t >= ? AND t < ? ORDER BY t LIMIT 60000', (start, end)).fetchall()
            return dict(self.status(), cursor=cursor, points=[dict(row) for row in rows])

    def rain_total(self, start, end):
        """Tips in [start, end) with what qualifies the total: sensor restarts inside the period
        (unreported tips before each reset are lost), packet gaps over GAP (fine within one
        sensor run, since counts are cumulative), and when rain logging (W5+) begins."""
        with self.lock:
            tips, first, last = self.db.execute(
                'SELECT COALESCE(SUM(added),0), MIN(t), MAX(t) FROM rain WHERE t >= ? AND t < ?',
                (start, end)).fetchone()
            restarts = [r[0] for r in self.db.execute(
                'SELECT first_t FROM runs WHERE first_t > ? AND first_t < ? '
                'AND first_t > (SELECT MIN(first_t) FROM runs) ORDER BY first_t', (start, end))]
            gaps = [dict(start=a, end=b) for a, b in self.db.execute(
                'SELECT start, end FROM gaps WHERE end > ? AND start < ? ORDER BY start', (start, end))]
            since = self.db.execute("SELECT MIN(first_t) FROM runs WHERE version >= 'W5'").fetchone()[0]
            return dict(start=start, end=end, tips=tips, first_tip=first, last_tip=last,
                        restarts=restarts, gaps=gaps, rain_logging_since=since, now=time.time())

    def rain(self, start, end):
        """Tip rows in [start, end); the browser applies mm/tip calibration and local midnights."""
        with self.lock:
            rows = self.db.execute('SELECT t, added, interval FROM rain WHERE t >= ? AND t < ? ORDER BY t',
                                   (start, end)).fetchall()
            return dict(self.status(), tips=[dict(row) for row in rows])


def serial_worker(store, port, stop):
    while not stop.is_set():
        try:
            with serial.Serial(port, 115200, timeout=1, exclusive=True) as device:
                store.connection(True)
                print(f'Receiver connected: {port}', flush=True)
                buffer = b''
                while not stop.is_set():
                    chunk = device.read(min(4096, device.in_waiting or 1))
                    if not chunk:
                        continue
                    buffer += chunk
                    while b'\n' in buffer:
                        line, buffer = buffer.split(b'\n', 1)
                        with store.lock:
                            store.last_serial = time.time()
                        try:
                            packet = decode(line.decode('ascii', errors='replace').strip())
                            if packet:
                                store.accept(packet)
                        except (ValueError, TypeError):
                            with store.lock:
                                store.malformed += 1
                    if len(buffer) > 2048:
                        buffer = b''
                        with store.lock:
                            store.malformed += 1
        except (serial.SerialException, OSError) as exc:
            store.connection(False, str(exc))
            print(f'Receiver unavailable: {exc}', flush=True)
            stop.wait(3)


def handler_for(store):
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            url = urlsplit(self.path)
            try:
                if url.path in ('/', '/index.html'):
                    body = (ROOT / 'web' / 'index.html').read_bytes()
                    kind = 'text/html; charset=utf-8'
                elif url.path == '/math.js':
                    body = (ROOT / 'web' / 'math.js').read_bytes()
                    kind = 'text/javascript; charset=utf-8'
                elif url.path == '/api/rain/total':
                    params = parse_qs(url.query)
                    start = float(params['start'][0])
                    end = float(params.get('end', [str(time.time())])[0])
                    if not all(map(math.isfinite, (start, end))) or not 0 < end - start <= 800 * 86400:
                        raise ValueError('rain total range must be positive and at most 800 days')
                    body = json.dumps(store.rain_total(start, end), allow_nan=False).encode()
                    kind = 'application/json'
                elif url.path == '/api/rain':
                    params = parse_qs(url.query)
                    start, end = float(params['start'][0]), float(params['end'][0])
                    if not all(map(math.isfinite, (start, end))) or not 0 < end-start <= 32*86400:
                        raise ValueError('rain range must be positive and at most 32 days')
                    body = json.dumps(store.rain(start, end), allow_nan=False).encode()
                    kind = 'application/json'
                elif url.path in ('/api/live', '/api/history'):
                    params = parse_qs(url.query)
                    if url.path == '/api/live':
                        after = int(params.get('after', ['0'])[0])
                        if after < 0:
                            raise ValueError('invalid cursor')
                        data = store.query(after=after)
                    else:
                        end = float(params.get('end', [str(time.time()+3600)])[0])
                        start = float(params.get('start', [str(end-3600)])[0])
                        if not all(map(math.isfinite, (start, end))) or end <= start:
                            raise ValueError('history range must be positive')
                        if 'bucket' in params:
                            # Condensed: up to 12 strips of 24 h, at most 20000 buckets.
                            bucket = float(params['bucket'][0])
                            calm = float(params.get('calm', ['20'])[0])
                            if not (math.isfinite(bucket) and math.isfinite(calm) and bucket >= 1 and calm > 0
                                    and end - start <= 12 * 86400 and (end - start) / bucket <= 20000):
                                raise ValueError('invalid condensed history request')
                            data = store.condensed(start, end, bucket, calm)
                        elif end - start > 12 * 3600:
                            raise ValueError('full-resolution history range is at most 12 hours')
                        else:
                            data = store.query(start=start, end=end)
                    body = json.dumps(data, allow_nan=False).encode()
                    kind = 'application/json'
                else:
                    self.send_error(404)
                    return
            except (ValueError, OverflowError, KeyError):
                self.send_error(400, 'Invalid query parameters')
                return
            self.send_response(200)
            self.send_header('Content-Type', kind)
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store')
            self.send_header('X-Content-Type-Options', 'nosniff')
            self.end_headers()
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass

        def log_message(self, *args):
            pass
    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='0.0.0.0')
    parser.add_argument('--port', type=int, default=8765)
    parser.add_argument('--serial', default=RECEIVER)
    parser.add_argument('--db', type=Path, default=ROOT / 'data' / 'wind.sqlite3')
    args = parser.parse_args()
    args.db.parent.mkdir(parents=True, exist_ok=True)
    store = Store(args.db)
    server = ThreadingHTTPServer((args.host, args.port), handler_for(store))
    stop = threading.Event()
    threading.Thread(target=serial_worker, args=(store, args.serial, stop), daemon=True).start()
    print(f'Dashboard: http://{args.host}:{args.port} (LAN, unauthenticated/read-only)', flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        server.server_close()


if __name__ == '__main__':
    main()
