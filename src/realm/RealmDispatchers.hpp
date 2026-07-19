#pragma once

#include "../SharedDefs.hpp"

#include <string>
#include <string_view>

namespace Realm {
    class CRealmManager;
    class CRealmWindowManager;

    SDispatchResult runRealmTakeoverDispatcher(CRealmManager& manager, CRealmWindowManager& windowManager, const std::string& arguments);
    SDispatchResult runRealmReleaseDispatcher(CRealmManager& manager, CRealmWindowManager& windowManager, const std::string& arguments);
    SDispatchResult runRealmPauseDispatcher(CRealmManager& manager, const std::string& arguments);
    SDispatchResult runRealmKillDispatcher(CRealmManager& manager, const std::string& arguments);

    SDispatchResult realmTakeoverDispatcher(std::string arguments);
    SDispatchResult realmReleaseDispatcher(std::string arguments);
    SDispatchResult realmPauseDispatcher(std::string arguments);
    SDispatchResult realmKillDispatcher(std::string arguments);
}
