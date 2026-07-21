#include "MCPServer.hpp"

#include "RealmControlClient.hpp"

#include "../../src/realm/RealmInputProtocol.hpp"

#include <algorithm>
#include <array>
#include <cairo/cairo.h>
#include <cctype>
#include <cstdint>
#include <chrono>
#include <expected>
#include <format>
#include <glaze/glaze.hpp>
#include <iostream>
#include <limits>
#include <linux/input-event-codes.h>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace Realm;
using namespace Realm::MCP;

static constexpr size_t           MAX_MCP_MESSAGE_SIZE = 1024 * 1024;
static constexpr size_t           MAX_PNG_SIZE         = 64 * 1024 * 1024;
static constexpr std::string_view MCP_PROTOCOL_VERSION = "2025-11-25";

struct SMCPRequest {
    std::optional<std::string>   jsonrpc;
    std::optional<glz::raw_json> id;
    std::optional<std::string>   method;
    std::optional<glz::raw_json> params;
};

struct SInitializeParameters {
    std::optional<std::string> protocolVersion;
};

struct SToolCallParameters {
    std::optional<std::string>   name;
    std::optional<glz::raw_json> arguments;
    std::optional<glz::raw_json> _meta;
};

struct SRealmArguments {
    std::optional<std::string> realm;
};

struct SPointerMoveArguments {
    std::optional<std::string> realm;
    std::optional<uint32_t>    x;
    std::optional<uint32_t>    y;
};

struct SPointAndClickArguments {
    std::optional<std::string> realm;
    std::optional<uint32_t>    x;
    std::optional<uint32_t>    y;
    std::optional<std::string> button;
    std::optional<uint32_t>    count;
};

struct SClickArguments {
    std::optional<std::string> realm;
    std::optional<std::string> button;
};

struct SScrollArguments {
    std::optional<std::string> realm;
    std::optional<std::string> axis;
    std::optional<int32_t>     steps;
};

struct SKeyArguments {
    std::optional<std::string> realm;
    std::optional<uint32_t>    keycode;
    std::optional<std::string> key;
};

struct SShortcutArguments {
    std::optional<std::string>              realm;
    std::optional<std::vector<std::string>> modifiers;
    std::optional<std::string>              key;
    std::optional<uint32_t>                 settle_ms;
};

struct STextArguments {
    std::optional<std::string> realm;
    std::optional<std::string> text;
};

struct SCaptureArguments {
    std::optional<std::string> realm;
    std::optional<uint32_t>    x;
    std::optional<uint32_t>    y;
    std::optional<uint32_t>    width;
    std::optional<uint32_t>    height;
    std::optional<bool>        wait_for_change;
    std::optional<uint32_t>    timeout_ms;
    std::optional<uint32_t>    poll_interval_ms;
};

struct SWaitArguments {
    std::optional<uint32_t> duration_ms;
};

struct SLaunchRealmArguments {
    std::optional<std::string> name;
    std::optional<std::string> application;
    std::optional<int64_t>     workspace;
};

struct SLaunchRealmsArguments {
    std::optional<std::vector<SLaunchRealmArguments>> realms;
};

struct SOpenApplicationArguments {
    std::optional<std::string> realm;
    std::optional<std::string> application;
};

struct SLaunchResult {
    std::string name;
    std::string json;
};

struct SRealmDescription {
    std::optional<uint64_t>    id;
    std::optional<std::string> name;
    std::optional<std::string> state;
};

struct SRealmEnvelope {
    std::optional<SRealmDescription> realm;
};

struct SRelaxedJSONOptions : glz::opts {
    bool error_on_unknown_keys        = false;
    bool validate_trailing_whitespace = true;
};

struct SStrictJSONOptions : glz::opts {
    bool validate_trailing_whitespace = true;
};

static std::string quoteJSON(std::string_view value) {
    auto encoded = glz::write_json(std::string{value});
    return encoded ? std::move(encoded.value()) : R"("")";
}

static bool validRealmName(std::string_view name) {
    return !name.empty() && name.size() <= 128 && std::ranges::none_of(name, [](unsigned char character) { return character < 0x20 || character == 0x7F; });
}

static bool validApplicationName(std::string_view application) {
    if (application.empty() || application.size() > 128 || application == "." || application == ".." || application.starts_with('-'))
        return false;
    return std::ranges::all_of(application,
                               [](unsigned char character) { return std::isalnum(character) || character == '-' || character == '_' || character == '.' || character == '+'; });
}

static std::expected<SRealmDescription, std::string> parseRealmDescription(std::string_view result) {
    SRealmEnvelope envelope;
    if (const auto error = glz::read<SRelaxedJSONOptions{}>(envelope, result); error || !envelope.realm || !envelope.realm->name || !envelope.realm->state)
        return std::unexpected("realm control response is missing realm state");
    return *envelope.realm;
}

static std::string_view trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);
    return value;
}

static bool validRequestID(std::string_view id) {
    id = trim(id);
    if (id == "null")
        return true;
    if (!id.empty() && id.front() == '"')
        return true;
    if (id.empty() || (id.front() != '-' && !std::isdigit(static_cast<unsigned char>(id.front()))))
        return false;

    double number = 0;
    return !glz::read<SStrictJSONOptions{}>(number, id);
}

static std::string jsonRPCError(std::string_view id, int code, std::string_view message) {
    return std::format(R"({{"jsonrpc":"2.0","id":{},"error":{{"code":{},"message":{}}}}})", id, code, quoteJSON(message));
}

static std::string jsonRPCResult(std::string_view id, std::string_view result) {
    return std::format(R"({{"jsonrpc":"2.0","id":{},"result":{}}})", id, result);
}

static std::string toolError(std::string_view message) {
    return std::format(R"({{"content":[{{"type":"text","text":{}}}],"isError":true}})", quoteJSON(message));
}

static std::string toolSuccess(std::string_view message, std::string_view structuredContent) {
    return std::format(R"({{"content":[{{"type":"text","text":{}}}],"structuredContent":{},"isError":false}})", quoteJSON(message), structuredContent);
}

template <typename T>
static std::expected<T, std::string> parseArguments(const std::optional<glz::raw_json>& arguments) {
    T           parsed;
    const auto& json = arguments ? arguments->str : std::string{"{}"};
    if (const auto error = glz::read<SStrictJSONOptions{}>(parsed, json); error)
        return std::unexpected("arguments do not match this tool's input schema");
    return parsed;
}

static std::expected<void, std::string> parseNoArguments(const std::optional<glz::raw_json>& arguments) {
    std::map<std::string, glz::raw_json> parsed;
    const auto&                          json = arguments ? arguments->str : std::string{"{}"};
    if (const auto error = glz::read<SStrictJSONOptions{}>(parsed, json); error || !parsed.empty())
        return std::unexpected("this tool does not accept arguments");
    return {};
}

static std::string normalizeKeyName(std::string_view name) {
    std::string normalized;
    normalized.reserve(name.size());
    for (const auto character : name)
        normalized.push_back(character == '-' || character == ' ' ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    return normalized;
}

static std::expected<uint32_t, std::string> keycodeForName(std::string_view name) {
    static const std::map<std::string_view, uint32_t> KEYCODES = {
        {"0", KEY_0},
        {"1", KEY_1},
        {"2", KEY_2},
        {"3", KEY_3},
        {"4", KEY_4},
        {"5", KEY_5},
        {"6", KEY_6},
        {"7", KEY_7},
        {"8", KEY_8},
        {"9", KEY_9},
        {"a", KEY_A},
        {"b", KEY_B},
        {"c", KEY_C},
        {"d", KEY_D},
        {"e", KEY_E},
        {"f", KEY_F},
        {"g", KEY_G},
        {"h", KEY_H},
        {"i", KEY_I},
        {"j", KEY_J},
        {"k", KEY_K},
        {"l", KEY_L},
        {"m", KEY_M},
        {"n", KEY_N},
        {"o", KEY_O},
        {"p", KEY_P},
        {"q", KEY_Q},
        {"r", KEY_R},
        {"s", KEY_S},
        {"t", KEY_T},
        {"u", KEY_U},
        {"v", KEY_V},
        {"w", KEY_W},
        {"x", KEY_X},
        {"y", KEY_Y},
        {"z", KEY_Z},
        {"enter", KEY_ENTER},
        {"return", KEY_ENTER},
        {"tab", KEY_TAB},
        {"escape", KEY_ESC},
        {"esc", KEY_ESC},
        {"space", KEY_SPACE},
        {"backspace", KEY_BACKSPACE},
        {"delete", KEY_DELETE},
        {"insert", KEY_INSERT},
        {"home", KEY_HOME},
        {"end", KEY_END},
        {"page_up", KEY_PAGEUP},
        {"page_down", KEY_PAGEDOWN},
        {"left", KEY_LEFT},
        {"right", KEY_RIGHT},
        {"up", KEY_UP},
        {"down", KEY_DOWN},
        {"caps_lock", KEY_CAPSLOCK},
        {"minus", KEY_MINUS},
        {"equal", KEY_EQUAL},
        {"comma", KEY_COMMA},
        {"period", KEY_DOT},
        {"dot", KEY_DOT},
        {"slash", KEY_SLASH},
        {"semicolon", KEY_SEMICOLON},
        {"apostrophe", KEY_APOSTROPHE},
        {"left_bracket", KEY_LEFTBRACE},
        {"right_bracket", KEY_RIGHTBRACE},
        {"backslash", KEY_BACKSLASH},
        {"grave", KEY_GRAVE},
        {"f1", KEY_F1},
        {"f2", KEY_F2},
        {"f3", KEY_F3},
        {"f4", KEY_F4},
        {"f5", KEY_F5},
        {"f6", KEY_F6},
        {"f7", KEY_F7},
        {"f8", KEY_F8},
        {"f9", KEY_F9},
        {"f10", KEY_F10},
        {"f11", KEY_F11},
        {"f12", KEY_F12},
    };

    const auto normalized = normalizeKeyName(name);
    if (const auto keycode = KEYCODES.find(normalized); keycode != KEYCODES.end())
        return keycode->second;
    return std::unexpected(std::format("unsupported key name '{}'; use a documented key name or raw keycode", name));
}

static std::expected<uint32_t, std::string> modifierKeycode(std::string_view modifier) {
    const auto normalized = normalizeKeyName(modifier);
    if (normalized == "ctrl" || normalized == "control")
        return KEY_LEFTCTRL;
    if (normalized == "alt")
        return KEY_LEFTALT;
    if (normalized == "shift")
        return KEY_LEFTSHIFT;
    if (normalized == "meta" || normalized == "super")
        return KEY_LEFTMETA;
    return std::unexpected(std::format("unsupported modifier '{}'", modifier));
}

static uint64_t captureHash(const SCaptureFrame& frame) {
    uint64_t   hash = 1469598103934665603ULL;
    const auto mix  = [&hash](uint8_t value) {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    for (uint32_t shift = 0; shift < 32; shift += 8) {
        mix(static_cast<uint8_t>(frame.format >> shift));
        mix(static_cast<uint8_t>(frame.width >> shift));
        mix(static_cast<uint8_t>(frame.height >> shift));
    }
    mix(frame.yInverted);
    const auto rowBytes = static_cast<size_t>(frame.width) * 4;
    for (uint32_t row = 0; row < frame.height; ++row) {
        const auto begin = frame.pixels.begin() + static_cast<size_t>(row) * frame.stride;
        for (const auto pixel : std::span{begin, rowBytes})
            mix(pixel);
    }
    return hash;
}

static constexpr std::string_view BOUND_TOOLS_RESULT = R"json({"tools":[
  {"name":"realm_info","description":"Inspect the realm bound to this MCP server, including state, ownership, observation permission, and host-granted capabilities.","inputSchema":{"type":"object","properties":{},"additionalProperties":false},"annotations":{"title":"Inspect bound realm","readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
  {"name":"realm_create","description":"Create the realm bound to this MCP server. It cannot create or name any other realm.","inputSchema":{"type":"object","properties":{},"additionalProperties":false},"annotations":{"title":"Create bound realm","readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}},
  {"name":"realm_start","description":"Start the realm bound to this MCP server.","inputSchema":{"type":"object","properties":{},"additionalProperties":false},"annotations":{"title":"Start bound realm","readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}},
  {"name":"realm_pause","description":"Pause the realm bound to this MCP server.","inputSchema":{"type":"object","properties":{},"additionalProperties":false},"annotations":{"title":"Pause bound realm","readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}},
  {"name":"realm_resume","description":"Resume the realm bound to this MCP server.","inputSchema":{"type":"object","properties":{},"additionalProperties":false},"annotations":{"title":"Resume bound realm","readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}},
  {"name":"realm_stop","description":"Stop the realm bound to this MCP server without destroying its record.","inputSchema":{"type":"object","properties":{},"additionalProperties":false},"annotations":{"title":"Stop bound realm","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
  {"name":"enable_observation","description":"Enable runtime observation for the bound realm. The host must have granted its observe capability first.","inputSchema":{"type":"object","properties":{},"additionalProperties":false},"annotations":{"title":"Enable realm observation","readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}},
  {"name":"disable_observation","description":"Disable runtime observation for the bound realm and cancel pending captures.","inputSchema":{"type":"object","properties":{},"additionalProperties":false},"annotations":{"title":"Disable realm observation","readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}},
  {"name":"capture_realm","description":"Capture the complete bound realm at its native dimensions without supplying a size. The returned width and height define the exact coordinate space used by subsequent pointer tools. Observation must already be enabled. Optional x, y, width, and height crop the native image; wait_for_change waits for a frame different from the last capture.","inputSchema":{"type":"object","properties":{"x":{"type":"integer","minimum":0,"maximum":16383},"y":{"type":"integer","minimum":0,"maximum":16383},"width":{"type":"integer","minimum":1,"maximum":16384},"height":{"type":"integer","minimum":1,"maximum":16384},"wait_for_change":{"type":"boolean","default":false},"timeout_ms":{"type":"integer","minimum":100,"maximum":30000,"default":5000},"poll_interval_ms":{"type":"integer","minimum":100,"maximum":2000,"default":100}},"additionalProperties":false},"annotations":{"title":"Capture bound realm","readOnlyHint":true,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}},
  {"name":"move_pointer","description":"Move the virtual pointer in the native coordinate space returned by the latest capture_realm call. Before the first capture, the compatibility coordinate space is 1280x720.","inputSchema":{"type":"object","properties":{"x":{"type":"integer","minimum":0,"maximum":16383},"y":{"type":"integer","minimum":0,"maximum":16383}},"required":["x","y"],"additionalProperties":false},"annotations":{"title":"Move realm pointer","readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
  {"name":"point_and_click","description":"Atomically move to a coordinate and click after the compositor receives the motion. Coordinates use the latest capture_realm dimensions. Prefer this over separate move_pointer and click calls.","inputSchema":{"type":"object","properties":{"x":{"type":"integer","minimum":0,"maximum":16383},"y":{"type":"integer","minimum":0,"maximum":16383},"button":{"type":"string","enum":["left","right","middle"],"default":"left"},"count":{"type":"integer","minimum":1,"maximum":3,"default":1}},"required":["x","y"],"additionalProperties":false},"annotations":{"title":"Point and click in realm","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
  {"name":"click","description":"Press and release a virtual pointer button inside the bound realm.","inputSchema":{"type":"object","properties":{"button":{"type":"string","enum":["left","right","middle"]}},"required":["button"],"additionalProperties":false},"annotations":{"title":"Click in realm","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
  {"name":"scroll","description":"Scroll the virtual pointer inside the bound realm by a bounded number of steps.","inputSchema":{"type":"object","properties":{"axis":{"type":"string","enum":["horizontal","vertical"]},"steps":{"type":"integer","minimum":-20,"maximum":20,"not":{"const":0}}},"required":["axis","steps"],"additionalProperties":false},"annotations":{"title":"Scroll in realm","readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}},
  {"name":"press_key","description":"Press and release one named key inside the bound realm. Prefer names such as enter, tab, escape, left, page_down, a, or f5; raw Linux evdev keycode remains available for compatibility. Supply exactly one of key or keycode.","inputSchema":{"type":"object","properties":{"key":{"type":"string","minLength":1,"maxLength":32},"keycode":{"type":"integer","minimum":0,"maximum":767}},"additionalProperties":false},"annotations":{"title":"Press key in realm","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
  {"name":"press_shortcut","description":"Atomically press a named key with one or more modifiers inside the bound realm, for example modifiers [ctrl] with key t. After compositor delivery, a bounded grace interval reduces races with application focus changes; settle_ms defaults to 400 and 0 disables it.","inputSchema":{"type":"object","properties":{"modifiers":{"type":"array","minItems":1,"maxItems":4,"uniqueItems":true,"items":{"type":"string","enum":["ctrl","alt","shift","meta"]}},"key":{"type":"string","minLength":1,"maxLength":32},"settle_ms":{"type":"integer","minimum":0,"maximum":2000,"default":400}},"required":["modifiers","key"],"additionalProperties":false},"annotations":{"title":"Press shortcut in realm","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
  {"name":"type_text","description":"Type UTF-8 text inside the bound realm, up to 4096 bytes.","inputSchema":{"type":"object","properties":{"text":{"type":"string","minLength":1,"maxLength":4096}},"required":["text"],"additionalProperties":false},"annotations":{"title":"Type text in realm","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
  {"name":"wait","description":"Wait for a bounded duration within the realm workflow so an application can finish navigation or animation. Prefer capture_realm with wait_for_change when a visual transition is expected.","inputSchema":{"type":"object","properties":{"duration_ms":{"type":"integer","minimum":0,"maximum":30000}},"required":["duration_ms"],"additionalProperties":false},"annotations":{"title":"Wait in realm workflow","readOnlyHint":true,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}}
]})json";

static constexpr std::string_view ORCHESTRATOR_TOOLS_RESULT = R"json({"tools":[
  {"name":"list_realms","description":"List all realms on this compositor so temporary workspaces can be inspected and reused deliberately.","inputSchema":{"type":"object","properties":{},"additionalProperties":false},"annotations":{"title":"List realms","readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
  {"name":"launch_realm","description":"Create a temporary agent realm, place its host window on a numeric Hyprland workspace, grant observe/pointer/keyboard, start it, enable observation, and directly execute one application without a shell.","inputSchema":{"type":"object","properties":{"name":{"type":"string","minLength":1,"maxLength":128},"application":{"type":"string","minLength":1,"maxLength":128,"pattern":"^[A-Za-z0-9][A-Za-z0-9._+-]*$"},"workspace":{"type":"integer","minimum":1,"maximum":999999}},"required":["application","workspace"],"additionalProperties":false},"annotations":{"title":"Launch temporary realm","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":true}},
  {"name":"launch_realms","description":"Launch between one and eight independent temporary agent realms. Each realm receives its own application and host workspace and can be operated by name after this call.","inputSchema":{"type":"object","properties":{"realms":{"type":"array","minItems":1,"maxItems":8,"items":{"type":"object","properties":{"name":{"type":"string","minLength":1,"maxLength":128},"application":{"type":"string","minLength":1,"maxLength":128,"pattern":"^[A-Za-z0-9][A-Za-z0-9._+-]*$"},"workspace":{"type":"integer","minimum":1,"maximum":999999}},"required":["application","workspace"],"additionalProperties":false}}},"required":["realms"],"additionalProperties":false},"annotations":{"title":"Launch multiple temporary realms","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":true}},
  {"name":"open_application","description":"Directly execute another application in an existing running realm without invoking a shell.","inputSchema":{"type":"object","properties":{"realm":{"type":"string","minLength":1,"maxLength":128},"application":{"type":"string","minLength":1,"maxLength":128,"pattern":"^[A-Za-z0-9][A-Za-z0-9._+-]*$"}},"required":["realm","application"],"additionalProperties":false},"annotations":{"title":"Open application in realm","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":true}},
  {"name":"finish_realm","description":"Stop a temporary realm, wait for compositor cleanup, destroy its record, and remove its runtime directory.","inputSchema":{"type":"object","properties":{"realm":{"type":"string","minLength":1,"maxLength":128}},"required":["realm"],"additionalProperties":false},"annotations":{"title":"Finish temporary realm","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
  {"name":"realm_info","description":"Inspect a realm's state, input ownership, observation permission, and capabilities.","inputSchema":{"type":"object","properties":{"realm":{"type":"string","minLength":1,"maxLength":128}},"required":["realm"],"additionalProperties":false},"annotations":{"title":"Inspect realm","readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
  {"name":"realm_pause","description":"Pause a running realm and all applications in its process group.","inputSchema":{"type":"object","properties":{"realm":{"type":"string","minLength":1,"maxLength":128}},"required":["realm"],"additionalProperties":false},"annotations":{"title":"Pause realm","readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}},
  {"name":"realm_resume","description":"Resume a paused realm and return input ownership to the agent.","inputSchema":{"type":"object","properties":{"realm":{"type":"string","minLength":1,"maxLength":128}},"required":["realm"],"additionalProperties":false},"annotations":{"title":"Resume realm","readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}},
  {"name":"capture_realm","description":"Capture a named realm at native dimensions. Optional x/y/width/height crop the image; wait_for_change polls for a visually different frame.","inputSchema":{"type":"object","properties":{"realm":{"type":"string","minLength":1,"maxLength":128},"x":{"type":"integer","minimum":0,"maximum":16383},"y":{"type":"integer","minimum":0,"maximum":16383},"width":{"type":"integer","minimum":1,"maximum":16384},"height":{"type":"integer","minimum":1,"maximum":16384},"wait_for_change":{"type":"boolean","default":false},"timeout_ms":{"type":"integer","minimum":100,"maximum":30000,"default":5000},"poll_interval_ms":{"type":"integer","minimum":100,"maximum":2000,"default":100}},"required":["realm"],"additionalProperties":false},"annotations":{"title":"Capture realm","readOnlyHint":true,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}},
  {"name":"move_pointer","description":"Move the virtual pointer in a named realm using its latest capture coordinate space.","inputSchema":{"type":"object","properties":{"realm":{"type":"string","minLength":1,"maxLength":128},"x":{"type":"integer","minimum":0,"maximum":16383},"y":{"type":"integer","minimum":0,"maximum":16383}},"required":["realm","x","y"],"additionalProperties":false},"annotations":{"title":"Move realm pointer","readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
  {"name":"point_and_click","description":"Atomically move and click in a named realm using its latest capture coordinate space.","inputSchema":{"type":"object","properties":{"realm":{"type":"string","minLength":1,"maxLength":128},"x":{"type":"integer","minimum":0,"maximum":16383},"y":{"type":"integer","minimum":0,"maximum":16383},"button":{"type":"string","enum":["left","right","middle"],"default":"left"},"count":{"type":"integer","minimum":1,"maximum":3,"default":1}},"required":["realm","x","y"],"additionalProperties":false},"annotations":{"title":"Point and click in realm","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
  {"name":"click","description":"Press and release a pointer button in a named realm.","inputSchema":{"type":"object","properties":{"realm":{"type":"string","minLength":1,"maxLength":128},"button":{"type":"string","enum":["left","right","middle"]}},"required":["realm","button"],"additionalProperties":false},"annotations":{"title":"Click in realm","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
  {"name":"scroll","description":"Scroll the virtual pointer in a named realm.","inputSchema":{"type":"object","properties":{"realm":{"type":"string","minLength":1,"maxLength":128},"axis":{"type":"string","enum":["horizontal","vertical"]},"steps":{"type":"integer","minimum":-20,"maximum":20,"not":{"const":0}}},"required":["realm","axis","steps"],"additionalProperties":false},"annotations":{"title":"Scroll in realm","readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}},
  {"name":"press_key","description":"Press and release one named key or raw evdev keycode in a named realm.","inputSchema":{"type":"object","properties":{"realm":{"type":"string","minLength":1,"maxLength":128},"key":{"type":"string","minLength":1,"maxLength":32},"keycode":{"type":"integer","minimum":0,"maximum":767}},"required":["realm"],"additionalProperties":false},"annotations":{"title":"Press key in realm","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
  {"name":"press_shortcut","description":"Atomically press a named key with modifiers in a named realm. After compositor delivery, a bounded grace interval reduces races with application focus changes; settle_ms defaults to 400 and 0 disables it.","inputSchema":{"type":"object","properties":{"realm":{"type":"string","minLength":1,"maxLength":128},"modifiers":{"type":"array","minItems":1,"maxItems":4,"uniqueItems":true,"items":{"type":"string","enum":["ctrl","alt","shift","meta"]}},"key":{"type":"string","minLength":1,"maxLength":32},"settle_ms":{"type":"integer","minimum":0,"maximum":2000,"default":400}},"required":["realm","modifiers","key"],"additionalProperties":false},"annotations":{"title":"Press shortcut in realm","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
  {"name":"type_text","description":"Type up to 4096 bytes of UTF-8 text in a named realm.","inputSchema":{"type":"object","properties":{"realm":{"type":"string","minLength":1,"maxLength":128},"text":{"type":"string","minLength":1,"maxLength":4096}},"required":["realm","text"],"additionalProperties":false},"annotations":{"title":"Type text in realm","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
  {"name":"wait","description":"Wait for bounded application navigation or animation time. Prefer capture_realm with wait_for_change for visual transitions.","inputSchema":{"type":"object","properties":{"duration_ms":{"type":"integer","minimum":0,"maximum":30000}},"required":["duration_ms"],"additionalProperties":false},"annotations":{"title":"Wait in realm workflow","readOnlyHint":true,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}}
]})json";

static const std::string&         toolsResult(bool bound) {
    static const auto compact = [](std::string_view source) {
        std::string result{source};
        std::erase(result, '\n');
        std::erase(result, '\r');
        return result;
    };
    static const auto boundResult        = compact(BOUND_TOOLS_RESULT);
    static const auto orchestratorResult = compact(ORCHESTRATOR_TOOLS_RESULT);
    return bound ? boundResult : orchestratorResult;
}

static std::expected<std::vector<uint8_t>, std::string> encodePNG(const SCaptureFrame& frame, std::optional<SCaptureRegion> region) {
    if (frame.width > static_cast<uint32_t>(std::numeric_limits<int>::max()) || frame.height > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        frame.stride > static_cast<uint32_t>(std::numeric_limits<int>::max()))
        return std::unexpected("capture dimensions cannot be encoded as PNG");

    auto pixels = frame.pixels;
    if (frame.yInverted) {
        std::vector<uint8_t> row(frame.stride);
        for (uint32_t top = 0, bottom = frame.height - 1; top < bottom; ++top, --bottom) {
            auto topRow    = pixels.begin() + static_cast<ptrdiff_t>(top) * frame.stride;
            auto bottomRow = pixels.begin() + static_cast<ptrdiff_t>(bottom) * frame.stride;
            std::copy_n(topRow, frame.stride, row.begin());
            std::copy_n(bottomRow, frame.stride, topRow);
            std::copy_n(row.begin(), frame.stride, bottomRow);
        }
    }

    cairo_surface_t* source = cairo_image_surface_create_for_data(pixels.data(), frame.format == 0 ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24, static_cast<int>(frame.width),
                                                                  static_cast<int>(frame.height), static_cast<int>(frame.stride));
    if (!source)
        return std::unexpected("failed creating realm capture source surface");
    if (const auto status = cairo_surface_status(source); status != CAIRO_STATUS_SUCCESS) {
        const auto message = std::format("failed creating realm capture source surface: {}", cairo_status_to_string(status));
        cairo_surface_destroy(source);
        return std::unexpected(message);
    }

    const auto       outputWidth  = region ? region->width : frame.width;
    const auto       outputHeight = region ? region->height : frame.height;
    cairo_surface_t* output       = cairo_image_surface_create(CAIRO_FORMAT_RGB24, static_cast<int>(outputWidth), static_cast<int>(outputHeight));
    if (!output || cairo_surface_status(output) != CAIRO_STATUS_SUCCESS) {
        const auto message = std::format("failed creating realm capture surface: {}", output ? cairo_status_to_string(cairo_surface_status(output)) : "out of memory");
        if (output)
            cairo_surface_destroy(output);
        cairo_surface_destroy(source);
        return std::unexpected(message);
    }

    cairo_surface_mark_dirty(source);
    cairo_t* context = cairo_create(output);
    if (!context || cairo_status(context) != CAIRO_STATUS_SUCCESS) {
        const auto message = std::format("failed creating realm capture context: {}", context ? cairo_status_to_string(cairo_status(context)) : "out of memory");
        if (context)
            cairo_destroy(context);
        cairo_surface_destroy(output);
        cairo_surface_destroy(source);
        return std::unexpected(message);
    }

    cairo_translate(context, -static_cast<double>(region ? region->x : 0), -static_cast<double>(region ? region->y : 0));
    cairo_set_source_surface(context, source, 0, 0);
    cairo_paint(context);
    const auto paintStatus = cairo_status(context);
    cairo_destroy(context);
    cairo_surface_destroy(source);
    if (paintStatus != CAIRO_STATUS_SUCCESS) {
        const auto message = std::format("failed rendering realm capture: {}", cairo_status_to_string(paintStatus));
        cairo_surface_destroy(output);
        return std::unexpected(message);
    }

    std::vector<uint8_t> png;
    const auto           writePNG = [](void* closure, const unsigned char* data, unsigned int size) {
        auto& output = *static_cast<std::vector<uint8_t>*>(closure);
        if (output.size() > MAX_PNG_SIZE || size > MAX_PNG_SIZE - output.size())
            return CAIRO_STATUS_WRITE_ERROR;
        output.insert(output.end(), data, data + size);
        return CAIRO_STATUS_SUCCESS;
    };
    cairo_surface_flush(output);
    const auto status = cairo_surface_write_to_png_stream(output, writePNG, &png);
    cairo_surface_destroy(output);
    if (status != CAIRO_STATUS_SUCCESS)
        return std::unexpected(std::format("failed encoding realm capture as PNG: {}", cairo_status_to_string(status)));
    return png;
}

static std::string base64Encode(const std::vector<uint8_t>& input) {
    static constexpr std::string_view ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string                       output;
    output.reserve(((input.size() + 2) / 3) * 4);
    for (size_t index = 0; index < input.size(); index += 3) {
        const uint32_t first  = input[index];
        const uint32_t second = index + 1 < input.size() ? input[index + 1] : 0;
        const uint32_t third  = index + 2 < input.size() ? input[index + 2] : 0;
        const uint32_t value  = (first << 16) | (second << 8) | third;
        output.push_back(ALPHABET[(value >> 18) & 0x3F]);
        output.push_back(ALPHABET[(value >> 12) & 0x3F]);
        output.push_back(index + 1 < input.size() ? ALPHABET[(value >> 6) & 0x3F] : '=');
        output.push_back(index + 2 < input.size() ? ALPHABET[value & 0x3F] : '=');
    }
    return output;
}

CMCPServer::CMCPServer(CRealmControlClient& controlClient, std::optional<std::string> boundRealm) : m_controlClient(controlClient), m_boundRealm(std::move(boundRealm)) {}

int CMCPServer::run() {
    std::string line;
    bool        oversized = false;
    char        character = '\0';
    while (std::cin.get(character)) {
        if (character != '\n') {
            if (line.size() < MAX_MCP_MESSAGE_SIZE)
                line.push_back(character);
            else
                oversized = true;
            continue;
        }

        if (oversized)
            std::cout << jsonRPCError("null", -32700, "MCP message exceeds the 1 MiB limit") << '\n' << std::flush;
        else if (!trim(line).empty()) {
            if (auto response = handleMessage(line); response)
                std::cout << *response << '\n' << std::flush;
        }
        line.clear();
        oversized = false;
    }

    if (oversized)
        std::cout << jsonRPCError("null", -32700, "MCP message exceeds the 1 MiB limit") << '\n' << std::flush;
    else if (!trim(line).empty()) {
        if (auto response = handleMessage(line); response)
            std::cout << *response << '\n' << std::flush;
    }
    return std::cin.bad() ? 1 : 0;
}

std::optional<std::string> CMCPServer::handleMessage(std::string_view message) {
    SMCPRequest request;
    if (const auto error = glz::read<SRelaxedJSONOptions{}>(request, message); error)
        return jsonRPCError("null", -32700, "invalid JSON");
    if (!request.jsonrpc || *request.jsonrpc != "2.0" || !request.method || request.method->empty())
        return jsonRPCError(request.id && validRequestID(request.id->str) ? request.id->str : "null", -32600, "invalid JSON-RPC request");
    if (request.id && !validRequestID(request.id->str))
        return jsonRPCError("null", -32600, "JSON-RPC id must be a string, number, or null");

    const bool isNotification = !request.id;
    const auto id             = request.id ? request.id->str : std::string{"null"};
    if (*request.method == "notifications/initialized" || *request.method == "notifications/cancelled")
        return std::nullopt;
    if (*request.method == "ping")
        return isNotification ? std::nullopt : std::optional{jsonRPCResult(id, "{}")};
    if (*request.method == "initialize") {
        if (isNotification)
            return std::nullopt;
        if (m_initialized)
            return jsonRPCError(id, -32600, "server is already initialized");

        SInitializeParameters parameters;
        if (request.params) {
            if (const auto error = glz::read<SRelaxedJSONOptions{}>(parameters, request.params->str); error)
                return jsonRPCError(id, -32602, "invalid initialize parameters");
        }
        constexpr std::array<std::string_view, 4> SUPPORTED_VERSIONS = {"2024-11-05", "2025-03-26", "2025-06-18", MCP_PROTOCOL_VERSION};
        const auto                                requestedVersion   = parameters.protocolVersion.value_or(std::string{MCP_PROTOCOL_VERSION});
        const auto protocolVersion = std::ranges::find(SUPPORTED_VERSIONS, requestedVersion) != SUPPORTED_VERSIONS.end() ? requestedVersion : std::string{MCP_PROTOCOL_VERSION};
        m_initialized              = true;
        const auto instructions    = m_boundRealm ?
               std::format("This server is permanently bound to realm '{}'. Host-granted capabilities remain authoritative. Call capture_realm without dimensions immediately before "
                              "coordinate input, prefer point_and_click, and capture again with wait_for_change after visible actions.",
                           *m_boundRealm) :
               std::string{
                "This is the Agent Realms demo orchestrator. It may create temporary realms, grant observe/pointer/keyboard, launch direct executables, and place realm windows on "
                   "numeric host workspaces. Use launch_realm or launch_realms, address later computer-use calls by realm name, and call finish_realm when work is complete. These "
                   "demo realms "
                   "are same-user process separation, not filesystem or network sandboxes."};
        const auto result          = std::format(
            R"({{"protocolVersion":{},"capabilities":{{"tools":{{"listChanged":false}}}},"serverInfo":{{"name":"hyprland-realm","version":"0.1.0"}},"instructions":{}}})",
            quoteJSON(protocolVersion), quoteJSON(instructions));
        return jsonRPCResult(id, result);
    }
    if (isNotification)
        return std::nullopt;
    if (!m_initialized)
        return jsonRPCError(id, -32002, "server is not initialized");
    if (*request.method == "tools/list")
        return jsonRPCResult(id, toolsResult(m_boundRealm.has_value()));
    if (*request.method == "tools/call") {
        if (!request.params)
            return jsonRPCError(id, -32602, "tools/call requires parameters");
        return jsonRPCResult(id, callTool(request.params->str));
    }
    return jsonRPCError(id, -32601, std::format("method '{}' was not found", *request.method));
}

std::string CMCPServer::callTool(std::string_view parametersJSON) {
    SToolCallParameters parameters;
    if (const auto error = glz::read<SRelaxedJSONOptions{}>(parameters, parametersJSON); error || !parameters.name || parameters.name->empty())
        return toolError("tools/call requires a tool name and valid arguments");

    const auto controlResult = [](std::expected<std::string, std::string> result, std::string_view success) {
        return result ? toolSuccess(success, *result) : toolError(result.error());
    };
    const auto resolveRealm = [this](const std::optional<std::string>& requested) -> std::expected<std::string, std::string> {
        if (m_boundRealm) {
            if (requested)
                return std::unexpected("this bound MCP tool does not accept a realm argument");
            return *m_boundRealm;
        }
        if (!requested || !validRealmName(*requested))
            return std::unexpected("realm must be a non-empty name of at most 128 bytes without control characters");
        return *requested;
    };
    const auto realmArguments = [&]() -> std::expected<std::string, std::string> {
        auto arguments = parseArguments<SRealmArguments>(parameters.arguments);
        if (!arguments)
            return std::unexpected(arguments.error());
        return resolveRealm(arguments->realm);
    };

    if (*parameters.name == "list_realms") {
        if (m_boundRealm)
            return toolError("unknown tool 'list_realms'");
        if (auto parsed = parseNoArguments(parameters.arguments); !parsed)
            return toolError(parsed.error());
        return controlResult(m_controlClient.listRealms(), "Realm list retrieved.");
    }

    const auto cleanupRealm = [this](std::string_view realm) {
        auto info = m_controlClient.realmInfo(realm);
        if (!info)
            return;
        auto description = parseRealmDescription(*info);
        if (!description)
            return;
        if (*description->state == "creating" || *description->state == "running" || *description->state == "paused")
            m_controlClient.stopRealm(realm);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            info = m_controlClient.realmInfo(realm);
            if (!info)
                return;
            description = parseRealmDescription(*info);
            if (!description)
                return;
            if (*description->state == "stopped" || *description->state == "failed") {
                if (m_controlClient.destroyRealm(realm))
                    return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    };

    const auto launchRealm = [this, &cleanupRealm](const SLaunchRealmArguments& arguments) -> std::expected<SLaunchResult, std::string> {
        if (!arguments.application || !validApplicationName(*arguments.application) || !arguments.workspace || *arguments.workspace <= 0 || *arguments.workspace > 999999)
            return std::unexpected("launch requires a validated executable name and workspace between 1 and 999999");

        static uint64_t generatedRealmSequence = 1;
        const auto      generated              = std::format("agent-{}-{}", getpid(), generatedRealmSequence++);
        const auto      realm                  = arguments.name.value_or(generated);
        if (!validRealmName(realm))
            return std::unexpected("realm name must contain 1 to 128 bytes without control characters");

        auto created = m_controlClient.createRealm(realm);
        if (!created)
            return std::unexpected(created.error());
        const auto fail = [&cleanupRealm, &realm](std::string message) -> std::expected<SLaunchResult, std::string> {
            cleanupRealm(realm);
            return std::unexpected(std::move(message));
        };

        if (auto placed = m_controlClient.placeRealm(realm, *arguments.workspace); !placed)
            return fail(placed.error());
        for (const std::string_view capability : {"observe", "pointer", "keyboard"}) {
            if (auto granted = m_controlClient.grantCapability(realm, capability); !granted)
                return fail(granted.error());
        }
        if (auto started = m_controlClient.startRealm(realm); !started)
            return fail(started.error());

        std::optional<std::string> runningInfo;
        const auto                 deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            auto info = m_controlClient.realmInfo(realm);
            if (!info)
                return fail(info.error());
            auto description = parseRealmDescription(*info);
            if (!description)
                return fail(description.error());
            if (*description->state == "running") {
                runningInfo = std::move(*info);
                break;
            }
            if (*description->state == "failed" || *description->state == "stopped")
                return fail(std::format("realm '{}' entered {} during startup", realm, *description->state));
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!runningInfo)
            return fail(std::format("realm '{}' did not reach running state within 10 seconds", realm));
        if (auto observed = m_controlClient.allowObservation(realm); !observed)
            return fail(observed.error());

        auto opened = m_controlClient.openApplication(realm, *arguments.application);
        if (!opened)
            return fail(opened.error());
        m_realmStates.try_emplace(realm);
        return SLaunchResult{
            .name = realm,
            .json = std::format(R"({{"temporary":true,"name":{},"application":{},"workspace":{},"realm_info":{},"application_result":{}}})", quoteJSON(realm),
                                quoteJSON(*arguments.application), *arguments.workspace, *runningInfo, *opened),
        };
    };

    if (*parameters.name == "launch_realm") {
        if (m_boundRealm)
            return toolError("unknown tool 'launch_realm'");
        auto arguments = parseArguments<SLaunchRealmArguments>(parameters.arguments);
        if (!arguments)
            return toolError(arguments.error());
        auto launched = launchRealm(*arguments);
        return launched ?
            toolSuccess(std::format("Temporary realm '{}' launched on workspace {} with {}.", launched->name, *arguments->workspace, *arguments->application), launched->json) :
            toolError(launched.error());
    }
    if (*parameters.name == "launch_realms") {
        if (m_boundRealm)
            return toolError("unknown tool 'launch_realms'");
        auto arguments = parseArguments<SLaunchRealmsArguments>(parameters.arguments);
        if (!arguments || !arguments->realms || arguments->realms->empty() || arguments->realms->size() > 8)
            return toolError(arguments ? "launch_realms requires between 1 and 8 realm specifications" : arguments.error());

        std::set<std::string, std::less<>> requestedNames;
        for (const auto& specification : *arguments->realms) {
            if (!specification.application || !validApplicationName(*specification.application) || !specification.workspace || *specification.workspace <= 0 ||
                *specification.workspace > 999999 || (specification.name && !validRealmName(*specification.name)))
                return toolError("every launch specification requires a valid executable, workspace, and optional realm name");
            if (specification.name && !requestedNames.emplace(*specification.name).second)
                return toolError(std::format("realm name '{}' is duplicated in launch_realms", *specification.name));
        }

        std::string launchedResults;
        size_t      launchedCount = 0;
        for (const auto& specification : *arguments->realms) {
            auto launched = launchRealm(specification);
            if (!launched)
                return toolError(std::format("one realm failed to launch after {} successful launch(es): {}", launchedCount, launched.error()));
            if (!launchedResults.empty())
                launchedResults += ',';
            launchedResults += launched->json;
            ++launchedCount;
        }
        return toolSuccess(std::format("Launched {} temporary realms.", arguments->realms->size()), std::format(R"({{"realms":[{}]}})", launchedResults));
    }
    if (*parameters.name == "open_application") {
        if (m_boundRealm)
            return toolError("unknown tool 'open_application'");
        auto arguments = parseArguments<SOpenApplicationArguments>(parameters.arguments);
        if (!arguments || !arguments->realm || !validRealmName(*arguments->realm) || !arguments->application || !validApplicationName(*arguments->application))
            return toolError(arguments ? "open_application requires a valid realm and direct executable name" : arguments.error());
        return controlResult(m_controlClient.openApplication(*arguments->realm, *arguments->application),
                             std::format("Application '{}' opened in realm '{}'.", *arguments->application, *arguments->realm));
    }
    if (*parameters.name == "finish_realm") {
        if (m_boundRealm)
            return toolError("unknown tool 'finish_realm'");
        auto realm = realmArguments();
        if (!realm)
            return toolError(realm.error());

        auto info = m_controlClient.realmInfo(*realm);
        if (!info)
            return toolError(info.error());
        auto description = parseRealmDescription(*info);
        if (!description)
            return toolError(description.error());
        if (*description->state == "creating" || *description->state == "running" || *description->state == "paused") {
            if (auto stopped = m_controlClient.stopRealm(*realm); !stopped)
                return toolError(stopped.error());
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            info = m_controlClient.realmInfo(*realm);
            if (!info)
                return toolError(info.error());
            description = parseRealmDescription(*info);
            if (!description)
                return toolError(description.error());
            if (*description->state == "stopped" || *description->state == "failed") {
                auto destroyed = m_controlClient.destroyRealm(*realm);
                if (destroyed) {
                    m_realmStates.erase(*realm);
                    return toolSuccess(std::format("Realm '{}' finished and its temporary runtime was removed.", *realm),
                                       std::format(R"({{"action":"finished","realm":{}}})", quoteJSON(*realm)));
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return toolError(std::format("realm '{}' did not finish cleanup within 10 seconds", *realm));
    }

    if (*parameters.name == "realm_info") {
        auto realm = realmArguments();
        if (!realm)
            return toolError(realm.error());
        return controlResult(m_controlClient.realmInfo(*realm), std::format("Realm '{}' information retrieved.", *realm));
    }
    if (*parameters.name == "realm_create") {
        if (!m_boundRealm)
            return toolError("unknown tool 'realm_create'");
        auto realm = realmArguments();
        if (!realm)
            return toolError(realm.error());
        return controlResult(m_controlClient.createRealm(*realm), "Bound realm created.");
    }
    if (*parameters.name == "realm_start") {
        if (!m_boundRealm)
            return toolError("unknown tool 'realm_start'");
        auto realm = realmArguments();
        if (!realm)
            return toolError(realm.error());
        return controlResult(m_controlClient.startRealm(*realm), "Bound realm is starting.");
    }
    if (*parameters.name == "realm_pause") {
        auto realm = realmArguments();
        if (!realm)
            return toolError(realm.error());
        return controlResult(m_controlClient.pauseRealm(*realm), std::format("Realm '{}' paused.", *realm));
    }
    if (*parameters.name == "realm_resume") {
        auto realm = realmArguments();
        if (!realm)
            return toolError(realm.error());
        return controlResult(m_controlClient.resumeRealm(*realm), std::format("Realm '{}' resumed.", *realm));
    }
    if (*parameters.name == "realm_stop") {
        if (!m_boundRealm)
            return toolError("unknown tool 'realm_stop'");
        auto realm = realmArguments();
        if (!realm)
            return toolError(realm.error());
        return controlResult(m_controlClient.stopRealm(*realm), "Bound realm is stopping.");
    }
    if (*parameters.name == "enable_observation") {
        if (!m_boundRealm)
            return toolError("unknown tool 'enable_observation'");
        auto realm = realmArguments();
        if (!realm)
            return toolError(realm.error());
        return controlResult(m_controlClient.allowObservation(*realm), "Observation enabled for the bound realm.");
    }
    if (*parameters.name == "disable_observation") {
        if (!m_boundRealm)
            return toolError("unknown tool 'disable_observation'");
        auto realm = realmArguments();
        if (!realm)
            return toolError(realm.error());
        return controlResult(m_controlClient.denyObservation(*realm), "Observation disabled for the bound realm.");
    }
    if (*parameters.name == "move_pointer") {
        auto arguments = parseArguments<SPointerMoveArguments>(parameters.arguments);
        if (!arguments)
            return toolError(arguments.error());
        auto realm = resolveRealm(arguments->realm);
        if (!realm)
            return toolError(realm.error());
        auto& state = m_realmStates[*realm];
        if (!arguments->x || !arguments->y || *arguments->x >= state.coordinateWidth || *arguments->y >= state.coordinateHeight)
            return toolError(
                std::format("x and y must be inside the current {}x{} realm coordinate space; call capture_realm to refresh it", state.coordinateWidth, state.coordinateHeight));
        return controlResult(m_controlClient.movePointer(*realm, *arguments->x, *arguments->y, state.coordinateWidth, state.coordinateHeight),
                             std::format("Pointer moved in realm '{}'.", *realm));
    }
    if (*parameters.name == "point_and_click") {
        auto arguments = parseArguments<SPointAndClickArguments>(parameters.arguments);
        if (!arguments)
            return toolError(arguments.error());
        auto realm = resolveRealm(arguments->realm);
        if (!realm)
            return toolError(realm.error());
        auto& state = m_realmStates[*realm];
        if (!arguments->x || !arguments->y || *arguments->x >= state.coordinateWidth || *arguments->y >= state.coordinateHeight)
            return toolError(
                std::format("x and y must be inside the current {}x{} realm coordinate space; call capture_realm to refresh it", state.coordinateWidth, state.coordinateHeight));
        const auto button = arguments->button.value_or("left");
        const auto count  = arguments->count.value_or(1);
        if ((button != "left" && button != "right" && button != "middle") || count == 0 || count > 3)
            return toolError("button must be left, right, or middle and count must be between 1 and 3");
        return controlResult(m_controlClient.pointAndClick(*realm, *arguments->x, *arguments->y, state.coordinateWidth, state.coordinateHeight, button, count),
                             std::format("Pointer moved and clicked in realm '{}'.", *realm));
    }
    if (*parameters.name == "click") {
        auto arguments = parseArguments<SClickArguments>(parameters.arguments);
        if (!arguments)
            return toolError(arguments.error());
        auto realm = resolveRealm(arguments->realm);
        if (!realm)
            return toolError(realm.error());
        if (!arguments->button || (*arguments->button != "left" && *arguments->button != "right" && *arguments->button != "middle"))
            return toolError("button must be left, right, or middle");
        return controlResult(m_controlClient.clickPointer(*realm, *arguments->button), std::format("Pointer clicked in realm '{}'.", *realm));
    }
    if (*parameters.name == "scroll") {
        auto arguments = parseArguments<SScrollArguments>(parameters.arguments);
        if (!arguments)
            return toolError(arguments.error());
        auto realm = resolveRealm(arguments->realm);
        if (!realm)
            return toolError(realm.error());
        if (!arguments->axis || !arguments->steps || (*arguments->axis != "horizontal" && *arguments->axis != "vertical") || *arguments->steps == 0 || *arguments->steps < -20 ||
            *arguments->steps > 20)
            return toolError("axis must be horizontal or vertical and steps must be between -20 and 20, excluding zero");
        return controlResult(m_controlClient.scrollPointer(*realm, *arguments->axis, *arguments->steps), std::format("Pointer scrolled in realm '{}'.", *realm));
    }
    if (*parameters.name == "press_key") {
        auto arguments = parseArguments<SKeyArguments>(parameters.arguments);
        if (!arguments)
            return toolError(arguments.error());
        auto realm = resolveRealm(arguments->realm);
        if (!realm)
            return toolError(realm.error());
        if (arguments->key.has_value() == arguments->keycode.has_value())
            return toolError("press_key requires exactly one of key or keycode");
        auto keycode = arguments->key ? keycodeForName(*arguments->key) : std::expected<uint32_t, std::string>{*arguments->keycode};
        if (!keycode || *keycode > KEY_MAX)
            return toolError(keycode ? std::format("keycode must not exceed {}", KEY_MAX) : keycode.error());
        return controlResult(m_controlClient.pressKey(*realm, *keycode), std::format("Key pressed in realm '{}'.", *realm));
    }
    if (*parameters.name == "press_shortcut") {
        auto arguments = parseArguments<SShortcutArguments>(parameters.arguments);
        if (!arguments || !arguments->modifiers || !arguments->key || arguments->modifiers->empty() || arguments->modifiers->size() > 4)
            return toolError(arguments ? "press_shortcut requires between 1 and 4 modifiers and one named key" : arguments.error());
        if (arguments->settle_ms && *arguments->settle_ms > 2000)
            return toolError("settle_ms must be between 0 and 2000");
        auto realm = resolveRealm(arguments->realm);
        if (!realm)
            return toolError(realm.error());

        std::vector<uint32_t> keycodes;
        keycodes.reserve(arguments->modifiers->size() + 1);
        for (const auto& modifier : *arguments->modifiers) {
            auto keycode = modifierKeycode(modifier);
            if (!keycode)
                return toolError(keycode.error());
            if (std::ranges::find(keycodes, *keycode) != keycodes.end())
                return toolError("shortcut modifiers must be unique");
            keycodes.emplace_back(*keycode);
        }
        auto keycode = keycodeForName(*arguments->key);
        if (!keycode)
            return toolError(keycode.error());
        if (std::ranges::find(keycodes, *keycode) != keycodes.end())
            return toolError("shortcut key must differ from its modifiers");
        keycodes.emplace_back(*keycode);
        return controlResult(m_controlClient.pressShortcut(*realm, keycodes, std::chrono::milliseconds{arguments->settle_ms.value_or(400)}),
                             std::format("Shortcut pressed in realm '{}'.", *realm));
    }
    if (*parameters.name == "type_text") {
        auto arguments = parseArguments<STextArguments>(parameters.arguments);
        if (!arguments)
            return toolError(arguments.error());
        auto realm = resolveRealm(arguments->realm);
        if (!realm)
            return toolError(realm.error());
        if (!arguments->text || arguments->text->empty() || arguments->text->size() > REALM_INPUT_MAX_TEXT_SIZE || arguments->text->contains('\0'))
            return toolError(std::format("text must contain 1 to {} bytes without NUL", REALM_INPUT_MAX_TEXT_SIZE));
        return controlResult(m_controlClient.typeText(*realm, *arguments->text), std::format("Text entered in realm '{}'.", *realm));
    }
    if (*parameters.name == "wait") {
        auto arguments = parseArguments<SWaitArguments>(parameters.arguments);
        if (!arguments || !arguments->duration_ms || *arguments->duration_ms > 30000)
            return toolError(arguments ? "duration_ms must be between 0 and 30000" : arguments.error());
        std::this_thread::sleep_for(std::chrono::milliseconds{*arguments->duration_ms});
        return toolSuccess(std::format("Waited {} ms in the realm workflow.", *arguments->duration_ms),
                           std::format(R"({{"requested_ms":{},"elapsed_ms":{}}})", *arguments->duration_ms, *arguments->duration_ms));
    }
    if (*parameters.name == "capture_realm") {
        auto arguments = parseArguments<SCaptureArguments>(parameters.arguments);
        if (!arguments)
            return toolError(arguments.error());
        auto realm = resolveRealm(arguments->realm);
        if (!realm)
            return toolError(realm.error());
        auto&      state  = m_realmStates[*realm];
        const auto fields = static_cast<unsigned>(arguments->x.has_value()) + static_cast<unsigned>(arguments->y.has_value()) +
            static_cast<unsigned>(arguments->width.has_value()) + static_cast<unsigned>(arguments->height.has_value());
        if (fields != 0 && fields != 4)
            return toolError("capture region requires x, y, width, and height together");

        const auto waitForChange = arguments->wait_for_change.value_or(false);
        const auto timeout       = arguments->timeout_ms.value_or(5000);
        const auto pollInterval  = arguments->poll_interval_ms.value_or(100);
        if ((!waitForChange && (arguments->timeout_ms || arguments->poll_interval_ms)) || timeout < 100 || timeout > 30000 || pollInterval < 100 || pollInterval > 2000)
            return toolError("timeout_ms and poll_interval_ms require wait_for_change=true and must be within their documented bounds");

        const auto                   started = std::chrono::steady_clock::now();
        std::optional<SCaptureFrame> captured;
        if (waitForChange) {
            auto baseline = state.lastCaptureHash;
            if (!baseline) {
                auto initial = m_controlClient.capture(*realm, std::nullopt);
                if (!initial)
                    return toolError(initial.error());
                baseline              = captureHash(*initial);
                state.lastCaptureHash = baseline;
            }

            const auto deadline = started + std::chrono::milliseconds{timeout};
            while (std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds{pollInterval});
                auto current = m_controlClient.capture(*realm, std::nullopt);
                if (!current)
                    return toolError(current.error());
                const auto hash = captureHash(*current);
                if (hash == *baseline)
                    continue;
                state.lastCaptureHash = hash;
                captured              = std::move(*current);
                break;
            }
            if (!captured)
                return toolError(std::format("realm did not produce a visually different frame within {} ms", timeout));
        } else {
            auto current = m_controlClient.capture(*realm, std::nullopt);
            if (!current)
                return toolError(current.error());
            state.lastCaptureHash = captureHash(*current);
            captured              = std::move(*current);
        }

        std::optional<SCaptureRegion> region;
        if (fields == 4) {
            if (*arguments->width == 0 || *arguments->height == 0 || *arguments->x >= captured->width || *arguments->y >= captured->height ||
                *arguments->width > captured->width - *arguments->x || *arguments->height > captured->height - *arguments->y)
                return toolError(std::format("capture region must be inside the native {}x{} realm frame", captured->width, captured->height));
            region = SCaptureRegion{.x = *arguments->x, .y = *arguments->y, .width = *arguments->width, .height = *arguments->height};
        }

        auto png = encodePNG(*captured, region);
        if (!png)
            return toolError(png.error());
        const auto outputWidth  = region ? region->width : captured->width;
        const auto outputHeight = region ? region->height : captured->height;
        state.coordinateWidth   = captured->width;
        state.coordinateHeight  = captured->height;
        const auto elapsed      = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        const auto metadata     = std::format(
            R"({{"realm":{},"width":{},"height":{},"source_width":{},"source_height":{},"coordinate_width":{},"coordinate_height":{},"waited_for_change":{},"elapsed_ms":{},"mimeType":"image/png"}})",
            quoteJSON(*realm), outputWidth, outputHeight, captured->width, captured->height, state.coordinateWidth, state.coordinateHeight, waitForChange, elapsed);
        return std::format(R"({{"content":[{{"type":"image","data":{},"mimeType":"image/png"}},{{"type":"text","text":{}}}],"structuredContent":{},"isError":false}})",
                           quoteJSON(base64Encode(*png)),
                           quoteJSON(std::format("Captured {}x{} PNG from realm '{}' in its native {}x{} coordinate space.", outputWidth, outputHeight, *realm, captured->width,
                                                 captured->height)),
                           metadata);
    }

    return toolError(std::format("unknown tool '{}'", *parameters.name));
}
