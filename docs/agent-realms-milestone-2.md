# Agent Realms: Milestone 2

Milestone 2 adds native realm lifecycle ownership to the host compositor. It
does not expose realm commands through `hyprctl`, associate host windows with
realms, inject input, capture the desktop, start an MCP server, or add container
isolation. Those remain later milestones.

## Lifecycle

`Realm::CRealmManager` owns named `CRealm` records and supports create, start,
pause, resume, stop, destroy, lookup, and enumeration. State changes are checked
against the following lifecycle:

```text
STOPPED -> CREATING -> RUNNING -> PAUSED
              |          |          |
              +----------+----------+-> STOPPING -> STOPPED
              +----------+----------+-> FAILED -> CREATING
```

Stopping a realm retains its private runtime directory so its log remains
available for inspection. Destroying the realm removes that directory. Host
shutdown terminates every active realm and removes all manager-owned runtime
directories.

## Process supervision

- The nested compositor is launched with an argument vector; realm names and
  paths never pass through a shell.
- Every compositor receives a private runtime, cache, state, config, log, and
  Wayland socket namespace.
- Generated realm configs explicitly disable XWayland. Wayland applications
  remain supported inside a realm, while X11-only applications are excluded to
  avoid a reproducible nested-XWayland teardown corruption in the current
  fork/dependency combination. The host compositor's XWayland setting is not
  modified.
- Realm process groups isolate pause, resume, graceful termination, and forced
  termination from the host compositor and other realms.
- A dedicated supervisor waits for the nested compositor and reports its exit
  status over a private pipe. This keeps child reaping correct even when the
  host's process-wide `SIGCHLD` policy uses `SA_NOCLDWAIT`.
- Readiness requires the expected compositor PID in `hyprland.lock` and a live
  Unix-domain Wayland socket in the private runtime directory.
- Startup timeout, exec failure, unexpected exit, and crashes move the realm to
  `FAILED`.
- Only directories created and recorded by the manager can be recursively
  removed.

The manager is created during Hyprland's late manager initialization, after the
host Wayland socket exists. It is destroyed early during compositor cleanup so
realms stop before the host event loop and socket disappear. Its event-loop
poller remains disarmed when no realm processes exist, so normal compositor idle
behavior does not gain a periodic wakeup.

## Tests

The lifecycle tests use a standalone fake compositor process. It creates only
temporary files and Unix sockets below a per-test directory in `/tmp`; it does
not connect to a display or control the running Hyprland session.

Coverage includes:

- valid and invalid state transitions;
- duplicate and invalid names;
- full start, pause, resume, stop, restart, and destroy behavior;
- literal handling of command-like realm names;
- startup failure and readiness timeout;
- unexpected crash detection and exit status;
- independent concurrent realms;
- forced shutdown of a process that ignores `SIGTERM`;
- supervisor reaping and runtime cleanup.

Verification on 2026-07-19:

- the `Hyprland` executable linked successfully;
- all 16 Realm tests passed;
- all 285 discovered CTest tests passed.

## Next boundary

Milestone 3 will add administrative `hyprctl` commands, JSON output, useful
errors, and lifecycle events. Until that IPC exists, the native manager has no
external command surface and cannot be used by an MCP server.
