#!/usr/bin/env python3
"""Maintenance windows and data trimming for the wind/rain history database.

A maintenance window keeps the data but hides it from graphs and rain totals: use it for
takedowns, bench tests and handling. It can be removed again. `purge` deletes rows for good,
after a full backup (data/backups/). Times are local: "21:08", "21:08:30", "2026-11-06 21:08",
or "now". Safe while the dashboard service is running.

  maint.py list
  maint.py start [--note TEXT]          open a window now (same as the dashboard button)
  maint.py stop                         close the open window now
  maint.py add FROM TO|open [--note]    record a window afterwards ("open" = still in progress)
  maint.py end ID TIME                  set the end of window ID
  maint.py remove ID                    delete a window (its data shows again)
  maint.py purge FROM TO [--yes]        back up, then delete samples, packets and rain in [FROM, TO)
"""
import argparse
import datetime as dt
from pathlib import Path
import sqlite3
import sys
import time

ROOT = Path(__file__).resolve().parent
DB = ROOT / 'data' / 'wind.sqlite3'


def when(text):
    """Local time text -> epoch seconds. A bare time means today (yesterday if in the future)."""
    if text == 'now':
        return time.time()
    for fmt in ('%Y-%m-%d %H:%M:%S', '%Y-%m-%d %H:%M', '%Y-%m-%dT%H:%M:%S', '%Y-%m-%dT%H:%M'):
        try:
            return dt.datetime.strptime(text, fmt).timestamp()
        except ValueError:
            pass
    for fmt in ('%H:%M:%S', '%H:%M'):
        try:
            clock = dt.datetime.strptime(text, fmt).time()
        except ValueError:
            continue
        moment = dt.datetime.combine(dt.date.today(), clock)
        if moment.timestamp() > time.time() + 60:
            moment -= dt.timedelta(days=1)
        return moment.timestamp()
    raise argparse.ArgumentTypeError(f'cannot read time {text!r} (use HH:MM, "YYYY-MM-DD HH:MM" or now)')


def show(t):
    return '…' if t is None else dt.datetime.fromtimestamp(t).strftime('%Y-%m-%d %H:%M:%S')


def connect(path):
    if not path.exists():
        sys.exit(f'No database at {path}')
    db = sqlite3.connect(str(path), timeout=10)
    db.execute('''CREATE TABLE IF NOT EXISTS maintenance (
        id INTEGER PRIMARY KEY AUTOINCREMENT, start REAL NOT NULL, end REAL, note TEXT NOT NULL DEFAULT '')''')
    return db


def counts(db, start, end):
    """Rows in [start, end): wind samples, packets, rain tips."""
    return (db.execute('SELECT COUNT(*) FROM samples WHERE t >= ? AND t < ?', (start, end)).fetchone()[0],
            db.execute('SELECT COUNT(*) FROM packets WHERE t >= ? AND t < ?', (start, end)).fetchone()[0],
            db.execute('SELECT COALESCE(SUM(added),0) FROM rain WHERE t >= ? AND t < ?', (start, end)).fetchone()[0])


def list_windows(db):
    rows = db.execute('SELECT id, start, end, note FROM maintenance ORDER BY start').fetchall()
    if not rows:
        print('No maintenance windows.')
    for i, start, end, note in rows:
        samples, packets, tips = counts(db, start, end if end is not None else time.time() + 1)
        print(f'{i:4}  {show(start)} → {show(end)}  {samples} samples, {packets} packets, {tips} tips hidden'
              f'{"  · " + note if note else ""}')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--db', type=Path, default=DB)
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('list')
    p = sub.add_parser('start'); p.add_argument('--note', default='')
    sub.add_parser('stop')
    p = sub.add_parser('add'); p.add_argument('start', type=when); p.add_argument('end'); p.add_argument('--note', default='')
    p = sub.add_parser('end'); p.add_argument('id', type=int); p.add_argument('end', type=when)
    p = sub.add_parser('remove'); p.add_argument('id', type=int)
    p = sub.add_parser('purge'); p.add_argument('start', type=when); p.add_argument('end', type=when)
    p.add_argument('--yes', action='store_true', help='do not ask for confirmation')
    args = parser.parse_args(argv)
    db = connect(args.db)
    open_ = db.execute('SELECT id, start FROM maintenance WHERE end IS NULL').fetchone()

    if args.command == 'list':
        list_windows(db)
    elif args.command == 'start':
        if open_:
            sys.exit(f'Window {open_[0]} is already open since {show(open_[1])}.')
        db.execute('INSERT INTO maintenance(start, note) VALUES(?, ?)', (time.time(), args.note))
    elif args.command == 'stop':
        if not open_:
            sys.exit('No open maintenance window.')
        db.execute('UPDATE maintenance SET end=? WHERE id=?', (time.time(), open_[0]))
    elif args.command == 'add':
        end = None if args.end == 'open' else when(args.end)
        if end is None and open_:
            sys.exit(f'Window {open_[0]} is already open; only one can be.')
        if end is not None and end <= args.start:
            sys.exit('The end must be after the start.')
        db.execute('INSERT INTO maintenance(start, end, note) VALUES(?, ?, ?)', (args.start, end, args.note))
    elif args.command == 'end':
        row = db.execute('SELECT start FROM maintenance WHERE id=?', (args.id,)).fetchone()
        if not row:
            sys.exit(f'No window {args.id}.')
        if args.end <= row[0]:
            sys.exit('The end must be after the start.')
        db.execute('UPDATE maintenance SET end=? WHERE id=?', (args.end, args.id))
    elif args.command == 'remove':
        if not db.execute('DELETE FROM maintenance WHERE id=?', (args.id,)).rowcount:
            sys.exit(f'No window {args.id}.')
    elif args.command == 'purge':
        if args.end <= args.start:
            sys.exit('The end must be after the start.')
        samples, packets, tips = counts(db, args.start, args.end)
        print(f'{show(args.start)} → {show(args.end)}: {samples} samples, {packets} packets, {tips} rain tips')
        if not args.yes and input('Delete permanently (after a backup)? [y/N] ').strip().lower() != 'y':
            sys.exit('Nothing deleted.')
        backups = args.db.parent / 'backups'
        backups.mkdir(exist_ok=True)
        target = backups / f'{args.db.stem}-{dt.datetime.now():%Y%m%d-%H%M%S}.sqlite3'
        with sqlite3.connect(str(target)) as copy:
            db.backup(copy)  # consistent copy, including changes still in the WAL
        copy.close()
        for table in ('samples', 'packets', 'rain'):
            db.execute(f'DELETE FROM {table} WHERE t >= ? AND t < ?', (args.start, args.end))
        db.commit()
        print(f'Deleted. Backup: {target}')
        if packets and args.end > time.time() - 120:
            print('Note: the dashboard\'s live cards still show the last packet received until the next '
                  'one arrives. For recent handling, prefer a maintenance window, which also hides them.')
    db.commit()
    if args.command != 'list':
        list_windows(db)


if __name__ == '__main__':
    main()
