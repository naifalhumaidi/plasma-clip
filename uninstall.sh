#!/usr/bin/env bash
# Remove custom-clipboard and restore Klipper's original Meta+V binding.
set -euo pipefail

echo "==> stopping app"
pkill -f "$HOME/.local/bin/custom-clipboard" 2>/dev/null || true

echo "==> removing files"
rm -f "$HOME/.local/bin/custom-clipboard"
rm -f "$HOME/.config/autostart/custom-clipboard.desktop"
rm -f "$HOME/.config/custom-clipboard.ini"

echo "==> restoring Klipper's Meta+V"
python3 - "$HOME/.config/kglobalshortcutsrc" <<'PY' 2>/dev/null || echo "    (could not edit kglobalshortcutsrc)"
import os, re, sys
path = sys.argv[1]
if not os.path.exists(path):
    sys.exit(0)
text = open(path).read()
new = re.sub(r'(?m)^show-on-mouse-pos=[^,]+,[^,]+,',
             'show-on-mouse-pos=Meta+V,Meta+V,', text)
if new != text:
    open(path, "w").write(new)
    print("    Klipper Meta+V restored (takes effect after re-login)")
else:
    print("    already on Meta+V")
PY

echo "==> removing custom-clipboard's shortcut block"
python3 - "$HOME/.config/kglobalshortcutsrc" <<'PY' 2>/dev/null || true
import re, sys
path = sys.argv[1]
text = open(path).read()
new = re.sub(r'\[custom-clipboard\]\n(?:[^\n]*\n)*\n?', '', text)
if new != text:
    open(path, "w").write(new)
    print("    removed [custom-clipboard] group")
PY

cat <<'EOF'

Note: ydotool.service was left enabled (it is harmless and tiny).
To remove it too:  systemctl --user disable --now ydotool.service

The built binary and sources remain in ~/.local/src/custom-clipboard/
EOF