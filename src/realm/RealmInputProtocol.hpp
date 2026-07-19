#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace Realm {
    inline constexpr size_t   REALM_INPUT_MAX_TEXT_SIZE     = 4096;
    inline constexpr uint32_t REALM_INPUT_OUTPUT_WIDTH      = 1280;
    inline constexpr uint32_t REALM_INPUT_OUTPUT_HEIGHT     = 720;
    inline constexpr uint64_t REALM_CAPTURE_MAX_BYTES       = 64ULL * 1024ULL * 1024ULL;
    inline constexpr uint32_t REALM_CAPTURE_FORMAT_ARGB8888 = 0;
    inline constexpr uint32_t REALM_CAPTURE_FORMAT_XRGB8888 = 1;

    enum class eRealmInputMessageType : uint16_t {
        READY = 1,
        ERROR,
        RELEASE_ALL,
        POINTER_MOVE,
        POINTER_BUTTON,
        POINTER_SCROLL,
        KEYBOARD_KEY,
        KEYBOARD_TYPE,
        CAPTURE,
        CAPTURE_REGION,
        CAPTURE_READY,
        CAPTURE_CANCEL,
    };

    struct SRealmInputMessage {
        eRealmInputMessageType type       = eRealmInputMessageType::ERROR;
        uint32_t               sequence   = 0;
        uint32_t               x          = 0;
        uint32_t               y          = 0;
        uint32_t               width      = 0;
        uint32_t               height     = 0;
        uint32_t               code       = 0;
        int32_t                horizontal = 0;
        int32_t                vertical   = 0;
        bool                   pressed    = false;
        uint32_t               format     = 0;
        uint32_t               stride     = 0;
        uint32_t               flags      = 0;
        uint64_t               byteSize   = 0;
        std::string            text;
    };

    std::expected<std::vector<uint8_t>, std::string> encodeRealmInputMessage(const SRealmInputMessage& message);
    std::expected<SRealmInputMessage, std::string>   decodeRealmInputMessage(const uint8_t* data, size_t size);
    std::expected<SRealmInputMessage, std::string>   decodeRealmInputMessage(std::string_view packet);
    size_t                                           realmInputEventCost(const SRealmInputMessage& message);
    bool                                             realmInputMessageIsInputCommand(eRealmInputMessageType type);
    bool                                             realmInputMessageIsCaptureCommand(eRealmInputMessageType type);
}
