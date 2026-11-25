# int77: simplify `drm_modeset_lock_all` for bring-up, log the lock brackets

int76 reached DRM level: module loads, card0/controlD64/fb0 stay, DECON IRQs
run continuously, libdrm opens the device (`driver exynos, version 1.0.0`).
But every userspace entry then blocks in a modeset lock:

```
modetest: drm_modeset_lock_all+0x40 <- drm_mode_getresources
dd /dev/fb0: fb_open+0x84 (mutex_lock(&info->lock))
```

so a lock stayed held after the initial modeset.

## 1. `drm_modeset_lock_all` simplified (0121, built-in)

The M0 4.0 backport takes `mode_config.mutex`, the legacy `crtc->mutex` loop,
**and** `connection_mutex` plus every crtc/plane `modeset_lock` through an
acquire ctx.  For this single-driver bring-up that expansion buys nothing and
is what `drm_mode_getresources` blocked on.  The function now:

* takes `mode_config.mutex` + the legacy `crtc->mutex` loop only;
* still installs `config->acquire_ctx`, because
  `drm_atomic_helper_set_config()` uses it as `state->acquire_ctx` and any w/w
  locks the commit path adds to that ctx are dropped by `unlock_all()`;
* logs `drm_modeset_lock_all: acquired by <comm> (ctx=…)`.

`drm_modeset_unlock_all()` is unchanged (it drops whatever the ctx holds,
unlocks the legacy crtc mutexes and `mode_config.mutex`) and now logs the
caller too, so an enter without a matching exit names the leaking path.

**This is a documented bring-up simplification to revisit before multiple
drivers/clients.**

## 2. fbdev restore bracket logged (0122, module)

`exynos7580_fbdev_start_scanout()` logs
`restore_mode enter/exit (mutex_locked=%d acquire_ctx=%p)` around
`drm_fbdev_cma_restore_mode()`, next to the built-in logs.

## Artifacts (image rebuild — 0121 is in the built-in DRM core)

| file | size | sha256 |
|---|---|---|
| `Image-int77` | 18075368 | `d865135ef23143e8e96efc4f7adeefe2be2232f608aa447b5295cb304c52dbe8` |
| `kernel-boot-int77.img` | 27213824 | `eb56cc01d8bcd9ba3abaafd0c246ba797755b26bd6e9ed29ef43f04ba7197ca0` |
| `exynos-drm7580-int77.ko` | 1434352 | `ec54d4b5af16f32f3a0e5200bade6cd6ff66dfc6113554c99b74e13888409369` |
| `drm_kms_helper-int77.ko` | 1443520 | `badf3e139cdbbaa85e0e61ebc883d25ce3c52cc65cc433e48f3e607a0da5b48a` |

## Gates (PASS)

All int76 gates plus: the lock_all simplification marker, the
`acquired by`/`unlock_all: enter by` logs in `vmlinux`, and the fbdev
`restore_mode enter/exit (mutex_locked=…)` logs in the module.

## What the trace will show

```
drm_modeset_lock_all: enter by kmsload (ctx=(null))
drm_modeset_lock_all: acquired by kmsload (config->acquire_ctx=…)
drm_modeset_unlock_all: enter by kmsload (config->acquire_ctx=…)
exynos7580_fbdev: restore_mode enter (mutex_locked=1 acquire_ctx=…)
exynos7580_fbdev: restore_mode exit  (mutex_locked=0 acquire_ctx=…)
```

If a lock is still held after the load, the last `acquired by` without a
matching `unlock_all` names the path; `fb_open` then follows the same
mutex.

Success: `modetest -c/-p` enumerate, `dd` to `/dev/fb0` returns, `kmscube`
renders 30 frames without a hang.

Log sha256s: build `f26edacb747802785050254a5984904d9071168b2fb9e22db3c554c9a1e244aa`, gates `32f1c7247d7b1f338297df2d2e92fb858ca90a0349106856cac865ff21079169`, evidence `6899461f90537c95d53f000c246a1f3e83b7bc59e48def322b037486cd1e2377`, config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.
