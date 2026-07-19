# Agent Realms: Milestone 9

Milestone 9 adds a deny-by-default capability manifest to every realm and
enforces the capabilities that the current vertical slice can actually
control. It does not claim operating-system isolation that is not yet present.

## Capability manifest

Realm inspection exposes:

```json
{
  "capabilities": {
    "observe": false,
    "pointer": false,
    "keyboard": false,
    "clipboard": false,
    "network": [],
    "filesystem_read": [],
    "filesystem_write": [],
    "secrets": []
  }
}
```

New realms deny every capability. The manifest belongs to the realm and
survives start, pause, resume, stop, and restart until an administrator changes
it or destroys the realm.

The current enforceable capabilities are:

| Capability | Protected operations | Additional runtime condition |
| --- | --- | --- |
| `observe` | `realm.observe`, `realm.capture`, `realm.capture_region` | realm running and observation permission allowed |
| `pointer` | pointer move, button, and scroll | realm running and input owner `agent` |
| `keyboard` | key and text input | realm running and input owner `agent` |

`clipboard`, network allowlists, filesystem grants, and secret grants are
present as locked-down manifest fields so clients can depend on a stable shape.
They cannot be granted yet. Accepting those fields without a corresponding
isolation mechanism would create a false security boundary.

## Administrative grants

Only the host administrative IPC can change the enforceable manifest:

```text
hyprctl realm grant <name> observe
hyprctl realm grant <name> pointer
hyprctl realm grant <name> keyboard

hyprctl realm revoke <name> observe
hyprctl realm revoke <name> pointer
hyprctl realm revoke <name> keyboard
```

The private agent control socket deliberately has no grant or revoke method.
An agent request such as `realm.grant` is rejected as an unknown method. The
same socket can call `realm.observe` only after the host has granted the
realm's `observe` capability; this activates the separate runtime observation
permission and is not a capability escalation.

Capability changes emit `realmcapabilitygranted` and
`realmcapabilityrevoked` host IPC events. Realm list, info, lifecycle, window
inspection, and the visible realm bar include the current capability state.

## Revocation behavior

Revocation takes effect synchronously on the host event loop:

- revoking `pointer` or `keyboard` sends `RELEASE_ALL` to the supervised realm
  controller before further input is accepted;
- revoking `observe` also denies the runtime observation permission and
  cancels any pending capture;
- a late frame from a canceled capture is closed and ignored;
- input ownership remains a separate exclusive lease, so a capability never
  bypasses pause or human takeover;
- observation permission remains separate from input ownership, so human
  takeover does not silently change a valid observation policy.

Controller-side checks enforce the manifest even when an internal caller
bypasses JSON request parsing. The control server returns the structured
`capability_denied` code when policy blocks an otherwise valid automation
request.

## Security boundary

This milestone is an application policy boundary inside Hyprland. The private
socket is still restricted to the compositor user with `0600` permissions and
`SO_PEERCRED`; processes running as that same user are not isolated from one
another by this manifest alone. In particular, an unsandboxed same-user agent
could invoke administrative programs directly.

Network enforcement, filesystem mounting, process namespaces, cgroups,
systemd scopes, bubblewrap, portal-mediated file access, clipboard mediation,
and secret delivery remain future isolation work. Until that work exists,
their manifest entries remain denied and immutable.

## Safety and tests

Tests use fake nested compositors/controllers, anonymous frame files, and
private temporary Unix sockets. Coverage includes safe defaults, manifest
inspection, stable capability names, administrative parsing, rejection of
unenforced capabilities, private-socket non-escalation, input and capture
gating, immediate virtual-input release, observation cancellation, late-frame
handling, and capability visibility in realm windows.

Verification builds and tests only the repository checkout. It does not
install Hyprland, alter user configuration, connect to the live host control
socket, restart the active compositor, or capture the host desktop.

## Next boundary

With lifecycle, private control, input, observation, and enforceable
capabilities in place, the next layer can be a separate MCP adapter. It should
translate a small typed tool surface into the private framed protocol, retain
SCM_RIGHTS frame descriptors correctly, and expose only the realm chosen by
its operator. Strong per-process resource isolation remains a separate step
before treating mutually untrusted same-user agents as hostile tenants.
