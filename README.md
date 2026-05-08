# kwinshot

Fast KWin-native screenshots for KDE Wayland, without Spectacle startup delay.

`kwinshot` uses a small native Qt selector and KWin's
`org.kde.KWin.ScreenShot2` API to capture screenshots directly through the
compositor.

## Features

- Region screenshots from a frozen current-screen frame by default.
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
kwinshot region --stdout > shot.png
kwinshot region --no-freeze
kwinshot region --border-color '#ff44aa'
```

By default, region screenshots are cropped from the frozen frame shown by the
selector. Use `--no-freeze` to select and capture the live desktop instead.
The selection border uses your Qt/KDE theme accent or highlight color by
default. Use `--border-color` to override it for one command.

Targets:

- `region`: select a rectangular region.
- `active-window`: capture the active window.
- `fullscreen`: capture the current screen under the cursor.

Outputs:

- Clipboard is the default.
- `--file path` writes a PNG file.
- `--stdout` writes PNG bytes to stdout.

Selector options:

- `--no-freeze`: capture the live desktop after selection.
- `--border-color color`: set the selection border color. Qt color names and
  hex values such as `#3daee9` are supported.

## KDE shortcuts

The desktop file exposes separate KDE actions:

- `Capture Rectangular Region` runs `kwinshot region`.
- `Capture Active Window` runs `kwinshot active-window`.
- `Capture Current Screen` runs `kwinshot fullscreen`.

After installing, open KDE's shortcut settings and bind these installed KWinShot
actions directly. Avoid creating separate custom-command shortcuts for the same
commands; those can be treated as a different, untrusted app by KWin.

## KDE authorization

KWin's `ScreenShot2` API is restricted. `kwinshot` is intended to be installed
as a normal native executable with a system desktop file. The installer keeps
the binary and desktop file root-owned so KWin can authorize the app.

If you get `NoAuthorized` errors, make sure you are launching the installed
application entry from `/usr/local/share/applications/net.local.kwinshot.desktop`
and not a local clone from `~/.local/share/applications`.

## Limitations

- KDE Plasma Wayland only.
- `fullscreen` currently means the current physical screen, not the whole
  virtual desktop across all monitors.
- Cross-monitor region selections are supported, but mixed-scale monitor setups
  may need more testing.

## License

MIT
