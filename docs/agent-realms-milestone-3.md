# Agent Realms: Milestone 3

Milestone 3 adds a host-administration surface for the native realm lifecycle
manager. It does not yet associate a realm with its host window, arbitrate human
and agent input, expose the private control socket, or implement MCP. Those
remain later milestones.

## Commands

The host compositor registers these commands with its existing Hyprland IPC
server:

```text
hyprctl realms
hyprctl realm create <name>
hyprctl realm start <name>
hyprctl realm pause <name>
hyprctl realm resume <name>
hyprctl realm stop <name>
hyprctl realm destroy <name>
hyprctl realm info <name>
```

Realm names are passed as data and may contain spaces when quoted by the shell.
Names are limited to 128 bytes and cannot contain control characters. Duplicate
names, missing realms, unknown actions, and invalid state transitions return an
`error:` response in text mode or a structured object in JSON mode.

`start` and `stop` are asynchronous. Their successful command responses report
`creating` and `stopping`; `realmstarted`, `realmfailed`, or `realmstopped`
announces the eventual result.

## Output

`hyprctl -j realms` returns an array, and `hyprctl -j realm info <name>` returns
one realm object:

```json
{
  "id": 1,
  "name": "codex",
  "state": "running",
  "pid": 1234,
  "wayland_socket": "wayland-1",
  "runtime_directory": "/run/user/1000/hypr/realm.a1b2c3",
  "config_path": "/run/user/1000/hypr/realm.a1b2c3/realm.lua",
  "log_path": "/run/user/1000/hypr/realm.a1b2c3/realm.log",
  "exit_code": -1
}
```

Mutation responses wrap the current realm object with `ok` and `action` fields.
Errors have the form:

```json
{"ok":false,"error":"realm 'missing' does not exist"}
```

## Events

Every externally meaningful lifecycle change emits one of:

```text
realmcreated
realmstarted
realmpaused
realmresumed
realmstopped
realmfailed
realmdestroyed
```

The event payload is compact JSON containing `id`, `name`, and the resulting
`state`. The manager owns the typed lifecycle signal; a small IPC bridge converts
it to Hyprland's existing event stream. The bridge is detached before realm
shutdown so compositor cleanup does not publish partially torn-down state.

## Safety and tests

The command handlers delegate all state validation and process ownership to
`CRealmManager`. They do not invoke a shell, install binaries, modify the user's
configuration, reload Hyprland, or address another running Hyprland instance.

Unit coverage exercises text and JSON listing, creation and lookup, escaping,
useful errors, invalid transitions, the complete lifecycle through the command
handlers, stable event names, event payloads, and ordered lifecycle delivery.
The process-backed tests use the fake compositor from Milestone 2 and isolated
temporary directories below `/tmp`.

Verification on 2026-07-19:

- the `Hyprland`, `hyprctl`, and `hyprland_gtests` targets linked successfully;
- all 22 focused Realm and IPC tests passed;
- all 291 discovered CTest tests passed;
- the Realm and Realm-test C++ formatting check passed;
- the locally built `hyprctl --help` lists `realm` and `realms`.

No verification command addressed, reloaded, replaced, or installed into the
running host compositor.

## Next boundary

Milestone 4 will associate each nested compositor's host window with its realm,
add realm metadata to window inspection, define realm-window close behavior, and
make realm windows visually distinguishable. MCP remains intentionally deferred
until the private control socket in Milestone 6.
