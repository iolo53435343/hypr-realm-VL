#include "WaylandInput.hpp"

#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

#if defined(__linux__)
#include <linux/memfd.h>
#include <sys/syscall.h>
#endif

using namespace Realm;

struct SRealmKeyStroke {
    uint32_t code      = 0;
    uint32_t modifiers = 0;
    uint32_t group     = 0;
};

static uint32_t inputTimestamp() {
    const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

static int createAnonymousFile(size_t size) {
    int fd = -1;
#if defined(__linux__) && defined(SYS_memfd_create)
    fd = static_cast<int>(syscall(SYS_memfd_create, "hypr-realm-keymap", MFD_CLOEXEC | MFD_ALLOW_SEALING));
#elif defined(SHM_ANON)
    fd = shm_open(SHM_ANON, O_RDWR | O_CLOEXEC, 0600);
#else
    std::array<char, 32> path = {"/tmp/hypr-realm-keymap-XXXXXX"};
    fd                        = mkostemp(path.data(), O_CLOEXEC);
    if (fd >= 0)
        unlink(path.data());
#endif
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) < 0) {
        const auto error = errno;
        if (fd >= 0)
            close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

static std::expected<std::vector<uint32_t>, std::string> decodeUTF8(std::string_view text) {
    std::vector<uint32_t> codepoints;
    codepoints.reserve(text.size());

    for (size_t offset = 0; offset < text.size();) {
        const auto first     = static_cast<uint8_t>(text[offset]);
        uint32_t   codepoint = 0;
        size_t     length    = 0;
        uint32_t   minimum   = 0;
        if (first < 0x80) {
            codepoint = first;
            length    = 1;
        } else if ((first & 0xE0) == 0xC0) {
            codepoint = first & 0x1F;
            length    = 2;
            minimum   = 0x80;
        } else if ((first & 0xF0) == 0xE0) {
            codepoint = first & 0x0F;
            length    = 3;
            minimum   = 0x800;
        } else if ((first & 0xF8) == 0xF0) {
            codepoint = first & 0x07;
            length    = 4;
            minimum   = 0x10000;
        } else
            return std::unexpected("keyboard text is not valid UTF-8");

        if (offset + length > text.size())
            return std::unexpected("keyboard text ends inside a UTF-8 sequence");
        for (size_t index = 1; index < length; ++index) {
            const auto continuation = static_cast<uint8_t>(text[offset + index]);
            if ((continuation & 0xC0) != 0x80)
                return std::unexpected("keyboard text contains an invalid UTF-8 continuation byte");
            codepoint = (codepoint << 6) | (continuation & 0x3F);
        }

        if (codepoint < minimum || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF) || codepoint == 0)
            return std::unexpected("keyboard text contains an invalid Unicode codepoint");
        codepoints.emplace_back(codepoint);
        offset += length;
    }

    return codepoints;
}

static xkb_keysym_t keysymForCodepoint(uint32_t codepoint) {
    switch (codepoint) {
        case '\n': return XKB_KEY_Return;
        case '\t': return XKB_KEY_Tab;
        case '\b': return XKB_KEY_BackSpace;
        case 0x1B: return XKB_KEY_Escape;
        default: return xkb_utf32_to_keysym(codepoint);
    }
}

struct CWaylandInput::SImpl {
    explicit SImpl(int waylandFD_) : waylandFD(waylandFD_) {}

    ~SImpl() {
        releaseAll();
        if (keyboard)
            zwp_virtual_keyboard_v1_destroy(keyboard);
        if (pointer)
            zwlr_virtual_pointer_v1_destroy(pointer);
        if (keyboardManager)
            zwp_virtual_keyboard_manager_v1_destroy(keyboardManager);
        if (pointerManager)
            zwlr_virtual_pointer_manager_v1_destroy(pointerManager);
        if (seat)
            wl_seat_release(seat);
        if (registry)
            wl_registry_destroy(registry);
        if (display)
            wl_display_disconnect(display);
        else if (waylandFD >= 0)
            close(waylandFD);
        if (keymap)
            xkb_keymap_unref(keymap);
        if (context)
            xkb_context_unref(context);
    }

    static void handleGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
        auto& self = *static_cast<SImpl*>(data);
        if (std::strcmp(interface, wl_seat_interface.name) == 0 && !self.seat && version >= 5)
            self.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 7U)));
        else if (std::strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0 && !self.keyboardManager)
            self.keyboardManager = static_cast<zwp_virtual_keyboard_manager_v1*>(wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1));
        else if (std::strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0 && !self.pointerManager)
            self.pointerManager =
                static_cast<zwlr_virtual_pointer_manager_v1*>(wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, std::min(version, 2U)));
    }

    static void handleGlobalRemove(void*, wl_registry*, uint32_t) {
        ;
    }

    std::expected<void, std::string> initialize() {
        display = wl_display_connect_to_fd(waylandFD);
        if (!display)
            return std::unexpected("failed creating a Wayland display from the realm socket");
        waylandFD = -1;

        registry = wl_display_get_registry(display);
        if (!registry)
            return std::unexpected("failed obtaining the realm Wayland registry");

        static constexpr wl_registry_listener LISTENER = {
            .global        = handleGlobal,
            .global_remove = handleGlobalRemove,
        };
        if (wl_registry_add_listener(registry, &LISTENER, this) < 0 || wl_display_roundtrip(display) < 0)
            return std::unexpected("failed reading globals from the realm Wayland display");
        if (!seat)
            return std::unexpected("realm Wayland display does not expose a compatible seat");
        if (!keyboardManager)
            return std::unexpected("realm Wayland display does not expose virtual keyboard support");
        if (!pointerManager)
            return std::unexpected("realm Wayland display does not expose virtual pointer support");

        keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(keyboardManager, seat);
        pointer  = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(pointerManager, seat);
        if (!keyboard || !pointer)
            return std::unexpected("failed creating realm virtual input devices");

        context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        if (!context)
            return std::unexpected("failed creating the keyboard map context");
        const xkb_rule_names names{};
        keymap = xkb_keymap_new_from_names2(context, &names, XKB_KEYMAP_FORMAT_TEXT_V2, XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (!keymap)
            return std::unexpected("failed compiling the realm keyboard map");

        char* keymapText = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V2);
        if (!keymapText)
            return std::unexpected("failed serializing the realm keyboard map");
        const auto keymapSize = std::strlen(keymapText) + 1;
        const auto keymapFD   = createAnonymousFile(keymapSize);
        if (keymapFD < 0) {
            std::free(keymapText);
            return std::unexpected(std::format("failed creating keyboard map storage: {}", std::strerror(errno)));
        }

        size_t written = 0;
        while (written < keymapSize) {
            const auto result = write(keymapFD, keymapText + written, keymapSize - written);
            if (result > 0) {
                written += static_cast<size_t>(result);
                continue;
            }
            if (result < 0 && errno == EINTR)
                continue;
            const auto error = errno;
            std::free(keymapText);
            close(keymapFD);
            return std::unexpected(std::format("failed writing the keyboard map: {}", std::strerror(error)));
        }
        std::free(keymapText);
        lseek(keymapFD, 0, SEEK_SET);
        zwp_virtual_keyboard_v1_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, keymapFD, static_cast<uint32_t>(keymapSize));
        close(keymapFD);
        return flush();
    }

    std::expected<SRealmKeyStroke, std::string> keyStrokeFor(uint32_t codepoint) {
        if (const auto cached = keyStrokes.find(codepoint); cached != keyStrokes.end())
            return cached->second;

        const auto target = keysymForCodepoint(codepoint);
        if (target == XKB_KEY_NoSymbol)
            return std::unexpected(std::format("Unicode codepoint U+{:04X} has no keyboard symbol", codepoint));

        std::optional<SRealmKeyStroke> best;
        for (xkb_keycode_t keycode = xkb_keymap_min_keycode(keymap); keycode <= xkb_keymap_max_keycode(keymap); ++keycode) {
            const auto layouts = xkb_keymap_num_layouts_for_key(keymap, keycode);
            for (xkb_layout_index_t layout = 0; layout < layouts; ++layout) {
                const auto levels = xkb_keymap_num_levels_for_key(keymap, keycode, layout);
                for (xkb_level_index_t level = 0; level < levels; ++level) {
                    const xkb_keysym_t* symbols     = nullptr;
                    const auto          symbolCount = xkb_keymap_key_get_syms_by_level(keymap, keycode, layout, level, &symbols);
                    if (symbolCount <= 0 || std::find(symbols, symbols + symbolCount, target) == symbols + symbolCount)
                        continue;

                    std::array<xkb_mod_mask_t, 16> masks{};
                    const auto                     maskCount = xkb_keymap_key_get_mods_for_level(keymap, keycode, layout, level, masks.data(), masks.size());
                    if (maskCount == 0)
                        continue;
                    const auto            selected = *std::min_element(masks.begin(), masks.begin() + std::min(maskCount, masks.size()),
                                                                       [](auto left, auto right) { return std::popcount(left) < std::popcount(right); });
                    const SRealmKeyStroke candidate{
                        .code      = keycode >= 8 ? keycode - 8 : keycode,
                        .modifiers = selected,
                        .group     = layout,
                    };
                    if (!best || std::popcount(candidate.modifiers) < std::popcount(best->modifiers))
                        best = candidate;
                }
            }
        }

        if (!best)
            return std::unexpected(std::format("the realm keyboard map cannot type Unicode codepoint U+{:04X}", codepoint));
        keyStrokes.emplace(codepoint, *best);
        return *best;
    }

    std::expected<void, std::string> typeText(std::string_view text) {
        auto codepoints = decodeUTF8(text);
        if (!codepoints)
            return std::unexpected(codepoints.error());

        std::vector<SRealmKeyStroke> strokes;
        strokes.reserve(codepoints->size());
        for (const auto codepoint : *codepoints) {
            auto stroke = keyStrokeFor(codepoint);
            if (!stroke)
                return std::unexpected(stroke.error());
            strokes.emplace_back(*stroke);
        }

        for (const auto& stroke : strokes) {
            const auto time = inputTimestamp();
            zwp_virtual_keyboard_v1_modifiers(keyboard, stroke.modifiers, 0, 0, stroke.group);
            zwp_virtual_keyboard_v1_key(keyboard, time, stroke.code, WL_KEYBOARD_KEY_STATE_PRESSED);
            zwp_virtual_keyboard_v1_key(keyboard, time, stroke.code, WL_KEYBOARD_KEY_STATE_RELEASED);
            zwp_virtual_keyboard_v1_modifiers(keyboard, 0, 0, 0, 0);
        }
        return flush();
    }

    std::expected<void, std::string> handle(const SRealmInputMessage& message) {
        const auto time = inputTimestamp();
        switch (message.type) {
            case eRealmInputMessageType::RELEASE_ALL: releaseAll(); return flush();
            case eRealmInputMessageType::POINTER_MOVE:
                zwlr_virtual_pointer_v1_motion_absolute(pointer, time, message.x, message.y, message.width, message.height);
                zwlr_virtual_pointer_v1_frame(pointer);
                return flush();
            case eRealmInputMessageType::POINTER_BUTTON:
                zwlr_virtual_pointer_v1_button(pointer, time, message.code, message.pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
                zwlr_virtual_pointer_v1_frame(pointer);
                if (message.pressed)
                    pressedButtons.emplace(message.code);
                else
                    pressedButtons.erase(message.code);
                return flush();
            case eRealmInputMessageType::POINTER_SCROLL:
                zwlr_virtual_pointer_v1_axis_source(pointer, WL_POINTER_AXIS_SOURCE_WHEEL);
                if (message.vertical != 0)
                    zwlr_virtual_pointer_v1_axis_discrete(pointer, time, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(message.vertical * 15), message.vertical);
                if (message.horizontal != 0)
                    zwlr_virtual_pointer_v1_axis_discrete(pointer, time, WL_POINTER_AXIS_HORIZONTAL_SCROLL, wl_fixed_from_int(message.horizontal * 15), message.horizontal);
                zwlr_virtual_pointer_v1_frame(pointer);
                return flush();
            case eRealmInputMessageType::KEYBOARD_KEY:
                zwp_virtual_keyboard_v1_key(keyboard, time, message.code, message.pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
                if (message.pressed)
                    pressedKeys.emplace(message.code);
                else
                    pressedKeys.erase(message.code);
                return flush();
            case eRealmInputMessageType::KEYBOARD_TYPE: return typeText(message.text);
            case eRealmInputMessageType::READY:
            case eRealmInputMessageType::ERROR: return std::unexpected("status messages cannot be sent to virtual input devices");
        }

        return std::unexpected("unknown realm input command");
    }

    void releaseAll() {
        if (!display)
            return;

        if (keyboard) {
            const auto time = inputTimestamp();
            for (const auto key : pressedKeys)
                zwp_virtual_keyboard_v1_key(keyboard, time, key, WL_KEYBOARD_KEY_STATE_RELEASED);
            zwp_virtual_keyboard_v1_modifiers(keyboard, 0, 0, 0, 0);
            pressedKeys.clear();
        }
        if (pointer) {
            const auto time = inputTimestamp();
            for (const auto button : pressedButtons)
                zwlr_virtual_pointer_v1_button(pointer, time, button, WL_POINTER_BUTTON_STATE_RELEASED);
            zwlr_virtual_pointer_v1_frame(pointer);
            pressedButtons.clear();
        }
        wl_display_flush(display);
    }

    std::expected<void, std::string> dispatch() {
        if (!display || wl_display_dispatch(display) < 0)
            return std::unexpected(std::format("realm Wayland connection failed: {}", std::strerror(errno)));
        return {};
    }

    std::expected<void, std::string> flush() {
        if (!display || wl_display_flush(display) < 0)
            return std::unexpected(std::format("failed flushing realm input events: {}", std::strerror(errno)));
        return {};
    }

    int                                 waylandFD       = -1;
    wl_display*                         display         = nullptr;
    wl_registry*                        registry        = nullptr;
    wl_seat*                            seat            = nullptr;
    zwp_virtual_keyboard_manager_v1*    keyboardManager = nullptr;
    zwlr_virtual_pointer_manager_v1*    pointerManager  = nullptr;
    zwp_virtual_keyboard_v1*            keyboard        = nullptr;
    zwlr_virtual_pointer_v1*            pointer         = nullptr;
    xkb_context*                        context         = nullptr;
    xkb_keymap*                         keymap          = nullptr;
    std::map<uint32_t, SRealmKeyStroke> keyStrokes;
    std::set<uint32_t>                  pressedKeys;
    std::set<uint32_t>                  pressedButtons;
};

CWaylandInput::CWaylandInput(int waylandFD) : m_impl(std::make_unique<SImpl>(waylandFD)) {}

CWaylandInput::~CWaylandInput() = default;

std::expected<void, std::string> CWaylandInput::initialize() {
    return m_impl->initialize();
}

std::expected<void, std::string> CWaylandInput::handle(const SRealmInputMessage& message) {
    return m_impl->handle(message);
}

std::expected<void, std::string> CWaylandInput::dispatch() {
    return m_impl->dispatch();
}

std::expected<void, std::string> CWaylandInput::flush() {
    return m_impl->flush();
}

void CWaylandInput::releaseAll() {
    m_impl->releaseAll();
}

int CWaylandInput::displayFD() const {
    return m_impl->display ? wl_display_get_fd(m_impl->display) : -1;
}
