#!/usr/bin/env bash
# Regenerate the screenshots in docs/ from SYNTHETIC clipboard data.
#
# The real clipboard history contains private content — file paths, localhost
# URLs, whatever the user happened to copy. Rendering docs from it would publish
# that. So we build a throwaway fixture database instead and point the app at it
# with CUSTOM_CLIPBOARD_DATA.
#
# Usage:  ./docs/make-screenshots.sh
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
BIN="$ROOT/custom-clipboard"
OUT="$ROOT/docs"

[ -x "$BIN" ] || "$ROOT/build.sh"

FIXTURE="$(mktemp -d)"
trap 'rm -rf "$FIXTURE"' EXIT

python3 - "$FIXTURE" <<'PY'
import json, sqlite3, struct, sys, time, zlib, pathlib

root = pathlib.Path(sys.argv[1])
db = root / "history3.sqlite"
con = sqlite3.connect(db)
con.executescript("""
CREATE TABLE main (uuid char(40) PRIMARY KEY, added_time REAL NOT NULL,
    last_used_time REAL, mimetypes TEXT NOT NULL, text NTEXT, starred BOOLEAN);
CREATE TABLE aux (uuid char(40) NOT NULL, mimetype TEXT NOT NULL,
    data_uuid char(40) NOT NULL, PRIMARY KEY (uuid, mimetype));
CREATE TABLE version (db_version INT NOT NULL);
INSERT INTO version VALUES (4);
""")

now = int(time.time())
rows = [
    ("https://example.com/docs/getting-started", 30,   1),
    ("Deploy the release branch and tag the version", 240, 1),
    ('git commit -m "fix: handle empty clipboard payload"', 600, 0),
    ("SELECT id, title FROM articles WHERE published = 1 ORDER BY created_at DESC LIMIT 10;", 3600, 0),
    ("Meeting notes: ship the installer, then write the docs", 7200, 1),
    ("https://example.com/issues/4821", 10800, 0),
    ("Remember to rotate the signing keys before Friday", 86400, 1),
    ("const ratio = (width / height).toFixed(2);", 172800, 0),
    ("example@example.com", 259200, 0),
    ("A longer paragraph of sample text so that the eliding behaviour is visible in the screenshot, and the timestamp column is exercised as well.", 345600, 0),
]
for i, (text, ago, starred) in enumerate(rows, start=1):
    uuid = str(i) * 40
    con.execute("INSERT INTO main VALUES (?,?,?,?,?,?)",
                (uuid, now - ago, None, "text/plain", text, starred))

# One real image entry so the thumbnail path is shown working.
img_uuid = "f" * 40
data_uuid = "e" * 40
name_uuid = "d" * 40
con.execute("INSERT INTO main VALUES (?,?,?,?,?,?)",
            (img_uuid, now - 1800, None,
             "image/png,application/x-kde-suggestedfilename", "", 0))
con.execute("INSERT INTO aux VALUES (?,?,?)", (img_uuid, "image/png", data_uuid))
con.execute("INSERT INTO aux VALUES (?,?,?)",
            (img_uuid, "application/x-kde-suggestedfilename", name_uuid))

d = root / "data" / img_uuid
d.mkdir(parents=True, exist_ok=True)
(d / name_uuid).write_text("diagram-export.png")

w = h = 96
raw = b"".join(b"\x00" + bytes(v for x in range(w)
                               for v in ((x * 255) // w, (y * 255) // h, 200))
               for y in range(h))

def chunk(tag, data):
    c = tag + data
    return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)

png = (b"\x89PNG\r\n\x1a\n"
       + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
       + chunk(b"IDAT", zlib.compress(raw))
       + chunk(b"IEND", b""))
(d / data_uuid).write_bytes(png)
con.commit()
con.close()

# A small trash sidecar too, so the Trash tab has something to show.
# Same shape saveTrash() writes: a JSON array of row objects. The uuids
# are deliberately not in main — a trashed row is not a history row.
trash = [
    {"uuid": letter * 40, "text": text, "mimetypes": "text/plain",
     "added": now - ago, "last": now - ago, "starred": 0}
    for letter, (text, ago, starred) in zip("abc", rows[5:8])
]
(root / "trash.json").write_text(json.dumps(trash))
print(f"fixture: {len(rows) + 1} entries, 1 image, {len(trash)} trashed")
PY

export CUSTOM_CLIPBOARD_DATA="$FIXTURE"
export CUSTOM_CLIPBOARD_TRASH="$FIXTURE/trash.json"
export QT_QPA_PLATFORM=offscreen

echo "==> rendering docs screenshots from synthetic data"
"$BIN" --preview "$OUT" >/dev/null
"$BIN" --screenshot "$OUT/preview-hover.png"   --state hover   >/dev/null
"$BIN" --screenshot "$OUT/preview-confirm.png" --state confirm >/dev/null

# --preview writes all/starred/notstarred/search/alpha/trash/new.png;
# keep only the ones the docs reference.
mv -f "$OUT/all.png"          "$OUT/preview.png"
mv -f "$OUT/starred.png"      "$OUT/preview-starred.png"
mv -f "$OUT/notstarred.png"   "$OUT/preview-notstarred.png"
mv -f "$OUT/trash.png"        "$OUT/preview-trash.png"
rm -f "$OUT/search.png" "$OUT/alpha.png" "$OUT/new.png"

echo "done:"
ls -1 "$OUT"/*.png