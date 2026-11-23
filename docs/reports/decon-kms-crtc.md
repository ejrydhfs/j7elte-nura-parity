# DECON KMS — CRTC, link enable and the vendor export shim (M1, slices 10 + 3-shim)

Status: **implemented, build-verified; nothing executed on a device** (no `card0`
exists yet, and no flash/reboot is allowed in this run). Owner: `DeconCrtc`.
Kernel citations are `path:line` in the read-only vendor tree
(`android_kernel_samsung_universal7580-2a746c1…`, 3.10.108); line numbers of
files this slice patches are the **unpatched** ones unless stated otherwise.

Deliverables

| Path | Contents |
|---|---|
| `patches/drm-kms/0100-decon-export-shim.patch` | `include/linux/exynos_decon_kms.h` (the `decon_api.h` contract), the shim `drivers/video/exynos/decon_7580/decon_kms_api.c`, the shared probe body + KMS attach path, the address-based window programming, late `register_framebuffer()`, guarded unregister, the vblank hook in the DECON IRQ |
| `patches/drm-kms/0101-crtc-decon-atomic-crtc-and-link-enable.patch` | `drivers/gpu/drm/exynos/exynos_drm7580_crtc.c` (new file only, no Makefile hunk) |
| `code/gpu31/kms/exynos_drm7580_crtc.c` | plain source of the CRTC |
| `code/gpu31/kms/decon_kms_api.c` | plain source of the vendor shim |

Apply order: M0 `patches/drm-atomic/0001..0004` → `0100` → `0101`.
`0101` additionally needs `drivers/gpu/drm/exynos/exynos_drm7580.h` (the shared
contract header committed in `59638b9`/`f1a770e`, installed by the integration
seat); it is not part of this series, so the two patches never conflict with a
sibling's header delivery. Neither patch touches `drivers/gpu/drm/exynos/Makefile`
or any Kconfig: the build wiring is `KmsAssemble`'s.

---

## 1. The two ownership rules (integration contract)

These were agreed with `DeconPlane`, `KmsFence` and the main agent, and are the
first thing any later slice must read:

1. **`DeconPlane` owns the single `decon_kms_commit()` of an atomic commit.**
   It builds `wins[]` from the *committed* plane states (`ex->planes[i].state`,
   valid because `drm_atomic_helper_swap_state()` runs before `commit_planes`)
   and issues the commit from the highest-index plane in the changeset. This
   file never commits a frame, never touches `crtc_state->event`, and never
   sends a page-flip event. `atomic_flush()` only re-asserts the (idempotent)
   link so a non-modeset commit cannot run on a disabled pipe.
   Consequence enforced here: `atomic_check()` **rejects** a commit that
   activates the CRTC (or needs a modeset) without the primary plane in the same
   changeset — the one case a plane-driven commit would silently miss.
2. **`DeconCrtc` owns the fallible display bring-up, and it runs before
   `swap_state`.**
   `mode_config.funcs.atomic_commit()` is exactly (§2). The M0 helper ignores
   `crtc_funcs->mode_set()`'s return value (`drm_atomic_helper.c`,
   `crtc_set_mode()`: `funcs->mode_set(...)` is called for its side effects), so
   a DSI/panel failure could not abort a commit if it only lived in `mode_set`.
   `mode_set()` therefore re-asserts the same idempotent link, which keeps the
   documented order "DSIM `s_stream(1)` before `decon_reg_init()`"
   (`decon-kms-dsi.md` §2.3).

Two corollaries: the DECON windows are programmed *after* `decon_reg_init()`
(because `commit_planes` runs inside the tail, step d), and a commit that
disables the CRTC programs no windows (the plane hook sees
`!crtc->state->active` and returns; the CRTC's `disable()` stops the pipe).

## 2. Atomic commit order

`mode_config.funcs.atomic_commit(drm, state, async)` — `exynos7580_atomic_commit()`
in the new file:

```
(a) for every crtc_state in `state` that is active and either was not active
    before or has mode_changed:
        exynos7580_crtc_enable_link()          <- the only fallible step
    on error: return it; nothing in software or hardware changed
(b) drm_atomic_helper_prepare_planes()
(c) drm_atomic_helper_swap_state()
(d) exynos7580_commit_tail(state)              <- KmsFence: pre/commit/post
    planes (DeconPlane commits the frame here), flip-event arming at the next
    DECON FRAME interrupt, one-vblank wait, cleanup_planes, state free
return 0
```

`async` is accepted and runs the same tail: M1 is synchronous and a
`NONBLOCK | PAGE_FLIP_EVENT` request is completed by its event, which is what
KWin waits for. `drm_atomic_helper_commit()` is deliberately **not** used — it
never arms the event, which is the 1 s "Pageflip timed out" failure in
`decon-kms-req.md` #22.

`atomic_check` (registered through `drm_atomic_helper_check()`):

* `enable && !active` → `-EINVAL` (cannot have an enabled but inactive CRTC);
* active + `mode_changed|active_changed` → the primary plane must be in the
  changeset, else `-EINVAL`;
* `crtc_link.needs_full_modeset` (set by every link-off, i.e. after any
  `pm_runtime` cycle) forces `state->mode_changed = true`, so the core rejects a
  plane-only commit without `DRM_MODE_ATOMIC_ALLOW_MODESET` and re-runs
  `mode_set()` (a full `decon_reg_init()`/`reg_start()`) when it is allowed.
  This is the `DispPower` §5.1 rule: DECON state does not survive a power cycle.

### TEST_ONLY

The TEST_ONLY path is `drm_mode_atomic_ioctl()` → `drm_atomic_check_only()` →
`drm_atomic_state_free()`, with `dev->mode_config.mutex` held for the whole
operation (`drm_atomic.c:1325-1329`, lock at `:1220-1230`). It never reaches
`config->funcs->atomic_commit`, therefore **no hardware is touched on a
TEST_ONLY commit**: `atomic_check()` calls neither `decon_kms_*` nor any
`decon_export_*` entry point — the only hardware entry points of this file are
in `enable_link()`, `disable_link()`, `mode_set()`, `disable()` and
`atomic_commit()`. `TEST_ONLY | PAGE_FLIP_EVENT` is rejected earlier by the M0
UAPI (`drm_atomic.c:1218-1221`), as KWin expects.

It cannot be *executed* here: there is no KMS node on the target (only
`renderD128` on `panfrost7580`), `modetest`/`kmscube` need a real CRTC, the
series is not assembled into a boot image yet, and this run is not allowed to
flash. The target-side exercise stays exactly the one in `decon-kms-req.md`
§4.3, run under `flock /tmp/gpsdev.lock` + `timeout`:

```sh
modetest -M exynos -p                                   # ACTIVE/MODE_ID/CRTC_ID/SRC_*/FB_ID
modetest -M exynos -a -s <conn>:720x1280 -P <plane>@<crtc>:720x1280
modetest -M exynos -a -v                                # vsynced flips = events + timestamps
kmscube -A -D /dev/dri/card0 -c 300
```

A TEST_ONLY-only probe is what KWin does first (`drm_commit.cpp` TEST_ONLY
commit); the observable outcome in `dmesg` is the absence of
`DECON link enable`/`modeset` lines for the TEST_ONLY pass and their presence
for the following real commit.

## 3. Locking, and why the order cannot deadlock

3.10 has no `ww_mutex`; M0 serialises atomic commits with the single
`drm_device.mode_config.mutex` (`drm_modeset_lock_all()`; the atomic ioctl takes
it explicitly, `drm_atomic.c:1226`). This slice adds one file-scope mutex,
`crtc_lock`, around the link/modeset bookkeeping. The complete nesting graph is

```
mode_config.mutex  ->  crtc_lock  ->  decon->output_lock  ->  decon->mutex
```

* `mode_config.mutex` is only taken by DRM core callers *above* this file
  (ioctl, `drm_crtc_init_with_planes`, fb-helper). Nothing here takes it, so it
  is always the outermost lock and the graph has no edge back to it.
* `crtc_lock` is taken only in `enable_link()`, `disable_link()`,
  `atomic_check()` (a leaf read/write) and `mode_set()`. Nothing calls into DRM
  while it is held; the only locks taken inside it belong to the vendor display
  stack (`decon_kms_enable/disable()` → `decon->output_lock`,
  `vsync_info.irq_lock`, `update_regs_list_lock`).
* `decon->output_lock` and `decon->mutex` are leaves: the vendor code under them
  never calls back into this file or into DRM (it only talks to DSIM through
  `v4l2_subdev_call`, which takes its own device lock and takes neither of the
  outer two).
* The DECON FRAME IRQ takes `decon->slock`, and the KMS callback is invoked
  *after* that spinlock is dropped; the DRM vblank code then takes its own
  `vbl_lock`/`vblank_time_lock`. No path takes `decon->slock` while holding a
  DRM lock, so IRQ context adds no cycle.

The hierarchy is therefore strict and acyclic. The one deliberate exception to
"no sleeping under a spinlock" is elsewhere in the vendor code
(`decon_wait_for_vsync()` is a sleeping wait and is only called from process
context) and is inherited, not introduced.

## 4. `decon_kms_get()`: standing DECON up without the vendor probe

In a KMS image the KMS driver binds `samsung,exynos5-decon_driver` itself, so
`decon_probe()` never runs. `decon_kms_get(&pdev->dev)` calls the shared body
`decon_probe_common(pdev, false)` — the vendor probe is now
`decon_probe_common(pdev, true)` — which performs every step the fbdev probe
performs **except registering `fb0`**. It is the same code, so clocks, IRQ,
media entity, window bookkeeping, the update kthread and runtime PM cannot
drift.

Step list with the errno each step can return (the M1 failure table):

| # | Step | Call | Error |
|---|---|---|---|
| 1 | DECON context | `devm_kzalloc()` | `-ENOMEM` |
| 2 | platform data | `devm_kzalloc()` | `-ENOMEM` |
| 3 | DT parse | `decon_parse_pdata()` | none (no error path exists) |
| 4 | clocks | `decon_int_get_clocks()` | **none** — vendor ignores the result (`decon_core.c:4427`); a missing clock name shows up later as a silent `clk_prepare_enable(NULL)` no-op (power report §7). Kept as-is: changing it would change fbdev probe behaviour |
| 5 | SFR mapping | `platform_get_resource()` + `devm_request_and_ioremap()` | `-ENODEV` (fixed: used to `goto fail` with `ret == 0`) |
| 6 | IRQ | `decon_int_register_irq()` | propagated (`-ENODEV`/`-EBUSY`/`-EINVAL`) |
| 7 | vsync thread | `decon_int_create_vsync_thread()` | propagated |
| 8 | PS/PSR thread | `decon_int_create_psr_thread()` | propagated |
| 9 | TE EINT | `decon_fb_config_eint_for_te()` | propagated (returns 0 in video mode) |
| 10 | LPD work | `decon_int_register_lpd_work()` | propagated |
| 11 | debugfs | `debugfs_create_dir()` | `-ENODEV` (fixed) |
| 12 | ION client | `ion_client_create()` | `-ENODEV` (fixed) |
| 13 | media entity/subdev | `decon_register_entity()` → `find_subdev_mipi()` | propagated (`-ENODEV` if `mdev` is absent) |
| 14 | bootloader pipe quiesce | `decon_reg_init_probe()` + `decon_reg_per_frame_off()` + linecnt-0 wait (video mode, no bootloader fb) | none (vendor) |
| 15 | IOMMU | `iovmm_activate()` | propagated |
| 16 | windows | `decon_acquire_window()` (scratch `fb_info` per window, ION buffer for `default_win`) | propagated (`-ENOENT` from `framebuffer_alloc`, `-ENOMEM` from the buffer) |
| 17 | media device lookup | `module_name_to_driver_data("exynos-mdev")` | `-ENODEV` (fixed) |
| 18 | links | `decon_create_links()` | propagated |
| 19 | subdev nodes | `decon_register_subdev_nodes()` | propagated |
| 20 | update kthread | `kthread_run()` | `PTR_ERR()` |
| 21 | LCD config | `decon_int_set_lcd_config()` | propagated |
| 22 | runtime PM | `pm_runtime_get_sync()` | **ignored** (vendor) |
| 23 | pipe state | `decon_reg_get_stop_status()` → `decon_reg_init_probe()` … `decon_init_done` | none |
| 24 | panel/wakelock/ESD | `device_init_wakeup()`, `pm_stay_awake()`, ESD IRQs | `-EINVAL` from `device_init_wakeup()` |
| 25 | `fb0` | `register_framebuffer()` | only when `register_fb == true` |

`decon_kms_put()` (last reference) reverses it: dma-buf maps, `disable` if the
pipe is on, `kthread_stop`, entity unregister, ION client, LPD workqueue, PSR and
vsync threads, `clk_put`, `iovmm_deactivate`, `pm_runtime_put_sync` then
`pm_runtime_disable`, debugfs, and `platform_set_drvdata(pdev, NULL)` so the
vendor fbdev driver can bind afterwards and probe from scratch. It **does not
`kfree()` the DECON context**: probe allocated it with `devm_kzalloc()`, so it
belongs to the device and is released when the failed KMS probe unwinds (this is
why `decon_kms_get()` cannot take a dangling pointer on the failure path).

What the attach path deliberately does *not* do (KmsCoexist / DsiPanel rules):
it does not touch `dsim@14800000`, does not claim
`samsung,exynos5-mipi-dsi`, does not run the panel rails/reset lists and does not
call the panel's `displayon`. All of that stays with the vendor DSIM driver;
the first `s_stream(1)` performed by `enable_link()` is what re-initialises the
panel if DSIM considers it suspended (`dsim_drv.c:860-909`,
`ea8061_lcd_ctrl.c:1586-1611`).

## 5. What `0100` changes in the vendor tree, precisely

1. `include/linux/exynos_decon_kms.h` — byte copy of `code/gpu31/kms/decon_api.h`.
2. `decon.h` — four new fields on `struct decon_device`
   (`kms_vblank_cb`, `kms_vblank_data`, `kms_owned`, `fb_registered`) and
   prototypes for the functions that stop being file-static plus
   `decon_probe_common()` / `decon_kms_attach()` / `decon_put_clocks()`.
3. `decon_core.c`
   * `decon_to_regs_param`, `decon_activate_vsync`, `decon_deactivate_vsync`,
     `decon_set_win_buffer`, `decon_update_regs`, `decon_acquire_windows`,
     `decon_release_windows`, `decon_unregister_entity` and
     `decon_fourcc_to_format` lose `static` (no body change);
   * new `decon_export_regs_param_addr()` (address path, §introduction) and
     `decon_export_set_win_config()` (fb/ION-free descriptor translation) —
     both `EXPORT_SYMBOL_GPL`; the fourcc→format table lives next to the
     register fill so the vendor pixel-format enum never crosses a file
     boundary (a deliberate deviation from `decon-kms-api.md` §5, which put the
     table in the shim);
   * `decon_probe()` split into `decon_probe_common(pdev, register_fb)` +
     wrapper; `register_framebuffer()` moved from
     `decon_core.c:4591` to the probe tail (after the update kthread,
     `pm_runtime_enable()` and the panel hand-off), with the KMS hand-off hook
     in front of it;
   * four probe paths that returned **0 after failing** (SFR mapping, debugfs
     root, `ion_client_create`, missing `mdev`) now return `-ENODEV` — needed so
     `decon_kms_get()` can fail and the fbdev fallback can bind;
   * `decon_remove()` unregisters `fb0` only if `decon->fb_registered`, and uses
     `windows[default_win]` (index 0 on J7) instead of a hard-coded `[0]`.
4. `decon-int_drv.c` — `decon_int_irq_handler()` records the FRAME interrupt and
   calls `decon->kms_vblank_cb()` after dropping `decon->slock`.
5. `decon_kms_api.c` (new, in `decon-y`) — layer A (`decon_kms_get`/`put`/`caps`/
   `enable`/`disable`/`commit`/`commit_async`/`register_vblank_cb`/
   `wait_vblank`/`power_get`/`power_put`/`map_dmabuf`/`unmap_dmabuf`) and layer B
   (`decon_export_*`), plus the weak `decon_kms_attach()` stub.

`decon_kms_enable()` deliberately converts `DECON_STATE_INIT` to
`DECON_STATE_OFF` before calling the vendor `decon_enable()`: the vendor
short-circuits `INIT` without programming anything (`decon_core.c:901-906`,
"DsiPanel 2.3: do not copy this short-circuit"), and probe leaves the state at
`INIT`. `decon_kms_disable()` is the vendor `decon_disable()`: `s_stream(0)`
first, then `decon_reg_stop()` — the order every screen-off has used since the
product shipped.

`decon_kms_commit()` holds `decon->output_lock` around the programming and calls
`decon_export_update_regs()`, which blocks up to ~600 ms (vsync 300 ms + shadow
update 300 ms): atomic-commit tail only, never IRQ context.
`decon_kms_commit_async()` keeps the vendor `sw_sync`/kthread/fd semantics (and
its "pre-signalled fence when the pipe is off" quirk, which an fd ABI cannot
report as an error); no slice uses it in M1.

## 6. Build evidence

Two trees: my working copy and an independent `DeconCrtc-verify` tree created
from the **pristine** vendor tree, where M0 `0001..0004` + `0100` + `0101` were
applied with `git apply` (`--check` clean before applying).
`drivers/gpu/drm/exynos/exynos_drm7580.h` and build-only `-ENOSYS` stubs for the
sibling slices (`exynos_drm7580_stubs.c`, ≤ 15 lines, **not** in any patch) were
added by hand in that tree only.

```
$ make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS=-Wno-error -j8 \
       drivers/video/exynos/decon_7580/ drivers/gpu/drm/
exit=0
errors: 0
```

Warnings in files this series touches: **one**, pre-existing and on a line the
patch does not modify —

```
drivers/video/exynos/decon_7580/decon_core.c:740:26: warning: '*' in boolean
  context, suggest '&&' instead [-Wint-in-bool-context]
      enable = (rect.w * rect.h) ? true : false;      (decon_core.c:739 pristine)
```

No warning in `decon_kms_api.c`, `decon-int_drv.c`, `decon.h` or
`exynos_drm7580_crtc.c`. The other warnings in the build
(`exynos_drm_fb.h` header guard, `exynos_drm_gem.c`/`_g2d.c` pointer casts,
`exynos_drm_dmabuf.c` format, `dsim_drv.c` unused table) are pre-existing files
this series does not touch.

## 7. Contract gaps and deviations (reported, not silently "fixed")

1. **No accessor for `mode_config.funcs`.** The contract header assigns the CRTC
   to this file but has no way for the main slice to obtain
   `atomic_check`/`atomic_commit`. They are therefore the only two symbols this
   file exports beyond the header:
   `int exynos7580_atomic_check(struct drm_device *, struct drm_atomic_state *)`
   and `int exynos7580_atomic_commit(struct drm_device *,
   struct drm_atomic_state *, bool)`. The main slice points its
   `mode_config.funcs` at them (`fb_create` stays `KmsDmabuf`'s). No header edit
   was made.
2. **No cross-slice window-descriptor accessor.** Because of rule 1 in §1 a
   plane-driven commit needs nothing from this file; if integration later moves
   the commit into `atomic_flush()`, a plane-side
   `exynos7580_plane_fill_win(plane, win)` has to be added to the header first.
3. The fourcc table lives in `decon_core.c` instead of the shim (rationale in
   §5.3) — same supported set (`decon_get_memory_plane_cnt()`).
4. `struct exynos7580_device` still has no CRTC private pointer, so the link
   state is a single file-scope struct; correct for a single-instance driver
   (`get_decon_drvdata()` ignores its id) and asserted by
   `drm->mode_config.num_crtc == 1` in practice.
5. `decon_kms_enable()` inherits the vendor `pm_stay_awake()` wakelock pair
   (`decon_enable()`/`decon_disable()`). It matches today's fbdev behaviour and
   is the reason the platform does not suspend while the panel is on; `DispPower`
   §6.3 item 7 wants a real `.suspend/.resume` instead before M4.
6. `decon_shutdown()` still calls `disable_irq(decon->irq)` unconditionally,
   i.e. `disable_irq(0)` in video mode (`decon-kms-api.md` §7.6). Pre-existing,
   untouched by this series; the KMS path must not reuse `decon_shutdown()` as
   is.
7. The vblank counter/`enable_vblank`/`disable_vblank` `drm_driver` ops stay with
   the main slice; `KmsFence` bumps the counter with
   `drm_handle_vblank(dev, 0)` from the callback registered through
   `decon_kms_register_vblank_cb()`. This file only calls
   `drm_vblank_pre/post_modeset()` around `mode_set()` and `drm_vblank_off()` on
   disable, as the 3.10 API requires.

## 8. Open risks carried into M1 bring-up

* The first KMS commit after `decon_kms_get()` is a full modeset by construction
  (`needs_full_modeset` starts true), so it re-runs `decon_reg_init()` over a
  pipe the bootloader left running; the probe body already quiesced it
  (`decon_reg_init_probe()` + per-frame off + linecnt-0 wait) — the earlier
  dark-screen attempt skipped exactly this.
* `decon_acquire_window()` still allocates the `default_win` framebuffer with
  ION even in KMS mode (≈ 7 MB for 720x1280x32). It is never scanned out in KMS
  mode; freeing it is a cheap M1 follow-up if the memory budget needs it
  (`decon-kms-memory.md`).
* If the panel is found asleep at first commit, `s_stream(1)` runs the full
  `ea8061_init()` (35 ms + 150 ms class); the DECON FIFO underruns for that
  window and the first frame is late. Expected, not a regression.
