#include <realm/RealmControlServer.hpp>
#include <realm/RealmInputController.hpp>
#include <realm/RealmManager.hpp>
#include <realm/RealmWindowManager.hpp>
#include <SharedDefs.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include <hyprutils/os/FileDescriptor.hpp>

using namespace Hyprutils::OS;
using namespace Realm;

static bool waitForControlState(CRealmManager& manager, const SP<CRealm>& realm, eRealmState state) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        manager.dispatchPendingEvents();
        if (realm->state() == state)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

static bool waitForInputController(CRealmManager& manager, CRealmInputControllerManager& inputController, const SP<CRealm>& realm) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        manager.dispatchPendingEvents();
        inputController.dispatchPendingEvents();
        if (inputController.controllerReady(realm->id()))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

static bool waitForControllerLog(const CRealm& realm, std::string_view expected) {
    const auto logPath  = std::filesystem::path{realm.runtimeDirectory()} / "input-controller.log";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        std::ifstream     stream{logPath};
        const std::string contents{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
        if (contents.contains(expected))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

static std::vector<std::string> decodeControlFrames(std::string& bytes) {
    std::vector<std::string> responses;
    size_t                   offset = 0;
    while (bytes.size() - offset >= 4) {
        const auto* header = rc<const unsigned char*>(bytes.data() + offset);
        const auto  length = (sc<uint32_t>(header[0]) << 24) | (sc<uint32_t>(header[1]) << 16) | (sc<uint32_t>(header[2]) << 8) | sc<uint32_t>(header[3]);
        if (bytes.size() - offset < sc<size_t>(length) + 4)
            break;

        responses.emplace_back(bytes.data() + offset + 4, length);
        offset += sc<size_t>(length) + 4;
    }

    bytes.erase(0, offset);
    return responses;
}

class CRealmControlServerTest : public testing::Test {
  protected:
    struct SReceivedResponses {
        std::vector<std::string> responses;
        CFileDescriptor          descriptor;
    };

    void SetUp() override {
        m_root = std::filesystem::temp_directory_path() / std::format("hrcontrol.{}", getpid());
        std::filesystem::remove_all(m_root);
        std::filesystem::create_directory(m_root);
        std::filesystem::permissions(m_root, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);

        m_manager         = makeUnique<CRealmManager>(SRealmManagerOptions{
                    .runtimeRoot            = m_root,
                    .compositorBinary       = REALM_PROCESS_HELPER_PATH,
                    .hostWaylandSocket      = "/tmp/unused-test-wayland-socket",
                    .startupTimeout         = std::chrono::seconds(1),
                    .stopTimeout            = std::chrono::milliseconds(200),
                    .integrateWithEventLoop = false,
        });
        m_inputController = makeUnique<CRealmInputControllerManager>(*m_manager,
                                                                     SRealmInputControllerOptions{
                                                                         .controllerBinary       = REALM_INPUT_CONTROLLER_PROCESS_HELPER_PATH,
                                                                         .integrateWithEventLoop = false,
                                                                     });
        m_windowManager   = makeUnique<CRealmWindowManager>(*m_manager, SRealmWindowManagerOptions{.integrateWithEventBus = false});
        startServer();
    }

    void TearDown() override {
        m_server.reset();
        m_windowManager.reset();
        m_inputController.reset();
        m_manager.reset();
        std::filesystem::remove_all(m_root);
    }

    void startServer(SRealmControlServerOptions options = {}) {
        if (options.socketPath.empty())
            options.socketPath = m_root / ".realm-control.sock";
        options.integrateWithEventLoop = false;
        m_server                       = makeUnique<CRealmControlServer>(*m_manager, *m_windowManager, *m_inputController, std::move(options));
        ASSERT_TRUE(m_server->isListening()) << m_server->lastError();
    }

    CFileDescriptor connectClient() {
        CFileDescriptor client{socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)};
        if (!client.isValid())
            return client;

        sockaddr_un address{.sun_family = AF_UNIX};
        const auto  path = m_server->socketPath().string();
        std::copy(path.begin(), path.end(), address.sun_path);
        address.sun_path[path.size()] = '\0';
        if (connect(client.get(), rc<sockaddr*>(&address), sizeof(address)) < 0) {
            client.reset();
            return client;
        }
        return client;
    }

    std::vector<std::string> receiveResponses(CFileDescriptor& client, size_t count) {
        std::string              bytes;
        std::vector<std::string> receivedResponses;
        for (size_t attempt = 0; attempt < 20; ++attempt) {
            m_server->dispatchPendingEvents();

            std::array<char, 4096> buffer{};
            while (true) {
                const auto received = recv(client.get(), buffer.data(), buffer.size(), 0);
                if (received > 0) {
                    bytes.append(buffer.data(), sc<size_t>(received));
                    continue;
                }
                if (received < 0 && errno == EINTR)
                    continue;
                break;
            }

            auto responses = decodeControlFrames(bytes);
            receivedResponses.insert(receivedResponses.end(), std::make_move_iterator(responses.begin()), std::make_move_iterator(responses.end()));
            if (receivedResponses.size() >= count)
                return receivedResponses;
        }
        return {};
    }

    SReceivedResponses receiveResponsesWithDescriptor(CFileDescriptor& client, size_t count) {
        std::string        bytes;
        SReceivedResponses received;
        for (size_t attempt = 0; attempt < 100; ++attempt) {
            m_manager->dispatchPendingEvents();
            m_inputController->dispatchPendingEvents();
            m_server->dispatchPendingEvents();

            std::array<char, 4096> buffer{};
            while (true) {
                iovec                                         iov{.iov_base = buffer.data(), .iov_len = buffer.size()};
                std::array<char, CMSG_SPACE(sizeof(int) * 2)> ancillary{};
                msghdr                                        header{
                                                           .msg_iov        = &iov,
                                                           .msg_iovlen     = 1,
                                                           .msg_control    = ancillary.data(),
                                                           .msg_controllen = ancillary.size(),
                };
                const auto size = recvmsg(client.get(), &header, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
                if (size > 0) {
                    bytes.append(buffer.data(), sc<size_t>(size));
                    for (auto* control = CMSG_FIRSTHDR(&header); control; control = CMSG_NXTHDR(&header, control)) {
                        if (control->cmsg_level != SOL_SOCKET || control->cmsg_type != SCM_RIGHTS || control->cmsg_len < CMSG_LEN(sizeof(int)))
                            continue;
                        int descriptor = -1;
                        std::memcpy(&descriptor, CMSG_DATA(control), sizeof(descriptor));
                        if (!received.descriptor.isValid())
                            received.descriptor = CFileDescriptor{descriptor};
                        else
                            close(descriptor);
                    }
                    continue;
                }
                if (size < 0 && errno == EINTR)
                    continue;
                break;
            }

            auto responses = decodeControlFrames(bytes);
            received.responses.insert(received.responses.end(), std::make_move_iterator(responses.begin()), std::make_move_iterator(responses.end()));
            if (received.responses.size() >= count)
                return received;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return received;
    }

    std::filesystem::path            m_root;
    UP<CRealmManager>                m_manager;
    UP<CRealmInputControllerManager> m_inputController;
    UP<CRealmWindowManager>          m_windowManager;
    UP<CRealmControlServer>          m_server;
};

TEST_F(CRealmControlServerTest, rejectsMalformedAndAmbiguousRequestsWithStructuredErrors) {
    EXPECT_TRUE(realmControlRequest(*m_manager, *m_windowManager, "not-json").contains(R"("code":"parse_error")"));
    EXPECT_TRUE(realmControlRequest(*m_manager, *m_windowManager, R"({"request_id":"1","method":"realm.list"} trailing)").contains(R"("code":"parse_error")"));
    EXPECT_TRUE(realmControlRequest(*m_manager, *m_windowManager, R"({"request_id":"1","method":"realm.list","unknown":true})").contains(R"("code":"parse_error")"));
    EXPECT_TRUE(realmControlRequest(*m_manager, *m_windowManager, R"({"method":"realm.list"})").contains(R"("code":"invalid_request")"));
    EXPECT_TRUE(realmControlRequest(*m_manager, *m_windowManager, R"({"request_id":"2","method":"realm.dance"})").contains(R"("code":"method_not_found")"));
    EXPECT_TRUE(realmControlRequest(*m_manager, *m_windowManager, R"({"request_id":"3","method":"realm.info"})").contains(R"("code":"invalid_params")"));
    EXPECT_TRUE(realmControlRequest(*m_manager, *m_windowManager, R"({"request_id":"4","method":"realm.list","params":{"realm":"codex"}})").contains(R"("code":"invalid_params")"));
}

TEST_F(CRealmControlServerTest, exposesEveryInitialRealmMethod) {
    auto response = realmControlRequest(*m_manager, *m_windowManager, R"({"request_id":"1","method":"realm.create","params":{"realm":"codex"}})");
    EXPECT_TRUE(response.contains(R"("ok":true)"));
    EXPECT_TRUE(response.contains(R"("action":"created")"));

    response = realmControlRequest(*m_manager, *m_windowManager, R"({"request_id":"2","method":"realm.list"})");
    EXPECT_TRUE(response.contains(R"("realms":[{"id":1,"name":"codex")"));
    response = realmControlRequest(*m_manager, *m_windowManager, R"({"request_id":"3","method":"realm.info","params":{"realm":"codex"}})");
    EXPECT_TRUE(response.contains(R"("realm":{"id":1,"name":"codex")"));

    response = realmControlRequest(*m_manager, *m_windowManager, R"({"request_id":"4","method":"realm.start","params":{"realm":"codex"}})");
    EXPECT_TRUE(response.contains(R"("action":"starting")"));
    const auto realm = m_manager->realmByName("codex");
    ASSERT_TRUE(realm);
    ASSERT_TRUE(waitForControlState(*m_manager, realm, eRealmState::RUNNING));

    ASSERT_TRUE(m_windowManager->associateWindow(42, realm->compositorPID()));
    response = realmControlRequest(*m_manager, *m_windowManager, R"({"request_id":"5","method":"realm.takeover","params":{"realm":"codex"}})");
    EXPECT_TRUE(response.contains(R"("action":"taken over")"));
    response = realmControlRequest(*m_manager, *m_windowManager, R"({"request_id":"6","method":"realm.release","params":{"realm":"codex"}})");
    EXPECT_TRUE(response.contains(R"("action":"released")"));

    response = realmControlRequest(*m_manager, *m_windowManager, R"({"request_id":"7","method":"realm.pause","params":{"realm":"codex"}})");
    EXPECT_TRUE(response.contains(R"("action":"paused")"));
    response = realmControlRequest(*m_manager, *m_windowManager, R"({"request_id":"8","method":"realm.resume","params":{"realm":"codex"}})");
    EXPECT_TRUE(response.contains(R"("action":"resumed")"));
    response = realmControlRequest(*m_manager, *m_windowManager, R"({"request_id":"9","method":"realm.stop","params":{"realm":"codex"}})");
    EXPECT_TRUE(response.contains(R"("action":"stopping")"));
    ASSERT_TRUE(waitForControlState(*m_manager, realm, eRealmState::STOPPED));

    m_windowManager->dissociateWindow(42);
    response = realmControlRequest(*m_manager, *m_windowManager, R"({"request_id":"10","method":"realm.destroy","params":{"realm":"codex"}})");
    EXPECT_TRUE(response.contains(R"("action":"destroyed")"));
    EXPECT_FALSE(m_manager->realmByName("codex"));
}

TEST_F(CRealmControlServerTest, routesInputOnlyToAReadyAgentOwnedRealm) {
    ASSERT_TRUE(m_manager->createRealm("input"));
    const auto realm = m_manager->realmByName("input");
    ASSERT_TRUE(realm);
    ASSERT_TRUE(m_manager->startRealm(realm->id()));
    ASSERT_TRUE(waitForControlState(*m_manager, realm, eRealmState::RUNNING));
    ASSERT_TRUE(waitForInputController(*m_manager, *m_inputController, realm)) << m_inputController->controllerError(realm->id());

    auto response = realmControlRequest(*m_manager, *m_windowManager, m_inputController.get(),
                                        R"({"request_id":"1","method":"keyboard.type","params":{"realm":"input","text":"echo realm\n"}})");
    EXPECT_TRUE(response.contains(R"("code":"capability_denied")"));
    ASSERT_TRUE(m_manager->grantCapability(realm->id(), eRealmCapability::KEYBOARD));
    response = realmControlRequest(*m_manager, *m_windowManager, m_inputController.get(),
                                   R"({"request_id":"1-granted","method":"keyboard.type","params":{"realm":"input","text":"echo realm\n"}})");
    EXPECT_TRUE(response.contains(R"("ok":true)"));
    EXPECT_TRUE(response.contains(R"("action":"queued")"));
    ASSERT_TRUE(waitForControllerLog(*realm, "KEYBOARD_TYPE"));
    response =
        realmControlRequest(*m_manager, *m_windowManager, m_inputController.get(), R"({"request_id":"1-press","method":"keyboard.press","params":{"realm":"input","keycode":30}})");
    EXPECT_TRUE(response.contains(R"("ok":true)"));
    ASSERT_TRUE(waitForControllerLog(*realm, "KEYBOARD_PRESS"));

    ASSERT_TRUE(m_windowManager->associateWindow(42, realm->compositorPID()));
    response = realmControlRequest(*m_manager, *m_windowManager, m_inputController.get(), R"({"request_id":"2","method":"realm.takeover","params":{"realm":"input"}})");
    EXPECT_TRUE(response.contains(R"("ok":true)"));
    ASSERT_TRUE(waitForControllerLog(*realm, "RELEASE_ALL"));

    response = realmControlRequest(*m_manager, *m_windowManager, m_inputController.get(), R"({"request_id":"3","method":"pointer.move","params":{"realm":"input","x":10,"y":20}})");
    EXPECT_TRUE(response.contains(R"("code":"input_denied")"));

    ASSERT_TRUE(m_windowManager->releaseRealm(realm->id()));
    response = realmControlRequest(*m_manager, *m_windowManager, m_inputController.get(), R"({"request_id":"4","method":"pointer.move","params":{"realm":"input","x":10,"y":20}})");
    EXPECT_TRUE(response.contains(R"("code":"capability_denied")"));
    ASSERT_TRUE(m_manager->grantCapability(realm->id(), eRealmCapability::POINTER));
    response = realmControlRequest(*m_manager, *m_windowManager, m_inputController.get(),
                                   R"({"request_id":"4-granted","method":"pointer.move","params":{"realm":"input","x":10,"y":20}})");
    EXPECT_TRUE(response.contains(R"("ok":true)"));
    ASSERT_TRUE(waitForControllerLog(*realm, "POINTER_MOVE"));
    response = realmControlRequest(*m_manager, *m_windowManager, m_inputController.get(),
                                   R"({"request_id":"4-click","method":"pointer.click","params":{"realm":"input","button":"left"}})");
    EXPECT_TRUE(response.contains(R"("ok":true)"));
    ASSERT_TRUE(waitForControllerLog(*realm, "POINTER_CLICK"));

    ASSERT_TRUE(m_manager->stopRealm(realm->id()));
    ASSERT_TRUE(waitForControlState(*m_manager, realm, eRealmState::STOPPED));
    EXPECT_FALSE(m_inputController->controllerReady(realm->id()));
}

TEST_F(CRealmControlServerTest, revokingInputCapabilityReleasesHeldVirtualInputImmediately) {
    ASSERT_TRUE(m_manager->createRealm("input-revoke"));
    const auto realm = m_manager->realmByName("input-revoke");
    ASSERT_TRUE(realm);
    ASSERT_TRUE(m_manager->grantCapability(realm->id(), eRealmCapability::POINTER));
    ASSERT_TRUE(m_manager->startRealm(realm->id()));
    ASSERT_TRUE(waitForControlState(*m_manager, realm, eRealmState::RUNNING));
    ASSERT_TRUE(waitForInputController(*m_manager, *m_inputController, realm));

    auto response = realmControlRequest(*m_manager, *m_windowManager, m_inputController.get(),
                                        R"({"request_id":"press","method":"pointer.button","params":{"realm":"input-revoke","button":"left","pressed":true}})");
    EXPECT_TRUE(response.contains(R"("ok":true)"));
    ASSERT_TRUE(waitForControllerLog(*realm, "POINTER_BUTTON"));

    ASSERT_TRUE(m_manager->revokeCapability(realm->id(), eRealmCapability::POINTER));
    ASSERT_TRUE(waitForControllerLog(*realm, "RELEASE_ALL"));
    response = realmControlRequest(*m_manager, *m_windowManager, m_inputController.get(),
                                   R"({"request_id":"after-revoke","method":"pointer.move","params":{"realm":"input-revoke","x":10,"y":20}})");
    EXPECT_TRUE(response.contains(R"("code":"capability_denied")"));
}

TEST_F(CRealmControlServerTest, capturesRealmFramesThroughSharedMemoryWithIndependentPermission) {
    ASSERT_TRUE(m_manager->createRealm("capture"));
    const auto realm = m_manager->realmByName("capture");
    ASSERT_TRUE(realm);
    ASSERT_TRUE(m_manager->startRealm(realm->id()));
    ASSERT_TRUE(waitForControlState(*m_manager, realm, eRealmState::RUNNING));
    ASSERT_TRUE(waitForInputController(*m_manager, *m_inputController, realm));

    auto response =
        realmControlRequest(*m_manager, *m_windowManager, m_inputController.get(), R"({"request_id":"capability-denied","method":"realm.capture","params":{"realm":"capture"}})");
    EXPECT_TRUE(response.contains(R"("code":"capability_denied")"));
    response =
        realmControlRequest(*m_manager, *m_windowManager, m_inputController.get(), R"({"request_id":"self-grant-denied","method":"realm.observe","params":{"realm":"capture"}})");
    EXPECT_TRUE(response.contains(R"("code":"capability_denied")"));
    EXPECT_TRUE(
        realmControlRequest(*m_manager, *m_windowManager, m_inputController.get(), R"({"request_id":"no-private-grant","method":"realm.grant","params":{"realm":"capture"}})")
            .contains(R"("code":"method_not_found")"));

    ASSERT_TRUE(m_manager->grantCapability(realm->id(), eRealmCapability::OBSERVE));
    response = realmControlRequest(*m_manager, *m_windowManager, m_inputController.get(), R"({"request_id":"denied","method":"realm.capture","params":{"realm":"capture"}})");
    EXPECT_TRUE(response.contains(R"("code":"observation_denied")"));

    response = realmControlRequest(*m_manager, *m_windowManager, m_inputController.get(), R"({"request_id":"allow","method":"realm.observe","params":{"realm":"capture"}})");
    EXPECT_TRUE(response.contains(R"("ok":true)"));
    ASSERT_TRUE(m_manager->takeoverRealm(realm->id()));
    EXPECT_EQ(realm->inputOwner(), eRealmInputOwner::HUMAN);
    EXPECT_EQ(realm->observationPermission(), eRealmObservationPermission::ALLOWED);

    auto client = connectClient();
    ASSERT_TRUE(client.isValid());
    const auto request = realmControlFrame(R"({"request_id":"frame","method":"realm.capture","params":{"realm":"capture"}})");
    ASSERT_EQ(send(client.get(), request.data(), request.size(), MSG_NOSIGNAL), sc<ssize_t>(request.size()));

    auto received = receiveResponsesWithDescriptor(client, 2);
    ASSERT_EQ(received.responses.size(), 2);
    EXPECT_TRUE(received.responses[0].contains(R"("action":"queued")"));
    EXPECT_TRUE(received.responses[0].contains(R"("capture_id":1)"));
    EXPECT_TRUE(received.responses[1].contains(R"("event":"realm.capture.ready")"));
    EXPECT_TRUE(received.responses[1].contains(R"("transport":"scm_rights")"));
    EXPECT_TRUE(received.responses[1].contains(R"("format_name":"xrgb8888")"));
    EXPECT_TRUE(received.responses[1].contains(R"("width":2)"));
    EXPECT_TRUE(received.responses[1].contains(R"("byte_size":16)"));
    ASSERT_TRUE(received.descriptor.isValid());

    std::array<uint8_t, 16> pixels{};
    ASSERT_EQ(pread(received.descriptor.get(), pixels.data(), pixels.size(), 0), sc<ssize_t>(pixels.size()));
    EXPECT_EQ(pixels.front(), 0x01);
    EXPECT_EQ(pixels.back(), 0x10);

    const auto regionRequest = realmControlFrame(R"({"request_id":"region","method":"realm.capture_region","params":{"realm":"capture","x":10,"y":20,"width":100,"height":80}})");
    ASSERT_EQ(send(client.get(), regionRequest.data(), regionRequest.size(), MSG_NOSIGNAL), sc<ssize_t>(regionRequest.size()));
    received = receiveResponsesWithDescriptor(client, 2);
    ASSERT_EQ(received.responses.size(), 2);
    EXPECT_TRUE(received.responses[0].contains(R"("capture_id":2)"));
    EXPECT_TRUE(received.responses[1].contains(R"("event":"realm.capture.ready")"));
    ASSERT_TRUE(received.descriptor.isValid());
    ASSERT_TRUE(waitForControllerLog(*realm, "CAPTURE_REGION"));

    response = realmControlRequest(*m_manager, *m_windowManager, m_inputController.get(), R"({"request_id":"deny","method":"realm.unobserve","params":{"realm":"capture"}})");
    EXPECT_TRUE(response.contains(R"("ok":true)"));
    response = realmControlRequest(*m_manager, *m_windowManager, m_inputController.get(), R"({"request_id":"denied-again","method":"realm.capture","params":{"realm":"capture"}})");
    EXPECT_TRUE(response.contains(R"("code":"observation_denied")"));
}

TEST_F(CRealmControlServerTest, revokingObservationCancelsPendingCaptureAndIgnoresLateFrame) {
    ASSERT_TRUE(m_manager->createRealm("capture-cancel"));
    const auto realm = m_manager->realmByName("capture-cancel");
    ASSERT_TRUE(realm);
    ASSERT_TRUE(m_manager->startRealm(realm->id()));
    ASSERT_TRUE(waitForControlState(*m_manager, realm, eRealmState::RUNNING));
    ASSERT_TRUE(waitForInputController(*m_manager, *m_inputController, realm));
    ASSERT_TRUE(m_manager->grantCapability(realm->id(), eRealmCapability::OBSERVE));
    ASSERT_TRUE(m_manager->allowObservation(realm->id()));

    auto client = connectClient();
    ASSERT_TRUE(client.isValid());
    const auto request = realmControlFrame(R"({"request_id":"cancel","method":"realm.capture","params":{"realm":"capture-cancel"}})");
    ASSERT_EQ(send(client.get(), request.data(), request.size(), MSG_NOSIGNAL), sc<ssize_t>(request.size()));
    m_server->dispatchPendingEvents();

    ASSERT_TRUE(m_manager->revokeCapability(realm->id(), eRealmCapability::OBSERVE));
    auto received = receiveResponsesWithDescriptor(client, 2);
    ASSERT_EQ(received.responses.size(), 2);
    EXPECT_TRUE(received.responses[0].contains(R"("action":"queued")"));
    EXPECT_TRUE(received.responses[1].contains(R"("event":"realm.capture.failed")"));
    EXPECT_TRUE(received.responses[1].contains("permission was revoked"));
    EXPECT_FALSE(received.descriptor.isValid());
    EXPECT_TRUE(m_inputController->controllerReady(realm->id()));
    EXPECT_FALSE(realm->capabilities().observe);
    EXPECT_EQ(realm->observationPermission(), eRealmObservationPermission::DENIED);
}

TEST_F(CRealmControlServerTest, validatesInputShapesAndBoundsBeforeRouting) {
    ASSERT_TRUE(m_manager->createRealm("validation"));

    EXPECT_TRUE(
        realmControlRequest(*m_manager, *m_windowManager, m_inputController.get(), R"({"request_id":"1","method":"pointer.move","params":{"realm":"validation","x":1280,"y":0}})")
            .contains(R"("code":"invalid_params")"));
    EXPECT_TRUE(realmControlRequest(*m_manager, *m_windowManager, m_inputController.get(),
                                    R"({"request_id":"2","method":"pointer.button","params":{"realm":"validation","button":"side","pressed":true}})")
                    .contains(R"("code":"invalid_params")"));
    EXPECT_TRUE(realmControlRequest(*m_manager, *m_windowManager, m_inputController.get(),
                                    R"({"request_id":"3","method":"pointer.scroll","params":{"realm":"validation","axis":"vertical","steps":0}})")
                    .contains(R"("code":"invalid_params")"));
    EXPECT_TRUE(
        realmControlRequest(*m_manager, *m_windowManager, m_inputController.get(), R"({"request_id":"4","method":"keyboard.type","params":{"realm":"validation","text":""}})")
            .contains(R"("code":"invalid_params")"));
    EXPECT_TRUE(realmControlRequest(*m_manager, *m_windowManager, m_inputController.get(), R"({"request_id":"5","method":"realm.info","params":{"realm":"validation","x":1}})")
                    .contains(R"("code":"invalid_params")"));
    EXPECT_TRUE(realmControlRequest(*m_manager, *m_windowManager, m_inputController.get(),
                                    R"({"request_id":"6","method":"realm.capture_region","params":{"realm":"validation","x":1200,"y":0,"width":100,"height":100}})")
                    .contains(R"("code":"invalid_params")"));
    EXPECT_TRUE(realmControlRequest(*m_manager, *m_windowManager, m_inputController.get(),
                                    R"({"request_id":"7","method":"pointer.click","params":{"realm":"validation","button":"left","pressed":true}})")
                    .contains(R"("code":"invalid_params")"));
    EXPECT_TRUE(realmControlRequest(*m_manager, *m_windowManager, m_inputController.get(),
                                    R"({"request_id":"8","method":"keyboard.press","params":{"realm":"validation","keycode":30,"pressed":true}})")
                    .contains(R"("code":"invalid_params")"));
}

TEST_F(CRealmControlServerTest, rateLimitsInputWithoutConsumingTheBurstOnRejection) {
    ASSERT_TRUE(m_manager->createRealm("limited"));
    const auto realm = m_manager->realmByName("limited");
    ASSERT_TRUE(realm);
    ASSERT_TRUE(m_manager->startRealm(realm->id()));
    ASSERT_TRUE(waitForControlState(*m_manager, realm, eRealmState::RUNNING));
    ASSERT_TRUE(waitForInputController(*m_manager, *m_inputController, realm));
    ASSERT_TRUE(m_manager->grantCapability(realm->id(), eRealmCapability::KEYBOARD));

    const auto oversizedBurst = std::string(513, 'x');
    const auto rejected       = realmControlRequest(*m_manager, *m_windowManager, m_inputController.get(),
                                                    std::format(R"({{"request_id":"1","method":"keyboard.type","params":{{"realm":"limited","text":"{}"}}}})", oversizedBurst));
    EXPECT_TRUE(rejected.contains(R"("code":"rate_limited")"));

    const auto accepted = realmControlRequest(*m_manager, *m_windowManager, m_inputController.get(),
                                              R"({"request_id":"2","method":"keyboard.key","params":{"realm":"limited","keycode":30,"pressed":true}})");
    EXPECT_TRUE(accepted.contains(R"("ok":true)"));
}

TEST_F(CRealmControlServerTest, createsPrivateSocketAndRemovesItOnShutdown) {
    struct stat socketStat{};
    ASSERT_EQ(lstat(m_server->socketPath().c_str(), &socketStat), 0);
    EXPECT_TRUE(S_ISSOCK(socketStat.st_mode));
    EXPECT_EQ(socketStat.st_uid, geteuid());
    EXPECT_EQ(socketStat.st_mode & 0777, 0600);

    const auto path = m_server->socketPath();
    m_server.reset();
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST_F(CRealmControlServerTest, doesNotRemoveAReplacementPathOnShutdown) {
    const auto path = m_server->socketPath();
    ASSERT_EQ(unlink(path.c_str()), 0);
    {
        std::ofstream replacement{path};
        ASSERT_TRUE(replacement.is_open());
        replacement << "replacement";
    }

    m_server.reset();
    EXPECT_TRUE(std::filesystem::is_regular_file(path));
}

TEST_F(CRealmControlServerTest, refusesNonPrivateRuntimeDirectories) {
    m_server.reset();
    const auto insecure = m_root / "insecure";
    std::filesystem::create_directory(insecure);
    std::filesystem::permissions(insecure, std::filesystem::perms::owner_all | std::filesystem::perms::group_read, std::filesystem::perm_options::replace);

    CRealmControlServer server{*m_manager, *m_windowManager, SRealmControlServerOptions{.socketPath = insecure / "control.sock", .integrateWithEventLoop = false}};
    EXPECT_FALSE(server.isListening());
    EXPECT_TRUE(server.lastError().contains("must be owned by the current user and private"));
    EXPECT_FALSE(std::filesystem::exists(insecure / "control.sock"));
}

TEST_F(CRealmControlServerTest, handlesFragmentedAndPipelinedFrames) {
    auto client = connectClient();
    ASSERT_TRUE(client.isValid());

    const auto fragmented = realmControlFrame(R"({"request_id":"fragmented","method":"realm.list"})");
    ASSERT_EQ(send(client.get(), fragmented.data(), 2, MSG_NOSIGNAL), 2);
    m_server->dispatchPendingEvents();
    std::array<char, 16> noResponse{};
    EXPECT_EQ(recv(client.get(), noResponse.data(), noResponse.size(), 0), -1);
    EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);

    ASSERT_EQ(send(client.get(), fragmented.data() + 2, fragmented.size() - 2, MSG_NOSIGNAL), sc<ssize_t>(fragmented.size() - 2));
    auto responses = receiveResponses(client, 1);
    ASSERT_EQ(responses.size(), 1);
    EXPECT_TRUE(responses[0].contains(R"("request_id":"fragmented")"));
    EXPECT_TRUE(responses[0].contains(R"("ok":true)"));

    const auto pipelined =
        realmControlFrame(R"({"request_id":"first","method":"realm.list"})") + realmControlFrame(R"({"request_id":"second","method":"realm.info","params":{"realm":"missing"}})");
    ASSERT_EQ(send(client.get(), pipelined.data(), pipelined.size(), MSG_NOSIGNAL), sc<ssize_t>(pipelined.size()));
    responses = receiveResponses(client, 2);
    ASSERT_EQ(responses.size(), 2);
    EXPECT_TRUE(responses[0].contains(R"("request_id":"first")"));
    EXPECT_TRUE(responses[1].contains(R"("request_id":"second")"));
    EXPECT_TRUE(responses[1].contains(R"("code":"realm_not_found")"));
}

TEST_F(CRealmControlServerTest, rejectsOversizedMessagesBeforeReadingTheirPayload) {
    auto client = connectClient();
    ASSERT_TRUE(client.isValid());

    constexpr uint32_t        OVERSIZED = REALM_CONTROL_MAX_MESSAGE_SIZE + 1;
    const std::array<char, 4> header    = {
        sc<char>((OVERSIZED >> 24) & 0xFF),
        sc<char>((OVERSIZED >> 16) & 0xFF),
        sc<char>((OVERSIZED >> 8) & 0xFF),
        sc<char>(OVERSIZED & 0xFF),
    };
    ASSERT_EQ(send(client.get(), header.data(), header.size(), MSG_NOSIGNAL), sc<ssize_t>(header.size()));
    const auto responses = receiveResponses(client, 1);
    ASSERT_EQ(responses.size(), 1);
    EXPECT_TRUE(responses[0].contains(R"("code":"message_too_large")"));
}

TEST_F(CRealmControlServerTest, boundsWorkForPipelinedRequests) {
    auto client = connectClient();
    ASSERT_TRUE(client.isValid());

    std::string requests;
    for (size_t index = 0; index < 9; ++index)
        requests += realmControlFrame(std::format(R"({{"request_id":"{}","method":"realm.list"}})", index));

    ASSERT_EQ(send(client.get(), requests.data(), requests.size(), MSG_NOSIGNAL), sc<ssize_t>(requests.size()));
    const auto responses = receiveResponses(client, 9);
    ASSERT_EQ(responses.size(), 9);
    for (size_t index = 0; index < 8; ++index)
        EXPECT_TRUE(responses[index].contains(R"("ok":true)"));
    EXPECT_TRUE(responses.back().contains(R"("code":"server_overloaded")"));
}

TEST_F(CRealmControlServerTest, rejectsPeersWhoseCredentialsDoNotMatch) {
    EXPECT_TRUE(realmControlPeerAuthorized(geteuid(), geteuid()));
    EXPECT_FALSE(realmControlPeerAuthorized(geteuid(), geteuid() + 1));

    m_server.reset();
    startServer(SRealmControlServerOptions{
        .socketPath      = m_root / ".realm-control.sock",
        .expectedPeerUID = geteuid() + 1,
    });
    auto client = connectClient();
    ASSERT_TRUE(client.isValid());

    const auto responses = receiveResponses(client, 1);
    ASSERT_EQ(responses.size(), 1);
    EXPECT_TRUE(responses[0].contains(R"("code":"authorization_failed")"));
}
