#include "Realm.hpp"

#include <cerrno>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

using namespace Realm;

static bool validXWaylandDisplay(std::string_view display) {
    if (display.size() < 2 || display.size() > 16 || display.front() != ':')
        return false;

    uint32_t displayNumber       = 0;
    const auto [end, parseError] = std::from_chars(display.data() + 1, display.data() + display.size(), displayNumber);
    return parseError == std::errc{} && end == display.data() + display.size() && displayNumber <= 65535;
}

std::expected<void, std::string> Realm::writeXWaylandDisplayMetadata(const std::filesystem::path& instanceDirectory, std::string_view display) {
    if (!validXWaylandDisplay(display))
        return std::unexpected(std::format("invalid XWayland display '{}'", display));

    std::error_code error;
    if (!std::filesystem::is_directory(instanceDirectory, error) || error)
        return std::unexpected(std::format("invalid realm instance directory: {}", error.message()));

    const auto path          = instanceDirectory / XWAYLAND_DISPLAY_METADATA_FILE;
    const auto temporaryPath = instanceDirectory / std::format("{}.tmp", XWAYLAND_DISPLAY_METADATA_FILE);
    std::filesystem::remove(temporaryPath, error);
    if (error)
        return std::unexpected(std::format("failed clearing stale {}: {}", temporaryPath.string(), error.message()));

    int fd = open(temporaryPath.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
    if (fd < 0)
        return std::unexpected(std::format("failed creating {}: {}", temporaryPath.string(), strerror(errno)));

    const auto fail = [&](std::string_view operation) -> std::expected<void, std::string> {
        const auto savedErrno = errno;
        close(fd);
        std::error_code cleanupError;
        std::filesystem::remove(temporaryPath, cleanupError);
        return std::unexpected(std::format("failed {} {}: {}", operation, temporaryPath.string(), strerror(savedErrno)));
    };

    const auto payload = std::format("{}\n", display);
    size_t     offset  = 0;
    while (offset < payload.size()) {
        const auto written = write(fd, payload.data() + offset, payload.size() - offset);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return fail("writing");
        }
        if (written == 0) {
            errno = EIO;
            return fail("writing");
        }
        offset += written;
    }

    if (fchmod(fd, S_IRUSR | S_IWUSR) < 0)
        return fail("securing");
    if (close(fd) < 0) {
        fd = -1;
        return fail("closing");
    }
    fd = -1;

    std::filesystem::rename(temporaryPath, path, error);
    if (error) {
        std::error_code cleanupError;
        std::filesystem::remove(temporaryPath, cleanupError);
        return std::unexpected(std::format("failed publishing {}: {}", path.string(), error.message()));
    }

    return {};
}

std::optional<std::string> Realm::readXWaylandDisplayMetadata(const std::filesystem::path& instanceDirectory) {
    const auto      path = instanceDirectory / XWAYLAND_DISPLAY_METADATA_FILE;
    std::error_code error;
    if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path, error)) || std::filesystem::file_size(path, error) > 16 || error)
        return std::nullopt;

    std::ifstream displayFile(path);
    std::string   display;
    std::string   trailingLine;
    if (!std::getline(displayFile, display) || std::getline(displayFile, trailingLine) || !validXWaylandDisplay(display))
        return std::nullopt;

    return display;
}

std::string_view Realm::realmStateName(eRealmState state) {
    switch (state) {
        case eRealmState::CREATING: return "creating";
        case eRealmState::RUNNING: return "running";
        case eRealmState::PAUSED: return "paused";
        case eRealmState::STOPPING: return "stopping";
        case eRealmState::STOPPED: return "stopped";
        case eRealmState::FAILED: return "failed";
    }

    return "unknown";
}

std::string_view Realm::realmInputOwnerName(eRealmInputOwner owner) {
    switch (owner) {
        case eRealmInputOwner::AGENT: return "agent";
        case eRealmInputOwner::HUMAN: return "human";
        case eRealmInputOwner::NONE: return "none";
    }

    return "unknown";
}

std::string_view Realm::realmObservationPermissionName(eRealmObservationPermission permission) {
    switch (permission) {
        case eRealmObservationPermission::DENIED: return "denied";
        case eRealmObservationPermission::ALLOWED: return "allowed";
    }

    return "unknown";
}

std::string_view Realm::realmCapabilityName(eRealmCapability capability) {
    switch (capability) {
        case eRealmCapability::OBSERVE: return "observe";
        case eRealmCapability::POINTER: return "pointer";
        case eRealmCapability::KEYBOARD: return "keyboard";
    }

    return "unknown";
}

std::expected<eRealmCapability, std::string> Realm::realmCapabilityFromName(std::string_view name) {
    if (name == "observe")
        return eRealmCapability::OBSERVE;
    if (name == "pointer")
        return eRealmCapability::POINTER;
    if (name == "keyboard")
        return eRealmCapability::KEYBOARD;
    return std::unexpected(std::format("unknown or unenforced realm capability '{}'", name));
}

bool SRealmCapabilityManifest::allows(eRealmCapability capability) const {
    switch (capability) {
        case eRealmCapability::OBSERVE: return observe;
        case eRealmCapability::POINTER: return pointer;
        case eRealmCapability::KEYBOARD: return keyboard;
    }

    return false;
}

CRealm::CRealm(uint64_t id, std::string name) : m_id(id), m_name(std::move(name)) {
    ;
}

uint64_t CRealm::id() const {
    return m_id;
}

const std::string& CRealm::name() const {
    return m_name;
}

eRealmState CRealm::state() const {
    return m_state;
}

pid_t CRealm::compositorPID() const {
    return m_compositorPID;
}

const std::string& CRealm::runtimeDirectory() const {
    return m_runtimeDirectory;
}

const std::string& CRealm::waylandSocket() const {
    return m_waylandSocket;
}

const std::string& CRealm::configPath() const {
    return m_configPath;
}

const std::string& CRealm::logPath() const {
    return m_logPath;
}

int CRealm::exitCode() const {
    return m_exitCode;
}

eRealmInputOwner CRealm::inputOwner() const {
    return m_inputOwner;
}

eRealmObservationPermission CRealm::observationPermission() const {
    return m_observationPermission;
}

const SRealmCapabilityManifest& CRealm::capabilities() const {
    return m_capabilities;
}

std::expected<void, std::string> CRealm::transitionTo(eRealmState state) {
    if (state == m_state)
        return std::unexpected(std::format("realm '{}' is already {}", m_name, realmStateName(m_state)));

    const bool allowed = (m_state == eRealmState::STOPPED && state == eRealmState::CREATING) ||
        (m_state == eRealmState::CREATING && (state == eRealmState::RUNNING || state == eRealmState::STOPPING || state == eRealmState::FAILED)) ||
        (m_state == eRealmState::RUNNING && (state == eRealmState::PAUSED || state == eRealmState::STOPPING || state == eRealmState::FAILED)) ||
        (m_state == eRealmState::PAUSED && (state == eRealmState::RUNNING || state == eRealmState::STOPPING || state == eRealmState::FAILED)) ||
        (m_state == eRealmState::STOPPING && (state == eRealmState::STOPPED || state == eRealmState::FAILED)) || (m_state == eRealmState::FAILED && state == eRealmState::CREATING);

    if (!allowed)
        return std::unexpected(std::format("cannot transition realm '{}' from {} to {}", m_name, realmStateName(m_state), realmStateName(state)));

    m_state = state;
    return {};
}
