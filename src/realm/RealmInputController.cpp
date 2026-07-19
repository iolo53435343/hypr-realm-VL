#include "RealmInputController.hpp"

#include "RealmManager.hpp"
#include "../Compositor.hpp"
#include "../debug/log/Logger.hpp"
#include "../managers/eventLoop/EventLoopManager.hpp"
#include "../managers/eventLoop/EventLoopTimer.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <map>
#include <poll.h>
#include <set>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <wayland-server-core.h>

#include <hyprutils/memory/Casts.hpp>
#include <hyprutils/os/FileDescriptor.hpp>

#if defined(__linux__)
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

using namespace Hyprutils::Memory;
using namespace Hyprutils::OS;
using namespace Realm;

std::string_view Realm::realmInputErrorName(eRealmInputError error) {
    switch (error) {
        case eRealmInputError::INVALID_COMMAND: return "invalid_params";
        case eRealmInputError::REALM_NOT_FOUND: return "realm_not_found";
        case eRealmInputError::INPUT_DENIED: return "input_denied";
        case eRealmInputError::CONTROLLER_UNAVAILABLE: return "controller_unavailable";
        case eRealmInputError::RATE_LIMITED: return "rate_limited";
        case eRealmInputError::TRANSPORT_ERROR: return "transport_error";
        case eRealmInputError::OBSERVATION_DENIED: return "observation_denied";
        case eRealmInputError::CAPTURE_BUSY: return "capture_busy";
        case eRealmInputError::CAPABILITY_DENIED: return "capability_denied";
    }

    return "input_error";
}

static SRealmInputControllerOptions defaultInputControllerOptions() {
    SRealmInputControllerOptions options;

#if defined(__linux__)
    std::error_code error;
    const auto      executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error)
        options.controllerBinary = executable.parent_path() / "hyprland-realm-agent-controller";
#elif defined(__FreeBSD__)
    std::error_code error;
    const auto      executable = std::filesystem::read_symlink("/proc/curproc/file", error);
    if (!error)
        options.controllerBinary = executable.parent_path() / "hyprland-realm-agent-controller";
#endif

    return options;
}

static bool pathIsWithin(const std::filesystem::path& path, const std::filesystem::path& parent) {
    auto pathPart   = path.begin();
    auto parentPart = parent.begin();
    while (parentPart != parent.end()) {
        if (pathPart == path.end() || *pathPart != *parentPart)
            return false;
        ++pathPart;
        ++parentPart;
    }

    return true;
}

static std::optional<eRealmCapability> capabilityForInputMessage(eRealmInputMessageType type) {
    switch (type) {
        case eRealmInputMessageType::POINTER_MOVE:
        case eRealmInputMessageType::POINTER_BUTTON:
        case eRealmInputMessageType::POINTER_CLICK:
        case eRealmInputMessageType::POINTER_SCROLL: return eRealmCapability::POINTER;
        case eRealmInputMessageType::KEYBOARD_KEY:
        case eRealmInputMessageType::KEYBOARD_PRESS:
        case eRealmInputMessageType::KEYBOARD_TYPE: return eRealmCapability::KEYBOARD;
        default: return std::nullopt;
    }
}

static std::expected<std::filesystem::path, std::string> realmWaylandSocketPath(const CRealm& realm) {
    if (realm.runtimeDirectory().empty() || realm.waylandSocket().empty())
        return std::unexpected("realm Wayland socket is unavailable");

    std::error_code error;
    const auto      runtime = std::filesystem::canonical(realm.runtimeDirectory(), error);
    if (error)
        return std::unexpected("realm runtime directory cannot be resolved");

    auto socketPath = std::filesystem::path{realm.waylandSocket()};
    if (!socketPath.is_absolute())
        socketPath = runtime / socketPath;

    const auto parent = std::filesystem::canonical(socketPath.parent_path(), error);
    if (error || !pathIsWithin(parent, runtime))
        return std::unexpected("realm Wayland socket is outside its private runtime directory");
    socketPath = parent / socketPath.filename();

    struct stat socketStat{};
    if (lstat(socketPath.c_str(), &socketStat) < 0 || !S_ISSOCK(socketStat.st_mode))
        return std::unexpected("realm Wayland socket is not a Unix socket");
    if (socketStat.st_uid != geteuid())
        return std::unexpected("realm Wayland socket is not owned by the Hyprland user");

    return socketPath;
}

static std::expected<CFileDescriptor, std::string> connectRealmWaylandSocket(const CRealm& realm) {
    auto path = realmWaylandSocketPath(realm);
    if (!path)
        return std::unexpected(path.error());

    sockaddr_un address{.sun_family = AF_UNIX};
    const auto  pathString = path->string();
    if (pathString.size() >= sizeof(address.sun_path))
        return std::unexpected("realm Wayland socket path is too long");
    std::memcpy(address.sun_path, pathString.c_str(), pathString.size() + 1);

    CFileDescriptor socketFD{socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (!socketFD.isValid())
        return std::unexpected(std::format("failed creating realm Wayland connection: {}", std::strerror(errno)));

    const auto addressSize = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + pathString.size() + 1);
    if (connect(socketFD.get(), reinterpret_cast<sockaddr*>(&address), addressSize) < 0)
        return std::unexpected(std::format("failed connecting the realm Wayland socket: {}", std::strerror(errno)));

#ifdef SO_PEERCRED
    ucred     credentials{};
    socklen_t credentialsSize = sizeof(credentials);
    if (getsockopt(socketFD.get(), SOL_SOCKET, SO_PEERCRED, &credentials, &credentialsSize) < 0 || credentialsSize != sizeof(credentials))
        return std::unexpected("failed verifying realm Wayland peer credentials");
    if (credentials.uid != geteuid() || credentials.pid != realm.compositorPID())
        return std::unexpected("realm Wayland socket peer does not match the supervised realm compositor");
#else
    return std::unexpected("realm Wayland peer credentials are unavailable on this platform");
#endif

    return socketFD;
}

static void closeControllerFDsExcept(int waylandFD, int controlFD) {
    bool usedCloseRange = false;

#if defined(__linux__) && defined(SYS_close_range)
    std::array<int, 2> preserved = {waylandFD, controlFD};
    std::ranges::sort(preserved);
    const bool firstClosed = preserved[0] <= 3 || syscall(SYS_close_range, 3U, static_cast<unsigned int>(preserved[0] - 1), 0U) == 0;
    const bool middleClosed =
        preserved[1] <= preserved[0] + 1 || syscall(SYS_close_range, static_cast<unsigned int>(preserved[0] + 1), static_cast<unsigned int>(preserved[1] - 1), 0U) == 0;
    const bool lastClosed = syscall(SYS_close_range, static_cast<unsigned int>(preserved[1] + 1), UINT_MAX, 0U) == 0;
    usedCloseRange        = firstClosed && middleClosed && lastClosed;
#endif

    if (usedCloseRange)
        return;

    const auto maxFD = std::max(1024L, sysconf(_SC_OPEN_MAX));
    for (int fd = 3; fd < maxFD; ++fd) {
        if (fd != waylandFD && fd != controlFD)
            close(fd);
    }
}

[[noreturn]] static void runInputController(const std::filesystem::path& binary, int waylandFD, int controlFD, int logFD) {
    if (dup2(logFD, STDOUT_FILENO) < 0 || dup2(logFD, STDERR_FILENO) < 0)
        _exit(127);

    const auto clearCloseOnExec = [](int fd) {
        const auto flags = fcntl(fd, F_GETFD);
        return flags >= 0 && fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC) == 0;
    };
    if (!clearCloseOnExec(waylandFD) || !clearCloseOnExec(controlFD))
        _exit(127);

    closeControllerFDsExcept(waylandFD, controlFD);

    sigset_t signalMask;
    sigemptyset(&signalMask);
    sigprocmask(SIG_SETMASK, &signalMask, nullptr);

#if defined(__linux__)
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() == 1)
        _exit(1);
#endif

    const auto           waylandArgument = std::to_string(waylandFD);
    const auto           controlArgument = std::to_string(controlFD);
    const auto           binaryString    = binary.string();
    std::array<char*, 6> argv            = {
        const_cast<char*>(binaryString.c_str()), const_cast<char*>("--wayland-fd"),          const_cast<char*>(waylandArgument.c_str()),
        const_cast<char*>("--control-fd"),       const_cast<char*>(controlArgument.c_str()), nullptr,
    };
    execv(binaryString.c_str(), argv.data());
    _exit(127);
}

struct CRealmInputControllerManager::SImpl {
    struct SController {
        struct SPendingCapture {
            uint64_t                              captureID = 0;
            uint32_t                              sequence  = 0;
            std::chrono::steady_clock::time_point deadline;
        };

        pid_t                                 pid = 0;
        CFileDescriptor                       controlFD;
        wl_event_source*                      eventSource = nullptr;
        bool                                  ready       = false;
        std::string                           error;
        double                                tokens = 0;
        std::chrono::steady_clock::time_point lastRefill;
        std::chrono::steady_clock::time_point startupDeadline;
        uint32_t                              nextSequence  = 1;
        double                                captureTokens = 0;
        std::chrono::steady_clock::time_point lastCaptureRefill;
        std::optional<SPendingCapture>        pendingCapture;
        std::set<uint32_t>                    ignoredCaptureSequences;
    };

    struct SReapingProcess {
        pid_t                                 pid = 0;
        std::chrono::steady_clock::time_point deadline;
    };

    SImpl(CRealmManager& manager_, SRealmInputControllerOptions options_) : manager(manager_), options(std::move(options_)) {
        if (options.ratePerSecond == 0)
            options.ratePerSecond = 1;
        if (options.burst == 0)
            options.burst = 1;
        if (options.captureRatePerSecond == 0)
            options.captureRatePerSecond = 1;
        if (options.captureBurst == 0)
            options.captureBurst = 1;

        lifecycleListener   = manager.m_events.lifecycle.listen([this](const SRealmLifecycleEvent& event) { handleLifecycle(event); });
        ownerListener       = manager.m_events.inputOwner.listen([this](const SRealmInputOwnerEvent& event) {
            if (event.realm && event.owner != eRealmInputOwner::AGENT)
                releaseAll(event.realm->id());
        });
        observationListener = manager.m_events.observationPermission.listen([this](const SRealmObservationPermissionEvent& event) {
            if (event.realm && event.permission == eRealmObservationPermission::DENIED)
                cancelCapture(event.realm->id(), "realm observation permission was revoked");
        });
        capabilityListener  = manager.m_events.capability.listen([this](const SRealmCapabilityEvent& event) {
            if (!event.realm || event.granted)
                return;
            if (event.capability == eRealmCapability::OBSERVE)
                cancelCapture(event.realm->id(), "realm observe capability was revoked");
            else
                releaseAll(event.realm->id());
        });
        setupMaintenanceTimer();
    }

    ~SImpl() {
        shuttingDown = true;
        capabilityListener.reset();
        observationListener.reset();
        ownerListener.reset();
        lifecycleListener.reset();
        if (maintenanceTimer) {
            maintenanceTimer->cancel();
            if (g_pEventLoopManager)
                g_pEventLoopManager->removeTimer(maintenanceTimer);
            maintenanceTimer.reset();
        }

        std::vector<uint64_t> realmIDs;
        realmIDs.reserve(controllers.size());
        for (const auto& [realmID, _] : controllers)
            realmIDs.emplace_back(realmID);
        for (const auto realmID : realmIDs)
            stopController(realmID);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        while (!reaping.empty() && std::chrono::steady_clock::now() < deadline) {
            reapProcesses(false);
            if (!reaping.empty())
                poll(nullptr, 0, 5);
        }
        reapProcesses(true);
    }

    static int onControllerEvent(int fd, uint32_t mask, void* data) {
        static_cast<SImpl*>(data)->handleControllerEvent(fd, mask);
        return 0;
    }

    void handleLifecycle(const SRealmLifecycleEvent& event) {
        if (!event.realm)
            return;

        if (event.type == eRealmLifecycleEvent::STARTED) {
            if (const auto started = startController(event.realm); !started) {
                failures[event.realm->id()] = started.error();
                if (Log::logger)
                    Log::logger->log(Log::ERR, "Failed starting input controller for realm '{}': {}", event.realm->name(), started.error());
            }
            return;
        }

        if (event.type == eRealmLifecycleEvent::PAUSED)
            cancelCapture(event.realm->id(), "realm was paused before the capture completed");

        if (event.type == eRealmLifecycleEvent::STOPPED || event.type == eRealmLifecycleEvent::FAILED || event.type == eRealmLifecycleEvent::DESTROYED)
            stopController(event.realm->id());
        if (event.type == eRealmLifecycleEvent::DESTROYED)
            failures.erase(event.realm->id());
    }

    void setupMaintenanceTimer() {
        if (!options.integrateWithEventLoop || !g_pEventLoopManager)
            return;

        maintenanceTimer = makeShared<CEventLoopTimer>(
            std::nullopt,
            [this](SP<CEventLoopTimer> self, void*) {
                dispatchMaintenance();
                if (!shuttingDown &&
                    (!reaping.empty() || std::ranges::any_of(controllers, [](const auto& entry) { return !entry.second.ready || entry.second.pendingCapture.has_value(); })))
                    self->updateTimeout(std::chrono::milliseconds(50));
            },
            nullptr);
        g_pEventLoopManager->addTimer(maintenanceTimer);
    }

    void scheduleMaintenance() {
        if (maintenanceTimer && !shuttingDown)
            maintenanceTimer->updateTimeout(std::chrono::milliseconds(50));
    }

    std::expected<void, std::string> startController(const SP<CRealm>& realm) {
        if (!realm)
            return std::unexpected("realm is unavailable");
        stopController(realm->id());
        failures.erase(realm->id());

        if (options.controllerBinary.empty() || access(options.controllerBinary.c_str(), X_OK) != 0)
            return std::unexpected(std::format("realm input controller is not executable: {}", options.controllerBinary.string()));

        auto waylandFD = connectRealmWaylandSocket(*realm);
        if (!waylandFD)
            return std::unexpected(waylandFD.error());

        int controlSockets[2] = {-1, -1};
        if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, controlSockets) < 0)
            return std::unexpected(std::format("failed creating the controller channel: {}", std::strerror(errno)));
        CFileDescriptor hostControl{controlSockets[0]};
        CFileDescriptor childControl{controlSockets[1]};

        const auto      logPath = std::filesystem::path{realm->runtimeDirectory()} / "input-controller.log";
        CFileDescriptor logFD{open(logPath.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR)};
        if (!logFD.isValid())
            return std::unexpected(std::format("failed opening the realm input controller log: {}", std::strerror(errno)));
        struct stat logStat{};
        if (fstat(logFD.get(), &logStat) < 0 || !S_ISREG(logStat.st_mode) || logStat.st_uid != geteuid() || logStat.st_nlink != 1 || fchmod(logFD.get(), S_IRUSR | S_IWUSR) < 0)
            return std::unexpected("realm input controller log is not a private regular file");

        const auto pid = fork();
        if (pid < 0)
            return std::unexpected(std::format("failed forking the realm input controller: {}", std::strerror(errno)));
        if (pid == 0)
            runInputController(options.controllerBinary, waylandFD->get(), childControl.get(), logFD.get());

        childControl.reset();
        waylandFD->reset();
        logFD.reset();

        const auto flags = fcntl(hostControl.get(), F_GETFL);
        if (flags < 0 || fcntl(hostControl.get(), F_SETFL, flags | O_NONBLOCK) < 0) {
            const auto error = errno;
            queueReap(pid, true);
            return std::unexpected(std::format("failed configuring the controller channel: {}", std::strerror(error)));
        }

        wl_event_source* eventSource = nullptr;
        if (options.integrateWithEventLoop) {
            if (!g_pCompositor || !g_pCompositor->m_wlEventLoop) {
                queueReap(pid, true);
                return std::unexpected("Hyprland event loop is unavailable for the realm input controller");
            }
            eventSource = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, hostControl.get(), WL_EVENT_READABLE, onControllerEvent, this);
            if (!eventSource) {
                queueReap(pid, true);
                return std::unexpected("failed registering the realm input controller with the event loop");
            }
        }

        controllers.emplace(realm->id(),
                            SController{
                                .pid               = pid,
                                .controlFD         = std::move(hostControl),
                                .eventSource       = eventSource,
                                .tokens            = static_cast<double>(options.burst),
                                .lastRefill        = std::chrono::steady_clock::now(),
                                .startupDeadline   = std::chrono::steady_clock::now() + options.startupTimeout,
                                .nextSequence      = 1,
                                .captureTokens     = static_cast<double>(options.captureBurst),
                                .lastCaptureRefill = std::chrono::steady_clock::now(),
                            });
        scheduleMaintenance();
        return {};
    }

    void stopController(uint64_t realmID) {
        const auto controller = controllers.find(realmID);
        if (controller == controllers.end())
            return;

        releaseAll(realmID);
        cancelCapture(realmID, "realm input controller stopped before the capture completed");
        if (controller->second.eventSource)
            wl_event_source_remove(controller->second.eventSource);
        controller->second.controlFD.reset();
        if (controller->second.pid > 1)
            queueReap(controller->second.pid, true);
        controllers.erase(controller);
    }

    void releaseAll(uint64_t realmID) {
        const auto controller = controllers.find(realmID);
        if (controller == controllers.end() || !controller->second.controlFD.isValid())
            return;

        SRealmInputMessage message{
            .type     = eRealmInputMessageType::RELEASE_ALL,
            .sequence = takeSequence(controller->second),
        };
        sendMessage(controller->second, message);
    }

    void cancelCapture(uint64_t realmID, std::string error) {
        const auto controller = controllers.find(realmID);
        if (controller == controllers.end() || !controller->second.pendingCapture)
            return;

        const auto pending = *controller->second.pendingCapture;
        controller->second.ignoredCaptureSequences.emplace(pending.sequence);
        if (controller->second.ignoredCaptureSequences.size() > 64)
            controller->second.ignoredCaptureSequences.erase(controller->second.ignoredCaptureSequences.begin());
        if (controller->second.controlFD.isValid())
            sendMessage(controller->second,
                        SRealmInputMessage{
                            .type     = eRealmInputMessageType::CAPTURE_CANCEL,
                            .sequence = pending.sequence,
                        });
        controller->second.pendingCapture.reset();
        emitCaptureResult(SRealmCaptureResult{
            .captureID = pending.captureID,
            .realmID   = realmID,
            .error     = std::move(error),
        });
    }

    void emitCaptureResult(SRealmCaptureResult result) {
        if (captureResultCallback)
            captureResultCallback(std::move(result));
    }

    uint32_t takeSequence(SController& controller) {
        const auto sequence = controller.nextSequence++;
        if (controller.nextSequence == 0)
            controller.nextSequence = 1;
        return sequence;
    }

    std::expected<void, SRealmInputError> sendMessage(SController& controller, const SRealmInputMessage& message) {
        auto packet = encodeRealmInputMessage(message);
        if (!packet)
            return std::unexpected(SRealmInputError{.code = eRealmInputError::INVALID_COMMAND, .message = packet.error()});

        const auto sent = send(controller.controlFD.get(), packet->data(), packet->size(), MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent < 0)
            return std::unexpected(
                SRealmInputError{.code = eRealmInputError::TRANSPORT_ERROR, .message = std::format("failed sending input to the realm controller: {}", std::strerror(errno))});
        if (static_cast<size_t>(sent) != packet->size())
            return std::unexpected(SRealmInputError{.code = eRealmInputError::TRANSPORT_ERROR, .message = "the realm controller accepted a partial packet"});
        return {};
    }

    std::expected<uint32_t, SRealmInputError> sendInput(uint64_t realmID, SRealmInputMessage message) {
        if (!realmInputMessageIsInputCommand(message.type))
            return std::unexpected(SRealmInputError{.code = eRealmInputError::INVALID_COMMAND, .message = "message is not an agent input command"});

        const auto realm = manager.realmByID(realmID);
        if (!realm)
            return std::unexpected(SRealmInputError{.code = eRealmInputError::REALM_NOT_FOUND, .message = std::format("realm {} does not exist", realmID)});
        if (realm->state() != eRealmState::RUNNING || realm->inputOwner() != eRealmInputOwner::AGENT)
            return std::unexpected(
                SRealmInputError{.code = eRealmInputError::INPUT_DENIED, .message = std::format("realm '{}' does not currently permit agent input", realm->name())});
        const auto capability = capabilityForInputMessage(message.type);
        if (!capability || !realm->capabilities().allows(*capability))
            return std::unexpected(SRealmInputError{
                .code    = eRealmInputError::CAPABILITY_DENIED,
                .message = std::format("realm '{}' does not have the {} capability", realm->name(), capability ? realmCapabilityName(*capability) : "required input")});

        const auto controller = controllers.find(realmID);
        if (controller == controllers.end()) {
            const auto failure = failures.find(realmID);
            return std::unexpected(SRealmInputError{.code    = eRealmInputError::CONTROLLER_UNAVAILABLE,
                                                    .message = failure == failures.end() ? "realm input controller is unavailable" : failure->second});
        }
        if (!controller->second.ready)
            return std::unexpected(SRealmInputError{.code    = eRealmInputError::CONTROLLER_UNAVAILABLE,
                                                    .message = controller->second.error.empty() ? "realm input controller is not ready" : controller->second.error});

        const auto cost    = realmInputEventCost(message);
        auto&      state   = controller->second;
        const auto now     = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration<double>(now - state.lastRefill).count();
        state.tokens       = std::min(static_cast<double>(options.burst), state.tokens + elapsed * static_cast<double>(options.ratePerSecond));
        state.lastRefill   = now;
        if (static_cast<double>(cost) > state.tokens)
            return std::unexpected(SRealmInputError{.code = eRealmInputError::RATE_LIMITED, .message = "realm input rate limit exceeded"});

        message.sequence = takeSequence(state);
        if (const auto sent = sendMessage(state, message); !sent)
            return std::unexpected(sent.error());
        state.tokens -= static_cast<double>(cost);
        return message.sequence;
    }

    std::expected<uint64_t, SRealmInputError> requestCapture(uint64_t realmID, std::optional<SRealmCaptureRegion> region) {
        const auto realm = manager.realmByID(realmID);
        if (!realm)
            return std::unexpected(SRealmInputError{.code = eRealmInputError::REALM_NOT_FOUND, .message = std::format("realm {} does not exist", realmID)});
        if (realm->state() != eRealmState::RUNNING)
            return std::unexpected(SRealmInputError{.code    = eRealmInputError::OBSERVATION_DENIED,
                                                    .message = std::format("realm '{}' cannot be observed while {}", realm->name(), realmStateName(realm->state()))});
        if (!realm->capabilities().allows(eRealmCapability::OBSERVE))
            return std::unexpected(
                SRealmInputError{.code = eRealmInputError::CAPABILITY_DENIED, .message = std::format("realm '{}' does not have the observe capability", realm->name())});
        if (realm->observationPermission() != eRealmObservationPermission::ALLOWED)
            return std::unexpected(
                SRealmInputError{.code = eRealmInputError::OBSERVATION_DENIED, .message = std::format("realm '{}' observation permission is denied", realm->name())});

        const auto controller = controllers.find(realmID);
        if (controller == controllers.end()) {
            const auto failure = failures.find(realmID);
            return std::unexpected(
                SRealmInputError{.code = eRealmInputError::CONTROLLER_UNAVAILABLE, .message = failure == failures.end() ? "realm controller is unavailable" : failure->second});
        }
        auto& state = controller->second;
        if (!state.ready)
            return std::unexpected(
                SRealmInputError{.code = eRealmInputError::CONTROLLER_UNAVAILABLE, .message = state.error.empty() ? "realm controller is not ready" : state.error});
        if (state.pendingCapture)
            return std::unexpected(SRealmInputError{.code = eRealmInputError::CAPTURE_BUSY, .message = "a capture is already pending for this realm"});

        const auto now          = std::chrono::steady_clock::now();
        const auto elapsed      = std::chrono::duration<double>(now - state.lastCaptureRefill).count();
        state.captureTokens     = std::min(static_cast<double>(options.captureBurst), state.captureTokens + elapsed * static_cast<double>(options.captureRatePerSecond));
        state.lastCaptureRefill = now;
        if (state.captureTokens < 1.0)
            return std::unexpected(SRealmInputError{.code = eRealmInputError::RATE_LIMITED, .message = "realm capture rate limit exceeded"});

        SRealmInputMessage message{
            .type = region ? eRealmInputMessageType::CAPTURE_REGION : eRealmInputMessageType::CAPTURE,
        };
        if (region) {
            message.x      = region->x;
            message.y      = region->y;
            message.width  = region->width;
            message.height = region->height;
        }
        message.sequence = takeSequence(state);
        state.ignoredCaptureSequences.erase(message.sequence);

        const auto captureID = takeCaptureID();
        if (const auto sent = sendMessage(state, message); !sent)
            return std::unexpected(sent.error());
        state.captureTokens -= 1.0;
        state.pendingCapture = SController::SPendingCapture{
            .captureID = captureID,
            .sequence  = message.sequence,
            .deadline  = now + options.captureTimeout,
        };
        scheduleMaintenance();
        return captureID;
    }

    uint64_t takeCaptureID() {
        const auto captureID = nextCaptureID++;
        if (nextCaptureID == 0)
            nextCaptureID = 1;
        return captureID;
    }

    void handleControllerEvent(int fd, uint32_t mask) {
        auto controller = std::ranges::find_if(controllers, [fd](const auto& entry) { return entry.second.controlFD.get() == fd; });
        if (controller == controllers.end())
            return;

        bool keep = !(mask & WL_EVENT_ERROR);
        if (keep && (mask & WL_EVENT_READABLE))
            keep = readController(controller->first, controller->second);
        if (keep && !(mask & WL_EVENT_HANGUP))
            return;

        const auto realmID = controller->first;
        const auto error   = controller->second.error.empty() ? "realm input controller disconnected" : controller->second.error;
        cancelCapture(realmID, error);
        controller = controllers.find(realmID);
        if (controller == controllers.end())
            return;
        detachController(controller);
        failures[realmID] = error;
    }

    bool readController(uint64_t realmID, SController& controller) {
        std::array<uint8_t, REALM_INPUT_MAX_TEXT_SIZE + 16> packet{};
        for (size_t reads = 0; reads < 16; ++reads) {
            iovec iov{
                .iov_base = packet.data(),
                .iov_len  = packet.size(),
            };
            std::array<char, CMSG_SPACE(sizeof(int) * 2)> ancillary{};
            msghdr                                        header{
                                                       .msg_iov        = &iov,
                                                       .msg_iovlen     = 1,
                                                       .msg_control    = ancillary.data(),
                                                       .msg_controllen = ancillary.size(),
            };
            const auto received = recvmsg(controller.controlFD.get(), &header, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
            if (received > 0) {
                std::vector<CFileDescriptor> receivedFDs;
                for (auto* control = CMSG_FIRSTHDR(&header); control; control = CMSG_NXTHDR(&header, control)) {
                    if (control->cmsg_level != SOL_SOCKET || control->cmsg_type != SCM_RIGHTS || control->cmsg_len < CMSG_LEN(sizeof(int))) {
                        controller.error = "realm controller sent invalid ancillary data";
                        return false;
                    }
                    const auto  descriptorCount = (control->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                    const auto* descriptors     = rc<const int*>(CMSG_DATA(control));
                    for (size_t index = 0; index < descriptorCount; ++index)
                        receivedFDs.emplace_back(descriptors[index]);
                }
                if (header.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) {
                    controller.error = "realm controller sent a truncated packet";
                    return false;
                }
                if (receivedFDs.size() > 1) {
                    controller.error = "realm controller sent more than one frame descriptor";
                    return false;
                }

                CFileDescriptor receivedFD = receivedFDs.empty() ? CFileDescriptor{} : std::move(receivedFDs.front());

                auto            message = decodeRealmInputMessage(packet.data(), static_cast<size_t>(received));
                if (!message) {
                    controller.error = message.error();
                    return false;
                }

                if (message->type == eRealmInputMessageType::READY) {
                    if (receivedFD.isValid()) {
                        controller.error = "realm controller attached a descriptor to its ready status";
                        return false;
                    }
                    controller.ready = true;
                    controller.error.clear();
                } else if (message->type == eRealmInputMessageType::ERROR) {
                    if (receivedFD.isValid()) {
                        controller.error = "realm controller attached a descriptor to an error status";
                        return false;
                    }
                    const auto error = message->text.empty() ? "realm controller reported an error" : message->text;
                    if (controller.ignoredCaptureSequences.erase(message->sequence) > 0)
                        continue;
                    if (controller.pendingCapture && controller.pendingCapture->sequence == message->sequence) {
                        const auto captureID = controller.pendingCapture->captureID;
                        controller.pendingCapture.reset();
                        emitCaptureResult(SRealmCaptureResult{
                            .captureID = captureID,
                            .realmID   = realmID,
                            .error     = error,
                        });
                    } else if (!controller.ready || message->sequence == 0)
                        controller.error = error;
                    if (Log::logger)
                        Log::logger->log(Log::ERR, "Realm {} controller: {}", realmID, error);
                } else if (message->type == eRealmInputMessageType::CAPTURE_READY) {
                    if (controller.ignoredCaptureSequences.erase(message->sequence) > 0)
                        continue;
                    if (!receivedFD.isValid() || !controller.pendingCapture || controller.pendingCapture->sequence != message->sequence) {
                        controller.error = "realm controller sent an unexpected capture result";
                        return false;
                    }

                    struct stat frameStat{};
                    if (fstat(receivedFD.get(), &frameStat) < 0 || !S_ISREG(frameStat.st_mode) || frameStat.st_uid != geteuid() || frameStat.st_size < 0 ||
                        static_cast<uint64_t>(frameStat.st_size) != message->byteSize) {
                        controller.error = "realm controller sent an invalid capture descriptor";
                        return false;
                    }

                    const auto captureID = controller.pendingCapture->captureID;
                    controller.pendingCapture.reset();
                    emitCaptureResult(SRealmCaptureResult{
                        .captureID = captureID,
                        .realmID   = realmID,
                        .format    = message->format,
                        .width     = message->width,
                        .height    = message->height,
                        .stride    = message->stride,
                        .flags     = message->flags,
                        .byteSize  = message->byteSize,
                        .frameFD   = std::move(receivedFD),
                    });
                } else {
                    controller.error = "realm controller sent an invalid status message";
                    return false;
                }
                continue;
            }
            if (received == 0)
                return false;
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return true;
            controller.error = std::format("failed reading realm input controller status: {}", std::strerror(errno));
            return false;
        }

        return true;
    }

    void detachController(std::map<uint64_t, SController>::iterator controller) {
        if (controller->second.eventSource)
            wl_event_source_remove(controller->second.eventSource);
        controller->second.controlFD.reset();
        if (controller->second.pid > 1)
            queueReap(controller->second.pid, false);
        controllers.erase(controller);
    }

    void queueReap(pid_t pid, bool terminate) {
        if (pid <= 1)
            return;
        if (terminate)
            kill(pid, SIGTERM);
        reaping.emplace_back(SReapingProcess{
            .pid      = pid,
            .deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250),
        });
        scheduleMaintenance();
    }

    void reapProcesses(bool force) {
        const auto now = std::chrono::steady_clock::now();
        std::erase_if(reaping, [now, force](const SReapingProcess& process) {
            int        status = 0;
            const auto waited = waitpid(process.pid, &status, WNOHANG);
            if (waited == process.pid || (waited < 0 && errno == ECHILD))
                return true;
            if (!force && now < process.deadline)
                return false;
            if (process.pid > 1)
                kill(process.pid, SIGKILL);
            do {
                errno = 0;
            } while (waitpid(process.pid, &status, 0) < 0 && errno == EINTR);
            return true;
        });
    }

    void dispatchPendingEvents() {
        std::vector<int> fds;
        fds.reserve(controllers.size());
        for (const auto& [_, controller] : controllers)
            fds.emplace_back(controller.controlFD.get());
        for (const auto fd : fds)
            handleControllerEvent(fd, WL_EVENT_READABLE);
        dispatchMaintenance();
    }

    void dispatchMaintenance() {
        const auto            now = std::chrono::steady_clock::now();
        std::vector<uint64_t> timedOut;
        std::vector<uint64_t> capturesTimedOut;
        for (const auto& [realmID, controller] : controllers) {
            if (!controller.ready && now >= controller.startupDeadline)
                timedOut.emplace_back(realmID);
            if (controller.pendingCapture && now >= controller.pendingCapture->deadline)
                capturesTimedOut.emplace_back(realmID);
        }
        for (const auto realmID : capturesTimedOut)
            cancelCapture(realmID, "realm capture timed out");
        for (const auto realmID : timedOut) {
            failures[realmID] = "realm input controller timed out during startup";
            stopController(realmID);
        }
        reapProcesses(false);
    }

    bool controllerReady(uint64_t realmID) const {
        const auto controller = controllers.find(realmID);
        return controller != controllers.end() && controller->second.ready;
    }

    std::string controllerError(uint64_t realmID) const {
        if (const auto controller = controllers.find(realmID); controller != controllers.end())
            return controller->second.error;
        if (const auto failure = failures.find(realmID); failure != failures.end())
            return failure->second;
        return {};
    }

    CRealmManager&                           manager;
    SRealmInputControllerOptions             options;
    std::map<uint64_t, SController>          controllers;
    std::map<uint64_t, std::string>          failures;
    std::vector<SReapingProcess>             reaping;
    CHyprSignalListener                      lifecycleListener;
    CHyprSignalListener                      ownerListener;
    CHyprSignalListener                      observationListener;
    CHyprSignalListener                      capabilityListener;
    SP<CEventLoopTimer>                      maintenanceTimer;
    std::function<void(SRealmCaptureResult)> captureResultCallback;
    uint64_t                                 nextCaptureID = 1;
    bool                                     shuttingDown  = false;
};

CRealmInputControllerManager::CRealmInputControllerManager(CRealmManager& manager) : CRealmInputControllerManager(manager, defaultInputControllerOptions()) {
    ;
}

CRealmInputControllerManager::CRealmInputControllerManager(CRealmManager& manager, SRealmInputControllerOptions options) : m_impl(makeUnique<SImpl>(manager, std::move(options))) {}

CRealmInputControllerManager::~CRealmInputControllerManager() = default;

std::expected<uint32_t, SRealmInputError> CRealmInputControllerManager::sendInput(uint64_t realmID, SRealmInputMessage message) {
    return m_impl->sendInput(realmID, std::move(message));
}

std::expected<uint64_t, SRealmInputError> CRealmInputControllerManager::requestCapture(uint64_t realmID, std::optional<SRealmCaptureRegion> region) {
    return m_impl->requestCapture(realmID, region);
}

void CRealmInputControllerManager::setCaptureResultCallback(std::function<void(SRealmCaptureResult)> callback) {
    m_impl->captureResultCallback = std::move(callback);
}

bool CRealmInputControllerManager::controllerReady(uint64_t realmID) const {
    return m_impl->controllerReady(realmID);
}

std::string CRealmInputControllerManager::controllerError(uint64_t realmID) const {
    return m_impl->controllerError(realmID);
}

void CRealmInputControllerManager::dispatchPendingEvents() {
    m_impl->dispatchPendingEvents();
}

UP<CRealmInputControllerManager>& Realm::inputControllerManager() {
    static UP<CRealmInputControllerManager> controllerManager;
    return controllerManager;
}
