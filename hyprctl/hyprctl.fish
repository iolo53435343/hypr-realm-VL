function _hyprctl_1
    set 1 $argv[1]
    hyprctl realms | sed -n 's/^Realm \(.*\) ([0-9]\+):$/\1/p'
end

function _hyprctl_4
    set 1 $argv[1]
    hyprctl monitors | awk '/Monitor/{ print $2 }'
end

function _hyprctl_5
    set 1 $argv[1]
    hyprctl clients | awk '/class/{print $2}'
end

function _hyprctl_3
    set 1 $argv[1]
    hyprctl devices | sed -n '/Keyboard at/{n; s/^\s\+//; p}'
end

function _hyprctl_2
    set 1 $argv[1]
    hyprpm list | awk '/Plugin/{print $4}'
end

function _hyprctl
    set COMP_LINE (commandline --cut-at-cursor)

    set COMP_WORDS
    echo $COMP_LINE | read --tokenize --array COMP_WORDS
    if string match --quiet --regex '.*\s$' $COMP_LINE
        set COMP_CWORD (math (count $COMP_WORDS) + 1)
    else
        set COMP_CWORD (count $COMP_WORDS)
    end

    set --local literals "config-only" "cyclenext" "realms" "cursorpos" "bordersize" "renameworkspace" "animationstyle" "focuswindow" "--help" "-f" "auto" "0" "start" "swapnext" "forceallowsinput" "moveactive" "activebordercolor" "alphafullscreen" "realm" "wayland" "layers" "minsize" "monitors" "1" "kill" "settiled" "3" "focusmonitor" "swapwindow" "moveoutofgroup" "notify" "movecursor" "setcursor" "movecurrentworkspacetomonitor" "4" "seterror" "nomaxsize" "1" "forcenoanims" "setprop" "-i" "-q" "togglefloating" "3" "workspacerules" "movetoworkspace" "globalshortcuts" "resume" "movetoworkspacesilent" "disable" "workspaces" "movegroupwindow" "closewindow" "0" "0" "binds" "movewindow" "splitratio" "alpha" "denywindowfromgroup" "workspace" "configerrors" "togglegroup" "getoption" "--instance" "forceopaque" "keepaspectratio" "-h" "killactive" "pass" "event" "decorations" "devices" "focuscurrentorlast" "submap" "global" "alphafullscreenoverride" "headless" "forcerendererreload" "movewindowpixel" "version" "dpms" "resizeactive" "moveintogroup" "2" "5" "alphaoverride" "setfloating" "rollinglog" "::=" "rounding" "layouts" "moveworkspacetomonitor" "exec" "info" "alphainactiveoverride" "alterzorder" "-1" "fakefullscreen" "nofocus" "animations" "keyword" "forcenoborder" "forcenodim" "status" "--quiet" "pin" "output" "forcenoblur" "sendkeystate" "togglespecialworkspace" "fullscreen" "toggleopaque" "pause" "focusworkspaceoncurrentmonitor" "next" "changegroupactive" "-j" "instances" "execr" "exit" "clients" "descriptions" "all" "--batch" "dismissnotify" "inactivebordercolor" "switchxkblayout" "fullscreenstate" "tagwindow" "movewindoworgroup" "-r" "stop" "movefocus" "focusurgentorlast" "remove" "activeworkspace" "dispatch" "create" "centerwindow" "2" "hyprpaper" "-1" "destroy" "reload" "alphainactive" "systeminfo" "plugin" "dimaround" "activewindow" "swapactiveworkspaces" "splash" "sendshortcut" "maxsize" "lockactivegroup" "windowdancecompat" "forceopaqueoverriden" "lockgroups" "movecursortocorner" "x11" "prev" "1" "resizewindowpixel" "forcenoshadow"

    set --local descriptions
    set descriptions[2] "Focus the next window on a workspace"
    set descriptions[3] "List all agent realms and their lifecycle state"
    set descriptions[4] "Get the current cursor pos in global layout coordinates"
    set descriptions[6] "Rename a workspace"
    set descriptions[8] "Focus the first window matching"
    set descriptions[9] "Prints the help message"
    set descriptions[14] "Swap the focused window with the next window"
    set descriptions[16] "Move the active window"
    set descriptions[19] "Manage an agent realm"
    set descriptions[21] "List the layers"
    set descriptions[23] "List active outputs with their properties"
    set descriptions[25] "Get into a kill mode, where you can kill an app by clicking on it"
    set descriptions[26] "Set the current window's floating state to false"
    set descriptions[27] "ERROR"
    set descriptions[28] "Focus a monitor"
    set descriptions[29] "Swap the active window with another window"
    set descriptions[30] "Move the active window out of a group"
    set descriptions[31] "Send a notification using the built-in Hyprland notification system"
    set descriptions[32] "Move the cursor to a specified position"
    set descriptions[33] "Set the cursor theme and reloads the cursor manager"
    set descriptions[34] "Move the active workspace to a monitor"
    set descriptions[35] "CONFUSED"
    set descriptions[36] "Set the hyprctl error string"
    set descriptions[38] "Maximize no fullscreen"
    set descriptions[40] "Set a property of a window"
    set descriptions[41] "Specify the Hyprland instance"
    set descriptions[42] "Disable output"
    set descriptions[43] "Toggle the current window's floating state"
    set descriptions[44] "Maximize and fullscreen"
    set descriptions[45] "Get the list of defined workspace rules"
    set descriptions[46] "Move the focused window to a workspace"
    set descriptions[47] "Lists all global shortcuts"
    set descriptions[49] "Move window doesn't switch to the workspace"
    set descriptions[51] "List all workspaces with their properties"
    set descriptions[52] "Swap the active window with the next or previous in a group"
    set descriptions[53] "Close a specified window"
    set descriptions[54] "None"
    set descriptions[55] "WARNING"
    set descriptions[56] "List all registered binds"
    set descriptions[57] "Move the active window in a direction or to a monitor"
    set descriptions[58] "Change the split ratio"
    set descriptions[60] "Prohibit the active window from becoming or being inserted into group"
    set descriptions[61] "Change the workspace"
    set descriptions[62] "List all current config parsing errors"
    set descriptions[63] "Toggle the current active window into a group"
    set descriptions[64] "Get the config option status (values)"
    set descriptions[65] "Specify the Hyprland instance"
    set descriptions[68] "Prints the help message"
    set descriptions[69] "Close the active window"
    set descriptions[70] "Pass the key to a specified window"
    set descriptions[71] "Emits a custom event to socket2"
    set descriptions[72] "List all decorations and their info"
    set descriptions[73] "List all connected keyboards and mice"
    set descriptions[74] "Switch focus from current to previously focused window"
    set descriptions[75] "Change the current mapping group"
    set descriptions[76] "Execute a Global Shortcut using the GlobalShortcuts portal"
    set descriptions[79] "Force the renderer to reload all resources and outputs"
    set descriptions[80] "Move a selected window"
    set descriptions[81] "Print the Hyprland version: flags, commit and branch of build"
    set descriptions[82] "Set all monitors' DPMS status"
    set descriptions[83] "Resize the active window"
    set descriptions[84] "Move the active window into a group"
    set descriptions[85] "Fullscreen"
    set descriptions[86] "OK"
    set descriptions[88] "Set the current window's floating state to true"
    set descriptions[89] "Print tail of the log"
    set descriptions[92] "List all layouts available (including plugin ones)"
    set descriptions[93] "Move a workspace to a monitor"
    set descriptions[94] "Execute a shell command"
    set descriptions[97] "Modify the window stack order of the active or specified window"
    set descriptions[98] "Current"
    set descriptions[99] "Toggle the focused window's internal fullscreen state"
    set descriptions[101] "Gets the current config info about animations and beziers"
    set descriptions[102] "Issue a keyword to call a config keyword dynamically"
    set descriptions[105] "Get internal status information like config format or backend"
    set descriptions[106] "Disable output"
    set descriptions[107] "Pin a window"
    set descriptions[108] "Allows adding/removing fake outputs to a specific backend"
    set descriptions[110] "Send a key with specific state (down/repeat/up) to a specified window (window must keep focus for events to continue)"
    set descriptions[111] "Toggle a special workspace on/off"
    set descriptions[112] "Toggle the focused window's fullscreen state"
    set descriptions[113] "Toggle the current window to always be opaque"
    set descriptions[115] "Focus the requested workspace"
    set descriptions[117] "Switch to the next window in a group"
    set descriptions[118] "Output in JSON format"
    set descriptions[119] "List all running Hyprland instances and their info"
    set descriptions[120] "Execute a raw shell command"
    set descriptions[121] "Exit the compositor with no questions asked"
    set descriptions[122] "List all windows with their properties"
    set descriptions[123] "Return a parsable JSON with all the config options, descriptions, value types and ranges"
    set descriptions[125] "Execute a batch of commands separated by ;"
    set descriptions[126] "Dismiss all or up to amount of notifications"
    set descriptions[128] "Set the xkb layout index for a keyboard"
    set descriptions[129] "Sets the focused window’s fullscreen mode and the one sent to the client"
    set descriptions[130] "Apply a tag to the window"
    set descriptions[131] "Behave as moveintogroup"
    set descriptions[132] "Refresh state after issuing the command"
    set descriptions[134] "Move the focus in a direction"
    set descriptions[135] "Focus the urgent window or the last window"
    set descriptions[137] "Get the active workspace name and its properties"
    set descriptions[138] "Issue a dispatch to call a keybind dispatcher with an arg"
    set descriptions[140] "Center the active window"
    set descriptions[141] "HINT"
    set descriptions[142] "Interact with hyprpaper if present"
    set descriptions[143] "No Icon"
    set descriptions[145] "Force reload the config"
    set descriptions[147] "Print system info"
    set descriptions[148] "Interact with a plugin"
    set descriptions[150] "Get the active window name and its properties"
    set descriptions[151] "Swap the active workspaces between two monitors"
    set descriptions[152] "Print the current random splash"
    set descriptions[153] "On shortcut X sends shortcut Y to a specified window"
    set descriptions[155] "Lock the focused group"
    set descriptions[158] "Lock the groups"
    set descriptions[159] "Move the cursor to the corner of the active window"
    set descriptions[162] "INFO"
    set descriptions[163] "Resize a selected window"

    set --local literal_transitions
    set literal_transitions[1] "set inputs 123 126 89 3 40 42 4 92 128 45 47 132 51 9 105 56 101 102 138 137 64 62 106 108 142 19 68 145 147 21 72 73 23 148 25 150 152 31 81 118 119 33 36 122 125; set tos 5 3 4 5 6 2 5 5 7 5 5 2 5 2 5 5 5 8 11 5 5 5 2 10 5 12 2 13 5 5 14 5 15 16 5 5 5 17 5 2 5 5 18 5 2"
    set literal_transitions[4] "set inputs 10; set tos 5"
    set literal_transitions[6] "set inputs 87 18 39 66 67 109 127 146 91 22 149 5 7 77 154 156 96 157 100 37 59 17 103 15 104 164; set tos 24 5 24 24 24 24 5 5 3 5 24 3 5 24 5 24 24 24 24 24 5 5 24 24 24 24"
    set literal_transitions[9] "set inputs 126 89 3 40 4 92 128 45 47 51 105 56 101 102 137 138 62 64 108 142 19 145 147 21 72 73 23 148 25 150 152 31 81 119 33 36 122 123; set tos 3 4 5 6 5 5 7 5 5 5 5 5 5 8 5 11 5 5 10 5 12 13 5 5 14 5 15 16 5 5 5 17 5 5 5 18 5 5"
    set literal_transitions[10] "set inputs 139 136; set tos 25 19"
    set literal_transitions[11] "set inputs 88 2 6 93 94 8 97 99 14 16 107 110 111 112 113 115 26 117 28 29 30 32 120 121 34 43 129 130 46 131 49 134 135 52 53 57 58 60 61 63 140 69 70 71 74 75 151 76 153 155 79 80 158 159 82 83 84 163; set tos 5 5 5 5 5 5 5 5 5 5 5 5 5 5 5 5 5 5 5 5 5 5 5 5 5 5 22 5 5 5 5 5 5 5 5 5 5 5 5 5 5 5 5 5 5 5 5 5 5 5 5 5 5 5 5 5 5 5"
    set literal_transitions[12] "set inputs 48 13 95 114 133 139 144; set tos 20 20 20 20 20 21 20"
    set literal_transitions[13] "set inputs 1; set tos 5"
    set literal_transitions[15] "set inputs 124; set tos 5"
    set literal_transitions[17] "set inputs 27 141 35 162 143 55 86; set tos 3 3 3 3 3 3 3"
    set literal_transitions[18] "set inputs 50; set tos 5"
    set literal_transitions[22] "set inputs 38 44 85 54 98; set tos 5 5 5 5 5"
    set literal_transitions[23] "set inputs 90; set tos 26"
    set literal_transitions[24] "set inputs 24 12; set tos 5 5"
    set literal_transitions[25] "set inputs 11 78 20 160; set tos 5 5 5 5"
    set literal_transitions[26] "set inputs 41 65; set tos 2 2"
    set literal_transitions[27] "set inputs 161 116; set tos 5 5"

    set --local match_anything_transitions_from 27 1 20 16 21 7 18 4 9 3 19 8 14 5 15 13
    set --local match_anything_transitions_to 5 9 5 5 5 27 23 23 9 5 5 5 5 23 23 23

    set --local state 1
    set --local word_index 2
    while test $word_index -lt $COMP_CWORD
        set --local -- word $COMP_WORDS[$word_index]

        if set --query literal_transitions[$state] && test -n $literal_transitions[$state]
            set --local --erase inputs
            set --local --erase tos
            eval $literal_transitions[$state]

            if contains -- $word $literals
                set --local literal_matched 0
                for literal_id in (seq 1 (count $literals))
                    if test $literals[$literal_id] = $word
                        set --local index (contains --index -- $literal_id $inputs)
                        set state $tos[$index]
                        set word_index (math $word_index + 1)
                        set literal_matched 1
                        break
                    end
                end
                if test $literal_matched -ne 0
                    continue
                end
            end
        end

        if set --query match_anything_transitions_from[$state] && test -n $match_anything_transitions_from[$state]
            set --local index (contains --index -- $state $match_anything_transitions_from)
            set state $match_anything_transitions_to[$index]
            set word_index (math $word_index + 1)
            continue
        end

        return 1
    end

    if set --query literal_transitions[$state] && test -n $literal_transitions[$state]
        set --local --erase inputs
        set --local --erase tos
        eval $literal_transitions[$state]
        for literal_id in $inputs
            if test -n $descriptions[$literal_id]
                printf '%s\t%s\n' $literals[$literal_id] $descriptions[$literal_id]
            else
                printf '%s\n' $literals[$literal_id]
            end
        end
    end

    set command_states 16 19 20 14 7
    set command_ids 2 4 1 5 3
    if contains $state $command_states
        set --local index (contains --index $state $command_states)
        set --local function_id $command_ids[$index]
        set --local function_name _hyprctl_$function_id
        set --local --erase inputs
        set --local --erase tos
        $function_name "$COMP_WORDS[$COMP_CWORD]"
    end

    return 0
end

complete --command hyprctl --no-files --arguments "(_hyprctl)"
