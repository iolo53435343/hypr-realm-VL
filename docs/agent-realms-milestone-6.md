# Agent Realms: Milestone 6

Milestone 6 adds a private, authenticated control plane for realm lifecycle
operations. It is the boundary that a later MCP server can use; MCP itself and
automated input are not part of this milestone.

## Socket location and access

Each host compositor creates one control socket in its existing private
instance runtime directory:

```text
<Hyprland instance directory>/.realm.sock
```

The basename is no longer than Hyprland's core `.socket.sock` basename, so the
control socket fits the Unix-domain path limit whenever core IPC does.

The instance directory must be owned by the compositor user and inaccessible
to group and other users. The socket is set to mode `0600`. Every accepted
connection is independently checked with `SO_PEERCRED`, and its effective user
ID must match the user running Hyprland. If the platform cannot provide
`SO_PEERCRED`, the server rejects the connection.

The server never removes a pre-existing path. During shutdown it unlinks the
socket only if the path still names the exact socket device and inode that this
server created.

## Framing and messages

The protocol is a stream of frames. Each frame contains a four-byte unsigned
big-endian payload length followed by exactly that many bytes of JSON. The
default maximum JSON payload is 64 KiB.

A request has this shape:

```json
{
  "request_id": "123",
  "method": "realm.pause",
  "params": { "realm": "codex" }
}
```

`request_id` is required, is returned unchanged in the response, and is
limited to 128 non-control characters. Unknown JSON fields are rejected. The
parser also rejects trailing non-whitespace data, incorrect field types,
missing required fields, and malformed JSON.

The initial methods are:

| Method | Parameters | Result |
| --- | --- | --- |
| `realm.list` | none | all realms |
| `realm.info` | `realm` | one realm |
| `realm.create` | `realm` | created realm |
| `realm.start` | `realm` | starting realm |
| `realm.pause` | `realm` | paused realm |
| `realm.resume` | `realm` | resumed realm |
| `realm.stop` | `realm` | stopping realm |
| `realm.destroy` | `realm` | destroyed realm |
| `realm.takeover` | `realm` | human-owned realm |
| `realm.release` | `realm` | agent-owned realm |

Successful responses use a stable envelope:

```json
{
  "request_id": "123",
  "ok": true,
  "result": {
    "action": "paused",
    "realm": {
      "id": 1,
      "name": "codex",
      "state": "paused",
      "input_owner": "none"
    }
  }
}
```

Errors are structured and machine-readable:

```json
{
  "request_id": "123",
  "ok": false,
  "error": {
    "code": "operation_failed",
    "message": "realm 'codex' cannot be paused while stopped"
  }
}
```

An error that occurs before a valid request ID can be recovered uses
`"request_id": null`. Error codes distinguish parse, request, parameter,
method, lookup, authorization, size, overload, response-size, and realm
operation failures.

## Event-loop and resource limits

The listening socket and every client descriptor are nonblocking. Reads,
writes, accepts, requests handled per callback, connected clients, buffered
input, and queued output are all bounded. A client that exceeds a limit gets a
structured terminal error where the socket can still deliver one, then the
connection is closed. The server never polls, sleeps, or waits for a client on
the compositor event loop.

Realm lifecycle commands remain ordinary host event-loop operations. This
socket is for low-frequency control messages; high-frequency pointer or
keyboard input must not be routed through it or through `hyprctl`.

Failure to create the optional control socket is logged but does not abort the
host compositor. This preserves the normal desktop if its control-plane setup
is unavailable or unsafe.

## Safety and tests

The implementation does not install a compositor, update user configuration,
reload Hyprland, or connect to another running Hyprland instance. Socket tests
use private temporary directories below `/tmp`, a fake nested-compositor
helper, and the server's manual nonblocking dispatch mode.

Coverage includes all ten methods, strict malformed and unknown-field
rejection, lifecycle errors, fragmented frames, pipelined frames, private
permissions, safe cleanup, insecure-directory rejection, payload limits, and
real peer-credential rejection.

Verification on 2026-07-19:

- the `Hyprland` and `hyprland_gtests` targets linked successfully;
- all 9 Milestone 6 control-server tests passed;
- all 42 Realm-focused tests passed;
- all 312 discovered CTest tests passed.

No verification command addressed, reloaded, replaced, or installed into the
running host compositor.

## Next boundary

Milestone 7 can place an agent controller and MCP adapter above this socket.
That layer should translate authenticated tool calls into low-frequency realm
operations while keeping bulk input and observation data on purpose-built
channels.
