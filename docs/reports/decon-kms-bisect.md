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
