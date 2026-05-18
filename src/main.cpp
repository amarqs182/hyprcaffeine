#define WLR_USE_UNSTABLE

#include <any>
#include <cstring>
#include <string>
#include <unistd.h>
#include <signal.h>
#include <systemd/sd-bus.h>

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/protocols/IdleNotify.hpp>

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

static HANDLE PHANDLE = nullptr;

/* ------------------------------------------------------------------ */
/* Caffeine state */
/* ------------------------------------------------------------------ */
static bool g_bCaffeineEnabled = false;
static int g_iInhibitFd = -1;
static bool g_bSleepInhibitActive = false;

/* Config values */
static bool g_cfgInhibitSleep = true;
static bool g_cfgInhibitScreensaver = true;
static int g_cfgAutoOffTimeout = 0;

/* Registered hyprctl command handle — must be kept for proper unregister */
static SP<SHyprCtlCommand> g_pCaffeineCmd = nullptr;

/* Forward declarations */
static void setCaffeineEnabled(bool enabled);

/* ------------------------------------------------------------------ */
/* D-Bus sleep inhibition via systemd-logind */
/* Uses sd_bus_open_system() for robustness. The inhibit FD is dup'd */
/* and the bus is closed immediately — the FD stays valid independently.*/
/* ------------------------------------------------------------------ */
static bool acquireSleepInhibit() {
    if (g_iInhibitFd >= 0) {
        close(g_iInhibitFd);
        g_iInhibitFd = -1;
    }

    sd_bus* bus = nullptr;
    int ret = sd_bus_open_system(&bus);
    if (ret < 0 || !bus) {
        g_bSleepInhibitActive = false;
        return false;
    }

    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;

    ret = sd_bus_call_method(bus,
        "org.freedesktop.login1",
        "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager",
        "Inhibit",
        &error,
        &reply,
        "ssss",
        "sleep",
        "HyprCaffeine",
        "Caffeine mode active — preventing suspend",
        "block");

    if (ret < 0) {
        sd_bus_error_free(&error);
        sd_bus_unref(bus);
        g_bSleepInhibitActive = false;
        return false;
    }

    int rawFd = -1;
    ret = sd_bus_message_read(reply, "h", &rawFd);
    if (ret < 0 || rawFd < 0) {
        sd_bus_message_unref(reply);
        sd_bus_error_free(&error);
        sd_bus_unref(bus);
        g_bSleepInhibitActive = false;
        return false;
    }

    g_iInhibitFd = dup(rawFd);
    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    sd_bus_unref(bus);

    if (g_iInhibitFd < 0) {
        g_bSleepInhibitActive = false;
        return false;
    }

    g_bSleepInhibitActive = true;
    return true;
}

static void releaseSleepInhibit() {
    if (g_iInhibitFd >= 0) {
        close(g_iInhibitFd);
        g_iInhibitFd = -1;
    }
    g_bSleepInhibitActive = false;
}

/* ------------------------------------------------------------------ */
/* Idle inhibition (ext-idle-notify protocol) */
/* ------------------------------------------------------------------ */
static void setIdleInhibit(bool inhibited) {
    if (PROTO::idle)
        PROTO::idle->setInhibit(inhibited);
}

/* ------------------------------------------------------------------ */
/* Auto-off timer (SIGALRM) */
/* ------------------------------------------------------------------ */
static void onAutoOffAlarm(int sig) {
    (void)sig;
    if (g_bCaffeineEnabled) {
        setCaffeineEnabled(false);
    }
}

/* ------------------------------------------------------------------ */
/* Toggle */
/* ------------------------------------------------------------------ */
static void setCaffeineEnabled(bool enabled) {
    if (enabled == g_bCaffeineEnabled)
        return;

    g_bCaffeineEnabled = enabled;

    if (enabled) {
        // 1. Inhibit idle notifications (Wayland protocol)
        setIdleInhibit(true);

        // 2. Inhibit system sleep (logind D-Bus) — non-fatal
        bool sleepOk = false;
        if (g_cfgInhibitSleep) {
            sleepOk = acquireSleepInhibit();
        }

        // Note: screensaver inhibition is handled by setIdleInhibit(true) above
        // — hypridle won't receive idle notifications, preventing lock/dpms/screensaver

        // 3. Auto-off timer
        if (g_cfgAutoOffTimeout > 0) {
            alarm(g_cfgAutoOffTimeout);
        }

        // Notification
        std::string msg = "☕ Caffeine ON";
        if (!sleepOk && g_cfgInhibitSleep)
            msg += " (sleep inhibit unavailable)";
        HyprlandAPI::addNotification(PHANDLE, msg,
            CHyprColor{0.2, 0.9, 0.4, 1.0}, 3000);
    } else {
        alarm(0);

        releaseSleepInhibit();
        setIdleInhibit(false);

        HyprlandAPI::addNotification(PHANDLE, "☕ Caffeine OFF",
            CHyprColor{0.9, 0.6, 0.2, 1.0}, 3000);
    }

    // Signal waybar to refresh
    system("pkill -RTMIN+11 waybar 2>/dev/null");
}

/* ------------------------------------------------------------------ */
/* Dispatcher handler — toggles via keybind */
/* ------------------------------------------------------------------ */
static SDispatchResult onCaffeineDispatch(std::string args) {
    bool enable = !g_bCaffeineEnabled;

    if (args == "on")
        enable = true;
    else if (args == "off")
        enable = false;

    setCaffeineEnabled(enable);
    return SDispatchResult{.passEvent = false, .success = true, .error = ""};
}

/* ------------------------------------------------------------------ */
/* hyprctl query — "hyprctl caffeine" returns JSON */
/* ------------------------------------------------------------------ */
static std::string onCaffeineQuery(eHyprCtlOutputFormat format, std::string args) {
    (void)format;
    (void)args;
    return "{ \"enabled\": " + std::string(g_bCaffeineEnabled ? "true" : "false")
        + ", \"sleep_inhibited\": " + std::string(g_bSleepInhibitActive ? "true" : "false")
        + ", \"cfg_inhibit_sleep\": " + std::string(g_cfgInhibitSleep ? "true" : "false")
        + ", \"cfg_inhibit_screensaver\": " + std::string(g_cfgInhibitScreensaver ? "true" : "false")
        + ", \"cfg_auto_off_timeout\": " + std::to_string(g_cfgAutoOffTimeout)
        + " }";
}

/* ------------------------------------------------------------------ */
/* Plugin init / exit */
/* ------------------------------------------------------------------ */

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string COMPOSITOR_HASH = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (COMPOSITOR_HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(PHANDLE, "[HyprCaffeine] Version mismatch!",
            CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[HyprCaffeine] Version mismatch");
    }

    // Config values
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprcaffeine:enabled",
        Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprcaffeine:inhibit_sleep",
        Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprcaffeine:inhibit_screensaver",
        Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprcaffeine:auto_off_timeout",
        Hyprlang::INT{0});

    // Register dispatcher
    HyprlandAPI::addDispatcherV2(PHANDLE, "caffeine", &onCaffeineDispatch);

    // Register hyprctl command — MUST store the returned SP for proper unregister
    g_pCaffeineCmd = HyprlandAPI::registerHyprCtlCommand(PHANDLE, SHyprCtlCommand{
        .name = "caffeine",
        .exact = false,
        .fn = &onCaffeineQuery,
    });

    // Set up auto-off signal handler
    signal(SIGALRM, onAutoOffAlarm);

    // Read config
    static auto* const PENABLED = HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprcaffeine:enabled");
    static auto* const PINHIBIT_SLEEP = HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprcaffeine:inhibit_sleep");
    static auto* const PINHIBIT_SAVER = HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprcaffeine:inhibit_screensaver");
    static auto* const PAUTOOFF = HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprcaffeine:auto_off_timeout");

    if (PINHIBIT_SLEEP) {
        const auto val = std::any_cast<Hyprlang::INT>(PINHIBIT_SLEEP->getValue());
        g_cfgInhibitSleep = (val != 0);
    }
    if (PINHIBIT_SAVER) {
        const auto val = std::any_cast<Hyprlang::INT>(PINHIBIT_SAVER->getValue());
        g_cfgInhibitScreensaver = (val != 0);
    }
    if (PAUTOOFF) {
        const auto val = std::any_cast<Hyprlang::INT>(PAUTOOFF->getValue());
        g_cfgAutoOffTimeout = (int)val;
    }

    if (PENABLED) {
        const auto val = std::any_cast<Hyprlang::INT>(PENABLED->getValue());
        if (val)
            setCaffeineEnabled(true);
    }

    return {
        "HyprCaffeine",
        "Caffeine mode — inhibits idle, screensaver, and system sleep.",
        "Hermes Agent",
        "2.1"
    };
}

APICALL EXPORT void PLUGIN_EXIT() {
    alarm(0);
    if (g_bCaffeineEnabled) {
        setIdleInhibit(false);
        releaseSleepInhibit();
        g_bCaffeineEnabled = false;
    }

    // Properly unregister the hyprctl command to prevent SEGV on plugin unload
    if (g_pCaffeineCmd) {
        HyprlandAPI::unregisterHyprCtlCommand(PHANDLE, g_pCaffeineCmd);
        g_pCaffeineCmd = nullptr;
    }
}
