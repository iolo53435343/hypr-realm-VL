#pragma once

#include "Realm.hpp"
#include "../helpers/memory/Memory.hpp"
#include "../helpers/signal/Signal.hpp"

#include <chrono>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <hyprutils/os/FileDescriptor.hpp>

class CEventLoopTimer;

namespace Realm {
    enum class eRealmLifecycleEvent : uint8_t {
        CREATED = 0,
        STARTED,
        PAUSED,
        RESUMED,
        STOPPED,
        FAILED,
        DESTROYED,
    };

    std::string_view realmLifecycleEventName(eRealmLifecycleEvent event);

    struct SRealmLifecycleEvent {
        eRealmLifecycleEvent type = eRealmLifecycleEvent::CREATED;
        SP<CRealm>           realm;
    };

    struct SRealmManagerOptions {
        std::filesystem::path     runtimeRoot;
        std::filesystem::path     compositorBinary;
        std::string               hostWaylandSocket;
        std::chrono::milliseconds startupTimeout         = std::chrono::seconds(10);
        std::chrono::milliseconds stopTimeout            = std::chrono::seconds(5);
        bool                      integrateWithEventLoop = true;
    };

    class CRealmManager {
      public:
        CRealmManager();
        explicit CRealmManager(SRealmManagerOptions options);
        ~CRealmManager();

        std::expected<SP<CRealm>, std::string> createRealm(const std::string& name);
        std::expected<void, std::string>       startRealm(uint64_t id);
        std::expected<void, std::string>       pauseRealm(uint64_t id);
        std::expected<void, std::string>       resumeRealm(uint64_t id);
        std::expected<void, std::string>       stopRealm(uint64_t id);
        std::expected<void, std::string>       destroyRealm(uint64_t id);

        SP<CRealm>                             realmByID(uint64_t id) const;
        SP<CRealm>                             realmByName(const std::string& name) const;
        SP<CRealm>                             realmByPID(pid_t pid) const;
        const std::vector<SP<CRealm>>&         realms() const;

        void                                   dispatchPendingEvents();
        void                                   shutdownAll();

        struct {
            CSignalT<SRealmLifecycleEvent> lifecycle;
        } m_events;

      private:
        struct SRealmProcess {
            pid_t                                                supervisorPID   = 0;
            pid_t                                                processGroupPID = 0;
            Hyprutils::OS::CFileDescriptor                       statusFD;
            std::vector<char>                                    statusBuffer;
            std::chrono::steady_clock::time_point                startupDeadline;
            std::optional<std::chrono::steady_clock::time_point> terminationDeadline;
            bool                                                 exitReceived  = false;
            bool                                                 forceKillSent = false;
            bool                                                 failOnExit    = false;
        };

        struct SLaunchResult {
            pid_t                          supervisorPID = 0;
            pid_t                          compositorPID = 0;
            int                            execError     = 0;
            Hyprutils::OS::CFileDescriptor statusFD;
        };

        static SRealmManagerOptions               defaultOptions();

        std::expected<void, std::string>          validateName(const std::string& name) const;
        std::expected<void, std::string>          prepareRuntime(CRealm& realm);
        std::expected<SLaunchResult, std::string> launchRealmProcess(const CRealm& realm) const;
        std::expected<void, std::string>          cleanupRuntime(CRealm& realm);
        void                                      updateReadiness(CRealm& realm, SRealmProcess& process);
        void                                      handleProcessExit(CRealm& realm, SRealmProcess& process, int exitCode);
        void                                      finishProcessCleanup(uint64_t id, SRealmProcess& process);
        bool                                      signalProcessGroup(const SRealmProcess& process, int signal) const;
        void                                      setupPollTimer();
        void                                      emitLifecycleEvent(eRealmLifecycleEvent event, const SP<CRealm>& realm);

        SRealmManagerOptions                      m_options;
        std::vector<SP<CRealm>>                   m_realms;
        std::map<uint64_t, SRealmProcess>         m_processes;
        std::set<std::filesystem::path>           m_ownedRuntimeDirectories;
        SP<CEventLoopTimer>                       m_pollTimer;
        uint64_t                                  m_nextID       = 1;
        bool                                      m_shuttingDown = false;
    };

    UP<CRealmManager>& manager();
}
