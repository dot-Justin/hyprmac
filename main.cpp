#include "globals.hpp"
#include "src/features/caps_lock/CapsLockIndicator.hpp"
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/Compositor.hpp>
#include <stdexcept>

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    if (__hyprland_api_get_hash() != __hyprland_api_get_client_hash()) {
        HyprlandAPI::addNotification(PHANDLE,
            "[hyprmac] Version mismatch! Please recompile against the running Hyprland version.",
            CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hyprmac] Version mismatch");
    }

    // Register config values before any getConfigValue calls
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprmac:caps_lock_color",
                                Hyprlang::INT{(int64_t)0x3B82F6FF});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprmac:caps_lock_size",
                                Hyprlang::INT{40});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprmac:caps_lock_offset_y",
                                Hyprlang::INT{8});

    CapsLockIndicator::init();

    HyprlandAPI::addNotification(PHANDLE, "[hyprmac] Loaded v0.1.0",
                                 CHyprColor{0.2, 0.8, 0.2, 1.0}, 3000);

    return {"hyprmac", "macOS-like interactions for Hyprland", "Justin", "0.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    CapsLockIndicator::destroy();
}
