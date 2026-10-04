# AGENTS.md

Hard-won rules for AI agents (or anyone) editing this codebase. Every item here
corresponds to a bug that actually happened and cost real debugging time.

## Never do these

### 1. Never inject input on the user's live desktop

**Never run `ydotool`, `wtype`, `xdotool` or similar against the real session.**
These write to `/dev/uinput` — the kernel input layer — so the keys and clicks go
to whatever the user is actually doing. You will hijack their mouse and keyboard
mid-work, on their actual screen, with no visible sign of why.

If you have done this by mistake, say so plainly rather than quietly moving on.

To test interaction, deliver events straight to the widget instead:

```cpp
QMouseEvent press(QEvent::MouseButtonPress, pos, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
QApplication::sendEvent(view->viewport(), &press);
```

This exercises identical hit-testing with zero OS involvement. All the
star/remove/gutter tests work this way.

### 2. Never restart `plasmashell` to reload configuration

It is a **disabled** systemd unit. Killing it does *not* bring it back — you must
start it manually, and the user's panel is gone in the meantime. This has already
happened once here.

Ask the user first, every time. Prefer having them log out and back in.

### 3. Never commit the compiled binary

`custom-clipboard` in the repo root is a build artifact and is gitignored.

## Verify, don't assume

### Shortcuts: register with `NoAutoloading`

```cpp
KGlobalAccel::self()->setShortcut(action, keys, KGlobalAccel::NoAutoloading);
```

The default (`Autoloading`) re-reads whatever is in `kglobalshortcutsrc` and
silently overrides the key you passed. Symptom: source and config both look
correct, yet the key is grabbed by something else.

Then confirm the key is *actually* held — the config file is not evidence:

```bash
python3 - <<'PY'
import dbus
b = dbus.SessionBus()
i = dbus.Interface(b.get_object("org.kde.kglobalaccel", "/kglobalaccel"),
                   "org.kde.KGlobalAccel")
for e in i.getGlobalShortcutsByKey(0x10000000 | ord('V')):
    print("Meta+V owned by:", e[2])
PY
```

`0 owners` means nothing holds it. Two owners means a conflict that editing the
config file cannot fix until the owning shell restarts.

### `QString::arg()` does not substitute `?`

`arg()` only replaces `%1`-style placeholders. This silently breaks queries:

```cpp
// BROKEN — SQLite receives a literal "?" and fails with no error
q.exec(QStringLiteral("UPDATE main SET starred=? WHERE uuid=?").arg(1).arg(uuid));

// CORRECT
q.prepare(QStringLiteral("UPDATE main SET starred=? WHERE uuid=?"));
q.addBindValue(1);
q.addBindValue(uuid);
```

This bug made every star click fail silently while the code looked perfect.

### Frameless always-on-top popups render dialogs behind themselves

`Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint` will draw
`QMenu` and `QMessageBox` *underneath* the window. They are not missing — they
are behind. This is why "New" is an inline field and the clear confirmation is an
in-window overlay rather than a message box.

### Shadowing a member with a constructor local

```cpp
Deck::Deck() {
    auto *root = new QWidget(this);   // local — the member stays null
}
```

Every other method then dereferences a null member. This caused a segfault in the
confirm popup that a null-check regression test now guards.

## Design rules

### Painting and hit-testing must share one geometry function

Never compute a button rect twice. `starButtonRect()` and `removeButtonRect()`
are called by both the delegate's `paint()` and the mouse handler.

### Never hardcode a width in the delegate's `sizeHint`

```cpp
QSize sizeHint(...) override { return QSize(0, ROW_HEIGHT); }   // correct
```

Returning `QSize(POPUP_WIDTH, ROW_HEIGHT)` makes item rects wider than the
viewport. Rows are then *painted* at one width and *hit-tested* at another, so
buttons appear where they are not clickable. This caused a long, entirely
misleading "the button doesn't work" bug.

### Klipper's data layout

- History: `~/.local/share/klipper/history3.sqlite`, tables `main`, `aux`.
- Image blobs: `data/<item-uuid>/<data-uuid>` — a **directory per item**, not a
  flat file. Guessing `data/<data-uuid>` yields blank thumbnails.
- Image filename: stored separately under `application/x-kde-suggestedfilename`.
- Copied files arrive as `text/uri-list` entries whose text is a `file://` URL.
  Detect image extensions to show a thumbnail.
- `getClipboardHistoryItem` stops at the first empty-text entry, so it cannot
  count items. Query SQLite.

### Database rules

- Open **read-only** (`QSQLITE_OPEN_READONLY`) and set `PRAGMA busy_timeout`, so
  you never block Klipper mid-write.
- Never assert against the user's live history in tests. Use
  `CUSTOM_CLIPBOARD_DATA` to point at a throwaway copy.
- Assert behaviour with a **synthetic fixture**. Tests that depend on what the
  user happens to have copied will fail the moment their data changes.

### Starred entries are not pinned

They have their own tab. Within "All", sorting must ignore `starred` entirely.
There is a fixture-based test for this; keep it that way.

## Before you claim something works

```bash
./build.sh
QT_QPA_PLATFORM=offscreen ./custom-clipboard --test          # must be ALL TESTS PASSED
QT_QPA_PLATFORM=offscreen ./custom-clipboard --preview /tmp/s
```

Then **look at the PNGs**. Rendering a view and reading it catches layout bugs
that code review cannot: overlapping text, controls centred against the wrong
widget, a stray frame line. Three bugs were found this way.

Report honestly what is verified and what is not. Interaction that depends on a
compositor handing focus back to another window cannot be verified headlessly —
say so instead of implying it works.