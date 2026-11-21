# GPU31 errtrace — always-on ERR diagnostics + re-armable trace budget + debugfs dump

Date: 2026-11-21. Owner: G31_ErrTrace. Ticket: job ERR path + trace budget.
Phone on int21 (per G31_PrimitiveDebug: slot-1 ERR `0x00020000` on raw
VERTEX+TILER, slot-0 ERR `0x00010000` on raw fragment, budget exhausted).

## 1. Problem

The int20/int21 trace budget (first 8 job-events, `trace_left`) is long
exhausted by Mesa startup by the time any faulting submit runs. The two
register reads that decide the fault — `JS_STATUS` (CONFIG vs
`TILE_RANGE` vs `ADDRESS_RANGE` vs `OUT_OF_MEMORY`, `jm_hw.h:159-190`)
and `JS_HEAD/TAIL` — were only printed inside budget-gated lines, so a
fault after event 8 left zero register evidence in the ring. Phone-side
`0xCF DEBUG_READ_BO` reads BO bytes, not registers.

## 2. What this ticket changes (canon + regen)

| File (canon) | Change |
|---|---|
| `code/gpu31/jobs/panfrost7580_job.h` | `struct p7580_job_last` (slot/seq/jc/cfg/as/affinity snapshot); `trace_arm` + `last` + `dbg_dir` on `struct p7580_job_dev`; `p7580_job_trace_arm()` + `p7580_job_dump_show()` decls |
| `code/gpu31/jobs/panfrost7580_job.c` | always-on `pr_err` ERR/TIMEOUT pins (§3); per-dispatch `last` snapshot; `DEFINE_SIMPLE_ATTRIBUTE` trace_arm + `single_open` dump debugfs glue (§4); init/fini create/remove |
| `code/gpu31/driver/0001-panfrost7580-driver-sources.patch` | regen of the two job blocks only (`@@ -0,0 +1,894` → `+1,1176`, `+1,182` → `+1,213`); all other 17 blocks byte-identical; `git apply --check` clean; applied `job.c`/`job.h` byte-equal to canon |
| `docs/reports/gpu31-errtrace.md` | this file |

No normal-IO-path behaviour changes: dispatch/post-start/IRQ-DONE/
fence-signal lines keep the exact old budget gate; DONE fast path adds
only register reads already done on that path plus a conditional
`pr_err` that fires solely on `js_status >= 0x40`.

## 3. Always-on ERR format (never budget-limited)

New helper (prints regardless of `trace_left`):

```c
pr_err("p7580_job: ERR slot=%d js_status=0x%08x (%s) stat=0x%08x rawstat=0x%08x head=0x%llx tail=0x%llx seq=%u jc=0x%llx\n",
	slot, js_status, p7580_js_status_name(js_status),
	status, rawstat, head, tail, seqno, jc);
```

Emission points:

- IRQ ERR-without-DONE leg: unconditional pin per faulting slot (plus
  the existing budget-gated `irq-err` info line when budget remains).
- IRQ DONE leg with `p7580_js_status_is_fault(js_status)`: pins even
  when DONE is also set (sticky fault bit on a short chain).
- `p7580_job_timedout` (no IRQ at all):

```c
pr_err("p7580_job: TIMEOUT slot=%d js_status=0x%08x (%s) stat=0x%08x rawstat=0x%08x head=0x%llx tail=0x%llx seq=%u jc=0x%llx\n",
	slot, status, p7580_js_status_name(status),
	t_stat, t_raw, head, tail, job->seqno, job->jc);
```

Expected dmesg on the current int21 fault shape (slot-1 raw TILER):

```
p7580_job: ERR slot=1 js_status=0x00000059 (TILE_RANGE_FAULT) stat=0x00020000 rawstat=0x00020000 head=0x<jc> tail=0x<jc> seq=12 jc=0x<gpu_addr>
```

Decision table (extends jobtrace §4):

| ERR line | meaning | next owner |
|---|---|---|
| `js_status=0x59 TILE_RANGE_FAULT` | tiler heap / polygon-list range wrong | heap sizing (Mesa batch) |
| `js_status=0x60 OUT_OF_MEMORY` | tiler ran out of heap mid-frame | heap sizing |
| `js_status=0x50-0x56 INSTR_*` | shader binary / TLS / alignment | shader-state pointers |
| `js_status=0x42/0x43 ADDRESS/READ/WRITE_FAULT` | descriptor VA not resident | MMU residency |
| `js_status=0x40/0x44 CONFIG/AFFINITY` | slot programming wrong | kernel slot programming |
| `TIMEOUT` + `js_status=0x0`, no IRQ | slot never executed (clock/power/IRQ mask/MMU) | power/MMU/IRQ |

## 4. Debugfs knobs (arm at any time, no rebuild)

Created at `p7580_job_dev_init`, removed at `p7580_job_dev_fini`
(tolerates `CONFIG_DEBUG_FS=n`: `IS_ERR_OR_NULL` → driver continues):

```
# re-arm the bounded trace for the next N job-events (also readable):
echo 32 > /sys/kernel/debug/p7580/trace_arm
cat /sys/kernel/debug/p7580/trace_arm
# last dispatch + live slot registers + JOB_INT views:
cat /sys/kernel/debug/p7580/dump
```

`trace_arm` write path: `p7580_job_trace_arm()` copies N into both
`trace_arm` and `trace_left` under `hw_lock`, then
`pr_info("p7580_job: trace re-armed to %u\n")`. `dump` output shape:

```
last slot=1 seq=12 jc=0x<gpu> cfg=0x00081301 as=0 aff=0x3 (trace_arm=32 trace_left=27)
job_int stat=0x00020000 rawstat=0x00020000 mask=0x00070007 js_present=0x7 pgd=0x<as0root>
slot0 status=0x00000000 (NO_EXCEPTION) head=0x... tail=0x...
slot1 status=0x00000059 (TILE_RANGE_FAULT) head=0x... tail=0x...
slot2 status=0x00000000 (NO_EXCEPTION) head=0x... tail=0x...
```

`dump` shows `no job yet (trace_arm=N trace_left=M ...)` before the
first dispatch. Register reads are plain MMIO (same discipline as the
IRQ handler); `last` copy holds `hw_lock` only for the struct copy.

Note on scope (per ticket): the ticket asks for "polygon-list/FBD
addresses" in dump. The kernel never sees those addresses as registers
— they live inside the GPU-side job chain the `jc` points at. What dump
does provide is the `jc` (chain root) plus per-slot HEAD/TAIL (how far
the JM walked the chain), which is the register-visible projection of
that state; the polygon-list/FBD words themselves remain readable via
the existing `0xCF DEBUG_READ_BO` route once their BOs are identified.

## 5. Verification (exercised)

- Host: `jobs/job_test` PASS (74 CHECKs), `gem/gem_validate` PASS,
  `driver/submit_fix_test` PASS (unchanged scope).
- Kernel syntax: `aarch64-linux-gnu-gcc -fsyntax-only -Wall -Werror
  -Wno-unused-but-set-variable -std=gnu89` vs real 3.10.108 headers +
  staged 0001–0004 shims: `panfrost7580_job.c` RC=0
  (`-DKBUILD_MODNAME='"panfrost7580_job"' -I. -I../iommu -I../gem
  -I../power`, same KINC shape as the slice Makefiles).
- Kernel object: same flags `-c` → `/tmp/p7580job.o` (30672 B), RC=0.
- Patch hygiene: regen touched ONLY the two job blocks (hunk counts
  `+1,1176` / `+1,213`, other 17 blocks byte-identical); full-file
  `git apply --check` clean; applied-in-tree `job.c`/`job.h`
  byte-identical to canon (script-compared).
- NOT verified (phone-gated, Main owns flash): int22 build, flash,
  `dmesg | grep 'p7580_job: ERR'`, `trace_arm`/`dump` reads on device.

## 6. Rerun for Main / next online agent (int22)

```sh
# phone on RNDIS, int22 flashed (this ticket + int21):
ssh user@172.16.42.1 'sudo -n sh -c "dmesg -c >/dev/null"'
# re-arm BEFORE the faulting run (budget otherwise long gone):
ssh user@172.16.42.1 'sudo -n sh -c "echo 64 > /sys/kernel/debug/p7580/trace_arm"'
ssh user@172.16.42.1 'sudo -n sh -c "flock /tmp/gpsdev.lock /data/egltest --offscreen-only --frames 1"'
ssh user@172.16.42.1 'sudo -n dmesg | grep -E "p7580_job: (ERR|TIMEOUT|dispatch|irq|fence)" | head -n 40'
ssh user@172.16.42.1 'sudo -n cat /sys/kernel/debug/p7580/dump'
# record: the ERR js_status code (§3 table) + dump HEAD/TAIL per slot
```

Close criteria: every faulting submit leaves exactly one `ERR` (or
`TIMEOUT`) line with a non-zero `js_status` name + HEAD/TAIL, and
`dump` shows the same slot state — decidable without any budget.

## 7. Integrator notes

- Apply order unchanged (0001–0005 → Mode-B → dmabuf → driver 0001+0002
  → fragments). Only driver-0001 job blocks changed.
- New header fields are zero-init safe (`kzalloc`'d `jd` gets
  `last.valid=0`, `dbg_dir=NULL` explicitly too).
- `linux/debugfs.h`, `linux/seq_file.h`, `linux/uaccess.h` are the only
  new includes; `DEFINE_SIMPLE_ATTRIBUTE` + `single_open`/`seq_read`/
  `seq_lseek`/`single_release` + `debugfs_create_dir/file` +
  `debugfs_remove_recursive` all verified present in the 3.10.108 tree
  (`CONFIG_DEBUG_FS=y` in defconfig).
- IRQ-safety preserved: ERR pins run under the same `hw_lock` the
  gated prints already held; `pr_err` in IRQ context matches the
  existing bounded-trace practice (printk, no sleep).
