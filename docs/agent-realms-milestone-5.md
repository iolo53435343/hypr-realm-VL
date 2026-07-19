# Agent Realms: Milestone 5

Milestone 5 adds an exclusive human-control lease for a realm's nested
compositor window. It also adds host keybind dispatchers for lease control,
emergency pause, and force-kill. Automated input injection, the private control
socket, and MCP remain later milestones.

## Input ownership

Every realm exposes one input owner:

| Owner | Meaning | Physical input to host window |
| --- | --- | --- |
| `agent` | The realm is available to automation | blocked |
| `human` | A human takeover lease is active | enabled |
| `none` | The realm is inactive or paused | blocked |

A newly started or resumed realm enters `agent`. Pausing, stopping, process
failure, and shutdown move it to `none`. Resuming a paused realm deliberately
returns it to `agent`; a previous human lease is not silently restored.

Ownership changes and physical-input gating happen synchronously on the host
event loop. Takeover changes the owner to `human` before unblocking the host
window. Release blocks the host window before returning ownership to `agent`.
The first implementation therefore never has agent and human input enabled at
the same time.

## Administrative commands

The host IPC adds:

```text
hyprctl realm takeover <name>
hyprctl realm release <name>
hyprctl realm kill <name>
```

`takeover` requires a running realm with an associated host window and focuses
that window after acquiring the lease. `release` returns a running human-owned
realm to agent control. `kill` sends `SIGKILL` to the supervised realm process
group and retains the normal asynchronous cleanup and final `STOPPED` state.

Realm JSON, lifecycle event data, and window inspection now include
`"input_owner":"agent|human|none"`. Text inspection includes the same value.
Successful takeovers and releases emit `realmtakeover` and `realmrelease`
events.

## Host dispatchers and emergency pause

The following dispatchers are registered before configuration is parsed:

```text
realmtakeover <name>
realmrelease <name>
realmpause [name]
realmkill <name>
```

`realmpause <name>` pauses one realm. An argument-free `realmpause` is the
global emergency action: it attempts to pause every running realm, including
human-owned realms, and moves all successfully paused realms to `none`.

For example, a legacy host configuration can assign its chosen emergency chord
with:

```ini
bind = SUPER SHIFT, Escape, realmpause
```

The equivalent native Lua binding is:

```lua
hl.bind("SUPER + SHIFT + Escape", hl.dsp.realm.pause())
```

The Lua namespace also exposes `hl.dsp.realm.takeover(name)`,
`hl.dsp.realm.release(name)`, `hl.dsp.realm.pause(name)`, and
`hl.dsp.realm.kill(name)`.

No default chord is installed because silently claiming a key combination
would change existing host behavior. Under a Lua configuration, the dispatcher
can also be tested without editing configuration using
`hyprctl dispatch 'hl.dsp.realm.pause()'`.

Realm host windows cannot suppress host shortcuts through the Wayland keyboard
shortcuts inhibition protocol. Host bindings, including the emergency action,
therefore remain available while a human lease is active.

## Visible ownership

The non-interactive realm bar now displays:

```text
Realm: <name> · <state> · input: <owner>
```

The bar updates on lifecycle and ownership events. Agent- and no-owner windows
remain visible but do not accept normal pointer or keyboard focus; takeover
unblocks and focuses the window.

## Safety and tests

The implementation does not install a compositor, update user configuration,
reload Hyprland, or send commands to another running Hyprland instance. Tests
use the fake compositor and private temporary directories below `/tmp`.

Coverage includes stable owner names and events, automatic ownership changes,
exclusive takeover/release transitions, host-window requirements, global
pause-all behavior, supervised force-kill cleanup, dispatcher errors, IPC
commands, JSON/text inspection, and the visible ownership label.

Verification on 2026-07-19:

- the `Hyprland` and `hyprland_gtests` targets linked successfully;
- all 33 Realm-focused tests passed;
- the native Lua Realm-dispatcher test passed;
- all 303 discovered CTest tests passed;
- changed and new C++ files passed the project's clang-format check.

No verification command addressed, reloaded, replaced, or installed into the
running host compositor.

## Next boundary

Milestone 6 will add a private per-realm control socket and authentication
boundary. MCP and agent work execution remain deferred until that control plane
exists.
