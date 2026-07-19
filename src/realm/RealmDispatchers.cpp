#include "RealmDispatchers.hpp"

#include "RealmManager.hpp"
#include "RealmWindowManager.hpp"

#include <format>

#include <hyprutils/string/String.hpp>

using namespace Hyprutils::String;
using namespace Realm;

static SDispatchResult missingName(std::string_view dispatcher) {
    return {.success = false, .error = std::format("{} requires a realm name", dispatcher)};
}

static SDispatchResult missingRealm(std::string_view name) {
    return {.success = false, .error = std::format("realm '{}' does not exist", name)};
}

SDispatchResult Realm::runRealmTakeoverDispatcher(CRealmManager& manager, CRealmWindowManager& windowManager, const std::string& arguments) {
    const auto name = trim(arguments);
    if (name.empty())
        return missingName("realmtakeover");

    const auto realm = manager.realmByName(name);
    if (!realm)
        return missingRealm(name);

    const auto result = windowManager.takeoverRealm(realm->id());
    return result ? SDispatchResult{} : SDispatchResult{.success = false, .error = result.error()};
}

SDispatchResult Realm::runRealmReleaseDispatcher(CRealmManager& manager, CRealmWindowManager& windowManager, const std::string& arguments) {
    const auto name = trim(arguments);
    if (name.empty())
        return missingName("realmrelease");

    const auto realm = manager.realmByName(name);
    if (!realm)
        return missingRealm(name);

    const auto result = windowManager.releaseRealm(realm->id());
    return result ? SDispatchResult{} : SDispatchResult{.success = false, .error = result.error()};
}

SDispatchResult Realm::runRealmPauseDispatcher(CRealmManager& manager, const std::string& arguments) {
    const auto name = trim(arguments);
    if (name.empty()) {
        const auto result = manager.pauseAllRealms();
        return result ? SDispatchResult{} : SDispatchResult{.success = false, .error = result.error()};
    }

    const auto realm = manager.realmByName(name);
    if (!realm)
        return missingRealm(name);

    const auto result = manager.pauseRealm(realm->id());
    return result ? SDispatchResult{} : SDispatchResult{.success = false, .error = result.error()};
}

SDispatchResult Realm::runRealmKillDispatcher(CRealmManager& manager, const std::string& arguments) {
    const auto name = trim(arguments);
    if (name.empty())
        return missingName("realmkill");

    const auto realm = manager.realmByName(name);
    if (!realm)
        return missingRealm(name);

    const auto result = manager.killRealm(realm->id());
    return result ? SDispatchResult{} : SDispatchResult{.success = false, .error = result.error()};
}

SDispatchResult Realm::realmTakeoverDispatcher(std::string arguments) {
    if (!manager() || !windowManager())
        return {.success = false, .error = "realm control is unavailable"};
    return runRealmTakeoverDispatcher(*manager(), *windowManager(), arguments);
}

SDispatchResult Realm::realmReleaseDispatcher(std::string arguments) {
    if (!manager() || !windowManager())
        return {.success = false, .error = "realm control is unavailable"};
    return runRealmReleaseDispatcher(*manager(), *windowManager(), arguments);
}

SDispatchResult Realm::realmPauseDispatcher(std::string arguments) {
    if (!manager())
        return {.success = false, .error = "realm control is unavailable"};
    return runRealmPauseDispatcher(*manager(), arguments);
}

SDispatchResult Realm::realmKillDispatcher(std::string arguments) {
    if (!manager())
        return {.success = false, .error = "realm control is unavailable"};
    return runRealmKillDispatcher(*manager(), arguments);
}
