#include "RealmIPC.hpp"

#include "../debug/HyprCtl.hpp"
#include "../helpers/MiscFunctions.hpp"
#include "../managers/EventManager.hpp"

#include <format>
#include <hyprutils/string/String.hpp>

using namespace Hyprutils::String;
using namespace Realm;

static std::string realmJSON(const CRealm& realm) {
    return std::format(R"({{"id":{},"name":"{}","state":"{}","pid":{},"wayland_socket":"{}","runtime_directory":"{}","config_path":"{}","log_path":"{}","exit_code":{}}})",
                       realm.id(), escapeJSONStrings(realm.name()), realmStateName(realm.state()), realm.compositorPID(), escapeJSONStrings(realm.waylandSocket()),
                       escapeJSONStrings(realm.runtimeDirectory()), escapeJSONStrings(realm.configPath()), escapeJSONStrings(realm.logPath()), realm.exitCode());
}

static std::string realmText(const CRealm& realm) {
    return std::format("Realm {} ({}):\n\tstate: {}\n\tpid: {}\n\twayland socket: {}\n\truntime directory: {}\n\tconfig path: {}\n\tlog path: {}\n\texit code: {}\n", realm.name(),
                       realm.id(), realmStateName(realm.state()), realm.compositorPID(), realm.waylandSocket().empty() ? "-" : realm.waylandSocket(),
                       realm.runtimeDirectory().empty() ? "-" : realm.runtimeDirectory(), realm.configPath().empty() ? "-" : realm.configPath(),
                       realm.logPath().empty() ? "-" : realm.logPath(), realm.exitCode());
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

static std::expected<std::pair<std::string, std::string>, std::string> parseRealmCommand(const std::string& request) {
    if (request == "realm")
        return std::unexpected("usage: hyprctl realm <create|start|pause|resume|stop|destroy|info> <name>");
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

    return std::pair<std::string, std::string>{std::move(action), std::move(name)};
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

std::string Realm::realmCommandRequest(CRealmManager& manager, eHyprCtlOutputFormat format, const std::string& request) {
    auto parsed = parseRealmCommand(request);
    if (!parsed)
        return errorResponse(format, parsed.error());

    const auto& [action, name] = *parsed;
    if (action == "create") {
        auto created = manager.createRealm(name);
        if (!created)
            return errorResponse(format, created.error());
        return successResponse(format, "created", *created);
    }

    if (action != "start" && action != "pause" && action != "resume" && action != "stop" && action != "destroy" && action != "info")
        return errorResponse(format, std::format("unknown realm action '{}'", action));

    auto realm = manager.realmByName(name);
    if (!realm)
        return errorResponse(format, std::format("realm '{}' does not exist", name));

    if (action == "info")
        return format == FORMAT_JSON ? realmJSON(*realm) : realmText(*realm);

    std::expected<void, std::string> result;
    std::string_view                 responseAction = "";
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
    return std::format(R"({{"id":{},"name":"{}","state":"{}"}})", event.realm->id(), escapeJSONStrings(event.realm->name()), realmStateName(event.realm->state()));
}

UP<CRealmIPC>& Realm::ipc() {
    static UP<CRealmIPC> realmIPC;
    return realmIPC;
}

CRealmIPC::CRealmIPC(CRealmManager& manager) : m_manager(manager) {
    if (g_pHyprCtl) {
        m_realmsCommand = g_pHyprCtl->registerCommand(SHyprCtlCommand{
            .name  = "realms",
            .exact = true,
            .fn    = [this](eHyprCtlOutputFormat format, std::string) { return realmListRequest(m_manager, format); },
        });
        m_realmCommand  = g_pHyprCtl->registerCommand(SHyprCtlCommand{
             .name  = "realm",
             .exact = false,
             .fn    = [this](eHyprCtlOutputFormat format, std::string request) { return realmCommandRequest(m_manager, format, request); },
        });
    }

    m_lifecycleListener = m_manager.m_events.lifecycle.listen([](const SRealmLifecycleEvent& event) {
        if (!g_pEventManager)
            return;
        g_pEventManager->postEvent(SHyprIPCEvent{.event = std::string(realmLifecycleEventName(event.type)), .data = realmLifecycleEventData(event)});
    });
}

CRealmIPC::~CRealmIPC() {
    m_lifecycleListener.reset();
    if (!g_pHyprCtl)
        return;
    if (m_realmsCommand)
        g_pHyprCtl->unregisterCommand(m_realmsCommand);
    if (m_realmCommand)
        g_pHyprCtl->unregisterCommand(m_realmCommand);
}
