# Agent Realms: Milestone 12

Milestone 12 turns the embedded realm window into an operator-facing control
surface and makes agent computer-use feedback synchronous and visually
traceable.

## Realm window controls

Every managed realm window has three controls in its status bar:

- `Take Over` transfers pointer and keyboard ownership to the human. While the
  human owns input, the button reads `Release` and returns ownership to the
  agent when clicked.
- `Pause` suspends the realm process group. It reads `Resume` while the realm
  is paused.
- `Stop` requests a graceful realm stop.

These controls are handled by the host compositor. An agent-owned realm cannot
receive an accidental host click through the status bar or its content. The
status text and border update with lifecycle and ownership changes.

Capabilities remain an explicit host-administration decision. The status bar
does not grant observation, pointer, or keyboard capabilities.

Managed realm host windows are excluded from the generic application-not-
responding dialog while intentionally paused. Suspending a supervised realm
also suspends its nested compositor, so presenting `Terminate` and `Wait` for
that expected state would be misleading. ANR monitoring resumes with the
realm, and realm lifecycle controls remain available in the host status bar.

## Visible cursors

Generated realm configurations use software cursors and keep them visible. A
cursor is therefore rendered in the embedded realm window even when the
nested backend cannot find a complete host Xcursor theme.

The host compositor suppresses its own cursor only while it is over realm
content, preventing a second stale pointer from appearing. The host cursor
remains visible over the realm control bar and is restored as soon as it
leaves the content.

Named realm cursors preserve their normal pointer, text, and resize shapes but
use a vivid magenta palette. An orange-to-magenta ring surrounds the exact
hotspot. This makes the realm pointer visibly different from the operator's
desktop cursor without requiring a separately installed cursor theme. Cursor
surfaces supplied directly by an application retain their pixels but still
receive the realm hotspot ring. A realm also preserves its last visible shape
when an application requests a hidden cursor, because an invisible pointer
would make both human takeover and observed agent input ambiguous.

Realm screencopy requests include the cursor overlay. The human can see where
the agent is pointing in the embedded window, and the agent receives the same
cursor in MCP captures.

## Applied input acknowledgements

Pointer and keyboard tools no longer return success as soon as a message is
placed on the controller socket. Each input operation is followed by an
ordered Wayland display synchronization callback. The controller reports
`realm.input.applied` only after the nested compositor has processed all input
requests before that callback.

The realm control server and MCP adapter wait for that event. Successful tools
now report `Pointer moved`, `Pointer clicked`, `Key pressed`, or `Text entered`.
They return a failure if input ownership or a capability is revoked, the realm
pauses or stops, the controller disconnects, or the acknowledgement times out.

This acknowledgement proves ordered delivery to the realm compositor. It does
not claim that an application accepted a click, completed a navigation, or
finished rendering. Agents should capture again after visible actions.

## Stable capture coordinates

The embedded realm window is resizable, while agent input intentionally uses a
stable 1280x720 logical coordinate space. The MCP adapter captures the current
nested output and normalizes it to 1280x720 before returning the PNG. Region
captures crop that normalized image. Screenshot pixels and `move_pointer`
coordinates therefore remain aligned when the operator tiles or resizes the
realm window.

Ordinary screencopy requests explicitly schedule a compositor frame. This is
important for idle nested outputs, where adding damage that is already pending
may otherwise fail to produce another frame. Capture timeout recovery still
cancels one stalled frame and retries it once.

Capture rate limiting remains intentional. Requests above the configured
burst or sustained rate return `rate_limited` immediately; they are not queued.

## Safety and verification

Unit tests cover input protocol acknowledgements, control-socket completion
events, MCP normalized full and region PNG dimensions, and the status-bar
control layout. A disposable nested-compositor test verifies visible software
cursors, cursor-inclusive MCP captures, back-to-back idle captures, and the
Take Over/Release interaction.

The test flow does not install the fork, reload the operator's configuration,
or restart the active host compositor.
