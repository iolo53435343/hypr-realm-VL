#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <sys/types.h>

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

        std::expected<void, std::string> transitionTo(eRealmState state);

      private:
        uint64_t         m_id = 0;
        std::string      m_name;
        eRealmState      m_state         = eRealmState::STOPPED;
        pid_t            m_compositorPID = 0;
        std::string      m_runtimeDirectory;
        std::string      m_waylandSocket;
        std::string      m_configPath;
        std::string      m_logPath;
        int              m_exitCode   = -1;
        eRealmInputOwner m_inputOwner = eRealmInputOwner::NONE;

        friend class CRealmManager;
    };
}
