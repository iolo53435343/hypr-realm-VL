#pragma once

#include "RealmManager.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

namespace Realm {
    struct SRealmWindowManagerOptions {
        bool integrateWithEventBus = true;
    };

    class CRealmWindowManager {
      public:
        explicit CRealmWindowManager(CRealmManager& manager, SRealmWindowManagerOptions options = {});
        ~CRealmWindowManager();

        std::expected<SP<CRealm>, std::string> associateWindow(uint64_t windowID, pid_t clientPID);
        void                                   dissociateWindow(uint64_t windowID);
        SP<CRealm>                             realmForWindow(uint64_t windowID) const;
        std::optional<uint64_t>                windowForRealm(uint64_t realmID) const;
        std::expected<void, std::string>       handleCloseRequest(uint64_t windowID);

      private:
        struct SImpl;

        CRealmManager& m_manager;
        UP<SImpl>      m_impl;
    };

    std::string              realmWindowJSON(const SP<CRealm>& realm);
    std::string              realmWindowText(const SP<CRealm>& realm);
    std::string              realmWindowDecorationLabel(const CRealm& realm);
    UP<CRealmWindowManager>& windowManager();
}
