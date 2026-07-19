#include "Realm.hpp"

#include <format>
#include <utility>

using namespace Realm;

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
