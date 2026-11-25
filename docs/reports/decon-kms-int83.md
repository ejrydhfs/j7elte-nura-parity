# int83: explicit page-flip state + force-release of the legacy lock_all

int82's trace showed the flip committing with no buffer
(`async=1 crtc->fb=0 primary->state->fb=0 by kmscube`).  Two fixes:

## 1. Page flip builds its atomic state (0134, module-only)

`exynos7580_crtc_page_flip()` replaces the reliance on
`drm_atomic_helper_page_flip()` (which left the committed plane without the
new framebuffer) with the same sequence built explicitly:

* `drm_atomic_state_alloc()`, `state->acquire_ctx =
  drm_modeset_legacy_acquire_ctx(crtc)`;
* `drm_atomic_get_crtc_state()` + `crtc_state->event = event`;
* `drm_atomic_get_plane_state(primary)` +
  `drm_atomic_set_crtc_for_plane(primary, crtc)` +
  `drm_atomic_set_fb_for_plane(primary, fb)`;
* full-mode src/dst rectangle from `crtc->state->mode`
  (`src_w = hdisplay << 16`, …);
* logs `state/total_planes/plane/planes[0]/plane_state/plane_state->fb/event/fb`;
* sets the legacy `crtc->fb` for `drm_mode_page_flip_ioctl()`'s pre-check and
  calls `drm_atomic_async_commit()`; on success syncs `plane->fb`/`crtc->fb`
  from the new plane state.

Note: the `primary->state->fb=0` in the int82 log was the *pre-swap* state
(commit entry logs `plane->state`, the new state lives in
`state->plane_states[]`); the explicit state now also logs the new plane
state's fb, so the flip buffer is verifiable before commit.

## 2. Dead-owner release of the legacy lock_all (0133, built-in)

`patches/drm-kms/0133-kms-lock-all-owner-death.patch` (peer-authored, commit
708d81a) supersedes the earlier untracked 0131 draft:

* the recorded owner is refcounted (`get_task_struct()` in lock_all,
  `put_task_struct()` in unlock_all and in the rescue path);
* liveness is `!pid_alive(owner) || owner->state == TASK_DEAD` (pid_alive stays
  true through do_exit's final schedule);
* `drm_modeset_lock_all_force_unlock(dev, why)` logs owner/depth/ctx, clears
  owner/depth/ctx under `mode_config.mutex`, drops the ctx locks and unlocks
  the legacy crtc mutexes + `mode_config.mutex`;
* `drm_release()` calls it when the releasing client is the owner (or the owner
  is dead), so a killed client cannot wedge every later DRM ioctl.

## Series in int83

0104–0130 as before, then **0133** (lock force-release) and **0134** (explicit
page flip).  0131 was deleted (superseded), 0132 was renumbered to 0134 so it
cannot collide with the vblank-timestamp patch of the same number.

## Artifacts (image rebuild — 0133 is built-in)

| file | size | sha256 |
|---|---|---|
| `Image-int83` | 18075368 | `b665b9de2fa182de495065dd8df3ac36f8c151cb264223df9a98fdebe0749702` |
| `kernel-boot-int83.img` | 27213824 | `d1f9a91a40363995842b0d0735ef66a671b07efeb9706df375d92dc3e8ab2ef3` |
| `exynos-drm7580-int83.ko` | 1434352 | `0b853e35668074afe0970d87491a63feaf2d5ecdddff43ead2b8e158a4f94f83` |
| `drm_kms_helper-int83.ko` | 1443520 | `2b5915dab44843082182876c6a98611fef0cbeea6dec222ef9502bc6f8d7fc68` |

Module vermagic: `3.10.108 SMP preempt mod_unload aarch64` — it matches this
image (CONFIG_MODULE_UNLOAD=y).  Glue: the int82 `insmod` failure seen by
MesaPair ("kernel expects no mod_unload") means the running kernel then was not
the int82 image; flash the image and the `.ko` pair from the *same* build.

## Gates (PASS)

All int82 gates plus: `drm_lock_all_force_release`/`pid_alive(owner)`/
`get_task_struct`/`put_task_struct`, the `client file release` hook in
drm_fops.c, the force-release log strings in vmlinux, and the flip-state log +
`drm_atomic_set_fb_for_plane(plane_state, fb)` + src/dst rectangle in the
module.

## Expected

`kmscube -D /dev/dri/card0 -c 30`: 30 flips, no "Resource busy", IRQ 234
growing, renderer may still be llvmpipe (Mesa pairing is separate: the render
node is named `panfrost7580`, kmsro wants `panfrost` — see
`docs/reports/kms-mesa-pairing.md`).  `modetest -s`, `dd /dev/fb0`, the
watchdog harness stay as before.

Log sha256s: build `9977780dbabdb262d00f19858ec1b67cf1c4297f38a9d684fc696c37abd579df`, gates `d3caae5edeec102ddfd31f026658ec2ccd40103356ab7b5a9a0db8023a6d050d`, evidence `484bc42df746839f2ccb8245631a1433d70d979173ded369d8f5e191f41ebaf5`, config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.
