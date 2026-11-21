# gpu31 integration21 — pre-signal coherency sync at all three completion sites, host suites PASS

Date: 2026-11-21. Owner: G31_Integrator21. Phone: up on int20;
this ticket builds only — Main flashes.

Consumes c207bdd (coherency-race: `p7580_gem_job_bos_pre_signal_sync`
invalidate BEFORE every `dma_fence_signal` — IRQ DONE, timedout
own-fence, recover -EIO; free_job keeps its post-signal
`job_done_sync` + unref walk) on top of 86f82b9 (tilerchain) +
2936796 (slotfix req0→JS1) + 67eaef1 (gpuwrite). No source changes
beyond the int21 script (copy of int20 + renamed names + 6h coherency
gate) and this report.

## 1. Build

Script: `code/gpu31/series/build-int21.sh` (committed). Fresh `cp -a`
of the pristine tree → `scratch-int21`, all 9 `--check` green
(backport 0001–0005 ×5, Mode-B ×1, dmabuf ×1, driver 0001+0002), then
`-j4` Image build, RC=0. One script defect surfaced by the first build
itself and fixed before the green run: the new 6h tree-count check
`grep -c 'p7580_gem_job_bos_pre_signal_sync(job'` undercounted (2, not
3) because the recover site passes `fault_job->bos`; the committed
script counts the split-line-robust `'(.*->bos,$'` call pattern (= 3).
6h was replayed PASS in a second fresh scratch before packing.

Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
KCFLAGS="-Wno-error" -j4 Image`.
Log: `…/j7k/gpu31/logs/gpu31-int21-build.log` (5393 lines, tail
`OBJCOPY arch/arm64/boot/Image`).
Pack: stock dt.img + reference ramdisk-new.gz (never rebuilt),
`console=tty0 loglevel=7` cmdline, 27299840 B =
kernel 18160760 + ramdisk 8822392 + dt 313344.
Console: build stdout (RC=0, all gates PASS);
evidence: `…/logs/gpu31-int21-evidence.log`.
Scratch + stage + offcheck one-shots deleted after the build; kept:
`Image-int21`, `kernel-boot-int21.img`, both logs.

## 2. Gate evidence (computed inside the build, not inferred)

Objects (before deletion, aarch64, `--time-style=full-iso`):

- `drivers/gpu/drm/drm_sched.o` (unchanged ops gate, PASS)
- `drivers/gpu/drm/panfrost/panfrost7580_submit.o` (224784 B, 2026-11-20 20:47:14)
- `drivers/gpu/drm/panfrost/panfrost7580_gem.o` (252872 B, 20:47:15)
- `drivers/gpu/drm/panfrost/panfrost7580_job.o` (120816 B, 20:47:15)
- `arch/arm64/boot/Image` (18160760 B, 2026-11-20 20:49)

Prior fixes intact (unchanged int20 gates, all PASS):

- Ops layout: **ops=24 slot=232 ran=236**, single defs, numeric
  ST-OPS/`runexp`/ST-RUN/bails/retire/finish, weak/strong
  run_job_addr linkage live.
- IRQ/completion: `(status | rawstat)` demux, rearm + AS0 program on
  recover/resume, all 7 `p7580_job:` probes in tree + objects + Image.
- Slot routing (2936796): FS → JS0, req0 → JS1, ONLY_COMPUTE → JS2
  in jm_hw.h + `p7580_job: dispatch` in job.o + Image.
- ABI-align (de142b3): BUILD_BUG_ON pins, SYNC_FILE tolerance,
  copy caps, handler symbols in object + System.map.
- gpuwrite (67eaef1): Zero contract, `0xCF` fops route,
  `job_done_sync` reuse, 6-row table, debug symbols live.

Coherency (c207bdd, this ticket's change — 6h gate, PASS):

- Applied tree: 5 `p7580_gem_job_bos_pre_signal_sync` mentions in
  `panfrost7580_job.c` (decl :67 + comment :463 + 3 call sites
  `:474` IRQ DONE `job->bos`, `:610` timedout `job->bos`, `:887`
  recover `fault_job->bos`); `p7580_gem_job_done_sync` +
  `drm_gem_object_unreference_unlocked` walk in applied submit.c
  `p7580_drv_free_job`; `void p7580_gem_job_bos_pre_signal_sync` def
  + `dma_sync_single_for_cpu` in applied gem.c.
- Symbols: `T p7580_gem_job_bos_pre_signal_sync` in gem.o (def),
  `U` in job.o (single undef ref TU); exactly one `T` in System.map
  (`ffffffc000579c80`).
- Objdump (job.o, `aarch64-linux-gnu-objdump -d`): `bl` to
  `<p7580_gem_job_bos_pre_signal_sync>` in all three functions —
  `p7580_job_irq` (@6a4), `p7580_job_timedout` (@124c),
  `p7580_job_recover` (@114c).
- free_job post-signal leg intact: `p7580_gem_job_done_sync` +
  `drm_gem_object_unreference_unlocked` + `job->bos[i] = NULL` walk
  in the applied tree (belt and braces for mmap/CPU reads after the
  free); fault_matrix 13/13 per c207bdd.

## 3. Outputs (sha256)

- `Image-int21`:
  `c69b12393718d8556f7559f897b966ead442d76c3ed335c6bdc8d05e4a7ec324`
- `kernel-boot-int21.img`:
  `6ba5d23a79b0968c8c9e3eebf82dfbd057d0de9c0296a26e84275e9b713cc9df`

Config: pmaports `config-samsung-j7elte.aarch64` base + d0-m1-minimal
+ d4-panfrost7580 fragments, then `olddefconfig` closure. No flash
per ticket (Main flashes).
