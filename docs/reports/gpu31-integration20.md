# gpu31 integration20 — slot routing corrected (req0 → JS1), host suites PASS

Date: 2026-11-21. Owner: G31_Integrator20. Phone: up on int19;
this ticket builds only — Main flashes.

Consumes 2936796 (mesa-render: `p7580_job_slot` matches mainline v5.2
`panfrost_job_get_slot` — FS → JS0, everything else incl. req==0 chain
→ JS1, ONLY_COMPUTE → JS2; job_test + 0001 surgically updated) on top
of 67eaef1 (gpuwrite) + e70769b (coherency, int18). No source changes
beyond the int20 script (copy of int19 + renamed names + slot gates)
and this report.

## 1. Build

Script: `code/gpu31/series/build-int20.sh` (committed). Fresh `cp -a`
of the pristine tree → `scratch-int20`, all 9 `--check` green
(backport 0001–0005 ×5, Mode-B ×1, dmabuf ×1, driver 0001+0002), then
`-j4` Image build, RC=0. One script defect surfaced by the build itself
and fixed before the green run: the new 6g stale-string check
`'requirements == 0'` matched the prose comment on jm_hw.h:111; the
committed script uses the paren-anchored `'(requirements == 0)'`
(code pattern, absent — verified `grep -c = 0`). 6g was replayed PASS
in the same kept scratch before packing (see §2).

Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
KCFLAGS="-Wno-error" -j4 Image`.
Log: `…/j7k/gpu31/logs/gpu31-int20-build.log` (5393 lines, tail
`OBJCOPY arch/arm64/boot/Image`; zero `No such file` lines; only
`error`-grep hits are the benign ext4 `strncpy`/`__save_error_info`
+ `scsi_error.o` filename + one `KERN_ERR` macro line — none in
panfrost/drm paths).
Pack: stock dt.img + reference ramdisk-new.gz (never rebuilt),
`console=tty0 loglevel=7` cmdline, 27299840 B =
kernel 18160760 + ramdisk 8822392 + dt 313344.
Console: `…/logs/gpu31-int20-console.log` (RC=0, all gates PASS);
evidence: `…/logs/gpu31-int20-evidence.log`.
Scratch + stage + offcheck one-shots deleted after the build; kept:
`Image-int20`, `kernel-boot-int20.img`, both logs.

## 2. Gate evidence (computed inside the build, not inferred)

Objects (before deletion, aarch64, `--time-style=full-iso`):

- `drivers/gpu/drm/drm_sched.o` (121912 B, 2026-11-20 20:01:44)
- `drivers/gpu/drm/panfrost/panfrost7580_submit.o` (224784 B, 20:01:51)
- `drivers/gpu/drm/panfrost/panfrost7580_job.o` (120192 B, 20:01:52)
- `arch/arm64/boot/Image` (18160760 B, 2026-11-20 20:03:31)

Prior fixes intact (unchanged int19 gates, all PASS):

- Single definitions: `drm_sched_job_alloc` T ×1 in sched.o /
  System.map; `p7580_drv_run_job` t ×1 in submit.o / System.map;
  `p7580_drv_sched_ops` r ×1 in submit.o / System.map;
  `__ksymtab_drm_sched_job_alloc` r present.
- True in-tree layout via kbuild include chain: **ops=24 slot=232
  ran=236** (all three compares passed).
- Compiler immediates: `stp x19, x20, [x4, #16]` (entity+ops);
  stamped-slot `ldr/str w12, [x6, #4]` + `cmp #0x2` range check in
  run_job; slot/ran BUILD_BUG_ON pins + alloc round-trip BUG_ON +
  ST-OPS numeric/`runexp`/`ST-RUN`/4×run_bail/`run_fallback`/ST-RETIRE
  ran+owner/ST-FINISH owner live in applied tree + objects + Image.
- IRQ/completion (d852459 minus routing): `(status | rawstat)` demux,
  `p7580_job_dev_rearm` decl/def/use (recover + main.c resume),
  `p7580_mmu_program_as` on both recover/resume paths, `P7580_JS_STATUS`
  reads, all 7 `p7580_job:` probes in applied tree + job.o + Image.
- ABI-align (de142b3): 16 B/32 B/HTOF/FTOH-bit/16 B/40 B BUILD_BUG_ON
  pins, SYNC_FILE tolerance, copy-in truncate / copy-back cap, handler
  symbols in object + System.map.
- gpuwrite (67eaef1): Zero-only contract, `0xCF` fops route,
  `job_done_sync` reuse, 6-row ioctl table, debug symbols in gem.o +
  ioctl.o + System.map.

Slot routing (2936796, this ticket's change):

- Applied `panfrost7580_jm_hw.h` in scratch:
  `if (requirements & P7580_JD_REQ_FS) return P7580_SLOT_FRAGMENT;`
  `if (requirements & P7580_JD_REQ_ONLY_COMPUTE) return P7580_SLOT_COMPUTE;`
  `return P7580_SLOT_VERTEX_TILER;` — i.e. **req==0 → JS1** (tail
  return), FS → JS0, ONLY_COMPUTE → JS2. No `if (requirements == 0)`
  code pattern (comment prose on :111 only).
- `p7580_job: dispatch` probe present in job.o and in the linked
  Image (slot evidence path); Image carries 7 `p7580_job:` strings
  (1 dispatch + 4 irq + 2 other), ST-OPS/ST-RETIRE numeric strings.
- Host `job_test` (pure-C, `cc -Wall -Wextra -std=gnu89`): all checks
  passed, incl. `p7580_job_slot(FS)==0`, `p7580_job_slot(0)==1`,
  `p7580_job_slot(ONLY_COMPUTE)==2` (pre-verified before the build;
  2936796 also records client mock + gem_validate PASS).

## 3. Outputs (sha256)

- `Image-int20`:
  `4b0f1e0761cff135d18e69cf166e18cd2f898afc5c878e6c5d8554f63c539b7f`
- `kernel-boot-int20.img`:
  `295ff9b0d9e3e0ec14bb2e672327d30337d89261cc797bc9bc1184f7a6caa6b8`

Config: pmaports `config-samsung-j7elte.aarch64` base + d0-m1-minimal
+ d4-panfrost7580 fragments, then `olddefconfig` closure. No flash
per ticket (Main flashes).
