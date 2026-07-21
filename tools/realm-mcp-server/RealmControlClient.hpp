#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <hyprutils/os/FileDescriptor.hpp>

namespace Realm::MCP {
    struct SCaptureRegion {
        uint32_t x      = 0;
        uint32_t y      = 0;
        uint32_t width  = 0;
        uint32_t height = 0;
    };

    struct SCaptureFrame {
        uint32_t             format    = 0;
        uint32_t             width     = 0;
        uint32_t             height    = 0;
        uint32_t             stride    = 0;
        bool                 yInverted = false;
        std::vector<uint8_t> pixels;
    };

    class CRealmControlClient {
      public:
        explicit CRealmControlClient(std::filesystem::path socketPath);

        static std::expected<std::filesystem::path, std::string> discoverSocketPath();

        std::expected<void, std::string>                         connect();
        std::expected<std::string, std::string>                  listRealms();
        std::expected<std::string, std::string>                  realmInfo(std::string_view realm);
        std::expected<std::string, std::string>                  createRealm(std::string_view realm);
        std::expected<std::string, std::string>                  startRealm(std::string_view realm);
        std::expected<std::string, std::string>                  pauseRealm(std::string_view realm);
        std::expected<std::string, std::string>                  resumeRealm(std::string_view realm);
        std::expected<std::string, std::string>                  stopRealm(std::string_view realm);
        std::expected<std::string, std::string>                  destroyRealm(std::string_view realm);
        std::expected<std::string, std::string>                  grantCapability(std::string_view realm, std::string_view capability);
        std::expected<std::string, std::string>                  openApplication(std::string_view realm, std::string_view application);
        std::expected<std::string, std::string>                  placeRealm(std::string_view realm, int64_t workspace);
        std::expected<std::string, std::string>                  allowObservation(std::string_view realm);
        std::expected<std::string, std::string>                  denyObservation(std::string_view realm);
        std::expected<SCaptureFrame, std::string>                capture(std::string_view realm, std::optional<SCaptureRegion> region);
        std::expected<std::string, std::string>                  movePointer(std::string_view realm, uint32_t x, uint32_t y, uint32_t width, uint32_t height);
        std::expected<std::string, std::string> pointAndClick(std::string_view realm, uint32_t x, uint32_t y, uint32_t width, uint32_t height, std::string_view button,
                                                              uint32_t count);
        std::expected<std::string, std::string> clickPointer(std::string_view realm, std::string_view button);
        std::expected<std::string, std::string> scrollPointer(std::string_view realm, std::string_view axis, int32_t steps);
        std::expected<std::string, std::string> pressKey(std::string_view realm, uint32_t keycode);
        std::expected<std::string, std::string> pressShortcut(std::string_view realm, const std::vector<uint32_t>& keycodes);
        std::expected<std::string, std::string> typeText(std::string_view realm, std::string_view text);

      private:
        struct SControlPacket {
            std::string                    json;
            Hyprutils::OS::CFileDescriptor descriptor;
        };

        std::expected<std::string, std::string>    request(std::string_view method, std::optional<std::string_view> realm, std::string_view extraParameters = {});
        std::expected<std::string, std::string>    input(std::string_view realm, std::string_view method, std::string_view extraParameters);
        std::expected<SControlPacket, std::string> readPacket();
        std::expected<void, std::string>           readExact(void* data, size_t size, Hyprutils::OS::CFileDescriptor& descriptor);
        std::expected<void, std::string>           writeAll(std::string_view data);

        std::filesystem::path                      m_socketPath;
        Hyprutils::OS::CFileDescriptor             m_socket;
        uint64_t                                   m_nextRequestID = 1;
    };
}
