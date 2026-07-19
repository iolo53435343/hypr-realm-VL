#include "WaylandInput.hpp"

#include "../../src/realm/RealmInputProtocol.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <format>
#include <iostream>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

using namespace Realm;

static volatile sig_atomic_t EXIT_REQUESTED = 0;

static void                  terminationSignal(int) {
    EXIT_REQUESTED = 1;
}

static std::expected<int, std::string> parseFD(std::string_view value) {
    int        fd     = -1;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), fd);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || fd < 3)
        return std::unexpected(std::format("invalid file descriptor '{}'", value));
    if (fcntl(fd, F_GETFD) < 0)
        return std::unexpected(std::format("file descriptor {} is unavailable", fd));
    return fd;
}

static bool sendStatus(int controlFD, const SRealmInputMessage& message) {
    auto packet = encodeRealmInputMessage(message);
    if (!packet)
        return false;

    const auto sent = send(controlFD, packet->data(), packet->size(), MSG_DONTWAIT | MSG_NOSIGNAL);
    return sent >= 0 && static_cast<size_t>(sent) == packet->size();
}

static bool sendError(int controlFD, uint32_t sequence, std::string message) {
    if (message.size() > REALM_INPUT_MAX_TEXT_SIZE)
        message.resize(REALM_INPUT_MAX_TEXT_SIZE);
    return sendStatus(controlFD,
                      SRealmInputMessage{
                          .type     = eRealmInputMessageType::ERROR,
                          .sequence = sequence,
                          .text     = std::move(message),
                      });
}

int main(int argc, char** argv) {
    if (argc != 5 || std::string_view{argv[1]} != "--wayland-fd" || std::string_view{argv[3]} != "--control-fd") {
        std::cerr << "usage: hyprland-realm-agent-controller --wayland-fd FD --control-fd FD\n";
        return 2;
    }

    auto waylandFD = parseFD(argv[2]);
    auto controlFD = parseFD(argv[4]);
    if (!waylandFD || !controlFD || *waylandFD == *controlFD) {
        std::cerr << (!waylandFD ? waylandFD.error() : !controlFD ? controlFD.error() : "Wayland and control file descriptors must be different") << '\n';
        return 2;
    }

    struct sigaction terminationAction = {};
    terminationAction.sa_handler       = terminationSignal;
    sigemptyset(&terminationAction.sa_mask);
    sigaction(SIGTERM, &terminationAction, nullptr);
    sigaction(SIGINT, &terminationAction, nullptr);
    signal(SIGPIPE, SIG_IGN);

    CWaylandInput input{*waylandFD};
    if (const auto initialized = input.initialize(); !initialized) {
        sendError(*controlFD, 0, initialized.error());
        std::cerr << initialized.error() << '\n';
        return 1;
    }

    if (!sendStatus(*controlFD, SRealmInputMessage{.type = eRealmInputMessageType::READY}))
        return 1;

    while (!EXIT_REQUESTED) {
        std::array<pollfd, 2> descriptors = {
            pollfd{.fd = *controlFD, .events = POLLIN},
            pollfd{.fd = input.displayFD(), .events = POLLIN},
        };
        int result = 0;
        do {
            result = poll(descriptors.data(), descriptors.size(), -1);
        } while (result < 0 && errno == EINTR && !EXIT_REQUESTED);
        if (EXIT_REQUESTED)
            break;
        if (result < 0) {
            std::cerr << "controller poll failed: " << std::strerror(errno) << '\n';
            break;
        }

        if (descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL))
            break;
        if (descriptors[1].revents & POLLIN) {
            if (const auto dispatched = input.dispatch(); !dispatched) {
                std::cerr << dispatched.error() << '\n';
                break;
            }
        }

        if (descriptors[0].revents & (POLLERR | POLLNVAL))
            break;
        if (descriptors[0].revents & POLLIN) {
            std::array<uint8_t, REALM_INPUT_MAX_TEXT_SIZE + 16> packet{};
            const auto                                          received = recv(*controlFD, packet.data(), packet.size(), 0);
            if (received <= 0)
                break;

            auto message = decodeRealmInputMessage(packet.data(), static_cast<size_t>(received));
            if (!message) {
                sendError(*controlFD, 0, message.error());
                continue;
            }
            if (const auto handled = input.handle(*message); !handled)
                sendError(*controlFD, message->sequence, handled.error());
        }
        if (descriptors[0].revents & POLLHUP)
            break;
    }

    input.releaseAll();
    input.flush();
    return 0;
}
