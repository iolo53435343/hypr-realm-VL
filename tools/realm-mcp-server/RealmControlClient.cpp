#include "RealmControlClient.hpp"

#include "../../src/realm/RealmControlProtocol.hpp"
#include "../../src/realm/RealmInputProtocol.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <glaze/glaze.hpp>
#include <limits>
#include <poll.h>
#include <ranges>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

using namespace Hyprutils::OS;
using namespace Realm::MCP;

static constexpr size_t                              MAX_CONTROL_MESSAGE_SIZE = 64 * 1024;
static constexpr std::chrono::steady_clock::duration CONTROL_TIMEOUT          = std::chrono::seconds(10);

struct SControlError {
    std::string code;
    std::string message;
};

struct SControlResponse {
    std::optional<std::string>   request_id;
    std::optional<bool>          ok;
    std::optional<glz::raw_json> result;
    std::optional<SControlError> error;
};

struct SQueuedCapture {
    std::optional<uint64_t> capture_id;
};

struct SQueuedInput {
    std::optional<uint32_t> sequence;
};

struct SControlCaptureFrame {
    std::optional<std::string> transport;
    std::optional<uint32_t>    fd_count;
    std::optional<uint32_t>    format;
    std::optional<uint32_t>    width;
    std::optional<uint32_t>    height;
    std::optional<uint32_t>    stride;
    std::optional<uint64_t>    byte_size;
    std::optional<bool>        y_inverted;
};

struct SControlEvent {
    std::optional<std::string>          event;
    std::optional<uint64_t>             capture_id;
    std::optional<uint32_t>             sequence;
    std::optional<uint64_t>             realm_id;
    std::optional<SControlCaptureFrame> frame;
    std::optional<std::string>          error;
};

struct SRelaxedJSONOptions : glz::opts {
    bool error_on_unknown_keys        = false;
    bool validate_trailing_whitespace = true;
};

static std::string quoteJSON(std::string_view value) {
    auto encoded = glz::write_json(std::string{value});
    return encoded ? std::move(encoded.value()) : R"("")";
}

static std::expected<void, std::string> waitForDescriptor(int descriptor, short events, std::chrono::steady_clock::time_point deadline) {
    while (true) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::milliseconds::zero())
            return std::unexpected("realm control socket timed out");

        pollfd     polled{.fd = descriptor, .events = events};
        const auto result = poll(&polled, 1, static_cast<int>(std::min<int64_t>(remaining.count(), std::numeric_limits<int>::max())));
        if (result < 0 && errno == EINTR)
            continue;
        if (result < 0)
            return std::unexpected(std::format("failed polling realm control socket: {}", strerror(errno)));
        if (result == 0)
            return std::unexpected("realm control socket timed out");
        if (polled.revents & events)
            return {};
        if (polled.revents & (POLLERR | POLLHUP | POLLNVAL))
            return std::unexpected("realm control socket closed unexpectedly");
    }
}

CRealmControlClient::CRealmControlClient(std::filesystem::path socketPath) : m_socketPath(std::move(socketPath)) {}

std::expected<std::filesystem::path, std::string> CRealmControlClient::discoverSocketPath() {
    const auto* runtime   = getenv("XDG_RUNTIME_DIR");
    const auto* signature = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!runtime || !*runtime)
        return std::unexpected("XDG_RUNTIME_DIR is not set; pass --socket explicitly or inherit it into Codex");
    if (!signature || !*signature)
        return std::unexpected("HYPRLAND_INSTANCE_SIGNATURE is not set; pass --socket explicitly or inherit it into Codex");

    const std::string_view signatureView{signature};
    if (signatureView == "." || signatureView == ".." || signatureView.contains('/') || signatureView.contains('\\') ||
        std::ranges::any_of(signatureView, [](unsigned char character) { return character < 0x21 || character == 0x7F; }))
        return std::unexpected("HYPRLAND_INSTANCE_SIGNATURE contains unsafe path characters");

    return std::filesystem::path{runtime} / "hypr" / signatureView / REALM_CONTROL_SOCKET_NAME;
}

std::expected<void, std::string> CRealmControlClient::connect() {
    if (m_socket.isValid())
        return {};
    if (m_socketPath.empty() || !m_socketPath.is_absolute())
        return std::unexpected("realm control socket path must be absolute");

    const auto path = m_socketPath.string();
    if (path.size() >= sizeof(sockaddr_un::sun_path))
        return std::unexpected("realm control socket path is too long");

    struct stat socketStat = {};
    if (lstat(path.c_str(), &socketStat) < 0)
        return std::unexpected(std::format("cannot inspect realm control socket '{}': {}", path, strerror(errno)));
    if (!S_ISSOCK(socketStat.st_mode))
        return std::unexpected("realm control path is not a Unix socket");
    if (socketStat.st_uid != geteuid())
        return std::unexpected("realm control socket is owned by a different user");
    if ((socketStat.st_mode & 0077) != 0)
        return std::unexpected("realm control socket is accessible by group or other users");

    CFileDescriptor socketDescriptor{socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)};
    if (!socketDescriptor.isValid())
        return std::unexpected(std::format("cannot create realm control socket: {}", strerror(errno)));

    sockaddr_un address{.sun_family = AF_UNIX};
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (::connect(socketDescriptor.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        if (errno != EINPROGRESS && errno != EAGAIN)
            return std::unexpected(std::format("cannot connect to realm control socket: {}", strerror(errno)));
        if (auto ready = waitForDescriptor(socketDescriptor.get(), POLLOUT, std::chrono::steady_clock::now() + CONTROL_TIMEOUT); !ready)
            return ready;
        int       socketError = 0;
        socklen_t errorSize   = sizeof(socketError);
        if (getsockopt(socketDescriptor.get(), SOL_SOCKET, SO_ERROR, &socketError, &errorSize) < 0 || socketError != 0)
            return std::unexpected(std::format("cannot connect to realm control socket: {}", strerror(socketError ? socketError : errno)));
    }

    uid_t peerUID = std::numeric_limits<uid_t>::max();
#if defined(__linux__)
    ucred     credentials = {};
    socklen_t size        = sizeof(credentials);
    if (getsockopt(socketDescriptor.get(), SOL_SOCKET, SO_PEERCRED, &credentials, &size) < 0)
        return std::unexpected(std::format("cannot verify realm control peer: {}", strerror(errno)));
    peerUID = credentials.uid;
#elif defined(__FreeBSD__)
    gid_t peerGID = 0;
    if (getpeereid(socketDescriptor.get(), &peerUID, &peerGID) < 0)
        return std::unexpected(std::format("cannot verify realm control peer: {}", strerror(errno)));
#else
    return std::unexpected("this platform cannot verify realm control peer credentials");
#endif
    if (peerUID != geteuid())
        return std::unexpected("realm control peer is owned by a different user");

    m_socket = std::move(socketDescriptor);
    return {};
}

std::expected<void, std::string> CRealmControlClient::writeAll(std::string_view data) {
    const auto deadline = std::chrono::steady_clock::now() + CONTROL_TIMEOUT;
    while (!data.empty()) {
        const auto sent = send(m_socket.get(), data.data(), data.size(), MSG_NOSIGNAL);
        if (sent > 0) {
            data.remove_prefix(static_cast<size_t>(sent));
            continue;
        }
        if (sent < 0 && errno == EINTR)
            continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (auto ready = waitForDescriptor(m_socket.get(), POLLOUT, deadline); !ready)
                return ready;
            continue;
        }
        return std::unexpected(std::format("failed writing realm control request: {}", sent == 0 ? "socket closed" : strerror(errno)));
    }
    return {};
}

std::expected<void, std::string> CRealmControlClient::readExact(void* data, size_t size, CFileDescriptor& descriptor) {
    auto*      output   = static_cast<uint8_t*>(data);
    const auto deadline = std::chrono::steady_clock::now() + CONTROL_TIMEOUT;
    while (size > 0) {
        if (auto ready = waitForDescriptor(m_socket.get(), POLLIN, deadline); !ready)
            return ready;

        iovec                                         iov{.iov_base = output, .iov_len = size};
        std::array<char, CMSG_SPACE(sizeof(int) * 2)> ancillary{};
        msghdr                                        message{
                                                   .msg_iov        = &iov,
                                                   .msg_iovlen     = 1,
                                                   .msg_control    = ancillary.data(),
                                                   .msg_controllen = ancillary.size(),
        };
        const auto received = recvmsg(m_socket.get(), &message, MSG_CMSG_CLOEXEC);
        if (received < 0 && errno == EINTR)
            continue;
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (received <= 0)
            return std::unexpected(received == 0 ? "realm control socket closed unexpectedly" : std::format("failed reading realm control response: {}", strerror(errno)));
        for (auto* control = CMSG_FIRSTHDR(&message); control; control = CMSG_NXTHDR(&message, control)) {
            if (control->cmsg_level != SOL_SOCKET || control->cmsg_type != SCM_RIGHTS || control->cmsg_len < CMSG_LEN(sizeof(int)))
                return std::unexpected("realm control response contained invalid descriptor metadata");
            const auto count = (control->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (size_t index = 0; index < count; ++index) {
                int receivedDescriptor = -1;
                std::memcpy(&receivedDescriptor, CMSG_DATA(control) + index * sizeof(int), sizeof(receivedDescriptor));
                if (!descriptor.isValid())
                    descriptor = CFileDescriptor{receivedDescriptor};
                else {
                    close(receivedDescriptor);
                    return std::unexpected("realm control response contained multiple file descriptors");
                }
            }
        }
        if (message.msg_flags & MSG_CTRUNC)
            return std::unexpected("realm control response contained truncated descriptor metadata");

        output += received;
        size -= static_cast<size_t>(received);
    }
    return {};
}

std::expected<CRealmControlClient::SControlPacket, std::string> CRealmControlClient::readPacket() {
    std::array<uint8_t, 4> header{};
    CFileDescriptor        descriptor;
    if (auto read = readExact(header.data(), header.size(), descriptor); !read)
        return std::unexpected(read.error());

    const auto size =
        (static_cast<uint32_t>(header[0]) << 24) | (static_cast<uint32_t>(header[1]) << 16) | (static_cast<uint32_t>(header[2]) << 8) | static_cast<uint32_t>(header[3]);
    if (size == 0 || size > MAX_CONTROL_MESSAGE_SIZE)
        return std::unexpected("realm control response has an invalid size");

    std::string payload(size, '\0');
    if (auto read = readExact(payload.data(), payload.size(), descriptor); !read)
        return std::unexpected(read.error());
    return SControlPacket{.json = std::move(payload), .descriptor = std::move(descriptor)};
}

std::expected<std::string, std::string> CRealmControlClient::request(std::string_view method, std::optional<std::string_view> realm, std::string_view extraParameters) {
    if (auto connected = connect(); !connected)
        return std::unexpected(connected.error());

    const auto requestID = std::to_string(m_nextRequestID++);
    auto       payload   = realm ? std::format(R"({{"request_id":"{}","method":{},"params":{{"realm":{}}}}})", requestID, quoteJSON(method), quoteJSON(*realm)) :
                                   std::format(R"({{"request_id":"{}","method":{},"params":{{}}}})", requestID, quoteJSON(method));
    if (!extraParameters.empty())
        payload.insert(payload.size() - 2, std::format(",{}", extraParameters));
    if (payload.size() > MAX_CONTROL_MESSAGE_SIZE)
        return std::unexpected("realm control request is too large");

    std::array<char, 4> header{
        static_cast<char>((payload.size() >> 24) & 0xFF),
        static_cast<char>((payload.size() >> 16) & 0xFF),
        static_cast<char>((payload.size() >> 8) & 0xFF),
        static_cast<char>(payload.size() & 0xFF),
    };
    if (auto written = writeAll(std::string_view{header.data(), header.size()}); !written)
        return std::unexpected(written.error());
    if (auto written = writeAll(payload); !written)
        return std::unexpected(written.error());

    auto packet = readPacket();
    if (!packet)
        return std::unexpected(packet.error());
    if (packet->descriptor.isValid())
        return std::unexpected("realm control response unexpectedly included a file descriptor");

    SControlResponse response;
    if (const auto error = glz::read<SRelaxedJSONOptions{}>(response, packet->json); error)
        return std::unexpected("realm control response is not valid JSON");
    if (!response.request_id || *response.request_id != requestID || !response.ok)
        return std::unexpected("realm control response is missing required fields");
    if (!*response.ok) {
        if (!response.error)
            return std::unexpected("realm control request failed without an error");
        return std::unexpected(std::format("{}: {}", response.error->code, response.error->message));
    }
    if (!response.result)
        return std::unexpected("realm control response is missing its result");
    return response.result->str;
}

std::expected<std::string, std::string> CRealmControlClient::listRealms() {
    return request("realm.list", std::nullopt);
}

std::expected<std::string, std::string> CRealmControlClient::realmInfo(std::string_view realm) {
    return request("realm.info", realm);
}

std::expected<std::string, std::string> CRealmControlClient::input(std::string_view realm, std::string_view method, std::string_view extraParameters,
                                                                   std::chrono::milliseconds settleTime) {
    const auto started = std::chrono::steady_clock::now();
    auto       queued  = request(method, realm, extraParameters);
    if (!queued)
        return std::unexpected(queued.error());

    SQueuedInput queuedInput;
    if (const auto error = glz::read<SRelaxedJSONOptions{}>(queuedInput, *queued); error || !queuedInput.sequence)
        return std::unexpected("realm control input response is missing its sequence");

    for (size_t ignoredEvents = 0; ignoredEvents < 64; ++ignoredEvents) {
        auto packet = readPacket();
        if (!packet)
            return std::unexpected(packet.error());
        if (packet->descriptor.isValid())
            return std::unexpected("realm control input event unexpectedly included a file descriptor");

        SControlEvent event;
        if (const auto error = glz::read<SRelaxedJSONOptions{}>(event, packet->json); error || !event.sequence || *event.sequence != *queuedInput.sequence)
            continue;
        if (event.event == "realm.input.failed")
            return std::unexpected(event.error.value_or("realm input failed"));
        if (event.event != "realm.input.applied")
            return std::unexpected("realm control input event is incomplete");
        const auto delivered = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        if (settleTime > std::chrono::milliseconds::zero())
            std::this_thread::sleep_for(settleTime);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        return std::format(R"({{"action":"applied","sequence":{},"delivery_ms":{},"settle_ms":{},"elapsed_ms":{},"realm_name":{}}})", *queuedInput.sequence, delivered.count(),
                           settleTime.count(), elapsed.count(), quoteJSON(realm));
    }
    return std::unexpected("too many unmatched realm control events while waiting for input completion");
}

std::expected<std::string, std::string> CRealmControlClient::createRealm(std::string_view realm) {
    return request("realm.create", realm);
}

std::expected<std::string, std::string> CRealmControlClient::startRealm(std::string_view realm) {
    return request("realm.start", realm);
}

std::expected<std::string, std::string> CRealmControlClient::pauseRealm(std::string_view realm) {
    return request("realm.pause", realm);
}

std::expected<std::string, std::string> CRealmControlClient::resumeRealm(std::string_view realm) {
    return request("realm.resume", realm);
}

std::expected<std::string, std::string> CRealmControlClient::stopRealm(std::string_view realm) {
    return request("realm.stop", realm);
}

std::expected<std::string, std::string> CRealmControlClient::destroyRealm(std::string_view realm) {
    return request("realm.destroy", realm);
}

std::expected<std::string, std::string> CRealmControlClient::grantCapability(std::string_view realm, std::string_view capability) {
    return request("realm.grant", realm, std::format(R"("capability":{})", quoteJSON(capability)));
}

std::expected<std::string, std::string> CRealmControlClient::openApplication(std::string_view realm, std::string_view application) {
    return request("realm.open", realm, std::format(R"("application":{})", quoteJSON(application)));
}

std::expected<std::string, std::string> CRealmControlClient::placeRealm(std::string_view realm, int64_t workspace) {
    return request("realm.place", realm, std::format(R"("workspace":{})", workspace));
}

std::expected<std::string, std::string> CRealmControlClient::allowObservation(std::string_view realm) {
    return request("realm.observe", realm);
}

std::expected<std::string, std::string> CRealmControlClient::denyObservation(std::string_view realm) {
    return request("realm.unobserve", realm);
}

std::expected<SCaptureFrame, std::string> CRealmControlClient::capture(std::string_view realm, std::optional<SCaptureRegion> region) {
    auto queued = region ? request("realm.capture_region", realm, std::format(R"("x":{},"y":{},"width":{},"height":{})", region->x, region->y, region->width, region->height)) :
                           request("realm.capture", realm);
    if (!queued)
        return std::unexpected(queued.error());

    SQueuedCapture queuedCapture;
    if (const auto error = glz::read<SRelaxedJSONOptions{}>(queuedCapture, *queued); error || !queuedCapture.capture_id)
        return std::unexpected("realm control capture response is missing its capture ID");

    for (size_t ignoredEvents = 0; ignoredEvents < 64; ++ignoredEvents) {
        auto packet = readPacket();
        if (!packet)
            return std::unexpected(packet.error());

        SControlEvent event;
        if (const auto error = glz::read<SRelaxedJSONOptions{}>(event, packet->json); error) {
            if (packet->descriptor.isValid())
                return std::unexpected("realm control capture event is not valid JSON");
            continue;
        }
        if (!event.capture_id || *event.capture_id != *queuedCapture.capture_id) {
            if (packet->descriptor.isValid())
                return std::unexpected("unmatched realm control event included a file descriptor");
            continue;
        }
        if (event.event == "realm.capture.failed")
            return std::unexpected(event.error.value_or("realm capture failed"));
        if (event.event != "realm.capture.ready" || !event.frame)
            return std::unexpected("realm control capture event is incomplete");
        if (!packet->descriptor.isValid())
            return std::unexpected("realm control capture event did not include a file descriptor");

        const auto& frame = *event.frame;
        if (frame.transport != "scm_rights" || frame.fd_count != 1 || !frame.format || !frame.width || !frame.height || !frame.stride || !frame.byte_size || !frame.y_inverted)
            return std::unexpected("realm control capture metadata is incomplete");
        if (*frame.format != REALM_CAPTURE_FORMAT_ARGB8888 && *frame.format != REALM_CAPTURE_FORMAT_XRGB8888)
            return std::unexpected("realm control capture has an unsupported pixel format");
        if (*frame.width == 0 || *frame.height == 0 || *frame.width > REALM_CAPTURE_MAX_DIMENSION || *frame.height > REALM_CAPTURE_MAX_DIMENSION ||
            *frame.stride < *frame.width * 4ULL)
            return std::unexpected(std::format("realm control capture metadata is invalid: width={}, height={}, stride={}, maximum_dimension={}", *frame.width, *frame.height,
                                               *frame.stride, REALM_CAPTURE_MAX_DIMENSION));
        if (*frame.height > std::numeric_limits<uint64_t>::max() / *frame.stride || *frame.byte_size != static_cast<uint64_t>(*frame.stride) * *frame.height ||
            *frame.byte_size > REALM_CAPTURE_MAX_BYTES)
            return std::unexpected("realm control capture size is invalid");

        struct stat descriptorStat = {};
        if (fstat(packet->descriptor.get(), &descriptorStat) < 0 || !S_ISREG(descriptorStat.st_mode) || descriptorStat.st_uid != geteuid() || descriptorStat.st_size < 0 ||
            static_cast<uint64_t>(descriptorStat.st_size) != *frame.byte_size)
            return std::unexpected("realm control capture descriptor is invalid");

        SCaptureFrame captured{
            .format    = *frame.format,
            .width     = *frame.width,
            .height    = *frame.height,
            .stride    = *frame.stride,
            .yInverted = *frame.y_inverted,
            .pixels    = std::vector<uint8_t>(static_cast<size_t>(*frame.byte_size)),
        };
        size_t offset = 0;
        while (offset < captured.pixels.size()) {
            const auto size = pread(packet->descriptor.get(), captured.pixels.data() + offset, captured.pixels.size() - offset, static_cast<off_t>(offset));
            if (size < 0 && errno == EINTR)
                continue;
            if (size <= 0)
                return std::unexpected(size == 0 ? "realm capture ended before its declared size" : std::format("failed reading realm capture: {}", strerror(errno)));
            offset += static_cast<size_t>(size);
        }
        return captured;
    }
    return std::unexpected("too many unmatched realm control events while waiting for a capture");
}

std::expected<std::string, std::string> CRealmControlClient::movePointer(std::string_view realm, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    return input(realm, "pointer.move", std::format(R"("x":{},"y":{},"width":{},"height":{})", x, y, width, height));
}

std::expected<std::string, std::string> CRealmControlClient::pointAndClick(std::string_view realm, uint32_t x, uint32_t y, uint32_t width, uint32_t height, std::string_view button,
                                                                           uint32_t count) {
    return input(realm, "pointer.point_and_click", std::format(R"("x":{},"y":{},"width":{},"height":{},"button":{},"count":{})", x, y, width, height, quoteJSON(button), count));
}

std::expected<std::string, std::string> CRealmControlClient::clickPointer(std::string_view realm, std::string_view button) {
    return input(realm, "pointer.click", std::format(R"("button":{})", quoteJSON(button)));
}

std::expected<std::string, std::string> CRealmControlClient::scrollPointer(std::string_view realm, std::string_view axis, int32_t steps) {
    return input(realm, "pointer.scroll", std::format(R"("axis":{},"steps":{})", quoteJSON(axis), steps));
}

std::expected<std::string, std::string> CRealmControlClient::pressKey(std::string_view realm, uint32_t keycode) {
    return input(realm, "keyboard.press", std::format(R"("keycode":{})", keycode));
}

std::expected<std::string, std::string> CRealmControlClient::pressShortcut(std::string_view realm, const std::vector<uint32_t>& keycodes, std::chrono::milliseconds settleTime) {
    std::string encoded;
    for (const auto keycode : keycodes) {
        if (!encoded.empty())
            encoded += ',';
        encoded += std::to_string(keycode);
    }
    return input(realm, "keyboard.shortcut", std::format(R"("keycodes":[{}])", encoded), settleTime);
}

std::expected<std::string, std::string> CRealmControlClient::typeText(std::string_view realm, std::string_view text) {
    return input(realm, "keyboard.type", std::format(R"("text":{})", quoteJSON(text)));
}
