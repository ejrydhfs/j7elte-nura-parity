# DECON KMS M1 — DSI encoder + connector (`exynos_drm7580_dsi.c`)

Slice: **DsiEncoder** (plan `docs/reports/decon-kms-plan.md` §6 item 12;
requirements `docs/reports/decon-kms-dsi.md` §2).
Status: **implemented, build-verified**; not booted (M1 brings up the device).
Kernel citations are `file:line` in the read-only vendor tree
`android_kernel_samsung_universal7580-2a746c1…` (3.10.108, int49), with the M0
atomic series (`patches/drm-atomic/0001..0004`) applied on top.

Deliverables

| Item | Path |
|---|---|
| Patch (applies on pristine + M0) | `patches/drm-kms/0001-dsi-encoder-connector.patch` |
| Plain source | `code/gpu31/kms/exynos_drm7580_dsi.c` |
| Contract implemented | `code/gpu31/kms/exynos_drm7580.h` (unmodified) |

The patch adds **only** the one new source file. Adding
`exynos_drm7580_dsi.o` to `drivers/gpu/drm/exynos/Makefile` is integration
(KmsAssemble) work; a per-slice Makefile hunk would collide with every sibling
patch.

## 1. What this file is, and what it deliberately is not

`exynos7580_dsi_init()` and `exynos7580_dsi_get_modes()` are the only exported
symbols (verified with `nm`: `T exynos7580_dsi_get_modes`,
`T exynos7580_dsi_init`; everything else is `static`, no cross-slice globals).
The object's only undefined cross-slice symbol is
`exynos7580_crtc_disable_link()` — a header-declared function, not a new
interface.

The file is DRM glue only, per `decon-kms-dsi.md` §2.1/§2.6:

* it does **not** bind `samsung,exynos5-mipi-dsi`, does not ioremap
  `0x14800000`, does not touch DSIM registers, clocks, regulators or
  interrupts (so `dsim_enable_interrupts()`'s refcount stays consistent,
  `dsim_drv.c:86-131`);
* the DSI host (`dsim_drv.c`), the ea8061 panel, the board rails/reset list
  and the `/sys/class/backlight/panel` contract all stay with the vendor
  drivers;
* it never parses the panel DT node: the timings come from the DSIM subdev
  (`DSIM_IOC_GET_LCD_INFO` → `v4l2_get_subdev_hostdata()`,
  `dsim_drv.c:1118-1120`), the same lookup the vendor DECON driver does
  (`decon-int_drv.c:203-227`);
* no `s_stream()` call of its own: the link is enabled by the CRTC
  (§3.2/§3.3). `->disable` only delegates to the CRTC-owned teardown;
* built against the final contract (`8048d08`): `ex->drm` is a
  `struct drm_device *` owned by `drm_platform_init()`, and `to_exynos7580()`
  resolves through `drm->dev_private`, so nothing here does `container_of()`
  on a DRM device. The file lives in `drivers/gpu/drm/exynos7580/`.

Single instance: the contract header has no private DSI storage in
`struct exynos7580_device`, and the SoC has one DSI output
(`exynos5_dism_phy_enable(0, …)`, `dsim_drv.c:688-692`). The state
(`dsim_sd`, `lcd`, `mode`, `pixclk`, `ready`, back-pointer to the instance)
lives in one file-static `struct exynos7580_dsi`; it is written once in
`exynos7580_dsi_init()` before the card is usable and read-only afterwards, so
no locking is needed.

## 2. Init sequence and the mode

`exynos7580_dsi_init(ex)`:

1. reject `!ex || !ex->dev` (`-EINVAL`) and an unregistered CRTC
   (`ex->crtc.base.id == 0` → `-EINVAL`): `possible_crtcs` must be valid;
2. `md = module_name_to_driver_data("exynos-mdev")` (`exynos_mc.h:128-140`);
   NULL → `-EPROBE_DEFER` (`MDEV_MODULE_NAME`, exynos_mc.h:28);
3. `sd = md->dsim_sd[0]`; `IS_ERR_OR_NULL` → `-EPROBE_DEFER` — the DSIM
   `late_initcall` has not registered into the media device yet;
4. `v4l2_subdev_call(sd, core, ioctl, DSIM_IOC_GET_LCD_INFO, NULL)` then
   `v4l2_get_subdev_hostdata(sd)` → `struct decon_lcd *`; copied, not shared;
5. sanity: video mode (`psr_mode == DECON_VIDEO_MODE` — the J7 DT has `<0>`,
   `exynos7580-j7elte_rev00.dts:1016`), non-zero `xres/yres`, non-zero
   `hsa/vsa` (the DSIM porch registers store `len - 1`; zero would program
   garbage); anything else aborts the bind with a `dev_err`;
6. pixel clock from the DT property `disp-pll-clk` on the DECON node
   (`…rev00.dts:1018`, 64 500 000 Hz), defaulting to the same value if the
   property is missing. This is the real vclk: `decon_int_set_clocks()` sets
   `disp_pll` to `pdata->disp_pll_clk` and the CRTC has a /1 divider into
   `d_clk_decon0_vclk` (`decon-int_drv.c:179-191`). The fbdev `pixclock`
   (1e12/(720·1280·60) ≈ 55.3 MHz, `decon_core.c:2821-2834`) is **not** used;
7. build the mode (§2.1), `drm_encoder_init(…, DRM_MODE_ENCODER_DSI)` — 6,
   the value M0 added to the uapi — `possible_crtcs = drm_crtc_mask(&ex->crtc)`,
   `drm_encoder_helper_add()`;
8. `drm_connector_init(…, DRM_MODE_CONNECTOR_DSI)` — 16, M0 uapi — then
   `display_info.width_mm/height_mm` from the panel's `size` (68×121 mm),
   `interlace_allowed = false`, `doublescan_allowed = false`,
   `polled = 0` (fixed panel), `status = connected`,
   `drm_connector_helper_add()`, `drm_mode_connector_attach_encoder()`;
9. publish `dsi0.ready` and print the three bind lines of §4.

Error paths return the `-errno`; every object already registered is reclaimed
by `drm_mode_config_cleanup()` in the caller (main slice), so no partial
cleanup is duplicated here.

### 2.1 The one mode

Sources: `ea8061_wxga` panel node (`…rev00.dts:1048-1061`) as parsed by
`dsim_parse_lcd_info()` (`dsim_drv.c:1286-1340`), and `disp-pll-clk`.

| Field | Value | From |
|---|---|---|
| `hdisplay` / `vdisplay` | 720 / 1280 | `resolution` |
| `hsync_start` | 776 | 720 + `hfp` 56 |
| `hsync_end` | 780 | + `hsa` 4 |
| `htotal` | 828 | + `hbp` 48 |
| `vsync_start` | 1293 | 1280 + `vfp` 13 |
| `vsync_end` | 1295 | + `vsa` 2 |
| `vtotal` | 1300 | + `vbp` 5 |
| `clock` | 64 500 kHz | `disp-pll-clk` / 1000 |
| flags | 0 | video-mode DSI is defined by porches; DSIM does not carry sync polarity |
| `type` | `DRIVER \| PREFERRED` | fixed panel |
| `name` | `720x1280` | `drm_mode_set_name()` |
| `vrefresh` | 60 | `drm_mode_vrefresh()`; the exact rate is 64 500 000/(828·1300) = **59.92 Hz** (the DSI report §1.8 writes 59.93; 59.9222 rounds to 59.92 — no other number differs) |
| mm size | 68 × 121 | `size` → `display_info` |

The same values are what DSIM programs (`dsim_reg_set_porch()`,
`dsim_reg_7580.c:363-392`) and what DECON programs
(`decon_reg_set_porch()`, `decon_reg_7580.c:131-150`), so the three stages
agree by construction. The DSI rate has margin: 4 lanes × 481 Mbps ≈
80.2 Mpix/s vs 64.58 Mpix/s required.

## 3. Modeset ordering (the part that matters on hardware)

### 3.1 Link ON — CRTC, not this file

`decon-kms-dsi.md` §2.3: `s_stream(1)` runs **before** `decon_reg_init()` and
`decon_reg_start()` (vendor order `decon_core.c:942 → 958 → 970`), and the only
fallible callback in the 3.10 modeset is `crtc_funcs->mode_set`
(`drm_crtc_helper.c:457-459`). Therefore:

| Step | Owner | Call |
|---|---|---|
| 1 | DeconCrtc `mode_set` (first) | `pm_runtime_get_sync(decon)`, then `v4l2_subdev_call(sd, video, s_stream, 1)` → rails, D-PHY, PLL/byte/esc clocks, lanes, panel reset, HS clock, panel `displayon` (`dsim_drv.c:860-909`) |
| 2 | DeconCrtc `mode_set` | `decon_reg_init()` + window registers |
| 3 | DeconCrtc `commit` | `decon_reg_start()` — first frame scanned out |

This file's encoder has **no** `prepare`/`mode_set`/`commit` hooks on purpose:
nothing in them could fail, and `dsim_enable()` sleeps (msleep in
`ea8061_exit()`), so it must never run from a `void` callback or from
`atomic_check`.

`mode_fixup` validates the requested mode (`drm_mode_equal()` against the
panel mode) and copies the canonical mode into `adjusted_mode`, so a
userspace variant of "the same" mode cannot reach DSIM/DECON.

### 3.2 Link OFF — this file, by delegation

3.10 disables unused encoders **before** `crtc->disable`
(`drm_crtc_helper.c:288-294` vs `:296-306`). That matches the vendor teardown
`decon_disable()`: `s_stream(0)` at `decon_core.c:1059-1063`, then
`decon_reg_stop()` at `:1072`. `exynos7580_dsi_encoder_disable()` therefore
calls `exynos7580_crtc_disable_link(ex)` and nothing else; the CRTC keeps the
link state (`link_on`) and does `v4l2_subdev_call(sd, video, s_stream, 0)` →
full-screen DCS, `ea8061_exit()` with its 35 ms + 150 ms delays, interrupts
off, HS clock off, lanes LP-00, DSIM clocks off, SW reset, D-PHY off, rails
off (`dsim_drv.c:911-951`). `s_stream(0)`/`s_stream(1)` are idempotent
(`dsim_drv.c:862-863, 913-914`), so a second call from the CRTC's own
`disable` is harmless, and no second runtime-PM reference is taken
(`dsim_enable` gets at `:868`, `dsim_disable` puts at `:946`).

### 3.3 Vblank, panel commands, suspend

Not this slice: vblank is DECON's frame IRQ (`decon-int_drv.c:76-127`), the TE
pin is command-mode only, and panel command writes require `HSCLKEN`
(`dsim_drv.c:239-244`) — never issued from here because this file issues no
panel command at all.

## 4. Exact dmesg at bind

Device name prefix is the DECON platform device (`decon_fb@14830000`), i.e.
`14830000.decon_fb`. Timestamps illustrative; the text is literal.

```
14830000.decon_fb: exynos7580-dsi: DSIM subdev exynos-mipi-dsi0-subdev, hs 481 Mbps/lane, esc 16 MHz
14830000.decon_fb: exynos7580-dsi: panel 720x1280 (68x121 mm), hbp/hfp/hsw 48/56/4, vbp/vfp/vsw 5/13/2
14830000.decon_fb: exynos7580-dsi: mode 720x1280 @ 59.92 Hz (64500 kHz, htotal 828, vtotal 1300), encoder DSI(6) possible_crtcs 0x1, connector DSI(16) connected
```

Failure/defer paths (M1 debugging):

```
14830000.decon_fb: exynos7580-dsi: exynos-mdev is not probed yet            (return -EPROBE_DEFER)
14830000.decon_fb: exynos7580-dsi: DSI0 subdev is not registered yet        (return -EPROBE_DEFER)
14830000.decon_fb: exynos7580-dsi: DSIM_IOC_GET_LCD_INFO failed (-N)
14830000.decon_fb: exynos7580-dsi: DSIM subdev has no lcd info
14830000.decon_fb: exynos7580-dsi: panel is not in DSI video mode (N), DECON KMS needs video mode
14830000.decon_fb: exynos7580-dsi: panel has no resolution (0x0)
14830000.decon_fb: exynos7580-dsi: panel sync lengths are 0 (hsa N vsa N)
14830000.decon_fb: exynos7580-dsi: CRTC must be registered first
```

`possible_crtcs 0x1` assumes the DECON CRTC is the first registered CRTC of
the device; with one CRTC (`drm_crtc_index()` 0) that is the value to expect.

Energy/backlight stay untouched: `cat /sys/class/backlight/panel/max_brightness`
must still be 355.

## 5. Connector behaviour (KWin 6.8 requirements)

| Requirement (`decon-kms-req.md`) | Implementation |
|---|---|
| #17 connected connector with ≥1 mode | `detect()` → `connector_status_connected` (soldered panel; the vendor panel driver already ran the ID read at probe, `ea8061_lcd_ctrl.c:769-840`), `polled = 0`, `get_modes()` → exactly 1 |
| "Could not find a suitable primary plane" / possible CRTCs | `encoder->possible_crtcs = drm_crtc_mask(&ex->crtc)`; `drm_mode_connector_attach_encoder()` makes `connector->encoders` non-empty for `drmModeConnectorGetPossibleCrtcs()` |
| #10/#11 `ACTIVE`/`MODE_ID`, `CRTC_ID` | M0's `drm_connector_init()` attaches `CRTC_ID` for `DRIVER_ATOMIC` devices; state via `drm_atomic_helper_connector_reset/duplicate/destroy` (3.10's core does not allocate connector state, `drm_mode_config_reset()` only calls `funcs->reset`) |
| #16 no EDID needed | none generated (EDID for a soldered panel is not a requirement; KWin falls back to the connector name) |
| no cursor / no extra modes | nothing added: fallback modes from `drm_add_modes_noedid()` are rejected by `mode_valid()` and pruned |

Note on M0: `drm_atomic_helper_connector_set_property()` in the backport keeps
the 3.19 signature `(connector, property, val)` and does not fit the 4.0
4-argument hook, so no `->atomic_set_property` is installed. Nothing is lost:
`CRTC_ID` is handled by the core (`drm_atomic.c:630-632`) and the connector has
no driver-private properties. (Reported to Main; the same mismatch exists for
the plane hook and affects `DeconPlane`.)

## 6. Build evidence

Tree: private copy `/run/media/…/j7k/kms/DsiEncoder` (`cp -a` of the read-only
vendor tree), M0 series 0001–0004 applied, contract header copied from
`code/gpu31/kms/` at `8048d08` (`struct drm_device *drm`, `to_exynos7580()`
via `dev_private`). The file sits in `drivers/gpu/drm/exynos7580/`. The
copy-only wiring is `obj-y += exynos7580/` in `drivers/gpu/drm/Makefile` plus
`obj-y += exynos_drm7580_dsi.o exynos_drm7580_stub.o` in
`drivers/gpu/drm/exynos7580/Makefile`, and a scratch `exynos_drm7580_stub.c`
providing the not-yet-written `exynos7580_crtc_disable_link()` — none of that
is in the patch.

```
$ make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS=-Wno-error -j8 drivers/gpu/drm/
  CC      drivers/gpu/drm/exynos7580/exynos_drm7580_dsi.o
  CC      drivers/gpu/drm/exynos7580/exynos_drm7580_stub.o
  LD      drivers/gpu/drm/exynos7580/built-in.o
  LD      drivers/gpu/drm/exynos/built-in.o
  LD      drivers/gpu/drm/built-in.o

$ make … drivers/video/exynos/decon_7580/
  LD      drivers/video/exynos/decon_7580/dsim.o
  LD      drivers/video/exynos/decon_7580/decon.o
  LD      drivers/video/exynos/decon_7580/built-in.o
```

* exit status 0, **0 errors**;
* **0 warnings in `exynos_drm7580_dsi.c`** with the production flags. The only
  warning lines in the build are the pre-existing vendor ones
  (`exynos_drm_fb.h` header guards, `exynos_drm_dmabuf.c:289`,
  `exynos_drm_g2d.c:1156,1174`, `exynos_drm_gem.c:211,476,483`) that M0 already
  documented as pre-existing. With `-Wextra` the only new note is the unused
  `force` argument of `detect()` (kernel convention, same as upstream
  `exynos_dsi.c`);
* `git apply --check` of `0001-dsi-encoder-connector.patch` against the
  pristine + M0 tree (`km0-pristine`): clean;
* `nm` on the object: two global symbols (`exynos7580_dsi_init`,
  `exynos7580_dsi_get_modes`), no globals beyond that, one cross-slice
  undefined symbol (`exynos7580_crtc_disable_link`);
* legacy exynos DRM (`drm_plane_init`/`drm_crtc_init` users) untouched: the
  patch touches no existing file.

## 7. What M1 should verify on the device (not run by this slice)

```sh
# 1. bind: three lines of §4 present, no -EPROBE_DEFER loop, fb0 still there
dmesg | grep -E "exynos7580-dsi|mipi-dsi driver|READ ID|can't find connected panel"
# 2. enumeration: DSI-1 connected, exactly one mode, 720x1280, clock 64500
modetest -M exynos -c
# 3. only the panel mode is accepted
modetest -M exynos -a -s <conn_id>:720x1280
# 4. atomic properties: connector CRTC_ID present, plane/CRTC props from M0
modetest -M exynos -p
# 5. first commit order in dmesg: DSIM up (no "not ready") before DECON start
#    (DECON side is DeconCrtc's evidence); blank/unblank must show the
#    ea8061 35 ms + 150 ms panel-off delays
# 6. link teardown: after a modeset-off the panel is dark, then lights again
```

Regression control: with KMS not bound (vendor fbdev path), nothing in this
patch executes — it only compiles.

## 8. Residual risks

| Risk | Mitigation / status |
|---|---|
| `exynos7580_dsi_init()` runs before the DSIM `late_initcall` registers the subdev | `-EPROBE_DEFER` + warn; the main slice must retry (or fall back to fbdev) — documented, not solved here |
| DSIM subdev pointer captured once at init | DSIM never re-registers (no unbind path in the vendor driver); `dsi0.dsim_sd` is informational, the CRTC does the `s_stream` calls |
| Timing drift if the board DT changes | every number is read from the subdev/DT; nothing is hard-coded except the `disp-pll-clk` fallback |
| `vrefresh` field is 60 while the exact rate is 59.92 Hz | the field is an integer Hz (3.10 rounds); the clock + porches are the authoritative values and are what userspace recomputes. KWin's timing maths tolerates it |
| Encoder `disable` delegation depends on `exynos7580_crtc_disable_link()` existing | header-declared, owned by DeconCrtc; stubbed only in the build copy. Coordinates with the CRTC slice; `s_stream(0)` is idempotent if the CRTC calls it again |
| No suspend/resume here | by design: system suspend is the CRTC/driver slice's (DispPower), the encoder adds no state to save |

## 9. Integration dependencies (what the assembled M1 must provide)

| Dependency | Owner | Why |
|---|---|---|
| wire `drivers/gpu/drm/exynos7580/exynos_drm7580_dsi.o` into the build (`obj-y += exynos7580/` + the per-dir Makefile) | KmsAssemble | the patch ships no Makefile hunk (would collide with every sibling patch) |
| `exynos7580_crtc_init()` returns before `exynos7580_dsi_init()` | KmsBoot/main | `possible_crtcs = drm_crtc_mask()` needs a registered CRTC (`ex->crtc.base.id != 0`) |
| `drm_dev->driver->driver_features` includes `DRIVER_MODESET \| DRIVER_ATOMIC`, `mode_config.funcs.atomic_commit = drm_atomic_helper_commit` | KmsBoot/main | `drm_atomic_helper_connector_dpms` (the connector `->dpms`) commits through it; and atomic is what KWin needs |
| `exynos7580_crtc_enable_link()`/`_disable_link()` behave as §3.1/§3.2 (idempotent, `s_stream` + `DSIM_IOC_SET_PORCH` if timings ever differ) | DeconCrtc | the encoder never starts/stops the link itself |
| `CONFIG_EXYNOS_MIPI_DSI=y`, `CONFIG_PANEL_EA8061_DYNAMIC=y`, `CONFIG_FB_WINDOW_UPDATE=y`, mdev driver probed before the DECON bind | KmsAssemble | no subdev / no `lcd_info` / no `DSIM_IOC_SET_PORCH` otherwise (`decon-kms-dsi.md` §4) |
| `disp-pll-clk` present in the board DT (64 500 000) | stock DT | the pixel clock of the single mode; fallback constant equals it |
