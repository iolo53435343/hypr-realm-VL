# Hypr Realm — a Hyprland fork that supports agentic realms

> **Experimental fork of Hyprland for compositor-native computer use.** An AI
> agent can create a visible, temporary nested desktop on a chosen workspace,
> launch applications, see its pixels, and control its own pointer and keyboard
> while the human keeps control of the host desktop.

## Built with ChatGPT and Codex

ChatGPT was used during the initial planning phase to turn the Agent Realms
idea into a first milestone plan. That plan was then pasted into Codex. Codex
inspected the Hyprland codebase, expanded the work into the 13 implementation
milestones documented below, and helped implement the compositor, controller,
MCP adapter, tests, documentation, and follow-up fixes.

The development loop was deliberately transparent: each capability was built
as a small commit, tested before moving forward, and then exercised on the live
fork under human supervision. The commit history shows the path from the first
nested-compositor proof to reliable browser computer use; follow-up commits
record the bugs found through real use rather than hiding them in a single
generated patch.

## Hypr Realm in four seconds

**An agent gets a visible Hyprland work surface—not your host desktop—where it
can open apps, see, click, type, and work. You can watch it, take over, pause it,
or stop it at any time.**

This is not a VM. A realm is a compositor-managed nested Hyprland instance with
its own Wayland and XWayland displays, application process group, virtual input devices,
cursor, capture stream, lifecycle, and host window. The current prototype
isolates the agent's display and input target; stronger filesystem, network,
secret, and process isolation is future work.

## The problem

Most computer-use agents either control the person's real desktop, where agent
input can collide with human input, or run in a remote browser/VM that is
detached from the local desktop experience. Desktop environments were designed
around one human input stream and have no first-class place for several visible
software operators.

Hypr Realm moves the computer-use boundary into the compositor. Each task gets
its own visible work surface and coordinate space. The agent can operate there
while the human works elsewhere, and two agents can work in separate realms on
different workspaces without sharing pointer focus or keyboard state.

## How it works

```text
Codex / any MCP client
        │  typed computer-use tools over stdio
        ▼
hyprland-realm-mcp-server
        │  framed JSON + shared-memory frame descriptors
        ▼
Host Hyprland ── private, same-user .realm.sock
├── RealmManager             lifecycle and process supervision
├── RealmWindowManager       workspace placement and visible controls
├── RealmControlServer       bounded commands and capability checks
└── RealmInputController     capture + virtual pointer/keyboard
        │
        ├── Realm A: nested Hyprland → Brave, terminal, editor…
        └── Realm B: nested Hyprland → another independent task…
```

The default MCP adapter is a dynamic orchestrator. An agent can:

- create one realm or up to eight realms in one request;
- place each realm window on a numeric Hyprland workspace;
- launch validated application executables without a shell;
- capture a realm at its native dimensions without capturing the host monitor;
- point and click atomically, scroll, type text, and press named shortcuts;
- observe the distinct realm cursor and wait for visual changes;
- pause, resume, inspect, and clean up a temporary realm.

Input calls are acknowledged only after the nested compositor has processed
them. Computer use follows an observe → act → observe loop, with per-realm
capture and coordinate state so parallel tasks do not become mixed together.
The realm window exposes **Take Over**, **Pause**, and **Stop** controls that are
owned by the host compositor, not the agent.

A prompt can therefore be as direct as:

> Create a temporary realm on workspace 5, open Brave, find the first result
> for “Messi epic skills,” and clean up the realm when finished.

## Why this is where compositors are heading

Computer use should be a desktop primitive, not a pile of global screenshots
and synthetic input aimed at the user's only session. The compositor already
knows which pixels belong to a surface, where input is routed, which workspace
contains it, and when a window is visible. Making agent work surfaces a native
compositor concept enables local, observable, parallel automation with a clear
human control point.

Hypr Realm is a capability demo of that architectural direction: agents become
visible participants in the desktop rather than invisible processes fighting
the user for the same cursor. The longer-term path is to combine this
compositor boundary with enforceable OS-level filesystem, network, secret, and
resource policies.

## Architecture boundaries today

Hypr Realm is experimental and does **not** yet claim hostile same-user tenant
isolation. Applications in a realm currently run as the desktop user and may
inherit the user's normal home directory, network access, and application
profiles. Clipboard, filesystem, network, secrets, cgroups, and namespace
fields are not presented as enforced protections until their corresponding
isolation mechanisms exist. Realm-local XWayland is enabled: readiness requires
a validated nested `DISPLAY`, and directly launched applications receive that
display without inheriting the host's X authentication state. Nested XWayland
teardown remains an explicit runtime acceptance gate because the original
`0.55.0` prototype reproduced allocator corruption on that path.

The maintained downstream baseline and compatibility decisions are documented
in [`docs/downstream-v0.56.2-xwayland.md`](docs/downstream-v0.56.2-xwayland.md).

What is enforced today is the computer-use boundary: realm capture does not
capture the host monitor, virtual input is addressed to the nested Wayland
display, human and agent input ownership are exclusive, process lifecycle is
supervised, control traffic is bounded, and the private control socket verifies
same-user ownership and peer credentials.

## The 13-milestone build

The original ChatGPT plan covered the foundational vertical slice. Codex
expanded it as implementation and live testing exposed the production and UX
work needed to make the idea demonstrable end to end:

| Milestone | Result |
| --- | --- |
| [1](docs/agent-realms-milestone-1.md) | Proved a safely nested Hyprland desktop manually (and recorded the Milestone 0 baseline). |
| [2](docs/agent-realms-milestone-2.md) | Added native realm records, lifecycle states, process supervision, and cleanup. |
| [3](docs/agent-realms-milestone-3.md) | Added `hyprctl` administration, inspection, errors, and lifecycle events. |
| [4](docs/agent-realms-milestone-4.md) | Associated trusted nested-compositor processes with visible host windows. |
| [5](docs/agent-realms-milestone-5.md) | Added exclusive human takeover, release, pause, and emergency termination. |
| [6](docs/agent-realms-milestone-6.md) | Added the private authenticated and resource-bounded realm control socket. |
| [7](docs/agent-realms-milestone-7.md) | Added the per-realm virtual pointer and keyboard controller. |
| [8](docs/agent-realms-milestone-8.md) | Added realm-only screencopy observation through shared-memory frame transport. |
| [9](docs/agent-realms-milestone-9.md) | Added deny-by-default capability manifests and runtime enforcement. |
| [10](docs/agent-realms-milestone-10.md) | Added the standalone, typed MCP computer-use adapter. |
| [11](docs/agent-realms-milestone-11.md) | Added direct, shell-free application launch into a realm. |
| [12](docs/agent-realms-milestone-12.md) | Added operator UI, visible agent cursor, synchronous input feedback, and computer-use polish. |
| [13](docs/agent-realms-milestone-13.md) | Added dynamic temporary-realm orchestration, workspace placement, parallel launch, and cleanup. |

See the [computer-use contract](docs/agent-realms-computer-use.md) for the MCP
observe-act-observe workflow and completion semantics. The
[`agent-realms` commit history](https://github.com/0xmiki/Hyprland/commits/agent-realms/)
is the detailed engineering record.

---

## Upstream Hyprland

<div align = center>

<img src="https://raw.githubusercontent.com/hyprwm/Hyprland/main/assets/header.svg" width="750" height="300" alt="banner">

<br>

[![Badge Workflow]][Workflow]
[![Badge License]][License] 
![Badge Language] 
[![Badge Pull Requests]][Pull Requests] 
[![Badge Issues]][Issues] 
![Badge Hi Mom]<br>

<br>

Hyprland is a 100% independent, dynamic tiling Wayland compositor that doesn't sacrifice on its looks.

It provides the latest Wayland features, is highly customizable, has all the eyecandy, the most powerful plugins,
easy IPC, much more QoL stuff than other compositors and more...
<br>
<br>

---

**[<kbd> <br> Install <br> </kbd>][Install]** 
**[<kbd> <br> Quick Start <br> </kbd>][Quick Start]** 
**[<kbd> <br> Configure <br> </kbd>][Configure]** 
**[<kbd> <br> Contribute <br> </kbd>][Contribute]**

---

<br>

</div>

# Features

- All of the eyecandy: gradient borders, blur, animations, shadows and much more
- A lot of customization
- 100% independent, no wlroots, no libweston, no kwin, no mutter.
- Custom bezier curves for the best animations
- Powerful plugin support
- Built-in plugin manager
- Tearing support for better gaming performance
- Easily expandable and readable codebase
- Fast and active development
- Not afraid to provide bleeding-edge features
- Config reloaded instantly upon saving
- Fully dynamic workspaces
- Two built-in layouts and more available as plugins
- Global keybinds passed to your apps of choice
- Tiling/pseudotiling/floating/fullscreen windows
- Special workspaces (scratchpads)
- Window groups (tabbed mode)
- Powerful window/monitor/layer rules
- Socket-based IPC
- Native IME and Input Panels Support
- and much more...

<br>
<br>

<div align = center>

# Gallery

<br>

![Preview A]

<br>

![Preview B]

<br>

![Preview C]

<br>
<br>

</div>

# Special Thanks

<br>

**[wlroots]** - *For powering Hyprland in the past*

**[tinywl]** - *For showing how 2 do stuff*

**[Sway]** - *For showing how 2 do stuff the overkill way*

**[Vivarium]** - *For showing how 2 do stuff the simple way*

**[dwl]** - *For showing how 2 do stuff the hacky way*

**[Wayfire]** - *For showing how 2 do some graphics stuff*


<!----------------------------------------------------------------------------->

[Configure]: https://wiki.hypr.land/Configuring/
[Stars]: https://starchart.cc/hyprwm/Hyprland
[Hypr]: https://github.com/hyprwm/Hypr

[Pull Requests]: https://github.com/hyprwm/Hyprland/pulls
[Issues]: https://github.com/hyprwm/Hyprland/issues
[Todo]: https://github.com/hyprwm/Hyprland/projects?type=beta

[Contribute]: https://wiki.hypr.land/Contributing-and-Debugging/
[Install]: https://wiki.hypr.land/Getting-Started/Installation/
[Quick Start]: https://wiki.hypr.land/Getting-Started/Master-Tutorial/
[Workflow]: https://github.com/hyprwm/Hyprland/actions/workflows/ci.yaml
[License]: LICENSE


<!----------------------------------{ Thanks }--------------------------------->

[Vivarium]: https://github.com/inclement/vivarium
[WlRoots]: https://gitlab.freedesktop.org/wlroots/wlroots
[Wayfire]: https://github.com/WayfireWM/wayfire
[TinyWl]: https://gitlab.freedesktop.org/wlroots/wlroots/-/blob/master/tinywl/tinywl.c
[Sway]: https://github.com/swaywm/sway
[DWL]: https://codeberg.org/dwl/dwl

<!----------------------------------{ Images }--------------------------------->

[Preview A]: ./assets/prev1.png
[Preview B]: ./assets/prev2.png
[Preview C]: ./assets/prev3.png


<!----------------------------------{ Badges }--------------------------------->

[Badge Workflow]: https://github.com/hyprwm/Hyprland/actions/workflows/ci.yaml/badge.svg

[Badge Issues]: https://img.shields.io/github/issues/hyprwm/Hyprland
[Badge Pull Requests]: https://img.shields.io/github/issues-pr/hyprwm/Hyprland
[Badge Language]: https://img.shields.io/github/languages/top/hyprwm/Hyprland
[Badge License]: https://img.shields.io/github/license/hyprwm/Hyprland
[Badge Lines]: https://img.shields.io/tokei/lines/github/hyprwm/Hyprland
[Badge Hi Mom]: https://img.shields.io/badge/Hi-mom!-ff69b4
