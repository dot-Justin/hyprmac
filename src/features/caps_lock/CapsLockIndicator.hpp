#pragma once

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprutils/signal/Signal.hpp>
#include <optional>
#include <string>

using namespace Hyprutils::Signal;
using namespace Hyprutils::Memory;

class CapsLockIndicator {
  public:
    static void init();
    static void destroy();

  private:
    static bool         s_capsActive;
    static bool         s_textureDirty;
    static SP<CTexture> s_pillTexture;
    static PHLMONITOR   s_currentMonitor;
    static std::optional<CBox> s_lastGlobalPillBox;

    // Listener handles — must stay alive to keep callbacks registered
    static CHyprSignalListener s_renderPreListener;
    static CHyprSignalListener s_renderStageListener;
    static CHyprSignalListener s_configReloadedListener;
    static CHyprSignalListener s_keyboardKeyListener;
    static CHyprSignalListener s_keyboardFocusListener;
    static CHyprSignalListener s_windowActiveListener;

    static void onRenderPre(const PHLMONITOR& pMonitor);
    static void onRenderStage(eRenderStage stage);
    static void onKeyboardKey(IKeyboard::SKeyEvent ev, Event::SCallbackInfo& info);
    static void onKeyboardFocus(SP<CWLSurfaceResource> surface);
    static void onWindowActive(PHLWINDOW window);
    static void onConfigReloaded();

    static bool isCapsLockActive();
    static void scheduleFrameAllMonitors();
    static void buildTexture();
    static void invalidateIndicator();
    static std::optional<CBox> caretBoxGlobal();
    static std::optional<CBox> pillBoxGlobal();
    static void damageBox(const std::optional<CBox>& box);
    static int  configInt(const std::string& key);
};
