#!/usr/bin/env bash

set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
readonly CONFIG_TEMPLATE="${REPO_ROOT}/example/agent-realm.lua"

realm_runtime=""
realm_launcher_pid=""
realm_compositor_pid=""
realm_process_group_id=""
realm_closed_by_host=0

usage() {
    cat <<'EOF'
Usage: scripts/run-agent-realm.sh [HYPRLAND_BINARY]

Launch the built Hyprland compositor as an isolated nested agent realm.

Environment overrides:
  HYPRLAND_REALM_BINARY    Hyprland binary (default: ./build/Hyprland)
  HYPRLAND_REALM_TERMINAL  Command launched inside the realm
  HYPRLAND_REALM_HOST_HYPRCTL  Host-compatible hyprctl binary
EOF
}

process_group_exists() {
    local pid="$1"
    [[ "${pid}" =~ ^[0-9]+$ ]] && ((pid > 1)) && kill -0 -- "-${pid}" 2>/dev/null
}

process_is_running() {
    local pid="$1"
    local state=""

    if ! kill -0 "${pid}" 2>/dev/null; then
        return 1
    fi

    state="$(ps -o stat= -p "${pid}" 2>/dev/null || true)"
    [[ -n "${state}" && "${state}" != Z* ]]
}

process_group_has_live_members() {
    local pid="$1"

    ps -eo pgid=,stat= | awk -v pgid="${pid}" '$1 == pgid && $2 !~ /^Z/ { found = 1 } END { exit !found }'
}

stop_process_group() {
    local pid="$1"

    if ! process_group_exists "${pid}"; then
        return
    fi

    kill -TERM -- "-${pid}" 2>/dev/null || true
    for _ in {1..50}; do
        if ! process_group_has_live_members "${pid}"; then
            return
        fi
        sleep 0.1
    done

    if process_group_has_live_members "${pid}"; then
        kill -KILL -- "-${pid}" 2>/dev/null || true
    fi
}

stop_realm() {
    local pid="$1"
    local process_group_id="$2"

    if [[ "${pid}" =~ ^[0-9]+$ ]] && ((pid > 1)) && process_is_running "${pid}"; then
        kill -TERM "${pid}" 2>/dev/null || true
    fi

    # Terminate realm applications at the same time as the compositor. Waiting
    # for the compositor first can leave its process group alive long enough for
    # an external launcher supervisor to time out before cleanup completes.
    stop_process_group "${process_group_id}"
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM HUP

    if [[ -n "${realm_compositor_pid}" ]]; then
        stop_realm "${realm_compositor_pid}" "${realm_process_group_id}"
    elif [[ -n "${realm_process_group_id}" ]]; then
        stop_process_group "${realm_process_group_id}"
    fi

    if [[ -n "${realm_runtime}" && "${realm_runtime}" == "${host_runtime}"/ar.* ]]; then
        rm -rf -- "${realm_runtime}"
    fi

    return "${status}"
}

handle_signal() {
    exit 130
}

wait_for_realm() {
    local lock_file=""
    local socket_path=""

    for _ in {1..200}; do
        if ! kill -0 "${realm_launcher_pid}" 2>/dev/null; then
            return 1
        fi

        lock_file="$(find "${realm_runtime}/hypr" -mindepth 2 -maxdepth 2 -name hyprland.lock -print -quit 2>/dev/null || true)"
        if [[ -n "${lock_file}" ]]; then
            realm_compositor_pid="$(sed -n '1p' "${lock_file}")"
            realm_wayland_socket="$(sed -n '2p' "${lock_file}")"

            if [[ "${realm_wayland_socket}" == /* ]]; then
                socket_path="${realm_wayland_socket}"
            else
                socket_path="${realm_runtime}/${realm_wayland_socket}"
            fi

            if [[ "${realm_compositor_pid}" =~ ^[0-9]+$ && -S "${socket_path}" ]]; then
                realm_process_group_id="${realm_compositor_pid}"
                realm_socket_path="${socket_path}"
                realm_lock_file="${lock_file}"
                return 0
            fi
        fi

        sleep 0.05
    done

    return 1
}

launch_realm() {
    local status=0

    set +e
    env \
        XDG_RUNTIME_DIR="${realm_runtime}" \
        XDG_CACHE_HOME="${realm_runtime}/cache" \
        XDG_STATE_HOME="${realm_runtime}/state" \
        WAYLAND_DISPLAY="${host_wayland_socket}" \
        PATH="${realm_runtime}/bin:${PATH}" \
        HYPRLAND_REALM_ID="manual" \
        HYPRLAND_REALM_NAME="manual" \
        HYPRLAND_REALM_ENV_GUARD_LOG="${realm_runtime}/environment-guard.log" \
        HYPRLAND_NO_RT=1 \
        HYPRLAND_NO_SD_VARS=1 \
        setsid --wait \
        "${hyprland_binary}" --config "${realm_config}" >"${realm_log}" 2>&1
    status=$?
    set -e

    return "${status}"
}

host_window_is_present() {
    local clients=""

    if ! clients="$("${host_hyprctl}" -j clients 2>/dev/null)"; then
        return 2
    fi

    grep -Eq '"pid"[[:space:]]*:[[:space:]]*'"${realm_compositor_pid}"'([,}])' <<<"${clients}"
}

wait_for_host_window() {
    if [[ -z "${host_hyprctl}" || -z "${HYPRLAND_INSTANCE_SIGNATURE:-}" ]]; then
        return 1
    fi

    for _ in {1..100}; do
        if host_window_is_present; then
            return 0
        fi
        sleep 0.05
    done

    return 1
}

create_environment_guard() {
    local command_name="$1"
    local guard_path="${realm_runtime}/bin/${command_name}"

    printf '%s\n' \
        '#!/bin/sh' \
        'printf '\''%s\n'\'' "${0##*/}" >> "${HYPRLAND_REALM_ENV_GUARD_LOG:-/dev/null}"' \
        'exit 0' >"${guard_path}"
    chmod 700 "${guard_path}"
}

if (($# > 1)); then
    usage >&2
    exit 2
fi

if (($# == 1)) && [[ "$1" == "-h" || "$1" == "--help" ]]; then
    usage
    exit 0
fi

if [[ -z "${XDG_RUNTIME_DIR:-}" ]]; then
    printf 'error: XDG_RUNTIME_DIR is not set\n' >&2
    exit 1
fi
readonly host_runtime="${XDG_RUNTIME_DIR}"

if [[ -z "${WAYLAND_DISPLAY:-}" ]]; then
    printf 'error: WAYLAND_DISPLAY is not set; run this from a Wayland session\n' >&2
    exit 1
fi

if [[ "${WAYLAND_DISPLAY}" == /* ]]; then
    readonly host_wayland_socket="${WAYLAND_DISPLAY}"
else
    readonly host_wayland_socket="${host_runtime}/${WAYLAND_DISPLAY}"
fi

if [[ ! -S "${host_wayland_socket}" ]]; then
    printf 'error: host Wayland socket does not exist: %s\n' "${host_wayland_socket}" >&2
    exit 1
fi

hyprland_binary="${1:-${HYPRLAND_REALM_BINARY:-${REPO_ROOT}/build/Hyprland}}"
if [[ ! -x "${hyprland_binary}" ]]; then
    printf 'error: Hyprland binary is not executable: %s\n' "${hyprland_binary}" >&2
    printf 'hint: run make debug inside nix develop first\n' >&2
    exit 1
fi
readonly hyprland_binary

if [[ ! -f "${CONFIG_TEMPLATE}" ]]; then
    printf 'error: realm config template is missing: %s\n' "${CONFIG_TEMPLATE}" >&2
    exit 1
fi

if [[ -z "${HYPRLAND_REALM_TERMINAL:-}" ]]; then
    if command -v kitty >/dev/null 2>&1; then
        HYPRLAND_REALM_TERMINAL="kitty --class agent-realm --name agent-realm --title 'Agent Realm Terminal'"
    elif command -v alacritty >/dev/null 2>&1; then
        HYPRLAND_REALM_TERMINAL="alacritty --class agent-realm,AgentRealm --title 'Agent Realm Terminal'"
    else
        printf 'error: no supported terminal found; set HYPRLAND_REALM_TERMINAL\n' >&2
        exit 1
    fi
fi
export HYPRLAND_REALM_TERMINAL

if ! command -v setsid >/dev/null 2>&1; then
    printf 'error: setsid is required for realm process cleanup\n' >&2
    exit 1
fi

host_hyprctl="${HYPRLAND_REALM_HOST_HYPRCTL:-}"
if [[ -z "${host_hyprctl}" ]]; then
    host_hyprctl="$(command -v hyprctl || true)"
fi
readonly host_hyprctl

# Hyprland appends a long instance signature and socket filename to this path.
# Keep the basename short enough to stay below sockaddr_un.sun_path's limit.
realm_runtime="$(mktemp -d "${host_runtime}/ar.XXXXXX")"
chmod 700 "${realm_runtime}"
mkdir -p "${realm_runtime}/bin" "${realm_runtime}/cache" "${realm_runtime}/state"
create_environment_guard systemctl
create_environment_guard dbus-update-activation-environment

readonly realm_config="${realm_runtime}/agent-realm.lua"
readonly realm_log="${realm_runtime}/agent-realm.log"
cp -- "${CONFIG_TEMPLATE}" "${realm_config}"

trap cleanup EXIT
trap handle_signal INT TERM HUP

launch_realm 2>>"${realm_log}" &
realm_launcher_pid=$!

printf 'Starting agent realm...\n'
printf 'Launcher PID: %s\n' "${realm_launcher_pid}"
printf 'Runtime directory: %s\n' "${realm_runtime}"
printf 'Log path: %s\n' "${realm_log}"

if ! wait_for_realm; then
    status=1
    if ! kill -0 "${realm_launcher_pid}" 2>/dev/null; then
        set +e
        wait "${realm_launcher_pid}"
        status=$?
        set -e
    fi

    printf 'error: nested Hyprland did not publish a Wayland socket\n' >&2
    tail -n 40 "${realm_log}" >&2 || true
    exit "${status}"
fi

printf 'Agent realm ready.\n'
printf 'PID: %s\n' "${realm_compositor_pid}"
printf 'Wayland socket: %s\n' "${realm_wayland_socket}"
printf 'Wayland socket path: %s\n' "${realm_socket_path}"
printf 'Hyprland lock file: %s\n' "${realm_lock_file}"
printf 'Press Ctrl-C here to terminate the realm.\n'

monitor_host_window=0
if wait_for_host_window; then
    monitor_host_window=1
else
    printf 'Warning: host window monitoring is unavailable; use Ctrl-C for cleanup.\n' >&2
fi

while kill -0 "${realm_launcher_pid}" 2>/dev/null; do
    if ((monitor_host_window)); then
        if host_window_is_present; then
            :
        else
            host_window_status=$?
            if ((host_window_status == 1)); then
                realm_closed_by_host=1
                printf 'Host realm window closed; stopping the realm.\n'
                stop_realm "${realm_compositor_pid}" "${realm_process_group_id}"
                break
            fi
        fi
    fi

    sleep 0.5
done

set +e
wait "${realm_launcher_pid}"
status=$?
set -e

if ((realm_closed_by_host)); then
    status=0
fi

if ((status != 0)); then
    printf 'error: agent realm exited abnormally with status %s\n' "${status}" >&2
    tail -n 80 "${realm_log}" >&2 || true
fi

printf 'Agent realm exited with status %s.\n' "${status}"
exit "${status}"
