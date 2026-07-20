# Agent Realms: Milestones 0 and 1

This milestone proves that an unmodified Hyprland compositor can run as a
nested Wayland client of the human-controlled host compositor. It does not add
realm lifecycle management, IPC, multi-seat support, containers, virtual input,
or MCP integration.

## Baseline

Baseline date: 2026-07-19

- Branch: `agent-realms`
- Commit: `ef903b8` (`[gha] Nix: update inputs`)
- Running `make debug` and `ctest` directly was not possible because the outer
  shell did not provide `make` or `ctest`.
- In the repository's Nix development shell, `make debug` completed and all
  269 CTest tests passed.

## Implementation notes

- Hyprland does not need a nested-mode command-line flag. Aquamarine requests a
  DRM backend when available and falls back to its Wayland backend when launched
  from an existing Wayland session.
- The launcher gives the child a private `XDG_RUNTIME_DIR`. It passes the host
  Wayland socket as an absolute `WAYLAND_DISPLAY` path so the nested backend can
  still connect to the host.
- The current executor can attempt a user systemd/DBus activation-environment
  import after Aquamarine probes DRM, even when the compositor ultimately uses
  its Wayland backend. The launcher prepends realm-local no-op `systemctl` and
  `dbus-update-activation-environment` guards so the nested proof of concept
  cannot replace host activation variables.
- The realm config disables Hyprland's synthetic `FALLBACK` output. When the
  host closes the Wayland-backed output, the launcher owns shutdown and runtime
  cleanup instead of allowing a headless fallback output to start.
- The development realm is Wayland-only. Nested XWayland is disabled because
  the current fork/dependency combination corrupts allocator state while
  tearing down a nested compositor. This does not change XWayland in the host
  session.
- Hyprland writes instance data below `$XDG_RUNTIME_DIR/hypr/<instance>/`. The
  launcher reads `hyprland.lock` to report the compositor PID and nested Wayland
  socket. Its temporary directory basename is intentionally short so the IPC
  socket path remains below the Unix-domain socket length limit.
- Existing config-launched processes use `CExecutor`, which currently forks and
  invokes `/bin/sh -c`. Several one-shot core helpers instead use
  `Hyprutils::OS::CProcess`. Native realm lifecycle work should prefer an
  argument-vector process API, but that work is outside this milestone.
- Unit tests are GTest cases discovered by CMake. Compositor integration tests
  and test clients live under `hyprtester/`.

## Files

- `example/agent-realm.lua`: minimal, visually distinct nested compositor
  configuration that launches a realm terminal.
- `scripts/run-agent-realm.sh`: isolated development launcher with startup
  discovery and process/runtime cleanup. Once the host window appears, the
  launcher monitors its compositor PID through the host `hyprctl`; closing that
  window stops the already-unusable isolated process group. `Ctrl-C`
  requests graceful compositor shutdown. Both paths remove all realm processes
  and runtime files. The launcher tracks the compositor PID and wrapper PID
  independently.

## Verification

- The Lua configuration passes `Hyprland --verify-config`.
- A nested realm published its private Wayland and Hyprland IPC sockets, and a
  terminal mapped inside the nested desktop.
- `Ctrl-C` stopped the nested compositor and its terminal, then removed the
  private runtime directory.
- Closing the host-side realm window was detected without dispatching commands
  into the host session; the nested process group was stopped and cleaned.

## Manual run

From the repository root, after `make debug`:

```bash
./scripts/run-agent-realm.sh
```

Set `HYPRLAND_REALM_TERMINAL` to override the automatically selected Kitty or
Alacritty command. Set `HYPRLAND_REALM_BINARY` to test another Hyprland binary.
Press `Ctrl-C` in the launcher terminal to terminate the realm and its child
processes.
