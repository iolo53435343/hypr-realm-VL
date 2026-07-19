#pragma once

#include "RealmManager.hpp"

#include <cstdint>
#include <string>

enum eHyprCtlOutputFormat : uint8_t;
struct SHyprCtlCommand;

namespace Realm {
    std::string realmListRequest(CRealmManager& manager, eHyprCtlOutputFormat format);
    std::string realmCommandRequest(CRealmManager& manager, eHyprCtlOutputFormat format, const std::string& request);
    std::string realmLifecycleEventData(const SRealmLifecycleEvent& event);

    class CRealmIPC {
      public:
        explicit CRealmIPC(CRealmManager& manager);
        ~CRealmIPC();

      private:
        CRealmManager&      m_manager;
        SP<SHyprCtlCommand> m_realmsCommand;
        SP<SHyprCtlCommand> m_realmCommand;
        CHyprSignalListener m_lifecycleListener;
    };

    UP<CRealmIPC>& ipc();
}
