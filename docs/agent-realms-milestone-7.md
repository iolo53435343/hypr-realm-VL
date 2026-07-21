# Agent Realms: Milestone 7

Milestone 7 adds a supervised input controller for every running realm. It
routes automated pointer and keyboard actions to the nested Wayland display
without injecting input into the host desktop. Observation, capability
manifests, and MCP integration remain later milestones.

## Per-realm controller

`CRealmInputControllerManager` starts one
`hyprland-realm-agent-controller` helper for each realm after its nested
compositor becomes ready. The helper connects only to that realm's private
Wayland socket and creates virtual pointer and keyboard devices there. It does
not connect to the host Wayland display or operate host input devices.

The helper belongs to the realm's supervised process group. Pausing, stopping,
killing, failure, destruction, and host shutdown therefore apply to the nested
compositor, its applications, and its controller as one lifecycle unit.
Controller startup and unexpected exit are tracked by the host instead of
silently accepting input that has nowhere safe to go.

## Input ownership

The private control plane adds bounded pointer movement, button, scroll,
keyboard, and text operations. Every request names one realm and is accepted
only while:

- the realm is running;
- its controller has completed its Wayland setup; and
- input ownership belongs to the agent.

Human takeover sends `RELEASE_ALL` before automation is denied, preventing a
virtual key or button from remaining held. Pause, stop, failure, and controller
disconnect perform the same release and cleanup. Releasing human control makes
the running realm eligible for agent input again.

Coordinates are validated against the realm output. Buttons, scroll steps,
keycodes, text length, message size, event rate, buffered bytes, and protocol
queues are bounded before data reaches the helper. Invalid and excess requests
return structured errors rather than being queued without limit.

## Controller protocol

The compositor and helper use a private binary protocol over an inherited Unix
socket pair. Messages have a versioned header and explicit payload length.
Pointer, keyboard, text, synchronization, readiness, error, and release events
are decoded with strict type and size validation. The socket is not a public
desktop IPC endpoint and is never exposed to applications inside the realm.

This split keeps Wayland virtual-input protocol handling outside the host
compositor while leaving lifecycle and ownership decisions authoritative in
Hyprland. The later MCP adapter talks to the host control server; it does not
talk directly to the helper.

## Safety and tests

Tests use private temporary runtimes and controller process helpers. Coverage
includes controller readiness, pointer and keyboard routing, coordinate and
payload validation, rate limiting, human takeover, `RELEASE_ALL`, lifecycle
cleanup, protocol fragmentation, unknown message rejection, and controller
failure. They do not inject input into the active host desktop or replace the
running compositor.

The implementation was introduced by commit `92672bf` (`feat: add realm agent
input controller`).
