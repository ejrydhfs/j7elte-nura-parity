# gpu31 integration22 — errtrace unconditional fault diagnostics, all prior fixes intact

Date: 2026-11-21. Owner: G31_Integrator22. Phone: up on int21;
this ticket builds only — Main flashes.

Consumes 7a5bf9b (errtrace: unconditional `p7580_job: ERR
slot=… js_status=… stat=… head/tail/jc` diagnostics for IRQ-ERR,
DONE-with-fault and TIMEOUT; debugfs knobs
`/sys/kernel/debug/p7580/trace_arm` and `/sys/kernel/debug/p7580/dump`)
on top of ac82b45 (int21) + c207bdd (coherency pre-signal) + 2936796
(slotfix req0→JS1) + 67eaef1 (gpuwrite). No source changes beyond the
int22 script (copy of int21 + renamed names + 6i errtrace gate) and
this report.

## 1. Build

Script: `code/gpu31/series/build-int22.sh` (committed). Fresh `cp -a`
of the pristine tree → `scratch-int22`, all 9 `--check` green
(backport 0001–0005 ×5, Mode-B ×1, dmabuf ×1, driver 0001+0002), then
`-j4` Image build, RC=0. First run green, no script defects.

Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
KCFLAGS="-Wno-error" -j4 Image`.
Log: `…/j7k/gpu31/logs/gpu31-int22-build.log` (tail
`OBJCOPY arch/arm64/boot/Image`).
Pack: stock dt.img + reference ramdisk-new.gz (never rebuilt),
`console=tty0 loglevel=7` cmdline, 27316224 B =
kernel 18177144 + ramdisk 8822392 + dt 313344.
Console: build stdout (RC=0, all gates PASS);
evidence: `…/logs/gpu31-int22-evidence.log`.
Scratch + stage deleted after the build; kept:
`Image-int22`, `kernel-boot-int22.img`, both logs, `offcheck22.s`.

## 2. Gate evidence (computed inside the build, not inferred)

Objects (before deletion, aarch64, `--time-style=full-iso`):

- `drivers/gpu/drm/drm_sched.o` (121912 B, 2026-11-20 21:27:31)
- `drivers/gpu/drm/panfrost/panfrost7580_submit.o` (225192 B, 21:27:39)
- `drivers/gpu/drm/panfrost/panfrost7580_gem.o` (252872 B, 21:27:40)
- `drivers/gpu/drm/panfrost/panfrost7580_ioctl.o` (173000 B, 21:27:40)
- `drivers/gpu/drm/panfrost/panfrost7580_syncobj.o` (220096 B, 21:27:42)
- `drivers/gpu/drm/panfrost/panfrost7580_job.o` (196448 B, 2026-11-20 21:27:40)
- `arch/arm64/boot/Image` (18177144 B, 2026-11-20 21:29:12)

Prior fixes intact (unchanged int21 gates, all PASS):

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
- Coherency (c207bdd): 3 pre_signal call sites (IRQ `job->bos`,
  timedout `job->bos`, recover `fault_job->bos`), `T` in gem.o,
  single `U` in job.o, `bl` in all three fns
  (`p7580_job_irq` @984, `p7580_job_timedout` @173c,
  `p7580_job_recover` @152c), one `T` in System.map
  (`ffffffc000579c80`).

Errtrace (7a5bf9b, this ticket's change — 6i gate, PASS):

- Applied tree: `p7580_job: ERR slot=%d js_status=0x%08x (%s)
  stat=0x%08x rawstat=0x%08x head=0x%llx tail=0x%llx seq=%u jc=0x%llx`
  at job.c :121 (`p7580_job_err_pin` def); `p7580_job: TIMEOUT
  slot=%d …` (same pin set) at :720; `debugfs_create_dir` +
  `"trace_arm"` + `"dump"` knob creation present.
- job.o strings: ERR ×3, TIMEOUT ×3, `trace_arm`, `dump`,
  `p7580_job_trace_arm` (param), `t p7580_job_err_pin` static in nm.
- Image strings: `p7580_job: ERR` ×1, TIMEOUT present, `trace_arm`
  present; `Image p7580_job: count = 12, dispatch = 1, irq = 4`.
- Unconditional discipline: ERR/TIMEOUT print via `pr_err` outside
  the `trace_left` budget; normal-path lines keep the old gate.

## 3. Outputs (sha256)

- `Image-int22`:
  `44511c6a37e6d69d63dd3c6448a0067f6cf82abecab7442300eda4c8c01f2b95`
- `kernel-boot-int22.img`:
  `2adfa73b43bb1ffb895e80c733dd1d8564658846bb3421ec9a92156160b1b21c`

Config: pmaports `config-samsung-j7elte.aarch64` base + d0-m1-minimal
+ d4-panfrost7580 fragments, then `olddefconfig` closure. No flash
per ticket (Main flashes).
