#include <realm/RealmInputProtocol.hpp>

#include <array>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <string_view>
#include <sys/socket.h>
#include <sys/syscall.h>
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
        case eRealmInputMessageType::CAPTURE: return "CAPTURE";
        case eRealmInputMessageType::CAPTURE_REGION: return "CAPTURE_REGION";
        case eRealmInputMessageType::CAPTURE_READY: return "CAPTURE_READY";
        case eRealmInputMessageType::CAPTURE_CANCEL: return "CAPTURE_CANCEL";
        case eRealmInputMessageType::POINTER_CLICK: return "POINTER_CLICK";
        case eRealmInputMessageType::KEYBOARD_PRESS: return "KEYBOARD_PRESS";
    }
    return "UNKNOWN";
}

static bool sendCapture(int controlFD, uint32_t sequence) {
#if defined(SYS_memfd_create)
    const auto frameFD = static_cast<int>(syscall(SYS_memfd_create, "realm-test-frame", 0));
#else
    const auto frameFD = -1;
#endif
    if (frameFD < 0 || ftruncate(frameFD, 16) < 0) {
        if (frameFD >= 0)
            close(frameFD);
        return false;
    }

    constexpr std::array<uint8_t, 16> PIXELS = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
    };
    if (pwrite(frameFD, PIXELS.data(), PIXELS.size(), 0) != static_cast<ssize_t>(PIXELS.size())) {
        close(frameFD);
        return false;
    }

    auto packet = encodeRealmInputMessage(SRealmInputMessage{
        .type     = eRealmInputMessageType::CAPTURE_READY,
        .sequence = sequence,
        .width    = 2,
        .height   = 2,
        .format   = REALM_CAPTURE_FORMAT_XRGB8888,
        .stride   = 8,
        .byteSize = 16,
    });
    if (!packet) {
        close(frameFD);
        return false;
    }

    iovec                                     iov{.iov_base = packet->data(), .iov_len = packet->size()};
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
    const auto sent = sendmsg(controlFD, &header, MSG_NOSIGNAL);
    close(frameFD);
    return sent == static_cast<ssize_t>(packet->size());
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
        if ((message->type == eRealmInputMessageType::CAPTURE || message->type == eRealmInputMessageType::CAPTURE_REGION) && !sendCapture(controlFD, message->sequence))
            return 6;
    }

    close(waylandFD);
    close(controlFD);
    return 0;
}
