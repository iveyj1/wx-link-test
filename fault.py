#!/usr/bin/env python3
"""Sensor faults: mark one sensor's readings as wrong for a period.

A fault is like a maintenance window for a single sensor: its data is kept but left out of
that sensor's graphs, averages and totals, and the dashboard shows the fault (a banner while
it is open, shading afterwards). Use it for e.g. a disconnected wire that makes the wind read
calm. Sensors: wind (anemometer), rain (gauge), baro (pressure/temperature). Times are local:
"21:08", "21:08:30", "2026-11-06 21:08", or "now". Safe while the dashboard service is running.

  fault.py list
  fault.py start SENSOR [--note TEXT]          open a fault now
  fault.py stop SENSOR                         close the sensor's open fault now
  fault.py add SENSOR FROM TO|open [--note]    record a fault afterwards ("open" = still faulty)
  fault.py end ID TIME                         set the end of fault ID
  fault.py remove ID                           delete a fault (its data shows again)
"""
import argparse
from pathlib import Path
import sqlite3
import sys
import time

from maint import DB, show, when
from web_server import FAULTS_TABLE, SENSORS

SAMPLES = {
    'wind': 'SELECT COUNT(*) FROM samples WHERE t - period < ? AND t >= ?',
    'rain': 'SELECT COALESCE(SUM(added), 0) FROM rain WHERE t < ? AND t >= ?',
    'baro': 'SELECT COUNT(*) FROM weather WHERE t < ? AND t >= ?',
}
UNITS = {'wind': 'revolutions', 'rain': 'tips', 'baro': 'readings'}


def connect(path):
    if not path.exists():
        sys.exit(f'No database at {path}')
    db = sqlite3.connect(str(path), timeout=10)
    db.execute(FAULTS_TABLE)
    return db


def list_faults(db):
    rows = db.execute('SELECT id, sensor, start, end, note FROM faults ORDER BY start').fetchall()
    if not rows:
        print('No sensor faults.')
    for i, sensor, start, end, note in rows:
        n = db.execute(SAMPLES[sensor], (end if end is not None else time.time() + 1, start)).fetchone()[0]
        print(f'{i:4}  {sensor:4}  {show(start)} → {show(end)}  {n} {UNITS[sensor]} hidden'
              f'{"  · " + note if note else ""}')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--db', type=Path, default=DB)
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('list')
    p = sub.add_parser('start'); p.add_argument('sensor', choices=SENSORS); p.add_argument('--note', default='')
    p = sub.add_parser('stop'); p.add_argument('sensor', choices=SENSORS)
    p = sub.add_parser('add'); p.add_argument('sensor', choices=SENSORS); p.add_argument('start', type=when)
    p.add_argument('end'); p.add_argument('--note', default='')
    p = sub.add_parser('end'); p.add_argument('id', type=int); p.add_argument('end', type=when)
    p = sub.add_parser('remove'); p.add_argument('id', type=int)
    args = parser.parse_args(argv)
    db = connect(args.db)
    sensor = getattr(args, 'sensor', None)
    open_ = db.execute('SELECT id, start FROM faults WHERE end IS NULL AND sensor = ?', (sensor,)).fetchone()

    if args.command == 'start':
        if open_:
            sys.exit(f'Fault {open_[0]} ({sensor}) is already open since {show(open_[1])}.')
        db.execute('INSERT INTO faults(sensor, start, note) VALUES(?, ?, ?)', (sensor, time.time(), args.note))
    elif args.command == 'stop':
        if not open_:
            sys.exit(f'No open {sensor} fault.')
        db.execute('UPDATE faults SET end=? WHERE id=?', (time.time(), open_[0]))
    elif args.command == 'add':
        end = None if args.end == 'open' else when(args.end)
        if end is None and open_:
            sys.exit(f'Fault {open_[0]} ({sensor}) is already open; only one per sensor can be.')
        if end is not None and end <= args.start:
            sys.exit('The end must be after the start.')
        db.execute('INSERT INTO faults(sensor, start, end, note) VALUES(?, ?, ?, ?)', (sensor, args.start, end, args.note))
    elif args.command == 'end':
        row = db.execute('SELECT start FROM faults WHERE id=?', (args.id,)).fetchone()
        if not row:
            sys.exit(f'No fault {args.id}.')
        if args.end <= row[0]:
            sys.exit('The end must be after the start.')
        db.execute('UPDATE faults SET end=? WHERE id=?', (args.end, args.id))
    elif args.command == 'remove':
        if not db.execute('DELETE FROM faults WHERE id=?', (args.id,)).rowcount:
            sys.exit(f'No fault {args.id}.')
    db.commit()
    list_faults(db)


if __name__ == '__main__':
    main()
