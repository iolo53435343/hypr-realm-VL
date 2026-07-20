#pragma once

#include <string_view>

namespace Realm {
    inline constexpr std::string_view REALM_CONTROL_SOCKET_NAME = ".realm.sock";

    // The control socket must fit whenever Hyprland's core IPC socket fits.
    static_assert(REALM_CONTROL_SOCKET_NAME.size() <= std::string_view{".socket.sock"}.size());
}
