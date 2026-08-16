#include "RealmIPC.hpp"

#include "RealmWindowManager.hpp"

#include "../debug/HyprCtl.hpp"
#include "../helpers/MiscFunctions.hpp"
#include "../managers/EventManager.hpp"

#include <format>
#include <hyprutils/string/String.hpp>

using namespace Hyprutils::String;
using namespace Realm;

static std::string realmStringArrayJSON(const std::vector<std::string>& values) {
    std::string response = "[";
    for (const auto& value : values) {
        if (response.size() > 1)
            response += ',';
        response += std::format(R"("{}")", escapeJSONStrings(value));
    }
    response += ']';
    return response;
}

static std::string realmCapabilitiesJSON(const SRealmCapabilityManifest& capabilities) {
    return std::format(R"({{"observe":{},"pointer":{},"keyboard":{},"clipboard":{},"network":{},"filesystem_read":{},"filesystem_write":{},"secrets":{}}})", capabilities.observe,
                       capabilities.pointer, capabilities.keyboard, capabilities.clipboard, realmStringArrayJSON(capabilities.network),
                       realmStringArrayJSON(capabilities.filesystemRead), realmStringArrayJSON(capabilities.filesystemWrite), realmStringArrayJSON(capabilities.secrets));
}

std::string Realm::realmJSON(const CRealm& realm) {
    return std::format(
        R"({{"id":{},"name":"{}","state":"{}","input_owner":"{}","observation_permission":"{}","capabilities":{},"pid":{},"wayland_socket":"{}","runtime_directory":"{}","config_path":"{}","log_path":"{}","exit_code":{}}})",
        realm.id(), escapeJSONStrings(realm.name()), realmStateName(realm.state()), realmInputOwnerName(realm.inputOwner()),
        realmObservationPermissionName(realm.observationPermission()), realmCapabilitiesJSON(realm.capabilities()), realm.compositorPID(), escapeJSONStrings(realm.waylandSocket()),
        escapeJSONStrings(realm.runtimeDirectory()), escapeJSONStrings(realm.configPath()), escapeJSONStrings(realm.logPath()), realm.exitCode());
}

static std::string realmText(const CRealm& realm) {
    return std::format("Realm {} ({}):\n\tstate: {}\n\tinput owner: {}\n\tobservation permission: {}\n\tcapabilities: observe={}, pointer={}, keyboard={}\n\tpid: {}\n\twayland "
                       "socket: {}\n\truntime directory: {}\n\tconfig path: {}\n\tlog path: {}\n\texit code: {}\n",
                       realm.name(), realm.id(), realmStateName(realm.state()), realmInputOwnerName(realm.inputOwner()),
                       realmObservationPermissionName(realm.observationPermission()), realm.capabilities().observe ? "granted" : "denied",
                       realm.capabilities().pointer ? "granted" : "denied", realm.capabilities().keyboard ? "granted" : "denied", realm.compositorPID(),
                       realm.waylandSocket().empty() ? "-" : realm.waylandSocket(), realm.runtimeDirectory().empty() ? "-" : realm.runtimeDirectory(),
                       realm.configPath().empty() ? "-" : realm.configPath(), realm.logPath().empty() ? "-" : realm.logPath(), realm.exitCode());
}

static std::string errorResponse(eHyprCtlOutputFormat format, const std::string& error) {
    if (format == FORMAT_JSON)
        return std::format(R"({{"ok":false,"error":"{}"}})", escapeJSONStrings(error));
    return std::format("error: {}", error);
}

static std::string successResponse(eHyprCtlOutputFormat format, std::string_view action, const SP<CRealm>& realm) {
    if (format == FORMAT_JSON)
        return std::format(R"({{"ok":true,"action":"{}","realm":{}}})", action, realmJSON(*realm));
    return std::format("{} realm '{}' ({})", action, realm->name(), realm->id());
}

struct SParsedRealmCommand {
    std::string                action;
    std::string                name;
    std::optional<std::string> capability;
    std::optional<std::string> application;
};

static std::expected<SParsedRealmCommand, std::string> parseRealmCommand(const std::string& request) {
    if (request == "realm")
        return std::unexpected(
            "usage: hyprctl realm <create|start|open|pause|resume|stop|kill|takeover|release|observe|unobserve|grant|revoke|destroy|info> <name> [application|capability]");
    if (!request.starts_with("realm "))
        return std::unexpected("invalid realm request");

    const auto arguments = trim(request.substr(6));
    const auto separator = arguments.find(' ');
    if (separator == std::string::npos)
        return std::unexpected(std::format("realm action '{}' requires a name", arguments));

    auto action = arguments.substr(0, separator);
    auto name   = trim(arguments.substr(separator + 1));
    if (name.empty())
        return std::unexpected(std::format("realm action '{}' requires a name", action));

    std::optional<std::string> capability;
    std::optional<std::string> application;
    if (action == "grant" || action == "revoke") {
        const auto capabilitySeparator = name.rfind(' ');
        if (capabilitySeparator == std::string::npos)
            return std::unexpected(std::format("realm action '{}' requires a name and capability", action));
        capability = trim(name.substr(capabilitySeparator + 1));
        name       = trim(name.substr(0, capabilitySeparator));
        if (name.empty() || capability->empty())
            return std::unexpected(std::format("realm action '{}' requires a name and capability", action));
    } else if (action == "open") {
        const auto applicationSeparator = name.rfind(' ');
        if (applicationSeparator == std::string::npos)
            return std::unexpected("realm action 'open' requires a name and application");
        application = trim(name.substr(applicationSeparator + 1));
        name        = trim(name.substr(0, applicationSeparator));
        if (name.empty() || application->empty())
            return std::unexpected("realm action 'open' requires a name and application");
    }

    return SParsedRealmCommand{
        .action      = std::move(action),
        .name        = std::move(name),
        .capability  = std::move(capability),
        .application = std::move(application),
    };
}

std::string Realm::realmListRequest(CRealmManager& manager, eHyprCtlOutputFormat format) {
    const auto& realms = manager.realms();
    if (format == FORMAT_JSON) {
        std::string response = "[";
        for (const auto& realm : realms) {
            if (response.size() > 1)
                response += ',';
            response += realmJSON(*realm);
        }
        response += ']';
        return response;
    }

    if (realms.empty())
        return "No realms";

    std::string response;
    for (const auto& realm : realms) {
        if (!response.empty())
            response += '\n';
        response += realmText(*realm);
    }
    return response;
}

std::string Realm::realmCommandRequest(CRealmManager& manager, CRealmWindowManager& windowManager, eHyprCtlOutputFormat format, const std::string& request) {
    auto parsed = parseRealmCommand(request);
    if (!parsed)
        return errorResponse(format, parsed.error());

    const auto& action = parsed->action;
    const auto& name   = parsed->name;
    if (action == "create") {
        auto created = manager.createRealm(name);
        if (!created)
            return errorResponse(format, created.error());
        return successResponse(format, "created", *created);
    }

    if (action != "start" && action != "open" && action != "pause" && action != "resume" && action != "stop" && action != "kill" && action != "takeover" && action != "release" &&
        action != "observe" && action != "unobserve" && action != "grant" && action != "revoke" && action != "destroy" && action != "info")
        return errorResponse(format, std::format("unknown realm action '{}'", action));

    std::optional<eRealmCapability> capability;
    if (parsed->capability) {
        auto parsedCapability = realmCapabilityFromName(*parsed->capability);
        if (!parsedCapability)
            return errorResponse(format, parsedCapability.error());
        capability = *parsedCapability;
    }

    auto realm = manager.realmByName(name);
    if (!realm)
        return errorResponse(format, std::format("realm '{}' does not exist", name));

    if (action == "info")
        return format == FORMAT_JSON ? realmJSON(*realm) : realmText(*realm);
    if (action == "open") {
        auto opened = manager.openApplication(realm->id(), *parsed->application);
        if (!opened)
            return errorResponse(format, opened.error());
        if (format == FORMAT_JSON)
            return std::format(R"({{"ok":true,"action":"opened","application":"{}","pid":{},"realm":{}}})", escapeJSONStrings(*parsed->application), *opened, realmJSON(*realm));
        return std::format("opened application '{}' (PID {}) in realm '{}' ({})", *parsed->application, *opened, realm->name(), realm->id());
    }

    std::expected<void, std::string> result;
    std::string                      responseAction;
    if (action == "start") {
        result         = manager.startRealm(realm->id());
        responseAction = "starting";
    } else if (action == "pause") {
        result         = manager.pauseRealm(realm->id());
        responseAction = "paused";
    } else if (action == "resume") {
        result         = manager.resumeRealm(realm->id());
        responseAction = "resumed";
    } else if (action == "stop") {
        result         = manager.stopRealm(realm->id());
        responseAction = "stopping";
    } else if (action == "kill") {
        result         = manager.killRealm(realm->id());
        responseAction = "killed";
    } else if (action == "takeover") {
        result         = windowManager.takeoverRealm(realm->id());
        responseAction = "taken over";
    } else if (action == "release") {
        result         = windowManager.releaseRealm(realm->id());
        responseAction = "released";
    } else if (action == "observe") {
        result         = manager.allowObservation(realm->id());
        responseAction = "observation allowed for";
    } else if (action == "unobserve") {
        result         = manager.denyObservation(realm->id());
        responseAction = "observation denied for";
    } else if (action == "grant") {
        result         = manager.grantCapability(realm->id(), *capability);
        responseAction = std::format("granted {} capability to", realmCapabilityName(*capability));
    } else if (action == "revoke") {
        result         = manager.revokeCapability(realm->id(), *capability);
        responseAction = std::format("revoked {} capability from", realmCapabilityName(*capability));
    } else if (action == "destroy") {
        result         = manager.destroyRealm(realm->id());
        responseAction = "destroyed";
    }

    if (!result)
        return errorResponse(format, result.error());
    return successResponse(format, responseAction, realm);
}

std::string Realm::realmLifecycleEventData(const SRealmLifecycleEvent& event) {
    if (!event.realm)
        return "{}";
    return std::format(R"({{"id":{},"name":"{}","state":"{}","input_owner":"{}","observation_permission":"{}","capabilities":{}}})", event.realm->id(),
                       escapeJSONStrings(event.realm->name()), realmStateName(event.realm->state()), realmInputOwnerName(event.realm->inputOwner()),
                       realmObservationPermissionName(event.realm->observationPermission()), realmCapabilitiesJSON(event.realm->capabilities()));
}

UP<CRealmIPC>& Realm::ipc() {
    static UP<CRealmIPC> realmIPC;
    return realmIPC;
}

CRealmIPC::CRealmIPC(CRealmManager& manager, CRealmWindowManager& windowManager) : m_manager(manager), m_windowManager(windowManager) {
    if (g_pHyprCtl) {
        m_realmsCommand = g_pHyprCtl->registerCommand(SHyprCtlCommand{
            .name  = "realms",
            .exact = true,
            .fn    = [this](eHyprCtlOutputFormat format, std::string) { return realmListRequest(m_manager, format); },
        });
        m_realmCommand  = g_pHyprCtl->registerCommand(SHyprCtlCommand{
            .name  = "realm",
            .exact = false,
            .fn    = [this](eHyprCtlOutputFormat format, std::string request) { return realmCommandRequest(m_manager, m_windowManager, format, request); },
        });
    }

    m_lifecycleListener   = m_manager.m_events.lifecycle.listen([](const SRealmLifecycleEvent& event) {
        if (!g_pEventManager)
            return;
        g_pEventManager->postEvent(SHyprIPCEvent{.event = std::string(realmLifecycleEventName(event.type)), .data = realmLifecycleEventData(event)});
    });
    m_observationListener = m_manager.m_events.observationPermission.listen([](const SRealmObservationPermissionEvent& event) {
        if (!g_pEventManager || !event.realm)
            return;
        g_pEventManager->postEvent(SHyprIPCEvent{
            .event = event.permission == eRealmObservationPermission::ALLOWED ? "realmobservationallowed" : "realmobservationdenied",
            .data  = std::format(R"({{"id":{},"name":"{}","observation_permission":"{}"}})", event.realm->id(), escapeJSONStrings(event.realm->name()),
                                 realmObservationPermissionName(event.permission)),
        });
    });
    m_capabilityListener  = m_manager.m_events.capability.listen([](const SRealmCapabilityEvent& event) {
        if (!g_pEventManager || !event.realm)
            return;
        g_pEventManager->postEvent(SHyprIPCEvent{
            .event = event.granted ? "realmcapabilitygranted" : "realmcapabilityrevoked",
            .data  = std::format(R"({{"id":{},"name":"{}","capability":"{}","granted":{}}})", event.realm->id(), escapeJSONStrings(event.realm->name()),
                                 realmCapabilityName(event.capability), event.granted),
        });
    });
}

CRealmIPC::~CRealmIPC() {
    m_capabilityListener.reset();
    m_observationListener.reset();
    m_lifecycleListener.reset();
    if (!g_pHyprCtl)
        return;
    if (m_realmsCommand)
        g_pHyprCtl->unregisterCommand(m_realmsCommand);
    if (m_realmCommand)
        g_pHyprCtl->unregisterCommand(m_realmCommand);
}
