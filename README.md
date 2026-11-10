# Custom Clipboard

**A fast clipboard-history panel for KDE Plasma.** Press **Meta+V**, type to
search your recent copies, and press **Enter** to paste — all without opening a
browser tab or digging through Klipper's popup.

![Custom Clipboard showing the history panel](docs/preview.png)

## Who this is for

This app is for **KDE Plasma users who copy things constantly** — developers,
writers, researchers, anyone juggling text and images all day. It replaces the
stock Klipper popup with a panel built for speed:

- **Search first.** Filter hundreds of clips by typing; Klipper's default
  history is a flat list.
- **Star what matters.** Keep frequently used snippets in their own tab.
- **Edit before you paste.** Fix a URL or command inline instead of recopying.
- **Delete without fear.** ✕ moves entries to a local Trash you can restore
  from, instead of destroying them.
- **Click-to-paste on Wayland.** Select an item and it types itself into the
  window you were just using.

If you use GNOME, macOS, or Windows, this won't run — it is built specifically
on Klipper's database and KDE's global shortcuts. If you live in Plasma and
wish your clipboard worked like this, it is for you.

## Quick start

Clone this repository, then:

```bash
cd plasma-clip
./install.sh
# log out and back in, then press Meta+V
```

Requirements: KDE Plasma with Klipper, Qt 6, KF6 GlobalAccel, Python 3.10+.
Arch/CachyOS and Debian/Ubuntu package examples are below. See
[DEVELOPMENT.md](DEVELOPMENT.md) if the installer needs adapting to your
distribution.

## What it does

- Reads text and image history from Klipper's local SQLite database.
- Searches, sorts, stars, edits, and restores entries.
- Keeps removed entries and their image payloads in a local Trash until cleared.
- Uses Klipper's D-Bus interface to set text, and `ydotool` for optional
  click-to-paste input on Wayland.
- Includes keyboard navigation and a compact, Plasma-themed interface.

The app has no network feature. Clipboard contents remain local in Klipper's
database; the app's Trash is stored separately at
`~/.local/share/custom-clipboard/trash.json`.

## Implementation

The panel is a C++17 / Qt 6 desktop app with KF6 GlobalAccel. It reads Klipper's
SQLite history, calls Klipper over D-Bus when setting copied text, and uses
`ydotool` only for optional key injection. The Trash sidecar keeps deleted rows
restorable without adding them back to Klipper's visible history. The interface
and database operations are kept in one translation unit to make the project
straightforward to build from source.

## Requirements

The install scripts target **KDE Plasma with Klipper** and a working
`systemd --user` session. Building requires Qt 6 Widgets, D-Bus and SQL modules,
KF6 GlobalAccel headers, `pkg-config`, a C++17 compiler, Python 3.10+, and Bash.

For Arch/CachyOS, the common packages are:

```bash
sudo pacman -S base-devel pkgconf qt6-base kglobalaccel python
```

For Debian/Ubuntu releases that provide these packages:

```bash
sudo apt install g++ pkg-config python3 qt6-base-dev libkf6globalaccel-dev
```

Package names and availability vary by release; this repository has not been
validated across all distributions.

### Portability

This is a KDE Plasma application, not an Arch-only application. In principle,
it can run on another Linux distribution that provides Plasma, Klipper, Qt 6,
KF6 GlobalAccel, and the required user-session services. The current installer
assumes a Bash/Python environment, `systemd --user`, and Plasma's shortcut and
autostart conventions, so another distribution or Plasma setup may need small
changes. Click-to-paste also depends on `ydotool` and permission to use
`/dev/uinput`; without that, users can still copy an item and paste it manually.

If it does not work out of the box, use this repository as a starting point for
a machine-specific setup. An AI coding agent can inspect the installed Plasma,
Klipper, package names, header locations, and user-service setup, then adapt the
build or installer for that system. It should clearly report what it changed
and what it verified, and keep the clipboard database and shortcut safeguards
in [DEVELOPMENT.md](DEVELOPMENT.md). Compatibility with a distribution should
only be claimed after testing there.

`ydotool` is optional for browsing and copying. Click-to-paste needs the client,
its user service, and permission to access `/dev/uinput`. The installer can
enable an already-installed `ydotool.service`; it does not install packages or
change device permissions for you.

Install it separately when available: `sudo pacman -S ydotool` on Arch-based
systems, or the distribution's `ydotool` package on Debian/Ubuntu releases that
provide it.

## Install

From the repository directory:

```bash
./install.sh
```

The script builds the app, installs it to `~/.local/bin`, writes a KDE autostart
entry, and configures a three-second restart policy for unexpected exits. It
backs up `~/.config/kglobalshortcutsrc` once, then disables Klipper's Meta+V and
the older `clipdeck` shortcut. **Log out and back in** for Plasma to reload the
shortcut and autostart settings.

If KF6 GlobalAccel headers are outside `/usr/include/KF6/KGlobalAccel`, set
`KF6_GLOBALACCEL_INCLUDE_DIR` to the directory containing `kglobalaccel.h`
before running the installer.

Check whether click-to-paste is available:

```bash
systemctl --user is-active ydotool.service
```

The app reports when `ydotoold` is unavailable. It can still copy the selected
entry, after which you can paste manually.

## Use

| Action | How |
|---|---|
| Open or close | **Meta+V** |
| Search and navigate | Type, then use **↑** / **↓** and **Enter** |
| Star or edit | Hover a row, then choose **★** or **✎** |
| Remove safely | **✕** or right-click → **Move to trash** |
| Restore | Open **Trash**, then choose **↩** or **★** |
| Clear unstarred entries | **Clear**, then confirm; entries move to Trash |
| Permanently empty Trash | **Clear Trash**, then confirm |

The Starred, Not starred, and Trash tabs appear only when they contain entries.
Starred clips keep their normal sort order in All.

### Preferences

Qt stores preferences in
`~/.config/custom-clipboard/custom-clipboard.conf`. Create or edit it to change
paste behavior:

```ini
[General]
autoPaste=true
pasteDelayMs=200
```

Set `autoPaste=false` to copy without injecting Ctrl+V. Increase `pasteDelayMs`
if the previous window needs more time to regain focus.

## Data and privacy

Klipper keeps history in `~/.local/share/klipper/history3.sqlite` and image
payloads under `~/.local/share/klipper/data/`. Custom Clipboard reads that
history and updates it for starring, editing, and restoring. Trash entries are
stored in a JSON sidecar so their text can be restored; image payloads are kept
until Trash is emptied. Treat both locations as clipboard data.

## Uninstall

```bash
./uninstall.sh
```

This stops the app, removes its binary, autostart entry and settings, and
restores the original Klipper and legacy shortcut values from the saved
shortcut backup. It leaves the backup and clipboard Trash in place. It also
leaves `ydotool.service` enabled because another app may use it; disable it
manually if you no longer need it:

```bash
systemctl --user disable --now ydotool.service
```

## For contributors

See [DEVELOPMENT.md](DEVELOPMENT.md) for the build and safe verification workflow,
and [AGENTS.md](AGENTS.md) for implementation and data-safety rules. Screenshots
in `docs/` use synthetic clipboard content.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
