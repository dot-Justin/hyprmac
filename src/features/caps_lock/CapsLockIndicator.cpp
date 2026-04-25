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

// ── Static member definitions ──────────────────────────────────────────────

bool         CapsLockIndicator::s_capsActive    = false;
bool         CapsLockIndicator::s_textureDirty  = true;
SP<CTexture> CapsLockIndicator::s_pillTexture;
PHLMONITOR   CapsLockIndicator::s_currentMonitor;

CHyprSignalListener CapsLockIndicator::s_renderPreListener;
CHyprSignalListener CapsLockIndicator::s_renderStageListener;
CHyprSignalListener CapsLockIndicator::s_keyboardKeyListener;
CHyprSignalListener CapsLockIndicator::s_configReloadedListener;

// ── Public ─────────────────────────────────────────────────────────────────

void CapsLockIndicator::init() {
    s_capsActive   = isCapsLockActive();
    s_textureDirty = true;

    s_renderPreListener = Event::bus()->m_events.render.pre.listen(
        [](const PHLMONITOR& pMonitor) { onRenderPre(pMonitor); }
    );
    s_renderStageListener = Event::bus()->m_events.render.stage.listen(
        [](eRenderStage stage) { onRenderStage(stage); }
    );
    s_keyboardKeyListener = Event::bus()->m_events.input.keyboard.key.listen(
        [](IKeyboard::SKeyEvent ev, Event::SCallbackInfo& info) { onKeyboardKey(ev, info); }
    );
    s_configReloadedListener = Event::bus()->m_events.config.reloaded.listen(
        []() { onConfigReloaded(); }
    );
}

void CapsLockIndicator::destroy() {
    s_renderPreListener.reset();
    s_renderStageListener.reset();
    s_keyboardKeyListener.reset();
    s_configReloadedListener.reset();
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

    PHLMONITOR pMonitor = s_currentMonitor;
    if (!pMonitor)
        return;

    // Build (or rebuild) the pill texture while the GL context is active
    if (s_textureDirty) {
        buildTexture();
        s_textureDirty = false;
    }
    if (!s_pillTexture)
        return;

    const int size    = configInt("plugin:hyprmac:caps_lock_size");
    const int offsetY = configInt("plugin:hyprmac:caps_lock_offset_y");

    Vector2D cursorGlobal = g_pPointerManager->position();

    // Only draw on the monitor containing the cursor
    CBox monBox{pMonitor->m_position.x, pMonitor->m_position.y,
                pMonitor->m_size.x,     pMonitor->m_size.y};
    if (!monBox.containsPoint(cursorGlobal))
        return;

    // Transform global → monitor-local logical coordinates
    Vector2D local = cursorGlobal - pMonitor->m_position;

    // Badge: centered on cursor X, top edge = cursor tip + offsetY
    CBox pillBox{
        local.x - size / 2.0,
        local.y + (double)offsetY,
        (double)size,
        (double)size
    };

    CHyprOpenGLImpl::STextureRenderData rd{};
    rd.a = 1.0f;
    g_pHyprOpenGL->renderTexture(s_pillTexture, pillBox, rd);
}

void CapsLockIndicator::onKeyboardKey(IKeyboard::SKeyEvent ev, Event::SCallbackInfo& info) {
    if (!ev.updateMods)
        return;

    bool newState = isCapsLockActive();
    if (newState == s_capsActive)
        return;

    s_capsActive = newState;
    scheduleFrameAllMonitors();
}

void CapsLockIndicator::onConfigReloaded() {
    s_textureDirty = true;
    s_pillTexture.reset();
    // Texture will be rebuilt on the next RENDER_LAST_MOMENT
}

// ── Helpers ────────────────────────────────────────────────────────────────

bool CapsLockIndicator::isCapsLockActive() {
    for (auto& kb : g_pInputManager->m_keyboards) {
        if (kb && (kb->getModifiers() & HL_MODIFIER_CAPS))
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

void CapsLockIndicator::buildTexture() {
    const int     size     = configInt("plugin:hyprmac:caps_lock_size");
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
