# GPU31 last-job fault — descriptor dump, Mesa origin, dump fix

Date: 2026-11-21. Owner: G31_LastJob. Phone: up, int22-era kernel
(`3.10.108 #37`, built Nov 20 22:14; boot sha
`8c60e60421d365f42c43f6ec51fca98cf810a2398151325e189df95569dbd2de`).
Mesa 20.3.5 on device (`/data/mesabuild/mesa20` + `-b`),
`egltest --offscreen-only` (via `/data/eglrun.sh`, 60 draws/frame).

Related: `gpu31-instrtype.md` (excl-fence theory), `gpu31-errtrace.md`
(ERR pins + `dump`), `gpu31-tilerchain.md` (FBD decode),
`gpu31-jobabi.md` (descriptor contract).

## 0. Fault (silicon, reproduced 3x this ticket)

```
p7580_job: ERR slot=0 js_status=0x00000052 (INSTR_TYPE_MISMATCH)
  stat=0x00010000 rawstat=0x00010000 head=0x4f2d600 tail=0x4f2d600
  seq=121 jc=0x4f2d600
p7580_job: TIMEOUT slot=0 js_status=0x52 ... head=0x4f2d600 (same)
```

Deterministic: same `jc`, same `seq` (121 of 122 submits = 61
chain+fragment pairs), same slot 0, every run; code varies across
boots (`0x52`/`0x58`). 60 earlier identical pairs DONE on both slots.
`head==tail==jc` = JM rejected the first fetch. This ticket treats the
excl-fence race as disproven for this fault (structural signature) and
grounds the descriptor bytes instead.

## 1. Descriptor dump (same-fd 0xCF + CPU maps, device shim)

Method: `/tmp/g31last3.so` (device gcc, `-shared -fPIC -ldl`, kept on
phone only) hooks `drmIoctl` (Mesa 20.3 path) + `mmap` (fake-offset →
GPU-IOVA resolve via CREATE_BO track), dumps chain heads after SUBMIT
returns, and issues the 0xCF `DEBUG_READ_BO` readback on the same fd
for every handle. Log `/tmp/g31last.log` (device-only). Header layout
from generated `mesa20-b/src/panfrost/lib/midgard_pack.h`
(`MALI_JOB_HEADER`: `w4 = is_64b|type<<1|...|index<<16`,
`w5 = dep1|dep2<<16`, `w6/7 = next`).

Faulting submit (#122 of 122, `req=1` → JS0):

```
SUBMIT#122 jc=0x4f2d600 req=1 nbos=12 rc=0
  cpu w4=0x00010013 w5=0x00000000 64b=1 type=9(FRAGMENT) idx=1
      dep=0,0 next=0x0
  cpu w0..7=00000000 00000000 00000000 00000000
            00010013 00000000 00000000 00000000
  frag bounds min=(0,0) max=(15,15) fbd=0x4f2d4c0
  frag cpu pl=00000000 000f000f 04f2d4c0 00000000 ...
  fbd cpu[0..7]=00000005 0000001f 05202000 00000000 ...
  fbd kern h=6 off=0x4c0 kv=00000005 0000001f 05202000 00000000 ...
  tiler kern h=6 sz=0x80200 hier=0x0 poly=0x57c2000 body=0x57c4200
             heap=0x4d68000..0x4e68000
```

The paired chain head (#121, `req=0` → JS1):

```
SUBMIT#121 jc=0x4f2d480 req=0 nbos=12 rc=0
  cpu w4=0x00020005 w5=0x00000000 64b=1 type=2(WRITE_VALUE) idx=2
      dep=0,0 next=0x4f2d0c0
```

Findings:

- Faulting bytes are **valid and complete**: `is_64b=1`, `type=9
  (FRAGMENT)`, `index=1`, `dep=0,0`, `next=NULL` — byte-identical to
  all 60 DONE fragments (first pair #1/#2 decoded equal).
- **Kernel view == CPU view**: 0xCF scan finds `w4` in handle 6 and
  the 8-word kernel readback matches the CPU words exactly; FBD
  head and SFBD tiler section (`sz=0x80200`, `hier=0`,
  `heap 1 MiB`) match the `gpu31-tilerchain.md` §2 capture. The
  "kernel page view is zeros" leg from `gpu31-instrtype.md` §1 does
  **not** reproduce here — descriptors are resident and coherent at
  SUBMIT time on this kernel.
- IOVA reuse is confirmed: #121/#122 reuse frame-1 IOVAs
  (`0x4f2d480/0x4f2d600`); strace shows 133 `GEM_CLOSE`s per run, so
  these are recycled pool-BO addresses. But with bytes proven good,
  reuse alone does not explain a deterministic last-only fault.

## 2. Mesa origin of the last submit

`pan_job.c` (`mesa20/src/gallium/drivers/panfrost/`):

- `panfrost_batch_submit_jobs` (l.1015): per batch, chain
  (`scoreboard.first_job`, `req=0` → JS1) is submitted first, then
  **the fragment job is always submitted last**
  (`panfrost_fragment_job(batch, has_tiler)`, `req=PANFROST_JD_REQ_FS`
  → JS0). `pan_fragment_job` stamps `header.type =
  MALI_JOB_TYPE_FRAGMENT, index = 1` and a NULL `next` — exactly the
  dumped words.
- `egltest` does `glClear + glDrawArrays(TRIANGLES)` × 60 draws, then
  `glFinish` → `panfrost_flush` → `panfrost_flush_all_batches` →
  per-batch `panfrost_batch_submit`. 122 submits = 61 batches; the
  faulting submit is the **fragment job of the last batch** (the
  `glFinish` flush; `has_frag = has_tiler || batch->clear` is true,
  `has_draws` true).
- What it should do: run on JS0 like its 60 twins (slot mapping is
  correct, `p7580_job_slot`: FS → JS0). No different slot/cfg/skip
  is indicated — the bytes are a legal FRAGMENT job. The defect is
  driver-side (why the JM rejects this fetch), not Mesa content.

## 3. Fixes (repo-side, this ticket)

### 3a. Debugfs `dump` segfault — root cause + fix

`cat /sys/kernel/debug/p7580/dump` oopsed in
`p7580_job_dump_show+0x68` reading `JOB_INT_STAT` (`x1=0x100c`):
`Unhandled fault: synchronous external abort ... at 0xffffff800027100c`.
The register mapping is fine (`jd->regs` is correct — the IRQ path
reads the same registers every dispatch); the fault is a **dark-bus
read**: the block autosuspends after `PF7580_AUTOSUSPEND_MS`, and a
debugfs read in process context touches JOB_INT/JS registers with no
PM ref held. Fix:

- `code/gpu31/jobs/panfrost7580_job.h`: `struct p7580_job_dev`
  gains opaque `pm_dev` + `pm_get`/`pm_put` hooks (void-arg, header
  stays device/PM-free); `p7580_job_dev_init` takes the three new
  args.
- `code/gpu31/jobs/panfrost7580_job.c`: init stores the hooks;
  `p7580_job_dump_show` takes the PM ref around all MMIO; on dark
  (`get` fails) it prints the `last`-snapshot view with `0xdead0000`
  registers + `dark` marker instead of faulting.
- `code/gpu31/driver/0001-...-driver-sources.patch`: job.c/job.h
  blocks regen'd from canon (`git apply --check` clean; hunk counts
  `1203`/`223` = `wc -l`); probe wires `&pdev->dev` +
  `p7580_job_pm_get/put_void` (device-direct `pm_runtime_get_sync`
  wrappers defined before probe; `pm_runtime.h` already included in
  that TU).

Verification exercised: host `jobs/job_test` PASS (all checks);
`git apply --check` clean on the full 19-block patch; phone-side
`dump` re-read pending Main's int24 flash (acceptance §4).

### 3b. Faulting-submit handling

No Mesa-content fix is warranted (bytes proven good, §1). The
deterministic last-only signature with a valid descriptor and a
resident mapping points at slot/JM state at end-of-batch rather than
bytes — but the exact mechanism (stale JS_HEAD latch vs. AS
tear-down racing the recycled IOVA vs. recovery-path interaction) is
**not isolated**: the phone runs a pre-int23 kernel (boot sha does
not match int22 `2adfa73b` or int23 `d4daba55`; `dmesg` shows the
int22 trace shape), so neither the int23 excl-fence publish nor the
§3a dump fix is live on silicon yet. No speculative driver change
beyond §3a is landed here. Next step for Main: flash int23 (excl
publish) + this ticket, then re-run; if the ERR survives with
identical bytes, the remaining suspects are (1) JS_HEAD/latch
staleness on the recycled IOVA, (2) upscale the fragment submit to
carry an explicit dep on the last chain fence.

## 4. Verification status (honest)

- `egltest` full run on the current phone kernel: **NOT 0 ERR** —
  `ERR slot=0 js_status=0x52 ... seq=121` + TIMEOUT, `corner` OK,
  `centre` clear (`RENDER_MISMATCH`). This matches the pre-fix
  kernel, not a failure of the §3a fix (which is not flashed).
- Exercised: descriptor dump (CPU == kernel, §1), host `job_test`
  PASS, patch `--check` clean. NOT exercised: on-silicon `dump`
  read, int23+§3a flash, 0-ERR egltest run.
- Required Main actions: rebuild int24 (this ticket + int23),
  flash, `dmesg -c`, `/data/eglrun.sh`, expect zero `ERR` lines;
  `cat /sys/kernel/debug/p7580/dump` must print the `last ...` view
  (or `... dark`) with no oops.

Device-only artefacts (not committed, stay on phone): `/tmp/g31last*.c`,
`/tmp/g31last*.so`, `/tmp/g31last.log`, `/tmp/egl.ioctl`.
