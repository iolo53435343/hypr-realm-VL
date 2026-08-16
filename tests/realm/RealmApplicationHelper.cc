#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <unistd.h>

static volatile sig_atomic_t RUNNING = 1;

static void                  stop(int) {
    RUNNING = 0;
}

static std::string_view environment(const char* name) {
    const auto* value = getenv(name);
    return value ? value : "<unset>";
}

int main() {
    sigset_t terminationSignals;
    sigemptyset(&terminationSignals);
    sigaddset(&terminationSignals, SIGTERM);
    sigaddset(&terminationSignals, SIGINT);
    if (sigprocmask(SIG_BLOCK, &terminationSignals, nullptr) < 0)
        return 1;

    signal(SIGTERM, stop);
    signal(SIGINT, stop);

    const auto runtimeDirectory = environment("XDG_RUNTIME_DIR");
    if (runtimeDirectory == "<unset>")
        return 2;

    std::ofstream state(std::filesystem::path(runtimeDirectory) / "realm-application-helper.state", std::ios::trunc);
    state << "pid=" << getpid() << '\n';
    state << "pgid=" << getpgrp() << '\n';
    state << "HOME=" << environment("HOME") << '\n';
    state << "XDG_RUNTIME_DIR=" << environment("XDG_RUNTIME_DIR") << '\n';
    state << "XDG_CACHE_HOME=" << environment("XDG_CACHE_HOME") << '\n';
    state << "XDG_CONFIG_HOME=" << environment("XDG_CONFIG_HOME") << '\n';
    state << "XDG_DATA_HOME=" << environment("XDG_DATA_HOME") << '\n';
    state << "XDG_STATE_HOME=" << environment("XDG_STATE_HOME") << '\n';
    state << "TMPDIR=" << environment("TMPDIR") << '\n';
    state << "PATH=" << environment("PATH") << '\n';
    state << "NIXOS_OZONE_WL=" << environment("NIXOS_OZONE_WL") << '\n';
    state << "MOZ_ENABLE_WAYLAND=" << environment("MOZ_ENABLE_WAYLAND") << '\n';
    state << "ELECTRON_OZONE_PLATFORM_HINT=" << environment("ELECTRON_OZONE_PLATFORM_HINT") << '\n';
    state << "WAYLAND_DISPLAY=" << environment("WAYLAND_DISPLAY") << '\n';
    state << "HYPRLAND_INSTANCE_SIGNATURE=" << environment("HYPRLAND_INSTANCE_SIGNATURE") << '\n';
    state << "HYPRLAND_REALM_ID=" << environment("HYPRLAND_REALM_ID") << '\n';
    state << "HYPRLAND_REALM_NAME=" << environment("HYPRLAND_REALM_NAME") << '\n';
    state << "DISPLAY=" << environment("DISPLAY") << '\n';
    state << "XAUTHORITY=" << environment("XAUTHORITY") << '\n';
    state << "SWAYSOCK=" << environment("SWAYSOCK") << '\n';
    state << "WAYLAND_SOCKET=" << environment("WAYLAND_SOCKET") << '\n';
    state.close();
    if (!state)
        return 3;

    sigset_t waitMask;
    sigemptyset(&waitMask);
    while (RUNNING)
        sigsuspend(&waitMask);

    return 0;
}
