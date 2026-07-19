#pragma once

#include "RealmInputProtocol.hpp"
#include "../helpers/memory/Memory.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include <hyprutils/os/FileDescriptor.hpp>

namespace Realm {
    class CRealmManager;

    enum class eRealmInputError : uint8_t {
        INVALID_COMMAND = 0,
        REALM_NOT_FOUND,
        INPUT_DENIED,
        CONTROLLER_UNAVAILABLE,
        RATE_LIMITED,
        TRANSPORT_ERROR,
        OBSERVATION_DENIED,
        CAPTURE_BUSY,
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
        std::chrono::milliseconds captureTimeout         = std::chrono::seconds(3);
        size_t                    captureRatePerSecond   = 4;
        size_t                    captureBurst           = 2;
        bool                      integrateWithEventLoop = true;
    };

    struct SRealmCaptureRegion {
        uint32_t x      = 0;
        uint32_t y      = 0;
        uint32_t width  = 0;
        uint32_t height = 0;
    };

    struct SRealmCaptureResult {
        uint64_t                       captureID = 0;
        uint64_t                       realmID   = 0;
        uint32_t                       format    = 0;
        uint32_t                       width     = 0;
        uint32_t                       height    = 0;
        uint32_t                       stride    = 0;
        uint32_t                       flags     = 0;
        uint64_t                       byteSize  = 0;
        Hyprutils::OS::CFileDescriptor frameFD;
        std::string                    error;
    };

    class CRealmInputControllerManager {
      public:
        explicit CRealmInputControllerManager(CRealmManager& manager);
        CRealmInputControllerManager(CRealmManager& manager, SRealmInputControllerOptions options);
        ~CRealmInputControllerManager();

        std::expected<uint32_t, SRealmInputError> sendInput(uint64_t realmID, SRealmInputMessage message);
        std::expected<uint64_t, SRealmInputError> requestCapture(uint64_t realmID, std::optional<SRealmCaptureRegion> region = std::nullopt);
        void                                      setCaptureResultCallback(std::function<void(SRealmCaptureResult)> callback);
        bool                                      controllerReady(uint64_t realmID) const;
        std::string                               controllerError(uint64_t realmID) const;
        void                                      dispatchPendingEvents();

      private:
        struct SImpl;
        UP<SImpl> m_impl;
    };

    UP<CRealmInputControllerManager>& inputControllerManager();
}
