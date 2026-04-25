#pragma once

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprutils/signal/Signal.hpp>
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

    // Listener handles — must stay alive to keep callbacks registered
    static CHyprSignalListener              s_renderPreListener;
    static CHyprSignalListener              s_renderStageListener;
    static CHyprSignalListener              s_configReloadedListener;
    // One modifier listener per keyboard (Caps Lock is a locked modifier — only
    // m_keyboardEvents.modifiers fires AFTER XKB has updated locked state)
    static std::vector<CHyprSignalListener> s_kbModListeners;

    static void onRenderPre(const PHLMONITOR& pMonitor);
    static void onRenderStage(eRenderStage stage);
    static void onKeyboardModifiers(const IKeyboard::SModifiersEvent& ev);
    static void onConfigReloaded();

    static bool isCapsLockActive();
    static void scheduleFrameAllMonitors();
    static void buildTexture();
    static int  configInt(const std::string& key);
};
