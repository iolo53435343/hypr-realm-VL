#include "RealmInputProtocol.hpp"

#include <algorithm>
#include <array>
#include <limits>

using namespace Realm;

static constexpr uint32_t REALM_INPUT_PROTOCOL_MAGIC   = 0x48524149;
static constexpr uint16_t REALM_INPUT_PROTOCOL_VERSION = 2;
static constexpr size_t   REALM_INPUT_HEADER_SIZE      = 16;
static constexpr size_t   REALM_INPUT_MAX_PACKET_SIZE  = REALM_INPUT_HEADER_SIZE + REALM_INPUT_MAX_TEXT_SIZE;

static void               appendUint16(std::vector<uint8_t>& output, uint16_t value) {
    output.emplace_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    output.emplace_back(static_cast<uint8_t>(value & 0xFF));
}

static void appendUint32(std::vector<uint8_t>& output, uint32_t value) {
    output.emplace_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    output.emplace_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    output.emplace_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    output.emplace_back(static_cast<uint8_t>(value & 0xFF));
}

static void appendUint64(std::vector<uint8_t>& output, uint64_t value) {
    appendUint32(output, static_cast<uint32_t>(value >> 32));
    appendUint32(output, static_cast<uint32_t>(value & 0xFFFFFFFF));
}

static uint16_t readUint16(const uint8_t* data) {
    return (static_cast<uint16_t>(data[0]) << 8) | static_cast<uint16_t>(data[1]);
}

static uint32_t readUint32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) | (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
}

static uint64_t readUint64(const uint8_t* data) {
    return (static_cast<uint64_t>(readUint32(data)) << 32) | readUint32(data + 4);
}

static bool isKnownType(eRealmInputMessageType type) {
    switch (type) {
        case eRealmInputMessageType::READY:
        case eRealmInputMessageType::ERROR:
        case eRealmInputMessageType::RELEASE_ALL:
        case eRealmInputMessageType::POINTER_MOVE:
        case eRealmInputMessageType::POINTER_BUTTON:
        case eRealmInputMessageType::POINTER_SCROLL:
        case eRealmInputMessageType::KEYBOARD_KEY:
        case eRealmInputMessageType::KEYBOARD_TYPE:
        case eRealmInputMessageType::CAPTURE:
        case eRealmInputMessageType::CAPTURE_REGION:
        case eRealmInputMessageType::CAPTURE_READY:
        case eRealmInputMessageType::CAPTURE_CANCEL:
        case eRealmInputMessageType::POINTER_CLICK:
        case eRealmInputMessageType::KEYBOARD_PRESS: return true;
    }

    return false;
}

static std::expected<std::vector<uint8_t>, std::string> encodePayload(const SRealmInputMessage& message) {
    std::vector<uint8_t> payload;

    switch (message.type) {
        case eRealmInputMessageType::READY:
        case eRealmInputMessageType::RELEASE_ALL:
        case eRealmInputMessageType::CAPTURE:
        case eRealmInputMessageType::CAPTURE_CANCEL: break;
        case eRealmInputMessageType::ERROR:
        case eRealmInputMessageType::KEYBOARD_TYPE: {
            if (message.text.size() > REALM_INPUT_MAX_TEXT_SIZE)
                return std::unexpected("realm input text exceeds the protocol limit");
            payload.assign(message.text.begin(), message.text.end());
            break;
        }
        case eRealmInputMessageType::POINTER_MOVE: {
            if (message.width == 0 || message.height == 0 || message.x >= message.width || message.y >= message.height)
                return std::unexpected("pointer coordinates must be inside a non-empty realm output");
            appendUint32(payload, message.x);
            appendUint32(payload, message.y);
            appendUint32(payload, message.width);
            appendUint32(payload, message.height);
            break;
        }
        case eRealmInputMessageType::CAPTURE_REGION: {
            if (message.width == 0 || message.height == 0 || message.x >= REALM_INPUT_OUTPUT_WIDTH || message.y >= REALM_INPUT_OUTPUT_HEIGHT ||
                message.width > REALM_INPUT_OUTPUT_WIDTH - message.x || message.height > REALM_INPUT_OUTPUT_HEIGHT - message.y)
                return std::unexpected("capture region must be inside the realm output");
            appendUint32(payload, message.x);
            appendUint32(payload, message.y);
            appendUint32(payload, message.width);
            appendUint32(payload, message.height);
            break;
        }
        case eRealmInputMessageType::POINTER_BUTTON:
        case eRealmInputMessageType::KEYBOARD_KEY: {
            appendUint32(payload, message.code);
            appendUint32(payload, message.pressed ? 1 : 0);
            break;
        }
        case eRealmInputMessageType::POINTER_CLICK:
        case eRealmInputMessageType::KEYBOARD_PRESS: appendUint32(payload, message.code); break;
        case eRealmInputMessageType::POINTER_SCROLL: {
            appendUint32(payload, static_cast<uint32_t>(message.horizontal));
            appendUint32(payload, static_cast<uint32_t>(message.vertical));
            break;
        }
        case eRealmInputMessageType::CAPTURE_READY: {
            if ((message.format != REALM_CAPTURE_FORMAT_ARGB8888 && message.format != REALM_CAPTURE_FORMAT_XRGB8888) || message.width == 0 || message.height == 0 ||
                message.width > REALM_INPUT_OUTPUT_WIDTH || message.height > REALM_INPUT_OUTPUT_HEIGHT || message.stride < message.width * 4ULL || message.byteSize == 0 ||
                message.byteSize > REALM_CAPTURE_MAX_BYTES || message.byteSize != static_cast<uint64_t>(message.stride) * message.height)
                return std::unexpected("capture metadata does not describe a valid bounded frame");
            appendUint32(payload, message.format);
            appendUint32(payload, message.width);
            appendUint32(payload, message.height);
            appendUint32(payload, message.stride);
            appendUint32(payload, message.flags);
            appendUint64(payload, message.byteSize);
            break;
        }
    }

    return payload;
}

std::expected<std::vector<uint8_t>, std::string> Realm::encodeRealmInputMessage(const SRealmInputMessage& message) {
    if (!isKnownType(message.type))
        return std::unexpected("unknown realm input message type");

    auto payload = encodePayload(message);
    if (!payload)
        return std::unexpected(payload.error());
    if (payload->size() > std::numeric_limits<uint32_t>::max())
        return std::unexpected("realm input payload is too large");

    std::vector<uint8_t> packet;
    packet.reserve(REALM_INPUT_HEADER_SIZE + payload->size());
    appendUint32(packet, REALM_INPUT_PROTOCOL_MAGIC);
    appendUint16(packet, REALM_INPUT_PROTOCOL_VERSION);
    appendUint16(packet, static_cast<uint16_t>(message.type));
    appendUint32(packet, message.sequence);
    appendUint32(packet, static_cast<uint32_t>(payload->size()));
    packet.insert(packet.end(), payload->begin(), payload->end());
    return packet;
}

std::expected<SRealmInputMessage, std::string> Realm::decodeRealmInputMessage(const uint8_t* data, size_t size) {
    if (!data || size < REALM_INPUT_HEADER_SIZE)
        return std::unexpected("realm input packet is truncated");
    if (size > REALM_INPUT_MAX_PACKET_SIZE)
        return std::unexpected("realm input packet exceeds the protocol limit");
    if (readUint32(data) != REALM_INPUT_PROTOCOL_MAGIC)
        return std::unexpected("realm input packet has an invalid magic value");
    if (readUint16(data + 4) != REALM_INPUT_PROTOCOL_VERSION)
        return std::unexpected("realm input packet uses an unsupported protocol version");

    const auto type = static_cast<eRealmInputMessageType>(readUint16(data + 6));
    if (!isKnownType(type))
        return std::unexpected("realm input packet has an unknown message type");

    const auto payloadSize = static_cast<size_t>(readUint32(data + 12));
    if (payloadSize != size - REALM_INPUT_HEADER_SIZE)
        return std::unexpected("realm input packet payload length does not match its header");

    SRealmInputMessage message{
        .type     = type,
        .sequence = readUint32(data + 8),
    };
    const auto* payload = data + REALM_INPUT_HEADER_SIZE;

    switch (type) {
        case eRealmInputMessageType::READY:
        case eRealmInputMessageType::RELEASE_ALL:
        case eRealmInputMessageType::CAPTURE:
        case eRealmInputMessageType::CAPTURE_CANCEL:
            if (payloadSize != 0)
                return std::unexpected("realm input message must not contain a payload");
            break;
        case eRealmInputMessageType::ERROR:
        case eRealmInputMessageType::KEYBOARD_TYPE:
            if (payloadSize > REALM_INPUT_MAX_TEXT_SIZE)
                return std::unexpected("realm input text exceeds the protocol limit");
            message.text.assign(reinterpret_cast<const char*>(payload), payloadSize);
            break;
        case eRealmInputMessageType::POINTER_MOVE:
            if (payloadSize != 16)
                return std::unexpected("pointer move payload has an invalid length");
            message.x      = readUint32(payload);
            message.y      = readUint32(payload + 4);
            message.width  = readUint32(payload + 8);
            message.height = readUint32(payload + 12);
            if (message.width == 0 || message.height == 0 || message.x >= message.width || message.y >= message.height)
                return std::unexpected("pointer coordinates must be inside a non-empty realm output");
            break;
        case eRealmInputMessageType::CAPTURE_REGION:
            if (payloadSize != 16)
                return std::unexpected("capture region payload has an invalid length");
            message.x      = readUint32(payload);
            message.y      = readUint32(payload + 4);
            message.width  = readUint32(payload + 8);
            message.height = readUint32(payload + 12);
            if (message.width == 0 || message.height == 0 || message.x >= REALM_INPUT_OUTPUT_WIDTH || message.y >= REALM_INPUT_OUTPUT_HEIGHT ||
                message.width > REALM_INPUT_OUTPUT_WIDTH - message.x || message.height > REALM_INPUT_OUTPUT_HEIGHT - message.y)
                return std::unexpected("capture region must be inside the realm output");
            break;
        case eRealmInputMessageType::POINTER_BUTTON:
        case eRealmInputMessageType::KEYBOARD_KEY:
            if (payloadSize != 8)
                return std::unexpected("realm input key or button payload has an invalid length");
            message.code = readUint32(payload);
            if (readUint32(payload + 4) > 1)
                return std::unexpected("realm input pressed state must be zero or one");
            message.pressed = readUint32(payload + 4) == 1;
            break;
        case eRealmInputMessageType::POINTER_CLICK:
        case eRealmInputMessageType::KEYBOARD_PRESS:
            if (payloadSize != 4)
                return std::unexpected("realm input atomic key or button payload has an invalid length");
            message.code = readUint32(payload);
            break;
        case eRealmInputMessageType::POINTER_SCROLL:
            if (payloadSize != 8)
                return std::unexpected("pointer scroll payload has an invalid length");
            message.horizontal = static_cast<int32_t>(readUint32(payload));
            message.vertical   = static_cast<int32_t>(readUint32(payload + 4));
            break;
        case eRealmInputMessageType::CAPTURE_READY:
            if (payloadSize != 28)
                return std::unexpected("capture result payload has an invalid length");
            message.format   = readUint32(payload);
            message.width    = readUint32(payload + 4);
            message.height   = readUint32(payload + 8);
            message.stride   = readUint32(payload + 12);
            message.flags    = readUint32(payload + 16);
            message.byteSize = readUint64(payload + 20);
            if ((message.format != REALM_CAPTURE_FORMAT_ARGB8888 && message.format != REALM_CAPTURE_FORMAT_XRGB8888) || message.width == 0 || message.height == 0 ||
                message.width > REALM_INPUT_OUTPUT_WIDTH || message.height > REALM_INPUT_OUTPUT_HEIGHT || message.stride < message.width * 4ULL || message.byteSize == 0 ||
                message.byteSize > REALM_CAPTURE_MAX_BYTES || message.byteSize != static_cast<uint64_t>(message.stride) * message.height)
                return std::unexpected("capture metadata does not describe a valid bounded frame");
            break;
    }

    return message;
}

std::expected<SRealmInputMessage, std::string> Realm::decodeRealmInputMessage(std::string_view packet) {
    return decodeRealmInputMessage(reinterpret_cast<const uint8_t*>(packet.data()), packet.size());
}

size_t Realm::realmInputEventCost(const SRealmInputMessage& message) {
    if (message.type != eRealmInputMessageType::KEYBOARD_TYPE)
        return 1;

    return std::max<size_t>(1, message.text.size());
}

bool Realm::realmInputMessageIsInputCommand(eRealmInputMessageType type) {
    switch (type) {
        case eRealmInputMessageType::POINTER_MOVE:
        case eRealmInputMessageType::POINTER_BUTTON:
        case eRealmInputMessageType::POINTER_CLICK:
        case eRealmInputMessageType::POINTER_SCROLL:
        case eRealmInputMessageType::KEYBOARD_KEY:
        case eRealmInputMessageType::KEYBOARD_PRESS:
        case eRealmInputMessageType::KEYBOARD_TYPE: return true;
        default: return false;
    }
}

bool Realm::realmInputMessageIsCaptureCommand(eRealmInputMessageType type) {
    return type == eRealmInputMessageType::CAPTURE || type == eRealmInputMessageType::CAPTURE_REGION;
}
