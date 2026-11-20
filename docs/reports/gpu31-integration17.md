# gpu31 integration17 — abialign de142b3 live, gates green (render-only + d0+d4)

Date: 2026-11-20. Owner: G31_Integrator17. Phone: up on
int16; this ticket builds only — Main flashes. Consumes de142b3
(syncobj HANDLE_TO_FD/FD_TO_HANDLE SYNC_FILE bit-0 tolerance,
dispatcher copy-in truncate / copy-back cap, BUILD_BUG_ON ABI
gates in syncobj + ioctl TUs, client DESTROY IOWR + 16 B handle
mirror + HTOF/FTOH wrappers + Mesa-exact selftest) plus fixups
d019c7b (gnu89: abi calls below locals) and 6d924a2 (new-file
hunk count +1,333 → +1,340); no source changes beyond the
int17 script (copy of int16 + renamed gates + new step 6e
ABI gates) and this report.

## 1. Build

Script: `code/gpu31/series/build-int17.sh` (committed, copy of
int16 with int17 names + header citing de142b3/d019c7b/6d924a2
+ new step 6e ABI gates + 6d freshness + step 8 scratch+stage
deletion). Fresh `cp -a` of the pristine tree →
`scratch-int17`, all 9 `--check` clean (backport 0001–0005 ×5,
Mode-B ×1, dmabuf ×1, driver 0001+0002), then `-j4` Image
build, RC=0. Two abialign-tree defects surfaced by the build
itself and fixed before the green run (both in G31_AbiAlign
slices, consumed as-is here): d019c7b moved the new
`p7580_abi_check_*` calls below the locals (ISO C90/gnu89);
6d924a2 corrected the regen'd 0001 ioctl.c new-file hunk
count (body had 340 `+` lines under a `+1,333` header, so
`git apply` truncated the file and dropped the submit tail).
Log: `…/j7k/gpu31/logs/gpu31-int17-build.log` (5393 lines, tail
`OBJCOPY arch/arm64/boot/Image`; zero `No such file` lines;
only `error`-grep hits are the benign ext4 `strncpy`/
`__save_error_info` + `scsi_error.o` filename + one `KERN_ERR`
macro line — none in panfrost/drm paths).
Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS="-Wno-error" -j4 Image`.
Pack: stock dt.img + reference ramdisk-new.gz (never rebuilt),
`console=tty0 loglevel=7` cmdline, 27299840 B =
kernel 18160760 + ramdisk 8822392 + dt 313344.
Gates (steps 6b+6c+6e, RC=1 FAIL on any disagreement —
scratch kept for diagnosis): int16's full gate (single
definitions via nm in both objects and System.map,
`__ksymtab_drm_sched_job_alloc`, offsetof getters 24/232/236
with the real kbuild include chain, stp/ldr immediates, pins
+ round-trip + numeric ST-OPS/ST-RUN/bail/retire in applied
tree and Image strings, weak/strong run_job_addr linkage)
PLUS int16 IRQ gates (zero-req JS0 route, `(status | rawstat)`
demux, rearm decl/def/use + program_as on both recover and
resume paths, JS_STATUS reads, all 7 job probe strings in
applied tree + job.o + linked Image) PLUS new step 6e ABI
gates (BUILD_BUG_ON pins in both TUs, SYNC_FILE tolerance,
copy-size caps, handler symbols in object + System.map).
Console: `…/logs/gpu31-int17-console.log` (RC=0, all three
gates PASS); evidence:
`…/logs/gpu31-int17-evidence.log`.
Scratch + stage + offcheck one-shots deleted after the build;
kept: `Image-int17`, `kernel-boot-int17.img`, both logs.

## 2. Gate evidence (computed inside the build, not inferred)

Objects (before deletion, aarch64, `--time-style=full-iso`):

- `drivers/gpu/drm/drm_sched.o` (121912 B, 2026-11-20 17:35:28)
- `drivers/gpu/drm/panfrost/panfrost7580_submit.o` (224848 B, 17:35:34)
- `drivers/gpu/drm/panfrost/panfrost7580_job.o` (120208 B, 17:35:35)
- `drivers/gpu/drm/panfrost/panfrost7580_ioctl.o` (170288 B, 17:35:34)
- `drivers/gpu/drm/panfrost/panfrost7580_syncobj.o` (218288 B, 17:35:36)
- `arch/arm64/boot/Image` (18160760 B, 2026-11-20 17:37:00)

Prior fixes intact (unchanged int16 gates, all PASS):

- Single definitions: `drm_sched_job_alloc` T ×1 in sched.o /
  System.map (`ffffffc00056cb00`); `p7580_drv_run_job` t ×1 in
  submit.o / System.map (`ffffffc000578064`);
  `p7580_drv_sched_ops` r ×1 in submit.o / System.map
  (`ffffffc000b4bff0`); `__ksymtab_drm_sched_job_alloc` r present.
- `p7580_drv_run_job_addr`: W (weak) in sched.o
  (`0000000000000c28`), T (strong) in submit.o
  (`00000000000001f0`), `ffffffc000577fd0 T` in System.map.
- True in-tree layout via kbuild include chain: **ops=24 slot=232
  ran=236** (all three compares passed).
- Compiler immediates: `550: a9015093 stp x19, x20, [x4, #16]`
  (entity+ops fused store) in `drm_sched_job_alloc`;
  `2b0: b94004cc ldr w12, [x6, #4]` stamped-slot load (+ stores
  at #4, `cmp #0x2` range check — same disasm as int16).
- Pins + round-trip + numeric probes in applied tree, both
  objects, and Image strings (ST-OPS numeric ×1, ST-RUN entry
  ×1, `run_bail` ×4, `retire` ×1, ST-FINISH owner ×1 →
  `p7580_sched:` total = **17**, same as int16).
- IRQ gates (d852459, all PASS): zero-req JS0 route present in
  applied `jm_hw.h`; `(status | rawstat)` ×4 in applied
  `job.c`; rearm decl/def/use + `program_as` on recover AND
  resume; all 7 job probe strings in applied tree + job.o +
  linked Image (`p7580_job:` total = **7**: dispatch ×1,
  irq ×4, irq-arm ×1, post-start ×1, fence ×1).

New ABI gates (de142b3, all PASS):

- BUILD_BUG_ON pins in applied `panfrost7580_syncobj.c`:
  `sizeof(struct p7580_syncobj_handle) != 16`,
  `sizeof(struct p7580_syncobj_wait) != 32`,
  `P7580_SYNCOBJ_HTOF_EXPORT_SYNC_FILE != (1u << 0)`,
  `P7580_SYNCOBJ_FTOH_IMPORT_SYNC_FILE != (1u << 0)`.
- BUILD_BUG_ON pins in applied `panfrost7580_ioctl.c`:
  `sizeof(struct drm_panfrost_get_param) != 16`,
  `sizeof(struct drm_panfrost_submit) != 40`.
  (The mirror `sizeof(struct p7580_submit) != 40` pin lives in
  `p7580_abi_check_submit_mirror()` after the struct definition
  — C needs the complete type — and both checks are called from
  `panfrost_ioctl_submit` after the locals, per d019c7b gnu89.)
- SYNC_FILE tolerance: `args->flags &
  ~P7580_SYNCOBJ_HTOF_EXPORT_SYNC_FILE` and the FTOH mirror
  both present in applied syncobj.c (bit 0 tolerated, all
  other bits — incl. TIMELINE bit 1 — rejected).
- Copy-size caps: `unsigned int cpin = usize < asize ? usize :
  asize` (truncate copy-in) and `unsigned int cpout = usize <
  asize ? usize : asize` (cap copy-back) both present in the
  applied dispatcher.
- Handler symbols: `panfrost_syncobj_handle_to_fd` T
  (`00000000000003a0`), `panfrost_syncobj_fd_to_handle` T
  (`00000000000005cc`), `panfrost_syncobj_ioctl` T
  (`0000000000001380`) in syncobj.o; both HTOF/FTOH ` T` rows
  present in System.map.

## 3. Config (pmaports base + d0-m1-minimal + d4-panfrost7580, olddefconfig)

Unchanged fragment application (same loop as int16). Built
`.config` carries exactly the fragment symbols (all `=y`):
`CONFIG_MODULES`, `CONFIG_DRM`, `CONFIG_DRM_EXYNOS`,
`CONFIG_DRM_EXYNOS_G2D`, `CONFIG_DRM_EXYNOS_DMABUF`,
`CONFIG_DRM_PANFROST7580` (plus auto `CONFIG_DRM_GEM_CMA_HELPER=y`,
`CONFIG_DMA_SHARED_BUFFER=y`). `# CONFIG_FRAMEBUFFER_CONSOLE is not
set`; no `DRM_EXYNOS7580`. Vendor fbdev intact; `.driver_features =
DRIVER_GEM | DRIVER_PRIME | DRIVER_RENDER_ONLY`; `Mali-T720 render-only`
string in main.o (all unchanged from int16).

## 4. Freshness proof (clean tree, not incremental reuse)

- Scratch born 2026-11-20 17:32:56 (`cp -a` of the pristine
  tree); objects newer (drm_sched.o 17:35:28, submit.o/ioctl.o
  17:35:34, job.o 17:35:35, syncobj.o 17:35:36, Image
  17:37:00) — built from this tree, not carried over.
- Pristine negatives: no `drivers/gpu/drm/drm_sched.c`, no
  `drivers/gpu/drm/panfrost/` — shim and driver exist only via
  the 9 applied patches.
- `scratch-int17` + `stage-int17` + `offstage-int17` all deleted
  (verified absent); offcheck one-shots (`offcheck17.c/.s`)
  removed.

## 5. Artifacts (sha256)

On the external disk (`…/j7k/gpu31/`):

- Image: `15cea4507afbb42217916a14dc20c755eb4069a7f1674869a4f7750df5a7ee94`
  (`Image-int17`, 18160760 B, 2026-11-20 17:37 -0500)
- boot.img: `a8c8d0916b5aa1634f039f47dbbce40e1d2a59a4182edb03109a218602620272`
  (`kernel-boot-int17.img`, 27299840 B = kernel 18160760 +
  ramdisk 8822392 + dt 313344, 2026-11-20 17:37 -0500)

Seeds (read-only, never rebuilt): dt.img
`352bda9bcf02bce50c9fc6ad6ca845727715fe1942fe25604d67f94e69d8f937`
(313344 B), ramdisk-new.gz
`ac0b6f8b9d1e3fe1298d931491bd972071755379cea788d3b1ada248c8491a18`
(8822392 B), cmdline `console=tty0 loglevel=7`.

## 6. Commits (this ticket)

- int17 script: `code/gpu31/series/build-int17.sh` (new) plus
  this report. No source/patch edits — de142b3 + d019c7b +
  6d924a2 consumed as-is.
- Post-flash rerun (Main): `/tmp/abialign/pf7580-abialign
  --device` (expect PASS incl. SYNC_FILE round-trip per
  abialign §6) and NULL-job submit-test with `dmesg | grep -E
  'p7580_sched|p7580_job'`, then egltest (expect SUBMIT rows,
  non-zero pixels). No flash by this ticket.
