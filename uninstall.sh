#!/usr/bin/env bash
# Remove custom-clipboard and restore the original shortcut values.
set -euo pipefail

CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
AUTOSTART_UNIT='app-custom\x2dclipboard@autostart.service'

echo "==> stopping app"
systemctl --user stop "$AUTOSTART_UNIT" 2>/dev/null || true
pkill -f "$HOME/.local/bin/custom-clipboard" 2>/dev/null || true

echo "==> removing files"
rm -f "$HOME/.local/bin/custom-clipboard"
rm -f "$CONFIG_HOME/autostart/custom-clipboard.desktop"
rm -f "$CONFIG_HOME/custom-clipboard/custom-clipboard.conf"
rmdir "$CONFIG_HOME/custom-clipboard" 2>/dev/null || true

rm -f "$CONFIG_HOME/systemd/user/${AUTOSTART_UNIT}.d/override.conf"
systemctl --user daemon-reload

echo "==> restoring original shortcut values"
python3 "$(dirname "$0")/tools/shortcuts.py" restore \
    "$CONFIG_HOME/kglobalshortcutsrc" \
    "$CONFIG_HOME/kglobalshortcutsrc.bak-custom-clipboard"

cat <<'EOF'

Note: ydotool.service was left enabled for other applications.
To remove it too:  systemctl --user disable --now ydotool.service

The source repository remains where you unpacked it.
EOF
