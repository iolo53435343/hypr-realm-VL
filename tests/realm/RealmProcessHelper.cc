#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

#include <hyprutils/memory/Casts.hpp>

using namespace Hyprutils::Memory;

static volatile sig_atomic_t RUNNING = 1;

static void                  stop(int) {
    RUNNING = 0;
}

int main() {
    const auto* runtime = getenv("XDG_RUNTIME_DIR");
    const auto* name    = getenv("HYPRLAND_REALM_NAME");
    if (!runtime || !name)
        return 2;

    const std::string realmName = name;
    if (realmName == "startup-failure")
        return 23;

    signal(SIGTERM, realmName == "ignore-term" ? SIG_IGN : stop);
    signal(SIGINT, stop);

    if (realmName == "no-ready") {
        while (RUNNING)
            pause();
        return 0;
    }

    const auto instanceDirectory = std::filesystem::path(runtime) / "hypr/test-instance";
    std::filesystem::create_directories(instanceDirectory);

    const std::string socketName       = "realm-test.sock";
    const auto        socketPath       = std::filesystem::path(runtime) / socketName;
    const auto        socketPathString = socketPath.string();
    const int         socketFD         = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socketFD < 0)
        return 3;

    sockaddr_un address = {.sun_family = AF_UNIX};
    if (socketPathString.size() >= sizeof(address.sun_path))
        return 4;
    std::copy(socketPathString.begin(), socketPathString.end(), address.sun_path);

    if (bind(socketFD, rc<sockaddr*>(&address), sizeof(address)) < 0 || listen(socketFD, 1) < 0)
        return 5;

    std::ofstream lock(instanceDirectory / "hyprland.lock", std::ios::trunc);
    lock << getpid() << '\n' << socketName << '\n';
    lock.close();
    if (!lock)
        return 6;

    if (realmName == "crash") {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const rlimit noCore = {.rlim_cur = 0, .rlim_max = 0};
        setrlimit(RLIMIT_CORE, &noCore);
        raise(SIGABRT);
        return 7;
    }

    while (RUNNING)
        pause();

    close(socketFD);
    return 0;
}
