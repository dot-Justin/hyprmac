#include "CapsLockIndicator.hpp"
#include "globals.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/managers/PointerManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <aquamarine/output/Output.hpp>

#include <cairo/cairo.h>
#include <cmath>
#include <unordered_map>
#include <string>
#include <fstream>
#include <chrono>
#include <iomanip>

// ── Static member definitions ──────────────────────────────────────────────

bool         CapsLockIndicator::s_capsActive    = false;
bool         CapsLockIndicator::s_textureDirty  = true;
int          CapsLockIndicator::s_lastPhysSize  = 0;
SP<CTexture> CapsLockIndicator::s_pillTexture;
PHLMONITOR   CapsLockIndicator::s_currentMonitor;
static bool  s_renderDebugShown = false;  // one-time debug flag

// ── Logging ─────────────────────────────────────────────────────────────────

static void logDebug(const std::string& msg) {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::ofstream log("/tmp/hyprmac_debug.log", std::ios::app);
    if (log.is_open()) {
        log << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S") << " - " << msg << std::endl;
    }
}

CHyprSignalListener CapsLockIndicator::s_renderPreListener;
CHyprSignalListener CapsLockIndicator::s_renderStageListener;
CHyprSignalListener CapsLockIndicator::s_configReloadedListener;
CHyprSignalListener CapsLockIndicator::s_keyboardKeyListener;

// ── Public ─────────────────────────────────────────────────────────────────

void CapsLockIndicator::init() {
    // Clear log file
    std::ofstream("/tmp/hyprmac_debug.log", std::ios::trunc).close();
    logDebug("=== hyprmac init ===");

    s_capsActive   = isCapsLockActive();
    s_textureDirty = true;

    logDebug("Initial Caps Lock state: " + std::string(s_capsActive ? "ON" : "OFF"));

    HyprlandAPI::addNotification(PHANDLE,
        "[hyprmac] init: Caps Lock active = " + std::string(s_capsActive ? "YES" : "NO"),
        CHyprColor{0.2, 0.8, 0.2, 1.0}, 3000);

    s_renderPreListener = Event::bus()->m_events.render.pre.listen(
        [](const PHLMONITOR& pMonitor) { onRenderPre(pMonitor); }
    );
    s_renderStageListener = Event::bus()->m_events.render.stage.listen(
        [](eRenderStage stage) { onRenderStage(stage); }
    );
    s_configReloadedListener = Event::bus()->m_events.config.reloaded.listen(
        []() { onConfigReloaded(); }
    );

    // Use global keyboard.key event to detect Caps Lock toggles
    // Note: updateMods is NOT set for Caps Lock, so we check keycode directly
    s_keyboardKeyListener = Event::bus()->m_events.input.keyboard.key.listen(
        [](IKeyboard::SKeyEvent ev, Event::SCallbackInfo& info) { onKeyboardKey(ev, info); }
    );
}

void CapsLockIndicator::destroy() {
    s_renderPreListener.reset();
    s_renderStageListener.reset();
    s_configReloadedListener.reset();
    s_keyboardKeyListener.reset();
    s_pillTexture.reset();
    s_textureDirty = true;
}

// ── Event handlers ─────────────────────────────────────────────────────────

void CapsLockIndicator::onRenderPre(const PHLMONITOR& pMonitor) {
    s_currentMonitor = pMonitor;
}

void CapsLockIndicator::onRenderStage(eRenderStage stage) {
    if (stage != RENDER_LAST_MOMENT)
        return;
    if (!s_capsActive)
        return;

    // One-time debug: show that render stage is being called
    if (!s_renderDebugShown) {
        s_renderDebugShown = true;
        logDebug("Render stage called with Caps Lock active");
        HyprlandAPI::addNotification(PHANDLE,
            "[hyprmac] render stage called with Caps Lock active",
            CHyprColor{0.2, 0.8, 0.2, 1.0}, 2000);
    }

    PHLMONITOR pMonitor = s_currentMonitor;
    if (!pMonitor)
        return;

    const int    size     = configInt("plugin:hyprmac:caps_lock_size");
    const int    offsetY  = configInt("plugin:hyprmac:caps_lock_offset_y");
    const double scale    = pMonitor->m_scale;
    const int    physSize = (int)std::round(size * scale);

    // Build (or rebuild) the pill texture at physical pixel size while GL context is active
    if (s_textureDirty || s_lastPhysSize != physSize) {
        logDebug("Building texture at physSize=" + std::to_string(physSize));
        buildTexture(physSize);
        s_lastPhysSize = physSize;
        s_textureDirty = false;
    }
    if (!s_pillTexture) {
        logDebug("Texture is null after build!");
        return;
    }

    Vector2D cursorGlobal = g_pPointerManager->position();

    // Only draw on the monitor containing the cursor
    CBox monBox{pMonitor->m_position.x, pMonitor->m_position.y,
                pMonitor->m_size.x,     pMonitor->m_size.y};
    if (!monBox.containsPoint(cursorGlobal))
        return;

    // Transform global → monitor-local logical coordinates, then scale to physical pixels
    // renderTexture expects physical (scaled) pixel coordinates
    Vector2D local = cursorGlobal - pMonitor->m_position;

    CBox pillBox{
        (local.x - size / 2.0) * scale,
        (local.y + (double)offsetY) * scale,
        (double)physSize,
        (double)physSize
    };

    logDebug("Rendering pill at (" + std::to_string(pillBox.x) + ", " + std::to_string(pillBox.y) + ") physSize=" + std::to_string(physSize));

    CHyprOpenGLImpl::STextureRenderData rd{};
    rd.a = 1.0f;
    g_pHyprOpenGL->renderTexture(s_pillTexture, pillBox, rd);
}

void CapsLockIndicator::onKeyboardKey(IKeyboard::SKeyEvent ev, Event::SCallbackInfo& info) {
    // Caps Lock keycode is typically 58 in XKB
    const uint32_t CAPS_LOCK_KEYCODE = 58;

    logDebug("key event: keycode=" + std::to_string(ev.keycode) +
             ", state=" + std::to_string(ev.state) +
             ", updateMods=" + std::string(ev.updateMods ? "YES" : "NO"));

    // Check if this is Caps Lock (keycode 58) and it was pressed
    if (ev.keycode == CAPS_LOCK_KEYCODE && ev.state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        logDebug("Caps Lock key pressed, toggling state...");

        // Toggle the state ourselves since XKB state isn't updated yet
        s_capsActive = !s_capsActive;
        logDebug("Caps Lock toggled to " + std::string(s_capsActive ? "ON" : "OFF"));

        HyprlandAPI::addNotification(PHANDLE,
            "[hyprmac] Caps Lock toggled to " + std::string(s_capsActive ? "ON" : "OFF"),
            CHyprColor{0.2, 0.8, 0.2, 1.0}, 2000);
        scheduleFrameAllMonitors();
    }
}

void CapsLockIndicator::onConfigReloaded() {
    s_textureDirty = true;
    s_pillTexture.reset();
    // Texture will be rebuilt on the next RENDER_LAST_MOMENT
}

// ── Helpers ────────────────────────────────────────────────────────────────

bool CapsLockIndicator::isCapsLockActive() {
    // Caps Lock is a locked modifier — it lives in m_modifiersState.locked,
    // not in the depressed/latched state that getModifiers() returns.
    for (auto& kb : g_pInputManager->m_keyboards) {
        if (kb && (kb->m_modifiersState.locked & HL_MODIFIER_CAPS))
            return true;
    }
    return false;
}

void CapsLockIndicator::scheduleFrameAllMonitors() {
    for (auto& pMon : g_pCompositor->m_monitors) {
        if (!pMon || !pMon->m_enabled)
            continue;
        g_pCompositor->scheduleFrameForMonitor(
            pMon, Aquamarine::IOutput::AQ_SCHEDULE_CLIENT_UNKNOWN
        );
    }
}

// ── Cairo texture rendering ────────────────────────────────────────────────

void CapsLockIndicator::buildTexture(int physSize) {
    const int     size     = physSize;
    const int64_t colorRaw = (int64_t)configInt("plugin:hyprmac:caps_lock_color");

    // Decode rgba(RRGGBBAA) packed int → normalised RGBA
    const double cR = ((colorRaw >> 24) & 0xFF) / 255.0;
    const double cG = ((colorRaw >> 16) & 0xFF) / 255.0;
    const double cB = ((colorRaw >>  8) & 0xFF) / 255.0;
    const double cA = ((colorRaw >>  0) & 0xFF) / 255.0;

    auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    auto* cr      = cairo_create(surface);

    // Clear to transparent
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_restore(cr);

    // ── Squircle background ──
    // Radius ~0.28× gives a macOS-style squircle (not a full circle, not sharp)
    const double rad = size * 0.28;
    const double w   = (double)size;
    const double h   = (double)size;

    cairo_new_path(cr);
    cairo_arc(cr, rad,     rad,     rad, M_PI,        M_PI * 1.5); // top-left
    cairo_arc(cr, w - rad, rad,     rad, M_PI * 1.5,  0.0        ); // top-right
    cairo_arc(cr, w - rad, h - rad, rad, 0.0,         M_PI * 0.5 ); // bottom-right
    cairo_arc(cr, rad,     h - rad, rad, M_PI * 0.5,  M_PI       ); // bottom-left
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, cR, cG, cB, cA);
    cairo_fill(cr);

    // ── Caps Lock icon (white) ──
    // Icon occupies 60% of the badge, centred
    const double iconSize = size * 0.60;
    const double ox       = (size - iconSize) / 2.0;
    const double oy       = (size - iconSize) / 2.0;
    const double sc       = iconSize / 256.0; // scale from Phosphor's 256px viewBox

    cairo_save(cr);
    cairo_translate(cr, ox, oy);
    cairo_scale(cr, sc, sc);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.92);

    // Arrow polygon — Phosphor "arrow-fat-line-up" coordinates (256×256 viewBox)
    cairo_new_path(cr);
    cairo_move_to(cr, 128,  35); // tip
    cairo_line_to(cr,  32, 128); // outer left
    cairo_line_to(cr,  72, 128); // inner left shoulder
    cairo_line_to(cr,  88, 184); // body bottom-left
    cairo_line_to(cr, 168, 184); // body bottom-right
    cairo_line_to(cr, 184, 128); // inner right shoulder
    cairo_line_to(cr, 224, 128); // outer right
    cairo_close_path(cr);
    cairo_fill(cr);

    // Bottom bar — rounded rectangle (x:80–176, y:200–216, corner r=8)
    const double bx = 80, by = 200, bw = 96, bh = 16, br = 8;
    cairo_new_path(cr);
    cairo_arc(cr, bx + br,      by + br,      br, M_PI,        M_PI * 1.5);
    cairo_arc(cr, bx + bw - br, by + br,      br, M_PI * 1.5,  0.0       );
    cairo_arc(cr, bx + bw - br, by + bh - br, br, 0.0,         M_PI * 0.5);
    cairo_arc(cr, bx + br,      by + bh - br, br, M_PI * 0.5,  M_PI      );
    cairo_close_path(cr);
    cairo_fill(cr);

    cairo_restore(cr);

    // Convert Cairo surface → OpenGL texture (requires active GL context)
    cairo_surface_flush(surface);
    s_pillTexture = g_pHyprOpenGL->texFromCairo(surface);

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

int CapsLockIndicator::configInt(const std::string& key) {
    static std::unordered_map<std::string, Hyprlang::INT* const*> cache;
    auto it = cache.find(key);
    if (it == cache.end()) {
        auto* cv = HyprlandAPI::getConfigValue(PHANDLE, key);
        if (!cv)
            return 0;
        cache[key] = (Hyprlang::INT* const*)cv->getDataStaticPtr();
        it         = cache.find(key);
    }
    return (int)**it->second;
}
