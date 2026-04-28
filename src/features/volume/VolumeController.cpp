#include "VolumeController.hpp"
#include "globals.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <aquamarine/output/Output.hpp>

#include <cairo/cairo.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <unordered_map>

// ── Static member definitions ──────────────────────────────────────────────

PHLMONITOR   VolumeController::s_currentMonitor;
bool         VolumeController::s_texDirty       = true;
SP<CTexture> VolumeController::s_tex;
std::optional<CBox> VolumeController::s_lastOsdBoxGlobal;

int          VolumeController::s_stepIndex       = 8;
bool         VolumeController::s_muted           = false;
int          VolumeController::s_lastNonZeroStep = 8;
std::string  VolumeController::s_deviceLabel;

std::mutex   VolumeController::s_desiredStateMutex;
int          VolumeController::s_desiredStepValue       = 8;
bool         VolumeController::s_desiredMutedValue      = false;
uint64_t     VolumeController::s_desiredGenerationValue = 1;
std::atomic<uint64_t> VolumeController::s_desiredGeneration{1};

VolumeController::Phase VolumeController::s_phase   = VolumeController::Phase::HIDDEN;
std::chrono::steady_clock::time_point VolumeController::s_phaseAt;
VolumeController::Pose VolumeController::s_startPose = {0.f, -12.f};

std::thread       VolumeController::s_worker;
std::atomic<bool> VolumeController::s_running{false};
int               VolumeController::s_pipeRead  = -1;
int               VolumeController::s_pipeWrite = -1;

std::atomic<int>  VolumeController::s_actualStep{8};
std::atomic<bool> VolumeController::s_actualMuted{false};
std::atomic<uint64_t> VolumeController::s_actualGeneration{1};
std::atomic<bool> VolumeController::s_syncReady{false};

CHyprSignalListener VolumeController::s_renderPreListener;
CHyprSignalListener VolumeController::s_renderStageListener;
CHyprSignalListener VolumeController::s_configReloadedListener;

// ── OSD geometry constants ─────────────────────────────────────────────────
// All in logical pixels. Texture is built at exactly these dimensions (no HiDPI
// scaling — same approach as CapsLockIndicator, avoids cairo_scale/Pango pitfalls).

static constexpr double OSD_W        = 320.0;
static constexpr double OSD_H        =  80.0;
static constexpr double OSD_R_MARGIN =  16.0; // gap from right edge of monitor
static constexpr double OSD_TOP_GAP  =  50.0; // gap below reserved-top (Waybar)
static constexpr int    OSD_ROUND    =  22;   // pill corner radius for renderRect

// Cairo content layout (logical pixels within the 320×80 canvas)
static constexpr double PAD          =  18.0; // left/right padding
static constexpr double ICON_SIZE    =  14.0; // speaker icon rendered size
static constexpr double ICON_GAP     =   8.0; // gap between icon and bar
static constexpr double ROW1_MID     =  22.0; // vertical centre of label row
static constexpr double ROW2_MID     =  61.0; // vertical centre of bar/icon row
static constexpr double BAR_X        = PAD + ICON_SIZE + ICON_GAP; // 40
static constexpr double BAR_R_X      = OSD_W - PAD - ICON_SIZE - ICON_GAP; // 280
static constexpr double BAR_W        = BAR_R_X - BAR_X;    // 240
static constexpr double BAR_H        =   5.0;
static constexpr double BAR_R        =   2.5;

// Animation poses
static constexpr VolumeController::Pose kHidden = {0.f, -12.f};
static constexpr VolumeController::Pose kFull   = {1.f,   0.f};
static constexpr auto kIntroDuration = std::chrono::milliseconds(220);
static constexpr auto kHoldDuration  = std::chrono::milliseconds(1100);
static constexpr auto kOutroDuration = std::chrono::milliseconds(200);

// ── Anonymous namespace ────────────────────────────────────────────────────

namespace {

// Cubic-bezier (0.22, 1, 0.36, 1): fast-in, eases to rest.
// Binary search on parameter s so that x(s) == t, then return y(s).
float bezierEase(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    float lo = 0.f, hi = 1.f, s = t;
    for (int i = 0; i < 10; ++i) {
        float x = 3.f*s*(1-s)*(1-s)*0.22f + 3.f*s*s*(1-s)*0.36f + s*s*s;
        if (std::abs(x - t) < 5e-5f) break;
        if (x < t) lo = s; else hi = s;
        s = (lo + hi) * 0.5f;
    }
    float y = 3.f*s*(1-s)*(1-s)*1.f + 3.f*s*s*(1-s)*1.f + s*s*s;
    return std::clamp(y, 0.0f, 1.0f);
}

VolumeController::Pose lerpPose(const VolumeController::Pose& a,
                                 const VolumeController::Pose& b, float t) {
    float e = bezierEase(t);
    return {a.opacity    + (b.opacity    - a.opacity)    * e,
            a.translateY + (b.translateY - a.translateY) * e};
}

// ── Cairo primitives ───────────────────────────────────────────────────────

void roundedRectPath(cairo_t* cr, double x, double y, double w, double h, double r) {
    cairo_new_path(cr);
    cairo_arc(cr, x + r,     y + r,     r, M_PI,       M_PI * 1.5);
    cairo_arc(cr, x + w - r, y + r,     r, M_PI * 1.5, 0.0       );
    cairo_arc(cr, x + w - r, y + h - r, r, 0.0,        M_PI * 0.5);
    cairo_arc(cr, x + r,     y + h - r, r, M_PI * 0.5, M_PI      );
    cairo_close_path(cr);
}

// Speaker body — 256×256 viewBox. Caller sets CTM (translate + scale) and colour.
void drawSpeakerBody(cairo_t* cr) {
    cairo_new_path(cr);
    cairo_move_to(cr,  68,  96);
    cairo_line_to(cr,  68, 160);
    cairo_line_to(cr, 116, 160);
    cairo_line_to(cr, 196, 212);
    cairo_line_to(cr, 196,  44);
    cairo_line_to(cr, 116,  96);
    cairo_close_path(cr);
    cairo_fill(cr);
}

void drawSpeakerWaves(cairo_t* cr) {
    cairo_save(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_width(cr, 20.0);
    cairo_arc(cr, 196, 128, 30, -M_PI * 0.36, M_PI * 0.36);
    cairo_stroke(cr);
    cairo_arc(cr, 196, 128, 56, -M_PI * 0.43, M_PI * 0.43);
    cairo_stroke(cr);
    cairo_restore(cr);
}

// ── Combined OSD pass element ──────────────────────────────────────────────
//
// Renders background (via renderRect with optional live blur) AND content
// texture in a single draw() call.  Having both in one element ensures they
// always composite together atomically and that needsLiveBlur() covers the
// same region as the texture.

class CVolumeOsdElement final : public IPassElement {
  public:
    struct SData {
        SP<CTexture>  tex;
        PHLMONITORREF monitor;
        CBox          box;
        float         alpha;
    };

    explicit CVolumeOsdElement(SData&& d) : m_data(std::move(d)) {}

    void draw(const CRegion& damage) override {
        (void)damage;
        auto renderMon   = g_pHyprOpenGL->m_renderData.pMonitor.lock();
        auto expectedMon = m_data.monitor.lock();
        if (!renderMon || !expectedMon || renderMon != expectedMon) return;

        // ── Content texture ──────────────────────────────────────────────
        if (!m_data.tex) return;
        CHyprOpenGLImpl::STextureRenderData rd{};
        rd.damage = nullptr;
        rd.a = m_data.alpha;
        g_pHyprOpenGL->renderTexture(m_data.tex, m_data.box, rd);
    }

    // Report live-blur need so the render pass inserts a pre-blur element
    // before us, giving the background its frosted-glass appearance.
    bool needsLiveBlur()       override { return false; }
    bool needsPrecomputeBlur() override { return false; }
    bool undiscardable()       override { return true; }
    bool disableSimplification() override { return true; }

    std::optional<CBox> boundingBox() override { return m_data.box.copy().expand(12.0); }
    CRegion opaqueRegion()     override { return {}; }
    const char* passName()     override { return "CVolumeOsdElement"; }

  private:
    SData m_data;
};

// ── Worker-thread helpers ──────────────────────────────────────────────────

bool boxesDiffer(const CBox& a, const CBox& b) {
    return a.x != b.x || a.y != b.y || a.w != b.w || a.h != b.h;
}

bool runCommand(const std::string& command) {
    return system(command.c_str()) == 0; // NOLINT
}

bool querySystemState(int& step, bool& muted) {
    FILE* f = popen("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null", "r");
    if (!f)
        return false;

    char buf[128] = {};
    if (!fgets(buf, sizeof(buf), f)) {
        pclose(f);
        return false;
    }
    pclose(f);

    muted = (strstr(buf, "[MUTED]") != nullptr);
    float vol = 0.5f;
    sscanf(buf, "Volume: %f", &vol);
    step = std::clamp(static_cast<int>(std::round(vol * 16.0f)), 0, 16);
    return true;
}

bool applyDesiredState(int step, bool muted) {
    bool ok = true;

    if (muted) {
        ok &= runCommand("wpctl set-mute @DEFAULT_AUDIO_SINK@ 1 >/dev/null 2>&1");
        return ok;
    }

    ok &= runCommand("wpctl set-mute @DEFAULT_AUDIO_SINK@ 0 >/dev/null 2>&1");

    char volumeCmd[160];
    std::snprintf(volumeCmd, sizeof(volumeCmd),
                  "wpctl set-volume @DEFAULT_AUDIO_SINK@ %.4f >/dev/null 2>&1",
                  step / 16.0);
    ok &= runCommand(volumeCmd);

    int actualStep = step;
    bool actualMuted = muted;
    if (!querySystemState(actualStep, actualMuted))
        return ok;

    if (actualMuted) {
        ok &= runCommand("wpctl set-mute @DEFAULT_AUDIO_SINK@ 0 >/dev/null 2>&1");
        ok &= runCommand(volumeCmd);
    }

    return ok;
}

void syncActualState(uint64_t generation) {
    int step = 8;
    bool muted = false;
    if (!querySystemState(step, muted))
        return;

    VolumeController::s_actualStep.store(step,  std::memory_order_release);
    VolumeController::s_actualMuted.store(muted, std::memory_order_release);
    VolumeController::s_actualGeneration.store(generation, std::memory_order_release);
    VolumeController::s_syncReady.store(true,   std::memory_order_release);
}

} // namespace

// ── Render callbacks ───────────────────────────────────────────────────────

void VolumeController::onRenderPre(const PHLMONITOR& mon) {
    s_currentMonitor = mon;
}

void VolumeController::onRenderStage(eRenderStage stage) {
    if (stage != RENDER_POST_WINDOWS) return;
    if (!configInt("plugin:hyprmac:volume_osd_enabled")) return;

    // Consume worker sync (compositor thread is sole reader of these atomics)
    if (s_syncReady.exchange(false, std::memory_order_acq_rel)) {
        const uint64_t actualGeneration  = s_actualGeneration.load(std::memory_order_acquire);
        const uint64_t desiredGeneration = s_desiredGeneration.load(std::memory_order_acquire);
        if (actualGeneration == desiredGeneration) {
            int  newStep  = s_actualStep.load(std::memory_order_acquire);
            bool newMuted = s_actualMuted.load(std::memory_order_acquire);
            if (newStep != s_stepIndex || newMuted != s_muted) {
                s_stepIndex = newStep;
                s_muted     = newMuted;
                if (s_stepIndex > 0)
                    s_lastNonZeroStep = s_stepIndex;
                s_texDirty = true;
                invalidateOsd();
            }
        }
    }

    // Only render on the focused monitor
    auto pMonitor = s_currentMonitor;
    if (!pMonitor) return;
    auto focusMon = Desktop::focusState()->monitor();
    if (!focusMon || pMonitor.get() != focusMon.get()) return;

    const auto now = std::chrono::steady_clock::now();
    if (s_phase == Phase::INTRO && now - s_phaseAt >= kIntroDuration)
        enterPhase(Phase::HOLD);
    if (s_phase == Phase::HOLD && now - s_phaseAt >= kHoldDuration)
        enterPhase(Phase::OUTRO);
    if (s_phase == Phase::OUTRO && now - s_phaseAt >= kOutroDuration) {
        damageBox(s_lastOsdBoxGlobal);
        s_lastOsdBoxGlobal.reset();
        s_phase = Phase::HIDDEN;
        return;
    }
    if (s_phase == Phase::HIDDEN)
        return;

    Pose pose = currentPose();
    auto globalBox = currentOsdBoxGlobal();
    if (!globalBox.has_value()) {
        damageBox(s_lastOsdBoxGlobal);
        s_lastOsdBoxGlobal.reset();
        return;
    }

    if (!s_lastOsdBoxGlobal.has_value() || boxesDiffer(*s_lastOsdBoxGlobal, *globalBox)) {
        damageBox(s_lastOsdBoxGlobal);
        damageBox(globalBox);
        s_lastOsdBoxGlobal = globalBox;
    }

    // Keep the full OSD damaged while visible so unrelated partial-damage
    // updates (screenshots, overlays, blur churn) cannot redraw only slices.
    damageBox(globalBox);

    // Build Cairo texture inside active GL context
    if (s_texDirty || !s_tex) {
        buildTexture();
        s_texDirty = false;
    }

    // ── OSD geometry (monitor-local logical coords) ────────────────────────
    CBox osdBox = globalBox->copy().translate(-pMonitor->m_position);

    // ── Single combined pass element ───────────────────────────────────────
    CVolumeOsdElement::SData data;
    data.tex         = s_tex;
    data.monitor     = pMonitor;
    data.box         = osdBox;
    data.alpha       = pose.opacity;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CVolumeOsdElement>(std::move(data)));

    // ── Animation / frame scheduling ───────────────────────────────────────
    g_pHyprRenderer->damageMonitor(pMonitor);
    switch (s_phase) {
    case Phase::INTRO:
    case Phase::HOLD:
    case Phase::OUTRO:
        scheduleFrameFocusedMonitor();
        break;
    case Phase::HIDDEN:
        break;
    }
}

void VolumeController::onConfigReloaded() {
    s_texDirty = true;
    s_tex.reset();
    invalidateOsd();
}

// ── Animation state machine ────────────────────────────────────────────────

VolumeController::Pose VolumeController::currentPose() {
    if (s_phase == Phase::HIDDEN) return kHidden;
    if (s_phase == Phase::HOLD)   return kFull;

    const auto elapsed = std::chrono::steady_clock::now() - s_phaseAt;

    if (s_phase == Phase::INTRO) {
        float t = std::min(1.0f,
            static_cast<float>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()) /
                static_cast<float>(std::chrono::duration_cast<std::chrono::microseconds>(kIntroDuration).count()));
        return lerpPose(s_startPose, kFull, t);
    }
    // OUTRO
    float t = std::min(1.0f,
        static_cast<float>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()) /
            static_cast<float>(std::chrono::duration_cast<std::chrono::microseconds>(kOutroDuration).count()));
    return lerpPose(s_startPose, kHidden, t);
}

void VolumeController::enterPhase(Phase p) {
    s_startPose = currentPose(); // capture current interpolated state for smooth interrupts
    s_phase     = p;
    s_phaseAt   = std::chrono::steady_clock::now();
}

void VolumeController::showOrExtend() {
    if (s_phase == Phase::HIDDEN || s_phase == Phase::OUTRO)
        enterPhase(Phase::INTRO);
    else if (s_phase == Phase::HOLD)
        s_phaseAt = std::chrono::steady_clock::now();
    // INTRO: no-op; hold timer resets naturally when INTRO completes
}

bool VolumeController::syncDisplayStateFromSystem() {
    int step = s_stepIndex;
    bool muted = s_muted;
    if (!querySystemState(step, muted))
        return false;

    s_stepIndex = step;
    s_muted = muted;
    if (s_stepIndex > 0)
        s_lastNonZeroStep = s_stepIndex;
    s_texDirty = true;
    return true;
}

std::optional<CBox> VolumeController::currentOsdBoxGlobal() {
    if (s_phase == Phase::HIDDEN)
        return std::nullopt;

    auto mon = Desktop::focusState()->monitor();
    if (!mon)
        return std::nullopt;

    const Pose pose = currentPose();
    const double baseX = mon->m_position.x + mon->m_size.x - OSD_W - OSD_R_MARGIN;
    const double baseY = mon->m_position.y + mon->m_reservedArea.top() + OSD_TOP_GAP + pose.translateY;
    return CBox{baseX, baseY, OSD_W, OSD_H};
}

void VolumeController::invalidateOsd() {
    const auto nextBox = currentOsdBoxGlobal();
    damageBox(s_lastOsdBoxGlobal);
    damageBox(nextBox);
    s_lastOsdBoxGlobal = nextBox;
}

void VolumeController::damageBox(const std::optional<CBox>& box) {
    if (!box.has_value())
        return;

    g_pHyprRenderer->damageBox(box->copy().expand(12.0));
}

void VolumeController::scheduleFrameFocusedMonitor() {
    auto mon = Desktop::focusState()->monitor();
    if (!mon) return;
    g_pCompositor->scheduleFrameForMonitor(
        mon, Aquamarine::IOutput::AQ_SCHEDULE_CLIENT_UNKNOWN);
}

// ── Texture building ───────────────────────────────────────────────────────
// Built at OSD_W × OSD_H logical pixels (no HiDPI multiplication).
// renderRect handles the background pill + blur; this texture is transparent
// everywhere except the content (label, bar, icons).

void VolumeController::buildTexture() {
    constexpr int W = static_cast<int>(OSD_W);
    constexpr int H = static_cast<int>(OSD_H);

    auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
    auto* cr      = cairo_create(surface);

    // Clear to fully transparent
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_restore(cr);

    const int64_t rawColor = configInt("plugin:hyprmac:osd_bg_color");
    const double bgR = ((rawColor >> 24) & 0xFF) / 255.0;
    const double bgG = ((rawColor >> 16) & 0xFF) / 255.0;
    const double bgB = ((rawColor >>  8) & 0xFF) / 255.0;
    const double bgA = ((rawColor >>  0) & 0xFF) / 255.0;

    // Bake the entire panel into one texture for stability.
    roundedRectPath(cr, 0.0, 0.0, OSD_W, OSD_H, static_cast<double>(OSD_ROUND));
    cairo_set_source_rgba(cr, bgR, bgG, bgB, bgA);
    cairo_fill(cr);

    // ── Row 1: device label ────────────────────────────────────────────────
    {
        PangoLayout*          layout = pango_cairo_create_layout(cr);
        PangoFontDescription* desc   = pango_font_description_new();
        pango_font_description_set_family(desc, "SF Pro Display, Inter, sans-serif");
        pango_font_description_set_weight(desc, PANGO_WEIGHT_SEMIBOLD);
        pango_font_description_set_absolute_size(desc, static_cast<int>(14.0 * PANGO_SCALE));
        pango_layout_set_font_description(layout, desc);
        pango_font_description_free(desc);

        pango_layout_set_text(layout, s_deviceLabel.c_str(), -1);
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        pango_layout_set_width(layout, static_cast<int>((W - 2 * PAD) * PANGO_SCALE));

        int pw = 0, ph = 0;
        pango_layout_get_size(layout, &pw, &ph);
        double textH = static_cast<double>(ph) / PANGO_SCALE;
        double textY = ROW1_MID - textH * 0.5;

        cairo_move_to(cr, PAD, std::max(0.0, textY));
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.92);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
    }

    // ── Row 2: speaker icons + volume bar ─────────────────────────────────
    constexpr double ICON_SC  = ICON_SIZE / 256.0; // 256-space → ICON_SIZE px
    constexpr double ICON_TOP = ROW2_MID - ICON_SIZE * 0.5;

    // Left icon (speaker-simple-none)
    cairo_save(cr);
    cairo_translate(cr, PAD, ICON_TOP);
    cairo_scale(cr, ICON_SC, ICON_SC);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.80);
    drawSpeakerBody(cr);
    cairo_restore(cr);

    // Right icon (speaker-simple-high)
    cairo_save(cr);
    cairo_translate(cr, OSD_W - PAD - ICON_SIZE, ICON_TOP);
    cairo_scale(cr, ICON_SC, ICON_SC);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.80);
    drawSpeakerBody(cr);
    drawSpeakerWaves(cr);
    cairo_restore(cr);

    // Bar trough (always visible)
    const double barY = ROW2_MID - BAR_H * 0.5;
    roundedRectPath(cr, BAR_X, barY, BAR_W, BAR_H, BAR_R);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.12);
    cairo_fill(cr);

    // Bar fill (hidden when muted or at step 0)
    if (!s_muted && s_stepIndex > 0) {
        double fillW = std::round((s_stepIndex / 16.0) * BAR_W);
        double fillR = std::min(BAR_R, fillW * 0.5);
        if (fillW > 0.0 && fillR > 0.0) {
            roundedRectPath(cr, BAR_X, barY, fillW, BAR_H, fillR);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.96);
            cairo_fill(cr);
        }
    }

    cairo_surface_flush(surface);
    s_tex = g_pHyprOpenGL->texFromCairo(surface);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

// ── Dispatcher ────────────────────────────────────────────────────────────

SDispatchResult VolumeController::dispatch(const std::string& args) {
    if (!configInt("plugin:hyprmac:volume_osd_enabled"))
        return SDispatchResult{};

    // Keep mute-sensitive actions anchored to the real backend state so an
    // external mute/unmute does not leave our optimistic state inverted.
    syncDisplayStateFromSystem();

    if (args == "up") {
        if (s_muted) { s_muted = false; s_stepIndex = s_lastNonZeroStep; }
        s_stepIndex       = std::min(16, s_stepIndex + 1);
        s_lastNonZeroStep = s_stepIndex;
        s_texDirty = true;

    } else if (args == "down") {
        if (s_muted) { s_muted = false; s_stepIndex = s_lastNonZeroStep; }
        s_stepIndex = std::max(0, s_stepIndex - 1);
        if (s_stepIndex > 0) s_lastNonZeroStep = s_stepIndex;
        s_texDirty = true;

    } else if (args == "toggle-mute") {
        s_muted = !s_muted;
        if (!s_muted && s_stepIndex == 0) s_stepIndex = s_lastNonZeroStep;
        s_texDirty = true;
    } else {
        return SDispatchResult{};
    }

    {
        std::lock_guard<std::mutex> lock(s_desiredStateMutex);
        s_desiredStepValue  = s_stepIndex;
        s_desiredMutedValue = s_muted;
        ++s_desiredGenerationValue;
        s_desiredGeneration.store(s_desiredGenerationValue, std::memory_order_release);
    }

    showOrExtend();
    invalidateOsd();
    if (s_pipeWrite >= 0) {
        const char wake = 1;
        write(s_pipeWrite, &wake, 1);
    }
    return SDispatchResult{};
}

// ── Worker thread ──────────────────────────────────────────────────────────

void VolumeController::workerMain() {
    while (s_running.load(std::memory_order_acquire)) {
        struct pollfd pfd{s_pipeRead, POLLIN, 0};
        if (poll(&pfd, 1, 50) <= 0) continue;

        char buf[64];
        ssize_t n = read(s_pipeRead, buf, sizeof(buf));
        if (n <= 0) continue;

        while (poll(&pfd, 1, 0) > 0) {
            n = read(s_pipeRead, buf, sizeof(buf));
            if (n <= 0)
                break;
        }
        if (!s_running.load(std::memory_order_acquire))
            break;

        int targetStep = 0;
        bool targetMuted = false;
        uint64_t targetGeneration = 0;
        {
            std::lock_guard<std::mutex> lock(s_desiredStateMutex);
            targetStep = s_desiredStepValue;
            targetMuted = s_desiredMutedValue;
            targetGeneration = s_desiredGenerationValue;
        }

        if (!applyDesiredState(targetStep, targetMuted))
            continue;

        syncActualState(targetGeneration);
    }
}

// ── Device label ───────────────────────────────────────────────────────────

std::string VolumeController::readDeviceLabel() {
    FILE* f = popen(
        "wpctl inspect @DEFAULT_AUDIO_SINK@ 2>/dev/null"
        " | grep 'node.description'"
        " | sed \"s/.*node.description = \\\"//; s/\\\".*//\"",
        "r");
    if (!f) return "Output";
    char buf[256] = {};
    if (!fgets(buf, sizeof(buf), f)) { pclose(f); return "Output"; }
    pclose(f);

    std::string label{buf};
    while (!label.empty() && (label.back() == '\n' || label.back() == '\r'))
        label.pop_back();
    if (label.empty()) return "Output";

    if (label == "Built-in Audio Analog Stereo")  return "Built-in Speakers";
    if (label == "Built-in Audio Digital Stereo") return "Built-in Speakers";
    if (label.find("Headphone") != std::string::npos) return "Headphones";
    if (label.find("Speaker")   != std::string::npos) return "Speakers";
    return label;
}

// ── Lifecycle ──────────────────────────────────────────────────────────────

void VolumeController::init() {
    // Bootstrap volume state synchronously (one-time at plugin load)
    {
        FILE* f = popen("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null", "r");
        if (f) {
            char buf[128] = {};
            fgets(buf, sizeof(buf), f);
            pclose(f);
            s_muted = (strstr(buf, "[MUTED]") != nullptr);
            float vol = 0.5f;
            sscanf(buf, "Volume: %f", &vol);
            s_stepIndex = std::clamp(static_cast<int>(std::round(vol * 16.0f)), 0, 16);
            s_lastNonZeroStep = (s_stepIndex > 0) ? s_stepIndex : 8;
        }
    }
    s_deviceLabel = readDeviceLabel();
    s_texDirty = true;
    s_tex.reset();
    s_lastOsdBoxGlobal.reset();
    s_phase = Phase::HIDDEN;
    s_startPose = kHidden;
    {
        std::lock_guard<std::mutex> lock(s_desiredStateMutex);
        s_desiredStepValue = s_stepIndex;
        s_desiredMutedValue = s_muted;
        s_desiredGenerationValue = 1;
    }
    s_desiredGeneration.store(1, std::memory_order_release);
    s_actualStep.store(s_stepIndex, std::memory_order_release);
    s_actualMuted.store(s_muted, std::memory_order_release);
    s_actualGeneration.store(1, std::memory_order_release);
    s_syncReady.store(false, std::memory_order_release);

    // Worker pipe (same pattern as VolumeSound)
    int fds[2];
    if (pipe(fds) != 0) return;
    s_pipeRead  = fds[0];
    s_pipeWrite = fds[1];
    fcntl(s_pipeWrite, F_SETFL, O_NONBLOCK);

    s_running.store(true, std::memory_order_release);
    s_worker = std::thread(workerMain);

    s_renderPreListener = Event::bus()->m_events.render.pre.listen(
        [](const PHLMONITOR& m) { onRenderPre(m); });
    s_renderStageListener = Event::bus()->m_events.render.stage.listen(
        [](eRenderStage s) { onRenderStage(s); });
    s_configReloadedListener = Event::bus()->m_events.config.reloaded.listen(
        []() { onConfigReloaded(); });
}

void VolumeController::destroy() {
    s_renderPreListener.reset();
    s_renderStageListener.reset();
    s_configReloadedListener.reset();

    s_running.store(false, std::memory_order_release);
    if (s_pipeWrite >= 0) { const char w = 0; write(s_pipeWrite, &w, 1); }
    if (s_worker.joinable()) s_worker.join();

    if (s_pipeRead  >= 0) { close(s_pipeRead);  s_pipeRead  = -1; }
    if (s_pipeWrite >= 0) { close(s_pipeWrite); s_pipeWrite = -1; }

    s_tex.reset();
    s_currentMonitor.reset();
    s_lastOsdBoxGlobal.reset();
    s_phase = Phase::HIDDEN;
    s_startPose = kHidden;
}

// ── Config helper ──────────────────────────────────────────────────────────

int VolumeController::configInt(const std::string& key) {
    static std::unordered_map<std::string, Hyprlang::INT* const*> cache;
    auto it = cache.find(key);
    if (it == cache.end()) {
        auto* cv = HyprlandAPI::getConfigValue(PHANDLE, key);
        if (!cv) return 0;
        cache[key] = (Hyprlang::INT* const*)cv->getDataStaticPtr();
        it         = cache.find(key);
    }
    return (int)**it->second;
}
