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
    if [[ $(type -t _get_comp_words_by_ref) != function ]]; then
        echo _get_comp_words_by_ref: function not defined.  Make sure the bash-completions system package is installed
        return 1
    fi

    local words cword
    _get_comp_words_by_ref -n "$COMP_WORDBREAKS" words cword

    local -a literals=("config-only" "cyclenext" "realms" "cursorpos" "bordersize" "renameworkspace" "animationstyle" "focuswindow" "--help" "-f" "auto" "0" "start" "swapnext" "forceallowsinput" "moveactive" "activebordercolor" "alphafullscreen" "realm" "wayland" "layers" "minsize" "monitors" "1" "kill" "settiled" "3" "focusmonitor" "swapwindow" "moveoutofgroup" "notify" "movecursor" "setcursor" "movecurrentworkspacetomonitor" "4" "seterror" "nomaxsize" "1" "forcenoanims" "setprop" "-i" "-q" "togglefloating" "3" "workspacerules" "movetoworkspace" "globalshortcuts" "resume" "movetoworkspacesilent" "disable" "workspaces" "movegroupwindow" "closewindow" "0" "0" "binds" "movewindow" "splitratio" "alpha" "denywindowfromgroup" "workspace" "configerrors" "togglegroup" "getoption" "--instance" "forceopaque" "keepaspectratio" "-h" "killactive" "pass" "event" "decorations" "devices" "focuscurrentorlast" "submap" "global" "alphafullscreenoverride" "headless" "forcerendererreload" "movewindowpixel" "version" "dpms" "resizeactive" "moveintogroup" "2" "5" "alphaoverride" "setfloating" "rollinglog" "::=" "rounding" "layouts" "moveworkspacetomonitor" "exec" "info" "alphainactiveoverride" "alterzorder" "-1" "fakefullscreen" "nofocus" "animations" "keyword" "forcenoborder" "forcenodim" "status" "--quiet" "pin" "output" "forcenoblur" "sendkeystate" "togglespecialworkspace" "fullscreen" "toggleopaque" "pause" "focusworkspaceoncurrentmonitor" "next" "changegroupactive" "-j" "instances" "execr" "exit" "clients" "descriptions" "all" "--batch" "dismissnotify" "inactivebordercolor" "switchxkblayout" "fullscreenstate" "tagwindow" "movewindoworgroup" "-r" "stop" "movefocus" "focusurgentorlast" "remove" "activeworkspace" "dispatch" "create" "centerwindow" "2" "hyprpaper" "-1" "destroy" "reload" "alphainactive" "systeminfo" "plugin" "dimaround" "activewindow" "swapactiveworkspaces" "splash" "sendshortcut" "maxsize" "lockactivegroup" "windowdancecompat" "forceopaqueoverriden" "lockgroups" "movecursortocorner" "x11" "prev" "1" "resizewindowpixel" "forcenoshadow")

    declare -A literal_transitions
    literal_transitions[0]="([122]=4 [125]=2 [88]=3 [2]=4 [39]=5 [41]=1 [3]=4 [91]=4 [127]=6 [44]=4 [46]=4 [131]=1 [50]=4 [8]=1 [104]=4 [55]=4 [100]=4 [101]=7 [137]=10 [136]=4 [63]=4 [61]=4 [105]=1 [107]=9 [141]=4 [18]=11 [67]=1 [144]=12 [146]=4 [20]=4 [71]=13 [72]=4 [22]=14 [147]=15 [24]=4 [149]=4 [151]=4 [30]=16 [80]=4 [117]=1 [118]=4 [32]=4 [35]=17 [121]=4 [124]=1)"
    literal_transitions[3]="([9]=4)"
    literal_transitions[5]="([86]=23 [17]=4 [38]=23 [65]=23 [66]=23 [108]=23 [126]=4 [145]=4 [90]=2 [21]=4 [148]=23 [4]=2 [6]=4 [76]=23 [153]=4 [155]=23 [95]=23 [156]=23 [99]=23 [36]=23 [58]=4 [16]=4 [102]=23 [14]=23 [103]=23 [163]=23)"
    literal_transitions[8]="([125]=2 [88]=3 [2]=4 [39]=5 [3]=4 [91]=4 [127]=6 [44]=4 [46]=4 [50]=4 [104]=4 [55]=4 [100]=4 [101]=7 [136]=4 [137]=10 [61]=4 [63]=4 [107]=9 [141]=4 [18]=11 [144]=12 [146]=4 [20]=4 [71]=13 [72]=4 [22]=14 [147]=15 [24]=4 [149]=4 [151]=4 [30]=16 [80]=4 [118]=4 [32]=4 [35]=17 [121]=4 [122]=4)"
    literal_transitions[9]="([138]=24 [135]=18)"
    literal_transitions[10]="([87]=4 [1]=4 [5]=4 [92]=4 [93]=4 [7]=4 [96]=4 [98]=4 [13]=4 [15]=4 [106]=4 [109]=4 [110]=4 [111]=4 [112]=4 [114]=4 [25]=4 [116]=4 [27]=4 [28]=4 [29]=4 [31]=4 [119]=4 [120]=4 [33]=4 [42]=4 [128]=21 [129]=4 [45]=4 [130]=4 [48]=4 [133]=4 [134]=4 [51]=4 [52]=4 [56]=4 [57]=4 [59]=4 [60]=4 [62]=4 [139]=4 [68]=4 [69]=4 [70]=4 [73]=4 [74]=4 [150]=4 [75]=4 [152]=4 [154]=4 [78]=4 [79]=4 [157]=4 [158]=4 [81]=4 [82]=4 [83]=4 [162]=4)"
    literal_transitions[11]="([47]=19 [12]=19 [94]=19 [113]=19 [132]=19 [138]=20 [143]=19)"
    literal_transitions[12]="([0]=4)"
    literal_transitions[14]="([123]=4)"
    literal_transitions[16]="([26]=2 [140]=2 [34]=2 [161]=2 [142]=2 [54]=2 [85]=2)"
    literal_transitions[17]="([49]=4)"
    literal_transitions[21]="([37]=4 [43]=4 [84]=4 [53]=4 [97]=4)"
    literal_transitions[22]="([89]=25)"
    literal_transitions[23]="([23]=4 [11]=4)"
    literal_transitions[24]="([10]=4 [77]=4 [19]=4 [159]=4)"
    literal_transitions[25]="([40]=1 [64]=1)"
    literal_transitions[26]="([160]=4 [115]=4)"

    declare -A match_anything_transitions
    match_anything_transitions=([26]=4 [0]=8 [19]=4 [15]=4 [20]=4 [6]=26 [17]=22 [3]=22 [8]=8 [2]=4 [18]=4 [7]=4 [13]=4 [4]=22 [14]=22 [12]=22)
    declare -A subword_transitions

    local state=0
    local word_index=1
    while [[ $word_index -lt $cword ]]; do
        local word=${words[$word_index]}

        if [[ -v "literal_transitions[$state]" ]]; then
            declare -A state_transitions
            eval "state_transitions=${literal_transitions[$state]}"

            local word_matched=0
            for literal_id in $(seq 0 $((${#literals[@]} - 1))); do
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


    local prefix="${words[$cword]}"

    local shortest_suffix="$word"
    for ((i=0; i < ${#COMP_WORDBREAKS}; i++)); do
        local char="${COMP_WORDBREAKS:$i:1}"
        local candidate="${word##*$char}"
        if [[ ${#candidate} -lt ${#shortest_suffix} ]]; then
            shortest_suffix=$candidate
        fi
    done
    local superfluous_prefix=""
    if [[ "$shortest_suffix" != "$word" ]]; then
        local superfluous_prefix=${word%$shortest_suffix}
    fi

    if [[ -v "literal_transitions[$state]" ]]; then
        local state_transitions_initializer=${literal_transitions[$state]}
        declare -A state_transitions
        eval "state_transitions=$state_transitions_initializer"

        for literal_id in "${!state_transitions[@]}"; do
            local literal="${literals[$literal_id]}"
            if [[ $literal = "${prefix}"* ]]; then
                local completion=${literal#"$superfluous_prefix"}
                COMPREPLY+=("$completion ")
            fi
        done
    fi
    declare -A commands
    commands=([15]=1 [18]=3 [19]=0 [13]=4 [6]=2)
    if [[ -v "commands[$state]" ]]; then
        local command_id=${commands[$state]}
        local completions=()
        mapfile -t completions < <(_hyprctl_cmd_${command_id} "$prefix" | cut -f1)
        for item in "${completions[@]}"; do
            if [[ $item = "${prefix}"* ]]; then
                COMPREPLY+=("$item")
            fi
        done
    fi


    return 0
}

complete -o nospace -F _hyprctl hyprctl
