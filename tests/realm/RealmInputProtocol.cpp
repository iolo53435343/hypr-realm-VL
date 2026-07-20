#include <realm/RealmInputProtocol.hpp>

#include <gtest/gtest.h>

#include <array>
#include <string>

using namespace Realm;

TEST(RealmInputProtocol, roundTripsEveryCommandShape) {
    const std::array messages = {
        SRealmInputMessage{.type = eRealmInputMessageType::READY},
        SRealmInputMessage{.type = eRealmInputMessageType::ERROR, .sequence = 2, .text = "failure"},
        SRealmInputMessage{.type = eRealmInputMessageType::RELEASE_ALL, .sequence = 3},
        SRealmInputMessage{.type = eRealmInputMessageType::POINTER_MOVE, .sequence = 4, .x = 12, .y = 34, .width = 1280, .height = 720},
        SRealmInputMessage{.type = eRealmInputMessageType::POINTER_BUTTON, .sequence = 5, .code = 272, .pressed = true},
        SRealmInputMessage{.type = eRealmInputMessageType::POINTER_SCROLL, .sequence = 6, .horizontal = -2, .vertical = 3},
        SRealmInputMessage{.type = eRealmInputMessageType::KEYBOARD_KEY, .sequence = 7, .code = 30, .pressed = true},
        SRealmInputMessage{.type = eRealmInputMessageType::KEYBOARD_TYPE, .sequence = 8, .text = "echo realm\n"},
        SRealmInputMessage{.type = eRealmInputMessageType::CAPTURE, .sequence = 9},
        SRealmInputMessage{.type = eRealmInputMessageType::CAPTURE_REGION, .sequence = 10, .x = 10, .y = 20, .width = 300, .height = 200},
        SRealmInputMessage{.type = eRealmInputMessageType::CAPTURE_READY, .sequence = 10, .width = 300, .height = 200, .format = 1, .stride = 1200, .flags = 1, .byteSize = 240000},
        SRealmInputMessage{.type = eRealmInputMessageType::CAPTURE_CANCEL, .sequence = 10},
        SRealmInputMessage{.type = eRealmInputMessageType::POINTER_CLICK, .sequence = 11, .code = 272},
        SRealmInputMessage{.type = eRealmInputMessageType::KEYBOARD_PRESS, .sequence = 12, .code = 30},
        SRealmInputMessage{.type = eRealmInputMessageType::INPUT_APPLIED, .sequence = 12},
        SRealmInputMessage{.type = eRealmInputMessageType::POINTER_POINT_AND_CLICK, .sequence = 13, .x = 900, .y = 500, .width = 1920, .height = 1048, .code = 272, .count = 2},
        SRealmInputMessage{.type = eRealmInputMessageType::KEYBOARD_SHORTCUT, .sequence = 14, .codes = {29, 20}},
    };

    for (const auto& message : messages) {
        auto encoded = encodeRealmInputMessage(message);
        ASSERT_TRUE(encoded) << encoded.error();
        auto decoded = decodeRealmInputMessage(encoded->data(), encoded->size());
        ASSERT_TRUE(decoded) << decoded.error();
        EXPECT_EQ(decoded->type, message.type);
        EXPECT_EQ(decoded->sequence, message.sequence);
        EXPECT_EQ(decoded->x, message.x);
        EXPECT_EQ(decoded->y, message.y);
        EXPECT_EQ(decoded->width, message.width);
        EXPECT_EQ(decoded->height, message.height);
        EXPECT_EQ(decoded->code, message.code);
        EXPECT_EQ(decoded->horizontal, message.horizontal);
        EXPECT_EQ(decoded->vertical, message.vertical);
        EXPECT_EQ(decoded->pressed, message.pressed);
        EXPECT_EQ(decoded->format, message.format);
        EXPECT_EQ(decoded->stride, message.stride);
        EXPECT_EQ(decoded->flags, message.flags);
        EXPECT_EQ(decoded->count, message.count);
        EXPECT_EQ(decoded->byteSize, message.byteSize);
        EXPECT_EQ(decoded->text, message.text);
        EXPECT_EQ(decoded->codes, message.codes);
    }
}

TEST(RealmInputProtocol, rejectsMalformedAndOversizedPackets) {
    EXPECT_FALSE(decodeRealmInputMessage(nullptr, 0));

    auto packet = encodeRealmInputMessage(SRealmInputMessage{.type = eRealmInputMessageType::KEYBOARD_TYPE, .text = "hello"});
    ASSERT_TRUE(packet);
    EXPECT_FALSE(decodeRealmInputMessage(packet->data(), packet->size() - 1));

    auto badMagic = *packet;
    badMagic[0]   = 0;
    EXPECT_FALSE(decodeRealmInputMessage(badMagic.data(), badMagic.size()));

    auto badVersion = *packet;
    badVersion[5]   = 3;
    EXPECT_FALSE(decodeRealmInputMessage(badVersion.data(), badVersion.size()));

    auto badType = *packet;
    badType[6]   = 0x7F;
    badType[7]   = 0xFF;
    EXPECT_FALSE(decodeRealmInputMessage(badType.data(), badType.size()));

    EXPECT_FALSE(encodeRealmInputMessage(SRealmInputMessage{
        .type = eRealmInputMessageType::KEYBOARD_TYPE,
        .text = std::string(REALM_INPUT_MAX_TEXT_SIZE + 1, 'x'),
    }));
    EXPECT_FALSE(encodeRealmInputMessage(SRealmInputMessage{
        .type   = eRealmInputMessageType::CAPTURE_REGION,
        .x      = 1200,
        .y      = 0,
        .width  = 100,
        .height = 100,
    }));
    EXPECT_FALSE(encodeRealmInputMessage(SRealmInputMessage{
        .type     = eRealmInputMessageType::CAPTURE_READY,
        .width    = 2,
        .height   = 2,
        .stride   = 8,
        .byteSize = 15,
    }));
    EXPECT_FALSE(encodeRealmInputMessage(SRealmInputMessage{
        .type     = eRealmInputMessageType::CAPTURE_READY,
        .width    = 2,
        .height   = 2,
        .format   = 99,
        .stride   = 8,
        .byteSize = 16,
    }));
    EXPECT_FALSE(encodeRealmInputMessage(SRealmInputMessage{
        .type   = eRealmInputMessageType::POINTER_MOVE,
        .x      = 1280,
        .y      = 0,
        .width  = 1280,
        .height = 720,
    }));
    EXPECT_FALSE(encodeRealmInputMessage(SRealmInputMessage{
        .type   = eRealmInputMessageType::POINTER_POINT_AND_CLICK,
        .x      = 10,
        .y      = 10,
        .width  = 1920,
        .height = 1048,
        .code   = 272,
        .count  = 4,
    }));
    EXPECT_FALSE(encodeRealmInputMessage(SRealmInputMessage{
        .type  = eRealmInputMessageType::KEYBOARD_SHORTCUT,
        .codes = {29},
    }));
}

TEST(RealmInputProtocol, acceptsNativeCaptureDimensionsWithinByteLimit) {
    EXPECT_TRUE(encodeRealmInputMessage(SRealmInputMessage{
        .type     = eRealmInputMessageType::CAPTURE_READY,
        .width    = 1920,
        .height   = 1048,
        .format   = REALM_CAPTURE_FORMAT_XRGB8888,
        .stride   = 7680,
        .byteSize = 8048640,
    }));
}

TEST(RealmInputProtocol, classifiesInputAndCaptureCommandsSeparately) {
    EXPECT_TRUE(realmInputMessageIsInputCommand(eRealmInputMessageType::KEYBOARD_TYPE));
    EXPECT_TRUE(realmInputMessageIsInputCommand(eRealmInputMessageType::POINTER_CLICK));
    EXPECT_TRUE(realmInputMessageIsInputCommand(eRealmInputMessageType::KEYBOARD_PRESS));
    EXPECT_TRUE(realmInputMessageIsInputCommand(eRealmInputMessageType::POINTER_POINT_AND_CLICK));
    EXPECT_TRUE(realmInputMessageIsInputCommand(eRealmInputMessageType::KEYBOARD_SHORTCUT));
    EXPECT_FALSE(realmInputMessageIsInputCommand(eRealmInputMessageType::CAPTURE));
    EXPECT_TRUE(realmInputMessageIsCaptureCommand(eRealmInputMessageType::CAPTURE));
    EXPECT_TRUE(realmInputMessageIsCaptureCommand(eRealmInputMessageType::CAPTURE_REGION));
    EXPECT_FALSE(realmInputMessageIsCaptureCommand(eRealmInputMessageType::CAPTURE_READY));
}

TEST(RealmInputProtocol, chargesTypedTextBySize) {
    EXPECT_EQ(realmInputEventCost(SRealmInputMessage{.type = eRealmInputMessageType::POINTER_MOVE}), 1);
    EXPECT_EQ(realmInputEventCost(SRealmInputMessage{.type = eRealmInputMessageType::KEYBOARD_TYPE, .text = "realm"}), 5);
    EXPECT_EQ(realmInputEventCost(SRealmInputMessage{.type = eRealmInputMessageType::KEYBOARD_SHORTCUT, .codes = {29, 42, 20}}), 3);
}
