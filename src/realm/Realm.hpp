#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

namespace Realm {
    enum class eRealmState : uint8_t {
        CREATING = 0,
        RUNNING,
        PAUSED,
        STOPPING,
        STOPPED,
        FAILED,
    };

    std::string_view realmStateName(eRealmState state);

    enum class eRealmInputOwner : uint8_t {
        AGENT = 0,
        HUMAN,
        NONE,
    };

    std::string_view realmInputOwnerName(eRealmInputOwner owner);

    enum class eRealmObservationPermission : uint8_t {
        DENIED = 0,
        ALLOWED,
    };

    std::string_view realmObservationPermissionName(eRealmObservationPermission permission);

    enum class eRealmCapability : uint8_t {
        OBSERVE = 0,
        POINTER,
        KEYBOARD,
    };

    std::string_view                             realmCapabilityName(eRealmCapability capability);
    std::expected<eRealmCapability, std::string> realmCapabilityFromName(std::string_view name);

    struct SRealmCapabilityManifest {
        bool                     observe   = false;
        bool                     pointer   = false;
        bool                     keyboard  = false;
        bool                     clipboard = false;
        std::vector<std::string> network;
        std::vector<std::string> filesystemRead;
        std::vector<std::string> filesystemWrite;
        std::vector<std::string> secrets;

        bool                     allows(eRealmCapability capability) const;
    };

    class CRealm {
      public:
        CRealm(uint64_t id, std::string name);

        uint64_t                         id() const;
        const std::string&               name() const;
        eRealmState                      state() const;
        pid_t                            compositorPID() const;
        const std::string&               runtimeDirectory() const;
        const std::string&               waylandSocket() const;
        const std::string&               configPath() const;
        const std::string&               logPath() const;
        int                              exitCode() const;
        eRealmInputOwner                 inputOwner() const;
        eRealmObservationPermission      observationPermission() const;
        const SRealmCapabilityManifest&  capabilities() const;

        std::expected<void, std::string> transitionTo(eRealmState state);

      private:
        uint64_t                    m_id = 0;
        std::string                 m_name;
        eRealmState                 m_state         = eRealmState::STOPPED;
        pid_t                       m_compositorPID = 0;
        std::string                 m_runtimeDirectory;
        std::string                 m_waylandSocket;
        std::string                 m_configPath;
        std::string                 m_logPath;
        int                         m_exitCode              = -1;
        eRealmInputOwner            m_inputOwner            = eRealmInputOwner::NONE;
        eRealmObservationPermission m_observationPermission = eRealmObservationPermission::DENIED;
        SRealmCapabilityManifest    m_capabilities;

        friend class CRealmManager;
    };
}
