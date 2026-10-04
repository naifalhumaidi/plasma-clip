# Changelog

All notable changes to this project are documented here.
Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [2.1.0] — Unreleased

### Added
- Click-to-paste: selecting an entry pastes it into the previously focused input.
- Unlimited history: reads Klipper's SQLite database directly, so it is not
  bound by Klipper's in-memory cap.
- Search box, pinned to the top of the popup.
- **All** / **Starred** tabs. The Starred tab only appears when something is
  starred.
- Sorting: Newest, Oldest, A–Z, Z–A.
- Relative timestamps per row ("just now", "19 min ago", "1 h ago").
- Image thumbnails, including copied image *files* (`file://` URIs) which Klipper
  records as text.
- Per-row star and remove buttons, revealed on hover.
- Right-click menu: Star/Unstar, Copy only, Delete entry.
- **＋ New** — add an entry to the clipboard from an inline field.
- **Clear** — remove every unstarred entry, with an in-window confirmation that
  reports both counts.
- Headless `--test` suite (41 checks) and `--preview` / `--screenshot` renderers
  for inspecting the UI without a display.
- Opt-in click tracing via `CUSTOM_CLIPBOARD_DEBUG=1`.

### Changed
- Popup restyled after the Windows 11 clipboard panel, themed with Plasma
  colours: 400px wide, rounded panel, 28px action buttons.
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