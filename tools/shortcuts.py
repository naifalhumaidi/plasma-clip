#!/usr/bin/env python3
"""Update only the Klipper shortcut entries owned by custom-clipboard."""

from __future__ import annotations

import os
import re
import shutil
import sys
import tempfile
from pathlib import Path

SECTION = re.compile(r"^\s*\[([^\]]+)\]")
TARGETS = {
    "klipper": {"show-on-mouse-pos"},
    "clipdeck": {"show-clipboard"},
}


def read_lines(path: Path) -> list[str]:
    return path.read_text(encoding="utf-8", errors="surrogateescape").splitlines(
        keepends=True
    )


def entries(lines: list[str]):
    section = ""
    for index, line in enumerate(lines):
        match = SECTION.match(line)
        if match:
            section = match.group(1).strip().lower()
        yield index, section, line


def key_of(line: str) -> str | None:
    if not line or line.lstrip().startswith(("#", ";", "[")) or "=" not in line:
        return None
    return line.split("=", 1)[0].strip().lower()


def disable_shortcuts(lines: list[str]) -> list[str]:
    result = []
    for _, section, line in entries(lines):
        key = key_of(line)
        if key not in TARGETS.get(section, set()):
            result.append(line)
            continue
        prefix, value = line.split("=", 1)
        ending = "\r\n" if value.endswith("\r\n") else "\n" if value.endswith("\n") else ""
        value = value[: -len(ending)] if ending else value
        fields = value.split(",")
        while len(fields) < 2:
            fields.append("")
        fields[0] = fields[1] = "none"
        result.append(prefix + "=" + ",".join(fields) + ending)
    return result


def saved_values(lines: list[str]) -> dict[tuple[str, str], list[str]]:
    result: dict[tuple[str, str], list[str]] = {}
    for _, section, line in entries(lines):
        key = key_of(line)
        if key in TARGETS.get(section, set()):
            result.setdefault((section, key), []).append(line)
    return result


def restore_shortcuts(lines: list[str], backup: list[str]) -> list[str]:
    originals = saved_values(backup)
    seen: dict[tuple[str, str], int] = {}
    result = []
    for _, section, line in entries(lines):
        key = key_of(line)
        ident = (section, key or "")
        saved = originals.get(ident, [])
        position = seen.get(ident, 0)
        if key in TARGETS.get(section, set()) and position < len(saved):
            result.append(saved[position])
            seen[ident] = position + 1
        else:
            result.append(line)

    # Remove only the application's own persisted KGlobalAccel group.
    cleaned = []
    skip = False
    for line in result:
        match = SECTION.match(line)
        if match:
            skip = match.group(1).strip().lower() == "custom-clipboard"
        if not skip:
            cleaned.append(line)
    return cleaned


def atomic_write(path: Path, lines: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    mode = path.stat().st_mode & 0o777 if path.exists() else 0o600
    fd, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        os.fchmod(fd, mode)
        with os.fdopen(fd, "w", encoding="utf-8", errors="surrogateescape", newline="") as out:
            out.writelines(lines)
            out.flush()
            os.fsync(out.fileno())
        os.replace(temporary, path)
    except BaseException:
        try:
            os.close(fd)
        except OSError:
            pass
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


def main() -> int:
    if len(sys.argv) != 4 or sys.argv[1] not in {"install", "restore"}:
        print(f"Usage: {sys.argv[0]} install|restore CONFIG BACKUP", file=sys.stderr)
        return 2
    mode, config_arg, backup_arg = sys.argv[1:]
    config, backup = Path(config_arg), Path(backup_arg)

    if mode == "install":
        if not config.exists():
            print("    no kglobalshortcutsrc yet (first login?) — nothing to do")
            return 0
        if not backup.exists():
            shutil.copy2(config, backup)
        current = read_lines(config)
        updated = disable_shortcuts(current)
        if updated != current:
            atomic_write(config, updated)
            print(f"    Klipper Meta+V released (original shortcuts saved in {backup.name})")
        else:
            print("    already released, nothing to change")
        return 0

    if not config.exists():
        print("    no kglobalshortcutsrc found; nothing to restore")
        return 0
    current = read_lines(config)
    if backup.exists():
        restored = restore_shortcuts(current, read_lines(backup))
    else:
        # Still remove the app's own persisted action if the user removed the
        # backup; preserve Klipper and legacy shortcut values as found.
        restored = restore_shortcuts(current, [])
    if restored != current:
        atomic_write(config, restored)
        if backup.exists():
            print("    original Klipper and legacy clipdeck shortcuts restored")
        else:
            print("    removed the app shortcut group; no original backup was found")
    else:
        print("    shortcut file already matches its original values")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
