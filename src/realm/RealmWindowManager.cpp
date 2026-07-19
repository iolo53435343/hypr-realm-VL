#include "RealmWindowManager.hpp"

#include "RealmDecoration.hpp"
#include "../debug/log/Logger.hpp"
#include "../desktop/state/FocusState.hpp"
#include "../desktop/view/Window.hpp"
#include "../event/EventBus.hpp"
#include "../helpers/MiscFunctions.hpp"

#include <format>
#include <map>

using namespace Realm;

struct CRealmWindowManager::SImpl {
    std::map<uint64_t, uint64_t>               windowToRealm;
    std::map<uint64_t, uint64_t>               realmToWindow;
    std::map<uint64_t, PHLWINDOWREF>           windows;
    std::map<uint64_t, IHyprWindowDecoration*> decorations;
    CHyprSignalListener                        windowOpenListener;
    CHyprSignalListener                        windowCloseListener;
    CHyprSignalListener                        windowCloseRequestListener;
    CHyprSignalListener                        lifecycleListener;
    CHyprSignalListener                        inputOwnerListener;
};

CRealmWindowManager::CRealmWindowManager(CRealmManager& manager, SRealmWindowManagerOptions options) : m_manager(manager), m_impl(makeUnique<SImpl>()) {
    if (!options.integrateWithEventBus)
        return;

    m_impl->windowOpenListener = Event::bus()->m_events.window.open.listen([this](PHLWINDOW window) {
        if (!window)
            return;

        // Wayland credentials remain stable when the nested backend changes its
        // host window title or app ID.
        const auto clientPID = window->getPID();
        if (!m_manager.realmByPID(clientPID))
            return;

        const auto associated = associateWindow(window->m_stableID, clientPID);
        if (!associated) {
            Log::logger->log(Log::ERR, "Failed associating realm host window {:x}: {}", window->m_stableID, associated.error());
            return;
        }

        m_impl->windows[window->m_stableID]     = window;
        auto decoration                         = makeUnique<CRealmDecoration>(window, *associated);
        m_impl->decorations[window->m_stableID] = decoration.get();
        window->addWindowDeco(std::move(decoration));
        updateWindowInputOwner(*associated);
    });

    m_impl->windowCloseListener = Event::bus()->m_events.window.close.listen([this](PHLWINDOW window) {
        if (!window)
            return;

        const auto decoration = m_impl->decorations.find(window->m_stableID);
        if (decoration != m_impl->decorations.end()) {
            window->removeWindowDeco(decoration->second);
            m_impl->decorations.erase(decoration);
        }
        dissociateWindow(window->m_stableID);
        m_impl->windows.erase(window->m_stableID);
    });

    m_impl->windowCloseRequestListener = Event::bus()->m_events.window.requestClose.listen([this](PHLWINDOW window) {
        if (!window || !realmForWindow(window->m_stableID))
            return;

        if (const auto stopped = handleCloseRequest(window->m_stableID); !stopped)
            Log::logger->log(Log::ERR, "Failed stopping realm for host window {:x}: {}", window->m_stableID, stopped.error());
        window->updateWindowDecos();
    });

    m_impl->lifecycleListener = m_manager.m_events.lifecycle.listen([this](const SRealmLifecycleEvent& event) {
        if (!event.realm)
            return;

        const auto windowID = windowForRealm(event.realm->id());
        if (!windowID)
            return;

        const auto windowIterator = m_impl->windows.find(*windowID);
        const auto window         = windowIterator == m_impl->windows.end() ? PHLWINDOW{} : windowIterator->second.lock();
        if (window)
            window->updateWindowDecos();

        if (event.type != eRealmLifecycleEvent::DESTROYED)
            return;

        const auto decoration = m_impl->decorations.find(*windowID);
        if (window && decoration != m_impl->decorations.end())
            window->removeWindowDeco(decoration->second);
        m_impl->decorations.erase(*windowID);
        dissociateWindow(*windowID);
        m_impl->windows.erase(*windowID);
    });

    m_impl->inputOwnerListener = m_manager.m_events.inputOwner.listen([this](const SRealmInputOwnerEvent& event) { updateWindowInputOwner(event.realm); });
}

CRealmWindowManager::~CRealmWindowManager() = default;

std::expected<SP<CRealm>, std::string> CRealmWindowManager::associateWindow(uint64_t windowID, pid_t clientPID) {
    if (windowID == 0)
        return std::unexpected("window ID cannot be zero");

    const auto realm = m_manager.realmByPID(clientPID);
    if (!realm)
        return std::unexpected(std::format("process {} is not a realm compositor", clientPID));

    if (const auto existing = m_impl->windowToRealm.find(windowID); existing != m_impl->windowToRealm.end()) {
        if (existing->second == realm->id())
            return realm;
        return std::unexpected(std::format("window {:x} is already associated with realm {}", windowID, existing->second));
    }

    if (const auto existing = m_impl->realmToWindow.find(realm->id()); existing != m_impl->realmToWindow.end())
        return std::unexpected(std::format("realm '{}' already has host window {:x}", realm->name(), existing->second));

    m_impl->windowToRealm.emplace(windowID, realm->id());
    m_impl->realmToWindow.emplace(realm->id(), windowID);
    return realm;
}

void CRealmWindowManager::dissociateWindow(uint64_t windowID) {
    const auto associated = m_impl->windowToRealm.find(windowID);
    if (associated == m_impl->windowToRealm.end())
        return;

    const auto windowIterator = m_impl->windows.find(windowID);
    const auto window         = windowIterator == m_impl->windows.end() ? PHLWINDOW{} : windowIterator->second.lock();
    if (window)
        window->setInputBlocked(Desktop::View::INPUT_BLOCK_REALM_AGENT, false);

    m_impl->realmToWindow.erase(associated->second);
    m_impl->windowToRealm.erase(associated);
}

SP<CRealm> CRealmWindowManager::realmForWindow(uint64_t windowID) const {
    const auto associated = m_impl->windowToRealm.find(windowID);
    return associated == m_impl->windowToRealm.end() ? SP<CRealm>{} : m_manager.realmByID(associated->second);
}

std::optional<uint64_t> CRealmWindowManager::windowForRealm(uint64_t realmID) const {
    const auto associated = m_impl->realmToWindow.find(realmID);
    return associated == m_impl->realmToWindow.end() ? std::nullopt : std::optional<uint64_t>{associated->second};
}

std::expected<void, std::string> CRealmWindowManager::handleCloseRequest(uint64_t windowID) {
    const auto realm = realmForWindow(windowID);
    if (!realm)
        return std::unexpected(std::format("window {:x} is not associated with a realm", windowID));

    if (realm->state() == eRealmState::CREATING || realm->state() == eRealmState::RUNNING || realm->state() == eRealmState::PAUSED)
        return m_manager.stopRealm(realm->id());

    return {};
}

void CRealmWindowManager::updateWindowInputOwner(const SP<CRealm>& realm) {
    if (!realm)
        return;

    const auto windowID = windowForRealm(realm->id());
    if (!windowID)
        return;

    const auto windowIterator = m_impl->windows.find(*windowID);
    const auto window         = windowIterator == m_impl->windows.end() ? PHLWINDOW{} : windowIterator->second.lock();
    if (!window)
        return;

    window->setInputBlocked(Desktop::View::INPUT_BLOCK_REALM_AGENT, realm->inputOwner() != eRealmInputOwner::HUMAN);
    window->updateWindowDecos();
}

std::expected<void, std::string> CRealmWindowManager::takeoverRealm(uint64_t realmID) {
    const auto realm = m_manager.realmByID(realmID);
    if (!realm)
        return std::unexpected(std::format("realm {} does not exist", realmID));
    if (!windowForRealm(realmID))
        return std::unexpected(std::format("realm '{}' has no host window", realm->name()));

    auto result = m_manager.takeoverRealm(realmID);
    if (!result)
        return result;

    const auto windowID       = windowForRealm(realmID);
    const auto windowIterator = windowID ? m_impl->windows.find(*windowID) : m_impl->windows.end();
    const auto window         = windowIterator == m_impl->windows.end() ? PHLWINDOW{} : windowIterator->second.lock();
    if (window)
        Desktop::focusState()->fullWindowFocus(window, Desktop::FOCUS_REASON_KEYBIND);
    return {};
}

std::expected<void, std::string> CRealmWindowManager::releaseRealm(uint64_t realmID) {
    const auto realm = m_manager.realmByID(realmID);
    if (!realm)
        return std::unexpected(std::format("realm {} does not exist", realmID));
    if (!windowForRealm(realmID))
        return std::unexpected(std::format("realm '{}' has no host window", realm->name()));

    const auto windowID       = windowForRealm(realmID);
    const auto windowIterator = windowID ? m_impl->windows.find(*windowID) : m_impl->windows.end();
    const auto window         = windowIterator == m_impl->windows.end() ? PHLWINDOW{} : windowIterator->second.lock();
    if (window)
        window->setInputBlocked(Desktop::View::INPUT_BLOCK_REALM_AGENT, true);

    auto result = m_manager.releaseRealm(realmID);
    if (!result && window && realm->inputOwner() == eRealmInputOwner::HUMAN)
        window->setInputBlocked(Desktop::View::INPUT_BLOCK_REALM_AGENT, false);
    return result;
}

std::string Realm::realmWindowJSON(const SP<CRealm>& realm) {
    if (!realm)
        return "null";
    return std::format(R"({{"id":{},"name":"{}","state":"{}","input_owner":"{}"}})", realm->id(), escapeJSONStrings(realm->name()), realmStateName(realm->state()),
                       realmInputOwnerName(realm->inputOwner()));
}

std::string Realm::realmWindowText(const SP<CRealm>& realm) {
    if (!realm)
        return "none";
    return std::format("{} ({}, {}, input: {})", realm->name(), realm->id(), realmStateName(realm->state()), realmInputOwnerName(realm->inputOwner()));
}

std::string Realm::realmWindowDecorationLabel(const CRealm& realm) {
    return std::format("Realm: {} · {} · input: {}", realm.name(), realmStateName(realm.state()), realmInputOwnerName(realm.inputOwner()));
}

UP<CRealmWindowManager>& Realm::windowManager() {
    static UP<CRealmWindowManager> realmWindowManager;
    return realmWindowManager;
}
