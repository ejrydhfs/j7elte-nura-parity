# DECON window-API reuse contract for KMS

Status: **contract fixed; shim patch specified, not applied**. Slice 3 of the
DECON KMS plan (`docs/reports/decon-kms-plan.md` §6). Kernel tree surveyed
read-only; every `file:line` below is
`android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07/drivers/video/exynos/decon_7580/`
unless the path says otherwise.

Deliverable of this slice: `code/gpu31/kms/decon_api.h` (install path
`include/linux/exynos_decon_kms.h`), the fb-free reuse contract.

---

## 1. What the window API is today

Three layers, from userspace ABI down to SFRs.

```
S3CFB_WIN_CONFIG (fb ioctl)                        decon_core.c:2446
  decon_set_win_config(decon, win_data)            decon_core.c:2185   [static]
    per window i < pdata->max_win:
      DECON_WIN_STATE_COLOR  -> vidosd_a/b, alpha  decon_core.c:2273-2280
      DECON_WIN_STATE_BUFFER -> decon_set_win_buffer    decon_core.c:1516 [static]
          ion_import_dma_buf -> dma_buf_get -> dma_buf_attach
          -> ion_iovmm_map -> exynos_ion_sync_dmabuf_for_device
          -> win->fbinfo->var/fix bookkeeping          :1554-1600
          -> regs->dma_buf_data[i], buf_start, vidosd_*,
             whole_w/h, offset_x/y, wincon, rgborder,
             blendeq                                 :1690-1701
      -> memcpy loop copies configs into struct decon_reg_data (:389 decon.h)
      -> sw_sync_pt_create + sync_fence_install, fd returned in win_data->fence
      -> list_add_tail + queue_kthread_work            :2342-2353
  decon_update_regs_handler (kthread)              decon_core.c:2162
    decon_update_regs(decon, regs)                 decon_core.c:2066  [static]
      sync_fence_wait() on each in-fence           :2093-2095 (:1980)
      decon_set_qos() bandwidth                    :2097-2098
      __decon_update_regs()                        decon_core.c:1916  [static]
        HW-trigger mask (only if trig_mode == HW)  :1930-1932
        shadow protect every window                :1934-1936
        decon_reg_chmap_validate()                 :1886
        decon_reg_set_regs_data() per window       decon_reg_7580.c:529
        shadow unprotect every window              :1963-1965
        decon_set_protected_content() exynos_smc() :1433
        decon_reg_start() (ENVID|ENVID_F)          decon_reg_7580.c:480
      decon_wait_for_vsync(decon, 300ms)           decon_core.c:1226
      decon_reg_wait_for_update_timeout(300ms)     decon_reg_7580.c:616
      size-mismatch guard, I80 ack, trigger mask   :2116-2148
      free previous windows' dma_bufs              :2151-2155
      sw_sync_timeline_inc(timeline, 1)            :2157
```

J7 specifics that change the picture (both from the board DT, not defaults):

| DT property | Value | Effect on the commit path |
|---|---|---|
| `psr_mode` | `<0>` video mode (`exynos7580-j7elte_rev00.dts:1016`) | FRAME IRQ = IRQ resource 2 (`decon-int_drv.c:788-797`); `decon_activate_vsync()` really enables the FRAME interrupt (`decon_core.c:1184-1196`) |
| `trig_mode` | `<1>` SW trigger (`:1017`) | commit never touches TRIGCON (`__decon_update_regs`:1930 gates on `HW_TRIG`); latching is done by the shadow registers closing plus `decon_reg_update_standalone` |
| `lcd_info` | `ea8061` (`:1023`) | `decon->lcd_info` is the DSIM subdev's hostdata, set in `find_subdev_mipi()` (`decon-int_drv.c:220`) — the DECON driver does not own the timings |
| LPD | `CONFIG_DECON_LPD_DISPLAY=y` (Kconfig:43-47) | **unreachable on J7**: LPD entry is only queued from the TE GPIO IRQ, which `decon_fb_config_eint_for_te()` does not even register outside MIPI command mode (`decon-int_drv.c:808-814`). A KMS client does not have to fight LPD. |
| `exynos7580.dtsi:1078-1079` | `compatible = "samsung,exynos5-decon_driver"` | binding must not steal this (§6) |

---

## 2. Inventory: what a CRTC can call

Legend for *reuse*: **R** = reusable as-is, **S** = reusable after splitting the
fb part out, **F** = fb-specific, do not reuse, **D** = dead/do not export.

### 2.1 Mode set / enable / disable

| Symbol | Location | Signature | Locking & context | PM | Reuse |
|---|---|---|---|---|---|
| `decon_enable` | `decon_core.c:894-1012` | `int (struct decon_device *)` | takes `decon->output_lock` unless state is `LPD_EXIT_REQ`; sleeps: `pm_runtime_get_sync`, DSIM `s_stream(1)` via `v4l2_subdev_call`, `iovmm_activate`, `enable_irq`; not idempotent-safe (returns 0 with a warning if already ON) | requires pd_disp + clocks, gets them itself | **R** for the KMS "CRTC enable after panel init" step, but it starts DSIM streaming — the DSI encoder slice must own that ordering |
| `decon_disable` | `:1014-1127` | `int (struct decon_device *)` | `output_lock`; `flush_kthread_worker(&update_regs_worker)`; `disable_irq(irq)` guarded by `vsync_info.irq_refcount <= 0`; `iovmm_deactivate`; `pm_runtime_put_sync` | yes, symmetric | **R** |
| `decon_blank` | `:1129-1175` | `int (int blank_mode, struct fb_info *)` | fb boundary | — | **F** (thin wrapper over enable/disable) |
| `decon_int_set_lcd_config` | `decon-int_drv.c:680-715` | `int (struct decon_device *)` | none; writes `win->win_mode` + `fbinfo->var/fix` | clocks on | **F**: pure fb plumbing over `decon_parse_lcd_info()` (`:661`), which is reusable but trivial |
| `decon_reg_init` | `decon_reg_7580.c:414-445` | `void (u32 id, enum decon_dsi_mode, struct decon_init_param *)` | none, writes SFRs | clocks + pd_disp on | **R** (already global) |
| `decon_reg_start` | `decon_reg_7580.c:480-490` | `void (u32 id, enum decon_dsi_mode, struct decon_psr_info *)` | none | same | **R** (already global) |
| `decon_reg_stop` | `decon_reg_7580.c:492-527` | `int (u32, enum decon_dsi_mode, struct decon_psr_info *)` | none; polls linecnt ≤ 50 ms, stop status ≤ 20 ms (`udelay` bubble) | same | **R** |

### 2.2 Window config, commit, shadow protect / trigger

| Symbol | Location | Signature | Locking & context | PM | Reuse |
|---|---|---|---|---|---|
| `decon_set_win_config` | `decon_core.c:2185-2358` | `int (struct decon_device *, struct decon_win_config_data *)` | no lock of its own (caller `decon_ioctl` holds nothing; `output_lock` at `:2198`, `get_unused_fd()` at `:2199`); **needs a live fd table** (`get_unused_fd()` `:2199`); queues to kthread; returns before the frame is programmed | requires DECON ON; silently no-ops (returns a pre-signalled fence) when state is OFF / TUI (`:2218-2228`) | **S**: the descriptor→`decon_reg_data` translation and the queue/fence plumbing must be separated |
| `decon_set_win_buffer` | `:1516-1708` | `int (struct decon_device *, struct decon_win *, struct decon_win_config *, struct decon_reg_data *)` | sleeps in `dma_buf_attach()`/`ion_iovmm_map()`; no lock | needs `decon->ion_client` (created in probe `:4470`) and iovmm active | **S**: lines `:1554-1600` are fb bookkeeping, `:1690-1701` are the reusable register fields. ION-only — see §7.1 |
| `decon_calc_plane_offset` | `:1411-1431` | `void (struct decon_win_config *, struct decon_dma_buf_data *)` | none | none | **R** (NV12/NV21 interleaved plane offset) |
| `decon_set_alpha_blending` | `:1479-1514` | `int (struct decon_win_config *, struct decon_reg_data *, int, int)` | none | none | **R** |
| `decon_get_memory_plane_cnt` | `:1369-1409` | `int (enum decon_pixel_format)` | none | none | **R** (format → 1/2 planes, -1 = unsupported) |
| `decon_set_win_blocking_mode` | `:648-729` | `int (struct decon_device *, struct decon_win *, struct decon_win_config *, struct decon_reg_data *)` | none | none | **R** (enabled by `CONFIG_DECON_BLOCKING_MODE=y`, Kconfig:61-64); pure math on `dst` rects |
| `__decon_update_regs` | `:1916-1978` | `void (struct decon_device *, struct decon_reg_data *)` | none; writes SFRs, touches `decon->windows[i]->dma_buf_data` and `cur_protection_bitmask` | clocks on; SMC for protection | **S**: reusable body; the only fb object it touches is `decon->windows[i]` for the cached buffer |
| `decon_update_regs` | `:2066-2160` | `void (struct decon_device *, struct decon_reg_data *)` | **blocks up to ~600 ms** (`decon_wait_for_vsync` 300 ms + `wait_for_update_timeout` 300 ms); takes no lock of its own; releases the previous frame's dma-bufs at the end | clocks on | **R** — this is the commit primitive a CRTC wants |
| `decon_update_regs_handler` | `:2162-2183` | `void (struct kthread_work *)` | runs in `update_regs_thread`; `update_regs_list_lock` for the list, then calls `decon_update_regs` unlocked | — | **R** (async flavour) |
| `decon_reg_set_regs_data` | `decon_reg_7580.c:529-566` | `void (u32 id, int win_idx, struct decon_regs_data *)` | none; raw SFR writes incl. `WINCON`, `WIN_MAP`, `VIDOSD_A..D`, `VIDW_ADD0/2/3`, `WHOLE_X/Y`, `OFFSET_X/Y`, `BLENDE` | clocks on | **R** (already global) |
| `decon_reg_shadow_protect_win` | `decon_reg_7580.c:640-646` | `void (u32 id, u32 win_idx, u32 protect)` | none | clocks on | **R** |
| `decon_reg_activate_window` | `decon_reg_7580.c:648-652` | `void (u32 id, u32 index)` | none | clocks on | **R** |
| `decon_reg_set_trigger` | `decon_reg_7580.c:599-614` | `void (u32 id, enum decon_dsi_mode, enum decon_trig_mode, enum decon_set_trig)` | none; also stores `decon->trigger_enable` | clocks on | **R** (unused on J7's SW-trigger config) |
| `decon_reg_wait_for_update_timeout` | `decon_reg_7580.c:616-638` | `int (u32 id, unsigned long timeout)` | `udelay(100)` poll of `DECON_UPDATE` | clocks on | **R** |
| `decon_reg_chmap_validate` | `decon_core.c:1886-1900` | `void (struct decon_device *, struct decon_reg_data *)` | none | — | **R** (drops windows that would double-map an IDMA channel) |
| `decon_set_protected_content` | `:1433-1477` | `static void (struct decon_device *, bool)` | may poll linecnt (35 ms) and calls `exynos_smc(SMC_PROTECTION_SET, 0, DRM_DEV_DECON, en)` `:1461` | clocks on | **R**, internal to the commit body |
| `decon_fence_wait` | `:1980-1990` | `void (struct sync_fence *)` | `sync_fence_wait(fence, 900)`, warns on error | — | **F-adjacent**: Android `sync_fence`, not `dma_fence` |

### 2.3 Vblank

| Symbol | Location | Signature | Locking & context | PM | Reuse |
|---|---|---|---|---|---|
| `decon_int_irq_handler` | `decon-int_drv.c:76-124` | `irqreturn_t (int, void *)` | **hard IRQ**; `spin_lock(&decon->slock)`; on `VIDINTCON1_INT_FRAME` sets `vsync_info.timestamp` and `wake_up_interruptible_all(&vsync_info.wait)` `:105-110` | clocks on (SFR reads) | **R** — the only place a `drm_crtc_handle_vblank()` hook belongs |
| `decon_fb_isr_for_eint` | `decon-int_drv.c:717-763` | `irqreturn_t (int, void *)` | hard IRQ; same wake-up `:756-758`; also drives SW trigger + packet-go + LPD entry | — | **R** in command mode only; not registered on J7 |
| `decon_wait_for_vsync` | `decon_core.c:1226-1266` | `int (struct decon_device *, u32 timeout_ms)` | sleeps; `vsync_info.irq_lock` around the FRAME-interrupt refcount (`:1177-1224`); `wait_event_interruptible_timeout` — **returns 0 when interrupted by a signal**, only `-ETIMEDOUT` on timeout | clocks on | **R** (blocking wait); not usable from atomic context |
| `decon_set_vsync_int` | `:1286-1301` | `int (struct fb_info *, bool)` | flips `vsync_info.active` + refcounts the IRQ | — | **F** (fb ioctl entry for the sysfs `vsync` attribute) |
| `decon_int_create_vsync_thread` | `decon-int_drv.c:871-889` | `int (struct decon_device *)` | creates `s3c-fb-vsync` kthread + `vsync` sysfs file (`:834-860`) | — | **F** |

### 2.4 Buffers / formats

| Symbol | Location | Signature | Locking & context | PM | Reuse |
|---|---|---|---|---|---|
| `decon_map_ion_handle` | `decon_core.c:1303-1347` | `unsigned int (struct decon_device *, struct device *, struct decon_dma_buf_data *, struct ion_handle *, struct dma_buf *, int win_no)` | sleeps; `dma_buf_attach`, `dma_buf_map_attachment`, `ion_iovmm_map`, `exynos_ion_sync_dmabuf_for_device` | iovmm active | **D** for DRM: `ion_iovmm_map()` is ION-exporter-only (§7.1). The *sequence* is the template for the replacement |
| `decon_free_dma_buf` | `:1349-1367` | `void (struct decon_device *, struct decon_dma_buf_data *)` | `ion_iovmm_unmap` + `dma_buf_unmap_attachment` + `ion_free`; lock-free | — | **D** for DRM, same reason |
| `decon_fb_alloc_memory` | `:2739-2818` | `int (struct decon_device *, struct decon_win *)` | ION alloc + `ion_map_kernel`, or `dma_alloc_writecombine` without ION | iovmm | **F** (fb0's own buffer) |

### 2.5 Suspend / resume, clocks, power

| Symbol | Location | Signature | Locking & context | PM | Reuse |
|---|---|---|---|---|---|
| `decon_runtime_resume` | `decon_core.c:2542-2566` | `int (struct device *)` | takes `decon->mutex`; `decon_int_set_clocks`, then `clk_prepare_enable` of pclk/aclk/eclk/vclk/aclk_disp | is the PM | **R** but not directly callable: registered in `decon_pm_ops` (`:2589-2592`), driven by `pm_runtime_get_sync` only |
| `decon_runtime_suspend` | `:2568-2587` | `int (struct device *)` | mirror | is the PM | same |
| `decon_int_get_clocks` | `decon-int_drv.c:130-175` | `int (struct decon_device *)` | `clk_get()` ×7 by name; probe-time only | probe | **R** |
| `decon_int_set_clocks` | `decon-int_drv.c:177-201` | `void (struct decon_device *)` | programs PLL/mux/divider chain; display must be stopped | clock on | **R** |
| `decon_put_clocks` | `decon_core.c:2532-2540` | `void (struct decon_device *)` | `clk_put` ×6 (`lh_disp0/1`, `dsd`, `aclk_disp_200` are never `clk_get`-ed — asymmetry is pre-existing) | remove | **R** |
| `decon_clocks_info` | `:2518-2530` | `void (struct decon_device *)` | debug print | — | **R** (harmless) |
| `decon_clock_on` / `decon_clock_off` / `init_display_decon_clocks` / `disable_display_decon_clocks` | declared `decon.h:804-808` | — | — | — | **D**: **no definition in `decon_7580/`**. The same names do exist in the *other* vendor stack, `drivers/video/exynos/decon_display/decon_pm_exynos5430.c:213,590,640`, with incompatible parameters (`struct display_driver *`), and that stack is off in the j7 config (`lineageos_j7elte_defconfig:2346`). Do not export the `decon_7580` declarations — the real clock entry points are `decon_int_get_clocks`/`_set_clocks` plus runtime PM |
| `decon_board.c` `run_list()` | `decon_board.c:449-461` | `void (struct device *, const char *)` | runs DT-encoded regulator/gpio/delay sequences | panel on | **R** but DSIM-owned: callers are `dsim_drv.c:839,851,853`; the DECON KMS path must not run them |

### 2.6 Probe / remove (binding surface)

`decon_probe` `decon_core.c:4385-4817` · `decon_remove` `:4819-4842` ·
`decon_shutdown` `:4844-4864` · `decon_driver` `:4867-4878` ·
`late_initcall(exynos_decon_register)` `:4890`.
`of_device_id` table `:56` (`samsung,exynos5-decon_driver`).
`decon_parse_pdata()` `:2936-2998` reads **only** DT properties; the non-DT
branch is only the `decon_warn("no device tree information")` at `:2996`,
with no property parsing (`:2995-2997`),
so there is no board-file/platform-data configuration path in this driver.

---

## 3. fb-specific vs genuinely reusable — the summary

**fb objects that the window API currently drags in**

| fb artefact | Where | Why it exists | Verdict |
|---|---|---|---|
| `framebuffer_alloc(sizeof(struct decon_win))` | `decon_acquire_windows` `:2846` | the *only* allocator for `struct decon_win`; `win = fbinfo->par` | harmless: allocate it, never register it. `decon_set_win_buffer` dereferences `win->fbinfo->var/fix`, so a KMS path that reuses that function needs the scratch `fb_info` |
| `fb_alloc_cmap`/`fb_set_cmap` | `:2909-2913` | fb console palette | harmless, unused by KMS |
| `register_framebuffer` | `:4591` | creates `/dev/fb0` | **this** is the line that decides fbdev-vs-KMS visibility |
| `struct fb_ops decon_fb_ops` + `decon_check_var/set_par/blank/setcolreg/pan_display/mmap/ioctl` | `:2499-2515`, `decon-int_drv.c:344/388/542/500/617` | userspace fb ABI | **F** |
| `win->fbinfo->var/fix` writes | `:1554-1600` | colour bitfields, `smem_start/len`, `line_length`, pansteps | **S**: a pure `struct decon_win_config` → `regs` translation can be lifted out (`decon_export_regs_param`) |
| `decon_win_config_data.fence` + `S3CFB_*` ioctl numbers | `decon.h:421`, `:879-903` | magic ioctl ABI | **F** |
| sysfs `vsync` / `psr_info` | `decon-int_drv.c:860,869` | fb-era vblank polling for Android | **F** |
| `decon->ion_client`, `decon->timeline` | probe `:4443,4470` | fd→IOVA and Android fences | **D** for DRM (see §7) |

**Genuinely reusable (pure hardware)**: everything in `decon_reg_7580.c`; the
geometry/colour math in `decon_core.c:119-200,551-600,1411-1514`;
`__decon_update_regs()`; `decon_update_regs()`; the FRAME IRQ body;
`decon_enable/disable`; `decon_wait_for_vsync`.

**The single most important finding** for the CRTC slice: the commit engine is
already synchronous and fb-free (`decon_update_regs`), the buffer attachment is
not (`decon_set_win_buffer`). A CRTC should build `struct decon_reg_data`
itself from DMA addresses it already owns and call the commit body directly —
that path touches no `fb_info`, no ION and no `sync_fence`.

---

## 4. The contract header

`code/gpu31/kms/decon_api.h` (install as `include/linux/exynos_decon_kms.h`).

Design rules baked into it:

1. **No fb types, no ION, no `sync_fence`** appear in the header. `struct
   decon_device`/`decon_reg_data`/`decon_win_config` are incomplete types, so
   the vendor layouts cannot leak into drivers/gpu/drm.
2. **Formats are DRM fourcc**, not the vendor `enum decon_pixel_format`: the
   plane slice already has `drm_framebuffer->pixel_format`, and the fourcc →
   vendor-enum table lives once, in the shim (supported: ARGB8888, ABGR8888,
   RGBA8888, BGRA8888, XRGB8888, XBGR8888, RGBX8888, BGRX8888, RGBA5551,
   RGB565, NV12, NV21 — the exact set `decon_get_memory_plane_cnt()` accepts,
   `decon_core.c:1369-1409`; note 3.10's `drm_fourcc.h` has no `*M` variants
   for the multi-plane formats).
3. **Layer A** (`decon_kms_get`, `_enable`, `_disable`, `_commit`,
   `_commit_async`, `_register_vblank_cb`, `_wait_vblank`, `_power_get/put`,
   `_map_dmabuf`) is the stable surface; **layer B** is the raw symbol export
   list with context/locking documented on each prototype.
4. Every prototype documents **context, locking, PM requirement and blocking
   time**, because the vendor code is inconsistent here: `decon_enable` takes
   `output_lock` itself, `decon_update_regs` takes none, `decon_set_win_config`
   takes it around the fd dance, and `decon_wait_for_vsync` takes
   `vsync_info.irq_lock`.

---

## 5. Shim patch: exact symbols, exact edits

Shape: one new file `drivers/video/exynos/decon_7580/decon_kms_api.c`, added to
`decon-y` in `Makefile:11` (`decon-y += ... decon_kms_api.o`). It holds the
wrappers, the fourcc table and the dma-buf mapping. No vendor function changes
signature; the statics get thin wrappers instead of being un-static'd, so
`struct decon_device` never becomes part of an exported signature (the vendor
header pulls `<media/videodev2_exynos_media.h>`, `<media/exynos_mc.h>`,
`<mach/bts.h>` — exporting that ABI would be worse than a 30-line wrapper).

### 5.1 Symbols to export (exact list)

Already global in the vendor tree — **no patch needed**, declared by
`decon_common.h` (fb-free, includes only `panels/decon_lcd.h`):
`decon_reg_init`, `decon_reg_init_probe`, `decon_reg_start`, `decon_reg_stop`,
`decon_reg_set_regs_data`, `decon_reg_set_int`, `decon_reg_set_trigger`,
`decon_reg_wait_for_update_timeout`, `decon_reg_shadow_protect_win`,
`decon_reg_activate_window`, `decon_enable_eclk_idle_gate`,
`decon_reg_reset`, `decon_reg_clear_int`, `decon_reg_direct_on_off`,
`decon_reg_per_frame_off`, `decon_reg_update_standalone`,
`decon_reg_set_winmap`, `decon_reg_get_stop_status`,
`decon_reg_wait_stop_status_timeout`, `decon_reg_is_shadow_updated`.

The statics (`decon_core.c` / `decon-int_drv.c`) that must become callable, with
the wrapper the shim adds:

| Vendor function (static) | Location | Shim symbol | Note |
|---|---|---|---|
| `decon_acquire_windows` / `decon_release_windows` | `:2836-2917` / `:2733-2737` | `decon_export_acquire_windows`, `decon_export_release_windows` | per-window `fb_info` scratch; never registered |
| `decon_set_win_buffer` | `:1516-1708` | `decon_export_set_win_buffer` | ION fd path, for the fbdev-emulation slice only |
| `decon_to_regs_param` + `decon_reg_set_regs_data` | `:119-142`, `decon_reg_7580.c:529` | `decon_export_regs_param` | the fb-free half |
| `decon_update_regs` | `:2066-2160` | `decon_export_update_regs` | the commit primitive |
| descriptor loop of `decon_set_win_config` | `:2250-2324` | `decon_export_set_win_config` | no fd/ion/sw_sync |
| — | — | `decon_export_regs_param_addr` | new: register fill from `dma_addr_t[3]`, the piece `decon_set_win_buffer` lacks |
| — | — | `decon_export_regs_alloc/free`, `decon_export_dev` | new: `struct decon_reg_data` is ≈3 KB on arm64 (`decon.h:389`) and callers must not see it |

Also already global but worth a uniform name (no patch needed, only
`decon.h`/`decon_core.c` declarations): `decon_int_get_clocks`
(`decon-int_drv.c:130`), `decon_int_set_clocks` (`:177`), `decon_put_clocks`
(`decon_core.c:2532`); the shim re-exports them as `decon_export_get_clocks`,
`decon_export_set_clocks`, `decon_export_put_clocks` so that a consumer never
includes `decon.h`.

`EXPORT_SYMBOL()` for each of the above **only matters if the consumer can be a
module**: `drivers/video/exynos/decon_7580/Kconfig:1-3` makes
`EXYNOS_DECON_EXYNOS7580` a `bool` and `Makefile:10-11` builds `decon.o` into
`built-in.o`, so today a linked-in DRM subdriver resolves them at link time and
the exports are inert. Emit them anyway — they cost nothing and make the
`CONFIG_DRM=m` fallback, and `modprobe`-free debugging, possible.

### 5.2 The two entry points worth reading in full

```c
/* decon_kms_api.c -- compiled into decon.o, sees struct decon_device */
struct decon_kms_dev {
	struct decon_device	*decon;
	struct decon_kms_caps	caps;
	decon_kms_vblank_cb	vblank_cb;
	void			*vblank_data;
};

int decon_kms_commit(struct decon_kms_dev *d,
		     const struct decon_kms_win *wins, unsigned int nr_wins)
{
	struct decon_device *decon = d->decon;
	struct decon_win_config cfg[MAX_DECON_WIN] = { 0 };
	struct decon_reg_data *regs;
	unsigned int i;
	int ret;

	if (decon->state != DECON_STATE_ON)
		return -EAGAIN;			/* contract 7.4 */

	regs = decon_export_regs_alloc();	/* kzalloc, ~3 KB */
	if (!regs)
		return -ENOMEM;

	mutex_lock(&decon->output_lock);	/* serialise with the fb ioctl path */

	for (i = 0; i < nr_wins && i < decon->pdata->max_win; i++) {
		ret = decon_kms_to_vendor(&wins[i], &cfg[i]);	/* fourcc table */
		if (ret)
			goto out;
		if (wins[i].state == DECON_KMS_WIN_BUFFER) {
			ret = decon_export_regs_param_addr(decon, regs, i,
							   &wins[i]);
			if (ret)
				goto out;
		}
	}

	ret = decon_export_set_win_config(decon, regs, cfg, nr_wins);
	if (ret)
		goto out;

	decon_export_update_regs(decon, regs);	/* blocks <= ~600 ms */
out:
	mutex_unlock(&decon->output_lock);
	decon_export_regs_free(regs);
	return ret;
}
```

`decon_export_regs_param_addr()` is the piece `decon_set_win_buffer()` does
not have: it fills `regs->buf_start/whole_w/whole_h/offset_x/offset_y/wincon/
blendeq/vidosd_*` and `regs->dma_buf_data[i][0..2].dma_addr` from
`dma_addr_t addr[3]` instead of resolving `fd_idma[]`
(`decon_core.c:1690-1701` is the code it mirrors). That is the entire change
`decon_set_win_buffer` needs to become exporter-agnostic.

```c
int decon_kms_map_dmabuf(struct decon_kms_dev *d, struct dma_buf *buf,
			 unsigned int id, size_t size,
			 dma_addr_t *addr, void **cookie)
{
	struct decon_device *decon = d->decon;
	struct dma_buf_attachment *att;
	struct sg_table *sgt;
	struct decon_kms_map *m;

	att = dma_buf_attach(buf, decon->dev);
	if (IS_ERR(att))
		return PTR_ERR(att);
	sgt = dma_buf_map_attachment(att, DMA_TO_DEVICE);
	if (IS_ERR(sgt)) { dma_buf_detach(buf, att); return PTR_ERR(sgt); }

	/* exporter-agnostic: no ion_iovmm_map(), which BUG_ON()s on
	 * non-ION dma-bufs (drivers/staging/android/ion/ion.c:2643) */
	m->iova = iovmm_map(decon->dev, sgt->sgl, 0, size ?: buf->size,
			    DMA_TO_DEVICE, id);
	if (IS_ERR_VALUE(m->iova)) { /* no IOMMU domain: use sg_dma_address */ }
	...
}
```

Note that `iovmm_map()` is the generic exynos primitive
(`include/linux/exynos_iovmm.h:47`) that `ion_iovmm_map()` itself calls
(`ion.c:2604`), so the mapping the DECON sees is identical to the vendor path.

### 5.3 Refactors the shim needs (all mechanical)

1. `decon_set_win_buffer()` → factor `:1554-1600` (fb var/fix) out of the
   register fill; keep the fd/ION front half as `decon_export_set_win_buffer()`.
2. `decon_set_win_config()` → factor `:2250-2324` (per-window translation,
   bandwidth, `regs->win_config[]` copy) into `decon_export_set_win_config()`;
   leave the `get_unused_fd`/`sw_sync`/queue code in the fb ioctl path.
3. Add a vblank hook to `struct decon_device` (`decon.h:644`) and call it from
   `decon_int_irq_handler()` next to the existing
   `wake_up_interruptible_all(&decon->vsync_info.wait)` (`decon-int_drv.c:110`)
   and from `decon_fb_isr_for_eint()` (`:758`). 3 lines + one field.
4. `decon_acquire_windows()` → split the `framebuffer_alloc` + `fb_alloc_cmap`
   part from the mode/geometry part, or just keep using it as-is (it needs no
   registered `fb_info`).
5. Delete nothing: the fbdev path keeps working untouched when KMS is off.

---

## 6. Binding without claiming the fb compatible

Constraint from `patches/drm-kms-coexist/DESIGN.md` §4 and the earlier attempt:
binding DRM to `samsung,exynos5-decon_driver` makes `of_match` hand the node to
the DRM driver; the vendor `decon_driver` (`decon_core.c:4867`) never probes, so
`/dev/fb0` disappears and the panel can go dark with no fallback. Also no DT
change is acceptable (S-Boot consumes the shipped DT payload).

Note on the "platform data hand-off" option: this driver has no board-file
platform data. `decon_parse_pdata()` (`decon_core.c:2936`) fills
`struct exynos_decon_platdata` exclusively from DT properties and the non-DT
branch is empty (`:2995-2997`, "no device tree information"). The only
legitimate hand-off is therefore an **in-kernel pointer hand-off inside
`decon_probe()`** — which is exactly what Option A does. Injecting pdata via
`platform_device_register_simple()` would mean inventing a second
configuration source for the whole display block, and it would still not solve
the "who owns `register_framebuffer()`" question.

### Option A — Kconfig `choice` inside `decon_7580/Kconfig` (recommended)

```kconfig
choice
	prompt "DECON output"
	default EXYNOS_DECON_7580_FBDEV
config EXYNOS_DECON_7580_FBDEV
	bool "fbdev (register /dev/fb0)"
config EXYNOS_DECON_7580_KMS
	bool "DRM/KMS subdriver"
	select DRM
	select DRM_KMS_HELPER
endchoice
```

`register_framebuffer()` currently sits at `decon_core.c:4591` — **too early for
KMS**, because everything after it (`mutex_init(&decon->output_lock)`,
`kthread_run(update_regs_thread)`, `decon_int_set_lcd_config()`,
`pm_runtime_enable()`, the panel power-on sequence and
`decon_register_esd_funcion()`) is what makes the window API usable. The patch
therefore *moves* the call to the tail of `decon_probe()` and puts the choice
there:

```c
/* tail of decon_probe(), between decon_esd_enable_interrupt(decon) (:4770)
 * and decon_info("decon registered successfully") (:4772); this replaces the
 * register_framebuffer() call that used to be at :4591 */
	if (IS_ENABLED(CONFIG_EXYNOS_DECON_7580_KMS) && !decon_kms_attach(decon))
		goto display_registered;	/* KMS owns the pipe, no fb0 */
	if (IS_ENABLED(CONFIG_EXYNOS_DECON_7580_KMS))
		decon_warn("KMS attach failed, falling back to fbdev\n");
	ret = register_framebuffer(decon->windows[decon->pdata->default_win]->fbinfo);
	if (ret) {
		decon_err("failed to register framebuffer\n");
		goto fail_iovmm;		/* same unwind as before, :4784 */
	}
display_registered:
	decon_info("decon registered successfully\n");
	return 0;
```

`decon_kms_attach()` is in the shim: it builds `struct decon_kms_dev`, creates
and registers the DRM device, registers the CRTC/plane/encoder/connector and
returns 0, or returns an error having left no DRM state behind. The
`decon_device` is handed over **by pointer, in the same process, inside the
same probe** — no lookup, no global, no ordering problem, and the panel is
already lit (`decon->state == DECON_STATE_INIT`, `:4740`) when the KMS device
appears.

Companion edits this forces:
- `decon_remove()` must not call `unregister_framebuffer()` unconditionally
  (`:4829`); guard it with the same `CONFIG_EXYNOS_DECON_7580_KMS`/registered
  condition, otherwise remove hits an unregistered `fb_info`.
- `decon_shutdown()` (`:4844`) works on `decon->state` only, so it needs no
  change — but see 7.6 before reusing it.

Failure modes:
- Both modes can never coexist → a compositor and X11 cannot share the pipe.
  Mitigated by `drm_fb_helper` (the fbdev-emulation slice) which reintroduces
  `fb0` on top of KMS, so userspace that only needs a framebuffer keeps working.
- Compile-time only: one kernel image cannot switch. A bad KMS image has no
  display until the fbdev kernel boots (rollback = boot the previous image;
  no DT/flash coupling).
- If `decon_kms_attach()` fails after DECON init, the fallback keeps `fb0`
  alive — but the DRM device may already be half-registered; the attach routine
  must be strictly all-or-nothing before any `drm_dev_register()`.
- The panel/DSIM ordering must not change: KMS enable still goes through
  `decon_enable()`, which streams DSIM itself.

### Option B — runtime mode via boot parameter (complements A)

`__setup("decon.kms=", ...)` plus a branch at the same `:4591`. Same code path
as A, one image, A/B testable without reflashing the kernel.
Failure modes: a typo silently takes fbdev (log it loudly); the parameter is
parsed before `late_initcall`, so it is available at probe; userspace may assume
`fb0` exists; and the choice is not visible in `/proc/config.gz`, so the
`doc`/`cmdline` must be trusted by the integrator.

### Option C — separate `platform_driver` + `device_find_child`/`of_find_*` hand-off

A DRM driver binds to nothing (or to a new, never-shipped compatible), then:

```c
np  = of_find_compatible_node(NULL, NULL, "samsung,exynos5-decon_driver");
pdev = of_find_device_by_node(np);
decon = platform_get_drvdata(pdev);      /* the fbdev driver's live context */
```

Failure modes (this is why it is not chosen):
- **Two owners of the same state.** `fb0` stays registered; the fb console,
  X11 or any `S3CFB_WIN_CONFIG` caller keeps flipping under the compositor, and
  `decon->state`, `decon->windows[]` and `update_regs_list` have no interlock
  that a third party can take.
- Ordering: the DRM side must run after `late_initcall(exynos_decon_register)`
  *and* after probe; `-EPROBE_DEFER` does not exist for "another driver's
  drvdata", so it degenerates into a retry/hack.
- Lifetime: `platform_get_drvdata` is not refcounted; unbind/`remove`
  (`:4819`) frees `decon` with `kfree()` while the DRM side may still hold the
  pointer.
- It works only if fbdev is quiesced (blank + no `fb0` consumers), which is
  exactly what Option A does properly.

### Option D — claim the compatible from DRM (rejected)

`patches/drm/0002-...` replaced `samsung,exynos5-decon_driver` and
`samsung,exynos5-mipi-dsi` in the board DTS and disabled the vendor Kconfig
symbols. Documented outcome (`patches/drm-kms-coexist/DESIGN.md` §1.2, §4): the
vendor drivers never probe, `fb0` is gone, no fallback, plus a rebuilt `dt.img`
is a brick risk. Do not repeat.

**Verdict**: Option A + B. The shim is compiled into `decon.o`, so the "binding"
is a function call from `decon_probe()`, the compatible strings in
`exynos7580.dtsi:1078` and `j7elte_rev00.dts:1013-1030` are untouched, no DTB is
rebuilt, and the fbdev driver remains the fallback in the same source tree.

---

## 7. Land mines the CRTC/plane slices must know

### 7.1 The ION-only buffer path panics on foreign dma-bufs

`ion_iovmm_map()` starts with `BUG_ON(dmabuf->ops != &dma_buf_ops)`
(`drivers/staging/android/ion/ion.c:2643`), and `ion_import_dma_buf()` rejects
foreign exporters earlier with `-EINVAL` (`ion.c:1582-1587`). A panfrost/GEM
buffer sent through `decon_set_win_buffer()` therefore either fails or **BUGs
the kernel**. The plane slice must use `decon_kms_map_dmabuf()`, which goes
through `iovmm_map(decon->dev, sg, ...)` — the same call `ion_iovmm_map()`
makes internally (`ion.c:2604`) — or the contiguous-phys fallback when the
device has no IOMMU domain (`iovmm_map` returns `-ENOSYS`, handled at
`ion.c:2607-2616`). For M2 either allocate scanout buffers from ION
(`EXYNOS_ION_HEAP_SYSTEM_MASK`, as `decon_fb_alloc_memory` does) or take this
path; the latter is required for panfrost zero-copy.

### 7.2 Fences are Android `sync_fence`, not `dma_fence`

`decon_win_config.fence_fd` is imported with `sync_fence_fdget()` (`:1632`)
and waited with `sync_fence_wait(fence, 900)` (`:1980-1990`). The DECON
completion fence handed back to userspace is `sw_sync` (`:2342-2353`). The
atomic core (slice 1) speaks `dma_fence`; the CRTC must wait its own in-fences
before `decon_kms_commit()` and signal its own out-fence from the vblank
callback (slice 15), not from the vendor fence.

### 7.3 Single instance, global context

`struct decon_device *decon_int_drvdata` (`decon_core.c:65`, declared
`decon.h:36`) and `get_decon_drvdata(u32 id)` which **ignores `id`**
(`decon.h:729-732`). Every `decon_reg_*` call resolves the same device. A second
DECON instance (there is only `decon_0` in DT) would silently alias.

### 7.4 Commit during OFF reports success

`decon_set_win_config()` builds a pre-signalled fence and does nothing when the
state is `DECON_STATE_OFF`, `ignore_vsync`, or TUI (`:2218-2228`). A CRTC that
commits before `decon_kms_enable()` gets `0` and a frame that never happened.
The contract requires enable-first; the shim's `decon_kms_commit()` returns
`-EAGAIN` in that case instead of silently succeeding.

### 7.5 `wait_event_interruptible` in the commit path

`decon_wait_for_vsync()` returns 0 on a signal (only `ret == 0` is treated as
timeout, `:1258-1261`). So a `-ERESTARTSYS` from the commit thread looks like a
successful vsync and the frame is considered latched. Acceptable for the fbdev
magic-ioctl ABI; the KMS shim should use `wait_event_timeout` in its own path
(two lines, in `decon_kms_api.c`) rather than change vendor behaviour.

### 7.6 `decon->irq` is 0 in video mode, and `decon_shutdown()` disables it

`decon_fb_config_eint_for_te()` returns early unless `psr_mode ==
DECON_MIPI_COMMAND_MODE` (`decon-int_drv.c:808-814`), so on J7 `decon->irq`
stays 0 (devm_kzalloc). `decon_disable()` guards with `eint_en_status`
(`:1052-1056`), but `decon_shutdown()` calls `disable_irq(decon->irq)`
unconditionally (`:4859`) — i.e. `disable_irq(0)` on poweroff/reboot. Pre-existing;
relevant here because a KMS takeover that reuses `decon_shutdown` inherits it.
Do not "fix" it in this slice's files; note it for the integration seat.

### 7.7 Panel/LPD/ESD ownership stays outside this contract

`decon->lcd_info` comes from the DSIM subdev's hostdata
(`decon-int_drv.c:220`); `run_list()` panel power/reset is DSIM's
(`dsim_drv.c:839-853`); ESD IRQs/workqueue are registered in `decon_probe`
(`:4198`, `:4273`) and flushed by `decon_disable` (`:1026-1030`). Slice 4 (DSI)
and slice 5 (power) own those; this contract only guarantees that
`decon_kms_enable/disable` keep calling the vendor paths that keep them
consistent.

---

## 8. Verification this slice asks the integrator to run

Build-host, after the shim patch lands (slice 16 assembles):

1. `make drivers/video/exynos/decon_7580/` clean, no new warnings in
   `decon_core.c`, `decon-int_drv.c`, `decon_kms_api.c`.
2. `nm vmlinux | grep decon_export_` lists exactly the wrappers of §5.1.
3. `CONFIG_EXYNOS_DECON_7580_KMS=y` + `CONFIG_EXYNOS_DECON_7580_FBDEV` unset:
   `dmesg` still ends with `decon registered successfully`, no `fb0:` line
   appears, `/sys/class/graphics/fb0` is absent, and `decon_kms_attach()` ran
   (its own dev_info).
4. With `FBDEV` selected: byte-identical behaviour to today — `fb0` present,
   `dmesg` shows the same probe sequence, X11 works. This is the rollback gate.

Note: none of these checks needs the phone to be idle or a compositor to be
running; they are `dmesg`/`/dev/fb0`/`nm` checks for the first int50 boot.

---

## 9. Open decisions for the integration seat

1. **Layer A vs direct layer-B calls in the CRTC.** Recommendation: direct
   wrapper calls for M1 (the shim is linked into `decon.o`; an ops struct only
   adds an indirection and a NULL-check). Keep the ops struct in the header
   because it is also the sane shape if the subdriver ever moves out of
   `decon_7580/`.
2. **Keep the scratch `fb_info` per window** (recommended) instead of
   refactoring `struct decon_win`. It costs one `framebuffer_alloc` per window
   and removes the risk of rewriting `decon_set_win_buffer()`'s fb bookkeeping
   that the fbdev fallback still needs.
3. **Who owns the vblank counter.** Recommendation: the DECON shim (it has the
   IRQ) calls `drm_crtc_handle_vblank()`, so slice 15 does not need its own
   interrupt registration.
