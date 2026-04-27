# hyprmac

**macOS-like feel for Hyprland.**

hyprmac is a Hyprland compositor plugin that brings the polished micro-interactions of macOS to your Wayland desktop. Rendered at the compositor level — not as floating windows or waybar widgets — so every effect works universally across all surfaces.

---

## Current features

### Caps Lock indicator
A small squircle badge appears just below the focused text caret whenever Caps Lock is active. It is rendered directly by Hyprland during the compositor render pass, not by a client window or bar widget.

![Caps Lock indicator — blue squircle badge below cursor with upward arrow icon](.github/screenshot.png)

---

## Planned features

- **Scroll momentum** — natural trackpad-style scroll deceleration
- **Transcription indicator** — visual badge when a microphone is active
- **Focus flash** — brief highlight ring when a window receives focus
- **Smart zoom** — keyboard-driven zoom with smooth interpolation

---

## Requirements

- Hyprland (see `hyprpm.toml` for pinned commits)
- GCC 12+ (`g++`)
- CMake 3.27+
- `pangocairo`, `cairo`, `pixman`, `wayland-server`, `xkbcommon`, `libdrm`, `libinput` (all pulled in as Hyprland dependencies on most distros)

---

## Installation

### Via hyprpm (recommended)

```sh
hyprpm add https://github.com/justinliang1020/hyprmac
hyprpm enable hyprmac
hyprpm reload
```

`hyprpm enable` only marks the plugin as enabled. Hyprland loads enabled plugins on startup when your config runs:

```ini
exec-once = hyprpm reload
```

Add that line to `~/.config/hypr/hyprland.conf` or a sourced override file such as `~/.config/hypr/userprefs.conf`.

After a Hyprland update, rebuild all plugins:

```sh
hyprpm update
```

### Manual build

```sh
git clone https://github.com/justinliang1020/hyprmac
cd hyprmac
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
hyprctl plugin load "$(pwd)/build/libhyprmac.so"
```

To load a manual build on startup, add to `~/.config/hypr/hyprland.conf` or a sourced override file such as `~/.config/hypr/userprefs.conf`:

```ini
exec-once = hyprctl plugin load /absolute/path/to/hyprmac/build/libhyprmac.so
```

Use an absolute path. `plugin:hyprmac { ... }` config only sets plugin options; it does not load the plugin by itself.

---

## Configuration

Add a `plugin:hyprmac` block to your `~/.config/hypr/hyprland.conf`:

```ini
plugin:hyprmac {
    caps_lock_color    = rgba(3B82F6ff)  # badge fill color (default: Tailwind blue-500)
    caps_lock_size     = 40              # badge diameter in logical pixels
    caps_lock_offset_y = 8               # pixels below the text caret
}
```

### Config reference

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `caps_lock_color` | `rgba(RRGGBBAA)` | `rgba(3B82F6ff)` | Badge fill color |
| `caps_lock_size` | integer (px) | `40` | Badge diameter in logical pixels |
| `caps_lock_offset_y` | integer (px) | `8` | Vertical offset from the text caret to the badge top |

Changes take effect immediately after `hyprctl reload` — no restart required.

The Caps Lock badge follows native Wayland text carets exposed through Hyprland's text-input state. Apps that do not provide a valid caret rectangle, including many XWayland cases, will hide the badge instead of guessing. Debug logs are written to `/tmp/hyprmac_debug.log`.

Implementation notes and protocol caveats live in [docs/caps-lock-indicator.md](docs/caps-lock-indicator.md).

---

## Unloading

```sh
hyprctl plugin unload /path/to/build/libhyprmac.so
```

---

## Contributing

hyprmac is designed to grow. Each macOS-feel feature lives in its own `src/features/<name>/` directory with a self-contained `init()` / `destroy()` pair wired into `main.cpp`. To add a new feature:

1. Create `src/features/your_feature/YourFeature.{hpp,cpp}`
2. Call `YourFeature::init()` from `PLUGIN_INIT` and `YourFeature::destroy()` from `PLUGIN_EXIT`
3. Register any new config values before calling `init()`

---

## License

MIT
