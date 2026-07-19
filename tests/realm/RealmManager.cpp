#include <realm/RealmManager.hpp>

#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace Realm;

TEST(RealmLifecycleEvent, namesAreStable) {
    EXPECT_EQ(realmLifecycleEventName(eRealmLifecycleEvent::CREATED), "realmcreated");
    EXPECT_EQ(realmLifecycleEventName(eRealmLifecycleEvent::STARTED), "realmstarted");
    EXPECT_EQ(realmLifecycleEventName(eRealmLifecycleEvent::PAUSED), "realmpaused");
    EXPECT_EQ(realmLifecycleEventName(eRealmLifecycleEvent::RESUMED), "realmresumed");
    EXPECT_EQ(realmLifecycleEventName(eRealmLifecycleEvent::STOPPED), "realmstopped");
    EXPECT_EQ(realmLifecycleEventName(eRealmLifecycleEvent::FAILED), "realmfailed");
    EXPECT_EQ(realmLifecycleEventName(eRealmLifecycleEvent::DESTROYED), "realmdestroyed");
    EXPECT_EQ(realmLifecycleEventName(eRealmLifecycleEvent::TAKEN_OVER), "realmtakeover");
    EXPECT_EQ(realmLifecycleEventName(eRealmLifecycleEvent::RELEASED), "realmrelease");
}

static bool waitForState(CRealmManager& manager, const SP<CRealm>& realm, eRealmState state, std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        manager.dispatchPendingEvents();
        if (realm->state() == state)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    manager.dispatchPendingEvents();
    return realm->state() == state;
}

static bool destroyEventually(CRealmManager& manager, uint64_t id) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        manager.dispatchPendingEvents();
        if (manager.destroyRealm(id))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

static bool startEventually(CRealmManager& manager, uint64_t id) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        manager.dispatchPendingEvents();
        if (manager.startRealm(id))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

class CRealmManagerTest : public testing::Test {
  protected:
    void SetUp() override {
        m_root = std::filesystem::temp_directory_path() / std::format("hrt.{}", getpid());
        std::filesystem::remove_all(m_root);
        std::filesystem::create_directory(m_root);
        std::filesystem::permissions(m_root, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);

        SRealmManagerOptions options{
            .runtimeRoot            = m_root,
            .compositorBinary       = REALM_PROCESS_HELPER_PATH,
            .hostWaylandSocket      = "/tmp/unused-test-wayland-socket",
            .startupTimeout         = std::chrono::seconds(1),
            .stopTimeout            = std::chrono::milliseconds(200),
            .integrateWithEventLoop = false,
        };
        m_manager = makeUnique<CRealmManager>(std::move(options));
    }

    void TearDown() override {
        m_manager.reset();
        std::filesystem::remove_all(m_root);
    }

    std::filesystem::path m_root;
    UP<CRealmManager>     m_manager;
};

TEST_F(CRealmManagerTest, createsNamedRealmsAndRejectsDuplicates) {
    auto codex    = m_manager->createRealm("codex");
    auto research = m_manager->createRealm("research");

    ASSERT_TRUE(codex);
    ASSERT_TRUE(research);
    EXPECT_NE((*codex)->id(), (*research)->id());
    EXPECT_EQ(m_manager->realmByID((*codex)->id()), *codex);
    EXPECT_EQ(m_manager->realmByName("research"), *research);
    EXPECT_EQ(m_manager->realms().size(), 2);

    const auto duplicate = m_manager->createRealm("codex");
    ASSERT_FALSE(duplicate);
    EXPECT_NE(duplicate.error().find("already exists"), std::string::npos);
}

TEST_F(CRealmManagerTest, validatesNames) {
    EXPECT_FALSE(m_manager->createRealm(""));
    EXPECT_FALSE(m_manager->createRealm(std::string("bad\nname")));
    EXPECT_FALSE(m_manager->createRealm(std::string(129, 'a')));
}

TEST_F(CRealmManagerTest, startsPausesResumesStopsAndDestroysRealm) {
    std::vector<eRealmLifecycleEvent> events;
    auto                              lifecycleListener = m_manager->m_events.lifecycle.listen([&events](const SRealmLifecycleEvent& event) { events.emplace_back(event.type); });

    auto                              created = m_manager->createRealm("lifecycle");
    ASSERT_TRUE(created);
    const auto realm = *created;

    ASSERT_TRUE(m_manager->startRealm(realm->id()));
    EXPECT_EQ(realm->state(), eRealmState::CREATING);
    const auto lockPath = std::filesystem::path(realm->runtimeDirectory()) / "hypr/test-instance/hyprland.lock";
    for (size_t attempt = 0; attempt < 50 && !std::filesystem::exists(lockPath); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(std::filesystem::is_regular_file(lockPath));

    std::ifstream lock(lockPath);
    pid_t         lockPID = 0;
    std::string   lockSocket;
    lock >> lockPID >> lockSocket;
    EXPECT_EQ(lockPID, realm->compositorPID());
    EXPECT_EQ(lockSocket, "realm-test.sock");
    EXPECT_TRUE(std::filesystem::is_socket(std::filesystem::path(realm->runtimeDirectory()) / lockSocket));

    ASSERT_TRUE(waitForState(*m_manager, realm, eRealmState::RUNNING));
    EXPECT_GT(realm->compositorPID(), 1);
    EXPECT_EQ(realm->waylandSocket(), "realm-test.sock");
    EXPECT_TRUE(std::filesystem::is_directory(realm->runtimeDirectory()));
    EXPECT_TRUE(std::filesystem::is_regular_file(realm->configPath()));
    EXPECT_EQ(std::filesystem::path(realm->runtimeDirectory()).parent_path(), m_root);

    const auto runtimePermissions = std::filesystem::status(realm->runtimeDirectory()).permissions();
    const auto privateMask        = std::filesystem::perms::group_all | std::filesystem::perms::others_all;
    EXPECT_EQ(runtimePermissions & privateMask, std::filesystem::perms::none);
    EXPECT_EQ(std::filesystem::status(realm->configPath()).permissions() & privateMask, std::filesystem::perms::none);

    EXPECT_TRUE(m_manager->pauseRealm(realm->id()));
    EXPECT_EQ(realm->state(), eRealmState::PAUSED);
    EXPECT_TRUE(m_manager->resumeRealm(realm->id()));
    EXPECT_EQ(realm->state(), eRealmState::RUNNING);
    EXPECT_TRUE(m_manager->stopRealm(realm->id()));
    EXPECT_EQ(realm->state(), eRealmState::STOPPING);
    ASSERT_TRUE(waitForState(*m_manager, realm, eRealmState::STOPPED));
    EXPECT_EQ(realm->exitCode(), 0);
    EXPECT_EQ(realm->compositorPID(), 0);

    const auto firstRuntime = realm->runtimeDirectory();
    ASSERT_TRUE(startEventually(*m_manager, realm->id()));
    ASSERT_TRUE(waitForState(*m_manager, realm, eRealmState::RUNNING));
    EXPECT_NE(realm->runtimeDirectory(), firstRuntime);
    EXPECT_FALSE(std::filesystem::exists(firstRuntime));
    ASSERT_TRUE(m_manager->stopRealm(realm->id()));
    ASSERT_TRUE(waitForState(*m_manager, realm, eRealmState::STOPPED));

    const auto runtime = realm->runtimeDirectory();
    ASSERT_TRUE(destroyEventually(*m_manager, realm->id()));
    EXPECT_FALSE(std::filesystem::exists(runtime));
    EXPECT_FALSE(m_manager->realmByID(realm->id()));
    EXPECT_EQ(events,
              (std::vector<eRealmLifecycleEvent>{
                  eRealmLifecycleEvent::CREATED,
                  eRealmLifecycleEvent::STARTED,
                  eRealmLifecycleEvent::PAUSED,
                  eRealmLifecycleEvent::RESUMED,
                  eRealmLifecycleEvent::STOPPED,
                  eRealmLifecycleEvent::STARTED,
                  eRealmLifecycleEvent::STOPPED,
                  eRealmLifecycleEvent::DESTROYED,
              }));
}

TEST_F(CRealmManagerTest, maintainsExclusiveInputOwnershipAcrossLifecycle) {
    std::vector<eRealmInputOwner> owners;
    auto                          inputOwnerListener = m_manager->m_events.inputOwner.listen([&owners](const SRealmInputOwnerEvent& event) { owners.emplace_back(event.owner); });

    auto                          created = m_manager->createRealm("ownership");
    ASSERT_TRUE(created);
    const auto realm = *created;
    EXPECT_EQ(realm->inputOwner(), eRealmInputOwner::NONE);

    ASSERT_TRUE(m_manager->startRealm(realm->id()));
    ASSERT_TRUE(waitForState(*m_manager, realm, eRealmState::RUNNING));
    EXPECT_EQ(realm->inputOwner(), eRealmInputOwner::AGENT);

    ASSERT_TRUE(m_manager->takeoverRealm(realm->id()));
    EXPECT_EQ(realm->inputOwner(), eRealmInputOwner::HUMAN);
    EXPECT_FALSE(m_manager->takeoverRealm(realm->id()));

    ASSERT_TRUE(m_manager->releaseRealm(realm->id()));
    EXPECT_EQ(realm->inputOwner(), eRealmInputOwner::AGENT);
    EXPECT_FALSE(m_manager->releaseRealm(realm->id()));

    ASSERT_TRUE(m_manager->pauseRealm(realm->id()));
    EXPECT_EQ(realm->inputOwner(), eRealmInputOwner::NONE);
    EXPECT_FALSE(m_manager->takeoverRealm(realm->id()));

    ASSERT_TRUE(m_manager->resumeRealm(realm->id()));
    EXPECT_EQ(realm->inputOwner(), eRealmInputOwner::AGENT);
    ASSERT_TRUE(m_manager->takeoverRealm(realm->id()));
    ASSERT_TRUE(m_manager->stopRealm(realm->id()));
    EXPECT_EQ(realm->inputOwner(), eRealmInputOwner::NONE);
    ASSERT_TRUE(waitForState(*m_manager, realm, eRealmState::STOPPED));

    EXPECT_EQ(owners,
              (std::vector<eRealmInputOwner>{
                  eRealmInputOwner::AGENT,
                  eRealmInputOwner::HUMAN,
                  eRealmInputOwner::AGENT,
                  eRealmInputOwner::NONE,
                  eRealmInputOwner::AGENT,
                  eRealmInputOwner::HUMAN,
                  eRealmInputOwner::NONE,
              }));
}

TEST_F(CRealmManagerTest, observationPermissionIsIndependentAndRevokedOnStop) {
    std::vector<eRealmObservationPermission> permissions;
    auto                                     observationListener =
        m_manager->m_events.observationPermission.listen([&permissions](const SRealmObservationPermissionEvent& event) { permissions.emplace_back(event.permission); });

    auto created = m_manager->createRealm("observation");
    ASSERT_TRUE(created);
    const auto realm = *created;
    EXPECT_EQ(realm->observationPermission(), eRealmObservationPermission::DENIED);

    ASSERT_TRUE(m_manager->startRealm(realm->id()));
    ASSERT_TRUE(waitForState(*m_manager, realm, eRealmState::RUNNING));
    ASSERT_TRUE(m_manager->allowObservation(realm->id()));
    EXPECT_EQ(realm->observationPermission(), eRealmObservationPermission::ALLOWED);

    ASSERT_TRUE(m_manager->takeoverRealm(realm->id()));
    EXPECT_EQ(realm->inputOwner(), eRealmInputOwner::HUMAN);
    EXPECT_EQ(realm->observationPermission(), eRealmObservationPermission::ALLOWED);

    ASSERT_TRUE(m_manager->pauseRealm(realm->id()));
    EXPECT_EQ(realm->inputOwner(), eRealmInputOwner::NONE);
    EXPECT_EQ(realm->observationPermission(), eRealmObservationPermission::ALLOWED);
    ASSERT_TRUE(m_manager->resumeRealm(realm->id()));
    ASSERT_TRUE(m_manager->stopRealm(realm->id()));
    EXPECT_EQ(realm->observationPermission(), eRealmObservationPermission::DENIED);
    ASSERT_TRUE(waitForState(*m_manager, realm, eRealmState::STOPPED));
    EXPECT_EQ(permissions, (std::vector<eRealmObservationPermission>{eRealmObservationPermission::ALLOWED, eRealmObservationPermission::DENIED}));
}

TEST_F(CRealmManagerTest, emergencyPauseStopsAutomationInEveryRunningRealm) {
    auto firstCreated  = m_manager->createRealm("emergency-first");
    auto secondCreated = m_manager->createRealm("emergency-second");
    ASSERT_TRUE(firstCreated);
    ASSERT_TRUE(secondCreated);
    const auto first  = *firstCreated;
    const auto second = *secondCreated;

    ASSERT_TRUE(m_manager->startRealm(first->id()));
    ASSERT_TRUE(m_manager->startRealm(second->id()));
    ASSERT_TRUE(waitForState(*m_manager, first, eRealmState::RUNNING));
    ASSERT_TRUE(waitForState(*m_manager, second, eRealmState::RUNNING));
    ASSERT_TRUE(m_manager->takeoverRealm(first->id()));

    const auto paused = m_manager->pauseAllRealms();
    ASSERT_TRUE(paused);
    EXPECT_EQ(*paused, 2);
    EXPECT_EQ(first->state(), eRealmState::PAUSED);
    EXPECT_EQ(second->state(), eRealmState::PAUSED);
    EXPECT_EQ(first->inputOwner(), eRealmInputOwner::NONE);
    EXPECT_EQ(second->inputOwner(), eRealmInputOwner::NONE);

    const auto repeated = m_manager->pauseAllRealms();
    ASSERT_TRUE(repeated);
    EXPECT_EQ(*repeated, 0);
}

TEST_F(CRealmManagerTest, forceKillUsesSupervisedCleanup) {
    auto created = m_manager->createRealm("force-kill");
    ASSERT_TRUE(created);
    const auto realm = *created;

    ASSERT_TRUE(m_manager->startRealm(realm->id()));
    ASSERT_TRUE(waitForState(*m_manager, realm, eRealmState::RUNNING));
    ASSERT_TRUE(m_manager->killRealm(realm->id()));
    EXPECT_EQ(realm->state(), eRealmState::STOPPING);
    EXPECT_EQ(realm->inputOwner(), eRealmInputOwner::NONE);
    ASSERT_TRUE(waitForState(*m_manager, realm, eRealmState::STOPPED));
    EXPECT_EQ(realm->exitCode(), 128 + SIGKILL);
}

TEST_F(CRealmManagerTest, treatsNamesAsDataNotCommands) {
    const auto marker  = m_root / "command-marker";
    auto       created = m_manager->createRealm(std::format("literal; touch {}", marker.string()));
    ASSERT_TRUE(created);
    const auto realm = *created;

    ASSERT_TRUE(m_manager->startRealm(realm->id()));
    ASSERT_TRUE(waitForState(*m_manager, realm, eRealmState::RUNNING));
    ASSERT_TRUE(m_manager->stopRealm(realm->id()));
    ASSERT_TRUE(waitForState(*m_manager, realm, eRealmState::STOPPED));
    EXPECT_FALSE(std::filesystem::exists(marker));
    EXPECT_TRUE(destroyEventually(*m_manager, realm->id()));
}

TEST_F(CRealmManagerTest, reportsStartupFailure) {
    auto created = m_manager->createRealm("startup-failure");
    ASSERT_TRUE(created);
    const auto realm = *created;

    ASSERT_TRUE(m_manager->startRealm(realm->id()));
    ASSERT_TRUE(waitForState(*m_manager, realm, eRealmState::FAILED));
    EXPECT_EQ(realm->exitCode(), 23);
    EXPECT_EQ(realm->compositorPID(), 0);
    EXPECT_TRUE(destroyEventually(*m_manager, realm->id()));
}

TEST_F(CRealmManagerTest, timesOutCompositorThatNeverBecomesReady) {
    auto created = m_manager->createRealm("no-ready");
    ASSERT_TRUE(created);
    const auto realm = *created;

    ASSERT_TRUE(m_manager->startRealm(realm->id()));
    ASSERT_TRUE(waitForState(*m_manager, realm, eRealmState::FAILED));
    EXPECT_EQ(realm->exitCode(), -1);
    EXPECT_TRUE(destroyEventually(*m_manager, realm->id()));
    EXPECT_EQ(realm->exitCode(), -1);
}

TEST_F(CRealmManagerTest, movesCrashedRealmToFailed) {
    std::vector<eRealmLifecycleEvent> events;
    auto                              lifecycleListener = m_manager->m_events.lifecycle.listen([&events](const SRealmLifecycleEvent& event) { events.emplace_back(event.type); });

    auto                              created = m_manager->createRealm("crash");
    ASSERT_TRUE(created);
    const auto realm = *created;

    ASSERT_TRUE(m_manager->startRealm(realm->id()));
    ASSERT_TRUE(waitForState(*m_manager, realm, eRealmState::RUNNING));
    ASSERT_TRUE(waitForState(*m_manager, realm, eRealmState::FAILED));
    EXPECT_EQ(realm->exitCode(), 128 + SIGABRT);
    EXPECT_EQ(realm->compositorPID(), 0);
    EXPECT_EQ(events, (std::vector<eRealmLifecycleEvent>{eRealmLifecycleEvent::CREATED, eRealmLifecycleEvent::STARTED, eRealmLifecycleEvent::FAILED}));
    EXPECT_TRUE(destroyEventually(*m_manager, realm->id()));
}

TEST_F(CRealmManagerTest, runsMultipleRealmsIndependently) {
    auto firstCreated  = m_manager->createRealm("first");
    auto secondCreated = m_manager->createRealm("second");
    ASSERT_TRUE(firstCreated);
    ASSERT_TRUE(secondCreated);
    const auto first  = *firstCreated;
    const auto second = *secondCreated;

    ASSERT_TRUE(m_manager->startRealm(first->id()));
    ASSERT_TRUE(m_manager->startRealm(second->id()));
    ASSERT_TRUE(waitForState(*m_manager, first, eRealmState::RUNNING));
    ASSERT_TRUE(waitForState(*m_manager, second, eRealmState::RUNNING));
    EXPECT_NE(first->compositorPID(), second->compositorPID());

    ASSERT_TRUE(m_manager->stopRealm(first->id()));
    ASSERT_TRUE(waitForState(*m_manager, first, eRealmState::STOPPED));
    EXPECT_EQ(second->state(), eRealmState::RUNNING);
    ASSERT_TRUE(m_manager->stopRealm(second->id()));
    ASSERT_TRUE(waitForState(*m_manager, second, eRealmState::STOPPED));
}

TEST_F(CRealmManagerTest, shutdownKillsUncooperativeRealmAndCleansRuntime) {
    auto created = m_manager->createRealm("ignore-term");
    ASSERT_TRUE(created);
    const auto realm = *created;

    ASSERT_TRUE(m_manager->startRealm(realm->id()));
    ASSERT_TRUE(waitForState(*m_manager, realm, eRealmState::RUNNING));
    const auto pid     = realm->compositorPID();
    const auto runtime = realm->runtimeDirectory();

    m_manager.reset();

    EXPECT_EQ(kill(pid, 0), -1);
    EXPECT_EQ(errno, ESRCH);
    EXPECT_FALSE(std::filesystem::exists(runtime));
}

TEST_F(CRealmManagerTest, reapsSupervisorAfterRealmExit) {
    auto created = m_manager->createRealm("reaped");
    ASSERT_TRUE(created);
    const auto realm = *created;

    ASSERT_TRUE(m_manager->startRealm(realm->id()));
    ASSERT_TRUE(waitForState(*m_manager, realm, eRealmState::RUNNING));
    ASSERT_TRUE(m_manager->stopRealm(realm->id()));
    ASSERT_TRUE(waitForState(*m_manager, realm, eRealmState::STOPPED));
    ASSERT_TRUE(destroyEventually(*m_manager, realm->id()));

    errno = 0;
    EXPECT_EQ(waitpid(-1, nullptr, WNOHANG), -1);
    EXPECT_EQ(errno, ECHILD);
}
