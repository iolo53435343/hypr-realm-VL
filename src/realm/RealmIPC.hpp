#pragma once

#include "RealmManager.hpp"

#include <cstdint>
#include <string>

enum eHyprCtlOutputFormat : uint8_t;
struct SHyprCtlCommand;

namespace Realm {
    class CRealmWindowManager;

    std::string realmJSON(const CRealm& realm);
    std::string realmListRequest(CRealmManager& manager, eHyprCtlOutputFormat format);
    std::string realmCommandRequest(CRealmManager& manager, CRealmWindowManager& windowManager, eHyprCtlOutputFormat format, const std::string& request);
    std::string realmLifecycleEventData(const SRealmLifecycleEvent& event);

    class CRealmIPC {
      public:
        CRealmIPC(CRealmManager& manager, CRealmWindowManager& windowManager);
        ~CRealmIPC();

      private:
        CRealmManager&       m_manager;
        CRealmWindowManager& m_windowManager;
        SP<SHyprCtlCommand>  m_realmsCommand;
        SP<SHyprCtlCommand>  m_realmCommand;
        CHyprSignalListener  m_lifecycleListener;
        CHyprSignalListener  m_observationListener;
    };

    UP<CRealmIPC>& ipc();
}
