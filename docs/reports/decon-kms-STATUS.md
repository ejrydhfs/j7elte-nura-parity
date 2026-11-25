# DECON KMS — status (2026-11-25)

One-page truth for the J7 (j7elte) atomic DECON KMS bring-up. Later
`decon-kms-int*.md` reports supersede earlier ones; this page resolves them and
deletes nothing.

Hardware state: **Image-int82** (`ac2f2bfe…`, boot `162bb2be…`) plus the int82
`.ko` pair, KMS loaded by hand. The image boots headless by design: the vendor
fbdev probe refuses under the KMS choice, so DECON stays unbound (panel keeps
the bootloader frame) until the modules load. M1 (card0 + KMS fb0 + modeset) is
up; scanout of a client buffer and a compositor are not.

## Works on hardware today

| Capability | Evidence |
|---|---|
| Module pair loads, `card0` + `controlD64` + KMS `fb0` persist | `/dev/dri/`, `/sys/class/graphics/fb0 -> …/14830000.decon_fb/…` |
| DECON FRAME interrupt fires | `/proc/interrupts` IRQ 234 rises (125→322); `VIDINTCON0=0x8803`, `decon irq: … frame=1` |
| libdrm enumeration | `modetest -c`: `DSI-1` connected 720x1280@59.92 (preferred+driver); `-p`: CRTC 18 ACTIVE |
| Mode set | `modetest -s 20@18:720x1280` → "setting mode", no gamma error, no hang, IRQ climbs |
| GEM mmap | `drm_gem_mmap ret=0`, `remap ret=0` (int79 64-bit offset fix) |
| fbdev writes | `dd /dev/fb0` returns, ~66 MB/s |
| Atomic commit entry | `kmscube -D /dev/dri/card0` reaches `exynos7580_atomic_commit: async=1`, no Oops |
| Lock hygiene, one client | owner/depth `lock_all`, nothing leaked after a clean client exit |

## Fixed, one line each

| Symptom | Fix — why it worked | Rev |
|---|---|---|
| Vendor `late_initcall_sync` move bootlooped | Keep stock `late_initcall`; vendor `decon_probe()` returns `-ENODEV` under the KMS choice | int58,int60 |
| No `card0`, probe `-EBUSY` | Publish own drvdata only after `decon_kms_get()` (it was read as `struct decon_device`) | int62 |
| `insmod`: relocation type 261 overflow | Build loadable DRM objects `-fno-unwind-tables` (no `.eh_frame` PREL32) | int64 |
| Abort on first DECON register read; clocks off | Attach/verify pd_disp and enable+verify the five clocks before any read; `pm_runtime_set_suspended()` first because the device was left `RPM_ACTIVE` | int65,int66 |
| NULL optional hooks (`.mode_set`, `.commit`, `.dpms`, `.atomic_update`, `.load_lut`, `.destroy`) | No-op encoder `mode_set`, null-guard every helper callback, real cleanup hooks | int67–69 |
| FRAME IRQ never fired (`VIDINTCON0=0`) | Re-arm after `decon_enable()`'s `reg_start`, and force `decon_reg_set_int(…,1)`+`DSIM_IOC_VSYNC` since the refcounted arm was a no-op | int70,int72 |
| Arm unobservable; teardown wedged in `kthread_stop` | Write/readback trace + scratch `VIDOUTCON0` write; skip `kthread_stop` on a D-state worker | int73 |
| Teardown deadlock (watchdog silent) | Vsync kthread waits with `…_timeout(HZ/10)` and breaks on `should_stop`/`kms_teardown` | int75 |
| fbdev and initial-modeset postchecks failed on a scanning pipe | They read legacy fields; use atomic state (`state->active`, `plane->state->fb`) + explicit plane-fb commit, guard `drm_crtc_index` | int74,int76 |
| Every userspace ioctl blocked in `lock_all` | Simplify the backported `lock_all` for one driver (documented bring-up shortcut) | int77 |
| `modetest -s` "failed to map buffer" | 32-bit `key << PAGE_SHIFT` overflow in `get_gem_mmap_offset`; return 64-bit | int79 |
| Second modeset blocked at `drm_mode_setcrtc` | Nested `lock_all` overwrote `acquire_ctx`; track owner task + depth, only the owner releases; `gamma_size=256` | int80,int81 |
| Async page flip `-EBUSY` | `drm_mode_page_flip_ioctl` checks legacy `crtc->fb`; sync it (and `plane->fb`) from the atomic state | int82 |

## Open

**1. The flip carries no buffer (M2 blocker).** The committing client logs
`exynos7580_atomic_commit: async=1 crtc->fb=0 primary->state->fb=0 by kmscube`:
int82 removed the `-EBUSY` rejection, the flip is accepted, but the committed
plane state has no framebuffer. Root cause not proven.
*Next diagnostic:* build/load the module with
`patches/drm-kms/0132-kms-manual-page-flip-state.patch` and read
`exynos7580_page_flip: state=… pstate=… pstate->fb=…`. A non-zero `pstate->fb`
means the state is right and the fault is in the commit tail/vblank event; zero
means the plane-state/property path is wrong.
*Implements it:* `code/gpu31/kms/exynos_drm7580_crtc.c`
(`exynos7580_crtc_page_flip`, `exynos7580_atomic_commit`), tail in
`code/gpu31/kms/exynos_drm7580_fence.c`, helper side in
`patches/drm-atomic/0003-*.patch`.

**2. Mesa renders on llvmpipe, not kmsro(panfrost).** `kmscube` prints
`renderer: llvmpipe` and no `drm_gem_prime_fd_to_handle` line appears: Mesa
never imports into card0, so it never selects us as the kmsro scanout device.
*Next diagnostic:* `kmscube -D /dev/dri/card0 -c 30` with `EGL_PLATFORM=gbm`,
`EGL_LOG_LEVEL=debug MESA_DEBUG=1 LIBGL_DEBUG=verbose`, `GBM_BACKEND=dri`; match
dmesg against `drm_gem_prime_fd_to_handle: dev=exynos …` (0129) and
`exynos7580_gem_prime_import: ok …` / `exynos7580_fb_create: …` (0124), and
confirm `exynos_dri.so` exists in the selected stack with kmsro pairing
`exynos`↔panfrost (the `panfrost7580`-vs-`panfrost` name trap is documented in
`code/gpu31/kms/tools/env.sh`).
*Implements it:* `code/gpu31/kms/exynos_drm7580_dmabuf.c` (import/`fb_create`),
`code/gpu31/kms/exynos_drm7580_main.c` (`DRIVER_PRIME`, name stays `exynos`);
userspace build `/data/mesabuild/mesa20-b` (outside this repo).

**3. A dead DRM client leaks `lock_all`.** Killing a client while it owns the
legacy lock blocks every later ioctl (and `fb_open`). Staged, not yet in any
image: `patches/drm-kms/0131-kms-lock-abandon-dead-owner.patch` adds
`drm_modeset_lock_all_abandon()`, a `pid_alive()` check before waiting and the
`drm_release()` hook (`drivers/gpu/drm/drm_crtc.c`). *Next diagnostic:* `kill
-9` a client mid-commit, then `modetest -c`; expect
`drm_modeset_lock_all_abandon: owner …` and enumeration to proceed. Needs an
image rebuild (built-in DRM core).

**4. `kwin_wayland` not attempted.** Blocked on 1+2. Next: kwin_wayland against
`card0` using `code/gpu31/kms/session/` once a compositor buffer scans out.

## Test and recovery harness (on device)

| Item | What it does |
|---|---|
| `/home/user/wdpet.sh` | Pets the hardware watchdog (30 s DT timeout): a full kernel hang resets the SoC instead of a battery pull |
| `/home/user/logd.sh` | Persists filtered dmesg to `/home/user/kms.trace` every 2 s |
| `/home/user/kmstest.sh <rev>` | Stops the UI, arms watchdog+logd, `insmod` helper then driver, prints nodes/traces/IRQs |
| Lockup recovery | int71+ cmdline `hardlockup_panic=1 softlockup_panic=1 panic=5`: a deadlock reboots into the headless SSH state; `PSTORE_CONSOLE` keeps the log |
| Rollback | `kernel-boot-int49.img` (`62377e6573ed5045…`): nura-shell UI, panfrost GPU at 155 fps, no KMS |
| Module-only iteration | Replace the two `.ko` and `insmod` (helper first); no flash, no reboot |
| Tooling | `code/gpu31/kms/tools/`: `kmsflip` (dumb-buffer flip + pattern checksum), `env.sh` (Mesa stack), `kms-dmesg.sh` (underrun/timeout deltas) |

The image is **OpenRC**: stop the UI with `rc-service xsession stop` (not
`systemctl`), or `fbwin` holds the modeset across its own `set_par`.
`kmstest.sh`'s `systemctl` line is a no-op there; its `pkill` covers the UI.
`Kernel BUG` spam after a failed load is userspace touching a torn-down fb0 —
fix the load, do not chase it.

## Contradictions resolved (later report wins)

- `decon-kms-m1-status.md`'s open blockers (FRAME IRQ never fires, `kthread_stop`
  deadlock) are fixed (int72/int74, int73/75); its `systemctl` procedure is
  superseded by OpenRC `rc-service xsession stop`.
- int61 blamed the probe `-EBUSY` on the vendor driver; int62 proved it was our
  own drvdata order (vendor `decon_probe` disassembles to `-ENODEV`/ret).
- int70 blamed `fbdev init failed: -5` on a missing vsync; int74/76 showed the
  failing predicates read legacy fields while the pipe was scanning
  (`VIDINTCON0=0x8803`, IRQ climbing).
- int80's depth-only lock nesting was wrong; int81's trace proved it (outer call
  lost the lock) — owner+depth is current. The note "kmscube dies inside SETCRTC
  holding `lock_all`" is stale: kmscube now reaches the commit.
- int53/int58: the module bootloop was the vendor `late_initcall_sync` move, not
  `CONFIG_MODULES`; that move is gone and int63+ boot.
