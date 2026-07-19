#include <realm/RealmDispatchers.hpp>

#include <realm/RealmManager.hpp>
#include <realm/RealmWindowManager.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <filesystem>
#include <format>
#include <thread>
#include <unistd.h>

using namespace Realm;

static bool waitForDispatcherState(CRealmManager& manager, const SP<CRealm>& realm, eRealmState state) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        manager.dispatchPendingEvents();
        if (realm->state() == state)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

class CRealmDispatchersTest : public testing::Test {
  protected:
    void SetUp() override {
        m_root = std::filesystem::temp_directory_path() / std::format("hrd.{}", getpid());
        std::filesystem::remove_all(m_root);
        std::filesystem::create_directory(m_root);
        std::filesystem::permissions(m_root, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);

        m_manager       = makeUnique<CRealmManager>(SRealmManagerOptions{
                  .runtimeRoot            = m_root,
                  .compositorBinary       = REALM_PROCESS_HELPER_PATH,
                  .hostWaylandSocket      = "/tmp/unused-test-wayland-socket",
                  .startupTimeout         = std::chrono::seconds(1),
                  .stopTimeout            = std::chrono::milliseconds(200),
                  .integrateWithEventLoop = false,
        });
        m_windowManager = makeUnique<CRealmWindowManager>(*m_manager, SRealmWindowManagerOptions{.integrateWithEventBus = false});
    }

    void TearDown() override {
        m_windowManager.reset();
        m_manager.reset();
        std::filesystem::remove_all(m_root);
    }

    SP<CRealm> startRealm(const std::string& name) {
        auto created = m_manager->createRealm(name);
        if (!created || !m_manager->startRealm((*created)->id()) || !waitForDispatcherState(*m_manager, *created, eRealmState::RUNNING))
            return {};
        return *created;
    }

    std::filesystem::path   m_root;
    UP<CRealmManager>       m_manager;
    UP<CRealmWindowManager> m_windowManager;
};

TEST_F(CRealmDispatchersTest, controlsLeasesAndSupportsGlobalEmergencyPause) {
    const auto first  = startRealm("first");
    const auto second = startRealm("second");
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);

    EXPECT_FALSE(runRealmTakeoverDispatcher(*m_manager, *m_windowManager, "first").success);
    ASSERT_TRUE(m_windowManager->associateWindow(42, first->compositorPID()));
    EXPECT_TRUE(runRealmTakeoverDispatcher(*m_manager, *m_windowManager, " first ").success);
    EXPECT_EQ(first->inputOwner(), eRealmInputOwner::HUMAN);
    EXPECT_TRUE(runRealmReleaseDispatcher(*m_manager, *m_windowManager, "first").success);
    EXPECT_EQ(first->inputOwner(), eRealmInputOwner::AGENT);

    EXPECT_TRUE(runRealmPauseDispatcher(*m_manager, "first").success);
    EXPECT_EQ(first->state(), eRealmState::PAUSED);
    ASSERT_TRUE(m_manager->resumeRealm(first->id()));

    EXPECT_TRUE(runRealmPauseDispatcher(*m_manager, "").success);
    EXPECT_EQ(first->state(), eRealmState::PAUSED);
    EXPECT_EQ(second->state(), eRealmState::PAUSED);
    EXPECT_EQ(first->inputOwner(), eRealmInputOwner::NONE);
    EXPECT_EQ(second->inputOwner(), eRealmInputOwner::NONE);
}

TEST_F(CRealmDispatchersTest, forceKillReportsErrorsAndCleansUp) {
    EXPECT_FALSE(runRealmKillDispatcher(*m_manager, "").success);
    EXPECT_FALSE(runRealmKillDispatcher(*m_manager, "missing").success);

    const auto realm = startRealm("kill-target");
    ASSERT_TRUE(realm);
    EXPECT_TRUE(runRealmKillDispatcher(*m_manager, "kill-target").success);
    ASSERT_TRUE(waitForDispatcherState(*m_manager, realm, eRealmState::STOPPED));
    EXPECT_EQ(realm->exitCode(), 128 + SIGKILL);
}
