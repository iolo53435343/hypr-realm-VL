#include "RealmControlServer.hpp"

#include "RealmIPC.hpp"
#include "RealmInputController.hpp"
#include "RealmManager.hpp"
#include "RealmWindowManager.hpp"

#include "../Compositor.hpp"
#include "../helpers/MiscFunctions.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <expected>
#include <format>
#include <glaze/glaze.hpp>
#include <limits>
#include <linux/input-event-codes.h>
#include <map>
#include <optional>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <wayland-server-core.h>

#include <hyprutils/os/FileDescriptor.hpp>

using namespace Hyprutils::OS;
using namespace Realm;

struct SRealmControlParams {
    std::optional<std::string> realm;
    std::optional<uint32_t>    x;
    std::optional<uint32_t>    y;
    std::optional<uint32_t>    width;
    std::optional<uint32_t>    height;
    std::optional<std::string> button;
    std::optional<std::string> axis;
    std::optional<int32_t>     steps;
    std::optional<uint32_t>    keycode;
    std::optional<bool>        pressed;
    std::optional<std::string> text;
};

struct SRealmControlRequest {
    std::optional<std::string>         request_id;
    std::optional<std::string>         method;
    std::optional<SRealmControlParams> params;
};

struct SRealmControlJSONOptions : glz::opts {
    bool validate_trailing_whitespace = true;
};

static std::string controlError(const std::optional<std::string>& requestID, std::string_view code, std::string_view message) {
    const auto encodedRequestID = requestID ? std::format(R"("{}")", escapeJSONStrings(*requestID)) : "null";
    return std::format(R"({{"request_id":{},"ok":false,"error":{{"code":"{}","message":"{}"}}}})", encodedRequestID, escapeJSONStrings(std::string{code}),
                       escapeJSONStrings(std::string{message}));
}

static std::string controlSuccess(std::string_view requestID, std::string_view result) {
    return std::format(R"({{"request_id":"{}","ok":true,"result":{}}})", escapeJSONStrings(std::string{requestID}), result);
}

static bool hasControlCharacters(std::string_view value) {
    return std::ranges::any_of(value, [](unsigned char character) { return character < 0x20 || character == 0x7F; });
}

static std::expected<SRealmControlRequest, std::string> parseControlRequest(std::string_view payload) {
    SRealmControlRequest request;
    if (const auto error = glz::read<SRealmControlJSONOptions{}>(request, payload); error)
        return std::unexpected("request is not valid JSON or contains unknown fields");

    return request;
}

static std::string realmResult(std::string_view action, const SP<CRealm>& realm) {
    return std::format(R"({{"action":"{}","realm":{}}})", action, realmJSON(*realm));
}

static bool hasAutomationParameters(const SRealmControlParams& params) {
    return params.x || params.y || params.width || params.height || params.button || params.axis || params.steps || params.keycode || params.pressed || params.text;
}

static bool onlyRealmAnd(const SRealmControlParams& params, std::initializer_list<std::string_view> fields) {
    const auto contains = [&fields](std::string_view field) { return std::ranges::find(fields, field) != fields.end(); };
    return (!params.x || contains("x")) && (!params.y || contains("y")) && (!params.width || contains("width")) && (!params.height || contains("height")) &&
        (!params.button || contains("button")) && (!params.axis || contains("axis")) && (!params.steps || contains("steps")) && (!params.keycode || contains("keycode")) &&
        (!params.pressed || contains("pressed")) && (!params.text || contains("text"));
}

static std::string inputResult(uint32_t sequence, const SP<CRealm>& realm) {
    return std::format(R"({{"action":"queued","sequence":{},"realm":{}}})", sequence, realmJSON(*realm));
}

static std::string handleInputRequest(const SRealmControlRequest& request, const SP<CRealm>& realm, CRealmInputControllerManager* inputController) {
    if (!inputController)
        return controlError(request.request_id, "controller_unavailable", "realm input controller support is unavailable");

    const auto&        method = *request.method;
    const auto&        params = *request.params;
    SRealmInputMessage message;
    if (method == "pointer.move") {
        if (!params.x || !params.y || !onlyRealmAnd(params, {"x", "y"}))
            return controlError(request.request_id, "invalid_params", "pointer.move requires only params.realm, params.x, and params.y");
        if (*params.x >= REALM_INPUT_OUTPUT_WIDTH || *params.y >= REALM_INPUT_OUTPUT_HEIGHT)
            return controlError(request.request_id, "invalid_params", "pointer coordinates must be inside the 1280x720 realm output");
        message = SRealmInputMessage{
            .type   = eRealmInputMessageType::POINTER_MOVE,
            .x      = *params.x,
            .y      = *params.y,
            .width  = REALM_INPUT_OUTPUT_WIDTH,
            .height = REALM_INPUT_OUTPUT_HEIGHT,
        };
    } else if (method == "pointer.button") {
        if (!params.button || !params.pressed || !onlyRealmAnd(params, {"button", "pressed"}))
            return controlError(request.request_id, "invalid_params", "pointer.button requires only params.realm, params.button, and params.pressed");

        uint32_t code = 0;
        if (*params.button == "left")
            code = BTN_LEFT;
        else if (*params.button == "right")
            code = BTN_RIGHT;
        else if (*params.button == "middle")
            code = BTN_MIDDLE;
        else
            return controlError(request.request_id, "invalid_params", "pointer button must be 'left', 'right', or 'middle'");
        message = SRealmInputMessage{
            .type    = eRealmInputMessageType::POINTER_BUTTON,
            .code    = code,
            .pressed = *params.pressed,
        };
    } else if (method == "pointer.scroll") {
        if (!params.axis || !params.steps || !onlyRealmAnd(params, {"axis", "steps"}))
            return controlError(request.request_id, "invalid_params", "pointer.scroll requires only params.realm, params.axis, and params.steps");
        if (*params.steps == 0 || *params.steps < -20 || *params.steps > 20)
            return controlError(request.request_id, "invalid_params", "pointer scroll steps must be between -20 and 20 and cannot be zero");

        message.type = eRealmInputMessageType::POINTER_SCROLL;
        if (*params.axis == "horizontal")
            message.horizontal = *params.steps;
        else if (*params.axis == "vertical")
            message.vertical = *params.steps;
        else
            return controlError(request.request_id, "invalid_params", "pointer scroll axis must be 'horizontal' or 'vertical'");
    } else if (method == "keyboard.key") {
        if (!params.keycode || !params.pressed || !onlyRealmAnd(params, {"keycode", "pressed"}))
            return controlError(request.request_id, "invalid_params", "keyboard.key requires only params.realm, params.keycode, and params.pressed");
        if (*params.keycode > KEY_MAX)
            return controlError(request.request_id, "invalid_params", std::format("keyboard keycode must not exceed {}", KEY_MAX));
        message = SRealmInputMessage{
            .type    = eRealmInputMessageType::KEYBOARD_KEY,
            .code    = *params.keycode,
            .pressed = *params.pressed,
        };
    } else if (method == "keyboard.type") {
        if (!params.text || !onlyRealmAnd(params, {"text"}))
            return controlError(request.request_id, "invalid_params", "keyboard.type requires only params.realm and params.text");
        if (params.text->empty() || params.text->size() > REALM_INPUT_MAX_TEXT_SIZE || params.text->find('\0') != std::string::npos)
            return controlError(request.request_id, "invalid_params", std::format("keyboard text must contain between 1 and {} bytes without NUL", REALM_INPUT_MAX_TEXT_SIZE));
        message = SRealmInputMessage{
            .type = eRealmInputMessageType::KEYBOARD_TYPE,
            .text = *params.text,
        };
    } else
        return controlError(request.request_id, "method_not_found", std::format("unknown method '{}'", method));

    auto result = inputController->sendInput(realm->id(), std::move(message));
    if (!result)
        return controlError(request.request_id, realmInputErrorName(result.error().code), result.error().message);
    return controlSuccess(*request.request_id, inputResult(*result, realm));
}

static std::string handleCaptureRequest(const SRealmControlRequest& request, const SP<CRealm>& realm, CRealmInputControllerManager* inputController,
                                        std::optional<uint64_t>* queuedCaptureID) {
    if (!inputController)
        return controlError(request.request_id, "controller_unavailable", "realm capture controller support is unavailable");

    const auto&                        method = *request.method;
    const auto&                        params = *request.params;
    std::optional<SRealmCaptureRegion> region;
    if (method == "realm.capture") {
        if (!onlyRealmAnd(params, {}))
            return controlError(request.request_id, "invalid_params", "realm.capture accepts only params.realm");
    } else {
        if (!params.x || !params.y || !params.width || !params.height || !onlyRealmAnd(params, {"x", "y", "width", "height"}))
            return controlError(request.request_id, "invalid_params", "realm.capture_region requires only params.realm, params.x, params.y, params.width, and params.height");
        if (*params.width == 0 || *params.height == 0 || *params.x >= REALM_INPUT_OUTPUT_WIDTH || *params.y >= REALM_INPUT_OUTPUT_HEIGHT ||
            *params.width > REALM_INPUT_OUTPUT_WIDTH - *params.x || *params.height > REALM_INPUT_OUTPUT_HEIGHT - *params.y)
            return controlError(request.request_id, "invalid_params", "capture region must be inside the 1280x720 realm output");
        region = SRealmCaptureRegion{.x = *params.x, .y = *params.y, .width = *params.width, .height = *params.height};
    }

    auto result = inputController->requestCapture(realm->id(), region);
    if (!result)
        return controlError(request.request_id, realmInputErrorName(result.error().code), result.error().message);
    if (queuedCaptureID)
        *queuedCaptureID = *result;
    return controlSuccess(*request.request_id, std::format(R"({{"action":"queued","capture_id":{},"realm":{}}})", *result, realmJSON(*realm)));
}

std::string Realm::realmControlRequest(CRealmManager& manager, CRealmWindowManager& windowManager, std::string_view payload) {
    return realmControlRequest(manager, windowManager, nullptr, payload);
}

static std::string realmControlRequestImpl(CRealmManager& manager, CRealmWindowManager& windowManager, CRealmInputControllerManager* inputController, std::string_view payload,
                                           std::optional<uint64_t>* queuedCaptureID) {
    auto parsed = parseControlRequest(payload);
    if (!parsed)
        return controlError(std::nullopt, "parse_error", parsed.error());

    const auto& request = *parsed;
    if (!request.request_id || request.request_id->empty() || request.request_id->size() > 128 || hasControlCharacters(*request.request_id))
        return controlError(std::nullopt, "invalid_request", "request_id must be a non-empty string of at most 128 characters without control characters");

    if (!request.method || request.method->empty() || request.method->size() > 64 || hasControlCharacters(*request.method))
        return controlError(request.request_id, "invalid_request", "method must be a non-empty string of at most 64 characters without control characters");

    const auto& method = *request.method;
    if (method == "realm.list") {
        if (request.params && (request.params->realm || hasAutomationParameters(*request.params)))
            return controlError(request.request_id, "invalid_params", "realm.list does not accept parameters");
        return controlSuccess(*request.request_id, std::format(R"({{"realms":{}}})", realmListRequest(manager, FORMAT_JSON)));
    }

    constexpr std::array<std::string_view, 18> REALM_METHODS = {
        "realm.info",    "realm.create",    "realm.start",   "realm.pause",          "realm.resume", "realm.stop",     "realm.destroy",  "realm.takeover", "realm.release",
        "realm.observe", "realm.unobserve", "realm.capture", "realm.capture_region", "pointer.move", "pointer.button", "pointer.scroll", "keyboard.key",   "keyboard.type",
    };
    if (std::ranges::find(REALM_METHODS, method) == REALM_METHODS.end())
        return controlError(request.request_id, "method_not_found", std::format("unknown method '{}'", method));

    if (!request.params || !request.params->realm || request.params->realm->empty())
        return controlError(request.request_id, "invalid_params", std::format("{} requires params.realm", method));

    const bool inputMethod   = method == "pointer.move" || method == "pointer.button" || method == "pointer.scroll" || method == "keyboard.key" || method == "keyboard.type";
    const bool captureMethod = method == "realm.capture" || method == "realm.capture_region";
    if (!inputMethod && !captureMethod && hasAutomationParameters(*request.params))
        return controlError(request.request_id, "invalid_params", std::format("{} does not accept automation parameters", method));

    const auto& name = *request.params->realm;
    if (method == "realm.create") {
        auto created = manager.createRealm(name);
        if (!created)
            return controlError(request.request_id, "operation_failed", created.error());
        return controlSuccess(*request.request_id, realmResult("created", *created));
    }

    const auto realm = manager.realmByName(name);
    if (!realm)
        return controlError(request.request_id, "realm_not_found", std::format("realm '{}' does not exist", name));

    if (method == "realm.info")
        return controlSuccess(*request.request_id, std::format(R"({{"realm":{}}})", realmJSON(*realm)));
    if (inputMethod)
        return handleInputRequest(request, realm, inputController);
    if (captureMethod)
        return handleCaptureRequest(request, realm, inputController, queuedCaptureID);

    std::expected<void, std::string> result         = std::unexpected("unsupported realm operation");
    std::string_view                 responseAction = "";
    if (method == "realm.start") {
        result         = manager.startRealm(realm->id());
        responseAction = "starting";
    } else if (method == "realm.pause") {
        result         = manager.pauseRealm(realm->id());
        responseAction = "paused";
    } else if (method == "realm.resume") {
        result         = manager.resumeRealm(realm->id());
        responseAction = "resumed";
    } else if (method == "realm.stop") {
        result         = manager.stopRealm(realm->id());
        responseAction = "stopping";
    } else if (method == "realm.destroy") {
        result         = manager.destroyRealm(realm->id());
        responseAction = "destroyed";
    } else if (method == "realm.takeover") {
        result         = windowManager.takeoverRealm(realm->id());
        responseAction = "taken over";
    } else if (method == "realm.release") {
        result         = windowManager.releaseRealm(realm->id());
        responseAction = "released";
    } else if (method == "realm.observe") {
        result         = manager.allowObservation(realm->id());
        responseAction = "observation allowed";
    } else if (method == "realm.unobserve") {
        result         = manager.denyObservation(realm->id());
        responseAction = "observation denied";
    }

    if (!result)
        return controlError(request.request_id, "operation_failed", result.error());
    return controlSuccess(*request.request_id, realmResult(responseAction, realm));
}

std::string Realm::realmControlRequest(CRealmManager& manager, CRealmWindowManager& windowManager, CRealmInputControllerManager* inputController, std::string_view payload) {
    return realmControlRequestImpl(manager, windowManager, inputController, payload, nullptr);
}

std::string Realm::realmControlFrame(std::string_view payload) {
    if (payload.size() > std::numeric_limits<uint32_t>::max())
        return {};

    const auto  length = sc<uint32_t>(payload.size());
    std::string frame(4, '\0');
    frame[0] = sc<char>((length >> 24) & 0xFF);
    frame[1] = sc<char>((length >> 16) & 0xFF);
    frame[2] = sc<char>((length >> 8) & 0xFF);
    frame[3] = sc<char>(length & 0xFF);
    frame.append(payload);
    return frame;
}

bool Realm::realmControlPeerAuthorized(uid_t peerUID, uid_t expectedUID) {
    return peerUID == expectedUID;
}

struct CRealmControlServer::SImpl {
    struct SOutputFrame {
        std::string     frame;
        size_t          offset = 0;
        CFileDescriptor descriptor;
    };

    struct SClient {
        uint64_t                 id = 0;
        CFileDescriptor          fd;
        wl_event_source*         eventSource = nullptr;
        std::string              input;
        size_t                   inputOffset = 0;
        std::deque<SOutputFrame> output;
        size_t                   queuedOutput    = 0;
        bool                     closeAfterWrite = false;
    };

    struct SPendingCapture {
        uint64_t clientID = 0;
    };

    SImpl(CRealmManager& manager_, CRealmWindowManager& windowManager_, CRealmInputControllerManager* inputController_, SRealmControlServerOptions options_) :
        manager(manager_), windowManager(windowManager_), inputController(inputController_), options(std::move(options_)) {
        if (!options.expectedPeerUID)
            options.expectedPeerUID = geteuid();
        if (inputController)
            inputController->setCaptureResultCallback([this](SRealmCaptureResult result) { handleCaptureResult(std::move(result)); });
        start();
    }

    ~SImpl() {
        if (inputController)
            inputController->setCaptureResultCallback({});
        shutdown();
    }

    static int onListenEvent(int fd, uint32_t mask, void* data) {
        sc<SImpl*>(data)->handleListenEvent(fd, mask);
        return 0;
    }

    static int onClientEvent(int fd, uint32_t mask, void* data) {
        sc<SImpl*>(data)->handleClientEvent(fd, mask);
        return 0;
    }

    void start() {
        if (options.maxMessageSize == 0 || options.maxMessageSize > std::numeric_limits<uint32_t>::max()) {
            lastError = "maximum message size must be between 1 and UINT32_MAX bytes";
            return;
        }
        if (options.maxQueuedOutput < options.maxMessageSize + 4) {
            lastError = "maximum queued output must fit at least one maximum-sized frame";
            return;
        }
        if (options.maxClients == 0) {
            lastError = "maximum client count must be greater than zero";
            return;
        }
        if (!options.socketPath.is_absolute() || options.socketPath.filename().empty()) {
            lastError = "control socket path must be an absolute file path";
            return;
        }

        const auto  parent = options.socketPath.parent_path();
        struct stat parentStat{};
        if (lstat(parent.c_str(), &parentStat) < 0 || !S_ISDIR(parentStat.st_mode)) {
            lastError = std::format("control socket parent '{}' is not a directory", parent.string());
            return;
        }
        if (parentStat.st_uid != geteuid() || (parentStat.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
            lastError = std::format("control socket parent '{}' must be owned by the current user and private", parent.string());
            return;
        }

        sockaddr_un address{.sun_family = AF_UNIX};
        const auto  pathString = options.socketPath.string();
        if (pathString.size() >= sizeof(address.sun_path)) {
            lastError = "control socket path is too long";
            return;
        }

        struct stat existingStat{};
        if (lstat(options.socketPath.c_str(), &existingStat) == 0 || errno != ENOENT) {
            lastError = std::format("control socket path '{}' already exists or cannot be inspected", pathString);
            return;
        }

        listenFD = CFileDescriptor{socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)};
        if (!listenFD.isValid()) {
            lastError = std::format("failed to create control socket: {}", std::strerror(errno));
            return;
        }

        std::memcpy(address.sun_path, pathString.c_str(), pathString.size() + 1);
        const auto addressSize = sc<socklen_t>(offsetof(sockaddr_un, sun_path) + pathString.size() + 1);
        if (bind(listenFD.get(), rc<sockaddr*>(&address), addressSize) < 0) {
            lastError = std::format("failed to bind control socket: {}", std::strerror(errno));
            listenFD.reset();
            return;
        }

        struct stat socketStat{};
        if (lstat(options.socketPath.c_str(), &socketStat) < 0 || !S_ISSOCK(socketStat.st_mode)) {
            lastError = "bound control socket could not be verified";
            shutdown();
            return;
        }
        socketDevice = socketStat.st_dev;
        socketInode  = socketStat.st_ino;
        ownsSocket   = true;

        if (chmod(options.socketPath.c_str(), S_IRUSR | S_IWUSR) < 0) {
            lastError = std::format("failed to restrict control socket permissions: {}", std::strerror(errno));
            shutdown();
            return;
        }

        struct stat restrictedSocketStat{};
        if (lstat(options.socketPath.c_str(), &restrictedSocketStat) < 0 || !S_ISSOCK(restrictedSocketStat.st_mode) || restrictedSocketStat.st_uid != geteuid() ||
            (restrictedSocketStat.st_mode & 0777) != 0600 || restrictedSocketStat.st_dev != socketDevice || restrictedSocketStat.st_ino != socketInode) {
            lastError = "control socket permissions or identity could not be verified";
            shutdown();
            return;
        }

        const auto backlog = sc<int>(std::min(options.maxClients, sc<size_t>(INT_MAX)));
        if (listen(listenFD.get(), backlog) < 0) {
            lastError = std::format("failed to listen on control socket: {}", std::strerror(errno));
            shutdown();
            return;
        }

        if (!options.integrateWithEventLoop)
            return;
        if (!g_pCompositor || !g_pCompositor->m_wlEventLoop) {
            lastError = "Hyprland event loop is unavailable for the control socket";
            shutdown();
            return;
        }

        listenEventSource = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, listenFD.get(), WL_EVENT_READABLE, onListenEvent, this);
        if (!listenEventSource) {
            lastError = "failed to register control socket with the Hyprland event loop";
            shutdown();
        }
    }

    void shutdown() {
        pendingCaptures.clear();
        for (auto& client : clients) {
            if (client.eventSource)
                wl_event_source_remove(client.eventSource);
        }
        clients.clear();

        if (listenEventSource) {
            wl_event_source_remove(listenEventSource);
            listenEventSource = nullptr;
        }
        listenFD.reset();

        if (!ownsSocket)
            return;

        struct stat socketStat{};
        if (lstat(options.socketPath.c_str(), &socketStat) == 0 && S_ISSOCK(socketStat.st_mode) && socketStat.st_dev == socketDevice && socketStat.st_ino == socketInode)
            unlink(options.socketPath.c_str());
        ownsSocket = false;
    }

    void dispatchPendingEvents() {
        if (!listenFD.isValid())
            return;

        handleListenEvent(listenFD.get(), WL_EVENT_READABLE);
        std::vector<int> clientFDs;
        clientFDs.reserve(clients.size());
        for (const auto& client : clients)
            clientFDs.emplace_back(client.fd.get());

        for (const auto fd : clientFDs)
            handleClientEvent(fd, WL_EVENT_READABLE | WL_EVENT_WRITABLE);
    }

    void handleListenEvent(int fd, uint32_t mask) {
        if (fd != listenFD.get())
            return;
        if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) {
            lastError = "control socket listener received an event-loop error";
            shutdown();
            return;
        }
        if (!(mask & WL_EVENT_READABLE))
            return;

        constexpr size_t MAX_ACCEPTS_PER_TICK = 8;
        for (size_t accepted = 0; accepted < MAX_ACCEPTS_PER_TICK; ++accepted) {
            sockaddr_un     peerAddress{};
            socklen_t       peerAddressSize = sizeof(peerAddress);
            CFileDescriptor connection{accept4(listenFD.get(), rc<sockaddr*>(&peerAddress), &peerAddressSize, SOCK_CLOEXEC | SOCK_NONBLOCK)};
            if (!connection.isValid()) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                    lastError = std::format("failed to accept control socket client: {}", std::strerror(errno));
                if (errno == EINTR)
                    continue;
                return;
            }

            if (clients.size() >= options.maxClients) {
                rejectConnection(connection, "server_overloaded", "the control socket has reached its client limit");
                continue;
            }

#ifdef SO_PEERCRED
            ucred     credentials{};
            socklen_t credentialsSize = sizeof(credentials);
            if (getsockopt(connection.get(), SOL_SOCKET, SO_PEERCRED, &credentials, &credentialsSize) < 0 || credentialsSize != sizeof(credentials)) {
                rejectConnection(connection, "authorization_failed", "could not verify peer credentials");
                continue;
            }
            if (!realmControlPeerAuthorized(credentials.uid, *options.expectedPeerUID)) {
                rejectConnection(connection, "authorization_failed", "control socket clients must run as the Hyprland user");
                continue;
            }
#else
            rejectConnection(connection, "authorization_failed", "SO_PEERCRED is unavailable on this platform");
            continue;
#endif

            wl_event_source* eventSource = nullptr;
            if (options.integrateWithEventLoop) {
                eventSource = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, connection.get(), WL_EVENT_READABLE, onClientEvent, this);
                if (!eventSource) {
                    rejectConnection(connection, "server_error", "failed to register the client with the event loop");
                    continue;
                }
            }

            clients.emplace_back(SClient{
                .id          = nextClientID++,
                .fd          = std::move(connection),
                .eventSource = eventSource,
            });
            if (nextClientID == 0)
                nextClientID = 1;
        }
    }

    void rejectConnection(CFileDescriptor& connection, std::string_view code, std::string_view message) {
        const auto response = realmControlFrame(controlError(std::nullopt, code, message));
        if (!response.empty())
            send(connection.get(), response.data(), response.size(), MSG_NOSIGNAL);
    }

    void handleClientEvent(int fd, uint32_t mask) {
        auto client = findClient(fd);
        if (client == clients.end())
            return;
        if (mask & WL_EVENT_ERROR) {
            removeClient(client);
            return;
        }

        bool keep = true;
        if ((mask & WL_EVENT_READABLE) && !client->closeAfterWrite)
            keep = readClient(*client);
        if (keep && (mask & WL_EVENT_WRITABLE))
            keep = flushClient(*client);
        if (keep && (mask & WL_EVENT_HANGUP) && client->output.empty())
            keep = false;

        if (!keep) {
            client = findClient(fd);
            if (client != clients.end())
                removeClient(client);
            return;
        }

        client = findClient(fd);
        if (client != clients.end())
            updateClientEvents(*client);
    }

    bool readClient(SClient& client) {
        constexpr size_t       MAX_READS_PER_TICK    = 16;
        constexpr size_t       MAX_REQUESTS_PER_TICK = 8;
        std::array<char, 4096> buffer{};
        size_t                 processedRequests = 0;

        for (size_t reads = 0; reads < MAX_READS_PER_TICK; ++reads) {
            processFrames(client, processedRequests);
            if (client.closeAfterWrite)
                return true;

            compactInput(client);
            const auto buffered = client.input.size() - client.inputOffset;
            const auto capacity = options.maxMessageSize + 4 - buffered;
            if (capacity == 0) {
                queueTerminalError(client, "message_too_large", "control request exceeds the configured message limit");
                return true;
            }

            const auto received = recv(client.fd.get(), buffer.data(), std::min(buffer.size(), capacity), 0);
            if (received > 0) {
                client.input.append(buffer.data(), sc<size_t>(received));
                if (processedRequests >= MAX_REQUESTS_PER_TICK) {
                    queueTerminalError(client, "server_overloaded", "too many pipelined requests");
                    return true;
                }
                continue;
            }
            if (received == 0)
                return false;
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return true;
            return false;
        }

        processFrames(client, processedRequests);
        return true;
    }

    void processFrames(SClient& client, size_t& processedRequests) {
        constexpr size_t MAX_REQUESTS_PER_TICK = 8;
        while (processedRequests < MAX_REQUESTS_PER_TICK) {
            const auto available = client.input.size() - client.inputOffset;
            if (available < 4)
                return;

            const auto* header = rc<const unsigned char*>(client.input.data() + client.inputOffset);
            const auto  length = (sc<uint32_t>(header[0]) << 24) | (sc<uint32_t>(header[1]) << 16) | (sc<uint32_t>(header[2]) << 8) | sc<uint32_t>(header[3]);
            if (length > options.maxMessageSize) {
                queueTerminalError(client, "message_too_large", "control request exceeds the configured message limit");
                return;
            }
            if (available < sc<size_t>(length) + 4)
                return;

            if (client.queuedOutput > options.maxQueuedOutput - options.maxMessageSize - 4) {
                queueTerminalError(client, "server_overloaded", "control response queue is full");
                return;
            }

            const std::string_view  payload{client.input.data() + client.inputOffset + 4, length};
            std::optional<uint64_t> queuedCaptureID;
            const auto              response = realmControlRequestImpl(manager, windowManager, inputController, payload, &queuedCaptureID);
            if (!queueResponse(client, response)) {
                queueTerminalError(client, "response_too_large", "control response exceeds the configured message limit");
                return;
            }
            if (queuedCaptureID)
                pendingCaptures[*queuedCaptureID] = SPendingCapture{.clientID = client.id};

            client.inputOffset += sc<size_t>(length) + 4;
            ++processedRequests;
        }

        if (client.input.size() != client.inputOffset)
            queueTerminalError(client, "server_overloaded", "too many pipelined requests");
    }

    bool queueResponse(SClient& client, std::string_view response, CFileDescriptor descriptor = {}) {
        if (response.size() > options.maxMessageSize)
            return false;

        auto frame = realmControlFrame(response);
        if (frame.empty() || frame.size() > options.maxQueuedOutput - client.queuedOutput)
            return false;

        client.queuedOutput += frame.size();
        client.output.emplace_back(SOutputFrame{.frame = std::move(frame), .descriptor = std::move(descriptor)});
        return true;
    }

    void queueTerminalError(SClient& client, std::string_view code, std::string_view message) {
        const auto error = controlError(std::nullopt, code, message);
        queueResponse(client, error);
        client.closeAfterWrite = true;
        client.input.clear();
        client.inputOffset = 0;
    }

    bool flushClient(SClient& client) {
        constexpr size_t MAX_WRITES_PER_TICK = 16;
        for (size_t writes = 0; writes < MAX_WRITES_PER_TICK && !client.output.empty(); ++writes) {
            auto&      output    = client.output.front();
            const auto remaining = output.frame.size() - output.offset;
            ssize_t    sent      = -1;
            if (output.descriptor.isValid() && output.offset == 0) {
                iovec iov{
                    .iov_base = output.frame.data(),
                    .iov_len  = remaining,
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
                const auto frameFD  = output.descriptor.get();
                std::memcpy(CMSG_DATA(control), &frameFD, sizeof(frameFD));
                sent = sendmsg(client.fd.get(), &header, MSG_NOSIGNAL);
            } else
                sent = send(client.fd.get(), output.frame.data() + output.offset, remaining, MSG_NOSIGNAL);
            if (sent > 0) {
                if (output.descriptor.isValid())
                    output.descriptor.reset();
                output.offset += sc<size_t>(sent);
                client.queuedOutput -= sc<size_t>(sent);
                if (output.offset == output.frame.size())
                    client.output.pop_front();
                continue;
            }
            if (sent < 0 && errno == EINTR)
                continue;
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return true;
            return false;
        }

        return !client.closeAfterWrite || !client.output.empty();
    }

    void compactInput(SClient& client) {
        if (client.inputOffset == 0)
            return;
        if (client.inputOffset == client.input.size()) {
            client.input.clear();
            client.inputOffset = 0;
            return;
        }
        if (client.inputOffset < 4096)
            return;

        client.input.erase(0, client.inputOffset);
        client.inputOffset = 0;
    }

    void handleCaptureResult(SRealmCaptureResult result) {
        const auto pending = pendingCaptures.find(result.captureID);
        if (pending == pendingCaptures.end())
            return;

        const auto client = std::ranges::find(clients, pending->second.clientID, &SClient::id);
        pendingCaptures.erase(pending);
        if (client == clients.end())
            return;

        if (!result.error.empty()) {
            queueResponse(
                *client,
                std::format(R"({{"event":"realm.capture.failed","capture_id":{},"realm_id":{},"error":"{}"}})", result.captureID, result.realmID, escapeJSONStrings(result.error)));
            updateClientEvents(*client);
            return;
        }

        const auto formatName = result.format == REALM_CAPTURE_FORMAT_ARGB8888 ? "argb8888" : "xrgb8888";
        const auto response   = std::format(
            R"({{"event":"realm.capture.ready","capture_id":{},"realm_id":{},"frame":{{"transport":"scm_rights","fd_count":1,"format":{},"format_name":"{}","width":{},"height":{},"stride":{},"byte_size":{},"y_inverted":{}}}}})",
            result.captureID, result.realmID, result.format, formatName, result.width, result.height, result.stride, result.byteSize, result.flags & 1 ? "true" : "false");
        if (!queueResponse(*client, response, std::move(result.frameFD)))
            queueTerminalError(*client, "server_overloaded", "capture result could not be queued");
        updateClientEvents(*client);
    }

    void updateClientEvents(SClient& client) {
        if (!client.eventSource)
            return;

        uint32_t events = client.closeAfterWrite ? 0 : WL_EVENT_READABLE;
        if (!client.output.empty())
            events |= WL_EVENT_WRITABLE;
        wl_event_source_fd_update(client.eventSource, events);
    }

    std::vector<SClient>::iterator findClient(int fd) {
        return std::ranges::find_if(clients, [fd](const auto& client) { return client.fd.get() == fd; });
    }

    void removeClient(std::vector<SClient>::iterator client) {
        const auto clientID = client->id;
        if (client->eventSource)
            wl_event_source_remove(client->eventSource);
        clients.erase(client);
        std::erase_if(pendingCaptures, [clientID](const auto& entry) { return entry.second.clientID == clientID; });
    }

    CRealmManager&                      manager;
    CRealmWindowManager&                windowManager;
    CRealmInputControllerManager*       inputController = nullptr;
    SRealmControlServerOptions          options;
    CFileDescriptor                     listenFD;
    wl_event_source*                    listenEventSource = nullptr;
    std::vector<SClient>                clients;
    std::map<uint64_t, SPendingCapture> pendingCaptures;
    std::string                         lastError;
    dev_t                               socketDevice = 0;
    ino_t                               socketInode  = 0;
    bool                                ownsSocket   = false;
    uint64_t                            nextClientID = 1;
};

CRealmControlServer::CRealmControlServer(CRealmManager& manager, CRealmWindowManager& windowManager) :
    CRealmControlServer(manager, windowManager,
                        SRealmControlServerOptions{.socketPath = g_pCompositor ? std::filesystem::path{g_pCompositor->m_instancePath} / ".realm-control.sock" : ""}) {}

CRealmControlServer::CRealmControlServer(CRealmManager& manager, CRealmWindowManager& windowManager, SRealmControlServerOptions options) :
    m_impl(makeUnique<SImpl>(manager, windowManager, inputControllerManager().get(), std::move(options))) {}

CRealmControlServer::CRealmControlServer(CRealmManager& manager, CRealmWindowManager& windowManager, CRealmInputControllerManager& inputController,
                                         SRealmControlServerOptions options) : m_impl(makeUnique<SImpl>(manager, windowManager, &inputController, std::move(options))) {}

CRealmControlServer::~CRealmControlServer() = default;

bool CRealmControlServer::isListening() const {
    return m_impl->listenFD.isValid();
}

const std::filesystem::path& CRealmControlServer::socketPath() const {
    return m_impl->options.socketPath;
}

const std::string& CRealmControlServer::lastError() const {
    return m_impl->lastError;
}

void CRealmControlServer::dispatchPendingEvents() {
    m_impl->dispatchPendingEvents();
}

UP<CRealmControlServer>& Realm::controlServer() {
    static UP<CRealmControlServer> server;
    return server;
}
