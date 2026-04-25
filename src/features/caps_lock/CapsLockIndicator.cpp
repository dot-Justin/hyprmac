#include "CapsLockIndicator.hpp"
#include "globals.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
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
bool         CapsLockIndicator::s_textureDirty  = false;
SP<CTexture> CapsLockIndicator::s_pillTexture   = nullptr;
PHLMONITOR   CapsLockIndicator::s_currentMonitor;
std::optional<CBox> CapsLockIndicator::s_lastGlobalPillBox;
static bool  s_renderDebugShown = false;  // one-time debug flag

namespace {

class CCapsLockPassElement final : public IPassElement {
  public:
    struct SData {
        PHLMONITORREF monitor;
        CBox box;
        CHyprColor bgColor;
    };

    explicit CCapsLockPassElement(SData&& data) : m_data(std::move(data)) {
    }

    void draw(const CRegion& damage) override {
        (void)damage;

        const auto renderMonitor = g_pHyprOpenGL->m_renderData.pMonitor.lock();
        const auto expectedMonitor = m_data.monitor.lock();
        if (!renderMonitor || !expectedMonitor || renderMonitor != expectedMonitor)
            return;

        const double size = m_data.box.w;
        const double centerX = m_data.box.x + size / 2.0;
        const int glyphRound = std::max(2, (int)std::round(size * 0.06));

        g_pHyprOpenGL->renderRect(
            m_data.box,
            m_data.bgColor,
            {.round = std::max(4, (int)std::round(size * 0.28))}
        );

        CBox stem{
            centerX - size * 0.07,
            m_data.box.y + size * 0.24,
            size * 0.14,
            size * 0.30
        };
        g_pHyprOpenGL->renderRect(stem, CHyprColor{1.0, 1.0, 1.0, 0.94}, {.round = glyphRound});

        CBox cap{
            centerX - size * 0.20,
            m_data.box.y + size * 0.16,
            size * 0.40,
            size * 0.12
        };
        g_pHyprOpenGL->renderRect(cap, CHyprColor{1.0, 1.0, 1.0, 0.94}, {.round = glyphRound});

        CBox bar{
            centerX - size * 0.24,
            m_data.box.y + size * 0.70,
            size * 0.48,
            size * 0.10
        };
        g_pHyprOpenGL->renderRect(bar, CHyprColor{1.0, 1.0, 1.0, 0.94}, {.round = glyphRound});
    }

    bool needsLiveBlur() override {
        return false;
    }

    bool needsPrecomputeBlur() override {
        return false;
    }

    bool undiscardable() override {
        return true;
    }

    std::optional<CBox> boundingBox() override {
        return m_data.box.copy().expand(4.0);
    }

    CRegion opaqueRegion() override {
        return {};
    }

    const char* passName() override {
        return "CCapsLockPassElement";
    }

  private:
    SData m_data;
};

} // namespace

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
CHyprSignalListener CapsLockIndicator::s_keyboardFocusListener;
CHyprSignalListener CapsLockIndicator::s_windowActiveListener;

// ── Public ─────────────────────────────────────────────────────────────────

void CapsLockIndicator::init() {
    // Clear log file
    std::ofstream("/tmp/hyprmac_debug.log", std::ios::trunc).close();
    logDebug("=== hyprmac init ===");

    s_capsActive   = isCapsLockActive();
    s_textureDirty = false;
    s_lastGlobalPillBox.reset();

    logDebug("Initial Caps Lock state: " + std::string(s_capsActive ? "ON" : "OFF"));

    s_renderPreListener = Event::bus()->m_events.render.pre.listen(
        [](const PHLMONITOR& pMonitor) { onRenderPre(pMonitor); }
    );
    s_renderStageListener = Event::bus()->m_events.render.stage.listen(
        [](eRenderStage stage) { onRenderStage(stage); }
    );
    s_configReloadedListener = Event::bus()->m_events.config.reloaded.listen(
        []() { onConfigReloaded(); }
    );

    s_keyboardKeyListener = Event::bus()->m_events.input.keyboard.key.listen(
        [](IKeyboard::SKeyEvent ev, Event::SCallbackInfo& info) { onKeyboardKey(ev, info); }
    );
    s_keyboardFocusListener = Event::bus()->m_events.input.keyboard.focus.listen(
        [](SP<CWLSurfaceResource> surface) { onKeyboardFocus(surface); }
    );
    s_windowActiveListener = Event::bus()->m_events.window.active.listen(
        [](PHLWINDOW window, Desktop::eFocusReason) { onWindowActive(window); }
    );

    if (s_capsActive) {
        invalidateIndicator();
    }
}

void CapsLockIndicator::destroy() {
    s_renderPreListener.reset();
    s_renderStageListener.reset();
    s_configReloadedListener.reset();
    s_keyboardKeyListener.reset();
    s_keyboardFocusListener.reset();
    s_windowActiveListener.reset();
    s_pillTexture.reset();
    s_textureDirty = false;
    s_lastGlobalPillBox.reset();
}

// ── Event handlers ─────────────────────────────────────────────────────────

void CapsLockIndicator::onRenderPre(const PHLMONITOR& pMonitor) {
    s_currentMonitor = pMonitor;
}

void CapsLockIndicator::onRenderStage(eRenderStage stage) {
    if (stage != RENDER_POST_WINDOWS)
        return;
    if (!s_capsActive)
        return;

    // One-time debug: show that render stage is being called
    if (!s_renderDebugShown) {
        s_renderDebugShown = true;
        logDebug("Render stage called with Caps Lock active");
    }

    PHLMONITOR pMonitor = s_currentMonitor;
    if (!pMonitor)
        return;

    const auto globalPillBox = pillBoxGlobal();
    if (!globalPillBox.has_value())
        return;

    PHLMONITOR pillMonitor = g_pCompositor->getMonitorFromVector(globalPillBox->middle());
    if (!pillMonitor || pillMonitor != pMonitor)
        return;

    CBox pillBox = globalPillBox->copy().translate(-pMonitor->m_position);
    s_lastGlobalPillBox = globalPillBox;

    logDebug("Queueing pill pass at logical (" + std::to_string(pillBox.x) + ", " + std::to_string(pillBox.y) + ")");

    CCapsLockPassElement::SData data;
    data.monitor = pMonitor;
    data.box = pillBox;
    const int64_t colorRaw = (int64_t)configInt("plugin:hyprmac:caps_lock_color");
    data.bgColor = CHyprColor{
        (float)(((colorRaw >> 24) & 0xFF) / 255.0),
        (float)(((colorRaw >> 16) & 0xFF) / 255.0),
        (float)(((colorRaw >>  8) & 0xFF) / 255.0),
        (float)(((colorRaw >>  0) & 0xFF) / 255.0)
    };

    g_pHyprRenderer->m_renderPass.add(makeUnique<CCapsLockPassElement>(std::move(data)));
}

void CapsLockIndicator::onKeyboardKey(IKeyboard::SKeyEvent ev, Event::SCallbackInfo& info) {
    const uint32_t CAPS_LOCK_KEYCODE = 58;
    (void)info;

    logDebug("key event: keycode=" + std::to_string(ev.keycode) +
             ", state=" + std::to_string(ev.state) +
             ", updateMods=" + std::string(ev.updateMods ? "YES" : "NO"));

    // Check if this is Caps Lock (keycode 58) and it was pressed
    if (ev.keycode == CAPS_LOCK_KEYCODE && ev.state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        logDebug("Caps Lock key pressed, toggling state...");

        s_capsActive = !s_capsActive;
        logDebug("Caps Lock toggled to " + std::string(s_capsActive ? "ON" : "OFF"));
        invalidateIndicator();
    }
}

void CapsLockIndicator::onKeyboardFocus(SP<CWLSurfaceResource> surface) {
    (void)surface;
    invalidateIndicator();
}

void CapsLockIndicator::onWindowActive(PHLWINDOW window) {
    (void)window;
    invalidateIndicator();
}

void CapsLockIndicator::onConfigReloaded() {
    s_textureDirty = false;
    s_pillTexture.reset();
    invalidateIndicator();
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

void CapsLockIndicator::invalidateIndicator() {
    const auto nextBox = s_capsActive ? pillBoxGlobal() : std::nullopt;
    damageBox(s_lastGlobalPillBox);
    damageBox(nextBox);
    s_lastGlobalPillBox = nextBox;
    scheduleFrameAllMonitors();
}

std::optional<CBox> CapsLockIndicator::caretBoxGlobal() {
    auto* const focusedTextInput = g_pInputManager->m_relay.getFocusedTextInput();
    if (!focusedTextInput || !focusedTextInput->isEnabled() || !focusedTextInput->hasCursorRectangle())
        return std::nullopt;

    const auto focusedSurface = focusedTextInput->focusedSurface();
    if (!focusedSurface)
        return std::nullopt;

    const auto hlSurface = focusedSurface->m_hlSurface.lock();
    if (!hlSurface)
        return std::nullopt;

    const auto surfaceBoxGlobal = hlSurface->getSurfaceBoxGlobal();
    if (!surfaceBoxGlobal.has_value())
        return std::nullopt;

    const auto caretLocal = focusedTextInput->cursorBox();
    return CBox{
        surfaceBoxGlobal->x + caretLocal.x,
        surfaceBoxGlobal->y + caretLocal.y,
        std::max(1.0, caretLocal.w),
        std::max(1.0, caretLocal.h)
    };
}

std::optional<CBox> CapsLockIndicator::pillBoxGlobal() {
    const auto caret = caretBoxGlobal();
    if (!caret.has_value())
        return std::nullopt;

    const int size    = configInt("plugin:hyprmac:caps_lock_size");
    const int offsetY = configInt("plugin:hyprmac:caps_lock_offset_y");
    const double centerX = caret->x + caret->w / 2.0;
    const double badgeX  = std::round(centerX - size / 2.0);
    const double badgeY  = std::round(caret->y + caret->h + (double)offsetY);

    return CBox{
        badgeX,
        badgeY,
        (double)size,
        (double)size
    };
}

void CapsLockIndicator::damageBox(const std::optional<CBox>& box) {
    if (!box.has_value())
        return;

    logDebug("Damaging global pill box at (" + std::to_string(box->x) + ", " + std::to_string(box->y) + ")");
    g_pHyprRenderer->damageBox(box->copy().expand(4.0));
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
