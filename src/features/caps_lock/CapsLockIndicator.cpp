#include "CapsLockIndicator.hpp"
#include "globals.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#define private public
#include <hyprland/src/protocols/TextInputV1.hpp>
#include <hyprland/src/protocols/TextInputV3.hpp>
#undef private
#include <aquamarine/output/Output.hpp>

#include <algorithm>
#include <cairo/cairo.h>
#include <cmath>
#include <unordered_map>
#include <string>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <sstream>

// ── Static member definitions ──────────────────────────────────────────────

bool         CapsLockIndicator::s_capsActive    = false;
bool         CapsLockIndicator::s_textureDirty  = true;
SP<CTexture> CapsLockIndicator::s_pillTexture;
PHLMONITOR   CapsLockIndicator::s_currentMonitor;
std::optional<CBox> CapsLockIndicator::s_lastGlobalPillBox;

namespace {

struct STrackedTextInputV3 {
    WP<CTextInputV3>     input;
    uint64_t             lastActivity = 0;
    CHyprSignalListener  commit;
    CHyprSignalListener  enable;
    CHyprSignalListener  disable;
    CHyprSignalListener  destroy;
};

struct STrackedTextInputV1 {
    WP<CTextInputV1>      input;
    SP<CWLSurfaceResource> surface;
    uint64_t              lastActivity = 0;
    CHyprSignalListener   commit;
    CHyprSignalListener   enable;
    CHyprSignalListener   disable;
    CHyprSignalListener   destroy;
};

enum class ECaretSource : uint8_t {
    NONE = 0,
    RELAY,
    DIRECT_V1,
    DIRECT_V3,
};

struct SCaretState {
    ECaretSource           source         = ECaretSource::NONE;
    SP<CWLSurfaceResource> focusSurface;
    CBox                   surfaceBoxGlobal;
    CBox                   caretLocal;
    size_t                 directMatches  = 0;
};

struct SCaretProbeResult {
    std::optional<SCaretState> state;
    std::string                reason;
    std::string                detail;
};

struct SCachedCaret {
    CBox                                 caretLocal;
    std::chrono::steady_clock::time_point seenAt;
};

static std::vector<UP<STrackedTextInputV3>> s_trackedTextInputsV3;
static std::vector<UP<STrackedTextInputV1>> s_trackedTextInputsV1;
static uint64_t                             s_textInputActivityCounter = 0;
static CHyprSignalListener                  s_textInputV1NewListener;
static CHyprSignalListener                  s_textInputV3NewListener;
static CHyprSignalListener                  s_mouseButtonListener;
static std::unordered_map<wl_client*, SCachedCaret> s_cachedCaretsByClient;
static std::string                          s_lastDiagnosticKey;
static std::chrono::steady_clock::time_point s_lastDiagnosticLogAt;
static std::chrono::steady_clock::time_point s_lastDiagnosticNotifyAt;
static constexpr auto                       DIAGNOSTIC_LOG_INTERVAL    = std::chrono::seconds(2);
static constexpr auto                       DIAGNOSTIC_NOTIFY_INTERVAL = std::chrono::seconds(3);
static constexpr auto                       CACHED_CARET_TTL           = std::chrono::seconds(2);

static void touchTrackedInput(STrackedTextInputV3* tracked) {
    if (!tracked)
        return;

    tracked->lastActivity = ++s_textInputActivityCounter;
}

static void eraseTrackedInput(STrackedTextInputV3* tracked) {
    if (!tracked)
        return;

    std::erase_if(s_trackedTextInputsV3, [tracked](const auto& other) { return other.get() == tracked; });
}

static void touchTrackedInput(STrackedTextInputV1* tracked) {
    if (!tracked)
        return;

    tracked->lastActivity = ++s_textInputActivityCounter;
}

static void eraseTrackedInput(STrackedTextInputV1* tracked) {
    if (!tracked)
        return;

    std::erase_if(s_trackedTextInputsV1, [tracked](const auto& other) { return other.get() == tracked; });
}

static void rememberCaret(wl_client* client, const CBox& caretLocal) {
    if (!client)
        return;

    s_cachedCaretsByClient[client] = SCachedCaret{
        .caretLocal = caretLocal,
        .seenAt     = std::chrono::steady_clock::now(),
    };
}

static void registerTrackedInputV1(WP<CTextInputV1> weakInput);
static void registerTrackedInputV3(WP<CTextInputV3> weakInput);
static SCaretProbeResult probeCaretState();

class CCapsLockPassElement final : public IPassElement {
  public:
    struct SData {
        SP<CTexture> tex;
        PHLMONITORREF monitor;
        CBox box;
    };

    explicit CCapsLockPassElement(SData&& data) : m_data(std::move(data)) {
    }

    void draw(const CRegion& damage) override {
        (void)damage;

        const auto renderMonitor = g_pHyprOpenGL->m_renderData.pMonitor.lock();
        const auto expectedMonitor = m_data.monitor.lock();
        if (!m_data.tex || !renderMonitor || !expectedMonitor || renderMonitor != expectedMonitor)
            return;

        CHyprOpenGLImpl::STextureRenderData rd{};
        rd.a = 1.0f;
        g_pHyprOpenGL->renderTexture(m_data.tex, m_data.box, rd);
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

static std::string focusSummary() {
    std::ostringstream out;
    const auto         focusSurface = Desktop::focusState()->surface();
    const auto         focusWindow  = Desktop::focusState()->window();

    out << "focus_surface=" << (focusSurface ? "yes" : "no");
    if (focusWindow)
        out << " window_class=\"" << focusWindow->m_class << "\" title=\"" << focusWindow->m_title << "\"";

    return out.str();
}

template <typename T>
static std::string ptrString(T* ptr) {
    std::ostringstream out;
    out << "0x" << std::hex << reinterpret_cast<uintptr_t>(ptr);
    return out.str();
}

static void reportDiagnosticFailure(const std::string& reason, const std::string& detail) {
    const auto now = std::chrono::steady_clock::now();
    const auto key = reason + "|" + detail;

    if (key != s_lastDiagnosticKey || now - s_lastDiagnosticLogAt >= DIAGNOSTIC_LOG_INTERVAL) {
        logDebug("Caret unavailable [" + reason + "] " + detail);
        s_lastDiagnosticLogAt = now;
    }

    if (key != s_lastDiagnosticKey || now - s_lastDiagnosticNotifyAt >= DIAGNOSTIC_NOTIFY_INTERVAL) {
        HyprlandAPI::addNotification(
            PHANDLE,
            "[hyprmac] Caps on, no caret: " + reason,
            CHyprColor{1.0, 0.65, 0.2, 1.0},
            1800
        );
        s_lastDiagnosticNotifyAt = now;
    }

    s_lastDiagnosticKey = key;
}

static void clearDiagnosticFailure(const SCaretState& state) {
    if (s_lastDiagnosticKey.empty())
        return;

    logDebug(
        "Caret recovered via " +
        std::string(
            state.source == ECaretSource::RELAY ? "relay" :
            state.source == ECaretSource::DIRECT_V1 ? "direct-v1" : "direct-v3"
        ) +
        " local=(" + std::to_string(state.caretLocal.x) + "," + std::to_string(state.caretLocal.y) +
        " " + std::to_string(state.caretLocal.w) + "x" + std::to_string(state.caretLocal.h) + ")"
    );

    s_lastDiagnosticKey.clear();
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
    s_textureDirty = true;
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
    s_mouseButtonListener = Event::bus()->m_events.input.mouse.button.listen(
        [](IPointer::SButtonEvent, Event::SCallbackInfo&) { CapsLockIndicator::invalidateIndicator(); }
    );
    if (PROTO::textInputV1) {
        logDebug("Init: existing text-input-v1 objects=" + std::to_string(PROTO::textInputV1->m_clients.size()));
        for (auto& input : PROTO::textInputV1->m_clients) {
            registerTrackedInputV1(input);
        }

        s_textInputV1NewListener = PROTO::textInputV1->m_events.newTextInput.listen(
            [](WP<CTextInputV1> input) { registerTrackedInputV1(input); }
        );
    }
    if (PROTO::textInputV3) {
        logDebug("Init: existing text-input-v3 objects=" + std::to_string(PROTO::textInputV3->m_textInputs.size()));
        for (auto& input : PROTO::textInputV3->m_textInputs) {
            registerTrackedInputV3(input);
        }

        s_textInputV3NewListener = PROTO::textInputV3->m_events.newTextInput.listen(
            [](WP<CTextInputV3> input) { registerTrackedInputV3(input); }
        );
    }

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
    s_mouseButtonListener.reset();
    s_textInputV1NewListener.reset();
    s_textInputV3NewListener.reset();
    s_trackedTextInputsV1.clear();
    s_trackedTextInputsV3.clear();
    s_textInputActivityCounter = 0;
    s_cachedCaretsByClient.clear();
    s_lastDiagnosticKey.clear();
    s_lastDiagnosticLogAt = {};
    s_lastDiagnosticNotifyAt = {};
    s_pillTexture.reset();
    s_textureDirty = true;
    s_lastGlobalPillBox.reset();
}

// ── Event handlers ─────────────────────────────────────────────────────────

void CapsLockIndicator::onRenderPre(const PHLMONITOR& pMonitor) {
    s_currentMonitor = pMonitor;
}

void CapsLockIndicator::onRenderStage(eRenderStage stage) {
    if (stage != RENDER_POST_WINDOWS)
        return;
    refreshCapsState("render_stage");
    if (!s_capsActive)
        return;

    PHLMONITOR pMonitor = s_currentMonitor;
    if (!pMonitor)
        return;

    if (s_textureDirty || !s_pillTexture) {
        logDebug("Building texture");
        buildTexture();
        s_textureDirty = false;
    }
    if (!s_pillTexture) {
        reportDiagnosticFailure("no_texture", focusSummary());
        return;
    }

    const auto globalPillBox = pillBoxGlobal();
    if (!globalPillBox.has_value())
        return;

    PHLMONITOR pillMonitor = g_pCompositor->getMonitorFromVector(globalPillBox->middle());
    if (!pillMonitor) {
        reportDiagnosticFailure(
            "no_monitor",
            "pill_mid=(" + std::to_string(globalPillBox->middle().x) + "," +
                std::to_string(globalPillBox->middle().y) + ") " + focusSummary()
        );
        return;
    }
    if (pillMonitor != pMonitor)
        return;

    CBox pillBox = globalPillBox->copy().translate(-pMonitor->m_position);
    s_lastGlobalPillBox = globalPillBox;

    CCapsLockPassElement::SData data;
    data.tex = s_pillTexture;
    data.monitor = pMonitor;
    data.box = pillBox;

    g_pHyprRenderer->m_renderPass.add(makeUnique<CCapsLockPassElement>(std::move(data)));
}

void CapsLockIndicator::onKeyboardKey(IKeyboard::SKeyEvent ev, Event::SCallbackInfo& info) {
    const uint32_t CAPS_LOCK_KEYCODE = 58;
    (void)info;

    if (ev.keycode == CAPS_LOCK_KEYCODE && ev.state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        damageBox(s_lastGlobalPillBox);
        s_lastGlobalPillBox.reset();
        scheduleFrameAllMonitors();
        return;
    }

    if (s_capsActive)
        invalidateIndicator();
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
    s_textureDirty = true;
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

bool CapsLockIndicator::refreshCapsState(const char* reason) {
    const bool actual = isCapsLockActive();
    if (actual == s_capsActive)
        return false;

    logDebug(
        "Caps Lock state synced to " + std::string(actual ? "ON" : "OFF") +
        " via " + reason
    );
    s_capsActive = actual;
    return true;
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
    const auto probe = probeCaretState();
    if (!probe.state.has_value()) {
        reportDiagnosticFailure(probe.reason, probe.detail);
        return std::nullopt;
    }

    clearDiagnosticFailure(*probe.state);
    rememberCaret(probe.state->focusSurface ? probe.state->focusSurface->client() : nullptr, probe.state->caretLocal);

    return CBox{
        probe.state->surfaceBoxGlobal.x + probe.state->caretLocal.x,
        probe.state->surfaceBoxGlobal.y + probe.state->caretLocal.y,
        std::max(1.0, probe.state->caretLocal.w),
        std::max(1.0, probe.state->caretLocal.h)
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

namespace {

static std::optional<CBox> focusSurfaceBoxGlobal(const SP<CWLSurfaceResource>& focusSurface) {
    if (!focusSurface)
        return std::nullopt;

    const auto hlSurface = Desktop::View::CWLSurface::fromResource(focusSurface);
    if (!hlSurface)
        return std::nullopt;

    return hlSurface->getSurfaceBoxGlobal();
}

static void registerTrackedInputV1(WP<CTextInputV1> weakInput) {
    const auto input = weakInput.lock();
    if (!input)
        return;

    for (auto& tracked : s_trackedTextInputsV1) {
        if (tracked->input.lock() == input)
            return;
    }

    auto tracked = makeUnique<STrackedTextInputV1>();
    auto* raw    = tracked.get();
    raw->input   = weakInput;
    touchTrackedInput(raw);

    raw->commit = input->m_events.onCommit.listen([raw] {
        touchTrackedInput(raw);
        CapsLockIndicator::invalidateIndicator();
    });
    raw->enable = input->m_events.enable.listen([raw](SP<CWLSurfaceResource> surface) {
        raw->surface = surface;
        touchTrackedInput(raw);
        CapsLockIndicator::invalidateIndicator();
    });
    raw->disable = input->m_events.disable.listen([raw] {
        raw->surface.reset();
        touchTrackedInput(raw);
        CapsLockIndicator::invalidateIndicator();
    });
    raw->destroy = input->m_events.destroy.listen([raw] {
        CapsLockIndicator::invalidateIndicator();
        eraseTrackedInput(raw);
    });

    if (input->m_active)
        raw->surface = Desktop::focusState()->surface();

    logDebug(
        "Tracked text-input-v1 client=" + ptrString(input->client()) +
        " active=" + std::string(input->m_active ? "yes" : "no") +
        " cursor=(" + std::to_string(input->m_cursorRectangle.x) + "," + std::to_string(input->m_cursorRectangle.y) +
        " " + std::to_string(input->m_cursorRectangle.w) + "x" + std::to_string(input->m_cursorRectangle.h) + ")"
    );

    s_trackedTextInputsV1.emplace_back(std::move(tracked));
}

static void registerTrackedInputV3(WP<CTextInputV3> weakInput) {
    const auto input = weakInput.lock();
    if (!input)
        return;

    for (auto& tracked : s_trackedTextInputsV3) {
        if (tracked->input.lock() == input)
            return;
    }

    auto tracked = makeUnique<STrackedTextInputV3>();
    auto* raw    = tracked.get();
    raw->input   = weakInput;
    touchTrackedInput(raw);

    raw->commit = input->m_events.onCommit.listen([raw] {
        touchTrackedInput(raw);
        CapsLockIndicator::invalidateIndicator();
    });
    raw->enable = input->m_events.enable.listen([raw] {
        touchTrackedInput(raw);
        CapsLockIndicator::invalidateIndicator();
    });
    raw->disable = input->m_events.disable.listen([raw] {
        touchTrackedInput(raw);
        CapsLockIndicator::invalidateIndicator();
    });
    raw->destroy = input->m_events.destroy.listen([raw] {
        CapsLockIndicator::invalidateIndicator();
        eraseTrackedInput(raw);
    });

    logDebug(
        "Tracked text-input-v3 client=" + ptrString(input->client()) +
        " enabled=" + std::string(input->m_current.enabled.value ? "yes" : "no") +
        " updated=" + std::string(input->m_current.box.updated ? "yes" : "no") +
        " cursor=(" + std::to_string(input->m_current.box.cursorBox.x) + "," + std::to_string(input->m_current.box.cursorBox.y) +
        " " + std::to_string(input->m_current.box.cursorBox.w) + "x" + std::to_string(input->m_current.box.cursorBox.h) + ")"
    );

    s_trackedTextInputsV3.emplace_back(std::move(tracked));
}

static SCaretProbeResult probeCaretState() {
    const auto focusSurface = Desktop::focusState()->surface();
    if (!focusSurface)
        return {
            .reason = "no_focus_surface",
            .detail = focusSummary() + " tracked_v1=" + std::to_string(s_trackedTextInputsV1.size()) +
                      " tracked_v3=" + std::to_string(s_trackedTextInputsV3.size()),
        };

    const auto surfaceBoxGlobal = focusSurfaceBoxGlobal(focusSurface);
    if (!surfaceBoxGlobal.has_value())
        return {
            .reason = "no_surface_box",
            .detail = focusSummary(),
        };

    auto* const relayInput = g_pInputManager->m_relay.getFocusedTextInput();
    const auto  focusClient = focusSurface->client();
    std::string relayStatus = "relay_missing";
    if (relayInput) {
        if (relayInput->focusedSurface() != focusSurface)
            relayStatus = "relay_stale";
        else if (!relayInput->isEnabled())
            relayStatus = "relay_disabled";
        else if (!relayInput->hasCursorRectangle())
            relayStatus = "relay_no_cursor_rect";
        else
            return {
                .state = SCaretState{
                    .source           = ECaretSource::RELAY,
                    .focusSurface     = focusSurface,
                    .surfaceBoxGlobal = *surfaceBoxGlobal,
                    .caretLocal       = relayInput->cursorBox(),
                },
            };
    }

    CTextInputV1* bestInputV1    = nullptr;
    uint64_t      bestActivityV1 = 0;
    size_t        directMatchesV1 = 0;
    size_t        rectMatchesV1   = 0;

    for (auto& tracked : s_trackedTextInputsV1) {
        const auto input = tracked->input.lock();
        if (!input || !input->m_active || tracked->surface != focusSurface)
            continue;

        directMatchesV1++;

        const auto rect = input->m_cursorRectangle;
        if (rect.w <= 0 && rect.h <= 0)
            continue;
        rectMatchesV1++;

        if (!bestInputV1 || tracked->lastActivity >= bestActivityV1) {
            bestInputV1    = input.get();
            bestActivityV1 = tracked->lastActivity;
        }
    }

    if (bestInputV1) {
        return {
            .state = SCaretState{
                .source           = ECaretSource::DIRECT_V1,
                .focusSurface     = focusSurface,
                .surfaceBoxGlobal = *surfaceBoxGlobal,
                .caretLocal       = bestInputV1->m_cursorRectangle,
                .directMatches    = directMatchesV1,
            },
        };
    }

    CTextInputV3* bestInput         = nullptr;
    uint64_t      bestActivity      = 0;
    size_t        directMatches     = 0;
    size_t        updatedMatches    = 0;
    size_t        enabledMatches    = 0;
    size_t        updatedDisabled   = 0;

    for (auto& tracked : s_trackedTextInputsV3) {
        const auto input = tracked->input.lock();
        if (!input || input->client() != focusSurface->client())
            continue;

        directMatches++;

        if (!input->m_current.box.updated)
            continue;
        updatedMatches++;

        if (input->m_current.enabled.value)
            enabledMatches++;
        else
            updatedDisabled++;

        if (!bestInput ||
            (input->m_current.enabled.value && !bestInput->m_current.enabled.value) ||
            (input->m_current.enabled.value == bestInput->m_current.enabled.value && tracked->lastActivity >= bestActivity)) {
            bestInput    = input.get();
            bestActivity = tracked->lastActivity;
        }
    }

    if (!bestInput)
        if (const auto cached = s_cachedCaretsByClient.find(focusClient);
            cached != s_cachedCaretsByClient.end() &&
            std::chrono::steady_clock::now() - cached->second.seenAt <= CACHED_CARET_TTL) {
            return {
                .state = SCaretState{
                    .source           = ECaretSource::DIRECT_V3,
                    .focusSurface     = focusSurface,
                    .surfaceBoxGlobal = *surfaceBoxGlobal,
                    .caretLocal       = cached->second.caretLocal,
                    .directMatches    = directMatches,
                },
            };
        }

    if (!bestInput)
        return {
            .reason = "no_v3_candidate",
            .detail = relayStatus +
                      " focus_client=" + ptrString(focusClient) +
                      " tracked_v1=" + std::to_string(s_trackedTextInputsV1.size()) +
                      " tracked_v3=" + std::to_string(s_trackedTextInputsV3.size()) +
                      " v1_matches=" + std::to_string(directMatchesV1) +
                      " v1_rect_matches=" + std::to_string(rectMatchesV1) +
                      " v3_matches=" + std::to_string(directMatches) +
                      " v3_updated_matches=" + std::to_string(updatedMatches) +
                      " v3_enabled_matches=" + std::to_string(enabledMatches) +
                      " v3_updated_disabled=" + std::to_string(updatedDisabled) + " " + focusSummary(),
        };

    return {
        .state = SCaretState{
            .source           = ECaretSource::DIRECT_V3,
            .focusSurface     = focusSurface,
            .surfaceBoxGlobal = *surfaceBoxGlobal,
            .caretLocal       = bestInput->m_current.box.cursorBox,
            .directMatches    = directMatches,
        },
    };
}

} // namespace
