# xdg-desktop-portal-aliveos

A backend implementation for [xdg-desktop-portal](http://github.com/flatpak/xdg-desktop-portal)
that uses GTK and various pieces of Cinnamon/MATE/Xfce4/Qtile infrastructure for AliveOS.

This portal backend provides file chooser dialogs (Open, Save, SaveFiles) for
sandboxed applications, flatpaks, and any desktop clients using the portal API.

## Features

- **Built-in Rich File Chooser**: Multi-view layout (Grid with large thumbnails, detailed List, and Compact view), interactive preview panel, places sidebar, breadcrumb pathbar with text entry toggle, and responsive file filters
- **OpenFile**: Native file/folder picker with filters, multiselect, and initial folder
- **SaveFile**: Single file save with suggested name, initial folder, and overwrite confirmation
- **SaveFiles**: Multi-file save for applications that export multiple files at once
- **Screenshot, Wallpaper, Inhibit, Background, Lockdown**: Additional portal interfaces
  for Cinnamon/MATE/Xfce4/Qtile desktop integration

## Installation

### Arch Linux (AUR)

```bash
yay -S xdg-desktop-portal-aliveos
```

### Building from Source

#### Dependencies

- `meson` (>= 0.56.0)
- `glib-2.0` (>= 2.44)
- `gtk+-3.0` (>= 3.0)
- `xdg-desktop-portal` (>= 1.5)
- `systemd` (>= 242)

#### Build

```bash
meson setup build --prefix=/usr
meson compile -C build
sudo meson install -C build
```

## How It Works

1. Applications call `org.freedesktop.impl.portal.FileChooser` methods (via `xdg-desktop-portal`)
2. This backend directly renders the built-in rich AliveOS file chooser dialog
3. Seamless integration without external IPC hops, timeouts, or daemon races

## D-Bus Interface

The backend implements:
- `org.freedesktop.impl.portal.FileChooser` (OpenFile, SaveFile, SaveFiles)
- `org.freedesktop.impl.portal.Screenshot`
- `org.freedesktop.impl.portal.Inhibit`
- `org.freedesktop.impl.portal.Background`
- `org.freedesktop.impl.portal.Lockdown`
- `org.freedesktop.impl.portal.Wallpaper`

## License

LGPL-2.1-or-later
