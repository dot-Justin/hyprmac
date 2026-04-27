#include "globals.hpp"
#include "src/features/caps_lock/CapsLockIndicator.hpp"
#include "src/features/volume_sound/VolumeSound.hpp"
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/version.h>
#include <stdexcept>
#include <string>
#include <string_view>

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

// Compute our own client hash from version.h macros rather than calling
// __hyprland_api_get_client_hash(), whose GNU unique static gets poisoned by
// any other plugin (e.g. hyprgrass) compiled against older library versions.
static std::string computeClientHash() {
    auto stripPatch = [](std::string_view v) -> std::string {
        if (!v.contains('.'))
            return std::string{v};
        return std::string{v.substr(0, v.find_last_of('.'))};
    };
    return std::string{GIT_COMMIT_HASH}
        + "_aq_"  + stripPatch(AQUAMARINE_VERSION)
        + "_hu_"  + stripPatch(HYPRUTILS_VERSION)
        + "_hg_"  + stripPatch(HYPRGRAPHICS_VERSION)
        + "_hc_"  + stripPatch(HYPRCURSOR_VERSION)
        + "_hlg_" + stripPatch(HYPRLANG_VERSION);
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    if (std::string_view{__hyprland_api_get_hash()} != computeClientHash()) {
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
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprmac:volume_sound_enabled",
                                Hyprlang::INT{1});

    CapsLockIndicator::init();
    VolumeSound::init();

    HyprlandAPI::addNotification(PHANDLE, "[hyprmac] Loaded v0.1.0",
                                 CHyprColor{0.2, 0.8, 0.2, 1.0}, 3000);

    return {"hyprmac", "macOS-like interactions for Hyprland", "Justin", "0.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    VolumeSound::destroy();
    CapsLockIndicator::destroy();
}
