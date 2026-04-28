#pragma once

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprutils/signal/Signal.hpp>
#include <hyprutils/memory/WeakPtr.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

using namespace Hyprutils::Signal;
using namespace Hyprutils::Memory;

class VolumeController {
  public:
    static void            init();
    static void            destroy();
    static SDispatchResult dispatch(const std::string& args);

    // ── types (public so anonymous-namespace helpers can use them) ────────────
    enum class Phase { HIDDEN, INTRO, HOLD, OUTRO };
    struct Pose { float opacity; float translateY; };

    // ── worker → compositor sync (public so syncActualState() can write them) ─
    static std::atomic<int>  s_actualStep;
    static std::atomic<bool> s_actualMuted;
    static std::atomic<uint64_t> s_actualGeneration;
    static std::atomic<bool> s_syncReady;

  private:
    // ── render-thread state ──────────────────────────────────────────────────
    static PHLMONITOR   s_currentMonitor;
    static bool         s_texDirty;
    static SP<CTexture> s_tex;
    static std::optional<CBox> s_lastOsdBoxGlobal;

    // ── volume state (compositor thread, optimistic) ─────────────────────────
    static int         s_stepIndex;       // 0..16  (0 = silent, 16 = 100 %)
    static bool        s_muted;
    static int         s_lastNonZeroStep; // remembered level for unmute/up-while-muted
    static std::string s_deviceLabel;

    // ── desired state (compositor → worker) ──────────────────────────────────
    static std::mutex   s_desiredStateMutex;
    static int          s_desiredStepValue;
    static bool         s_desiredMutedValue;
    static uint64_t     s_desiredGenerationValue;
    static std::atomic<uint64_t> s_desiredGeneration;

    // ── animation ────────────────────────────────────────────────────────────
    static Phase s_phase;
    static std::chrono::steady_clock::time_point s_phaseAt;
    static Pose  s_startPose;

    // ── worker thread (wpctl I/O) ────────────────────────────────────────────
    static std::thread       s_worker;
    static std::atomic<bool> s_running;
    static int               s_pipeRead;
    static int               s_pipeWrite;

    // ── event handles ────────────────────────────────────────────────────────
    static CHyprSignalListener s_renderPreListener;
    static CHyprSignalListener s_renderStageListener;
    static CHyprSignalListener s_configReloadedListener;

    // ── internal helpers ─────────────────────────────────────────────────────
    static void onRenderPre(const PHLMONITOR& mon);
    static void onRenderStage(eRenderStage stage);
    static void onConfigReloaded();

    static void showOrExtend();
    static void scheduleFrameFocusedMonitor();
    static void buildTexture();              // call only inside GL context
    static Pose currentPose();               // interpolated from state machine
    static void enterPhase(Phase p);         // samples pose, starts new phase
    static std::optional<CBox> currentOsdBoxGlobal();
    static void invalidateOsd();
    static void damageBox(const std::optional<CBox>& box);
    static bool syncDisplayStateFromSystem();

    static void        workerMain();
    static std::string readDeviceLabel();    // synchronous at init time
    static int         configInt(const std::string& key);
};
