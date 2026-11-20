# gpu31 integration16 — irqfix d852459 live, gates green (render-only + d0+d4)

Date: 2026-11-20. Owner: G31_Integrator16. Phone: up on
int15; this ticket builds only — Main flashes. Consumes d852459
as-is (zero-req jobs → JS0, STAT|RAWSTAT IRQ demux, AS0 + mask
re-arm on recover/resume, irq-arm/irq-enter/post-start/irq/
irq-err/fence-signal probes); no source changes beyond the int16
script (copy of int15 + renamed gates + new step 6c IRQ gates)
and this report.

## 1. Build

Script: `code/gpu31/series/build-int16.sh` (committed, copy of
int15 with int16 names + header citing d852459 + new step 6c IRQ
gates + 6d freshness + step 8 scratch+stage deletion).
Fresh `cp -a` of the pristine tree → `scratch-int16`, all 9
`--check` clean (backport 0001–0005 ×5, Mode-B ×1, dmabuf ×1,
driver 0001+0002), then `-j4` Image build, RC=0. No repair
needed (driver 0001 regen at d852459 applies clean).
Log: `…/j7k/gpu31/logs/gpu31-int16-build.log` (5393 lines, tail
`SORTEX vmlinux` / `SYSMAP System.map` / `OBJCOPY
arch/arm64/boot/Image`; zero `No such file` lines; only
`error`-grep hits are the benign ext4 `strncpy`/
`__save_error_info` + `scsi_error.o` filename + one `KERN_ERR`
macro line — none in panfrost/drm paths).
Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS="-Wno-error" -j4 Image`.
Pack: stock dt.img + reference ramdisk-new.gz (never rebuilt),
`console=tty0 loglevel=7` cmdline, 27299840 B =
kernel 18160760 + ramdisk 8822392 + dt 313344.
Gate (steps 6b+6c, RC=1 FAIL on any disagreement — scratch kept
for diagnosis): int15's full gate (single definitions via nm in
both objects and System.map, `__ksymtab_drm_sched_job_alloc`,
offsetof getters 24/232/236 with the real kbuild include chain,
stp/ldr immediates, pins + round-trip + numeric ST-OPS/ST-RUN/
bail/retire in applied tree and Image strings, weak/strong
run_job_addr linkage) PLUS new IRQ gates: zero-req JS0 route,
`(status | rawstat)` demux, rearm decl/def/use + program_as on
both recover and resume paths, JS_STATUS reads, all 7 job probe
strings in applied tree + job.o + linked Image.
Console: `…/logs/gpu31-int16-console.log` (RC=0, both gates PASS);
evidence: `…/logs/gpu31-int16-evidence.log`.
Scratch + stage + offcheck one-shots deleted after the build;
kept: `Image-int16`, `kernel-boot-int16.img`, both logs.

## 2. Gate evidence (computed inside the build, not inferred)

Objects present (before deletion, aarch64, `--time-style=full-iso`):

- `drivers/gpu/drm/drm_sched.o` (121912 B, 2026-11-20 16:36:43)
- `drivers/gpu/drm/panfrost/panfrost7580_submit.o` (224848 B, 16:36:51)
- `drivers/gpu/drm/panfrost/panfrost7580_job.o` (120208 B, 16:36:53)
- `arch/arm64/boot/Image` (18160760 B, 16:38:29)

Prior fixes intact (unchanged int15 gates, all PASS):

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
  at #4, `cmp #0x2` range check held from int15 — same disasm).
- Pins + round-trip + numeric probes in applied tree, both
  objects, and Image strings (ST-OPS numeric ×1, ST-RUN entry
  ×1, `run_bail` ×4, `retire` ×1, ST-FINISH owner ×1 →
  `p7580_sched:` total = **17**, same as int15).

New IRQ gates (d852459, all PASS):

- JS0 selection: `requirements == 0` route present in applied
  `panfrost7580_jm_hw.h` (zero-req → JS0; nonzero non-FS → JS1,
  ONLY_COMPUTE → JS2).
- STAT|RAWSTAT demux: `(status | rawstat)` ×4 in applied
  `panfrost7580_job.c` (per-slot DONE/ERR/CLEAR branches + loop
  guard — masked-completed slots demux via RAWSTAT instead of
  timing out).
- AS0 + mask re-arm: `p7580_job_dev_rearm` declared in applied
  `job.h`, defined (`void p7580_job_dev_rearm`) in applied
  `job.c`, used in applied `main.c` (resume path);
  `p7580_mmu_program_as` in applied `job.c` (recover) AND
  applied `main.c` (resume); `P7580_JS_STATUS` reads present
  (post-start dump + IRQ per-slot + irq-err naming).
- Job probes (applied tree `grep` + `job.o` strings + Image
  strings, each presence-gated): `p7580_job: irq-arm`,
  `irq-enter`, `post-start`, `irq slot=`, `fence signal`,
  `irq-err`, `dispatch` — all 7 live in the linked Image
  (`p7580_job:` total = **7**: dispatch ×1, irq ×4
  [enter/slot/err + dispatch-irq prefix], irq-arm ×1,
  post-start ×1, fence ×1).

## 3. Config (pmaports base + d0-m1-minimal + d4-panfrost7580, olddefconfig)

Unchanged fragment application (same loop as int15). Built
`.config` carries exactly the fragment symbols (all `=y`):
`CONFIG_MODULES`, `CONFIG_DRM`, `CONFIG_DRM_EXYNOS`,
`CONFIG_DRM_EXYNOS_G2D`, `CONFIG_DRM_EXYNOS_DMABUF`,
`CONFIG_DRM_PANFROST7580` (plus auto `CONFIG_DRM_GEM_CMA_HELPER=y`,
`CONFIG_DMA_SHARED_BUFFER=y`). `# CONFIG_FRAMEBUFFER_CONSOLE is not
set`; no `DRM_EXYNOS7580`. Vendor fbdev intact; `.driver_features =
DRIVER_GEM | DRIVER_PRIME | DRIVER_RENDER_ONLY`; `Mali-T720 render-only`
string in main.o (all unchanged from int15).

## 4. Freshness proof (clean tree, not incremental reuse)

- Scratch born 2026-11-20 16:34:33 (`cp -a` of the pristine
  tree); objects newer (drm_sched.o 16:36:43, submit.o 16:36:51,
  job.o 16:36:53, Image 16:38:29) — built from this tree, not
  carried over.
- Pristine negatives: no `drivers/gpu/drm/drm_sched.c`, no
  `drivers/gpu/drm/panfrost/` — shim and driver exist only via
  the 9 applied patches.
- `scratch-int16` + `stage-int16` + `offstage-int16` all deleted
  (verified absent); offcheck one-shots (`offcheck16.c/.s`)
  removed.

## 5. Artifacts (sha256)

On the external disk (`…/j7k/gpu31/`):

- Image: `2c5b15bd03e2f0ff7bf1bd378a122a6144ade85b87e59a9f952011366ba2fff4`
  (`Image-int16`, 18160760 B, 2026-11-20 16:38 -0500)
- boot.img: `7c7379d25cc705dedc21a189f573330c8c0af243d00d437b46f2a511ab657011`
  (`kernel-boot-int16.img`, 27299840 B = kernel 18160760 +
  ramdisk 8822392 + dt 313344, 2026-11-20 16:38 -0500)

Seeds (read-only, never rebuilt): dt.img
`352bda9bcf02bce50c9fc6ad6ca845727715fe1942fe25604d67f94e69d8f937`
(313344 B), ramdisk-new.gz
`ac0b6f8b9d1e3fe1298d931491bd972071755379cea788d3b1ada248c8491a18`
(8822392 B), cmdline `console=tty0 loglevel=7`.

## 6. Commits (this ticket)

- int16 script: `code/gpu31/series/build-int16.sh` (new) plus
  this report. No source/patch edits — d852459 consumed as-is.
- Post-flash rerun (Main): NULL-job submit-test with `dmesg |
  grep -E 'p7580_sched|p7580_job'`, match against irq §5
  expected sequence (`irq-arm` zeros → ioctl/push/queued slot=0
  → `run_job seq=0 slot=0 sslot=0` → `dispatch slot=0` →
  `post-start … js_status=0x00000000` → `irq-enter
  rawstat=0x00000001 stat=0x00000001` → `irq slot=0 … has_job=1`
  → `fence signal … sig_rc=0` → `retire … ran=1`; diagnosis
  rows in irq §5). No flash by this ticket.
