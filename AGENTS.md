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

### 4. Never let a test touch the real clipboard database

This happened here, and it destroyed the maintainer's clipboard history.

`CUSTOM_CLIPBOARD_DATA` was set only around the write-path block and then unset,
so every check after it ran against the real Klipper database. A regression test
that clicks the ✕ button deleted real entries on every run.

- Redirect the data directory **for the entire suite**, before anything reads it.
- Resolve the real database path *before* redirecting. Resolving it afterwards
  makes "real" point at the sandbox, so the guard passes for the wrong reason.
- Assert the real row count is unchanged at the end, reading the real file
  directly — `readHistory()` cannot see past the sandbox.
- Fail loudly when the real history is empty; otherwise the read and write
  checks pass *vacuously* rather than meaningfully.
- Never assert a count. Assert an outcome.

Recovery, if it ever happens: deleted rows survive in `history3.sqlite-wal`, and
`tools/recover-klipper-history.py` carves them out. Back the database up first.

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

### Reproduce the bug before you theorise about it

Two bugs here were explained confidently and wrongly before being reproduced.
The symptom was "clicking ✕ twice pastes an entry"; the plausible theory was
that the button was not yet drawn because it appears on hover. The real cause
was that Qt delivers the second press of a double-click as
`MouseButtonDblClick`, not `MouseButtonPress`, so the hit-test ignored it.

A synthesised double-click must send the same event types the real one does:

```cpp
QMouseEvent press(QEvent::MouseButtonPress, p, g, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
QMouseEvent dbl(QEvent::MouseButtonDblClick, p, g, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
```

Two plain presses exercise a different path and hide the bug entirely.

Then prove the fix: revert the one-line change and confirm the new test fails.
A test that has never failed is not evidence.

### Anything the OS can veto must be probed, not assumed

The client binary being installed proves nothing: `ydotool` needs a running
`ydotoold`, which dies on its own if it cannot open `/dev/uinput`. Assuming it
works turned a dead daemon into "clicking an item is broken".

```cpp
// Bad: the binary exists, so paste "works".
haveYdotool();

// Good: the daemon answers.
ydotoolReady(); // probes with a no-op key command, cached ~2s
```

Note the cache: probing in a click path stalls the UI, so never run a blocking
process wait per click. Do not guess socket paths either —
`QStandardPaths::RuntimeLocation` resolves differently under the offscreen
platform the tests use, so a path check is wrong in exactly the place you test.

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

### Dismissal must never fire on the panel's own interaction

A `Qt::Tool` window gets no pointer grab on Wayland, so "clicked outside" can
only be inferred from losing activation. But the panel's own widgets deactivate
it too — a row click, a context menu opening, the confirmation card. Hiding on
any deactivation closes the panel on the mouse *press*, so the row never
receives its release, `itemClicked` never fires, and the click pastes nothing.

So a deactivation only means "outside" when the pointer is genuinely off the
panel and no popup of ours owns the interaction. Keep that check in one place.

### Never let a status message resize the list

The status line used to be a layout row, so showing it shifted every row and a
second click landed on a different entry. Transient UI floats over the content
it reports on, and is `WA_TransparentForMouseEvents` so it cannot eat clicks.

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