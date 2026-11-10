# Custom Clipboard

**A faster clipboard for KDE Plasma.** Press **Meta+V**, type to search
everything you've copied, and press **Enter** — or just click an item — to
paste it instantly.

![Custom Clipboard showing the history panel](docs/preview.png)

For Plasma users who copy things all day: developers, writers, researchers.
It reads your existing Klipper history, so nothing changes about how you copy —
only how you find and reuse it. Not for GNOME, macOS, or Windows.

## Features

- **Search** text and image history as you type
- **Star** frequent clips into their own tab
- **Edit** an entry inline before pasting
- **Trash** — delete moves entries to a restoreable trash, never destroys them
- **Click-to-paste** on Wayland (via `ydotool`)
- Keyboard navigation, compact Plasma-styled panel, fully local — no network

## Install

```bash
git clone <repo-url> && cd plasma-clip
./install.sh
# log out and back in, then press Meta+V
```

Needs KDE Plasma with Klipper, Qt 6, KF6 GlobalAccel, Python 3.10+.
Arch/CachyOS: `sudo pacman -S base-devel pkgconf qt6-base kglobalaccel python`
Debian/Ubuntu: `sudo apt install g++ pkg-config python3 qt6-base-dev libkf6globalaccel-dev`
Other distros may need small installer changes — see below.

Click-to-paste needs `ydotool` installed and `ydotool.service` running; without
it, copying still works and you paste manually.

## Use

| Action | How |
|---|---|
| Open / close | **Meta+V** |
| Search, navigate | Type, then **↑** / **↓** / **Enter** |
| Star / edit | Hover a row → **★** / **✎** |
| Delete safely | **✕** or right-click → **Move to trash** |
| Restore | **Trash** tab → **↩** |

Preferences: `~/.config/custom-clipboard/custom-clipboard.conf`
(`autoPaste`, `pasteDelayMs`).

## Uninstall

```bash
./uninstall.sh
```

Removes the app and restores your original shortcuts. Leaves clipboard data
untouched.

## For AI coding agents

Hand this repo to an agent and say **"set up plasma-clip"** — everything it
needs is here:

- [DEVELOPMENT.md](DEVELOPMENT.md) — build, install behavior, verification
- [AGENTS.md](AGENTS.md) — data-safety rules (never test against real clipboard
  data, never inject input on the live desktop)
- `tools/shortcuts.py` — shortcut backup/restore, fixture-tested
- `tools/recover-klipper-history.py` — recovers deleted Klipper history

If the target distro differs, the agent should adapt package names and paths,
verify with synthetic fixtures only, and report what it could not verify.

## For humans

[DEVELOPMENT.md](DEVELOPMENT.md) covers building from source and the safe test
workflow. Screenshots in `docs/` use synthetic clipboard content.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
