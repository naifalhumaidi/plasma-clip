# Development

Build, installation and verification notes for contributors and coding agents.

## Supported setup

The application is designed for KDE Plasma with Klipper and Qt 6 / KF6. The
installer also expects Bash, Python 3.10+, `systemctl --user`, and the KDE
shortcut configuration file. The documented package examples cover Arch-based
and Debian/Ubuntu systems; other distributions may use different package names,
header locations, or `ydotool` service packaging.

The code is Linux-distribution independent in principle, but setup is not
universal. Another distribution can use the app when it provides the KDE
components and libraries above; package names, KF6 header paths, shortcut
configuration, autostart handling, and service setup can differ. Treat the
listed distributions as package examples, not proof of support elsewhere.

### Adapting to another system

When a user asks an AI agent to set this up on a different Plasma distribution,
the agent should first inspect the local environment and compare it with the
requirements above. Adapt package instructions, header discovery, or per-user
startup integration only where the system differs. If a required component or
Klipper interface is missing, explain the gap and offer a system-specific
solution based on this code rather than claiming the stock setup works.

Keep the data-safety rules in `AGENTS.md`: never point tests at real clipboard
data, never change live shortcuts or restart the user's shell as a test, and
verify changes with synthetic fixtures. The agent should state which build and
runtime checks it could complete on that machine.

Click-to-paste is optional. It requires the `ydotool` client, a responding
`ydotoold` user service, and access to `/dev/uinput`. The app cannot grant that
permission. On Wayland, focus return is compositor-controlled, so users may
need to increase `pasteDelayMs`.

## Build

Install the system development packages first; the scripts never install or
update packages automatically. Then run:

```bash
./build.sh
```

This produces the gitignored `custom-clipboard` executable in the repository.
The build checks Qt 6 Widgets, D-Bus and SQL through `pkg-config`, plus
`kglobalaccel.h`. The default KF6 header directory is
`/usr/include/KF6/KGlobalAccel`; set `KF6_GLOBALACCEL_INCLUDE_DIR` if it lives
elsewhere. The link step uses `-lKF6GlobalAccel`.

Equivalent compile command:

```bash
g++ -std=c++17 -O2 -fPIC src/main.cpp -o custom-clipboard \
    $(pkg-config --cflags --libs Qt6Widgets Qt6DBus Qt6Sql) \
    -I"${KF6_GLOBALACCEL_INCLUDE_DIR:-/usr/include/KF6/KGlobalAccel}" \
    -lKF6GlobalAccel
```

## Install behavior

`install.sh` builds and installs to `~/.local/bin`, writes a KDE autostart
desktop entry, and adds a user-systemd restart override. It saves the first
copy of `kglobalshortcutsrc` as
`~/.config/kglobalshortcutsrc.bak-custom-clipboard`, then releases only
Klipper's Meta+V action and the legacy `clipdeck` action. Re-login is needed for
Plasma to reload those settings. If `ydotool` is already installed, the script
tries to enable and start `ydotool.service`; it does not install it or modify
device permissions.

`uninstall.sh` stops the app, removes its installed binary, autostart entry,
settings and restart override, then restores those two shortcut entries from
the original backup while preserving unrelated current shortcut settings. The
backup and clipboard Trash remain. `ydotool.service` is left enabled for other
applications.

Run only one app instance at a time. The program does not enforce a process
singleton, so manually starting it while its autostart instance is running can
create competing shortcut registrations.

Both scripts operate on the current user's home directory. Do not run them to
verify behavior on a maintainer's or friend's active desktop; test the shortcut
helper against temporary fixture files instead.

## Verification and clipboard safety

The current `--test` harness is **not safe to point at an ordinary user
profile**. It creates its `Deck` before its internal redirect, then copies the
database, WAL file and image payloads from `XDG_DATA_HOME/klipper` into a
temporary directory. It also checks for a live Klipper D-Bus service and a
working `ydotoold`; paste-related UI actions can call Klipper over D-Bus.

For isolated feature checks, compile into a temporary directory and set all of
these before starting the process:

- `XDG_DATA_HOME` to a temporary directory containing a synthetic
  `klipper/history3.sqlite` fixture with both starred and unstarred rows;
- `XDG_CONFIG_HOME` to another temporary directory;
- `TMPDIR` to a private temporary directory (the harness uses a fixed child
  named `custom-clipboard-test`);
- `DBUS_SESSION_BUS_ADDRESS` to a private, unavailable socket; and
- `PATH` to a directory without `ydotool` or `systemctl`.

The suite exercises its UI/database paths in that setup, but the three final
host-integration checks for ydotool presence, ydotoold reachability and Klipper
D-Bus are expected to fail. Report those as unavailable, not as product
failures or as a full pass. Do not use only `CUSTOM_CLIPBOARD_DATA` as a safety
boundary: it does not affect the constructor's initial read or the test's
source database selection.

The screenshot generator uses its own synthetic fixture, but writes PNGs into
`docs/`. Run it only when you intend to update those assets. A headless render
does not verify Wayland focus handoff or real global-shortcut ownership.

## Project map

| Path | Purpose |
|---|---|
| `src/main.cpp` | Qt panel, Klipper database access, actions and headless harness |
| `build.sh` | Local compile and dependency checks |
| `install.sh` / `uninstall.sh` | Per-user setup and removal |
| `tools/shortcuts.py` | Fixture-testable shortcut editing and restoration |
| `docs/` | Synthetic UI previews |
| `AGENTS.md` | Detailed coding-agent rules and regression history |

The app is a single C++ translation unit. Keep data access and writes explicit,
use prepared SQL statements for values, and test destructive behavior against
synthetic history and a redirected Trash sidecar. Preserve the user's current
changes and do not commit generated binaries or screenshots unless requested.
