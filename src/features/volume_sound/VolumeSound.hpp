#pragma once

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprutils/signal/Signal.hpp>

using namespace Hyprutils::Signal;
using namespace Hyprutils::Memory;

class VolumeSound {
  public:
    static void init();
    static void destroy();

  private:
    static CHyprSignalListener s_keyListener;
    static void onKey(IKeyboard::SKeyEvent ev, Event::SCallbackInfo& info);
    static int  configInt(const std::string& key);
};
