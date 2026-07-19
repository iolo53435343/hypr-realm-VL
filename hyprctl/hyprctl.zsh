_hyprctl_cmd_0 () {
    hyprctl realms | sed -n 's/^Realm \(.*\) ([0-9]\+):$/\1/p'
}

_hyprctl_cmd_3 () {
    hyprctl monitors | awk '/Monitor/{ print $2 }'
}

_hyprctl_cmd_4 () {
    hyprctl clients | awk '/class/{print $2}'
}

_hyprctl_cmd_2 () {
    hyprctl devices | sed -n '/Keyboard at/{n; s/^\s\+//; p}'
}

_hyprctl_cmd_1 () {
    hyprpm list | awk '/Plugin/{print $4}'
}

_hyprctl () {
    local -a literals=("config-only" "cyclenext" "realms" "cursorpos" "bordersize" "renameworkspace" "animationstyle" "focuswindow" "--help" "-f" "auto" "0" "start" "swapnext" "forceallowsinput" "moveactive" "activebordercolor" "alphafullscreen" "realm" "wayland" "layers" "minsize" "monitors" "1" "kill" "settiled" "3" "focusmonitor" "swapwindow" "moveoutofgroup" "notify" "movecursor" "setcursor" "movecurrentworkspacetomonitor" "4" "seterror" "nomaxsize" "1" "forcenoanims" "setprop" "-i" "-q" "togglefloating" "3" "workspacerules" "movetoworkspace" "globalshortcuts" "resume" "movetoworkspacesilent" "disable" "workspaces" "movegroupwindow" "closewindow" "0" "0" "binds" "movewindow" "splitratio" "alpha" "denywindowfromgroup" "workspace" "configerrors" "togglegroup" "getoption" "--instance" "forceopaque" "keepaspectratio" "-h" "killactive" "pass" "event" "decorations" "devices" "focuscurrentorlast" "submap" "global" "alphafullscreenoverride" "headless" "forcerendererreload" "movewindowpixel" "version" "dpms" "resizeactive" "moveintogroup" "2" "5" "alphaoverride" "setfloating" "rollinglog" "::=" "rounding" "layouts" "moveworkspacetomonitor" "exec" "info" "alphainactiveoverride" "alterzorder" "-1" "fakefullscreen" "nofocus" "animations" "keyword" "forcenoborder" "forcenodim" "status" "--quiet" "pin" "output" "forcenoblur" "sendkeystate" "togglespecialworkspace" "fullscreen" "toggleopaque" "pause" "focusworkspaceoncurrentmonitor" "next" "changegroupactive" "-j" "instances" "execr" "exit" "clients" "descriptions" "all" "--batch" "dismissnotify" "inactivebordercolor" "switchxkblayout" "fullscreenstate" "tagwindow" "movewindoworgroup" "-r" "stop" "movefocus" "focusurgentorlast" "remove" "activeworkspace" "dispatch" "create" "centerwindow" "2" "hyprpaper" "-1" "destroy" "reload" "alphainactive" "systeminfo" "plugin" "dimaround" "activewindow" "swapactiveworkspaces" "splash" "sendshortcut" "maxsize" "lockactivegroup" "windowdancecompat" "forceopaqueoverriden" "lockgroups" "movecursortocorner" "x11" "prev" "1" "resizewindowpixel" "forcenoshadow")

    local -A descriptions
    descriptions[2]="Focus the next window on a workspace"
    descriptions[3]="List all agent realms and their lifecycle state"
    descriptions[4]="Get the current cursor pos in global layout coordinates"
    descriptions[6]="Rename a workspace"
    descriptions[8]="Focus the first window matching"
    descriptions[9]="Prints the help message"
    descriptions[14]="Swap the focused window with the next window"
    descriptions[16]="Move the active window"
    descriptions[19]="Manage an agent realm"
    descriptions[21]="List the layers"
    descriptions[23]="List active outputs with their properties"
    descriptions[25]="Get into a kill mode, where you can kill an app by clicking on it"
    descriptions[26]="Set the current window's floating state to false"
    descriptions[27]="ERROR"
    descriptions[28]="Focus a monitor"
    descriptions[29]="Swap the active window with another window"
    descriptions[30]="Move the active window out of a group"
    descriptions[31]="Send a notification using the built-in Hyprland notification system"
    descriptions[32]="Move the cursor to a specified position"
    descriptions[33]="Set the cursor theme and reloads the cursor manager"
    descriptions[34]="Move the active workspace to a monitor"
    descriptions[35]="CONFUSED"
    descriptions[36]="Set the hyprctl error string"
    descriptions[38]="Maximize no fullscreen"
    descriptions[40]="Set a property of a window"
    descriptions[41]="Specify the Hyprland instance"
    descriptions[42]="Disable output"
    descriptions[43]="Toggle the current window's floating state"
    descriptions[44]="Maximize and fullscreen"
    descriptions[45]="Get the list of defined workspace rules"
    descriptions[46]="Move the focused window to a workspace"
    descriptions[47]="Lists all global shortcuts"
    descriptions[49]="Move window doesn't switch to the workspace"
    descriptions[51]="List all workspaces with their properties"
    descriptions[52]="Swap the active window with the next or previous in a group"
    descriptions[53]="Close a specified window"
    descriptions[54]="None"
    descriptions[55]="WARNING"
    descriptions[56]="List all registered binds"
    descriptions[57]="Move the active window in a direction or to a monitor"
    descriptions[58]="Change the split ratio"
    descriptions[60]="Prohibit the active window from becoming or being inserted into group"
    descriptions[61]="Change the workspace"
    descriptions[62]="List all current config parsing errors"
    descriptions[63]="Toggle the current active window into a group"
    descriptions[64]="Get the config option status (values)"
    descriptions[65]="Specify the Hyprland instance"
    descriptions[68]="Prints the help message"
    descriptions[69]="Close the active window"
    descriptions[70]="Pass the key to a specified window"
    descriptions[71]="Emits a custom event to socket2"
    descriptions[72]="List all decorations and their info"
    descriptions[73]="List all connected keyboards and mice"
    descriptions[74]="Switch focus from current to previously focused window"
    descriptions[75]="Change the current mapping group"
    descriptions[76]="Execute a Global Shortcut using the GlobalShortcuts portal"
    descriptions[79]="Force the renderer to reload all resources and outputs"
    descriptions[80]="Move a selected window"
    descriptions[81]="Print the Hyprland version: flags, commit and branch of build"
    descriptions[82]="Set all monitors' DPMS status"
    descriptions[83]="Resize the active window"
    descriptions[84]="Move the active window into a group"
    descriptions[85]="Fullscreen"
    descriptions[86]="OK"
    descriptions[88]="Set the current window's floating state to true"
    descriptions[89]="Print tail of the log"
    descriptions[92]="List all layouts available (including plugin ones)"
    descriptions[93]="Move a workspace to a monitor"
    descriptions[94]="Execute a shell command"
    descriptions[97]="Modify the window stack order of the active or specified window"
    descriptions[98]="Current"
    descriptions[99]="Toggle the focused window's internal fullscreen state"
    descriptions[101]="Gets the current config info about animations and beziers"
    descriptions[102]="Issue a keyword to call a config keyword dynamically"
    descriptions[105]="Get internal status information like config format or backend"
    descriptions[106]="Disable output"
    descriptions[107]="Pin a window"
    descriptions[108]="Allows adding/removing fake outputs to a specific backend"
    descriptions[110]="Send a key with specific state (down/repeat/up) to a specified window (window must keep focus for events to continue)"
    descriptions[111]="Toggle a special workspace on/off"
    descriptions[112]="Toggle the focused window's fullscreen state"
    descriptions[113]="Toggle the current window to always be opaque"
    descriptions[115]="Focus the requested workspace"
    descriptions[117]="Switch to the next window in a group"
    descriptions[118]="Output in JSON format"
    descriptions[119]="List all running Hyprland instances and their info"
    descriptions[120]="Execute a raw shell command"
    descriptions[121]="Exit the compositor with no questions asked"
    descriptions[122]="List all windows with their properties"
    descriptions[123]="Return a parsable JSON with all the config options, descriptions, value types and ranges"
    descriptions[125]="Execute a batch of commands separated by ;"
    descriptions[126]="Dismiss all or up to amount of notifications"
    descriptions[128]="Set the xkb layout index for a keyboard"
    descriptions[129]="Sets the focused window’s fullscreen mode and the one sent to the client"
    descriptions[130]="Apply a tag to the window"
    descriptions[131]="Behave as moveintogroup"
    descriptions[132]="Refresh state after issuing the command"
    descriptions[134]="Move the focus in a direction"
    descriptions[135]="Focus the urgent window or the last window"
    descriptions[137]="Get the active workspace name and its properties"
    descriptions[138]="Issue a dispatch to call a keybind dispatcher with an arg"
    descriptions[140]="Center the active window"
    descriptions[141]="HINT"
    descriptions[142]="Interact with hyprpaper if present"
    descriptions[143]="No Icon"
    descriptions[145]="Force reload the config"
    descriptions[147]="Print system info"
    descriptions[148]="Interact with a plugin"
    descriptions[150]="Get the active window name and its properties"
    descriptions[151]="Swap the active workspaces between two monitors"
    descriptions[152]="Print the current random splash"
    descriptions[153]="On shortcut X sends shortcut Y to a specified window"
    descriptions[155]="Lock the focused group"
    descriptions[158]="Lock the groups"
    descriptions[159]="Move the cursor to the corner of the active window"
    descriptions[162]="INFO"
    descriptions[163]="Resize a selected window"

    local -A literal_transitions
    literal_transitions[1]="([123]=5 [126]=3 [89]=4 [3]=5 [40]=6 [42]=2 [4]=5 [92]=5 [128]=7 [45]=5 [47]=5 [132]=2 [51]=5 [9]=2 [105]=5 [56]=5 [101]=5 [102]=8 [138]=11 [137]=5 [64]=5 [62]=5 [106]=2 [108]=10 [142]=5 [19]=12 [68]=2 [145]=13 [147]=5 [21]=5 [72]=14 [73]=5 [23]=15 [148]=16 [25]=5 [150]=5 [152]=5 [31]=17 [81]=5 [118]=2 [119]=5 [33]=5 [36]=18 [122]=5 [125]=2)"
    literal_transitions[4]="([10]=5)"
    literal_transitions[6]="([87]=24 [18]=5 [39]=24 [66]=24 [67]=24 [109]=24 [127]=5 [146]=5 [91]=3 [22]=5 [149]=24 [5]=3 [7]=5 [77]=24 [154]=5 [156]=24 [96]=24 [157]=24 [100]=24 [37]=24 [59]=5 [17]=5 [103]=24 [15]=24 [104]=24 [164]=24)"
    literal_transitions[9]="([126]=3 [89]=4 [3]=5 [40]=6 [4]=5 [92]=5 [128]=7 [45]=5 [47]=5 [51]=5 [105]=5 [56]=5 [101]=5 [102]=8 [137]=5 [138]=11 [62]=5 [64]=5 [108]=10 [142]=5 [19]=12 [145]=13 [147]=5 [21]=5 [72]=14 [73]=5 [23]=15 [148]=16 [25]=5 [150]=5 [152]=5 [31]=17 [81]=5 [119]=5 [33]=5 [36]=18 [122]=5 [123]=5)"
    literal_transitions[10]="([139]=25 [136]=19)"
    literal_transitions[11]="([88]=5 [2]=5 [6]=5 [93]=5 [94]=5 [8]=5 [97]=5 [99]=5 [14]=5 [16]=5 [107]=5 [110]=5 [111]=5 [112]=5 [113]=5 [115]=5 [26]=5 [117]=5 [28]=5 [29]=5 [30]=5 [32]=5 [120]=5 [121]=5 [34]=5 [43]=5 [129]=22 [130]=5 [46]=5 [131]=5 [49]=5 [134]=5 [135]=5 [52]=5 [53]=5 [57]=5 [58]=5 [60]=5 [61]=5 [63]=5 [140]=5 [69]=5 [70]=5 [71]=5 [74]=5 [75]=5 [151]=5 [76]=5 [153]=5 [155]=5 [79]=5 [80]=5 [158]=5 [159]=5 [82]=5 [83]=5 [84]=5 [163]=5)"
    literal_transitions[12]="([48]=20 [13]=20 [95]=20 [114]=20 [133]=20 [139]=21 [144]=20)"
    literal_transitions[13]="([1]=5)"
    literal_transitions[15]="([124]=5)"
    literal_transitions[17]="([27]=3 [141]=3 [35]=3 [162]=3 [143]=3 [55]=3 [86]=3)"
    literal_transitions[18]="([50]=5)"
    literal_transitions[22]="([38]=5 [44]=5 [85]=5 [54]=5 [98]=5)"
    literal_transitions[23]="([90]=26)"
    literal_transitions[24]="([24]=5 [12]=5)"
    literal_transitions[25]="([11]=5 [78]=5 [20]=5 [160]=5)"
    literal_transitions[26]="([41]=2 [65]=2)"
    literal_transitions[27]="([161]=5 [116]=5)"

    local -A match_anything_transitions
    match_anything_transitions=([27]=5 [1]=9 [20]=5 [16]=5 [21]=5 [7]=27 [18]=23 [4]=23 [9]=9 [3]=5 [19]=5 [8]=5 [14]=5 [5]=23 [15]=23 [13]=23)

    declare -A subword_transitions

    local state=1
    local word_index=2
    while [[ $word_index -lt $CURRENT ]]; do
        if [[ -v "literal_transitions[$state]" ]]; then
            local -A state_transitions
            eval "state_transitions=${literal_transitions[$state]}"

            local word=${words[$word_index]}
            local word_matched=0
            for ((literal_id = 1; literal_id <= $#literals; literal_id++)); do
                if [[ ${literals[$literal_id]} = "$word" ]]; then
                    if [[ -v "state_transitions[$literal_id]" ]]; then
                        state=${state_transitions[$literal_id]}
                        word_index=$((word_index + 1))
                        word_matched=1
                        break
                    fi
                fi
            done
            if [[ $word_matched -ne 0 ]]; then
                continue
            fi
        fi

        if [[ -v "match_anything_transitions[$state]" ]]; then
            state=${match_anything_transitions[$state]}
            word_index=$((word_index + 1))
            continue
        fi

        return 1
    done

    completions_no_description_trailing_space=()
    completions_no_description_no_trailing_space=()
    completions_trailing_space=()
    suffixes_trailing_space=()
    descriptions_trailing_space=()
    completions_no_trailing_space=()
    suffixes_no_trailing_space=()
    descriptions_no_trailing_space=()

    if [[ -v "literal_transitions[$state]" ]]; then
        local -A state_transitions
        eval "state_transitions=${literal_transitions[$state]}"

        for literal_id in ${(k)state_transitions}; do
            if [[ -v "descriptions[$literal_id]" ]]; then
                completions_trailing_space+=("${literals[$literal_id]}")
                suffixes_trailing_space+=("${literals[$literal_id]}")
                descriptions_trailing_space+=("${descriptions[$literal_id]}")
            else
                completions_no_description_trailing_space+=("${literals[$literal_id]}")
            fi
        done
    fi
    local -A commands=([16]=1 [19]=3 [20]=0 [14]=4 [7]=2)

    if [[ -v "commands[$state]" ]]; then
        local command_id=${commands[$state]}
        local output=$(_hyprctl_cmd_${command_id} "${words[$CURRENT]}")
        local -a command_completions=("${(@f)output}")
        for line in ${command_completions[@]}; do
            local parts=(${(@s:	:)line})
            if [[ -v "parts[2]" ]]; then
                completions_trailing_space+=("${parts[1]}")
                suffixes_trailing_space+=("${parts[1]}")
                descriptions_trailing_space+=("${parts[2]}")
            else
                completions_no_description_trailing_space+=("${parts[1]}")
            fi
        done
    fi

    local maxlen=0
    for suffix in ${suffixes_trailing_space[@]}; do
        if [[ ${#suffix} -gt $maxlen ]]; then
            maxlen=${#suffix}
        fi
    done
    for suffix in ${suffixes_no_trailing_space[@]}; do
        if [[ ${#suffix} -gt $maxlen ]]; then
            maxlen=${#suffix}
        fi
    done

    for ((i = 1; i <= $#suffixes_trailing_space; i++)); do
        if [[ -z ${descriptions_trailing_space[$i]} ]]; then
            descriptions_trailing_space[$i]="${(r($maxlen)( ))${suffixes_trailing_space[$i]}}"
        else
            descriptions_trailing_space[$i]="${(r($maxlen)( ))${suffixes_trailing_space[$i]}} -- ${descriptions_trailing_space[$i]}"
        fi
    done

    for ((i = 1; i <= $#suffixes_no_trailing_space; i++)); do
        if [[ -z ${descriptions_no_trailing_space[$i]} ]]; then
            descriptions_no_trailing_space[$i]="${(r($maxlen)( ))${suffixes_no_trailing_space[$i]}}"
        else
            descriptions_no_trailing_space[$i]="${(r($maxlen)( ))${suffixes_no_trailing_space[$i]}} -- ${descriptions_no_trailing_space[$i]}"
        fi
    done

    compadd -Q -a completions_no_description_trailing_space
    compadd -Q -S ' ' -a completions_no_description_no_trailing_space
    compadd -l -Q -a -d descriptions_trailing_space completions_trailing_space
    compadd -l -Q -S '' -a -d descriptions_no_trailing_space completions_no_trailing_space
    return 0
}

compdef _hyprctl hyprctl
