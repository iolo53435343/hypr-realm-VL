#include "MCPServer.hpp"

#include "RealmControlClient.hpp"

#include "../../src/realm/RealmInputProtocol.hpp"

#include <algorithm>
#include <array>
#include <cairo/cairo.h>
#include <cctype>
#include <cstdint>
#include <expected>
#include <format>
#include <glaze/glaze.hpp>
#include <iostream>
#include <limits>
#include <linux/input-event-codes.h>
#include <map>
#include <optional>
#include <string>
#include <string_view>
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

struct SClickArguments {
    std::optional<std::string> button;
};

struct SScrollArguments {
    std::optional<std::string> axis;
    std::optional<int32_t>     steps;
};

struct SKeyArguments {
    std::optional<uint32_t> keycode;
};

struct STextArguments {
    std::optional<std::string> text;
};

struct SCaptureArguments {
    std::optional<uint32_t> x;
    std::optional<uint32_t> y;
    std::optional<uint32_t> width;
    std::optional<uint32_t> height;
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

static constexpr std::string_view TOOLS_RESULT = R"json({"tools":[
  {"name":"realm_info","description":"Inspect the realm bound to this MCP server, including state, ownership, observation permission, and host-granted capabilities.","inputSchema":{"type":"object","properties":{},"additionalProperties":false},"annotations":{"title":"Inspect bound realm","readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
  {"name":"realm_create","description":"Create the realm bound to this MCP server. It cannot create or name any other realm.","inputSchema":{"type":"object","properties":{},"additionalProperties":false},"annotations":{"title":"Create bound realm","readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}},
  {"name":"realm_start","description":"Start the realm bound to this MCP server.","inputSchema":{"type":"object","properties":{},"additionalProperties":false},"annotations":{"title":"Start bound realm","readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}},
  {"name":"realm_pause","description":"Pause the realm bound to this MCP server.","inputSchema":{"type":"object","properties":{},"additionalProperties":false},"annotations":{"title":"Pause bound realm","readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}},
  {"name":"realm_resume","description":"Resume the realm bound to this MCP server.","inputSchema":{"type":"object","properties":{},"additionalProperties":false},"annotations":{"title":"Resume bound realm","readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}},
  {"name":"realm_stop","description":"Stop the realm bound to this MCP server without destroying its record.","inputSchema":{"type":"object","properties":{},"additionalProperties":false},"annotations":{"title":"Stop bound realm","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
  {"name":"enable_observation","description":"Enable runtime observation for the bound realm. The host must have granted its observe capability first.","inputSchema":{"type":"object","properties":{},"additionalProperties":false},"annotations":{"title":"Enable realm observation","readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}},
  {"name":"disable_observation","description":"Disable runtime observation for the bound realm and cancel pending captures.","inputSchema":{"type":"object","properties":{},"additionalProperties":false},"annotations":{"title":"Disable realm observation","readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}},
  {"name":"capture_realm","description":"Capture the bound realm as a PNG in the same stable 1280x720 coordinate space used by pointer input. Observation must already be enabled. Supply either all four region fields or none.","inputSchema":{"type":"object","properties":{"x":{"type":"integer","minimum":0,"maximum":1279},"y":{"type":"integer","minimum":0,"maximum":719},"width":{"type":"integer","minimum":1,"maximum":1280},"height":{"type":"integer","minimum":1,"maximum":720}},"additionalProperties":false},"annotations":{"title":"Capture bound realm","readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
  {"name":"move_pointer","description":"Move the virtual pointer inside the bound realm. The host must have granted its pointer capability.","inputSchema":{"type":"object","properties":{"x":{"type":"integer","minimum":0,"maximum":1279},"y":{"type":"integer","minimum":0,"maximum":719}},"required":["x","y"],"additionalProperties":false},"annotations":{"title":"Move realm pointer","readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
  {"name":"click","description":"Press and release a virtual pointer button inside the bound realm.","inputSchema":{"type":"object","properties":{"button":{"type":"string","enum":["left","right","middle"]}},"required":["button"],"additionalProperties":false},"annotations":{"title":"Click in realm","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
  {"name":"scroll","description":"Scroll the virtual pointer inside the bound realm by a bounded number of steps.","inputSchema":{"type":"object","properties":{"axis":{"type":"string","enum":["horizontal","vertical"]},"steps":{"type":"integer","minimum":-20,"maximum":20,"not":{"const":0}}},"required":["axis","steps"],"additionalProperties":false},"annotations":{"title":"Scroll in realm","readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}},
  {"name":"press_key","description":"Press and release one Linux evdev keycode inside the bound realm.","inputSchema":{"type":"object","properties":{"keycode":{"type":"integer","minimum":0,"maximum":767}},"required":["keycode"],"additionalProperties":false},"annotations":{"title":"Press key in realm","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
  {"name":"type_text","description":"Type UTF-8 text inside the bound realm, up to 4096 bytes.","inputSchema":{"type":"object","properties":{"text":{"type":"string","minLength":1,"maxLength":4096}},"required":["text"],"additionalProperties":false},"annotations":{"title":"Type text in realm","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}}
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

    const auto       outputWidth  = region ? region->width : REALM_INPUT_OUTPUT_WIDTH;
    const auto       outputHeight = region ? region->height : REALM_INPUT_OUTPUT_HEIGHT;
    cairo_surface_t* output       = cairo_image_surface_create(CAIRO_FORMAT_RGB24, static_cast<int>(outputWidth), static_cast<int>(outputHeight));
    if (!output || cairo_surface_status(output) != CAIRO_STATUS_SUCCESS) {
        const auto message = std::format("failed creating normalized realm capture surface: {}", output ? cairo_status_to_string(cairo_surface_status(output)) : "out of memory");
        if (output)
            cairo_surface_destroy(output);
        cairo_surface_destroy(source);
        return std::unexpected(message);
    }

    cairo_surface_mark_dirty(source);
    cairo_t* context = cairo_create(output);
    if (!context || cairo_status(context) != CAIRO_STATUS_SUCCESS) {
        const auto message = std::format("failed creating normalized realm capture context: {}", context ? cairo_status_to_string(cairo_status(context)) : "out of memory");
        if (context)
            cairo_destroy(context);
        cairo_surface_destroy(output);
        cairo_surface_destroy(source);
        return std::unexpected(message);
    }

    cairo_translate(context, -static_cast<double>(region ? region->x : 0), -static_cast<double>(region ? region->y : 0));
    cairo_scale(context, static_cast<double>(REALM_INPUT_OUTPUT_WIDTH) / frame.width, static_cast<double>(REALM_INPUT_OUTPUT_HEIGHT) / frame.height);
    cairo_set_source_surface(context, source, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(context), CAIRO_FILTER_BILINEAR);
    cairo_paint(context);
    const auto paintStatus = cairo_status(context);
    cairo_destroy(context);
    cairo_surface_destroy(source);
    if (paintStatus != CAIRO_STATUS_SUCCESS) {
        const auto message = std::format("failed normalizing realm capture: {}", cairo_status_to_string(paintStatus));
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
            quoteJSON(std::format("This server is permanently bound to realm '{}'. Host-granted capabilities remain authoritative; tools cannot grant or revoke them.",
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
        if (!arguments || !arguments->x || !arguments->y || *arguments->x >= REALM_INPUT_OUTPUT_WIDTH || *arguments->y >= REALM_INPUT_OUTPUT_HEIGHT)
            return toolError(arguments ? "x and y must be coordinates inside the 1280x720 realm output" : arguments.error());
        return controlResult(m_controlClient.movePointer(*arguments->x, *arguments->y), "Pointer moved in the bound realm.");
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
        if (!arguments || !arguments->keycode || *arguments->keycode > KEY_MAX)
            return toolError(arguments ? std::format("keycode must not exceed {}", KEY_MAX) : arguments.error());
        return controlResult(m_controlClient.pressKey(*arguments->keycode), "Key pressed in the bound realm.");
    }
    if (*parameters.name == "type_text") {
        auto arguments = parseArguments<STextArguments>(parameters.arguments);
        if (!arguments || !arguments->text || arguments->text->empty() || arguments->text->size() > REALM_INPUT_MAX_TEXT_SIZE || arguments->text->contains('\0'))
            return toolError(arguments ? std::format("text must contain 1 to {} bytes without NUL", REALM_INPUT_MAX_TEXT_SIZE) : arguments.error());
        return controlResult(m_controlClient.typeText(*arguments->text), "Text entered in the bound realm.");
    }
    if (*parameters.name == "capture_realm") {
        auto arguments = parseArguments<SCaptureArguments>(parameters.arguments);
        if (!arguments)
            return toolError(arguments.error());
        const auto fields = static_cast<unsigned>(arguments->x.has_value()) + static_cast<unsigned>(arguments->y.has_value()) +
            static_cast<unsigned>(arguments->width.has_value()) + static_cast<unsigned>(arguments->height.has_value());
        if (fields != 0 && fields != 4)
            return toolError("capture region requires x, y, width, and height together");

        std::optional<SCaptureRegion> region;
        if (fields == 4) {
            if (*arguments->width == 0 || *arguments->height == 0 || *arguments->x >= REALM_INPUT_OUTPUT_WIDTH || *arguments->y >= REALM_INPUT_OUTPUT_HEIGHT ||
                *arguments->width > REALM_INPUT_OUTPUT_WIDTH - *arguments->x || *arguments->height > REALM_INPUT_OUTPUT_HEIGHT - *arguments->y)
                return toolError("capture region must be inside the 1280x720 realm output");
            region = SCaptureRegion{.x = *arguments->x, .y = *arguments->y, .width = *arguments->width, .height = *arguments->height};
        }

        auto capture = m_controlClient.capture(std::nullopt);
        if (!capture)
            return toolError(capture.error());
        auto png = encodePNG(*capture, region);
        if (!png)
            return toolError(png.error());
        const auto outputWidth  = region ? region->width : REALM_INPUT_OUTPUT_WIDTH;
        const auto outputHeight = region ? region->height : REALM_INPUT_OUTPUT_HEIGHT;
        const auto metadata     = std::format(R"({{"realm":{},"width":{},"height":{},"source_width":{},"source_height":{},"mimeType":"image/png"}})",
                                              quoteJSON(m_controlClient.realm()), outputWidth, outputHeight, capture->width, capture->height);
        return std::format(R"({{"content":[{{"type":"image","data":{},"mimeType":"image/png"}},{{"type":"text","text":{}}}],"structuredContent":{},"isError":false}})",
                           quoteJSON(base64Encode(*png)), quoteJSON(std::format("Captured {}x{} PNG from realm '{}'.", outputWidth, outputHeight, m_controlClient.realm())),
                           metadata);
    }

    return toolError(std::format("unknown tool '{}'", *parameters.name));
}
