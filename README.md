# kwinshot - a leaner Spectacle alternative.

Fast KWin-native screenshots for KDE Wayland, without Spectacle startup delay. (see https://bugs.kde.org/show_bug.cgi?id=442876)

`kwinshot` uses a small native Qt selector and KWin's
`org.kde.KWin.ScreenShot2` API to capture screenshots directly through the
compositor.

https://github.com/user-attachments/assets/d9db2974-d909-44af-84b9-8d45209eb98f

## Features

- Region screenshots from a frozen frame by default.
- Active-window screenshots.
- Current-screen screenshots.
- Multi-monitor region selections.
- Clipboard, file, and stdout output.
- Theme-aware selection border with an optional color override.
- KDE shortcut actions for each capture mode.

## Requirements

- KDE Plasma Wayland session with KWin.
- Qt 6 development packages: Core, Gui, Widgets, DBus.
- CMake and a C++17 compiler.
- `wl-copy` from `wl-clipboard` for clipboard output.
- `desktop-file-validate` from `desktop-file-utils` is optional but used by the
  installer when available.

## Install

```sh
./install.sh
```

The installer builds in `./build`, installs to `/usr/local`, fixes the
root-owned file permissions needed by KWin's restricted screenshot API, and
refreshes KDE's service cache.

Equivalent manual install:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build --prefix /usr/local
sudo chown root:root /usr/local/bin/kwinshot /usr/local/share/applications/net.local.kwinshot.desktop
sudo chmod 755 /usr/local/bin/kwinshot
sudo chmod 644 /usr/local/share/applications/net.local.kwinshot.desktop
kbuildsycoca6 --noincremental
```

## Update

```sh
./update.sh
```

This pulls the latest source with `git pull --ff-only` and runs the installer
again. It refuses to run if the checkout has local changes.

## Uninstall

```sh
./uninstall.sh
```

This removes `/usr/local/bin/kwinshot` and the installed desktop file, then
refreshes KDE's service cache.

## Usage

```sh
kwinshot
kwinshot region
kwinshot active-window
kwinshot fullscreen
kwinshot region --file shot.png
kwinshot region --autosave
kwinshot region --autosave --clipboard
kwinshot region --stdout > shot.png
kwinshot region --no-freeze
kwinshot region --border-color '#ff44aa'
```

By default, region screenshots are cropped from the frozen frame shown by the
selector. Use `--no-freeze` to select and capture the live desktop instead.
The selection border uses your Qt/KDE theme accent or highlight color by
default. Use `--border-color` to override it for one command.

After selecting a region, use the small selector buttons to copy or save the
screenshot. `Enter`/`Ctrl+C` copies, and `Ctrl+S` opens the save dialog. Saving
from the selector also attempts to copy the saved PNG to the clipboard.

Targets:

- `region`: select a rectangular region.
- `active-window`: capture the active window.
- `fullscreen`: capture the current screen under the cursor.

Outputs:

- Clipboard is the default.
- `--file path` writes a PNG file.
- `--autosave` writes a timestamped PNG to `~/Pictures/Screenshots`.
- Add `--clipboard` to `--file` or `--autosave` to also copy the PNG.
- `--stdout` writes PNG bytes to stdout.

Selector options:

- `--no-freeze`: capture the live desktop after selection.
- `--border-color color`: set the selection border color. Qt color names and
  hex values such as `#3daee9` are supported.

## KDE shortcuts

The desktop file exposes separate KDE actions:

- `Capture Rectangular Region` runs `kwinshot region`.
- `Capture Region to File` runs `kwinshot region --autosave`.
- `Capture Active Window` runs `kwinshot active-window`.
- `Capture Current Screen` runs `kwinshot fullscreen`.

After installing, open KDE's shortcut settings and bind these installed KWinShot
actions directly. Custom-command shortcuts can also work after a system install,
but the installed actions are the safest path because they keep KDE's app
identity tied to the trusted desktop file.

## KDE authorization

KWin's `ScreenShot2` API is restricted. `kwinshot` is intended to be installed
as a normal native executable with a system desktop file. The installer keeps
the binary and desktop file root-owned so KWin can authorize the app.

If you get `NoAuthorized` errors, check that `/usr/local/bin/kwinshot` and
`/usr/local/share/applications/net.local.kwinshot.desktop` are root-owned. Also
make sure a local desktop-file clone in `~/.local/share/applications` is not
shadowing the installed system entry.

## Limitations

- KDE Plasma Wayland only.
- `fullscreen` currently means the current physical screen, not the whole
  virtual desktop across all monitors.
- Cross-monitor region selections are supported. On mixed-scale monitor setups,
  the selection overlay can appear offset or scaled while dragging between
  outputs. Single-monitor selections and same-scale cross-monitor selections are
  the most reliable.
  
## Acknowledgments

* **seth (Arch Linux Forums):** Special thanks for [articulating the necessity](https://bbs.archlinux.org/viewtopic.php?id=298864) of using a privileged `.desktop` file to interface with restricted KWin APIs. This tool is a direct implementation of that architectural approach to solving the "NoAuthorized" error on Wayland.

## License

MIT
