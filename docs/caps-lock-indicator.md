# Caps Lock Indicator Notes

This feature no longer follows the mouse pointer. It follows the focused text caret exposed to Hyprland through Wayland text-input protocols.

## What actually works

- The badge is anchored to the focused surface plus that surface's reported caret rectangle.
- The preferred source is Hyprland's focused relay text input.
- If the relay is missing, stale, or has no cursor rectangle, the plugin falls back to directly tracked `text-input-v1` and `text-input-v3` objects already known to Hyprland.
- If there is still no valid caret rectangle, the badge hides instead of guessing from the mouse cursor.

## Research findings

- Hyprland's own IME and popup placement code is driven by text-input cursor rectangles. There is not a separate hidden compositor API for "the text caret position" that plugins can use instead.
- Relay-only was not sufficient on this machine. In practice, `g_pInputManager->m_relay.getFocusedTextInput()` was often missing or stale even when Hyprland still had usable `text-input-v3` objects in memory.
- Kitty on this machine does support `zwp_text_input_manager_v3` and sends `set_cursor_rectangle(...)`. The earlier assumption that Kitty simply did not expose a caret was wrong.
- Some clients, including Helium during parts of navigation or video-heavy UI, transiently drop or stop updating their caret rectangle. For those cases the plugin keeps a short same-client caret cache to smooth brief protocol gaps.
- Hyprland can retain a valid updated `text-input-v3` rectangle even when that tracked object is marked `enabled=no`. The plugin therefore prefers enabled objects, but it will still use a same-client updated rectangle when that is the only valid source.

## Compatibility expectations

- Native Wayland editors, terminals, and browser text fields are the target.
- XWayland apps and any client that never sends a valid cursor rectangle are unsupported by design for this feature.
- Browser UI is less stable than terminal/editor widgets because some browser surfaces stop reporting caret updates outside true editable fields.
- Chromium and Electron-family apps may also need their Wayland IME path enabled before they publish usable text-input caret rectangles. If `/tmp/hyprmac_debug.log` shows `chromium_like=yes enable_wayland_ime=no`, the app launch command is a likely blocker rather than the compositor plugin itself.

## Debugging

- Runtime diagnostics are written to `/tmp/hyprmac_debug.log`.
- Missing-caret reasons are logged explicitly, for example:
  - `no_focus_surface`
  - `no_surface_box`
  - `relay_stale`
  - `relay_no_cursor_rect`
  - `no_v3_candidate`
  - `no_monitor`
  - `no_texture`
- While Caps Lock is active, the plugin may also emit a short Hyprland notification when the missing-caret reason changes.

## Caps Lock state

- Caps Lock must be read from Hyprland's locked modifier state, not from depressed modifiers and not by flipping a local boolean on keypress.
- The plugin now resyncs against Hyprland's real locked modifier state during rendering and damages the old and new badge regions when the state changes. That avoids one-toggle-behind behavior.

## Known rough edges

- Placement is based on logical compositor coordinates from the focused surface and caret box. It should be setup-agnostic, but per-app visual offsets can still look slightly off in some configurations and need further tuning.
- Helium-class browser surfaces can still disappear when the app stops publishing a valid caret rectangle. When that happens, the debug log should make the reason explicit instead of failing silently.
