-- Minimal configuration for a manually launched nested agent realm.
-- scripts/run-agent-realm.sh isolates this compositor's runtime and logs.

local terminal = os.getenv("HYPRLAND_REALM_TERMINAL")
if terminal == nil or terminal == "" then
    terminal = "kitty --class agent-realm --name agent-realm --title 'Agent Realm Terminal'"
end

hl.monitor({
    output = "",
    mode = "1280x720@60",
    position = "0x0",
    scale = "1",
})

-- Keep the last matching rule specific so the generic rule cannot re-enable
-- Hyprland's synthetic output after the host closes the realm window.
hl.monitor({
    output = "FALLBACK",
    disabled = true,
})

hl.config({
    general = {
        border_size = 4,
        gaps_in = 4,
        gaps_out = 8,
        col = {
            active_border = "rgba(ff3ac8ff)",
            inactive_border = "rgba(6f3dc4dd)",
        },
    },

    decoration = {
        rounding = 6,
        shadow = {
            enabled = false,
        },
        blur = {
            enabled = false,
        },
    },

    animations = {
        enabled = false,
    },

    misc = {
        background_color = "rgba(17111fff)",
        disable_hyprland_logo = true,
        disable_splash_rendering = true,
        disable_watchdog_warning = true,
        disable_xdg_env_checks = true,
        force_default_wallpaper = -1,
    },

    debug = {
        disable_logs = false,
        enable_stdout_logs = true,
    },
})

hl.on("hyprland.start", function()
    hl.exec_cmd(terminal)
end)

-- Intentionally no keybindings: host shortcuts remain authoritative.
