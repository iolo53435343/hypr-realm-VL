#include <realm/RealmWindowManager.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <thread>
#include <unistd.h>

using namespace Realm;

static bool waitForRealmWindowState(CRealmManager& manager, const SP<CRealm>& realm, eRealmState state) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        manager.dispatchPendingEvents();
        if (realm->state() == state)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

class CRealmWindowManagerTest : public testing::Test {
  protected:
    void SetUp() override {
        m_root = std::filesystem::temp_directory_path() / std::format("hrwm.{}", getpid());
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
        if (!created)
            return {};
        if (!m_manager->startRealm((*created)->id()))
            return {};
        if (!waitForRealmWindowState(*m_manager, *created, eRealmState::RUNNING))
            return {};
        return *created;
    }

    std::filesystem::path   m_root;
    UP<CRealmManager>       m_manager;
    UP<CRealmWindowManager> m_windowManager;
};

TEST_F(CRealmWindowManagerTest, associatesHostWindowByCompositorPID) {
    const auto realm = startRealm("codex");
    ASSERT_TRUE(realm);
    ASSERT_GT(realm->compositorPID(), 1);
    EXPECT_EQ(m_manager->realmByPID(realm->compositorPID()), realm);

    constexpr uint64_t WINDOW_ID  = 0xCAFE;
    const auto         associated = m_windowManager->associateWindow(WINDOW_ID, realm->compositorPID());
    ASSERT_TRUE(associated);
    EXPECT_EQ(*associated, realm);
    EXPECT_EQ(m_windowManager->realmForWindow(WINDOW_ID), realm);
    EXPECT_EQ(m_windowManager->windowForRealm(realm->id()), WINDOW_ID);

    m_windowManager->dissociateWindow(WINDOW_ID);
    EXPECT_FALSE(m_windowManager->realmForWindow(WINDOW_ID));
    EXPECT_FALSE(m_windowManager->windowForRealm(realm->id()));
}

TEST_F(CRealmWindowManagerTest, rejectsUnknownProcessesAndConflictingAssociations) {
    const auto first  = startRealm("first");
    const auto second = startRealm("second");
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);

    EXPECT_FALSE(m_windowManager->associateWindow(1, 1));
    ASSERT_TRUE(m_windowManager->associateWindow(1, first->compositorPID()));
    EXPECT_TRUE(m_windowManager->associateWindow(1, first->compositorPID()));
    EXPECT_FALSE(m_windowManager->associateWindow(1, second->compositorPID()));
    EXPECT_FALSE(m_windowManager->associateWindow(2, first->compositorPID()));
}

TEST_F(CRealmWindowManagerTest, explicitHostWindowCloseStopsRealmCleanly) {
    const auto realm = startRealm("close-policy");
    ASSERT_TRUE(realm);
    ASSERT_TRUE(m_windowManager->associateWindow(42, realm->compositorPID()));

    ASSERT_TRUE(m_windowManager->handleCloseRequest(42));
    EXPECT_EQ(realm->state(), eRealmState::STOPPING);
    ASSERT_TRUE(waitForRealmWindowState(*m_manager, realm, eRealmState::STOPPED));
    EXPECT_EQ(realm->exitCode(), 0);

    EXPECT_TRUE(m_windowManager->handleCloseRequest(42));
    m_windowManager->dissociateWindow(42);
    EXPECT_FALSE(m_windowManager->handleCloseRequest(42));
}

TEST_F(CRealmWindowManagerTest, takeoverRequiresHostWindowAndReleaseRestoresAgent) {
    const auto realm = startRealm("lease");
    ASSERT_TRUE(realm);
    EXPECT_EQ(realm->inputOwner(), eRealmInputOwner::AGENT);
    EXPECT_FALSE(m_windowManager->takeoverRealm(realm->id()));

    ASSERT_TRUE(m_windowManager->associateWindow(42, realm->compositorPID()));
    ASSERT_TRUE(m_windowManager->takeoverRealm(realm->id()));
    EXPECT_EQ(realm->inputOwner(), eRealmInputOwner::HUMAN);
    EXPECT_FALSE(m_windowManager->takeoverRealm(realm->id()));

    ASSERT_TRUE(m_windowManager->releaseRealm(realm->id()));
    EXPECT_EQ(realm->inputOwner(), eRealmInputOwner::AGENT);
    EXPECT_FALSE(m_windowManager->releaseRealm(realm->id()));

    ASSERT_TRUE(m_manager->grantCapability(realm->id(), eRealmCapability::POINTER));
    EXPECT_TRUE(realmWindowDecorationLabel(*realm).contains("capabilities: pointer"));
}

TEST(RealmWindowFormatting, includesEscapedInspectionMetadataAndReadableLabel) {
    const auto realm = makeShared<CRealm>(7, R"(codex "work")");

    EXPECT_EQ(realmWindowJSON({}), "null");
    EXPECT_EQ(realmWindowText({}), "none");
    EXPECT_EQ(realmWindowJSON(realm),
              R"({"id":7,"name":"codex \"work\"","state":"stopped","input_owner":"none","capabilities":{"observe":false,"pointer":false,"keyboard":false}})");
    EXPECT_EQ(realmWindowText(realm), R"(codex "work" (7, stopped, input: none, capabilities: none))");
    EXPECT_EQ(realmWindowDecorationLabel(*realm), R"(Realm: codex "work" · stopped · input: none · capabilities: none)");
}
