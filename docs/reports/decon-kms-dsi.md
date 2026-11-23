# DECON KMS — MIPI-DSI encoder/connector requirements (ea8061, j7elte)

Slice: **DsiPanel** (plan `docs/reports/decon-kms-plan.md` §6 item 4).
Kernel citations are `path:line` in
`android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07`
(read-only). Board/panel facts come from the DT and the in-tree vendor drivers;
nothing here was executed on the device.

**Bottom line.** The DSI link and the panel already have a complete, proven
vendor implementation in `decon_7580/` (`dsim_drv.c` + `dsim_reg_7580.c` +
`panels/ea8061_lcd_ctrl.c`). A KMS encoder should **not** re-implement them: it
should consume the existing DSIM v4l2 subdev (`s_stream`, `DSIM_IOC_*`) exactly
the way the vendor DECON driver does, and only add DRM-native plumbing (encoder,
connector, modes, vblank, atomic enable/disable ordering). The DECON fbdev
driver must give up `decon_fb@14830000`, but `dsim@14800000` can stay with the
vendor driver — that is the cheapest, lowest-risk split and it keeps the panel
power/reset/backlight code in its only tested form.

---

## 1. Inventory of the vendor DSI + panel path

### 1.1 Device tree (the contract the encoder must honour)

SoC nodes, `arch/arm64/boot/dts/exynos7580.dtsi`:

| Node | Facts | Cite |
|---|---|---|
| `decon_fb@14830000` | compatible `samsung,exynos5-decon_driver`; reg 0x14830000/0x8000; `samsung,power-domain = <&pd_disp>`; 3 IRQs (fifo 203, cmd 201, frame_done 202) | `:1078-1085` |
| `dsim@14800000` | compatible `samsung,exynos5-mipi-dsi`; reg 0x14800000/0x1000; IRQ 207; `pd_disp`; `status="disabled"` in the dtsi | `:1169-1175` |
| dsim clocks | `pclk_dsim0, mipi0_rx, mipi0_bit, rgb_vclk0, pclk_disp` | `:1177-1187` |
| dsim pinctrl | `turnon_tes`/`turnoff_tes` states exist | `:1189-1191` |
| dsim lanes | `data_lane_cnt = <4>` | `:1194` |
| `mdev_output` | `samsung,exynos5-mdev` — the v4l2/media container DECON and DSIM register into | `:1165-1167` |

Board nodes, `arch/arm64/boot/dts/exynos7580-j7elte_rev00.dts` (rev04/rev06
differ only in `decon_board_list` power sequencing):

| Node | Facts | Cite |
|---|---|---|
| `decon_fb` | `psr_mode = <0>` (video mode), `trig_mode = <1>` (sw), `disp-pll-clk = <64500000>`, `gpio_err = <&gpa3 7 0x1>`, `status="okay"` | `:1013-1021` |
| `dsim_0` | `lcd_info = <&ea8061>`, `decon_board = <&decon_board_list>`, `status="okay"` | `:1022-1030` |
| `decon_board_list` | power-on: enable `LCD_1P8`, usleep 10–11 ms, enable `LCD_3P0`; power-off: disable `LCD_3P0`, delay, disable `LCD_1P8`; reset: `gpd1 3` high→10 ms→low→10 ms→high→10 ms | `:1031-1047` |
| `ea8061_wxga` | 720×1280, 68×121 mm, refresh 60, h-porch **48/56/4** (hbp/hfp/hsw), v-porch **5/13/2** (vbp/vfp/vsw), `dsi-hs-clk = <481>` (Mbps/lane), `dsi-escape-clk = <16>` (MHz), `mic = <0>`, `rev = <4>` | `:1048-1061` |

`psr_mode = 0` = `DECON_VIDEO_MODE` (`panels/decon_lcd.h:12-16`), i.e. DECON
streams RGB continuously into DSIM, which transmits it as MIPI-DSI **video
mode** in burst mode. There is no command-mode framebuffer update path on this
board.

### 1.2 Who owns what today (probe order)

1. `dsim` platform driver: `of_match` `samsung,exynos5-mipi-dsi`
   (`dsim_drv.c:1571-1574`), `late_initcall(dsim_init)` (`:1637-1644`).
2. `decon` platform driver: `of_match` `samsung,exynos5-decon_driver`
   (`decon_core.c:55-56`), `late_initcall(exynos_decon_register)`
   (`:4890`). Link order puts `dsim.o` before `decon.o`
   (`decon_7580/Makefile:9-11`), so DSIM probes first.
3. `dsim_probe()` (`dsim_drv.c:1342`): parses `lcd_info` (`dsim_parse_lcd_info`,
   `:1286-1340`), sets `panel_ops` from `panels/dsim_panel.c:18-38`, maps regs,
   registers the v4l2 subdev into `mdev` (`dsim_register_entity`, `:1199-1241`),
   then either takes over the bootloader's live link or starts it itself
   (`:1458-1527`), sets `state = HSCLKEN` and calls the panel `probe` (`:1526-1528`).
4. Panel `probe` (`panels/ea8061_lcd_ctrl.c:1544-1584`) registers
   `/sys/class/lcd/panel` (`:1556`) and `/sys/class/backlight/panel`
   (`:1563`), then `ea8061_probe` (`:1123`) reads the panel ID/MTP over DSI
   (`ea8061_read_init_info`, `:769-840`).
5. `decon_probe` (`decon_core.c:4396+`): registers its own subdev
   (`:4532`), which calls `find_subdev_mipi` (`decon_core.c:2730` →
   `decon-int_drv.c:203-227`) to grab `md->dsim_sd[0]` from the mdev and then
   `DSIM_IOC_GET_LCD_INFO` to copy `dsim->lcd_info` into `decon->lcd_info`
   (`decon-int_drv.c:219-220`).
   Only after that does it build `fb_var_screeninfo` from those timings
   (`decon_core.c:2864-2890`), register fb0 (`:4591`), and later start the pipe
   (`decon_reg_init` `:4684`, `decon_reg_start` `:4703`) and call the panel's
   `displayon` (`:4713-4714`).

Two consequences for the KMS slice:

* **The DSIM subdev pre-exists.** Any DRM glue can reach it with
  `module_name_to_driver_data("exynos-mdev")` + `md->dsim_sd[0]`
  (`include/media/exynos_mc.h:128-140,153-166`), exactly like DECON does. No
  new exports, no DT changes, no contest for `samsung,exynos5-mipi-dsi`.
* Panel power/reset/link bring-up happen **before** any DRM object exists, and
  are already idempotent (see §1.3/§1.4), so the encoder does not need to
  duplicate them at probe.

### 1.3 DSI host bring-up sequence (what `s_stream(1)` does for you)

`dsim_s_stream()` (`dsim_drv.c:1101-1109`) → `dsim_enable()`
(`:860-909`). Exact order:

| # | Action | Cite |
|---|---|---|
| 1 | `pm_runtime_get_sync(dsim->dev)` — enables `pclk_dsim0`, `rgb_vclk0`, `pclk_disp` | `:868`, `:1604-1618` |
| 2 | panel rails on via board list `dsim_set_panel_power_enable` (`LCD_1P8`→10 ms→`LCD_3P0`) | `:873`, `:846-858`, `decon_board.c:449-460` |
| 3 | panel `resume` op (NULL for ea8061; only `probe/displayon/suspend` exist) | `:875`, `ea8061_lcd_ctrl.c:1635-1639` |
| 4 | D-PHY block enable (`exynos5_dism_phy_enable(0,1)`, id hard-coded 0) | `:878`, `:688-692` |
| 5 | `dsim_reg_init()`: function reset, counters, shadow, **porch**, **video-vs-command config**, clock-continuous mode | `:880`, `dsim_reg_7580.c:739-758` |
| 6 | DSIM PLL/byte/escape clocks on (PMS search, D-PHY timings, PLL enable) | `:882-883`, `dsim_reg_7580.c:918-946` |
| 7 | `clk_prepare_enable(dphy_esc)`, `clk_prepare_enable(dphy_byte)` | `:885-886` |
| 8 | `dsim_reg_set_lanes(CLK+4 data, 1)` and wait for lane stop-state | `:888`, `dsim_reg_7580.c:948-961` |
| 9 | board list `dsim_reset_panel` (GPIO pulse) | `:890`, `:835-844` |
| 10 | `dsim_reg_set_hs_clock(1)`: standby set, function reset, HS clock request, wait ready | `:896`, `dsim_reg_7580.c:971-999` |
| 11 | `dsim_reg_set_int(1)` (unmask), `state = HSCLKEN` | `:899-901` |
| 12 | panel `displayon`: if panel state is SUSPENDED run `ea8061_init`, then brightness, then `ea8061_displayon` (stub) | `:903`, `ea8061_lcd_ctrl.c:1586-1611` |

`dsim_enable()` early-returns when `state == HSCLKEN` (`:862-863`), and
`dsim_disable()` early-returns when `state == DSIM_STATE_SUSPEND` (`:913-914`),
so `s_stream()` is safe to call repeatedly. Important corner case: on a normal
cold boot the **bootloader has already left the link in HS**; `dsim_probe()`
detects that (`dsim_reg_is_hs_clk_ready`, `:1458`), skips `dsim_reg_init` in
video mode, sets `state = HSCLKEN` anyway (`:1526`), and the panel keeps its
bootloader state. The first KMS `s_stream(1)` is then a no-op — that is exactly
what the current fbdev stack relies on, and the DECON output simply takes over
scan-out. Do not "fix" it.

### 1.4 Teardown (`s_stream(0)`)

`dsim_disable()` (`dsim_drv.c:911-951`), order: full-screen partial-area DCS
(`:917`) → panel `suspend` = `ea8061_exit()`: DISPLAY_OFF, msleep(35), SLEEP_IN,
msleep(150) (`:921`, `ea8061_lcd_ctrl.c:856-879`) → interrupts off → HS clock
off (`:933`) → lanes to LP-00 (`:936`) → DSIM clocks off (`:938`) → SW reset
(`:940`) → D-PHY off (`:942`) → rails off (`:944`) → `pm_runtime_put_sync`
(`:946`).

The vendor DECON calls this **before** `decon_reg_stop()`
(`decon_core.c:1059-1072`); mirror that order (§2.3).

### 1.5 Lane and clock programming

* Lanes: 4 data + clock, from `data_lane_cnt` (`dsim_drv.c:1413-1438`,
  dtsi:1194).
* `timing,dsi-hs-clk = <481>` is Mbps per lane. `dsim_reg_calculate_pms()`
  (`dsim_reg_7580.c:620-668`) searches P/M/S (fin = 26 MHz, VCO 1250–2500 MHz,
  fout 40–2500 MHz) for the lowest attainable output ≥ the request, then
  `byte_clk = hs/8` and the escape divider is chosen so the escape clock is
  ≤ 16 MHz (`:824-843`). `dphy_timing[]` (`:34-143`) is indexed by the result,
  `b_dphyctl[]` (`:145-148`) by escape clock 7–20 MHz
  (`dsim_reg_get_dphy_timing`, `:671-711`).
* Sanity: 4 lanes × ~481 Mbps / 24 bpp ≈ 80.2 Mpix/s ≥ the 64.58 Mpix/s the
  porches demand (§1.8), so burst video mode closes with margin.
* `DSIM_CONFIG` for video mode = `VIDEO_MODE | BURST_MODE | EOT_R03_DISABLE |
  HFP_DISABLE | NUM_OF_DATA_LANE(3) | PIXEL_FORMAT(RGB24)`;
  `CLKLANE_STOP_START` stays clear and the non-continuous-clock bit is cleared
  explicitly (`dsim_reg_7580.c:394-421`, `:751-757`) — the HS clock runs
  continuously in video mode.
* Porches: `MVPORCH` (VBP + stable-VFP + cmd-allow), `MHPORCH` (HFP/HBP),
  `MSYNC` (VSA/HSA), `MDRESOL` (720×1280 + shadow) in
  `dsim_reg_set_porch()` (`:363-392`).
* DECON side: same numbers via `decon_reg_set_porch()` →
  `VIDTCON0..4` (`decon_reg_7580.c:131-150`), pixel clock from `disp_pll`
  divided by 1 (`decon-int_drv.c:181,189-191`).

### 1.6 Command writes / reads

* `dsim_write_data(dsim, data_id, data0, data1)` (`dsim_drv.c:221-380`) is the
  only command path: short packets for 1–2 byte DCS writes, long packets
  (`dsim_long_data_wr`, `:133-181`) for ≥3 bytes, with completion-based FIFO
  drains (`dsim_wait_for_cmd_fifo_empty`, `:183-219`) driven by the DSIM IRQ
  (`dsim_interrupt_handler`, `:694-730`). It refuses to run unless
  `state == HSCLKEN` (`:239-244`).
* `dsim_read_data()` (`:484-607`) implements DCS read: set max return size,
  send the read header, wait `RX_DAT_DONE`, drain RX FIFO; on error it forces
  the D-PHY stop state (`:590-596`).
* The panel driver wraps both with 4–5 retries
  (`ea8061_lcd_ctrl.c:63-113`) and uses them for ID/MTP/gamma/ELVSS/ACL
  (§1.7). Panel init is one long scripted sequence
  (`ea8061_param.h` + `ea8061_init`, `:881-1121`): test-key F0/FC, LTPS
  stop/timing/update, scan direction, AID default, slew control, GP/F3 EL_ON
  toggles, SLEEP_OUT, 120 ms, brightness, DISPLAY_ON, test-key off.
  Teardown is `ea8061_exit()` (`:856-879`).

Because the command path needs `HSCLKEN` and the DSIM IRQ, **DRM code must not
touch `DSIM_INTMSK`** — `dsim_enable_interrupts`/`disable_interrupts`
refcount it (`dsim_drv.c:86-131`) around command transfers.

### 1.7 Panel: reset, rails, ID, backlight

* Reset/rails are **not** in the panel driver; they are board-list text actions
  executed by `run_list()` (`decon_board.c:449-460`): `make_list()` resolves
  `type`/`gpios`/`regulator`/`delay`/`pinctrl` once (`:268-360`, regulator via
  `regulator_bulk_get`, `:319`), `do_list()` replays them (GPIO via
  `gpio_request_one`+`free`, `:374-384`; regulators via
  `regulator_enable/disable`, `:386-394`; pinctrl, `:405`).
* Panel ID: `0xFD 0xD1` TX then read 3 bytes through `0xFE`
  (`ea8061_param.h:14-16,209-212`; `ea8061_read_init_info`,
  `ea8061_lcd_ctrl.c:769-840`). A failed ID read sets
  `priv->lcdConnected = PANEL_DISCONNEDTED`, which makes all later
  `dsim_write_hl_data()` calls silent no-ops (`:66-68`).
* Brightness: `/sys/class/backlight/panel`, max = `EXTEND_BRIGHTNESS` (355,
  `ea8061_param.h:42`), default `UI_DEFAULT_BRIGHTNESS` (134, `:45`)
  (`ea8061_lcd_ctrl.c:1133-1134`). `panel_backlight_ops.update_status`
  (`:456-483`) writes, in order: F0 on, LTPS stop, gamma (`0xCA`),
  AID, LTPS update, ELVSS (`0xB2`/`0xD4`), ACL, F0 off
  (`low_level_set_brightness`, `:329-362`). There is no DRM brightness
  property and none is needed: Plasma reads the backlight class.
* `CONFIG_PANEL_EA8061_DYNAMIC=y` is what selects `ea8061_mipi_lcd_driver`
  (`panels/dsim_panel.c:28`, `Kconfig`). With it off, `dsim->panel_ops` stays
  NULL and `call_panel_ops()` no-ops (`dsim.h:52-53`) — the link would come up
  with an uninitialised panel. Keep the option on in every KMS config.

### 1.8 Timings actually used by the fb driver

| Parameter | Value | Cite |
|---|---|---|
| mode | 720×1280, video, burst, continuous HS clock | dts + `dsim_reg_7580.c:394-421` |
| hbp / hfp / hsw | 48 / 56 / 4 → htotal 828 | `exynos7580-j7elte_rev00.dts:1054` |
| vbp / vfp / vsw | 5 / 13 / 2 → vtotal 1300 | `:1056` |
| refresh | 60 Hz (DT) | `:1052` |
| pixel clock | `disp-pll-clk` 64.5 MHz → 64.5e6/(828×1300) = **59.93 Hz** actual | `.dts:1018`, `decon-int_drv.c:181` |
| DSI rate | 481 Mbps/lane × 4 lanes, escape 16 MHz | `.dts:1057-1058`, dtsi:1194 |
| data format | RGB24 (DSIM pixel format 0x7) | `dsim_reg_7580.c:394-421` |

Note the fbdev `pixclock` is **not** usable for DRM: `decon_missing_pixclock()`
computes 1e12/(xres·yres·60) = 55.3 MHz, ignoring porches
(`decon_core.c:2821-2834`, applied at `:2877`). A DRM mode must be built from
the porches and `disp-pll-clk`: `clock = 64500` kHz, `hsync_start = 720+56`,
`hsync_end = 780`, `htotal = 828`, `vsync_start = 1293`, `vsync_end = 1295`,
`vtotal = 1300`. (DECON's real vclk is exactly 64.5 MHz, so 59.93 Hz is the
honest vrefresh; KWin/timing maths tolerate it.)

### 1.9 Video vs command mode, TE, vsync

* Video mode is selected three times consistently: DT `psr_mode=0`, panel node
  `mode=0`, and `dsim_reg_set_config(DECON_VIDEO_MODE, lanes)`.
* TE pin: the dsim node has `turnon_tes`/`turnoff_tes` pinctrl states
  (dtsi:1189-1191) but **nothing on this board selects them**: the
  `decon_board_list` has no `pinctrl,…` action (dts:1031-1047), and the TE
  GPIO ISR is only registered in command mode
  (`decon_fb_config_eint_for_te` returns immediately unless
  `psr_mode == DECON_MIPI_COMMAND_MODE`, `decon-int_drv.c:808-832`).
* In video mode the vblank source is DECON's own frame IRQ:
  `decon_int_irq_handler` handles `VIDINTCON1_INT_FRAME` and wakes
  `vsync_info.wait` (`decon-int_drv.c:76-127`, VSYNC block `:93-99`); `decon_wait_for_vsync()` gates
  on it (`decon_core.c:1226-1268`). DSIM's `FRAME_DONE` IRQ is only used for
  LPD wake (`dsim_drv.c:718-722`) and as the `DSIM_IOC_VSYNC` refcounted source
  (`:86-131`, `decon_activate_vsync`, `decon_core.c:1177-1199`).

### 1.10 Power, ULPS, suspend/resume today

* `dsim_pm_ops` is **runtime-only** (`dsim_drv.c:1620-1623`): suspend disables
  `pclk`, `rgb_vclk0`, `pclk_disp` and both D-PHY clocks (`:1585-1602`);
  resume re-enables the three base clocks (`:1604-1618`). `dphy_esc/dphy_byte`
  are *not* owned by runtime PM in the enable direction; `dsim_enable()`
  prepares them explicitly (`:885-886`).
* `decon_pm_ops` is runtime-only too (`decon_core.c:2588-2592`,
  `:2542-2586`), gating `pclk/aclk/eclk/vclk/aclk_disp`.
* There is **no system `.suspend`/`.resume`** on either driver. Panel off at
  system suspend therefore depends on fb blank (`decon_blank` →
  `decon_disable`, `decon_core.c:1129-1176`) or on LPD. LPD is compiled out in
  the shipping config (`# CONFIG_DECON_LPD_DISPLAY is not set`, `.config:2388`),
  so `DSIM_IOC_ENTER_ULPS` (`dsim_enter_ulps`, `dsim_drv.c:976-1037`) is
  currently dead code. A KMS driver must add real suspend/resume (§2.5).
* `pd_disp` is the genpd shared by DECON and DSIM (dtsi:1082,1173); with both
  runtime-suspended the domain drops.

---

## 2. What the DRM encoder/connector pair must do

### 2.1 Ownership split (recommended)

| Component | Owner after KMS lands |
|---|---|
| `decon_fb@14830000` (CRTC + planes + fbdev emulation) | new DECON KMS driver (DECON fbdev driver stops registering fb0) |
| `dsim@14800000` (DSI host + D-PHY) | **unchanged vendor `dsim_drv.c`** |
| panel (`ea8061_lcd_ctrl.c`), backlight, lcd class | **unchanged** |
| board-list power/reset | **unchanged** (`decon_board.c`) |

The encoder is therefore a thin DRM object with a `struct v4l2_subdev *dsim_sd`
and a copy of `struct decon_lcd`. It must not ioremap 0x14800000, must not touch
DSIM clocks/regulators, and must not bind the DSIM DT node.

### 2.2 One-time init (DRM load / encoder init), in order

1. `md = module_name_to_driver_data("exynos-mdev")`;
   `dsim_sd = md->dsim_sd[0]` — same lookup as `find_subdev_mipi`
   (`decon-int_drv.c:203-227`, called from `decon_core.c:2730`). If NULL,
   return `-EPROBE_DEFER`: the
   DSIM probe (`late_initcall`) must have registered the subdev
   (`dsim_drv.c:1199-1241,1637-1644`).
2. `v4l2_subdev_call(dsim_sd, core, ioctl, DSIM_IOC_GET_LCD_INFO, NULL)` then
   `lcd = v4l2_get_subdev_hostdata(dsim_sd)` — this hands back the panel's
   parsed `struct decon_lcd` (`dsim_drv.c:1118-1120`; same trick at
   `decon-int_drv.c:219-220`). Copy it; do not parse the panel DT node twice.
3. Build the single `drm_display_mode` from `lcd` + `disp-pll-clk` (§1.8) and
   `drm_mode_set_name()`.
4. `drm_encoder_init(dev, enc, &encoder_funcs, DRM_MODE_ENCODER_LVDS)`
   (`include/drm/drm_crtc.h:885-888`); set `enc->possible_crtcs` to the DECON
   CRTC mask.
5. `drm_connector_init(dev, conn, &connector_funcs, DRM_MODE_CONNECTOR_LVDS)`
   (`:877-879`), `drm_connector_helper_add()` with
   `get_modes/mode_valid/best_encoder` (`drm_crtc_helper.h:121-133`),
   `drm_mode_connector_attach_encoder()`.
6. Fixed panel: `conn->polled = 0`, `detect()` returns
   `connector_status_connected` unconditionally, `get_modes()` returns exactly
   one `DRIVER|PREFERRED` mode, `mode_valid()` accepts only that mode.
7. Do **not** power, reset or init the panel here. Rails/reset/ID read already
   happened in `dsim_probe` + panel probe (`dsim_drv.c:1458-1528`). Do not
   create a second brightness interface; `/sys/class/backlight/panel` is
   already the userspace contract.

> 3.10 has no `DRM_MODE_CONNECTOR_DSI` and no `DRM_MODE_ENCODER_DSI`
> (`include/uapi/drm/drm_mode.h:190-205,162-167`). Use `LVDS` (what pre-DSI
> kernels did for fixed panels), or let the M0 slice add `DSI = 16` to the
> uapi — 16 is exactly the upstream number, so the ABI stays compatible.

### 2.3 Modeset ON — required order

The decisive constraint is the vendor's own ordering in `decon_enable()`:
`DSIM s_stream(1)` **before** `decon_reg_init()` and `decon_reg_start()`
(`decon_core.c:942`, `:958`, `:970`). The link and the panel must be live before
DECON starts pushing pixels, and the panel must not be re-initialised after
pixels start.

In the 3.10 helper sequence (`drm_crtc_helper_set_mode`,
`drm_crtc_helper.c:385-511`) the callbacks run:
`encoder->mode_fixup` → `crtc->mode_fixup` → `encoder->prepare` (`:447`) →
`crtc->prepare` (`:452`) → **`crtc->mode_set` (`:457`, the only fallible
one)** → `encoder->mode_set` (`:470`) → **`crtc->commit` (`:474`)** →
`encoder->commit` (`:482`). Therefore the transformer split must be:

* encoder `mode_set`/`prepare` are **void** and must not start the link;
* the **CRTC's** `mode_set` performs the link enable (it can fail and abort);
* the **CRTC's** `commit` performs `decon_reg_start()`.

Required order for one commit:

1. *(optional, only if timings differ from `dsim->lcd_info`)*
   `v4l2_set_subdev_hostdata(dsim_sd, &lcd)` and
   `v4l2_subdev_call(dsim_sd, core, ioctl, DSIM_IOC_SET_PORCH, NULL)`
   (the ioctl ignores its argument and reads the hostdata pointer). On this
   panel the timings are fixed, so this is a no-op in practice; the vendor uses
   it for partial updates (`decon_win_update_disp_config()`,
   `decon_core.c:815-841`). ⚠ It is compiled **only** under
   `CONFIG_FB_WINDOW_UPDATE` (`dsim_drv.c:1143-1151`); a KMS build must keep
   that option or add a tiny exported helper.
2. **CRTC `mode_set`, step A — power + link up:**
   `pm_runtime_get_sync(decon_dev)` first (vendor does this before
   `s_stream`, `decon_core.c:925`), then `v4l2_subdev_call(dsim_sd, video,
   s_stream, 1)` → the §1.3 sequence (rails → D-PHY → `dsim_reg_init` →
   PLL/byte/esc clocks → lanes → panel reset → HS clock → ints → panel
   `displayon`). `crtc_funcs->mode_set` is an `int` where **0 = success and
   non-zero = failure** (`drm_crtc_helper.c:457-459`); on failure the helper
   restores the previous state and never reaches step B.
3. **CRTC `mode_set`, step B — controller init:** `decon_reg_init()` (reset,
   clkgate, VIDOUT, vclk hold, window clear, RGB order, porch, linecnt
   threshold, clkdiv, freerun, `direct_on_off(0)`; it does **not** start
   scan-out) — `decon_reg_7580.c:131-150,184-200,414-450`; then program the
   window registers (`WINCON/VIDOSD/VIDWADD/…`).
4. **CRTC `commit`:** `decon_reg_start()` (`direct_on_off(1)` +
   `update_standalone`) — `:480-490`. The first frame is now scanned out.
5. Vblank is enabled only if the client asked for it (`enable_vblank`, §2.5),
   never inside `mode_set`.

Do **not** copy the vendor's `DECON_STATE_INIT` short-circuit
(`decon_core.c:901-905`) — it makes the first `decon_enable()` a no-op because
fbdev probe already started the pipe. A KMS CRTC's mode_set/commit must always
program.

### 2.4 Modeset OFF — required order

Mirror the vendor (`decon_disable`, `decon_core.c:1014-1120`): in video mode it
calls `s_stream(0)` at `:1059-1063` and only then `decon_reg_stop()` at `:1072`
(plus `decon_reg_clear_int`, `iovmm_deactivate`, `pm_runtime_put_sync` at
`:1074-1089`).

The 3.10 helper already disables in the wanted order:
`drm_helper_disable_unused_functions()` calls `drm_encoder_disable()` for every
unused encoder (`drm_crtc_helper.c:288-294`) **before** `crtc_funcs->disable()`
(`:296-306`). So the encoder's `disable`/`dpms(OFF)` is where `s_stream(0)`
belongs, and the CRTC's `disable` runs immediately after it.

1. Disable vblank (drop the DECON frame IRQ refcount, `DSIM_IOC_VSYNC(0)` if the
   DSIM counter was in use — `decon_deactivate_vsync`, `decon_core.c:1201-1224`).
2. **Encoder disable:** `s_stream(0)` → §1.4 (full-screen DCS, panel suspend
   with DISPLAY_OFF/SLEEP_IN delays, ints off, HS clock off, lanes LP-00, DSIM
   clocks off, SW reset, D-PHY off, rails off).
3. **CRTC disable:** `decon_reg_stop()` (`per_frame_off`, wait stop status,
   `direct_on_off(0)`), `decon_reg_clear_int()`, `iovmm_deactivate()`,
   `pm_runtime_put_sync(decon)` (vendor order `decon_core.c:1072-1089`).
   DSIM's runtime reference is released by `s_stream(0)` itself
   (`dsim_enable` gets at `dsim_drv.c:868`, `dsim_disable` puts at `:946`);
   DRM must not add a second get/put on the DSIM device.

Rationale for keeping the vendor order rather than "stop DECON first": the
panel-off DCS writes need the link still in HS, and the sequence is the one
that has been running on every screen-off since the product shipped. DECON's
FIFO merely underruns for the few µs between step 2 and step 3.

### 2.5 Vblank and suspend/resume

* `get_vblank_counter`: software counter incremented in the DECON frame IRQ
  (`VIDINTCON1_INT_FRAME`, `decon-int_drv.c:93-99`); 3.10's DRM needs
  `drm_driver.get_vblank_counter/enable_vblank/disable_vblank`.
* `enable_vblank`: `decon_reg_set_int(..., DSI_MODE_SINGLE, 1)` and
  `DSIM_IOC_VSYNC(1)` — exactly `decon_activate_vsync()`
  (`decon_core.c:1177-1199`). 3.10 has no `drm_crtc_vblank_on/off`
  (`include/drm/drmP.h:1464,1491-1492`): use
  `drm_vblank_off(dev, crtc)` to stop it and
  `drm_vblank_pre_modeset()`/`drm_vblank_post_modeset()` around every
  mode set, as KMS drivers of this vintage do.
* **System suspend** (new for KMS; neither vendor driver has it, §1.10):
  1. `drm_kms_helper_poll_disable()`;
  2. disable vblank;
  3. encoder disable (`s_stream(0)`, panel off, D-PHY off) — mandatory before
     any `pm_runtime_put`, because `s_stream` is what releases
     `dphy_esc`/`dphy_byte` and the panel rails;
  4. CRTC disable (stop DECON, `iovmm_deactivate`);
  5. `pm_runtime_put_sync(decon)` only; `s_stream(0)` has already released
     DSIM's own runtime reference (`dsim_drv.c:868,946`), so no extra DSIM
     put — with both devices idle `pd_disp` then powers down.
  Both DECON and DSIM must be idle before the last runtime-PM put lets
  `pd_disp` drop. Keep the order used at every screen-off (§2.4): link and
  panel down first (the panel-off DCS needs the live link), DECON stop
  immediately after, then the runtime puts. The one forbidden state is DECON
  still scanning out while DSIM's clocks/rails are gone and the power domain is
  powering down.
* **Resume**: reverse — encoder enable (`s_stream(1)` takes DSIM's runtime
  reference itself and enables the base clocks via `dsim_runtime_resume`,
  `dsim_drv.c:868,1604-1618`; it performs the full panel init because the
  panel state is SUSPENDED after §1.4, `ea8061_lcd_ctrl.c:1586-1611`),
  `pm_runtime_get_sync(decon)` + CRTC init+start, `drm_atomic_helper_resume()` (or re-commit the saved
  state; **not** `drm_mode_config_reset()`, which would drop the mode), re-enable vblank,
  `drm_kms_helper_poll_enable()`.
* Note `s_stream(1)` after a real panel-off performs the *complete*
  `ea8061_init()` script; before any panel-off it is a no-op. That asymmetry is
  intended and is what makes cold boot work (§1.3).

### 2.6 Reuse vs DRM-native, item by item

| Piece | Verdict |
|---|---|
| `dsim_reg_7580.c` (PLL/PMS, D-PHY timings, porch, config, lanes, HS clock, ints) | **Reuse as-is** via `s_stream`/ioctls. No DRM equivalent exists in 3.10 and none should be written. |
| `dsim_reg_*` command FIFO/RX path (`dsim_write_data/read_data`) | **Reuse as-is**, driven by the panel driver. Only add `EXPORT_SYMBOL` if the KMS driver is ever built as a module. |
| `dsim_drv.c` power/clock/reset sequencing, ULPS, LPD hooks | **Reuse as-is** (`s_stream`, `DSIM_IOC_ENTER_ULPS`). |
| `panels/ea8061_lcd_ctrl.c` init/uninit/brightness/MTP | **Reuse as-is** through the panel ops (`displayon`/`suspend`) and the backlight class. |
| `decon_board.c` rails/reset/pinctrl lists | **Reuse as-is** (called from DSIM). |
| DT panel node parsing | Reuse the already-parsed `struct decon_lcd` (`DSIM_IOC_GET_LCD_INFO`); do not re-parse. |
| `decon_display/decon_mipi_dsi.c` | **Not for this SoC.** It belongs to `CONFIG_EXYNOS_DECON_DISPLAY` (5430 family; `drivers/video/exynos/Makefile:9`), matches `samsung,exynos5-dsim` (`decon_mipi_dsi.c:68`), is not enabled for 7580 (`.config:2383` `# CONFIG_EXYNOS_DECON_DISPLAY is not set`). The plan's mention of `decon_display/decon_mipi_dsi_lowlevel.c` refers to a file that **does not exist**; the 7580 low-level is `dsim_reg_7580.c`. |
| Encoder/connector objects, mode list, `detect`, `best_encoder`, `mode_valid` | **Must be new DRM code.** |
| vblank, `get_vblank_counter`, suspend/resume integration | **Must be new** (CRTC/driver level, not connector). |
| fbdev emulation (`drm_fb_helper`) | New, but it consumes this connector transparently. |
| Brightness DRM property | **Not needed**; keep `/sys/class/backlight/panel`. |

### 2.7 Pitfalls specific to this panel

1. **Never claim `samsung,exynos5-mipi-dsi`.** That is the failure mode of
   `patches/drm-kms-coexist/DESIGN.md`: the fbdev DECON lost its device and the
   screen went dark. The DSIM driver must keep its DT node; the encoder talks
   to its v4l2 subdev.
2. `dsim_enable()` is a no-op in the bootloader-handoff case; do not treat the
   absence of panel init at boot as a bug or try to force one.
3. `dsim_write_data()` requires `HSCLKEN` (`dsim_drv.c:239-244`): never issue
   panel commands from an encoder hook that can run before `s_stream(1)` or
   after `s_stream(0)`.
4. Do not mask/unmask DSIM interrupts from DRM code; the refcount in
   `dsim_enable_interrupts` (`:86-107`) is used around every FIFO wait.
5. `DSIM_IOC_SET_PORCH` disappears if `CONFIG_FB_WINDOW_UPDATE` is turned off
   (`:1143-1151`). Either keep the option or add an explicit helper.
6. `exynos5_dism_phy_enable(0, …)` hard-codes DSI0 (`:688-692`); there is no
   second DSI on this board, and no per-id plumbing should be attempted.
7. Panel ID read failure silently disables all panel writes
   (`ea8061_lcd_ctrl.c:66-68`, `:769-786`). If KMS later shows a black screen,
   check for `"can't find connected panel"` in dmesg before blaming DRM.
8. `CONFIG_PANEL_EA8061_DYNAMIC` and `CONFIG_EXYNOS_MIPI_DSI` must stay `y`;
   with them off there is no `panel_ops`, no backlight class and no lcd class.

---

## 3. Encoder skeleton (pseudo-code)

Legacy 3.10 helper callbacks are shown; when M0's atomic core lands, `dpms`
maps to `atomic_enable`/`atomic_disable` and `mode_set` to the CRTC/encoder
atomic mode-set path — **the call order below does not change**.

```c
struct decon_dsi_encoder {
        struct drm_encoder      encoder;
        struct drm_connector    connector;
        struct v4l2_subdev     *dsim_sd;      /* md->dsim_sd[0]        */
        struct decon_lcd        lcd;          /* copy from hostdata    */
        struct drm_display_mode mode;
        bool                    link_on;
};

/* ---- one-time init, after the DECON CRTC exists -------------------- */
int decon_dsi_encoder_init(struct drm_device *drm, int crtc_mask)
{
        struct decon_dsi_encoder *e = drv->dsi;   /* DECON driver's object */
        struct exynos_md *md = module_name_to_driver_data("exynos-mdev");

        if (!md || !md->dsim_sd[0])
                return -EPROBE_DEFER;

        e->dsim_sd = md->dsim_sd[0];
        v4l2_subdev_call(e->dsim_sd, core, ioctl, DSIM_IOC_GET_LCD_INFO, NULL);
        memcpy(&e->lcd, v4l2_get_subdev_hostdata(e->dsim_sd), sizeof(e->lcd));

        decon_dsi_build_mode(&e->mode, &e->lcd, disp_pll_clk /* 64500 kHz */);

        drm_encoder_init(drm, &e->encoder, &enc_funcs, DRM_MODE_ENCODER_LVDS);
        e->encoder.possible_crtcs = crtc_mask;

        drm_connector_init(drm, &e->connector, &conn_funcs,
                           DRM_MODE_CONNECTOR_LVDS);
        drm_connector_helper_add(&e->connector, &conn_helper_funcs);
        drm_mode_connector_attach_encoder(&e->connector, &e->encoder);
        e->connector.polled = 0;              /* soldered panel */
        return 0;
}

/* ---- link control: called by the CRTC, not by the encoder ---------- */
/* CRTC mode_set() calls this FIRST (it is the only fallible step) and must
 * abort the modeset if it fails; only then does it run decon_reg_init().
 * CRTC commit() runs decon_reg_start() afterwards.                       */
int decon_dsi_link_enable(struct decon_dsi_encoder *e,
                          const struct drm_display_mode *adj)
{
        /* timings are fixed; push them only if they ever change */
        if (decon_dsi_timings_changed(e, adj)) {
                v4l2_set_subdev_hostdata(e->dsim_sd, &e->lcd);
                v4l2_subdev_call(e->dsim_sd, core, ioctl,
                                 DSIM_IOC_SET_PORCH, NULL);
        }
        if (e->link_on)
                return 0;
        if (v4l2_subdev_call(e->dsim_sd, video, s_stream, 1))
                return -EIO;                  /* rails/PHY/panel failed */
        e->link_on = true;
        return 0;
}

/* ---- modeset OFF --------------------------------------------------- */
/* The helper disables encoders (drm_crtc_helper.c:288-294) before the CRTC
 * (:296-306), which is the vendor order: link+panel off, then DECON stop. */
static void decon_dsi_encoder_disable(struct drm_encoder *enc)
{
        struct decon_dsi_encoder *e = to_dsi(enc);

        if (e->link_on) {
                v4l2_subdev_call(e->dsim_sd, video, s_stream, 0);
                e->link_on = false;
        }
}
/* CRTC disable() then runs decon_reg_stop() + iovmm_deactivate +
 * pm_runtime_put; vblank is dropped by the CRTC's vblank_off path.       */

/* encoder prepare/mode_set/commit are no-ops: the link is owned by the
 * CRTC hooks above and no panel command may run from a void callback.    */

/* ---- connector ----------------------------------------------------- */
static int decon_dsi_connector_get_modes(struct drm_connector *c)
{
        struct decon_dsi_encoder *e = to_dsi_conn(c);
        struct drm_display_mode *m = drm_mode_duplicate(c->dev, &e->mode);

        if (!m)
                return 0;
        m->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
        drm_mode_probed_add(c, m);
        return 1;
}

static enum drm_connector_status
decon_dsi_connector_detect(struct drm_connector *c, bool force)
{
        return connector_status_connected;
}

static int decon_dsi_connector_mode_valid(struct drm_connector *c,
                                          struct drm_display_mode *m)
{
        struct decon_dsi_encoder *e = to_dsi_conn(c);
        return drm_mode_equal(m, &e->mode) ? MODE_OK : MODE_BAD;
}

/* ---- suspend / resume (new: vendor drivers have runtime PM only) ---- */
int decon_dsi_suspend(struct drm_device *drm)
{
        drm_kms_helper_poll_disable(drm);
        drm_vblank_off(drm, crtc_idx);                /* 3.10 API: no
                                         * drm_crtc_vblank_on/off */
        v4l2_subdev_call(e->dsim_sd, video, s_stream, 0);  /* panel + PHY */
        e->link_on = false;
        decon_crtc_stop(drv);                         /* decon_reg_stop   */
        pm_runtime_put_sync(decon_dev);
        /* no DSIM put here: s_stream(0) released its own reference */
        return 0;
}

int decon_dsi_resume(struct drm_device *drm)
{
        v4l2_subdev_call(e->dsim_sd, video, s_stream, 1);  /* gets DSIM
                                              * PM ref, full panel init */
        e->link_on = true;
        pm_runtime_get_sync(decon_dev);
        decon_crtc_start(drv);                        /* init + start     */
        drm_atomic_helper_resume(drm, saved_state);   /* not mode_config_reset */
        drm_vblank_post_modeset(drm, crtc_idx);       /* pairs with a
                                         * pre_modeset before start */
        drm_kms_helper_poll_enable(drm);
        return 0;
}
```

---

## 4. Concrete requirements handed to other slices

| Requirement | For | Why |
|---|---|---|
| Keep `CONFIG_EXYNOS_MIPI_DSI=y`, `CONFIG_PANEL_EA8061_DYNAMIC=y`, `CONFIG_EXYNOS_DECON_LCD_SYSFS=y`, `CONFIG_BACKLIGHT_LCD_SUPPORT=y` in the KMS config | KmsAssemble | without them there is no DSI host, no `panel_ops`, no backlight (`.config:2380,2401-2405`) |
| Keep `CONFIG_FB_WINDOW_UPDATE=y` **or** add an exported porch helper | KmsAssemble / DeconApi | `DSIM_IOC_SET_PORCH` is `#ifdef`-gated (`dsim_drv.c:1143-1151`) |
| Do **not** reassign `dsim@14800000`'s compatible | KmsCoexist | only `decon_fb@14830000` changes hands |
| CRTC `mode_set` must call `decon_dsi_link_enable()` **before** `decon_reg_init()`; CRTC `commit` calls `decon_reg_start()`; CRTC `disable` calls `decon_reg_stop()` after the encoder's `s_stream(0)` | DeconCrtc | vendor order `decon_core.c:942→958→970`, `:1059→1072`; 3.10 helper order `drm_crtc_helper.c:288-306,457-482` |
| Encoder `prepare/mode_set/commit` stay void no-ops; the fallible link enable lives in the CRTC's `mode_set` | DeconCrtc / DeconApi | only `crtc_funcs->mode_set` can abort a modeset (`drm_crtc_helper.c:457`) |
| Vblank comes from DECON's frame IRQ, not the connector/TE pin | DeconCrtc | `decon-int_drv.c:76-127`; TE ISR is command-mode only (`:808-832`) |
| Add `DRM_MODE_CONNECTOR_DSI = 16` / `DRM_MODE_ENCODER_DSI = 6` **or** accept LVDS | DRMAtomicM0 | absent in 3.10 uapi (`drm_mode.h:162-167,190-205`) |
| Add real `.suspend/.resume` for the display pair | DeconCrtc / DispPower | vendor has runtime PM only (`decon_core.c:2588-2592`, `dsim_drv.c:1620-1623`) |
| Never route DRM panel commands through a new write path | DeconApi | `dsim_write_data` needs `HSCLKEN` and the DSIM IRQ refcount |

## 5. Verification this slice did not run (for whoever brings M1 up)

* `modetest -M <driver>`: one connector (LVDS/DSI), one mode 720x1280@59.93,
  `mode_valid` accepts only it.
* `dmesg`: `mipi-dsi driver(VIDEO mode) has been probed`, panel
  `READ ID : …` (must not say `can't find connected panel`).
* `cat /sys/class/backlight/panel/max_brightness` → 355; brightness set/unset
  must still work with KMS active.
* Flip test: first commit powers DSIM then DECON; `dmesg` must show no
  `dsim ... not ready` and no DECON `vsync time over`.
* Blank/unblank (`FB_BLANK_POWERDOWN`/`UNBLANK` or KMS DPMS) must run the
  panel `ea8061_exit()` delays (35 ms + 150 ms) and a full `ea8061_init()` on
  the way back.
* Suspend/resume: panel dark, `pd_disp` off in
  `/sys/kernel/debug/pm_genpd/*`, and a lit panel after resume.

## 6. Residual risks

* The encoder's correctness depends on the DSIM subdev staying bound. If
  KmsCoexist decides to hide the DSIM DT node or switch off
  `CONFIG_EXYNOS_MIPI_DSI`, this whole design collapses and the DSI host must
  be re-implemented in DRM (large, unproven). Recommend making that a hard
  constraint in the coexistence decision.
* `dsim_enable()`/`dsim_disable()` are not atomic-safe primitives: they sleep
  (`msleep(150)` in `ea8061_exit`) and take mutexes. They must run in the
  atomic commit tail (worker), never in `atomic_check`/`atomic_begin`.
* Multi-CRTC or plane-only updates are unsupported by this panel path; the
  encoder exposes exactly one mode and DECON has one DSI output.
