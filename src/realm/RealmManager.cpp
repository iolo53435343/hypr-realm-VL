#include "RealmManager.hpp"

#include "../Compositor.hpp"
#include "../debug/log/Logger.hpp"
#include "../managers/eventLoop/EventLoopManager.hpp"
#include "../managers/eventLoop/EventLoopTimer.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <climits>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <poll.h>
#include <random>
#include <ranges>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include <hyprutils/memory/Casts.hpp>

#if defined(__linux__)
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

using namespace Realm;
using namespace Hyprutils::Memory;
using namespace Hyprutils::OS;

enum class eSupervisorMessageType : uint32_t {
    STARTED = 1,
    START_FAILED,
    EXITED,
};

struct SSupervisorMessage {
    eSupervisorMessageType type  = eSupervisorMessageType::START_FAILED;
    int32_t                value = 0;
    int64_t                pid   = 0;
};

static_assert(sizeof(SSupervisorMessage) <= PIPE_BUF);

static volatile sig_atomic_t SUPERVISED_PROCESS_GROUP = 0;

static bool                  createCloexecPipe(int (&fds)[2]) {
    if (pipe(fds) < 0)
        return false;

    if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) == 0 && fcntl(fds[1], F_SETFD, FD_CLOEXEC) == 0)
        return true;

    const int error = errno;
    close(fds[0]);
    close(fds[1]);
    fds[0] = -1;
    fds[1] = -1;
    errno  = error;
    return false;
}

static bool writeSupervisorMessage(int fd, const SSupervisorMessage& message) {
    const auto* bytes   = rc<const char*>(&message);
    size_t      written = 0;

    while (written < sizeof(message)) {
        const auto result = write(fd, bytes + written, sizeof(message) - written);
        if (result > 0) {
            written += result;
            continue;
        }

        if (result < 0 && errno == EINTR)
            continue;

        return false;
    }

    return true;
}

static void supervisorTerminationSignal(int) {
    if (SUPERVISED_PROCESS_GROUP > 1)
        kill(-SUPERVISED_PROCESS_GROUP, SIGTERM);

    alarm(5);
}

static void supervisorAlarmSignal(int) {
    if (SUPERVISED_PROCESS_GROUP > 1)
        kill(-SUPERVISED_PROCESS_GROUP, SIGKILL);
}

static void closeSupervisorFDsExcept(int preservedFD) {
    bool usedCloseRange = false;

#if defined(__linux__) && defined(SYS_close_range)
    const bool lowerClosed = preservedFD <= 3 || syscall(SYS_close_range, 3U, sc<unsigned int>(preservedFD - 1), 0U) == 0;
    const bool upperClosed = syscall(SYS_close_range, sc<unsigned int>(preservedFD + 1), UINT_MAX, 0U) == 0;
    usedCloseRange         = lowerClosed && upperClosed;
#endif

    if (usedCloseRange)
        return;

    const auto maxFD = std::max(1024L, sysconf(_SC_OPEN_MAX));
    for (int fd = 3; fd < maxFD; ++fd) {
        if (fd != preservedFD)
            close(fd);
    }
}

static int processExitCode(int status) {
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return -1;
}

[[noreturn]] static void runSupervisor(int statusFD, const std::vector<std::string>& arguments, const std::vector<std::pair<std::string, std::string>>& environment,
                                       const std::string& logPath) {
    closeSupervisorFDsExcept(statusFD);

    sigset_t signalMask;
    sigemptyset(&signalMask);
    if (sigprocmask(SIG_SETMASK, &signalMask, nullptr) < 0) {
        writeSupervisorMessage(statusFD, {.type = eSupervisorMessageType::START_FAILED, .value = errno});
        _exit(1);
    }

    struct sigaction childAction = {};
    childAction.sa_handler       = SIG_DFL;
    sigemptyset(&childAction.sa_mask);
    sigaction(SIGCHLD, &childAction, nullptr);

    struct sigaction terminationAction = {};
    terminationAction.sa_handler       = supervisorTerminationSignal;
    sigemptyset(&terminationAction.sa_mask);
    sigaction(SIGTERM, &terminationAction, nullptr);

    struct sigaction alarmAction = {};
    alarmAction.sa_handler       = supervisorAlarmSignal;
    sigemptyset(&alarmAction.sa_mask);
    sigaction(SIGALRM, &alarmAction, nullptr);

#if defined(__linux__)
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() == 1)
        _exit(1);
#endif

    int execStatusPipe[2] = {-1, -1};
    if (!createCloexecPipe(execStatusPipe)) {
        writeSupervisorMessage(statusFD, {.type = eSupervisorMessageType::START_FAILED, .value = errno});
        _exit(1);
    }

    const auto compositorPID = fork();
    if (compositorPID < 0) {
        writeSupervisorMessage(statusFD, {.type = eSupervisorMessageType::START_FAILED, .value = errno});
        _exit(1);
    }

    if (compositorPID == 0) {
        close(execStatusPipe[0]);
        close(statusFD);

        if (setpgid(0, 0) < 0) {
            const int error = errno;
            write(execStatusPipe[1], &error, sizeof(error));
            _exit(127);
        }

        const int logFD = open(logPath.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, S_IRUSR | S_IWUSR);
        if (logFD < 0 || dup2(logFD, STDOUT_FILENO) < 0 || dup2(logFD, STDERR_FILENO) < 0) {
            const int error = errno;
            write(execStatusPipe[1], &error, sizeof(error));
            _exit(127);
        }
        close(logFD);

        for (const auto& [name, value] : environment) {
            if (setenv(name.c_str(), value.c_str(), 1) == 0)
                continue;

            const int error = errno;
            write(execStatusPipe[1], &error, sizeof(error));
            _exit(127);
        }

        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1);
        for (const auto& argument : arguments)
            argv.push_back(cc<char*>(argument.c_str()));
        argv.push_back(nullptr);

        execv(argv.front(), argv.data());

        const int error = errno;
        write(execStatusPipe[1], &error, sizeof(error));
        _exit(127);
    }

    close(execStatusPipe[1]);
    setpgid(compositorPID, compositorPID);
    SUPERVISED_PROCESS_GROUP = compositorPID;

    int     execError = 0;
    ssize_t execRead  = 0;
    do {
        execRead = read(execStatusPipe[0], &execError, sizeof(execError));
    } while (execRead < 0 && errno == EINTR);
    close(execStatusPipe[0]);

    if (execRead > 0)
        writeSupervisorMessage(statusFD, {.type = eSupervisorMessageType::START_FAILED, .value = execError, .pid = compositorPID});
    else
        writeSupervisorMessage(statusFD, {.type = eSupervisorMessageType::STARTED, .pid = compositorPID});

    int   status = 0;
    pid_t waited = 0;
    do {
        waited = waitpid(compositorPID, &status, 0);
    } while (waited < 0 && errno == EINTR);

    writeSupervisorMessage(statusFD, {.type = eSupervisorMessageType::EXITED, .value = waited == compositorPID ? processExitCode(status) : -1, .pid = compositorPID});
    close(statusFD);
    _exit(0);
}

static std::expected<SSupervisorMessage, std::string> readSupervisorHandshake(int fd) {
    pollfd descriptor = {.fd = fd, .events = POLLIN};
    int    pollResult = 0;
    do {
        pollResult = poll(&descriptor, 1, 5000);
    } while (pollResult < 0 && errno == EINTR);

    if (pollResult == 0)
        return std::unexpected("realm process supervisor timed out during exec");
    if (pollResult < 0)
        return std::unexpected(std::format("failed polling realm process supervisor: {}", strerror(errno)));

    SSupervisorMessage message;
    auto*              bytes     = rc<char*>(&message);
    size_t             readBytes = 0;
    while (readBytes < sizeof(message)) {
        const auto result = read(fd, bytes + readBytes, sizeof(message) - readBytes);
        if (result > 0) {
            readBytes += result;
            continue;
        }
        if (result < 0 && errno == EINTR)
            continue;
        if (result == 0)
            return std::unexpected("realm process supervisor exited before reporting exec status");
        return std::unexpected(std::format("failed reading realm process supervisor: {}", strerror(errno)));
    }

    return message;
}

static bool processGroupExists(pid_t processGroupPID) {
    if (processGroupPID <= 1)
        return false;

    if (kill(-processGroupPID, 0) == 0)
        return true;

    return errno == EPERM;
}

static std::string realmConfig() {
    return R"REALM(-- Generated by Hyprland's native agent realm manager.
hl.monitor({
    output = "",
    mode = "1280x720@60",
    position = "0x0",
    scale = "1",
})

hl.monitor({
    output = "FALLBACK",
    disabled = true,
})

hl.config({
    general = {
        border_size = 4,
        gaps_in = 4,
        gaps_out = 8,
        col = {
            active_border = "rgba(ff3ac8ff)",
            inactive_border = "rgba(6f3dc4dd)",
        },
    },
    decoration = {
        rounding = 6,
        shadow = { enabled = false },
        blur = { enabled = false },
    },
    animations = { enabled = false },
    misc = {
        background_color = "rgba(17111fff)",
        disable_hyprland_logo = true,
        disable_splash_rendering = true,
        disable_watchdog_warning = true,
        disable_xdg_env_checks = true,
        force_default_wallpaper = -1,
    },
    debug = {
        disable_logs = false,
        enable_stdout_logs = true,
    },
})
)REALM";
}

static std::expected<void, std::string> writePrivateFile(const std::filesystem::path& path, const std::string& contents, std::filesystem::perms permissions) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream)
        return std::unexpected(std::format("failed opening {}", path.string()));

    stream << contents;
    stream.close();
    if (!stream)
        return std::unexpected(std::format("failed writing {}", path.string()));

    std::error_code error;
    std::filesystem::permissions(path, permissions, std::filesystem::perm_options::replace, error);
    if (error)
        return std::unexpected(std::format("failed setting permissions on {}: {}", path.string(), error.message()));

    return {};
}

static std::expected<std::filesystem::path, std::string> createRuntimeDirectory(const std::filesystem::path& root) {
    static constexpr std::string_view     ALPHABET = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

    std::random_device                    randomDevice;
    std::mt19937                          generator(randomDevice());
    std::uniform_int_distribution<size_t> distribution(0, ALPHABET.size() - 1);

    for (size_t attempt = 0; attempt < 64; ++attempt) {
        std::string suffix(6, '0');
        for (auto& character : suffix)
            character = ALPHABET[distribution(generator)];

        const auto      candidate = root / std::format("hr.{}", suffix);
        std::error_code error;
        if (!std::filesystem::create_directory(candidate, error)) {
            if (!error || error == std::errc::file_exists)
                continue;
            return std::unexpected(std::format("failed creating realm runtime directory: {}", error.message()));
        }

        std::filesystem::permissions(candidate, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, error);
        if (error) {
            std::filesystem::remove(candidate);
            return std::unexpected(std::format("failed securing realm runtime directory: {}", error.message()));
        }

        return candidate;
    }

    return std::unexpected("failed creating a unique realm runtime directory");
}

static std::optional<std::string> readyWaylandSocket(const CRealm& realm, pid_t compositorPID) {
    const auto      hyprDirectory = std::filesystem::path(realm.runtimeDirectory()) / "hypr";
    std::error_code error;
    if (!std::filesystem::is_directory(hyprDirectory, error))
        return std::nullopt;

    for (std::filesystem::directory_iterator iterator(hyprDirectory, error), end; !error && iterator != end; iterator.increment(error)) {
        const auto    lockPath = iterator->path() / "hyprland.lock";
        std::ifstream lock(lockPath);
        if (!lock)
            continue;

        std::string pidString;
        std::string socket;
        if (!std::getline(lock, pidString) || !std::getline(lock, socket))
            continue;

        pid_t parsedPID            = 0;
        const auto [_, parseError] = std::from_chars(pidString.data(), pidString.data() + pidString.size(), parsedPID);
        if (parseError != std::errc{} || parsedPID != compositorPID || socket.empty())
            continue;

        auto socketPath = std::filesystem::path(socket);
        if (!socketPath.is_absolute())
            socketPath = std::filesystem::path(realm.runtimeDirectory()) / socketPath;

        if (std::filesystem::is_socket(socketPath, error))
            return socket;
        error.clear();
    }

    return std::nullopt;
}

UP<CRealmManager>& Realm::manager() {
    static UP<CRealmManager> realmManager;
    return realmManager;
}

SRealmManagerOptions CRealmManager::defaultOptions() {
    SRealmManagerOptions options;

    if (const auto* runtime = getenv("XDG_RUNTIME_DIR"); runtime)
        options.runtimeRoot = runtime;

#if defined(__linux__)
    std::error_code error;
    options.compositorBinary = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error)
        options.compositorBinary.clear();
#elif defined(__FreeBSD__)
    std::error_code error;
    options.compositorBinary = std::filesystem::read_symlink("/proc/curproc/file", error);
    if (error)
        options.compositorBinary.clear();
#endif

    if (g_pCompositor) {
        auto socket = std::filesystem::path(g_pCompositor->m_wlDisplaySocket);
        if (!socket.is_absolute())
            socket = options.runtimeRoot / socket;
        options.hostWaylandSocket = socket.string();
    }

    return options;
}

CRealmManager::CRealmManager() : CRealmManager(defaultOptions()) {
    ;
}

CRealmManager::CRealmManager(SRealmManagerOptions options) : m_options(std::move(options)) {
    setupPollTimer();
}

CRealmManager::~CRealmManager() {
    shutdownAll();

    if (m_pollTimer) {
        m_pollTimer->cancel();
        if (g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(m_pollTimer);
        m_pollTimer.reset();
    }
}

void CRealmManager::setupPollTimer() {
    if (!m_options.integrateWithEventLoop || !g_pEventLoopManager)
        return;

    m_pollTimer = makeShared<CEventLoopTimer>(
        std::nullopt,
        [this](SP<CEventLoopTimer> self, void*) {
            dispatchPendingEvents();
            if (!m_shuttingDown && !m_processes.empty())
                self->updateTimeout(std::chrono::milliseconds(50));
        },
        nullptr);
    g_pEventLoopManager->addTimer(m_pollTimer);
}

std::expected<void, std::string> CRealmManager::validateName(const std::string& name) const {
    if (name.empty())
        return std::unexpected("realm name cannot be empty");
    if (name.size() > 128)
        return std::unexpected("realm name cannot exceed 128 bytes");
    if (std::ranges::any_of(name, [](unsigned char character) { return character < 0x20 || character == 0x7F; }))
        return std::unexpected("realm name cannot contain control characters");
    if (realmByName(name))
        return std::unexpected(std::format("realm '{}' already exists", name));
    return {};
}

std::expected<SP<CRealm>, std::string> CRealmManager::createRealm(const std::string& name) {
    if (m_shuttingDown)
        return std::unexpected("realm manager is shutting down");

    if (const auto valid = validateName(name); !valid)
        return std::unexpected(valid.error());

    if (m_nextID == 0)
        return std::unexpected("realm ID space exhausted");

    auto realm = makeShared<CRealm>(m_nextID++, name);
    m_realms.emplace_back(realm);
    return realm;
}

std::expected<void, std::string> CRealmManager::prepareRuntime(CRealm& realm) {
    if (const auto cleaned = cleanupRuntime(realm); !cleaned)
        return cleaned;

    std::error_code error;
    if (m_options.runtimeRoot.empty())
        return std::unexpected("realm runtime root is unavailable");
    const auto runtimeRoot = std::filesystem::canonical(m_options.runtimeRoot, error);
    if (error || !std::filesystem::is_directory(runtimeRoot, error))
        return std::unexpected("realm runtime root is unavailable");
    if (m_options.compositorBinary.empty() || access(m_options.compositorBinary.c_str(), X_OK) != 0)
        return std::unexpected(std::format("realm compositor is not executable: {}", m_options.compositorBinary.string()));
    if (m_options.hostWaylandSocket.empty())
        return std::unexpected("host Wayland socket is unavailable");

    auto runtime = createRuntimeDirectory(runtimeRoot);
    if (!runtime)
        return std::unexpected(runtime.error());

    m_ownedRuntimeDirectories.emplace(*runtime);
    realm.m_runtimeDirectory = runtime->string();
    realm.m_configPath       = (*runtime / "realm.lua").string();
    realm.m_logPath          = (*runtime / "realm.log").string();
    realm.m_waylandSocket.clear();

    std::filesystem::create_directory(*runtime / "bin", error);
    if (!error)
        std::filesystem::create_directory(*runtime / "cache", error);
    if (!error)
        std::filesystem::create_directory(*runtime / "state", error);
    if (error) {
        cleanupRuntime(realm);
        return std::unexpected(std::format("failed creating realm runtime structure: {}", error.message()));
    }

    auto writeResult = writePrivateFile(realm.m_configPath, realmConfig(), std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    if (!writeResult) {
        cleanupRuntime(realm);
        return writeResult;
    }

    static constexpr std::string_view ENVIRONMENT_GUARD = "#!/bin/sh\nexit 0\n";
    const auto                        guardPermissions  = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec;
    writeResult                                         = writePrivateFile(*runtime / "bin/systemctl", std::string(ENVIRONMENT_GUARD), guardPermissions);
    if (writeResult)
        writeResult = writePrivateFile(*runtime / "bin/dbus-update-activation-environment", std::string(ENVIRONMENT_GUARD), guardPermissions);
    if (!writeResult) {
        cleanupRuntime(realm);
        return writeResult;
    }

    return {};
}

std::expected<CRealmManager::SLaunchResult, std::string> CRealmManager::launchRealmProcess(const CRealm& realm) const {
    int statusPipe[2] = {-1, -1};
    if (!createCloexecPipe(statusPipe))
        return std::unexpected(std::format("failed creating realm supervisor pipe: {}", strerror(errno)));

    CFileDescriptor                readFD{statusPipe[0]};
    CFileDescriptor                writeFD{statusPipe[1]};

    const std::vector<std::string> arguments = {
        m_options.compositorBinary.string(),
        "--config",
        realm.configPath(),
    };

    std::string path = (std::filesystem::path(realm.runtimeDirectory()) / "bin").string();
    if (const auto* inheritedPath = getenv("PATH"); inheritedPath && *inheritedPath)
        path += std::format(":{}", inheritedPath);

    const std::vector<std::pair<std::string, std::string>> environment = {
        {"XDG_RUNTIME_DIR", realm.runtimeDirectory()},
        {"XDG_CACHE_HOME", (std::filesystem::path(realm.runtimeDirectory()) / "cache").string()},
        {"XDG_STATE_HOME", (std::filesystem::path(realm.runtimeDirectory()) / "state").string()},
        {"WAYLAND_DISPLAY", m_options.hostWaylandSocket},
        {"PATH", path},
        {"HYPRLAND_REALM_ID", std::to_string(realm.id())},
        {"HYPRLAND_REALM_NAME", realm.name()},
        {"HYPRLAND_NO_RT", "1"},
        {"HYPRLAND_NO_SD_VARS", "1"},
    };

    const auto supervisorPID = fork();
    if (supervisorPID < 0)
        return std::unexpected(std::format("failed forking realm process supervisor: {}", strerror(errno)));

    if (supervisorPID == 0) {
        readFD.reset();
        runSupervisor(writeFD.take(), arguments, environment, realm.logPath());
    }

    writeFD.reset();
    auto handshake = readSupervisorHandshake(readFD.get());
    if (!handshake) {
        kill(supervisorPID, SIGTERM);
        waitpid(supervisorPID, nullptr, 0);
        return std::unexpected(handshake.error());
    }

    const auto currentFlags = fcntl(readFD.get(), F_GETFL);
    if (currentFlags < 0 || fcntl(readFD.get(), F_SETFL, currentFlags | O_NONBLOCK) < 0) {
        const int  error         = errno;
        const auto compositorPID = sc<pid_t>(handshake->pid);
        if (compositorPID > 1)
            kill(-compositorPID, SIGTERM);
        kill(supervisorPID, SIGTERM);
        waitpid(supervisorPID, nullptr, 0);
        return std::unexpected(std::format("failed configuring realm supervisor pipe: {}", strerror(error)));
    }

    return SLaunchResult{
        .supervisorPID = supervisorPID,
        .compositorPID = sc<pid_t>(handshake->pid),
        .execError     = handshake->type == eSupervisorMessageType::START_FAILED ? handshake->value : 0,
        .statusFD      = std::move(readFD),
    };
}

std::expected<void, std::string> CRealmManager::startRealm(uint64_t id) {
    if (m_shuttingDown)
        return std::unexpected("realm manager is shutting down");

    auto realm = realmByID(id);
    if (!realm)
        return std::unexpected(std::format("realm {} does not exist", id));
    if (m_processes.contains(id))
        return std::unexpected(std::format("realm '{}' still has an active process supervisor", realm->name()));

    auto transition = realm->transitionTo(eRealmState::CREATING);
    if (!transition)
        return transition;

    realm->m_exitCode = -1;
    if (const auto prepared = prepareRuntime(*realm); !prepared) {
        realm->transitionTo(eRealmState::FAILED);
        return prepared;
    }

    auto launched = launchRealmProcess(*realm);
    if (!launched) {
        realm->transitionTo(eRealmState::FAILED);
        return std::unexpected(launched.error());
    }

    realm->m_compositorPID = launched->execError == 0 ? launched->compositorPID : 0;
    auto& process          = m_processes
                        .emplace(id,
                                 SRealmProcess{
                                     .supervisorPID   = launched->supervisorPID,
                                     .processGroupPID = launched->compositorPID,
                                     .statusFD        = std::move(launched->statusFD),
                                     .startupDeadline = std::chrono::steady_clock::now() + m_options.startupTimeout,
                                 })
                        .first->second;

    if (m_pollTimer)
        m_pollTimer->updateTimeout(std::chrono::milliseconds(50));

    if (launched->execError != 0) {
        process.failOnExit = true;
        realm->m_exitCode  = 127;
        realm->transitionTo(eRealmState::FAILED);
        return std::unexpected(std::format("failed executing realm compositor: {}", strerror(launched->execError)));
    }

    return {};
}

bool CRealmManager::signalProcessGroup(const SRealmProcess& process, int signal) const {
    if (process.processGroupPID <= 1)
        return false;
    return kill(-process.processGroupPID, signal) == 0;
}

std::expected<void, std::string> CRealmManager::pauseRealm(uint64_t id) {
    auto realm = realmByID(id);
    if (!realm)
        return std::unexpected(std::format("realm {} does not exist", id));
    if (realm->state() != eRealmState::RUNNING)
        return std::unexpected(std::format("realm '{}' cannot be paused while {}", realm->name(), realmStateName(realm->state())));

    const auto process = m_processes.find(id);
    if (process == m_processes.end())
        return std::unexpected(std::format("realm '{}' has no active process", realm->name()));
    if (!signalProcessGroup(process->second, SIGSTOP))
        return std::unexpected(std::format("failed pausing realm '{}': {}", realm->name(), strerror(errno)));

    return realm->transitionTo(eRealmState::PAUSED);
}

std::expected<void, std::string> CRealmManager::resumeRealm(uint64_t id) {
    auto realm = realmByID(id);
    if (!realm)
        return std::unexpected(std::format("realm {} does not exist", id));
    if (realm->state() != eRealmState::PAUSED)
        return std::unexpected(std::format("realm '{}' cannot be resumed while {}", realm->name(), realmStateName(realm->state())));

    const auto process = m_processes.find(id);
    if (process == m_processes.end())
        return std::unexpected(std::format("realm '{}' has no active process", realm->name()));
    if (!signalProcessGroup(process->second, SIGCONT))
        return std::unexpected(std::format("failed resuming realm '{}': {}", realm->name(), strerror(errno)));

    return realm->transitionTo(eRealmState::RUNNING);
}

std::expected<void, std::string> CRealmManager::stopRealm(uint64_t id) {
    auto realm = realmByID(id);
    if (!realm)
        return std::unexpected(std::format("realm {} does not exist", id));
    if (realm->state() != eRealmState::CREATING && realm->state() != eRealmState::RUNNING && realm->state() != eRealmState::PAUSED)
        return std::unexpected(std::format("realm '{}' cannot be stopped while {}", realm->name(), realmStateName(realm->state())));

    const auto process = m_processes.find(id);
    if (process == m_processes.end())
        return std::unexpected(std::format("realm '{}' has no active process", realm->name()));

    if (realm->state() == eRealmState::PAUSED)
        signalProcessGroup(process->second, SIGCONT);
    if (!signalProcessGroup(process->second, SIGTERM))
        return std::unexpected(std::format("failed stopping realm '{}': {}", realm->name(), strerror(errno)));

    process->second.terminationDeadline = std::chrono::steady_clock::now() + m_options.stopTimeout;
    return realm->transitionTo(eRealmState::STOPPING);
}

void CRealmManager::handleProcessExit(CRealm& realm, SRealmProcess& process, int exitCode) {
    if (process.exitReceived)
        return;

    process.exitReceived = true;
    if (realm.state() != eRealmState::FAILED)
        realm.m_exitCode = exitCode;
    realm.m_compositorPID       = 0;
    process.terminationDeadline = std::chrono::steady_clock::now() + m_options.stopTimeout;

    signalProcessGroup(process, SIGTERM);

    if (realm.state() == eRealmState::STOPPING && !process.failOnExit)
        realm.transitionTo(eRealmState::STOPPED);
    else if (realm.state() != eRealmState::FAILED)
        realm.transitionTo(eRealmState::FAILED);
}

void CRealmManager::updateReadiness(CRealm& realm, SRealmProcess& process) {
    if (realm.state() != eRealmState::CREATING || realm.compositorPID() <= 1)
        return;

    if (const auto socket = readyWaylandSocket(realm, realm.compositorPID()); socket) {
        realm.m_waylandSocket = *socket;
        realm.transitionTo(eRealmState::RUNNING);
        return;
    }

    if (std::chrono::steady_clock::now() < process.startupDeadline)
        return;

    process.failOnExit          = true;
    process.terminationDeadline = std::chrono::steady_clock::now() + m_options.stopTimeout;
    realm.m_exitCode            = -1;
    realm.transitionTo(eRealmState::FAILED);
    signalProcessGroup(process, SIGTERM);
}

void CRealmManager::finishProcessCleanup(uint64_t id, SRealmProcess& process) {
    if (!process.exitReceived)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (processGroupExists(process.processGroupPID)) {
        if (process.terminationDeadline && now >= *process.terminationDeadline && !process.forceKillSent) {
            signalProcessGroup(process, SIGKILL);
            process.forceKillSent       = true;
            process.terminationDeadline = now + std::chrono::seconds(1);
        }
        return;
    }

    int        status = 0;
    const auto waited = waitpid(process.supervisorPID, &status, WNOHANG);
    if (waited == 0)
        return;
    if (waited < 0 && errno != ECHILD)
        return;

    m_processes.erase(id);
}

void CRealmManager::dispatchPendingEvents() {
    const auto ids = m_processes | std::views::keys | std::ranges::to<std::vector>();

    for (const auto id : ids) {
        auto processIterator = m_processes.find(id);
        if (processIterator == m_processes.end())
            continue;

        auto realm = realmByID(id);
        if (!realm) {
            signalProcessGroup(processIterator->second, SIGKILL);
            continue;
        }

        auto&                                            process = processIterator->second;
        std::array<char, sizeof(SSupervisorMessage) * 4> buffer;
        bool                                             reachedEOF = false;

        while (true) {
            const auto bytes = read(process.statusFD.get(), buffer.data(), buffer.size());
            if (bytes > 0) {
                process.statusBuffer.insert(process.statusBuffer.end(), buffer.data(), buffer.data() + bytes);
                continue;
            }
            if (bytes == 0)
                reachedEOF = true;
            if (bytes < 0 && errno == EINTR)
                continue;
            break;
        }

        while (process.statusBuffer.size() >= sizeof(SSupervisorMessage)) {
            SSupervisorMessage message;
            std::memcpy(&message, process.statusBuffer.data(), sizeof(message));
            process.statusBuffer.erase(process.statusBuffer.begin(), process.statusBuffer.begin() + sizeof(message));

            if (message.type == eSupervisorMessageType::EXITED)
                handleProcessExit(*realm, process, message.value);
        }

        if (reachedEOF && !process.exitReceived)
            handleProcessExit(*realm, process, -1);

        updateReadiness(*realm, process);

        const auto now = std::chrono::steady_clock::now();
        if (!process.exitReceived && process.terminationDeadline && now >= *process.terminationDeadline && !process.forceKillSent) {
            signalProcessGroup(process, SIGKILL);
            process.forceKillSent       = true;
            process.terminationDeadline = now + std::chrono::seconds(1);
        }

        finishProcessCleanup(id, process);
    }
}

std::expected<void, std::string> CRealmManager::cleanupRuntime(CRealm& realm) {
    if (realm.m_runtimeDirectory.empty())
        return {};

    const auto runtime = std::filesystem::path(realm.m_runtimeDirectory);
    const auto owned   = m_ownedRuntimeDirectories.find(runtime);
    if (owned == m_ownedRuntimeDirectories.end()) {
        Log::logger->log(Log::ERR, "Refusing to remove unowned realm runtime directory {}", runtime.string());
        return std::unexpected(std::format("refusing to remove unowned realm runtime directory {}", runtime.string()));
    }

    std::error_code error;
    std::filesystem::remove_all(runtime, error);
    if (error) {
        Log::logger->log(Log::ERR, "Failed removing realm runtime directory {}: {}", runtime.string(), error.message());
        return std::unexpected(std::format("failed removing realm runtime directory {}: {}", runtime.string(), error.message()));
    }

    m_ownedRuntimeDirectories.erase(owned);
    realm.m_runtimeDirectory.clear();
    realm.m_waylandSocket.clear();
    realm.m_configPath.clear();
    realm.m_logPath.clear();
    return {};
}

std::expected<void, std::string> CRealmManager::destroyRealm(uint64_t id) {
    auto realm = realmByID(id);
    if (!realm)
        return std::unexpected(std::format("realm {} does not exist", id));
    if (m_processes.contains(id))
        return std::unexpected(std::format("realm '{}' is still cleaning up its process", realm->name()));
    if (realm->state() != eRealmState::STOPPED && realm->state() != eRealmState::FAILED)
        return std::unexpected(std::format("realm '{}' cannot be destroyed while {}", realm->name(), realmStateName(realm->state())));

    if (const auto cleaned = cleanupRuntime(*realm); !cleaned)
        return cleaned;
    std::erase(m_realms, realm);
    return {};
}

SP<CRealm> CRealmManager::realmByID(uint64_t id) const {
    const auto realm = std::ranges::find(m_realms, id, &CRealm::id);
    return realm == m_realms.end() ? SP<CRealm>{} : *realm;
}

SP<CRealm> CRealmManager::realmByName(const std::string& name) const {
    const auto realm = std::ranges::find(m_realms, name, &CRealm::name);
    return realm == m_realms.end() ? SP<CRealm>{} : *realm;
}

const std::vector<SP<CRealm>>& CRealmManager::realms() const {
    return m_realms;
}

void CRealmManager::shutdownAll() {
    if (m_shuttingDown)
        return;

    m_shuttingDown = true;
    if (m_pollTimer)
        m_pollTimer->cancel();

    const auto stopDeadline = std::chrono::steady_clock::now() + m_options.stopTimeout;
    for (auto& [id, process] : m_processes) {
        auto realm = realmByID(id);
        if (realm && realm->state() == eRealmState::PAUSED)
            signalProcessGroup(process, SIGCONT);
        signalProcessGroup(process, SIGTERM);
        process.terminationDeadline = stopDeadline;

        if (realm && (realm->state() == eRealmState::CREATING || realm->state() == eRealmState::RUNNING || realm->state() == eRealmState::PAUSED))
            realm->transitionTo(eRealmState::STOPPING);
    }

    while (!m_processes.empty() && std::chrono::steady_clock::now() < stopDeadline) {
        dispatchPendingEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    for (auto& [_, process] : m_processes)
        signalProcessGroup(process, SIGKILL);

    const auto killDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!m_processes.empty() && std::chrono::steady_clock::now() < killDeadline) {
        dispatchPendingEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    for (auto& [id, process] : m_processes) {
        signalProcessGroup(process, SIGKILL);
        kill(process.supervisorPID, SIGKILL);
        waitpid(process.supervisorPID, nullptr, 0);

        if (auto realm = realmByID(id); realm) {
            realm->m_compositorPID = 0;
            if (realm->state() == eRealmState::STOPPING)
                realm->transitionTo(eRealmState::STOPPED);
        }
    }
    m_processes.clear();

    for (auto& realm : m_realms)
        cleanupRuntime(*realm);
}
