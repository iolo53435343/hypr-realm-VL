#include <realm/RealmInputProtocol.hpp>

#include <array>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

using namespace Realm;

static volatile sig_atomic_t RUNNING = 1;

static void                  stop(int) {
    RUNNING = 0;
}

static int parseFD(std::string_view value) {
    int        fd     = -1;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), fd);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || fd < 3 || fcntl(fd, F_GETFD) < 0)
        return -1;
    return fd;
}

static const char* messageName(eRealmInputMessageType type) {
    switch (type) {
        case eRealmInputMessageType::READY: return "READY";
        case eRealmInputMessageType::ERROR: return "ERROR";
        case eRealmInputMessageType::RELEASE_ALL: return "RELEASE_ALL";
        case eRealmInputMessageType::POINTER_MOVE: return "POINTER_MOVE";
        case eRealmInputMessageType::POINTER_BUTTON: return "POINTER_BUTTON";
        case eRealmInputMessageType::POINTER_SCROLL: return "POINTER_SCROLL";
        case eRealmInputMessageType::KEYBOARD_KEY: return "KEYBOARD_KEY";
        case eRealmInputMessageType::KEYBOARD_TYPE: return "KEYBOARD_TYPE";
    }
    return "UNKNOWN";
}

int main(int argc, char** argv) {
    if (argc != 5 || std::string_view{argv[1]} != "--wayland-fd" || std::string_view{argv[3]} != "--control-fd")
        return 2;

    const auto waylandFD = parseFD(argv[2]);
    const auto controlFD = parseFD(argv[4]);
    if (waylandFD < 0 || controlFD < 0 || waylandFD == controlFD)
        return 3;

    signal(SIGTERM, stop);
    signal(SIGINT, stop);

    auto ready = encodeRealmInputMessage(SRealmInputMessage{.type = eRealmInputMessageType::READY});
    if (!ready || send(controlFD, ready->data(), ready->size(), MSG_NOSIGNAL) != static_cast<ssize_t>(ready->size()))
        return 4;

    while (RUNNING) {
        pollfd descriptor{.fd = controlFD, .events = POLLIN};
        int    result = 0;
        do {
            result = poll(&descriptor, 1, -1);
        } while (result < 0 && errno == EINTR && RUNNING);
        if (!RUNNING || result <= 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)))
            break;

        std::array<uint8_t, REALM_INPUT_MAX_TEXT_SIZE + 16> packet{};
        const auto                                          received = recv(controlFD, packet.data(), packet.size(), 0);
        if (received <= 0)
            break;

        auto message = decodeRealmInputMessage(packet.data(), static_cast<size_t>(received));
        if (!message)
            return 5;
        std::cout << messageName(message->type) << ' ' << message->sequence << '\n' << std::flush;
    }

    close(waylandFD);
    close(controlFD);
    return 0;
}
