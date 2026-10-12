# Changelog

All notable changes to this project are documented here.
Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [2.1.0] — Unreleased

### Added
- Click-to-paste: selecting an entry pastes it into the previously focused input.
- Close by clicking outside the panel, without stealing the click.
- Unlimited history: reads Klipper's SQLite database directly, so it is not
  bound by Klipper's in-memory cap.
- Search box, pinned to the top of the popup.
- **All** / **Starred** tabs. The Starred tab only appears when something is
  starred.
- **Not starred** tab — the complement of Starred, so you can act on everything
  that is *not* protected. Appears only when unstarred entries exist.
- **Trash** tab with restorable deletes. ✕ moves the entry into a JSON sidecar
  (`~/.local/share/custom-clipboard/trash.json`) instead of destroying it; on
  the Trash tab the buttons become **↩ restore** and **restore + star**, a body
  click never pastes, and **Clear Trash** (red, its own confirmation) empties
  the sidecar. Image payloads survive until then, so restored thumbnails work.
- Sorting: Newest, Oldest, A–Z, Z–A.
- Relative timestamps per row ("just now", "19 min ago", "1 h ago").
- Image thumbnails, including copied image *files* (`file://` URIs) which Klipper
  records as text.
- Per-row star and remove buttons, revealed on hover.
- Right-click menu: Star/Unstar, Copy only, Move to trash (Restore / Restore and
  star / Copy only on the Trash tab).
- **＋ New** — add an entry to the clipboard from an inline field.
- Keyboard navigation from the search box: ↑/↓ walk the list, `Enter` pastes the
  selected row.
- **Clear** — remove every unstarred entry, with an in-window confirmation that
  reports both counts.
- Headless `--test` suite and `--preview` / `--screenshot` renderers
  for inspecting the UI without a display.
- Opt-in click tracing via `CUSTOM_CLIPBOARD_DEBUG=1`.
- A clear message when a click cannot paste because `ydotoold` is not running,
  instead of closing silently.

### Changed
- Popup restyled after the Windows 11 clipboard panel, themed with Plasma
  colours: 400px wide, rounded panel, 28px action buttons.
- The status line floats over the list instead of sitting below it, so showing a
  message no longer resizes the list and shifts the rows.
- The per-row ✕ and the right-click *Delete entry* no longer delete outright;
  they move the entry to the Trash tab (renamed *Move to trash*).

### Fixed
- The row ✕ button destroyed entries instead of moving them to the **Trash**
  tab: it called the hard delete while the right-click action trashed. Entries
  vanished with an empty trash as the only trace, including ones copied seconds
  earlier. The suite did not catch it because every check only asserted "gone
  from the history", which a hard delete also satisfies — the check now asserts
  where the row went, and fails on the old code.
- The list could not be walked with the keyboard: the panel opens with the caret
  in the search box, where ↑/↓ and `Enter` did nothing, so entries were reachable
  with the mouse only. ↑/↓ now move the selection (clamped, scrolling it into
  view) and `Enter` pastes the selected row — from the search box, in every tab.
  `Enter` in the trash still never pastes.
- Click-to-paste stayed broken for the rest of the session whenever `ydotoold`
  died, which it does whenever it cannot open `/dev/uinput` — systemd hits the
  unit's start limit and nothing retries. Every click then reported success
  while typing nothing. The app now restarts the daemon itself: at startup, on
  the click that finds it dead (which then asks you to click again), and again
  inside the paste injection if the daemon died between the click and the key.
  Attempts are rate limited to one per 30 s, so a permanently unfixable daemon
  does not bury the message explaining it.
- Meta+V did not reopen the panel for a while after closing it with an outside
  click. The window's `isVisible()` lags the asynchronous Wayland hide, so the
  toggle thought the panel was still open and hid it again. The intent is now
  tracked explicitly (`m_wantVisible`) and outside-click dismissal arms on
  window activation instead of a timer.
- Clicking a row no longer closes the panel without pasting when the click
  deactivated the window. Dismissal on an outside click now checks that the
  pointer is genuinely off the panel, and ignores deactivations caused by the
  panel's own widgets, context menus and the confirmation card.
- Double-clicking ✕ no longer pastes an entry. Qt delivers the second press of a
  double-click as `MouseButtonDblClick`, which the button hit-test ignored, so
  the click fell through to the list and pasted.
- A click no longer reports success while silently doing nothing when `ydotoold`
  has died. The daemon is now probed rather than assumed.
- `install.sh` verifies `ydotoold` actually responds, since `enable --now`
  reports success even when the daemon dies on startup.
- The test suite can no longer reach the real clipboard database. It previously
  could, and deleted real entries.
- Starred entries are no longer pinned to the top of the All tab; they sort
  normally and live in their own tab.
- Removed Klipper's Meta+V binding so this app can own it; Klipper moves to
  Meta+Shift+V.

### Fixed
- Star/unstar silently failed: `QString::arg()` was used on a query with `?`
  placeholders, so SQLite received a literal `?`.
- Image thumbnails were blank: Klipper stores blobs at `data/<item>/<data>`, not
  `data/<data>`.
- Row hover buttons were painted but had no click handler at all.
- Button hit areas did not match where buttons were painted, because the
  delegate forced a fixed `sizeHint` width wider than the viewport.
- `QMenu` and `QMessageBox` opened *behind* the always-on-top frameless popup;
  both are now in-window.
- A null `root` member (shadowed by a constructor local) crashed the confirm
  dialog.

[2.1.0]: https://github.com/OWNER/custom-clipboard/releases