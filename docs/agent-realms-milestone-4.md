# Agent Realms: Milestone 4

Milestone 4 associates a nested compositor's host window with its native realm,
exposes the association through normal window inspection, and marks the window
with a non-interactive realm bar. Human/agent input arbitration, the private
control socket, and MCP remain later milestones.

## Association

The lifecycle manager already launches each nested compositor with
`HYPRLAND_REALM_ID` and `HYPRLAND_REALM_NAME` in its isolated environment and
records the exact compositor PID. Wayland supplies that client PID as trusted
host-window metadata. `CRealmWindowManager` therefore follows this chain:

```text
realm ID -> recorded nested compositor PID -> Wayland host-window PID
```

The association is keyed by the host window's stable ID after that match. It
does not inspect the mutable title or app ID, so normal title changes cannot
detach or redirect a realm. A window and a realm may each participate in only
one active association; conflicting or non-realm PIDs are rejected.

Unmapping the host window removes the association and its decoration. Realm
destruction also detaches any remaining association.

## Close policy

An explicit close request sent through Hyprland's normal window-close path
stops an associated realm when it is creating, running, or paused. This uses the
realm manager's existing process-group supervision and transitions the realm to
`STOPPING`, then `STOPPED` after a clean exit.

A plain unmap only detaches the window. It does not relabel an unexpected
compositor exit as a requested stop, so crashes continue to transition to
`FAILED` with their exit status preserved.

## Inspection

`hyprctl -j clients` and `hyprctl -j activewindow` now include a `realm` field.
Ordinary windows report `null`; an associated host window reports:

```json
"realm": {
  "id": 1,
  "name": "codex",
  "state": "running"
}
```

Text output reports `realm: none` or the realm name, ID, and current state.

## Decoration

Every associated realm window receives a 24-logical-pixel bar above its client
content. The bar displays `Realm: <name> · <state>` and uses a distinct color
for creating, running, paused, stopping, stopped, and failed states. Textures
are cached until the label or monitor scale changes.

The bar reserves its own layout extent and does not set the decoration mouse
input flag. It therefore neither overlays application content nor participates
in pointer handling. Agent-control status is intentionally deferred until the
human-control lease milestone.

## Safety and tests

The implementation does not install a compositor, update user configuration,
reload Hyprland, or send commands to another running Hyprland instance. Tests
use the existing fake compositor and private temporary directories below
`/tmp`.

Coverage includes PID and stable-window-ID association, non-realm and duplicate
rejection, bidirectional lookup, detachment, clean close-to-stop behavior,
ordinary-window output, escaped realm JSON, text output, and the visual label.

Verification on 2026-07-19:

- the `Hyprland` and `hyprland_gtests` targets linked successfully;
- all 4 new Milestone 4 tests passed;
- all 26 Realm-focused tests passed;
- all 295 discovered CTest tests passed;
- changed and new C++ files passed the project's clang-format check.

No verification command addressed, reloaded, replaced, or installed into the
running host compositor.

## Next boundary

Milestone 5 will add a human-control lease and visible agent-control status.
MCP remains deferred until the private control socket in Milestone 6.
