#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Realm::MCP {
    class CRealmControlClient;

    class CMCPServer {
      public:
        explicit CMCPServer(CRealmControlClient& controlClient);

        int run();

      private:
        std::optional<std::string> handleMessage(std::string_view message);
        std::string                callTool(std::string_view parameters);

        CRealmControlClient&       m_controlClient;
        std::optional<uint64_t>    m_lastCaptureHash;
        bool                       m_initialized = false;
    };
}
