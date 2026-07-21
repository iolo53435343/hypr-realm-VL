#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace Realm::MCP {
    class CRealmControlClient;

    class CMCPServer {
      public:
        CMCPServer(CRealmControlClient& controlClient, std::optional<std::string> boundRealm = std::nullopt);

        int run();

      private:
        std::optional<std::string> handleMessage(std::string_view message);
        std::string                callTool(std::string_view parameters);

        struct SRealmToolState {
            std::optional<uint64_t> lastCaptureHash;
            uint32_t                coordinateWidth  = 1280;
            uint32_t                coordinateHeight = 720;
        };

        CRealmControlClient&                                m_controlClient;
        std::optional<std::string>                          m_boundRealm;
        std::map<std::string, SRealmToolState, std::less<>> m_realmStates;
        bool                                                m_initialized = false;
    };
}
