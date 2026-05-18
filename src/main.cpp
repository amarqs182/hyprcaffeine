#define WLR_USE_UNSTABLE

#include <any>
#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <csignal>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <cstdlib>
#include <systemd/sd-bus.h>

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/protocols/IdleNotify.hpp>

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

static HANDLE PHANDLE = nullptr;

/* ------------------------------------------------------------------ */
/* Caffeine state                                                     */
/* ------------------------------------------------------------------ */
static std::atomic<bool> g_bCaffeineEnabled{false};
static int g_iInhibitFd = -1;
static std::atomic<bool> g_bSleepInhibitActive{false};

/* Config values */
static bool g_cfgInhibitSleep = true;
static bool g_cfgInhibitScreensaver = true;
static int g_cfgAutoOffTimeout = 0;

/* Auto-off timer thread */
static std::thread g_autoOffThread;
static std::atomic<bool> g_bStopAutoOff{false};

/* Registered hyprctl command handle */
static SP<SHyprCtlCommand> g_pCaffeineCmd = nullptr;

/* Forward declarations */
static void setCaffeineEnabled(bool enabled);

/* ------------------------------------------------------------------ */
/* D-Bus sleep inhibition via systemd-logind                          */
/* Uses sd_bus_open_system() for robustness. The inhibit FD is dup'd  */
/* and the bus is closed immediately — the FD stays valid independently*/
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
/* Idle inhibition (ext-idle-notify protocol)                         */
/* ------------------------------------------------------------------ */
static void setIdleInhibit(bool inhibited) {
    if (PROTO::idle)
        PROTO::idle->setInhibit(inhibited);
}

/* ------------------------------------------------------------------ */
/* Signal waybar to refresh (async-signal-safe, no fork)              */
/* ------------------------------------------------------------------ */
static void signalWaybar() {
    // Find waybar PIDs via /proc — no fork, no system()
    // SIGRTMIN+11 is the signal waybar watches
    DIR* proc = opendir("/proc");
    if (!proc) return;

    struct dirent* entry;
    while ((entry = readdir(proc)) != nullptr) {
        if (entry->d_type != DT_DIR) continue;
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;

        // d_name max is 256, PIDs are <= 7 digits — use d_name size for safety
        char comm_path[sizeof(entry->d_name) + 12];
        snprintf(comm_path, sizeof(comm_path), "/proc/%s/comm", entry->d_name);

        int fd = open(comm_path, O_RDONLY);
        if (fd < 0) continue;

        char comm[32] = {};
        ssize_t n = read(fd, comm, sizeof(comm) - 1);
        close(fd);
        if (n <= 0) continue;

        // Strip trailing newline
        if (comm[n - 1] == '\n') comm[n - 1] = '\0';

        if (strcmp(comm, "waybar") == 0) {
            pid_t pid = (pid_t)atoi(entry->d_name);
            if (pid > 0)
                kill(pid, SIGRTMIN + 11);
        }
    }
    closedir(proc);
}

/* ------------------------------------------------------------------ */
/* Auto-off timer (thread-based, no signal handler)                   */
/* ------------------------------------------------------------------ */
static void startAutoOffTimer() {
    // Cancel any existing timer
    g_bStopAutoOff = true;
    if (g_autoOffThread.joinable())
        g_autoOffThread.join();
    g_bStopAutoOff = false;

    if (g_cfgAutoOffTimeout <= 0) return;

    g_autoOffThread = std::thread([]() {
        int remaining = g_cfgAutoOffTimeout;
        while (remaining > 0 && !g_bStopAutoOff) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            remaining--;
        }
        if (!g_bStopAutoOff && g_bCaffeineEnabled) {
            setCaffeineEnabled(false);
        }
    });
    g_autoOffThread.detach();
}

static void stopAutoOffTimer() {
    g_bStopAutoOff = true;
    if (g_autoOffThread.joinable())
        g_autoOffThread.join();
}

/* ------------------------------------------------------------------ */
/* Toggle                                                             */
/* ------------------------------------------------------------------ */
static void setCaffeineEnabled(bool enabled) {
    if (enabled == g_bCaffeineEnabled.load())
        return;

    g_bCaffeineEnabled = enabled;

    if (enabled) {
        // 1. Inhibit idle notifications if screensaver inhibit is on
        if (g_cfgInhibitScreensaver)
            setIdleInhibit(true);

        // 2. Inhibit system sleep (logind D-Bus) — non-fatal
        bool sleepOk = false;
        if (g_cfgInhibitSleep)
            sleepOk = acquireSleepInhibit();

        // 3. Auto-off timer (thread-based)
        startAutoOffTimer();

        // Notification
        std::string msg = "☕ Caffeine ON";
        if (!sleepOk && g_cfgInhibitSleep)
            msg += " (sleep inhibit unavailable)";
        HyprlandAPI::addNotification(PHANDLE, msg,
            CHyprColor{0.2, 0.9, 0.4, 1.0}, 3000);
    } else {
        stopAutoOffTimer();
        releaseSleepInhibit();
        setIdleInhibit(false);

        HyprlandAPI::addNotification(PHANDLE, "☕ Caffeine OFF",
            CHyprColor{0.9, 0.6, 0.2, 1.0}, 3000);
    }

    signalWaybar();
}

/* ------------------------------------------------------------------ */
/* Dispatcher handler — toggles via keybind                           */
/* ------------------------------------------------------------------ */
static SDispatchResult onCaffeineDispatch(std::string args) {
    bool enable = !g_bCaffeineEnabled.load();

    if (args == "on")
        enable = true;
    else if (args == "off")
        enable = false;

    setCaffeineEnabled(enable);
    return SDispatchResult{.passEvent = false, .success = true, .error = ""};
}

/* ------------------------------------------------------------------ */
/* hyprctl query — "hyprctl caffeine" returns JSON                    */
/* ------------------------------------------------------------------ */
static std::string onCaffeineQuery(eHyprCtlOutputFormat format, std::string args) {
    (void)format;
    (void)args;

    char buf[256];
    snprintf(buf, sizeof(buf),
        "{ \"enabled\": %s, \"sleep_inhibited\": %s, \"cfg_inhibit_sleep\": %s, "
        "\"cfg_inhibit_screensaver\": %s, \"cfg_auto_off_timeout\": %d }",
        g_bCaffeineEnabled.load() ? "true" : "false",
        g_bSleepInhibitActive.load() ? "true" : "false",
        g_cfgInhibitSleep ? "true" : "false",
        g_cfgInhibitScreensaver ? "true" : "false",
        g_cfgAutoOffTimeout);
    return std::string(buf);
}

/* ------------------------------------------------------------------ */
/* Plugin init / exit                                                 */
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

    // Register hyprctl command
    g_pCaffeineCmd = HyprlandAPI::registerHyprCtlCommand(PHANDLE, SHyprCtlCommand{
        .name = "caffeine",
        .exact = false,
        .fn = &onCaffeineQuery,
    });

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
        if (g_cfgAutoOffTimeout < 0)
            g_cfgAutoOffTimeout = 0;
    }

    // Enable AFTER config is fully loaded to avoid race with auto-off timer
    if (PENABLED) {
        const auto val = std::any_cast<Hyprlang::INT>(PENABLED->getValue());
        if (val)
            setCaffeineEnabled(true);
    }

    return {
        "HyprCaffeine",
        "Caffeine mode — inhibits idle, screensaver, and system sleep.",
        "Hermes Agent",
        "2.2"
    };
}

APICALL EXPORT void PLUGIN_EXIT() {
    stopAutoOffTimer();

    if (g_bCaffeineEnabled.load()) {
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
