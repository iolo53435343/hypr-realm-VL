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
#include <span>
#include <string>
#include <string_view>
#include <thread>
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

struct SPointerMoveArguments {
    std::optional<uint32_t> x;
    std::optional<uint32_t> y;
};

struct SPointAndClickArguments {
    std::optional<uint32_t>    x;
    std::optional<uint32_t>    y;
    std::optional<std::string> button;
    std::optional<uint32_t>    count;
};

struct SClickArguments {
    std::optional<std::string> button;
};

struct SScrollArguments {
    std::optional<std::string> axis;
    std::optional<int32_t>     steps;
};

struct SKeyArguments {
    std::optional<uint32_t>    keycode;
    std::optional<std::string> key;
};

struct SShortcutArguments {
    std::optional<std::vector<std::string>> modifiers;
    std::optional<std::string>              key;
};

struct STextArguments {
    std::optional<std::string> text;
};

struct SCaptureArguments {
    std::optional<uint32_t> x;
    std::optional<uint32_t> y;
    std::optional<uint32_t> width;
    std::optional<uint32_t> height;
    std::optional<bool>     wait_for_change;
    std::optional<uint32_t> timeout_ms;
    std::optional<uint32_t> poll_interval_ms;
};

struct SWaitArguments {
    std::optional<uint32_t> duration_ms;
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

static constexpr std::string_view TOOLS_RESULT = R"json({"tools":[
  {"name":"realm_info","description":"Inspect the realm bound to this MCP server, including state, ownership, observation permission, and host-granted capabilities.","inputSchema":{"type":"object","properties":{},"additionalProperties":false},"annotations":{"title":"Inspect bound realm","readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
  {"name":"realm_create","description":"Create the realm bound to this MCP server. It cannot create or name any other realm.","inputSchema":{"type":"object","properties":{},"additionalProperties":false},"annotations":{"title":"Create bound realm","readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}},
  {"name":"realm_start","description":"Start the realm bound to this MCP server.","inputSchema":{"type":"object","properties":{},"additionalProperties":false},"annotations":{"title":"Start bound realm","readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}},
  {"name":"realm_pause","description":"Pause the realm bound to this MCP server.","inputSchema":{"type":"object","properties":{},"additionalProperties":false},"annotations":{"title":"Pause bound realm","readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}},
  {"name":"realm_resume","description":"Resume the realm bound to this MCP server.","inputSchema":{"type":"object","properties":{},"additionalProperties":false},"annotations":{"title":"Resume bound realm","readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}},
  {"name":"realm_stop","description":"Stop the realm bound to this MCP server without destroying its record.","inputSchema":{"type":"object","properties":{},"additionalProperties":false},"annotations":{"title":"Stop bound realm","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
  {"name":"enable_observation","description":"Enable runtime observation for the bound realm. The host must have granted its observe capability first.","inputSchema":{"type":"object","properties":{},"additionalProperties":false},"annotations":{"title":"Enable realm observation","readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}},
  {"name":"disable_observation","description":"Disable runtime observation for the bound realm and cancel pending captures.","inputSchema":{"type":"object","properties":{},"additionalProperties":false},"annotations":{"title":"Disable realm observation","readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}},
  {"name":"capture_realm","description":"Capture the complete bound realm at its native dimensions without supplying a size. The returned width and height define the exact coordinate space used by subsequent pointer tools. Observation must already be enabled. Optional x, y, width, and height crop the native image; wait_for_change waits for a frame different from the last capture.","inputSchema":{"type":"object","properties":{"x":{"type":"integer","minimum":0,"maximum":16383},"y":{"type":"integer","minimum":0,"maximum":16383},"width":{"type":"integer","minimum":1,"maximum":16384},"height":{"type":"integer","minimum":1,"maximum":16384},"wait_for_change":{"type":"boolean","default":false},"timeout_ms":{"type":"integer","minimum":100,"maximum":30000,"default":5000},"poll_interval_ms":{"type":"integer","minimum":250,"maximum":2000,"default":300}},"additionalProperties":false},"annotations":{"title":"Capture bound realm","readOnlyHint":true,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}},
  {"name":"move_pointer","description":"Move the virtual pointer in the native coordinate space returned by the latest capture_realm call. Before the first capture, the compatibility coordinate space is 1280x720.","inputSchema":{"type":"object","properties":{"x":{"type":"integer","minimum":0,"maximum":16383},"y":{"type":"integer","minimum":0,"maximum":16383}},"required":["x","y"],"additionalProperties":false},"annotations":{"title":"Move realm pointer","readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
  {"name":"point_and_click","description":"Atomically move to a coordinate and click after the compositor receives the motion. Coordinates use the latest capture_realm dimensions. Prefer this over separate move_pointer and click calls.","inputSchema":{"type":"object","properties":{"x":{"type":"integer","minimum":0,"maximum":16383},"y":{"type":"integer","minimum":0,"maximum":16383},"button":{"type":"string","enum":["left","right","middle"],"default":"left"},"count":{"type":"integer","minimum":1,"maximum":3,"default":1}},"required":["x","y"],"additionalProperties":false},"annotations":{"title":"Point and click in realm","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
  {"name":"click","description":"Press and release a virtual pointer button inside the bound realm.","inputSchema":{"type":"object","properties":{"button":{"type":"string","enum":["left","right","middle"]}},"required":["button"],"additionalProperties":false},"annotations":{"title":"Click in realm","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
  {"name":"scroll","description":"Scroll the virtual pointer inside the bound realm by a bounded number of steps.","inputSchema":{"type":"object","properties":{"axis":{"type":"string","enum":["horizontal","vertical"]},"steps":{"type":"integer","minimum":-20,"maximum":20,"not":{"const":0}}},"required":["axis","steps"],"additionalProperties":false},"annotations":{"title":"Scroll in realm","readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}},
  {"name":"press_key","description":"Press and release one named key inside the bound realm. Prefer names such as enter, tab, escape, left, page_down, a, or f5; raw Linux evdev keycode remains available for compatibility. Supply exactly one of key or keycode.","inputSchema":{"type":"object","properties":{"key":{"type":"string","minLength":1,"maxLength":32},"keycode":{"type":"integer","minimum":0,"maximum":767}},"additionalProperties":false},"annotations":{"title":"Press key in realm","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
  {"name":"press_shortcut","description":"Atomically press a named key with one or more modifiers inside the bound realm, for example modifiers [ctrl] with key t.","inputSchema":{"type":"object","properties":{"modifiers":{"type":"array","minItems":1,"maxItems":4,"uniqueItems":true,"items":{"type":"string","enum":["ctrl","alt","shift","meta"]}},"key":{"type":"string","minLength":1,"maxLength":32}},"required":["modifiers","key"],"additionalProperties":false},"annotations":{"title":"Press shortcut in realm","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
  {"name":"type_text","description":"Type UTF-8 text inside the bound realm, up to 4096 bytes.","inputSchema":{"type":"object","properties":{"text":{"type":"string","minLength":1,"maxLength":4096}},"required":["text"],"additionalProperties":false},"annotations":{"title":"Type text in realm","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
  {"name":"wait","description":"Wait for a bounded duration within the realm workflow so an application can finish navigation or animation. Prefer capture_realm with wait_for_change when a visual transition is expected.","inputSchema":{"type":"object","properties":{"duration_ms":{"type":"integer","minimum":0,"maximum":30000}},"required":["duration_ms"],"additionalProperties":false},"annotations":{"title":"Wait in realm workflow","readOnlyHint":true,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}}
]})json";

static const std::string&         toolsResult() {
    static const auto result = [] {
        std::string compact{TOOLS_RESULT};
        std::erase(compact, '\n');
        std::erase(compact, '\r');
        return compact;
    }();
    return result;
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

CMCPServer::CMCPServer(CRealmControlClient& controlClient) : m_controlClient(controlClient) {}

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
        const auto result          = std::format(
            R"({{"protocolVersion":{},"capabilities":{{"tools":{{"listChanged":false}}}},"serverInfo":{{"name":"hyprland-realm","version":"0.1.0"}},"instructions":{}}})",
            quoteJSON(protocolVersion),
            quoteJSON(
                std::format("This server is permanently bound to realm '{}'. Host-granted capabilities remain authoritative; tools cannot grant or revoke them. Call capture_realm "
                                              "without dimensions immediately before coordinate input, prefer point_and_click over separate move and click calls, and capture again with "
                                              "wait_for_change after visible "
                                              "actions. Input success confirms compositor processing, not application-level completion.",
                                     m_controlClient.realm())));
        return jsonRPCResult(id, result);
    }
    if (isNotification)
        return std::nullopt;
    if (!m_initialized)
        return jsonRPCError(id, -32002, "server is not initialized");
    if (*request.method == "tools/list")
        return jsonRPCResult(id, toolsResult());
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

    const auto noArguments = [&]() -> std::optional<std::string> {
        if (auto parsed = parseNoArguments(parameters.arguments); !parsed)
            return toolError(parsed.error());
        return std::nullopt;
    };
    const auto controlResult = [](std::expected<std::string, std::string> result, std::string_view success) {
        return result ? toolSuccess(success, *result) : toolError(result.error());
    };

    if (*parameters.name == "realm_info") {
        if (auto error = noArguments(); error)
            return *error;
        return controlResult(m_controlClient.realmInfo(), "Bound realm information retrieved.");
    }
    if (*parameters.name == "realm_create") {
        if (auto error = noArguments(); error)
            return *error;
        return controlResult(m_controlClient.createRealm(), "Bound realm created.");
    }
    if (*parameters.name == "realm_start") {
        if (auto error = noArguments(); error)
            return *error;
        return controlResult(m_controlClient.startRealm(), "Bound realm is starting.");
    }
    if (*parameters.name == "realm_pause") {
        if (auto error = noArguments(); error)
            return *error;
        return controlResult(m_controlClient.pauseRealm(), "Bound realm paused.");
    }
    if (*parameters.name == "realm_resume") {
        if (auto error = noArguments(); error)
            return *error;
        return controlResult(m_controlClient.resumeRealm(), "Bound realm resumed.");
    }
    if (*parameters.name == "realm_stop") {
        if (auto error = noArguments(); error)
            return *error;
        return controlResult(m_controlClient.stopRealm(), "Bound realm is stopping.");
    }
    if (*parameters.name == "enable_observation") {
        if (auto error = noArguments(); error)
            return *error;
        return controlResult(m_controlClient.allowObservation(), "Observation enabled for the bound realm.");
    }
    if (*parameters.name == "disable_observation") {
        if (auto error = noArguments(); error)
            return *error;
        return controlResult(m_controlClient.denyObservation(), "Observation disabled for the bound realm.");
    }
    if (*parameters.name == "move_pointer") {
        auto arguments = parseArguments<SPointerMoveArguments>(parameters.arguments);
        if (!arguments || !arguments->x || !arguments->y || *arguments->x >= m_controlClient.coordinateWidth() || *arguments->y >= m_controlClient.coordinateHeight())
            return toolError(arguments ? std::format("x and y must be inside the current {}x{} realm coordinate space; call capture_realm to refresh it",
                                                     m_controlClient.coordinateWidth(), m_controlClient.coordinateHeight()) :
                                         arguments.error());
        return controlResult(m_controlClient.movePointer(*arguments->x, *arguments->y), "Pointer moved in the bound realm.");
    }
    if (*parameters.name == "point_and_click") {
        auto arguments = parseArguments<SPointAndClickArguments>(parameters.arguments);
        if (!arguments || !arguments->x || !arguments->y || *arguments->x >= m_controlClient.coordinateWidth() || *arguments->y >= m_controlClient.coordinateHeight())
            return toolError(arguments ? std::format("x and y must be inside the current {}x{} realm coordinate space; call capture_realm to refresh it",
                                                     m_controlClient.coordinateWidth(), m_controlClient.coordinateHeight()) :
                                         arguments.error());
        const auto button = arguments->button.value_or("left");
        const auto count  = arguments->count.value_or(1);
        if ((button != "left" && button != "right" && button != "middle") || count == 0 || count > 3)
            return toolError("button must be left, right, or middle and count must be between 1 and 3");
        return controlResult(m_controlClient.pointAndClick(*arguments->x, *arguments->y, button, count), "Pointer moved and clicked in the bound realm.");
    }
    if (*parameters.name == "click") {
        auto arguments = parseArguments<SClickArguments>(parameters.arguments);
        if (!arguments || !arguments->button || (*arguments->button != "left" && *arguments->button != "right" && *arguments->button != "middle"))
            return toolError(arguments ? "button must be left, right, or middle" : arguments.error());
        return controlResult(m_controlClient.clickPointer(*arguments->button), "Pointer clicked in the bound realm.");
    }
    if (*parameters.name == "scroll") {
        auto arguments = parseArguments<SScrollArguments>(parameters.arguments);
        if (!arguments || !arguments->axis || !arguments->steps || (*arguments->axis != "horizontal" && *arguments->axis != "vertical") || *arguments->steps == 0 ||
            *arguments->steps < -20 || *arguments->steps > 20)
            return toolError(arguments ? "axis must be horizontal or vertical and steps must be between -20 and 20, excluding zero" : arguments.error());
        return controlResult(m_controlClient.scrollPointer(*arguments->axis, *arguments->steps), "Pointer scrolled in the bound realm.");
    }
    if (*parameters.name == "press_key") {
        auto arguments = parseArguments<SKeyArguments>(parameters.arguments);
        if (!arguments)
            return toolError(arguments.error());
        if (arguments->key.has_value() == arguments->keycode.has_value())
            return toolError("press_key requires exactly one of key or keycode");
        auto keycode = arguments->key ? keycodeForName(*arguments->key) : std::expected<uint32_t, std::string>{*arguments->keycode};
        if (!keycode || *keycode > KEY_MAX)
            return toolError(keycode ? std::format("keycode must not exceed {}", KEY_MAX) : keycode.error());
        return controlResult(m_controlClient.pressKey(*keycode), "Key pressed in the bound realm.");
    }
    if (*parameters.name == "press_shortcut") {
        auto arguments = parseArguments<SShortcutArguments>(parameters.arguments);
        if (!arguments || !arguments->modifiers || !arguments->key || arguments->modifiers->empty() || arguments->modifiers->size() > 4)
            return toolError(arguments ? "press_shortcut requires between 1 and 4 modifiers and one named key" : arguments.error());

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
        return controlResult(m_controlClient.pressShortcut(keycodes), "Shortcut pressed in the bound realm.");
    }
    if (*parameters.name == "type_text") {
        auto arguments = parseArguments<STextArguments>(parameters.arguments);
        if (!arguments || !arguments->text || arguments->text->empty() || arguments->text->size() > REALM_INPUT_MAX_TEXT_SIZE || arguments->text->contains('\0'))
            return toolError(arguments ? std::format("text must contain 1 to {} bytes without NUL", REALM_INPUT_MAX_TEXT_SIZE) : arguments.error());
        return controlResult(m_controlClient.typeText(*arguments->text), "Text entered in the bound realm.");
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
        const auto fields = static_cast<unsigned>(arguments->x.has_value()) + static_cast<unsigned>(arguments->y.has_value()) +
            static_cast<unsigned>(arguments->width.has_value()) + static_cast<unsigned>(arguments->height.has_value());
        if (fields != 0 && fields != 4)
            return toolError("capture region requires x, y, width, and height together");

        const auto waitForChange = arguments->wait_for_change.value_or(false);
        const auto timeout       = arguments->timeout_ms.value_or(5000);
        const auto pollInterval  = arguments->poll_interval_ms.value_or(300);
        if ((!waitForChange && (arguments->timeout_ms || arguments->poll_interval_ms)) || timeout < 100 || timeout > 30000 || pollInterval < 250 || pollInterval > 2000)
            return toolError("timeout_ms and poll_interval_ms require wait_for_change=true and must be within their documented bounds");

        const auto                   started = std::chrono::steady_clock::now();
        std::optional<SCaptureFrame> captured;
        if (waitForChange) {
            auto baseline = m_lastCaptureHash;
            if (!baseline) {
                auto initial = m_controlClient.capture(std::nullopt);
                if (!initial)
                    return toolError(initial.error());
                baseline          = captureHash(*initial);
                m_lastCaptureHash = baseline;
            }

            const auto deadline = started + std::chrono::milliseconds{timeout};
            while (std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds{pollInterval});
                auto current = m_controlClient.capture(std::nullopt);
                if (!current)
                    return toolError(current.error());
                const auto hash = captureHash(*current);
                if (hash == *baseline)
                    continue;
                m_lastCaptureHash = hash;
                captured          = std::move(*current);
                break;
            }
            if (!captured)
                return toolError(std::format("realm did not produce a visually different frame within {} ms", timeout));
        } else {
            auto current = m_controlClient.capture(std::nullopt);
            if (!current)
                return toolError(current.error());
            m_lastCaptureHash = captureHash(*current);
            captured          = std::move(*current);
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
        const auto elapsed      = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        const auto metadata     = std::format(
            R"({{"realm":{},"width":{},"height":{},"source_width":{},"source_height":{},"coordinate_width":{},"coordinate_height":{},"waited_for_change":{},"elapsed_ms":{},"mimeType":"image/png"}})",
            quoteJSON(m_controlClient.realm()), outputWidth, outputHeight, captured->width, captured->height, m_controlClient.coordinateWidth(), m_controlClient.coordinateHeight(),
            waitForChange, elapsed);
        return std::format(R"({{"content":[{{"type":"image","data":{},"mimeType":"image/png"}},{{"type":"text","text":{}}}],"structuredContent":{},"isError":false}})",
                           quoteJSON(base64Encode(*png)),
                           quoteJSON(std::format("Captured {}x{} PNG from realm '{}' in its native {}x{} coordinate space.", outputWidth, outputHeight, m_controlClient.realm(),
                                                 captured->width, captured->height)),
                           metadata);
    }

    return toolError(std::format("unknown tool '{}'", *parameters.name));
}
