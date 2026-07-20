# Realm computer-use contract

The realm MCP follows the same observe-act-observe loop as mature desktop
automation tools without granting access to the host desktop. The adapter is
permanently bound to one realm, and compositor capabilities, input ownership,
and runtime observation permission remain authoritative.

## Recommended loop

1. Call `capture_realm` with no dimensions. The returned native width and
   height are the pointer coordinate space.
2. Prefer `point_and_click` for an atomic motion and click. Use `move_pointer`
   separately only for hover or scrolling.
3. Prefer named `press_key` and `press_shortcut` operations over raw Linux
   keycodes.
4. After a visible action, call `capture_realm` with
   `wait_for_change=true`. Use the bounded `wait` tool when an application has
   a known delay but no reliable visual transition.
5. Re-observe before selecting another coordinate.

`capture_realm` waits for a compositor frame and works whether the realm is on
the operator's active workspace or completely hidden. It never captures the
host monitor. Optional region fields crop the returned native frame and do not
change the pointer coordinate space. Recognized realm compositor windows use
Hyprland's existing throttled background-frame scheduler while offscreen; this
does not change the user's window rules or opt ordinary applications into
background rendering.

## Completion and diagnostics

Input success means the nested compositor processed the complete ordered
action. Structured results include the input sequence and acknowledgement
latency in milliseconds. This does not claim that an application completed
network activity or accepted a semantic action; agents verify that with a new
capture.

Capture metadata reports native source dimensions, returned image dimensions,
the active pointer coordinate dimensions, whether the call waited for a visual
change, and total elapsed time. Invalid screencopy metadata reports the offered
width, height, stride, format, and configured safety limit.

Full frames are bounded to 64 MiB of shared memory and a maximum dimension of
16384 pixels. Visual-change polling compares visible pixels in the full native
frame against the previous capture, is bounded to 30 seconds, and runs no more
than four polls per second, matching the compositor's capture rate limit.
