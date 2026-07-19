#include <realm/RealmIPC.hpp>
#include <SharedDefs.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <string>
#include <thread>
#include <unistd.h>

using namespace Realm;

static bool waitForIPCState(CRealmManager& manager, const SP<CRealm>& realm, eRealmState state) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        manager.dispatchPendingEvents();
        if (realm->state() == state)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

static bool destroyThroughIPC(CRealmManager& manager, const std::string& name) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        manager.dispatchPendingEvents();
        const auto response = realmCommandRequest(manager, FORMAT_JSON, std::format("realm destroy {}", name));
        if (response.contains(R"("ok":true)"))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

class CRealmIPCTest : public testing::Test {
  protected:
    void SetUp() override {
        m_root = std::filesystem::temp_directory_path() / std::format("hripc.{}", getpid());
        std::filesystem::remove_all(m_root);
        std::filesystem::create_directory(m_root);
        std::filesystem::permissions(m_root, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);

        m_manager = makeUnique<CRealmManager>(SRealmManagerOptions{
            .runtimeRoot            = m_root,
            .compositorBinary       = REALM_PROCESS_HELPER_PATH,
            .hostWaylandSocket      = "/tmp/unused-test-wayland-socket",
            .startupTimeout         = std::chrono::seconds(1),
            .stopTimeout            = std::chrono::milliseconds(200),
            .integrateWithEventLoop = false,
        });
    }

    void TearDown() override {
        m_manager.reset();
        std::filesystem::remove_all(m_root);
    }

    std::filesystem::path m_root;
    UP<CRealmManager>     m_manager;
};

TEST_F(CRealmIPCTest, listsNoRealmsInTextAndJSON) {
    EXPECT_EQ(realmListRequest(*m_manager, FORMAT_NORMAL), "No realms");
    EXPECT_EQ(realmListRequest(*m_manager, FORMAT_JSON), "[]");
}

TEST_F(CRealmIPCTest, createsListsAndInspectsRealm) {
    const auto created = realmCommandRequest(*m_manager, FORMAT_JSON, R"(realm create codex "primary")");
    EXPECT_TRUE(created.contains(R"("ok":true)"));
    EXPECT_TRUE(created.contains(R"("action":"created")"));
    EXPECT_TRUE(created.contains(R"("name":"codex \"primary\"")"));
    EXPECT_TRUE(created.contains(R"("state":"stopped")"));

    const auto listed = realmListRequest(*m_manager, FORMAT_JSON);
    EXPECT_TRUE(listed.starts_with('['));
    EXPECT_TRUE(listed.contains(R"("id":1)"));
    EXPECT_TRUE(listed.contains(R"("wayland_socket":"")"));
    EXPECT_TRUE(listed.contains(R"("exit_code":-1)"));

    const auto info = realmCommandRequest(*m_manager, FORMAT_NORMAL, R"(realm info codex "primary")");
    EXPECT_TRUE(info.contains("Realm codex \"primary\" (1):"));
    EXPECT_TRUE(info.contains("state: stopped"));
}

TEST_F(CRealmIPCTest, returnsStructuredUsefulErrors) {
    EXPECT_TRUE(realmCommandRequest(*m_manager, FORMAT_NORMAL, "realm").starts_with("error: usage:"));
    EXPECT_EQ(realmCommandRequest(*m_manager, FORMAT_JSON, "realm start missing"), R"({"ok":false,"error":"realm 'missing' does not exist"})");
    EXPECT_EQ(realmCommandRequest(*m_manager, FORMAT_JSON, "realm dance missing"), R"({"ok":false,"error":"unknown realm action 'dance'"})");

    EXPECT_TRUE(realmCommandRequest(*m_manager, FORMAT_NORMAL, "realm create duplicate").starts_with("created realm"));
    const auto invalidTransition = realmCommandRequest(*m_manager, FORMAT_JSON, "realm pause duplicate");
    EXPECT_TRUE(invalidTransition.contains(R"("ok":false)"));
    EXPECT_TRUE(invalidTransition.contains("cannot be paused while stopped"));

    const auto duplicate = realmCommandRequest(*m_manager, FORMAT_JSON, "realm create duplicate");
    EXPECT_TRUE(duplicate.contains(R"("ok":false)"));
    EXPECT_TRUE(duplicate.contains("already exists"));

    const auto unknown = realmCommandRequest(*m_manager, FORMAT_JSON, "realm dance duplicate");
    EXPECT_TRUE(unknown.contains(R"("ok":false)"));
    EXPECT_TRUE(unknown.contains("unknown realm action"));
}

TEST_F(CRealmIPCTest, controlsFullLifecycle) {
    EXPECT_TRUE(realmCommandRequest(*m_manager, FORMAT_NORMAL, "realm create lifecycle").starts_with("created realm"));
    const auto realm = m_manager->realmByName("lifecycle");
    ASSERT_TRUE(realm);

    const auto started = realmCommandRequest(*m_manager, FORMAT_JSON, "realm start lifecycle");
    EXPECT_TRUE(started.contains(R"("ok":true)"));
    EXPECT_TRUE(started.contains(R"("action":"starting")"));
    EXPECT_TRUE(started.contains(R"("state":"creating")"));
    ASSERT_TRUE(waitForIPCState(*m_manager, realm, eRealmState::RUNNING));

    EXPECT_TRUE(realmCommandRequest(*m_manager, FORMAT_NORMAL, "realm pause lifecycle").starts_with("paused realm"));
    EXPECT_EQ(realm->state(), eRealmState::PAUSED);
    EXPECT_TRUE(realmCommandRequest(*m_manager, FORMAT_NORMAL, "realm resume lifecycle").starts_with("resumed realm"));
    EXPECT_EQ(realm->state(), eRealmState::RUNNING);
    EXPECT_TRUE(realmCommandRequest(*m_manager, FORMAT_NORMAL, "realm stop lifecycle").starts_with("stopping realm"));
    ASSERT_TRUE(waitForIPCState(*m_manager, realm, eRealmState::STOPPED));

    ASSERT_TRUE(destroyThroughIPC(*m_manager, "lifecycle"));
    EXPECT_FALSE(m_manager->realmByName("lifecycle"));
}

TEST_F(CRealmIPCTest, formatsLifecycleEventDataAsEscapedJSON) {
    auto created = m_manager->createRealm(R"(quoted"realm)");
    ASSERT_TRUE(created);

    EXPECT_EQ(realmLifecycleEventData(SRealmLifecycleEvent{.type = eRealmLifecycleEvent::CREATED, .realm = *created}), R"({"id":1,"name":"quoted\"realm","state":"stopped"})");
}
