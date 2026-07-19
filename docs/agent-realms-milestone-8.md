# Agent Realms: Milestone 8

Milestone 8 adds observation of a nested realm without exposing the host
desktop. The existing per-realm controller uses the nested compositor's
`wlr-screencopy` protocol and returns pixels in an anonymous shared-memory
file. Image bytes are never embedded in JSON.

## Observation permission

Observation is denied by default and is independent from input ownership. A
human takeover can therefore block agent keyboard and pointer input without
implicitly changing an existing observation grant.

The host administrative IPC adds:

```text
hyprctl realm observe <name>
hyprctl realm unobserve <name>
```

Realm JSON and text inspection expose `observation_permission` as `allowed` or
`denied`. Stopping, killing, failing, or shutting down a realm revokes its
grant. Pausing retains the grant, cancels any pending frame, and rejects new
capture requests while the realm is not running.

The private control socket exposes equivalent `realm.observe` and
`realm.unobserve` methods for the current vertical slice. Per-agent authority
to administer grants remains a Milestone 9 capability boundary.

## Capture requests

The private control socket accepts a full-output request:

```json
{
  "request_id": "frame-1",
  "method": "realm.capture",
  "params": { "realm": "codex" }
}
```

and a logical-output region request:

```json
{
  "request_id": "frame-2",
  "method": "realm.capture_region",
  "params": {
    "realm": "codex",
    "x": 100,
    "y": 80,
    "width": 640,
    "height": 360
  }
}
```

Regions are bounded by the realm's fixed 1280x720 output. A successful request
immediately receives the normal response envelope with an asynchronous
`capture_id`:

```json
{
  "request_id": "frame-1",
  "ok": true,
  "result": {
    "action": "queued",
    "capture_id": 7,
    "realm": {}
  }
}
```

Only one capture may be pending per realm. Requests are separately rate
limited from input events and have a bounded completion timeout.

## Shared-memory frame transport

When the nested compositor completes the copy, the same requesting control
connection receives an asynchronous framed JSON event:

```json
{
  "event": "realm.capture.ready",
  "capture_id": 7,
  "realm_id": 1,
  "frame": {
    "transport": "scm_rights",
    "fd_count": 1,
    "format": 1,
    "format_name": "xrgb8888",
    "width": 1280,
    "height": 720,
    "stride": 5120,
    "byte_size": 3686400,
    "y_inverted": false
  }
}
```

Exactly one shared-memory file descriptor is attached to this event using
`SCM_RIGHTS`. A client must receive the stream with `recvmsg`, retain ancillary
descriptors while decoding the existing four-byte length-prefixed frames, and
match the event by `capture_id`. The file length is exactly `byte_size`; pixel
rows use `stride` bytes. The current supported formats are Wayland shared-memory
ARGB8888 and XRGB8888.

Failures arrive without a descriptor:

```json
{
  "event": "realm.capture.failed",
  "capture_id": 7,
  "realm_id": 1,
  "error": "realm observation permission was revoked"
}
```

Disconnecting a requester drops its result. Revoking permission, stopping the
realm, controller failure, and capture timeout all invalidate the pending
request. Late descriptors from a canceled capture are closed and ignored.

## Isolation and safety

The host opens and verifies the nested Wayland socket before passing an already
connected descriptor to the controller. The controller can see only the
nested realm output selected from that connection; it never connects to the
host Wayland socket. The helper accepts bounded 8-bit shared-memory frames and
the host validates descriptor type, ownership, and exact file length before
forwarding it only to the requesting client.

Tests use a fake nested compositor/controller and temporary private Unix
sockets. They verify permission gating, independence from input takeover,
full and region requests, both descriptor handoffs, exact frame bytes,
validation, revocation, late-frame handling, and lifecycle cleanup. Real
visual screencopy acceptance remains an isolated manual test and is not run
against the active desktop.

The `Hyprland`, `hyprland-realm-agent-controller`, and `hyprland_gtests`
targets build successfully, and the complete CTest suite passes. No install,
live compositor restart, or host-desktop capture is part of this milestone's
automated verification.
