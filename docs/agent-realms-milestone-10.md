# Agent Realms: Milestone 10

Milestone 10 adds a separate MCP stdio adapter named
`hyprland-realm-mcp-server`. It gives an MCP client typed realm tools without
embedding an MCP implementation in the compositor or exposing the host
Wayland socket.

## One adapter process, one realm

Every adapter process requires a realm at startup:

```text
hyprland-realm-mcp-server --realm codex
```

The realm name is not present in any tool input schema. The adapter inserts
the configured name into every private control request, so a model cannot use
tool arguments to inspect or control another realm. Run a separately named MCP
server process for each realm that should be available to an agent.

The adapter discovers the current compositor socket from
`XDG_RUNTIME_DIR` and `HYPRLAND_INSTANCE_SIGNATURE`. An explicit absolute
socket can be supplied for development and isolated testing:

```text
hyprland-realm-mcp-server --realm codex \
  --socket /absolute/path/to/.realm.sock
```

Before connecting, it requires the path to be a Unix socket owned by the
current user with no group or other permissions. It then verifies the
connected peer UID. Requests and responses have size and time bounds, capture
descriptors are checked again for type, owner, and exact size, and no host
Wayland connection is opened.

## MCP tools

The adapter implements newline-delimited JSON-RPC over stdin/stdout and
negotiates the MCP protocol during `initialize`. Its tool surface is:

| Tool | Operation |
| --- | --- |
| `realm_info` | inspect state, ownership, permission, and capabilities |
| `realm_create` | create only the bound realm |
| `realm_start` | start the bound realm |
| `realm_pause` | pause the bound realm |
| `realm_resume` | resume the bound realm |
| `realm_stop` | stop, but do not destroy, the bound realm |
| `enable_observation` | activate runtime observation after a host grant |
| `disable_observation` | revoke runtime observation and pending captures |
| `capture_realm` | return a full or bounded region as MCP `image/png` content |
| `move_pointer` | move the realm's virtual pointer |
| `click` | atomically press and release a realm pointer button |
| `scroll` | send a bounded realm pointer scroll |
| `press_key` | atomically press and release one Linux evdev keycode |
| `type_text` | type up to 4096 bytes of UTF-8 text |

Click and key press are single host control commands and single controller
messages. The realm helper emits both Wayland states before flushing, avoiding
a stuck virtual key or button if an adapter connection fails between two
requests. The lower-level held-state methods remain available to the private
control protocol for deliberate key/button holds and are not exposed as MCP
tools. The private controller protocol version is incremented so a mismatched
old helper fails during startup instead of silently ignoring these commands.

All compositor-side Milestone 9 checks remain authoritative. The MCP adapter
cannot grant or revoke capabilities. A host operator must grant the relevant
capability explicitly, for example:

```text
hyprctl realm grant codex observe
hyprctl realm grant codex pointer
hyprctl realm grant codex keyboard
```

The model can then call `enable_observation`; it cannot turn a denied
capability into an allowed one. Human takeover, realm state, rate limits, and
capture permission continue to gate otherwise valid tool calls.

## Codex configuration

After installing matching compositor, controller, and MCP binaries, a Codex
stdio server can be configured like this:

```toml
[mcp_servers.hyprland-codex-realm]
command = "/absolute/path/to/hyprland-realm-mcp-server"
args = ["--realm", "codex"]
env_vars = ["XDG_RUNTIME_DIR", "HYPRLAND_INSTANCE_SIGNATURE"]
startup_timeout_sec = 10
tool_timeout_sec = 30
```

The two inherited environment variables intentionally resolve the active
Hyprland instance when Codex starts. They should not be replaced with a stale
socket path copied from an earlier login session.

For multiple agents, configure multiple MCP server entries with distinct
names and distinct `--realm` values. Each agent receives only the server entry
for its realm. The user continues working on the host desktop; the tools act
through the nested realm controller rather than host input devices.

Agent realms are currently Wayland-only: their generated config disables
XWayland to avoid a reproducible nested-compositor teardown corruption. This
does not alter the host session, but X11-only applications cannot be launched
inside a realm.

## Security boundary

Realm binding prevents accidental cross-realm tool calls, and capabilities
prevent the MCP adapter from self-authorizing observation or input. This is
not hostile same-user process isolation: an unsandboxed process running as the
desktop user can still invoke other same-user programs or sockets directly.
Filesystem, network, process, clipboard, and secret isolation remain future
work before mutually untrusted agents should be treated as tenants.

## Safety and tests

The end-to-end MCP test starts the adapter against a fake `0600` Unix socket
inside a temporary directory. It verifies initialization, tool discovery,
realm-name injection, rejection of a cross-realm argument, lifecycle/input
translation, capability error propagation, descriptor-backed capture, PNG
encoding, and atomic click/key commands. Protocol and control-server tests
also cover the new atomic messages.

Automated verification does not install binaries, edit Codex configuration,
connect to the live realm control socket, launch a realm, capture the host
desktop, or restart the active compositor.

## Next boundary

The next layer should package a safe operator workflow around realm creation,
capability review, and per-agent MCP registration. Strong process/resource
isolation and enforceable filesystem/network manifests remain separate future
milestones.
