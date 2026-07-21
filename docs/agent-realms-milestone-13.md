# Agent Realms: Milestone 13

Milestone 13 turns the MCP adapter into a capability-demo orchestrator. A user
can ask an agent to create a temporary realm on a numeric Hyprland workspace,
open an application there, and operate several named realms without manually
running `hyprctl realm create`, `grant`, `start`, `observe`, and `open` for
each one.

This milestone deliberately optimizes for showing the compositor architecture.
It does not claim hostile same-user process, filesystem, or network isolation.

## Default MCP registration

The production default no longer requires a pre-created realm:

```toml
[mcp_servers.hyprland-realms]
command = "/run/current-system/sw/bin/hyprland-realm-mcp-server"
env_vars = ["XDG_RUNTIME_DIR", "HYPRLAND_INSTANCE_SIGNATURE"]
startup_timeout_sec = 10
tool_timeout_sec = 30
```

The adapter discovers the active compositor's private `0600` control socket
and verifies its owner and peer UID exactly as before. No copied runtime path
or per-realm MCP entry is needed.

For example, an MCP client can call:

```json
{
  "name": "launch_realm",
  "arguments": {
    "name": "research",
    "application": "brave",
    "workspace": 5
  }
}
```

The compositor creates `research`, remembers workspace 5 before the nested
window maps, grants `observe`, `pointer`, and `keyboard`, starts the nested
compositor, enables observation, and directly executes `brave`. Application
names are validated executable names and are passed to `execvp` as a single
argument; no shell, path, URL, or command fragment is accepted.

`launch_realms` accepts up to eight specifications so the agent can establish
several independent work surfaces in one request. Subsequent capture and input
tools require a realm name, so coordinate state and visual-change history stay
separate for each realm. Multiple MCP adapter processes can also operate
different realms through the same compositor socket.

## Orchestrator tools

| Tool | Operation |
| --- | --- |
| `list_realms` | inspect all current realm records |
| `launch_realm` | create, authorize, place, start, observe, and open one app |
| `launch_realms` | establish one to eight independent temporary realms |
| `open_application` | directly execute another app in a running realm |
| `finish_realm` | stop, wait for cleanup, destroy, and remove runtime files |
| `realm_info` | inspect one named realm |
| `realm_pause` / `realm_resume` | suspend or resume one realm process group |
| computer-use tools | capture and send acknowledged input to a named realm |

Temporary means the realm is intended to be removed with `finish_realm` when
the task ends. The adapter does not silently kill realms when its stdio
connection closes: an MCP client restart must not destroy visible work. The
realm window's `Stop`, `Pause`, and `Take Over` controls remain available to
the human at all times.

## Workspace placement

Workspace placement is compositor-native. The private control request stores a
positive numeric workspace ID even if the realm host window has not opened.
When the window maps, Hyprland creates or finds that workspace and moves the
realm window through its normal global window controller. The MCP adapter does
not spawn `hyprctl`, change the active workspace, synthesize host input, or
invoke a shell.

## Faster observation

The former sustained capture rate of four frames per second is now twelve,
with a burst of four. `capture_realm(wait_for_change=true)` permits a 100 ms
poll interval and uses 100 ms by default. Pointer and keyboard input retain
their existing 256-cost-unit-per-second token bucket and synchronous
`realm.input.applied` acknowledgements. Higher throughput therefore does not
restore the old ambiguous “queued” completion behavior.

## Compatibility and boundary

Starting the adapter with `--realm NAME` preserves the Milestone 10 bound tool
schemas and lifecycle workflow. Bound tools reject a supplied realm argument.
This is useful for older Codex configurations and for deliberately constrained
setups.

The default orchestrator is intentionally permissive: any same-user MCP client
that can reach the private socket can ask the compositor to grant the three
implemented computer-use capabilities and launch validated applications.
Unix-socket ownership, host-input separation, direct execution, human takeover,
and capture/input limits still apply. Filesystem, network, secrets, clipboard,
resource accounting, and mutually untrusted tenant isolation remain future
production work.

## Verification

The fake-socket MCP test covers both default orchestrator and legacy bound
modes, including multi-realm launch, workspace placement, automatic grants,
direct application launch, per-realm capture/input routing, and final cleanup.
Compositor unit tests cover the new private control methods and pending
workspace requests. These tests use disposable processes and temporary runtime
directories; they do not restart or reconfigure the active desktop session.
