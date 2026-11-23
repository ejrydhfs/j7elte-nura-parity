# DECON KMS bisect pair (int51a/int51b) — atomic series vs KMS bind path; both images built, not flashed

Date: 2026-11-23. Owner: G31_Integrator51 (build seat). Phone: int50
hangs before the USB gadget comes up; no card0 test yet. **No flash** —
Main flashes.

int50 (full series, KMS on) produced no USB gadget/SSH/ping after 7
minutes, so the hang is before or during late-boot init. This pair
separates "the atomic/core series itself breaks early boot" from "the
KMS bind path (`decon_kms_get` / DECON stand-up) hangs". Both images
apply the **same 18-patch series**; only configuration and, for b, one
diagnostic hunk differ.

## 1. Images

| | **int51a** | **int51b** |
|---|---|---|
| config | `EXYNOS7580_DISPLAY_FBDEV=y`, `EXYNOS7580_DISPLAY_KMS` not set → `CONFIG_DRM_EXYNOS7580` **absent** | KMS on (`EXYNOS7580_DISPLAY_KMS=y`, `CONFIG_DRM_EXYNOS7580=y`) |
| series | 18 patches (atomic/core/shim/slices/main) | same 18 **+** diag `0202-int51b-probe-enodev.patch` |
| KMS driver | compiled out (no `exynos-drm7580.o`) | compiled and linked |
| probe | n/a | `dev_err("int51b: probe refused before decon_kms_get (diagnostic)")` then `return -ENODEV` **before** `decon_kms_get()` |
| what it proves | the whole atomic series in a running kernel **without** the KMS driver: if this does not boot, the series (not DECON) is at fault | the KMS driver registers and links, but never touches DECON: if this boots and int50 does not, the hang is in `decon_kms_get()`/DECON stand-up or later in `->load()` |
| Image | 17417376 B, `4b3c33cd647255c424b04c8bb6a2ea5bf8517cdde3a5a614d319c74cf8e3ad92` | 17500384 B, `f396aef2a377a675c726f67cd6323a455644524a98c21f3a49c6a6662752070c` |
| boot.img | 26556416 B, `2be7bd0e8b07f0fe72f37fc8ea87427fcfeea6fff1ec4fe6200d391e2422510b` | 26640384 B, `23932fece605085cb5b04a0079240d034986eff08505b1437d939a3aec0cc29d` |

Common: stock `dt.img` (+313344), reference ramdisk (+8822392), `/dev/mmcblk0p10`.
Config base `config-samsung-j7elte.aarch64`; the fragment parser honours
`# CONFIG_X is not set` lines.

## 2. Gates (both PASS, 0 failures)

- 18 (a) / 19 (b) patches, `git apply --check` clean, per-patch sha256 in
  the evidence log, new-file hunk integrity verified.
- layout: 7 sources + header in `drivers/gpu/drm/exynos7580/`, none left
  in `exynos/`; contract header identical to `code/gpu31/kms`.
- **atomic core linked in both**: `drivers/gpu/drm/drm_atomic.o`,
  `drm_mode_atomic_ioctl` once in vmlinux, `DRM_IOCTL_MODE_ATOMIC` in
  `drm_drv.c`.
- a: `EXYNOS7580_DISPLAY_FBDEV=y`, KMS symbol absent, no
  `exynos-drm7580.o`, **0** `exynos7580_atomic_commit`, **0**
  `__initcall_exynos7580_drm_init7`, vendor `decon_7580/built-in.o`
  present, DECON compatible string in the Image; config closure 11/11
  (KMS helper symbols are legitimately unset when nothing selects them).
- b: KMS object built, probe diagnostic present **before**
  `ex->decon = decon_kms_get(dev);` (source line order gated), the
  diagnostic string in the Image, `exynos7580_atomic_commit` and the
  `late_initcall` symbol each once; config closure 14/14.
- build: a 5279 lines / b 5304 lines, **0 errors**, 487 warnings each
  (ambient vendor), **0** in `drivers/gpu/drm/exynos7580/`; Image and
  vmlinux freshness gated.

Logs: `…/logs/int51a-{build,gates,evidence}.log`,
`…/logs/int51b-{build,gates,evidence}.log`; sha256s in §5.

## 3. Reading the outcomes

- **a boots, b boots** → the series is harmless in a non-KMS kernel and
  the KMS bind path is the suspect; build **int51c** (stage-limited
  probe) to find which step hangs.
- **a boots, b hangs** → the hang is *not* `decon_kms_get()`; it is in
  the KMS registration path itself (`late_initcall` →
  `platform_driver_register` → probe return path) or in the vendor
  fallback being blocked. int51c then bisects between "probe returns"
  and "late_initcall_sync fallback binds" (e.g. make the probe return
  before `of_device_is_compatible`, or at each successive check).
- **a hangs** → the atomic/core/shim series breaks early boot even with
  no KMS driver bound; bisect M0 alone (int51a1) before touching DECON.
- **b hangs with the diag string absent from early dmesg** → the kernel
  never reached probe; suspect initcall ordering or an earlier patch
  (shim/0100 touches `decon_7580` code that runs at vendor probe time).

## 4. What the atomic-core patches change on the pre-initcall path

The M0 series has **no initcalls, no new device registration and no
changes to DRM core init**:

- `0001` (headers/state/UAPI): `drmP.h`, `drm_atomic.h`,
  `drm_atomic_helper.h`, `drm_crtc.h`, `drm_crtc_helper.h`,
  `drm_modeset_lock.h`, `drm_plane_helper.h`, `drm_rect.h`, UAPI
  `drm.h`/`drm_mode.h` — types, enums and prototypes only.
- `0002` (core): new `drm_atomic.c` (state alloc/get/set, commit tail —
  reached from the atomic ioctl or a driver's commit), plus edits to
  `drm_crtc.c`, `drm_framebuffer.c`, `drm_plane.c`,
  `drm_modeset_lock.c`, `drm_rect.c` — functions used when a driver
  creates CRTCs/planes/framebuffers or takes modeset locks.
- `0003` (helpers/build): new `drm_atomic_helper.c`,
  `drm_plane_helper.c`, Makefile objects — no initcall, no global state.
- `0004` (ioctl wire): `drm_drv.c` ioctl-table entry
  `DRM_IOCTL_MODE_ATOMIC` and `drm_ioctl.c`'s
  `DRM_CLIENT_CAP_ATOMIC` bit in `drm_setclientcap` — both run only on
  ioctl entry, never at boot.
- `0005` (4-arg property hooks): `drm_atomic_helper.c` + header only.
- `0001-core-drm-crtc-helper` / `0002-core-drm-fb-helper`: edits inside
  helpers that only run when a driver binds (crtc-helper teardown,
  drmfb id name).

So the only *always-on* additions are linked code (unused until called)
and one extra entry in the DRM ioctl table. If something in the series
breaks early boot with KMS compiled out, it would have to be an
unintended side effect of those edits in code the vendor fbdev stack
runs at probe — that is exactly what int51a tests.

By contrast, the likely hang point in int50 is the KMS bind path:
`late_initcall(exynos7580_drm_init)` →
`platform_driver_register` → `exynos7580_probe` → `decon_kms_get`
(clocks, SFR mapping, runtime PM/iovmm, panel hand-off) → `->load()`
(mode_config, vblank, planes, CRTC, DSI, fbdev, initial modeset). int51b
cuts at the first of those.

## 5. Hashes and files

- `Image-int51a` `4b3c33cd647255c424b04c8bb6a2ea5bf8517cdde3a5a614d319c74cf8e3ad92`
- `kernel-boot-int51a.img` `2be7bd0e8b07f0fe72f37fc8ea87427fcfeea6fff1ec4fe6200d391e2422510b`
- `Image-int51b` `f396aef2a377a675c726f67cd6323a455644524a98c21f3a49c6a6662752070c`
- `kernel-boot-int51b.img` `23932fece605085cb5b04a0079240d034986eff08505b1437d939a3aec0cc29d`
- `…/logs/int51a-build.log`
  `ab93786a00c91eb6012692096018db5c63cfbfea7c24a6eb151b6e078e527251`
- `…/logs/int51a-gates.log`
  `116c2cf55cbec05bf522eb61a2d98326e9fee2f2f5fd8b525f5aa6e077623a8d`
- `…/logs/int51a-evidence.log`
  `edf8674259bd6ffe18baba4e3d9470415cbeaba2c55df802011031d3e81ff305`
- `…/logs/int51b-build.log`
  `d74ae95c11b320c3a7316c3d6249da4a963c10e9a68d4910e74351c6b0986940`
- `…/logs/int51b-gates.log`
  `64cb9284d7ab77b46e473432cf30ce34740a28a198381deab0041f21d9e26750`
- `…/logs/int51b-evidence.log`
  `5dfd10f9f4ac70768c7aacc5c5c1bac3847c2b62302e906587b7e49f8684cf14`
- script `code/gpu31/series/build-int51.sh` (233 lines)
  `5a9788d8e8312c11cf103845c89ef2f82d975261be2ab533a6de1a325a880fbd`
- diag probe patch `patches/drm-kms/diag/0202-int51b-probe-enodev.patch`
  `65677c78acee102e2a1f6c06a61dc0333dd8780793460e9557c2640357427b0d`
- diag config `patches/drm-kms/diag/display-owner-fbdev-diag.fragment`
  `a75c7b01184d0d637d93cadbdc93740c0c640fdd5812ea29158a81d5d2302203`

## 6. int51c (prepared, built only if a and b boot)

Design: KMS on, `decon_kms_get()` reachable, with a `kms_probe=N` module
parameter and a bounded watchdog around each stage of the stand-up, so
the last stage printed before the hang identifies the call. Stage
markers at: entry, `decon_kms_get` return, clocks on, SFR remap, runtime
PM get, iovmm init, panel hand-off. A hard 3 s
`schedule_timeout_killable()` guard turns a hang into a probe failure so
the vendor `late_initcall_sync` fallback can still bind and the board
stays reachable over USB. Not built for this pair because its
interpretation depends on which of a/b boots.

## 7. Notes

- Both images keep the atomic core: a proves the series is present but
  unreachable without KMS; b proves the driver registers and links.
- In mode a, `CONFIG_DRM_KMS_HELPER`, `CONFIG_DRM_KMS_CMA_HELPER` and the
  `DRM_EXYNOS7580` symbol are legitimately absent (nothing selects them
  when the choice is FBDEV), so the closure gate checks those as
  absences rather than literal lines.
- 487 warnings in both logs is the vendor ambient count; the KMS sources
  contribute none.

## 8. Strict-minimal pair (int51d / int51e) — after int51a also bootlooped

int51a still applied the whole series, including the vendor-DECON shim
(`0100`), whose code runs in the **fbdev** path. So int51a did not
isolate the atomic core. These two variants are minimal: each applies
exactly one patch group to the pristine vendor tree, and the config is
the stock `config-samsung-j7elte.aarch64` plus the diagnostic fragment
noted below (nothing else).

| | **int51d** | **int51e** |
|---|---|---|
| patches | `drm-atomic/0001..0005` only | `0100-decon-export-shim.patch` only |
| tree otherwise | pristine vendor | pristine vendor |
| config | stock + `CONFIG_DRM=y` + `CONFIG_PSTORE_CONSOLE=y` (atomic objects need DRM; hidden `DRM_KMS_HELPER`/`CMA_HELPER` are not user-settable and are not forced) | stock + `CONFIG_PSTORE_CONSOLE=y` (no DRM at all) |
| KMS driver | absent | absent |
| proves | does the 3.10 atomic core backport **alone** boot? | does the vendor DECON probe split + `register_framebuffer()`-at-tail break boot? |
| Image | 17400992 B `ede726ceefc1af3827e6eda3b17521cf60b3242276910c54f5069e0834ced50d` | 17184904 B `c0366a22c12fbae1faa787a7d31cfdea9b1c94b6e46eedd376acf461418e8fba` |
| boot.img | 26540032 B `b47646cd472c7f05aab9dc237b4e2efffd79008c5140aac4e6d7376202b11866` | 26324992 B `8472eff145f318273cff6bccc3bbfdbe86ce5efd6ff0356f3fc70af693d4b831` |

Both: 0 errors, 487 / 486 ambient vendor warnings, 0 new warnings;
Image and vmlinux freshness gated; patch hunk integrity checked; stock
`dt.img` and reference ramdisk. Gates also prove isolation: d has no
`exynos7580/` directory, no `decon_probe_common`/`decon_kms_api.c`; e has
no `drivers/gpu/drm/drm_atomic.c` and no `exynos7580/`, while
`decon_probe_common`, `decon_kms_get`, `fb_registered` and
`register_framebuffer` are present.

Reading it: d boots + e hangs → the shim's DECON probe changes are the
culprit (not M0). d hangs → the atomic core impacts boot even without a
DRM display driver. Both boot → the hang is in the 0200 wiring or the KMS
path, and int51c (stage watchdog) is next.

### 8.1 pstore/ramoops (both variants)

No DT edit is needed: the vendor snapshot driver already registers a
`ramoops` platform device on the bootloader-reserved `"log_pstore"`
region (`drivers/trace/exynos-ss.c:ess_pstore_init`,
`record/console/ftrace/pmsg = 512 KiB`, `dump_oops = 1`), and the stock
config already has `CONFIG_PSTORE=y`, `CONFIG_PSTORE_RAM=y`,
`CONFIG_EXYNOS_SNAPSHOT_PSTORE=y`. Both variants only add
`CONFIG_PSTORE_CONSOLE=y`, so the boot console (and a panic, via
`dump_oops`) lands in pstore. Gates: `ess_pstore_init` linked,
`PSTORE_CONSOLE=y`, `ramoops` string in the Image. A panic with `dump_oops = 1`
writes synchronously at panic time; the records are read from
`/sys/fs/pstore/` by a kernel that reaches userspace. Because the region
is RAM, a bootloop that never reaches userspace cannot be read back
after a power cycle — the record survives only until power is removed.

### 8.2 Ordering hazards before the USB gadget (0100, and 0200)

- `0100` **moves `register_framebuffer()` to the very end of
  `decon_probe_common()`** (after clocks, SFR map, IRQ, media entity,
  windows, the update kthread, `pm_runtime_enable()` and the panel
  hand-off), and records `decon->fb_registered`. In the vendor path
  (`register_fb == true`) fb0 therefore appears *later* than before, and
  the update/vsync kthreads are already running when it does. If any
  kthread or notifier path waits on `fbinfo`/`fb_registered` (or on the
  probe-held mutex) without a timeout, probe can deadlock before init
  finishes — which would present exactly as "no USB gadget". That is the
  prime suspect 51e tests.
- `0100` also splits the vendor probe into `decon_probe_common()` and a
  thin `decon_probe()`, and changes the remove path to unregister only
  when `fb_registered`; error unwinding in the split is new code on the
  vendor boot path (compiled in even with KMS off, because the shim's
  Makefile hunk always builds `decon_kms_api.o`).
- `decon_kms_api.c` adds **no** initcall, no device registration and no
  global constructor; its exports are called by the KMS driver only. It
  does, however, get linked into the vendor display image in every
  variant that applies 0100.
- `0200` (not in d/e) additionally moves the vendor DECON registration
  to `late_initcall_sync` and touches `decon_core.c`; that ordering
  change is **not** present in 51d/51e, so a 51e hang points at 0100
  alone, and a 51e boot with int50 hanging points at 0200's wiring or the
  KMS `->load()` path.
- The M0 patches still contribute nothing pre-initcall (§4): no
  initcalls, no registrations, one ioctl-table entry and a client-cap
  bit reachable only from ioctls.

### 8.3 Hashes for the d/e logs

- d build `1275c4be220dad6d6959f8af7ca8daea21299d620d7728c55c75b85b55b30a20`,
  gates `0a32044929eb83ff311ecc2ec673a5723b422f1975690c1cdc14bcdabc903833`,
  evidence `904e5013ec1e02f45cdf403c83287859b26fe6f69151a43d05a7594dfd504838`
- e build `8baff82b0bbbfb3624fc5977b8162d23a8308a09656a3f59650bc38cec2273de`,
  gates `9da261599ae7a7bab81db1bc82d9d6b8525376fad0ed3673be35648caf32810c`,
  evidence `8db4cc0a1e45dd1804c7ab051fb6751ef1316d59584485bb23b39bba6898277f`
- script `code/gpu31/series/build-int51de.sh` (180 lines)
  `aac8607028c320f44cc25c1e574c74a67f4181181d19135efcaf06284264dc58`
- fragments `patches/drm-kms/diag/m0-only.fragment`
  `9b61f6e23035069e3e22f61512cfb6c5d9587840ec149b725d6a868268534c1c`,
  `…/shim-only.fragment`
  `3c5cb59b4ffd3ada6454df3392ea06dd697a10ea690b35e1290d335c838a7123`

## 9. Fix for 0100 (int51f / int52)

int51e localized the bootloop to `0100` alone. The static cause, as Main
identified in the vendor path: the patch deleted the original
`register_framebuffer()` block, and the window-programming branch further
down (`decon_reg_get_stop_status()` false) dereferences the local
`fbinfo` that block assigned:

```c
win_regs.vidosd_b = vidosd_b(0, 0, fbinfo->var.xres, fbinfo->var.yres);
win_regs.vidw_buf_start = fbinfo->fix.smem_start;
```

With registration moved to the probe tail, that branch runs first and
`fbinfo` is an uninitialised stack pointer -> panic during probe (the
"yellow lines brightening, then reboot" signature). The
bootloader-fb `goto decon_init_done` path does not help because it needs
`psr_mode == DECON_VIDEO_MODE && !decon_is_no_bootloader_fb()`.

Fixed `0100` (sha256 `744e2ce9…`), exactly per the requested shape:

1. `register_framebuffer()` is back at its original position — before
   `mutex_init(&decon->output_lock)`, the update kthread, `pm_runtime_
   enable()` and the window branch — gated by `register_fb`, keeping the
   original `fail_iovmm` path, and it sets `fb_registered`.
2. The `CONFIG_EXYNOS_DECON_7580_KMS` / `decon_kms_attach()` hand-off
   branch is **gone** from the vendor probe (the KMS driver binds the
   node itself); the weak stub and its export remain but have no caller.
3. `register_fb == false` (KMS stand-up) now `goto decon_init_done;`
   before the stop-status/window-programming section, so it never touches
   `fbinfo`; the local is initialised to NULL and the tail registration
   block is deleted. `fb_registered` still makes `decon_remove()` correct.

| | **int51f** (fixed shim only) | **int52** (full series, fixed shim) |
|---|---|---|
| patches | fixed `0100` only, pristine vendor tree | the 18-patch KMS series with fixed `0100` |
| config | stock + `PSTORE_CONSOLE=y` (no DRM) | full KMS config (as int50) |
| proves | the corrected DECON probe (registration in place, no hand-off, KMS path skips fb) boots the vendor fbdev stack | the full KMS assembly boots past probe with the corrected shim |
| Image | 17184904 B `0d88f443128d8f0d2d9917ca64d41c8d88076a976511edfdef3f387c66fc64f5` | 17500384 B `a9a53890928f0568d4dd1c3aeb3f591393ca3e712b26fa57c78c34e5e40b2e75` |
| boot.img | 26324992 B `e81144ccf408c3b317969b387b842b9abb8a2fbe75343cac0e2508dec51cb9df` | 26640384 B `aff0c6e8c6555e4592b9abc119721dce8128e5f8f195f883c6f92d15644245dd` |

Gates (both PASS): patch hunk integrity, config closure, pstore/ramoops
linked, fresh Image+vmlinux, stock `dt.img`; int51f additionally gates
`register_framebuffer(fbinfo)` count 1 and **before**
`mutex_init(&decon->output_lock)`, absence of the tail-registration
comment and of any `decon_kms_attach(decon)` call, presence of the
`if (!register_fb)` skip before the `fbinfo` window branch, the
`decon_init_done` target, `fbinfo = NULL` and `fb_registered = true`.
int52 gates the same ordering plus the full KMS layout/symbol set as
int50. Builds: int51f 5236 lines / 486 warnings, int52 5304 / 487 — 0
errors, 0 in `drivers/gpu/drm/exynos7580/`.

int51e (`8472eff1…`) is kept as the defect reproduction: its log/gates
assert the tail registration (registration line **after**
`output_lock`), so the pair e/f brackets the exact change.

Log sha256s: int51f build `ed3ee1a1e88d9f2cfd6b769bd43a8d85c011e678dca071f1f605b072891913ba`,
gates `332cf22bc23ce9abb9972403c03c8d0ac30e205643ceeae06303b02864b352fd`,
evidence `bbec2949184765062313a4b4d8cd0eb9ede4878034a0176216594a184b1ffdbf`;
int52 build `444bafe03ac3afd87ba7e98bae9bbb29b10943b5781aefa1372ae04cdaf018b5`,
gates `7275ee0155d3591ed4e4da1d232e7637d93b23e58fbbb9fce7886aa0805506c4`,
evidence `22543f2506c0623cb9ad15dbb6e41794d085148e06d28405412f585da304d16e`.
