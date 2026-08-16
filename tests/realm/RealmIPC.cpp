#include <realm/RealmIPC.hpp>
#include <realm/RealmWindowManager.hpp>
#include <SharedDefs.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <unistd.h>

using namespace Realm;

static std::string environmentValue(const char* name) {
    const auto* value = getenv(name);
    return value ? value : "<unset>";
}

class CScopedEnvironmentVariable {
  public:
    CScopedEnvironmentVariable(std::string name, const std::string& value) : m_name(std::move(name)) {
        if (const auto* previous = getenv(m_name.c_str()); previous)
            m_previous = previous;
        setenv(m_name.c_str(), value.c_str(), 1);
    }

    ~CScopedEnvironmentVariable() {
        if (m_previous)
            setenv(m_name.c_str(), m_previous->c_str(), 1);
        else
            unsetenv(m_name.c_str());
    }

  private:
    std::string                m_name;
    std::optional<std::string> m_previous;
};

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

static bool destroyThroughIPC(CRealmManager& manager, CRealmWindowManager& windowManager, const std::string& name) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        manager.dispatchPendingEvents();
        const auto response = realmCommandRequest(manager, windowManager, FORMAT_JSON, std::format("realm destroy {}", name));
        if (response.contains(R"("ok":true)"))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

static std::map<std::string, std::string> waitForApplicationState(CRealmManager& manager, const std::filesystem::path& path) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        manager.dispatchPendingEvents();
        std::ifstream state(path);
        if (state) {
            std::map<std::string, std::string> values;
            for (std::string line; std::getline(state, line);) {
                const auto separator = line.find('=');
                if (separator != std::string::npos)
                    values.emplace(line.substr(0, separator), line.substr(separator + 1));
            }
            if (values.contains("WAYLAND_SOCKET"))
                return values;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return {};
}

static bool waitForProcessExit(CRealmManager& manager, pid_t pid) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        manager.dispatchPendingEvents();
        if (kill(pid, 0) < 0 && errno == ESRCH)
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

    std::filesystem::path   m_root;
    UP<CRealmManager>       m_manager;
    UP<CRealmWindowManager> m_windowManager;
};

TEST_F(CRealmIPCTest, listsNoRealmsInTextAndJSON) {
    EXPECT_EQ(realmListRequest(*m_manager, FORMAT_NORMAL), "No realms");
    EXPECT_EQ(realmListRequest(*m_manager, FORMAT_JSON), "[]");
}

TEST_F(CRealmIPCTest, createsListsAndInspectsRealm) {
    const auto created = realmCommandRequest(*m_manager, *m_windowManager, FORMAT_JSON, R"(realm create codex "primary")");
    EXPECT_TRUE(created.contains(R"("ok":true)"));
    EXPECT_TRUE(created.contains(R"("action":"created")"));
    EXPECT_TRUE(created.contains(R"("name":"codex \"primary\"")"));
    EXPECT_TRUE(created.contains(R"("state":"stopped")"));

    const auto listed = realmListRequest(*m_manager, FORMAT_JSON);
    EXPECT_TRUE(listed.starts_with('['));
    EXPECT_TRUE(listed.contains(R"("id":1)"));
    EXPECT_TRUE(listed.contains(R"("wayland_socket":"")"));
    EXPECT_TRUE(listed.contains(R"("exit_code":-1)"));
    EXPECT_TRUE(listed.contains(
        R"("capabilities":{"observe":false,"pointer":false,"keyboard":false,"clipboard":false,"network":[],"filesystem_read":[],"filesystem_write":[],"secrets":[]})"));

    const auto info = realmCommandRequest(*m_manager, *m_windowManager, FORMAT_NORMAL, R"(realm info codex "primary")");
    EXPECT_TRUE(info.contains("Realm codex \"primary\" (1):"));
    EXPECT_TRUE(info.contains("state: stopped"));
    EXPECT_TRUE(info.contains("capabilities: observe=denied, pointer=denied, keyboard=denied"));
}

TEST_F(CRealmIPCTest, returnsStructuredUsefulErrors) {
    EXPECT_TRUE(realmCommandRequest(*m_manager, *m_windowManager, FORMAT_NORMAL, "realm").starts_with("error: usage:"));
    EXPECT_EQ(realmCommandRequest(*m_manager, *m_windowManager, FORMAT_JSON, "realm start missing"), R"({"ok":false,"error":"realm 'missing' does not exist"})");
    EXPECT_EQ(realmCommandRequest(*m_manager, *m_windowManager, FORMAT_JSON, "realm dance missing"), R"({"ok":false,"error":"unknown realm action 'dance'"})");

    EXPECT_TRUE(realmCommandRequest(*m_manager, *m_windowManager, FORMAT_NORMAL, "realm create duplicate").starts_with("created realm"));
    const auto invalidTransition = realmCommandRequest(*m_manager, *m_windowManager, FORMAT_JSON, "realm pause duplicate");
    EXPECT_TRUE(invalidTransition.contains(R"("ok":false)"));
    EXPECT_TRUE(invalidTransition.contains("cannot be paused while stopped"));

    const auto duplicate = realmCommandRequest(*m_manager, *m_windowManager, FORMAT_JSON, "realm create duplicate");
    EXPECT_TRUE(duplicate.contains(R"("ok":false)"));
    EXPECT_TRUE(duplicate.contains("already exists"));

    const auto unknown = realmCommandRequest(*m_manager, *m_windowManager, FORMAT_JSON, "realm dance duplicate");
    EXPECT_TRUE(unknown.contains(R"("ok":false)"));
    EXPECT_TRUE(unknown.contains("unknown realm action"));
    EXPECT_TRUE(realmCommandRequest(*m_manager, *m_windowManager, FORMAT_NORMAL, "realm open duplicate").contains("requires a name and application"));
    EXPECT_TRUE(realmCommandRequest(*m_manager, *m_windowManager, FORMAT_NORMAL, "realm open duplicate brave").contains("cannot open applications while stopped"));
}

TEST_F(CRealmIPCTest, opensAnApplicationInsideTheRealmProcessAndEnvironment) {
    const auto inheritedHome                 = environmentValue("HOME");
    const auto inheritedCacheHome            = environmentValue("XDG_CACHE_HOME");
    const auto inheritedConfigHome           = environmentValue("XDG_CONFIG_HOME");
    const auto inheritedDataHome             = environmentValue("XDG_DATA_HOME");
    const auto inheritedStateHome            = environmentValue("XDG_STATE_HOME");
    const auto inheritedTemporaryDirectory   = environmentValue("TMPDIR");
    const auto inheritedNixOSOzonePreference = environmentValue("NIXOS_OZONE_WL");
    const auto inheritedMozillaPreference    = environmentValue("MOZ_ENABLE_WAYLAND");
    const auto inheritedElectronPreference   = environmentValue("ELECTRON_OZONE_PLATFORM_HINT");

    ASSERT_TRUE(m_manager->createRealm("application realm"));
    const auto realm = m_manager->realmByName("application realm");
    ASSERT_TRUE(realm);
    ASSERT_TRUE(m_manager->startRealm(realm->id()));
    ASSERT_TRUE(waitForIPCState(*m_manager, realm, eRealmState::RUNNING));

    const auto application = std::filesystem::path(realm->runtimeDirectory()) / "bin/realm-application-helper";
    std::filesystem::create_symlink(REALM_APPLICATION_HELPER_PATH, application);
    auto applicationPath = application.parent_path().string();
    if (const auto inheritedPath = environmentValue("PATH"); inheritedPath != "<unset>")
        applicationPath += std::format(":{}", inheritedPath);
    CScopedEnvironmentVariable scopedPath{"PATH", applicationPath};
    CScopedEnvironmentVariable scopedXAuthority{"XAUTHORITY", "/tmp/host-xauthority"};
    CScopedEnvironmentVariable scopedSwaySocket{"SWAYSOCK", "/tmp/host-sway.sock"};
    CScopedEnvironmentVariable scopedWaylandSocket{"WAYLAND_SOCKET", "999"};

    const auto                 invalid = realmCommandRequest(*m_manager, *m_windowManager, FORMAT_JSON, "realm open application realm brave;touch");
    EXPECT_TRUE(invalid.contains(R"("ok":false)"));
    EXPECT_TRUE(invalid.contains("without paths, whitespace, or shell syntax"));

    const auto missing = realmCommandRequest(*m_manager, *m_windowManager, FORMAT_JSON, "realm open application realm missing-realm-application");
    EXPECT_TRUE(missing.contains(R"("ok":false)"));
    EXPECT_TRUE(missing.contains("failed executing application"));

    const auto opened = realmCommandRequest(*m_manager, *m_windowManager, FORMAT_JSON, "realm open application realm realm-application-helper");
    EXPECT_TRUE(opened.contains(R"("ok":true)"));
    EXPECT_TRUE(opened.contains(R"("action":"opened")"));
    EXPECT_TRUE(opened.contains(R"("application":"realm-application-helper")"));

    const auto runtime = std::filesystem::path(realm->runtimeDirectory());
    const auto state   = waitForApplicationState(*m_manager, runtime / "realm-application-helper.state");
    ASSERT_FALSE(state.empty());
    const auto applicationPID = sc<pid_t>(std::stoi(state.at("pid")));
    EXPECT_EQ(sc<pid_t>(std::stoi(state.at("pgid"))), realm->compositorPID());
    EXPECT_EQ(getpgid(applicationPID), realm->compositorPID());
    EXPECT_EQ(state.at("HOME"), inheritedHome);
    EXPECT_EQ(state.at("XDG_RUNTIME_DIR"), realm->runtimeDirectory());
    EXPECT_EQ(state.at("XDG_CACHE_HOME"), inheritedCacheHome);
    EXPECT_EQ(state.at("XDG_CONFIG_HOME"), inheritedConfigHome);
    EXPECT_EQ(state.at("XDG_DATA_HOME"), inheritedDataHome);
    EXPECT_EQ(state.at("XDG_STATE_HOME"), inheritedStateHome);
    EXPECT_EQ(state.at("TMPDIR"), inheritedTemporaryDirectory);
    EXPECT_EQ(state.at("PATH"), applicationPath);
    EXPECT_EQ(state.at("NIXOS_OZONE_WL"), inheritedNixOSOzonePreference);
    EXPECT_EQ(state.at("MOZ_ENABLE_WAYLAND"), inheritedMozillaPreference);
    EXPECT_EQ(state.at("ELECTRON_OZONE_PLATFORM_HINT"), inheritedElectronPreference);
    EXPECT_EQ(state.at("WAYLAND_DISPLAY"), realm->waylandSocket());
    EXPECT_EQ(state.at("HYPRLAND_INSTANCE_SIGNATURE"), "test-instance");
    EXPECT_EQ(state.at("HYPRLAND_REALM_ID"), std::to_string(realm->id()));
    EXPECT_EQ(state.at("HYPRLAND_REALM_NAME"), realm->name());
    EXPECT_EQ(state.at("DISPLAY"), ":77");
    const auto authorityPath = runtime / "Xauthority";
    EXPECT_EQ(state.at("XAUTHORITY"), authorityPath.string());
    ASSERT_TRUE(std::filesystem::is_regular_file(authorityPath));
    EXPECT_EQ(std::filesystem::file_size(authorityPath), 0);
    EXPECT_EQ(std::filesystem::status(authorityPath).permissions(), std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    EXPECT_EQ(state.at("SWAYSOCK"), "<unset>");
    EXPECT_EQ(state.at("WAYLAND_SOCKET"), "<unset>");

    EXPECT_TRUE(realmCommandRequest(*m_manager, *m_windowManager, FORMAT_NORMAL, "realm stop application realm").starts_with("stopping realm"));
    ASSERT_TRUE(waitForIPCState(*m_manager, realm, eRealmState::STOPPED));
    EXPECT_TRUE(waitForProcessExit(*m_manager, applicationPID));
    ASSERT_TRUE(destroyThroughIPC(*m_manager, *m_windowManager, "application realm"));
}

TEST_F(CRealmIPCTest, revalidatesXWaylandDisplayForEveryApplicationLaunch) {
    ASSERT_TRUE(m_manager->createRealm("xwayland restart"));
    const auto realm = m_manager->realmByName("xwayland restart");
    ASSERT_TRUE(realm);
    ASSERT_TRUE(m_manager->startRealm(realm->id()));
    ASSERT_TRUE(waitForIPCState(*m_manager, realm, eRealmState::RUNNING));

    const auto application = std::filesystem::path(realm->runtimeDirectory()) / "bin/realm-application-helper";
    std::filesystem::create_symlink(REALM_APPLICATION_HELPER_PATH, application);
    auto applicationPath = application.parent_path().string();
    if (const auto inheritedPath = environmentValue("PATH"); inheritedPath != "<unset>")
        applicationPath += std::format(":{}", inheritedPath);
    CScopedEnvironmentVariable scopedPath{"PATH", applicationPath};

    const auto                 instanceDirectory = std::filesystem::path(realm->runtimeDirectory()) / "hypr/test-instance";
    ASSERT_TRUE(std::filesystem::remove(instanceDirectory / XWAYLAND_DISPLAY_METADATA_FILE));

    const auto unavailable = m_manager->openApplication(realm->id(), "realm-application-helper");
    ASSERT_FALSE(unavailable);
    EXPECT_TRUE(unavailable.error().contains("XWayland display is unavailable"));

    ASSERT_TRUE(writeXWaylandDisplayMetadata(instanceDirectory, ":88"));
    ASSERT_TRUE(m_manager->openApplication(realm->id(), "realm-application-helper"));

    const auto state = waitForApplicationState(*m_manager, std::filesystem::path(realm->runtimeDirectory()) / "realm-application-helper.state");
    ASSERT_FALSE(state.empty());
    EXPECT_EQ(state.at("DISPLAY"), ":88");
}

TEST_F(CRealmIPCTest, controlsFullLifecycle) {
    EXPECT_TRUE(realmCommandRequest(*m_manager, *m_windowManager, FORMAT_NORMAL, "realm create lifecycle").starts_with("created realm"));
    const auto realm = m_manager->realmByName("lifecycle");
    ASSERT_TRUE(realm);

    const auto started = realmCommandRequest(*m_manager, *m_windowManager, FORMAT_JSON, "realm start lifecycle");
    EXPECT_TRUE(started.contains(R"("ok":true)"));
    EXPECT_TRUE(started.contains(R"("action":"starting")"));
    EXPECT_TRUE(started.contains(R"("state":"creating")"));
    ASSERT_TRUE(waitForIPCState(*m_manager, realm, eRealmState::RUNNING));
    EXPECT_EQ(realm->inputOwner(), eRealmInputOwner::AGENT);
    EXPECT_TRUE(realmCommandRequest(*m_manager, *m_windowManager, FORMAT_NORMAL, "realm grant lifecycle observe").starts_with("granted observe capability to realm"));
    EXPECT_TRUE(realmCommandRequest(*m_manager, *m_windowManager, FORMAT_NORMAL, "realm observe lifecycle").starts_with("observation allowed for realm"));
    EXPECT_EQ(realm->observationPermission(), eRealmObservationPermission::ALLOWED);
    EXPECT_TRUE(realmCommandRequest(*m_manager, *m_windowManager, FORMAT_NORMAL, "realm unobserve lifecycle").starts_with("observation denied for realm"));
    EXPECT_EQ(realm->observationPermission(), eRealmObservationPermission::DENIED);

    const auto noWindow = realmCommandRequest(*m_manager, *m_windowManager, FORMAT_JSON, "realm takeover lifecycle");
    EXPECT_TRUE(noWindow.contains("has no host window"));
    ASSERT_TRUE(m_windowManager->associateWindow(42, realm->compositorPID()));
    EXPECT_TRUE(realmCommandRequest(*m_manager, *m_windowManager, FORMAT_NORMAL, "realm takeover lifecycle").starts_with("taken over realm"));
    EXPECT_EQ(realm->inputOwner(), eRealmInputOwner::HUMAN);
    EXPECT_TRUE(realmCommandRequest(*m_manager, *m_windowManager, FORMAT_NORMAL, "realm release lifecycle").starts_with("released realm"));
    EXPECT_EQ(realm->inputOwner(), eRealmInputOwner::AGENT);

    EXPECT_TRUE(realmCommandRequest(*m_manager, *m_windowManager, FORMAT_NORMAL, "realm pause lifecycle").starts_with("paused realm"));
    EXPECT_EQ(realm->state(), eRealmState::PAUSED);
    EXPECT_TRUE(realmCommandRequest(*m_manager, *m_windowManager, FORMAT_NORMAL, "realm resume lifecycle").starts_with("resumed realm"));
    EXPECT_EQ(realm->state(), eRealmState::RUNNING);
    EXPECT_TRUE(realmCommandRequest(*m_manager, *m_windowManager, FORMAT_NORMAL, "realm stop lifecycle").starts_with("stopping realm"));
    ASSERT_TRUE(waitForIPCState(*m_manager, realm, eRealmState::STOPPED));

    m_windowManager->dissociateWindow(42);
    ASSERT_TRUE(destroyThroughIPC(*m_manager, *m_windowManager, "lifecycle"));
    EXPECT_FALSE(m_manager->realmByName("lifecycle"));
}

TEST_F(CRealmIPCTest, grantsAndRevokesOnlyEnforcedCapabilitiesThroughAdministrativeIPC) {
    ASSERT_TRUE(m_manager->createRealm("policy realm"));
    const auto realm = m_manager->realmByName("policy realm");
    ASSERT_TRUE(realm);

    auto response = realmCommandRequest(*m_manager, *m_windowManager, FORMAT_JSON, "realm grant policy realm keyboard");
    EXPECT_TRUE(response.contains(R"("ok":true)"));
    EXPECT_TRUE(response.contains(R"("keyboard":true)"));
    EXPECT_TRUE(realm->capabilities().keyboard);

    response = realmCommandRequest(*m_manager, *m_windowManager, FORMAT_JSON, "realm grant policy realm network");
    EXPECT_TRUE(response.contains(R"("ok":false)"));
    EXPECT_TRUE(response.contains("unknown or unenforced realm capability"));
    EXPECT_TRUE(realm->capabilities().network.empty());

    response = realmCommandRequest(*m_manager, *m_windowManager, FORMAT_JSON, "realm revoke policy realm keyboard");
    EXPECT_TRUE(response.contains(R"("ok":true)"));
    EXPECT_TRUE(response.contains(R"("keyboard":false)"));
    EXPECT_FALSE(realm->capabilities().keyboard);
    EXPECT_TRUE(realmCommandRequest(*m_manager, *m_windowManager, FORMAT_NORMAL, "realm grant policy").contains("requires a name and capability"));
}

TEST_F(CRealmIPCTest, formatsLifecycleEventDataAsEscapedJSON) {
    auto created = m_manager->createRealm(R"(quoted"realm)");
    ASSERT_TRUE(created);

    EXPECT_EQ(
        realmLifecycleEventData(SRealmLifecycleEvent{.type = eRealmLifecycleEvent::CREATED, .realm = *created}),
        R"({"id":1,"name":"quoted\"realm","state":"stopped","input_owner":"none","observation_permission":"denied","capabilities":{"observe":false,"pointer":false,"keyboard":false,"clipboard":false,"network":[],"filesystem_read":[],"filesystem_write":[],"secrets":[]}})");
}
