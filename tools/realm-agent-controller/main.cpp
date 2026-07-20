#include "WaylandInput.hpp"

#include "../../src/realm/RealmInputProtocol.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstring>
#include <deque>
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

static ssize_t sendStatus(int controlFD, const SRealmInputMessage& message, int frameFD = -1) {
    auto packet = encodeRealmInputMessage(message);
    if (!packet)
        return -1;

    if (frameFD < 0)
        return send(controlFD, packet->data(), packet->size(), MSG_DONTWAIT | MSG_NOSIGNAL);

    iovec iov{
        .iov_base = packet->data(),
        .iov_len  = packet->size(),
    };
    std::array<char, CMSG_SPACE(sizeof(int))> ancillary{};
    msghdr                                    header{
                                           .msg_iov        = &iov,
                                           .msg_iovlen     = 1,
                                           .msg_control    = ancillary.data(),
                                           .msg_controllen = ancillary.size(),
    };
    auto* control       = CMSG_FIRSTHDR(&header);
    control->cmsg_level = SOL_SOCKET;
    control->cmsg_type  = SCM_RIGHTS;
    control->cmsg_len   = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(control), &frameFD, sizeof(frameFD));
    return sendmsg(controlFD, &header, MSG_DONTWAIT | MSG_NOSIGNAL);
}

static bool sendError(int controlFD, uint32_t sequence, std::string message) {
    if (message.size() > REALM_INPUT_MAX_TEXT_SIZE)
        message.resize(REALM_INPUT_MAX_TEXT_SIZE);
    const auto status = SRealmInputMessage{
        .type     = eRealmInputMessageType::ERROR,
        .sequence = sequence,
        .text     = std::move(message),
    };
    auto packet = encodeRealmInputMessage(status);
    if (!packet)
        return false;
    return sendStatus(controlFD, status) == static_cast<ssize_t>(packet->size());
}

static void queueResults(CWaylandInput& input, std::deque<SWaylandResult>& outgoing) {
    for (auto& result : input.takeResults()) {
        if (!result.error.empty())
            result.message = SRealmInputMessage{
                .type     = eRealmInputMessageType::ERROR,
                .sequence = result.sequence,
                .text     = std::move(result.error),
            };
        outgoing.emplace_back(std::move(result));
    }
}

static bool flushResults(int controlFD, std::deque<SWaylandResult>& outgoing) {
    while (!outgoing.empty()) {
        auto packet = encodeRealmInputMessage(outgoing.front().message);
        if (!packet)
            return false;
        const auto sent = sendStatus(controlFD, outgoing.front().message, outgoing.front().frameFD);
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return true;
        if (sent < 0 || static_cast<size_t>(sent) != packet->size())
            return false;
        if (const auto frameFD = outgoing.front().releaseFrameFD(); frameFD >= 0)
            close(frameFD);
        outgoing.pop_front();
    }
    return true;
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

    const auto ready       = SRealmInputMessage{.type = eRealmInputMessageType::READY};
    const auto readyPacket = encodeRealmInputMessage(ready);
    if (!readyPacket || sendStatus(*controlFD, ready) != static_cast<ssize_t>(readyPacket->size()))
        return 1;

    std::deque<SWaylandResult> outgoing;
    while (!EXIT_REQUESTED) {
        if (!flushResults(*controlFD, outgoing))
            break;
        std::array<pollfd, 2> descriptors = {
            pollfd{.fd = *controlFD, .events = static_cast<short>(POLLIN | (outgoing.empty() ? 0 : POLLOUT))},
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
            queueResults(input, outgoing);
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
                SWaylandResult error;
                error.message = SRealmInputMessage{.type = eRealmInputMessageType::ERROR, .text = message.error()};
                outgoing.emplace_back(std::move(error));
                continue;
            }
            if (const auto handled = input.handle(*message); !handled) {
                SWaylandResult error;
                error.message = SRealmInputMessage{.type = eRealmInputMessageType::ERROR, .sequence = message->sequence, .text = handled.error()};
                outgoing.emplace_back(std::move(error));
            }
            queueResults(input, outgoing);
        }
        if (descriptors[0].revents & POLLHUP)
            break;
    }

    input.releaseAll();
    input.flush();
    return 0;
}
