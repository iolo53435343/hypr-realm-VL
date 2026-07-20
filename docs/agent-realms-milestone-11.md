# Agent Realms: Milestone 11

Milestone 11 adds the host-operator application launcher. Once a realm is
running, an application can be opened directly on its nested Wayland display:

```text
hyprctl realm open codex brave
```

The syntax is:

```text
hyprctl realm open <realm-name> <application>
```

`<application>` is a single executable name resolved through `PATH`. Paths,
whitespace, arguments, and shell syntax are rejected. The compositor calls the
executable directly and does not invoke a shell.

## Realm application environment

An opened application:

- connects to the realm's `WAYLAND_DISPLAY` and Hyprland instance;
- joins the realm compositor's process group, so pause, resume, stop, kill, and
  host shutdown apply to it;
- inherits the operator's normal `HOME`, `PATH`, XDG configuration, data,
  cache and state directories, browser profile, and toolkit preferences; and
- receives the realm's runtime directory, Wayland display, Hyprland instance,
  and realm identity, while inherited host X11 and Wayland descriptor
  variables are removed.

The launcher does not create application-specific profiles or rewrite general
application settings. Chromium-family browsers enforce one running process per
profile themselves. If Brave already owns the normal profile on the host,
another `brave` invocation may ask that existing host process to open a window
instead of creating a second process in the realm. The operator can close the
host instance first or deliberately choose a separate profile outside this
generic launcher.

Standard output and standard error are appended to the realm log. Standard
input is disconnected from the host compositor.

## Operator boundary

Application launch is deliberately available only through the host
administrative `hyprctl` command. There is no `realm.open` method on the
private realm control socket and no MCP `open_application` tool. An agent can
operate an application after the human opens it and grants the required
observation or input capabilities, but the realm MCP adapter cannot choose or
start a program.

This is an interface boundary, not hostile same-user isolation. A separate
unsandboxed process running as the desktop user may still execute `hyprctl`
itself. Filesystem, network, process-namespace, and resource isolation remain
future work.

## Example operator flow

With this fork installed as the active host compositor:

```text
hyprctl realm create codex
hyprctl realm start codex
hyprctl realm open codex brave

hyprctl realm grant codex observe
hyprctl realm grant codex pointer
hyprctl realm grant codex keyboard
```

The MCP adapter configured with `--realm codex` is started automatically by
its MCP client. It can inspect and control the browser only after the operator
grants those capabilities; it cannot open the browser itself.

Agent realms remain Wayland-only, so the selected application must have native
Wayland support.

## Safety and tests

The IPC tests open a dedicated helper executable in a fake realm. They verify
the process group, display targeting, preservation of the normal application
environment, removal of host display variables, direct-exec validation,
failed-exec reporting, and cleanup when the realm stops. The control-server and
MCP tests explicitly verify that application launch is absent from both
agent-facing interfaces.

Automated verification does not install this fork, replace or restart the
active compositor, open Brave, or connect to the live host session.
