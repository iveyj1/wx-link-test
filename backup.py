#!/usr/bin/env python3
"""Nightly copy of the history database: ./backup.py [--db PATH] [--keep 14]

Uses SQLite's backup API, so the copy is consistent while web_server.py is writing (a plain file
copy would miss recent writes held in the -wal file). Copies go to data/backups/nightly/ and the
oldest beyond --keep are deleted. Run by deploy/anemometer-backup.timer.
"""
import argparse
import datetime as dt
import sqlite3
from pathlib import Path

ROOT = Path(__file__).parent


def backup(db_path, keep, now=None):
    folder = db_path.parent / 'backups' / 'nightly'
    folder.mkdir(parents=True, exist_ok=True)
    target = folder / f'{db_path.stem}-{(now or dt.datetime.now()):%Y%m%d}.sqlite3'
    source = sqlite3.connect(f'file:{db_path}?mode=ro', uri=True, timeout=30)
    try:
        copy = sqlite3.connect(str(target))
        try:
            source.backup(copy)
            if copy.execute('PRAGMA integrity_check').fetchone()[0] != 'ok':
                raise SystemExit(f'Backup failed its integrity check: {target}')
        finally:
            copy.close()
    finally:
        source.close()
    for old in sorted(folder.glob(f'{db_path.stem}-*.sqlite3'))[:-keep]:
        old.unlink()
    return target


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--db', type=Path, default=ROOT / 'data' / 'wind.sqlite3')
    parser.add_argument('--keep', type=int, default=14)
    args = parser.parse_args()
    if not args.db.exists():
        raise SystemExit(f'No database at {args.db}')
    print(f'Backup written: {backup(args.db, max(1, args.keep))}')
