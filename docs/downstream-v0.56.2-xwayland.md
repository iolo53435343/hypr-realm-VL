# Hypr Realm downstream: Hyprland 0.56.2 and XWayland

This branch is a maintained downstream of two sources:

- Hyprland tag `v0.56.2` is the compositor baseline.
- The 23 realm feature commits from `0xmiki/hypr-realm` are replayed above that baseline with their original authorship.

Keep the Hyprland and realm source remotes separate. Downstream compatibility fixes and behavior changes belong above the replayed realm series so a later Hyprland update can be audited as a patch stack rather than a blind tree merge.

## Realm-local XWayland contract

Generated and example realm configurations enable XWayland. The nested compositor publishes `hyprland.lock` when its Wayland side is ready, then atomically writes a private `realm-xwayland-display` sidecar only after Xwayland and its XWM have initialized.

The host realm manager promotes a realm to `RUNNING` only when all three readiness values are valid:

- the nested compositor PID and instance signature;
- the nested Wayland socket;
- a local X display token in the form `:<decimal>`.

Direct application launch revalidates the sidecar and sets both `WAYLAND_DISPLAY` and the current nested `DISPLAY`. It sets `XAUTHORITY` to an empty private realm file and removes inherited `SWAYSOCK` and `WAYLAND_SOCKET` values so the child does not fall back to host display credentials or an inherited socket descriptor. Xwayland teardown removes the sidecar immediately; application launch remains blocked until a restarted Xwayland instance republishes readiness.

Missing or malformed X display metadata leaves the realm in `CREATING`; startup then fails through the existing bounded timeout and supervised process cleanup.

## Compatibility changes

- The realm series rebases cleanly from Hyprland `0.55.0` onto `v0.56.2`.
- Hyprland upstream commit `91f29f23bb691462f8aa6171b964069aebc37910` removed an obsolete Glaze `<8` requirement after the `v0.56.2` lock began resolving Glaze 8. This branch backports that one-line CMake compatibility fix.
- The locked XDPH revision contains upstream PR #417's poll-hangup detection. The older private patch is reduced rather than removed: its duplicate detection hunk is dropped while atomic termination, thread joins, expanded disconnect cleanup, and `Restart=always` service recovery remain downstream.

## Verification boundary

Unit tests cover delayed and required XWayland readiness, malformed metadata rejection, atomic no-follow sidecar publication, launch-time `DISPLAY` revalidation across loss/restart, private `XAUTHORITY`, lifecycle cleanup, and the existing realm control protocol.

A real nested runtime smoke is still mandatory before installation. The original `0.55.0` fork documented allocator corruption during nested XWayland teardown. Source compatibility and fake-process tests cannot prove that the `0.56.2` compositor/dependency stack fixed it. Acceptance therefore requires repeated start, X11 application launch, clean stop, force-stop, and host-session survival checks.

No runtime smoke requires replacing or restarting the active host compositor: the candidate binary can first run as a nested test host from its build output.
