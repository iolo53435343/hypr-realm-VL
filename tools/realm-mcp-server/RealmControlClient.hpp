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
        CRealmControlClient(std::filesystem::path socketPath, std::string realm);

        static std::expected<std::filesystem::path, std::string> discoverSocketPath();

        std::expected<void, std::string>                         connect();
        std::expected<std::string, std::string>                  realmInfo();
        std::expected<std::string, std::string>                  createRealm();
        std::expected<std::string, std::string>                  startRealm();
        std::expected<std::string, std::string>                  pauseRealm();
        std::expected<std::string, std::string>                  resumeRealm();
        std::expected<std::string, std::string>                  stopRealm();
        std::expected<std::string, std::string>                  allowObservation();
        std::expected<std::string, std::string>                  denyObservation();
        std::expected<SCaptureFrame, std::string>                capture(std::optional<SCaptureRegion> region);
        std::expected<std::string, std::string>                  movePointer(uint32_t x, uint32_t y);
        std::expected<std::string, std::string>                  pointAndClick(uint32_t x, uint32_t y, std::string_view button, uint32_t count);
        std::expected<std::string, std::string>                  clickPointer(std::string_view button);
        std::expected<std::string, std::string>                  scrollPointer(std::string_view axis, int32_t steps);
        std::expected<std::string, std::string>                  pressKey(uint32_t keycode);
        std::expected<std::string, std::string>                  pressShortcut(const std::vector<uint32_t>& keycodes);
        std::expected<std::string, std::string>                  typeText(std::string_view text);

        const std::string&                                       realm() const;
        uint32_t                                                 coordinateWidth() const;
        uint32_t                                                 coordinateHeight() const;

      private:
        struct SControlPacket {
            std::string                    json;
            Hyprutils::OS::CFileDescriptor descriptor;
        };

        std::expected<std::string, std::string>    request(std::string_view method, std::string_view extraParameters = {});
        std::expected<std::string, std::string>    input(std::string_view method, std::string_view extraParameters);
        std::expected<SControlPacket, std::string> readPacket();
        std::expected<void, std::string>           readExact(void* data, size_t size, Hyprutils::OS::CFileDescriptor& descriptor);
        std::expected<void, std::string>           writeAll(std::string_view data);

        std::filesystem::path                      m_socketPath;
        std::string                                m_realm;
        Hyprutils::OS::CFileDescriptor             m_socket;
        uint64_t                                   m_nextRequestID    = 1;
        uint32_t                                   m_coordinateWidth  = 1280;
        uint32_t                                   m_coordinateHeight = 720;
    };
}
