#!/usr/bin/env python3
"""Recover Klipper clipboard rows carved out of the SQLite WAL.

The `main` table is empty but the WAL still holds the deleted row records, so
the text can be salvaged. Writes to a NEW database; never touches the original.

Usage: recover_klipper.py <out.sqlite>
"""
import os
import re
import sqlite3
import sys

WAL = os.path.expanduser('~/.local/share/klipper/history3.sqlite-wal')

# A row's payload begins right after its mimetypes marker and runs to the next
# record boundary, which is a uuid followed by the 0x41 ('A') header byte.
# Built per marker: a payload can only be delimited by the marker it follows,
# so this must not be hardcoded to one mimetype or the other variants are lost.
RECORD_END = rb'(?=[0-9a-f]{40}A)'
MARKERS = [
    # Order matters: the html variant is listed first because it is a superset
    # of the plain one, and `found` keeps the first text seen for a uuid.
    b'text/plain,text/plain;charset=utf-8,text/html',
    b'text/plain,text/plain;charset=utf-8',
    b'text/markdown',
]


def payload_pattern(marker: bytes) -> re.Pattern:
    return re.compile(re.escape(marker) + rb'(.{0,4000}?)' + RECORD_END, re.S)


def clean(raw: bytes) -> str:
    """Trim trailing SQLite record/varint noise from a carved payload."""
    text = raw.decode('utf-8', 'replace')
    # Control bytes below 0x20 (except \n \r \t) are record structure, not text.
    out = []
    for ch in text:
        o = ord(ch)
        if o < 0x20 and ch not in '\n\r\t':
            break
        out.append(ch)
    text = ''.join(out).strip()
    # Drop a leading fragment such as ",text/markdown".
    text = re.sub(r'^[,;]?\s*text/[a-z+;\-=]*', '', text).strip()
    return text


def main() -> int:
    out_path = sys.argv[1]
    wal = open(WAL, 'rb').read()

    found: dict[str, tuple[str, str]] = {}
    for marker in MARKERS:
        pattern = payload_pattern(marker)
        for m in re.finditer(re.escape(marker), wal):
            seg = wal[m.end():m.end() + 4000]
            hit = pattern.match(marker + seg)
            if not hit:
                continue
            # The uuid sits a few bytes before the marker in the record header.
            head = wal[max(0, m.start() - 64):m.start()]
            ids = re.findall(rb'([0-9a-f]{40})', head)
            if not ids:
                continue
            uuid = ids[-1].decode()
            text = clean(hit.group(1))
            if text and len(text) > 1:
                found.setdefault(uuid, (text, marker.decode()))

    if os.path.exists(out_path):
        os.remove(out_path)
    con = sqlite3.connect(out_path)
    con.execute('CREATE TABLE main (uuid char(40) PRIMARY KEY, added_time REAL NOT NULL,'
                ' last_used_time REAL, mimetypes TEXT NOT NULL, text NTEXT, starred BOOLEAN)')
    con.execute('CREATE TABLE aux (uuid char(40) NOT NULL, mimetype TEXT NOT NULL,'
                ' data_uuid char(40) NOT NULL, PRIMARY KEY (uuid, mimetype))')
    con.execute('CREATE TABLE version (db_version INT NOT NULL)')
    con.execute('INSERT INTO version VALUES (3)')

    import time
    now = time.time()
    for i, (uuid, (text, mimetypes)) in enumerate(found.items()):
        con.execute('INSERT OR REPLACE INTO main VALUES (?,?,?,?,?,?)',
                    (uuid, now - i, now - i, mimetypes, text, 0))
    con.commit()
    n = con.execute('SELECT COUNT(*) FROM main').fetchone()[0]
    con.close()
    print(f'recovered {n} entries -> {out_path}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())