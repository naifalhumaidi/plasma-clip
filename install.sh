#!/usr/bin/env bash
# Install custom-clipboard for the current user.
#   - builds the binary
#   - installs to ~/.local/bin
#   - registers autostart
#   - enables the ydotool helper service (needed for click-to-paste)
#   - moves Klipper's own Meta+V binding aside, since we take that key
#
# No root required. Nothing outside ~ is touched.
set -euo pipefail

SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
BIN="$HOME/.local/bin/custom-clipboard"
AUTOSTART="$HOME/.config/autostart/custom-clipboard.desktop"

echo "==> building"
"$SRC_DIR/build.sh"

echo "==> installing binary"
mkdir -p "$HOME/.local/bin"
install -m 0755 "$SRC_DIR/custom-clipboard" "$BIN"

echo "==> installing autostart entry"
mkdir -p "$HOME/.config/autostart"
cat > "$AUTOSTART" <<EOF
[Desktop Entry]
Type=Application
Name=Custom Clipboard
Comment=Clipboard history popup that pastes on click
Exec=$BIN
Icon=edit-paste
Terminal=false
Categories=Utility;
X-KDE-autostart-phase=2
X-KDE-StartupNotify=false
EOF

echo "==> ydotool (required for click-to-paste)"
if ! command -v ydotool >/dev/null 2>&1; then
    echo "    ydotool is NOT installed. Install it with:"
    echo "      sudo pacman -S ydotool        # Arch / CachyOS"
    echo "      sudo apt install ydotool       # Debian/Ubuntu"
else
    # `enable --now` reports success even when the daemon dies instantly: if
    # /dev/uinput is not writable, ydotoold exits and systemd hits its start
    # limit, leaving the unit "enabled" but dead. Verify it really answers.
    systemctl --user enable ydotool.service 2>/dev/null || true
    systemctl --user reset-failed ydotool.service 2>/dev/null || true
    systemctl --user start ydotool.service 2>/dev/null || true
    if ydotool key 0:0 2>/dev/null; then
        echo "    ydotool.service running — click-to-paste will work"
    else
        echo "    WARNING: ydotoold is NOT working, so clicks will copy but not paste."
        echo "      Check: systemctl --user status ydotool.service"
        echo "      If it says 'failed to open uinput device', your user needs"
        echo "      access to /dev/uinput, e.g.:"
        echo "        sudo usermod -aG input $USER   (then log out and back in)"
    fi
fi

echo "==> releasing Klipper's Meta+V binding"
# Klipper ships Meta+V for its own popup and lives inside plasmashell, which
# re-registers that key from memory. D-Bus calls (setShortcut / unRegister)
# report success but are immediately overwritten, so the only thing that works
# is editing kglobalshortcutsrc directly. plasmashell picks it up on next login.
python3 - "$HOME/.config/kglobalshortcutsrc" <<'PY'
import os, re, shutil, sys
path = sys.argv[1]
if not os.path.exists(path):
    print("    no kglobalshortcutsrc yet (first login?) — nothing to do")
    raise SystemExit(0)
shutil.copy2(path, path + ".bak-custom-clipboard")
text = open(path).read()
new = re.sub(r'(?m)^show-on-mouse-pos=[^,]+,[^,]+,', 'show-on-mouse-pos=none,none,', text)
if new != text:
    open(path, "w").write(new)
    print("    Klipper Meta+V released (backup: kglobalshortcutsrc.bak-custom-clipboard)")
else:
    print("    already released, nothing to change")
PY

echo
echo "Installed."
echo "  binary    : $BIN"
echo "  autostart : $AUTOSTART"
echo "  shortcut  : Meta+V   (Win+V)"
echo
if pgrep -x plasmashell >/dev/null 2>&1; then
    echo "NOTE: Klipper still holds Meta+V until the shell reloads it."
    echo "      Log out and back in, or run:"
    echo "        systemctl --user restart plasma-plasmashell.service"
    echo "      (that briefly removes the panel, so do it when convenient)"
fi