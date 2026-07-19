#pragma once

#include "RealmInputProtocol.hpp"
#include "../helpers/memory/Memory.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace Realm {
    class CRealmManager;

    enum class eRealmInputError : uint8_t {
        INVALID_COMMAND = 0,
        REALM_NOT_FOUND,
        INPUT_DENIED,
        CONTROLLER_UNAVAILABLE,
        RATE_LIMITED,
        TRANSPORT_ERROR,
    };

    std::string_view realmInputErrorName(eRealmInputError error);

    struct SRealmInputError {
        eRealmInputError code = eRealmInputError::INVALID_COMMAND;
        std::string      message;
    };

    struct SRealmInputControllerOptions {
        std::filesystem::path     controllerBinary;
        size_t                    ratePerSecond          = 256;
        size_t                    burst                  = 512;
        std::chrono::milliseconds startupTimeout         = std::chrono::seconds(5);
        bool                      integrateWithEventLoop = true;
    };

    class CRealmInputControllerManager {
      public:
        explicit CRealmInputControllerManager(CRealmManager& manager);
        CRealmInputControllerManager(CRealmManager& manager, SRealmInputControllerOptions options);
        ~CRealmInputControllerManager();

        std::expected<uint32_t, SRealmInputError> sendInput(uint64_t realmID, SRealmInputMessage message);
        bool                                      controllerReady(uint64_t realmID) const;
        std::string                               controllerError(uint64_t realmID) const;
        void                                      dispatchPendingEvents();

      private:
        struct SImpl;
        UP<SImpl> m_impl;
    };

    UP<CRealmInputControllerManager>& inputControllerManager();
}
