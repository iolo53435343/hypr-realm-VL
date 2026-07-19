#pragma once

#include "../helpers/memory/Memory.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace Realm {
    class CRealmManager;
    class CRealmInputControllerManager;
    class CRealmWindowManager;

    inline constexpr size_t REALM_CONTROL_MAX_MESSAGE_SIZE = 64 * 1024;

    struct SRealmControlServerOptions {
        std::filesystem::path socketPath;
        size_t                maxMessageSize  = REALM_CONTROL_MAX_MESSAGE_SIZE;
        size_t                maxQueuedOutput = REALM_CONTROL_MAX_MESSAGE_SIZE * 4;
        size_t                maxClients      = 32;
        std::optional<uid_t>  expectedPeerUID;
        bool                  integrateWithEventLoop = true;
    };

    std::string realmControlRequest(CRealmManager& manager, CRealmWindowManager& windowManager, std::string_view payload);
    std::string realmControlRequest(CRealmManager& manager, CRealmWindowManager& windowManager, CRealmInputControllerManager* inputController, std::string_view payload);
    std::string realmControlFrame(std::string_view payload);
    bool        realmControlPeerAuthorized(uid_t peerUID, uid_t expectedUID);

    class CRealmControlServer {
      public:
        CRealmControlServer(CRealmManager& manager, CRealmWindowManager& windowManager);
        CRealmControlServer(CRealmManager& manager, CRealmWindowManager& windowManager, SRealmControlServerOptions options);
        CRealmControlServer(CRealmManager& manager, CRealmWindowManager& windowManager, CRealmInputControllerManager& inputController, SRealmControlServerOptions options);
        ~CRealmControlServer();

        bool                         isListening() const;
        const std::filesystem::path& socketPath() const;
        const std::string&           lastError() const;
        void                         dispatchPendingEvents();

      private:
        struct SImpl;
        UP<SImpl> m_impl;
    };

    UP<CRealmControlServer>& controlServer();
}
