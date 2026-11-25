# int81: owner/depth legacy lock_all (the int80 depth-only scheme was wrong)

int80's log proved the depth-only nesting was wrong:

```
drm_modeset_lock_all: enter by modetest (ctx=0 depth=0)
drm_modeset_lock_all: nested by modetest depth=1 (outer ctx=0)
drm_modeset_unlock_all: enter by modetest (ctx=0 depth=1)
drm_modeset_unlock_all: nested release by modetest depth=0
```

the outer call lost the lock and the next ioctl (`drm_mode_getconnector`)
blocked.  `mode_config.mutex` is not recursive, so the nesting has to be
tracked by **owner task**, not just a counter.

## Fix (0128, built-in DRM core)

* `struct drm_mode_config` gains `struct task_struct *lock_all_owner` (next to
  `lock_all_depth`).
* `lock_all`: if `lock_all_owner == current`, depth++ and return without any
  lock; otherwise take `mode_config.mutex` + the legacy crtc mutexes (with a
  re-check after the mutex in case another task became owner while waiting),
  set owner/depth=1 and install `acquire_ctx`.
* `unlock_all`: only the owner may release; depth-- and only at 0 clear owner,
  drop the ctx, unlock the crtc mutexes and `mode_config.mutex`.  A non-owner
  call logs `is not the owner (...)`, not unlocking.
* Both log `owner=<comm> depth=<n> ctx=<p>`; the **first same-task nested
  lock_all dumps a backtrace**, so the redundant acquisition path is named in
  the next trace (our commit path takes no DRM locks itself, so if it is ours
  the stack will show it).

## Artifacts (image rebuild)

| file | size | sha256 |
|---|---|---|
| `Image-int81` | 18075368 | `659dd29fcc5bb743041d1a93ce69d34620b2be7223bfa82401626fb184529dbb` |
| `kernel-boot-int81.img` | 27213824 | `38fe4d6e261defab0c4b95d6eea3ca21349415bac11d9d3aaff527f1648102df` |
| `exynos-drm7580-int81.ko` | 1434352 | `e343bef29b0e8e0db49150aefcfe29127a8ec24432a9a33761b9eb29987a979a` |
| `drm_kms_helper-int81.ko` | 1443520 | `3f06c4afba533d11b193995933b6ea8e654d2a89afeb70160544006666b0be9e` |

## Gates (PASS)

All int80 gates plus: `lock_all_owner` in the header, the same-task nest and
non-owner logs, the first-nest backtrace string, and the owner/nest log
strings in `vmlinux`.

## Expected

In one boot: `modetest -s 20@18:720x1280` prints "setting mode" without a
hang (IRQ 234 climbing), then `kmscube -D /dev/dri/card0 -c 30` renders
without a segfault; the log shows `owner=<same comm>` for the nested calls and
a single `released by` per outermost lock_all.  The first nest backtrace names
the redundant acquisition if one of ours remains; `dd /dev/fb0` keeps working.

Log sha256s: build `fe1e6a2e7fcfdfec33b2619c4790af09d9d18083c8b6112d78dcfb7d76d63a5b`, gates `4092aada1a0a4b48a4bbdb913714858d250d309f0012bf80d9e9c25935c43500`, evidence `5319dfcb7c4d109bbbc395d72c1a9539509f04d2f988e932b2a5ced1ef4e3969`, config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.
