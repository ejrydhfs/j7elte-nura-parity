# Stock Mali kbase reference: JM job start, IRQ masks, and what `js_status=0x08` means

Read-only reverse engineering of the stock Samsung firmware for this exact
phone (`SAMFW.COM_SM-J700M_PCW_J700MUBU2BRC2_fac.zip`) against driver 0001.
The archive was only read; all extracted data lives outside the repo.

```
stock kernel : Linux 3.10.61-13213628 (dpi@SWDG9701), gcc 4.9.x, #1 SMP PREEMPT
GPU driver   : ARM Mali kbase t72x/r7p0, built into the kernel Image
               (drivers/gpu/arm/t72x/r7p0/... path strings are in the Image;
                no mali.ko is needed and none was extracted)
image        : raw, uncompressed arm64 Image, 15,990,784 bytes, _text 0xffffffc000205000
reference src: android.googlesource.com/kernel/mediatek android-mtk-3.18
               drivers/misc/mediatek/gpu/gpu_mali/mali_midgard/mali-r7p0
               (the r7p0 tree matches the stock binary function-for-function:
                every write below was read out of the Image, the source is
                cited only to name the values and the surrounding checks)
```

## 0. Verdict

1. **Our job-start sequence is write-for-write identical to the stock
   `kbase_job_hw_submit()`.** Same six MMIO writes, same order, same
   `JS_CONFIG_NEXT` value (`0x00083700 | as_nr`), same `START` bit, and the
   same barrier discipline (`writel()` on arm64 emits `dsb st` before the
   store, exactly like `kbase_reg_write()`). There is **no missing start
   write**. The only extra the stock code can emit, `JS_CONFIG_NEXT` bit 11
   (`JOB_CHAIN_FLAG`), is gated on
   `BASE_HW_FEATURE_JOBCHAIN_DISAMBIGUATION`, which the t72x feature table
   does not contain — that branch is dead code on this SoC.
2. **`js_status=0x08` is not "NO_EXCEPTION" and not "job not started".** The
   stock kbase names 0x08 **`ACTIVE`** (`BASE_JD_EVENT_ACTIVE`, "jobs only
   returned on complete/fail/cancel"; 0x00 = `NOT_STARTED/IDLE/OK`,
   0x01 = `DONE`). Our decoder table only contains fault codes, so the value
   falls through to `"NO_EXCEPTION"` — that is where the "silent start
   timeout / slot idle" reading in earlier reports came from. `head==tail`
   with `0x08` means the JM accepted the START (COMMAND_NEXT drained), the
   descriptor chain is at its end, and the slot **still considers the job
   running**: the job never retired.
3. The only *job-start/IRQ-path preconditions* the stock driver programs and
   we do not are **interrupt masks** and **post-reset engine config**:
   * `MMU_IRQ_CLEAR`/`MMU_IRQ_MASK = 0xFFFFFFFF` after every reset — **we
     never write `MMU_IRQ_MASK` (0x2008) at all**. A GPU page/bus fault is
     therefore completely silent: its code lands in `AS0_FAULTSTATUS`
     (0x241C), *not* in `JS_STATUS`, and the job slot is not retired until the
     driver kills it. That reproduces every field of the failure
     (`js_status=0x08`, `stat=rawstat=0`, `head==tail`, no error).
   * `GPU_IRQ_MASK = GPU_IRQ_REG_ALL (0x10581)`; we run with `GPU_IRQ_MASK=0`
     outside reset, hiding `GPU_FAULT`/`MULTIPLE_GPU_FAULTS`.
   * `SHADER_CONFIG (0xF04) = 0x02000000` (`SC_ENABLE_TEXGRD_FLAGS`) after
     every reset; t72x r1p0 carries `BASE_HW_ISSUE_10797`, and we never write
     this register.
4. Minimal patch to try (one block, §10): clear+unmask the MMU block in the
   job init/rearm path and print `AS0_FAULTSTATUS`/`FAULTADDRESS`/`MMU_IRQ_RAWSTAT`
   at timeout. If those registers are non-zero at the next timeout, the 0x08
   timeouts are silent GPU page faults. If they are zero, the next candidate
   is the `SHADER_CONFIG` quirk (also in §10).

## 1. Extraction

Helper: `code/gpu31/re/stock-mali/extract-stock-kbase.sh`. The zip holds a
3.3 GB Odin tar; only `boot.img` is needed.

```sh
$ unzip -l SAMFW.COM_SM-J700M_PCW_J700MUBU2BRC2_fac.zip
 3299727540  AP_J700MUBU2BRC2_CL13213628_QB17312148_REV00_user_low_ship.tar.md5
    4556977  BL_...
   34898017  CP_...
   94126263  CSC_...
$ unzip -p ...zip 'AP_...tar.md5' > AP.tar.md5
$ tar -tvf AP.tar.md5
 -rw-rw-r-- 19112224 boot.img
 -rw-rw-r-- 20293920 recovery.img
 -rw-rw-r-- 1886944320 system.img
 -rw-rw-r-- 1373364528 userdata.img
$ tar -xf AP.tar.md5 boot.img
```

`unpack_bootimg` rejects this image (Samsung's v0 header carries a non-UTF-8
name), so the header is parsed directly. Layout at 0x00: `ANDROID!`,
kernel_size u32@0x08 = 0x00f3f4a8, page_size u32@0x24 = 0x800. Kernel starts
at page 1:

```sh
$ dd if=boot.img of=kernel.bin bs=2048 skip=1 count=7808
$ xxd -l 0x40 kernel.bin
00000000: 1000 0014 0000 0000 0050 2000 0000 0000   <- b +0x40, text_offset 0x205000
00000038: 4152 4d64 ...                             <- "ARMd": raw arm64 Image
```

* not gzip (`1f 8b`) and not lz4 (`02 21 4c 18`) — nothing to decompress;
* `text_offset` = 0x205000 = `kallsyms _text` = `0xffffffc000205000`, so
  **file offset = VA − 0xffffffc000205000** (cross-checked: the
  `"kbase_gpu_dump_slots:"` format string is referenced by code as
  0xffffffc000b9409b and sits at file offset 0x98f09b).

Symbols: the Image is stripped and Samsung shipped no `System.map`, but
kallsyms is embedded. Recovered with `kallsyms-finder` (from the
`vmlinux-to-elf` package):

```sh
$ kallsyms-finder kernel.bin > kallsyms.txt      # 56,883 symbols
$ head -1 kallsyms.txt
ffffffc000205000 T _text
```

Relevant symbols are committed in `code/gpu31/re/stock-mali/kbase-symbols.txt`.
No `mali.ko` exists in the boot path and none was needed: the kbase
`drivers/gpu/arm/t72x/r7p0/...` file-name strings are inside the Image.

Disassemble:

```sh
code/gpu31/re/stock-mali/dis-kbase.sh kernel.bin --sym kbase_job_hw_submit
code/gpu31/re/stock-mali/dis-kbase.sh kernel.bin --sym kbase_job_done
code/gpu31/re/stock-mali/dis-kbase.sh kernel.bin 0xffffffc0004eff5c 0xffffffc0004f010c
```

### 1.1 Function addresses (stock Image)

| VA | symbol | role |
|---|---|---|
| 0xffffffc0004eff5c | `kbase_job_hw_submit` | the entire job-slot START sequence |
| 0xffffffc0004f1980 | call site inside `kbase_gpu_slot_update` (0xffffffc0004f14bc) | cores requested, then submit |
| 0xffffffc0004ee120 | `kbase_job_irq_handler` | reads `JOB_IRQ_STATUS` 0x100c → `kbase_job_done` |
| 0xffffffc0004f010c | `kbase_job_done` | throttle write, `JOB_IRQ_CLEAR`, `JOB_IRQ_JS_STATE` |
| 0xffffffc0004ed838 | `kbase_reg_write` | `dsb st; str` (one barrier per register write) |
| 0xffffffc0004ed880 | `kbase_reg_read` | `ldr; dsb ld` |
| 0xffffffc0004f4774 | `kbase_pm_enable_interrupts` | GPU/JOB/MMU CLEAR+MASK after every reset |
| 0xffffffc0004f48f8 | `kbase_pm_disable_interrupts` | masks all (power-off) |
| 0xffffffc0004f4b60 | `kbase_pm_init_hw` | reset → engine config → enable IRQs |
| 0xffffffc0004f4fdc | `kbase_pm_clock_on` | per-AS MMU reprogram → enable IRQs |
| 0xffffffc0004f32a0 | `kbase_mmu_hw_configure` | AS TRANSTAB/MEMATTR → AS_COMMAND_UPDATE |
| 0xffffffc0004f2f80 | `wait_ready` | polls `AS_STATUS` bit0 before each AS command |
| 0xffffffc0004f3090 | `kbase_mmu_interrupt` | masks MMU IRQs, demuxes per-AS fault |
| 0xffffffc0004d947c | `kbase_mmu_report_fault_and_kill` | **kills the faulting context's slots** |
| 0xffffffc0004daa74 | `kbase_exception_name` | status/exception name table (0xffffffc0009ba710) |
| 0xffffffc0004ed8d4 | `kbase_gpu_interrupt` | GPU_FAULT (bit0), MULTIPLE (bit7), reset/Power/cache bits |

### 1.2 Stock register map (read out of the Image, not assumed)

The stock `kbase_debug_dump_registers()` prints register names and offsets in
one format string at 0xffffffc000b94000+0xb8f:

```
JOB_IRQ_RAWSTAT=0x%08x JOB_IRQ_JS_STATE=0x%08x JOB_IRQ_THROTTLE=0x%08x
```

| reg | offset | note (stock wording / source) |
|---|---|---|
| GPU_IRQ_RAWSTAT/CLEAR/MASK | 0x020/0x024/0x028 | `GPU_IRQ_REG_ALL` = 0x10581 |
| GPU_FAULTSTATUS / FAULTADDRESS | 0x03C / 0x040,0x044 | read by `kbase_gpu_interrupt` on bit0 |
| GPU_COMMAND | 0x030 | 1 = SOFT_RESET, 2 = HARD_RESET, 8 = CLEAN_INV_CACHES |
| **GPU engine config** | **0x0F00..0x0F0C** | JM_CONFIG / SHADER_CONFIG / TILER_CONFIG / L2_MMU_CONFIG |
| JOB_IRQ_RAWSTAT / CLEAR / MASK / STATUS | 0x1000 / 0x1004 / 0x1008 / 0x100C | bits 0..15 done, 16..31 error (per slot) |
| **JOB_IRQ_JS_STATE** | **0x1010** | "status==active and _next==busy snapshot from last JOB_IRQ_CLEAR" |
| **JOB_IRQ_THROTTLE** | **0x1014** | "cycles to delay delivering an interrupt externally. The JOB_IRQ_STATUS is NOT affected by this, just the delivery" |
| JS_HEAD/TAIL (RO) | 0x1800+n*0x80 + 0x00/0x08 | |
| JS_CONFIG (RO) / JS_COMMAND / JS_STATUS | +0x18 / +0x20 / +0x24 | STATUS 0x08 = ACTIVE (§8) |
| JS_HEAD_NEXT / JS_AFFINITY_NEXT / JS_CONFIG_NEXT / JS_COMMAND_NEXT | +0x40/44, +0x50/54, +0x58, +0x60 | |
| MMU_IRQ_RAWSTAT/CLEAR/MASK/STATUS | 0x2000/0x2004/0x2008/0x200C | bit n = page fault ASn, bit n+16 = bus error ASn |
| ASn block | 0x2400+n*0x40 | TRANSTAB 0x00/04, MEMATTR 0x08/0C, LOCKADDR 0x10/14, COMMAND 0x18, FAULTSTATUS 0x1C, FAULTADDRESS 0x20/24, STATUS 0x28 |

`JS_STATUS` code table (from `kbase_exception_name`, reproduced with
`dump-js-status-table.py`):

```
0x00 NOT_STARTED/IDLE/OK   0x01 DONE      0x02 INTERRUPTED   0x03 STOPPED
0x04 TERMINATED            0x08 ACTIVE
0x40 JOB_CONFIG_FAULT  0x41 JOB_POWER_FAULT  0x42 JOB_READ_FAULT  0x43 JOB_WRITE_FAULT
0x44 JOB_AFFINITY_FAULT 0x48 JOB_BUS_FAULT
0x50..0x56 INSTR_*  0x58 DATA_INVALID_FAULT  0x59 TILE_RANGE_FAULT  0x5a ADDR_RANGE_FAULT
0x60 OUT_OF_MEMORY  0x80 DELAYED_BUS_FAULT  0x88 SHAREABILITY_FAULT
0xc0 TRANSLATION_FAULT  0xc8 PERMISSION_FAULT  0xd0 TRANSTAB_BUS_FAULT  0xd8 ACCESS_FLAG
```

## 2. Stock JM job-start sequence (verbatim)

`kbase_job_hw_submit(kbdev, katom, js)` is 0x1b0 bytes. For slot `js`
(all addresses absolute; slot stride 0x80, slot0 at 0x1800):

| # | write | value | stock instruction |
|---|---|---|---|
| 0 | — (checks only) | `COMMAND_NEXT` must read 0; `kbase_js_debug_log_current_affinities`; affinity-violation assert (debug builds) | 0x4eff90 |
| 1 | `JS_HEAD_NEXT_LO` (0x1840) | `jc & 0xffffffff` | `bl kbase_reg_write` @0x4effa4 |
| 2 | `JS_HEAD_NEXT_HI` (0x1844) | `jc >> 32` | @0x4effb8 |
| 3 | `JS_AFFINITY_NEXT_LO` (0x1850) | `affinity & 0xffffffff` | @0x4effcc |
| 4 | `JS_AFFINITY_NEXT_HI` (0x1854) | `affinity >> 32` | @0x4effe0 |
| 5 | `JS_CONFIG_NEXT` (0x1858) | `cfg` (below) | @0x4f0054 (+branch @0x4efffc…0x4f0044) |
| — | (no MMIO: `ktime_get`, systrace/timeline) | | 0x4f0058…0x4f00d8 |
| 6 | `JS_COMMAND_NEXT` (0x1860) | **0x00000001 = START** (last write) | @0x4f00ec |

`cfg` = `(as_nr & 0xf) | 0x00083700` where 0x83700 is
`3<<8 | 1<<10 | 3<<12 | 8<<16`
(START flush clean+invalidate, START_MMU, END flush clean+invalidate,
thread priority 8). The stock code can OR bit 11 (`JOB_CHAIN_FLAG`, making
0x83f00) **only** when
`kbase_hw_has_feature(JOBCHAIN_DISAMBIGUATION)` is true; the t72x feature
list (`mali_base_hwconfig_features.h:73-84`) does not have that feature, so on
this SoC the branch never fires.

Before submit, `kbase_gpu_slot_update` (0x4f14bc) does two software things per
atom and one per-job PM request:

* `kbase_js_choose_affinity()` → `katom->affinity` (0x4f1978 region);
* `kbase_pm_request_cores(kbdev, tiler_required, affinity)` (called at
  0x4f1614) — records the atom's cores as *in use* and pushes a PM transition;
  this is what tells the PM policy not to power those cores down while the job
  runs. It does not wait: a job may START while cores are still transitioning
  on (hardware queues it);
* `kbase_pm_register_inuse_cores()` (0x4f163c) after choosing affinity.

`kbase_reg_write` itself is `dsb st` **before** every store
(0xffffffc0004ed838), so each of the six writes is ordered to the device.
On arm64 Linux `writel()` is exactly that (`__iowmb(); __raw_writel()`), so
our `p7580_js_write()` has identical semantics.

## 3. Vendor vs ours — job start

Ours: `p7580_job_hw_submit()` in
`code/gpu31/driver/0001-panfrost7580-driver-sources.patch:~3781-3885`,
`p7580_js_config()` at ~2905-2922, writes at ~3828/3831/3845/3873.

| step | stock kbase | ours | verdict |
|---|---|---|---|
| COMMAND_NEXT free check | `KBASE_DEBUG_ASSERT(... == 0)` (debug only) | `WARN_ON(...)` + bail (production) | ours stricter |
| HEAD_NEXT_LO/HI | jc lo/hi | jc lo/hi | identical |
| AFFINITY_NEXT_LO/HI | atom affinity | `job->affinity` | identical |
| CONFIG_NEXT | `as_nr | 0x83700` (+0x800 dead on t72x) | `as_nr | 0x83700` | identical |
| barrier | `dsb st` per write | `writel()` = `dsb st` per write, plus an explicit `wmb()` before START | identical/stricter |
| COMMAND_NEXT | 1 (START), last | 1 (START), last | identical |
| per-job core request | `kbase_pm_request_cores(affinity)` before START | all present cores are powered at resume and kept on (only whole-GPU core-off, gated on `p7580_job_is_idle`) | equivalent while the block is up |
| AS setup | `kbase_mmu_hw_configure`: TRANSTAB, MEMATTR → `AS_COMMAND_UPDATE`; `wait_ready` polls `AS_STATUS` **before** each AS command | `p7580_mmu_program_as`: TRANSTAB, MEMATTR, TRANSCFG=0 → `AS_COMMAND_UPDATE`, no `AS_STATUS` wait | divergence, but the MMU stalls on UPDATE; cannot explain a wedged slot |

**Conclusion:** no missing start write, bit, value or ordering. The start path
cannot be the reason for a job that reports `ACTIVE` after START.

## 4. Vendor vs ours — IRQ masks and post-reset config (the real gap)

Stock `kbase_pm_enable_interrupts()` (0xffffffc0004f4774), called from
`kbase_pm_init_hw()` (call site 0x4f4f5c) after every reset and from
`kbase_pm_clock_on()` after the per-AS MMU reprogram:

| write | stock value | stock instruction | ours |
|---|---|---|---|
| `GPU_IRQ_CLEAR` (0x024) | 0x00010581 (`GPU_IRQ_REG_ALL`) | 0x4f47ac | 0x100 during reset only |
| `GPU_IRQ_MASK` (0x028) | **0x00010581** | 0x4f47c4 | 0x100 during reset, then **0** (0x7154) |
| `JOB_IRQ_CLEAR` (0x1004) | 0xFFFFFFFF | 0x4f47e4 | present-slot mask 0x70007 |
| `JOB_IRQ_MASK` (0x1008) | **0xFFFFFFFF** | 0x4f47f8 | **0x70007** (functionally equal for 3 slots) |
| `MMU_IRQ_CLEAR` (0x2004) | 0xFFFFFFFF | 0x4f480c | only `=stat` inside the IRQ handler |
| `MMU_IRQ_MASK` (0x2008) | **0xFFFFFFFF** | 0x4f4820 | **never written anywhere** |

`GPU_IRQ_REG_ALL` = GPU_FAULT(0) | MULTIPLE_GPU_FAULTS(7) | RESET_COMPLETED(8) |
POWER_CHANGED_ALL(10) | PRFCNT_SAMPLE_COMPLETED(16) = 0x10581. Our driver
programs GPU_IRQ_MASK=0 outside the reset poll, so **GPU faults are invisible
to us**; `p7580_drv_gpu_irq()` also never reads `GPU_FAULTSTATUS` (0x3C) /
`GPU_FAULTADDRESS` (0x40/0x44).

The MMU block is the important one. Ours defines
`P7580_MMU_INT_CLEAR/MASK/STAT` (0x04/0x08/0x0C at 0x2000) and an IRQ handler
that demuxes `(1<<as)|(1<<(as+16))` and clears via 0x2004 — but nothing ever
writes 0x2008. The mask therefore keeps whatever a GPU reset leaves in it.

Stock `kbase_pm_init_hw()` (0x4f4b60) additionally programs the engine config
registers after **every** reset (`kbase_pm_hw_issues_apply`, source
`mali_kbase_pm_driver.c:1158`, disasm 0x4f4ee4…0x4f4f40):

| write | stock value on this GPU | stock instruction | ours |
|---|---|---|---|
| `GPU_COMMAND` (0x030) | 1 (SOFT_RESET), 2 on timeout | 0x4f4c80, 0x4f4d64 | same (0x7127-0x7158) |
| `GPU_IRQ_MASK` (0x028) | 0x100 (RESET_COMPLETED) during reset | 0x4f4c94 | same |
| `SHADER_CONFIG` (0xF04) | **0x02000000** = `SC_ENABLE_TEXGRD_FLAGS` | 0x4f4ef8 (value built 0x4f4dc8…0x4f4e04) | **never** |
| `TILER_CONFIG` (0xF08) | read-modify-write; `TC_CLOCK_GATE_OVERRIDE` only for `T76X_3953` (not t72x) → unchanged | 0x4f4f0c | never |
| `L2_MMU_CONFIG` (0xF0C) | `read & 0xF0FFFFFF` (ARID/AWID limit fields cleared; t72x does not set new limits) | 0x4f4f20 | never |
| `JM_CONFIG` (0xF00) | not written on t72x (sentinel 0x80000000; T86x/T88x only) | 0x4f4f24-0x4f4f40 | never (fine) |

Why `0x02000000`: `kbase_pm_hw_issues_detect()` tests issue-mask bit indices
20/64/41/51; in the r7p0 `enum base_hw_issue`, index 51 is
`BASE_HW_ISSUE_10797` and `base_hw_issues_t72x_r1p0[]` contains 10797
(`mali_base_hwconfig_issues.h:448-461`, 10797 at :453). Indices 20/64 (8443/11035,
`SC_LS_PAUSEBUFFER_DISABLE`) and 41 (10327, `SC_SDC_DISABLE_OQ_DISCARD`) are
*not* in the t72x list, and `T76X_3953` is absent, so the only shader-config
quirk this SoC needs is `SC_ENABLE_TEXGRD_FLAGS`.

## 5. Per-completion writes the stock driver does and we do not

`kbase_job_done()` (stock 0xffffffc0004f010c, source
`mali_kbase_jm_hw.c:231`) on every job interrupt:

| write/read | value | stock instruction | ours |
|---|---|---|---|
| `JOB_IRQ_THROTTLE` (0x1014) | `irq_throttle_cycles` (per-device value, ≈ gpu_khz × 10) | 0x4f0188 (value set in `kbasep_js_devdata_init`, 0x4ddd10) | never |
| `JOB_IRQ_CLEAR` (0x1004) | `done & ((1<<i)|(1<<(i+16)))` | 0x4f0324 | same idea (`=stat` bits) |
| `JOB_IRQ_JS_STATE` (0x1010) | **read** after every CLEAR, to count which atoms actually retired (the stock comment describes a race where a second job starts/fails between DONE and CLEAR) | 0x4f0334 | never read |

Neither can explain the observed signature:

* the stock register-map header states `JOB_IRQ_THROTTLE` affects *delivery*
  only ("JOB_IRQ_STATUS is NOT affected by this"), so it cannot produce
  `rawstat=0`; and
* `JOB_IRQ_JS_STATE` only matters when more than one atom can be queued on a
  slot; our `hw_submit` refuses to program a slot whose `COMMAND_NEXT` is
  non-zero and tracks exactly one `in_flight[slot]`.

## 6. `0x08` = ACTIVE: why the start path cannot explain the hang

Evidence chain:

* stock table (0xffffffc0009ba710, decoded by `dump-js-status-table.py`):
  `0x00 NOT_STARTED/IDLE/OK`, `0x01 DONE`, `0x08 ACTIVE`;
* `BASE_JD_EVENT_ACTIVE = 0x08` — "Can't be seen by userspace, jobs only
  returned on complete/fail/cancel" (`mali_base_kernel.h:838`);
* our own traces: a healthy dispatch logs `post-start js_status=0x08` and the
  matching IRQ later reads `0x00`, i.e. 0x08 is the running state;
* in the failing trace 0x08 is still there 750 ms later, `COMMAND_NEXT` is
  drained (`drain_rc=0`), `HEAD==TAIL==jc`, and no JOB DONE/ERR bit was ever
  latched.

So: START was accepted, the chain reached its tail, and the slot still thinks
the job is executing. This is a *non-retiring job*, not a start failure, and
the name `NO_EXCEPTION` in our log is an artifact of our decoder table missing
0x08.

## 7. Ranked explanation

### #1 (best fit) — GPU page/bus fault is structurally silent because we never program `MMU_IRQ_MASK`

Mechanism, from the stock code:

* a GPU access that the MMU cannot translate/authorise raises
  `MMU_IRQ_RAWSTAT` bit n (page fault) or bit n+16 (bus error) — it does
  **not** produce a `JS_STATUS` fault code and does not retire the job;
* the stock driver's page-fault worker (`kbase_mmu_interrupt_process` →
  `kbase_mmu_report_fault_and_kill`, 0x4d947c) is what resolves it: clear the
  fault, disable/mark the AS, and **kill the context's jobs**
  (`kbase_backend_jm_kill_jobs_from_kctx` → `kbase_job_slot_hardstop`), after
  which the fence is completed with the fault;
* with `MMU_IRQ_MASK` never enabled, none of that runs: the slot stays
  `ACTIVE`, `HEAD==TAIL`, `JOB_INT` 0, no error, and only the 750 ms
  drm_sched timeout notices. Every field of the reported signature.

Both references unmask this block after every reset: stock
`kbase_pm_enable_interrupts()` writes `MMU_IRQ_CLEAR/MASK = 0xFFFFFFFF`
(0x4f480c/0x4f4820), and mainline panfrost v5.2 `panfrost_mmu_init()` does
`mmu_write(MMU_INT_CLEAR, ~0); mmu_write(MMU_INT_MASK, ~0)`
(panfrost_mmu.c:365-366). Our driver has the handler and the defines, but no
write to 0x2008 — so the block is in whatever state a GPU reset leaves it,
i.e. masked.

Why KWin and not the 155 fps benchmark: the compositor churns and re-maps the
window/pixmap BOs every frame and the GPU is power-cycled between frames;
a single stale/unmapped tile-heap or framebuffer access is enough. The
benchmark keeps one static BO set and never idles, so it neither faults nor
resets.

Falsifiable prediction: at the next timeout, `AS0_FAULTSTATUS` (0x241C) and/or
`MMU_IRQ_RAWSTAT` (0x2000) is non-zero, and the value decodes to a
translation/permission/access-flag fault. This also means the earlier
"zero MMU fault lines ⇒ no MMU fault" reasoning in
`docs/reports/gpu31-mesa-render.md` and `gpu31-jobtrace.md` is unsound: those
lines can only appear if the mask was enabled.

Enabling the mask is not merely diagnostic: clearing the fault is what lets
the MMU abort the stalled transaction, so the slot either resumes/progresses
or receives a real JS fault that the existing ERR/recovery path handles
immediately instead of after 750 ms.

### #2 — `GPU_IRQ_MASK=0` hides GPU faults

Vendor keeps `GPU_IRQ_REG_ALL` (0x10581) enabled permanently and its
`kbase_gpu_interrupt()` logs `GPU_FAULT`/`MULTIPLE_GPU_FAULTS` with
`GPU_FAULTSTATUS`/`FAULTADDRESS`. We mask all GPU interrupts outside the reset
poll and never read 0x3C. A bus/engine fault is another way for a slot to
wedge in `ACTIVE` with no JOB_INT event. Same silence, slightly less likely
than an MMU fault under compositor BO churn; the fix is the same patch family
(mask 0x10581 + log 0x3C on bit0/bit7).

### #3 — Missing t72x `SHADER_CONFIG` quirk (`SC_ENABLE_TEXGRD_FLAGS`)

Stock writes 0x02000000 to 0xF04 after every reset; the t72x r1p0 issue list
requires it (10797). We never write the register, so the value is the reset
default after every KWin power cycle. A shader that relies on texture
gradient/derivative behaviour can misbehave or spin without it, which would
show up as a fragment job that stays ACTIVE with no fault. This is the only
vendor engine-config write we lack; it is the next thing to add if #1/#2 come
back zero.

### Ruled out

| candidate | why not |
|---|---|
| missing/mis-valued START writes or order (HEAD/AFFINITY/CONFIG/COMMAND) | byte-identical (§2/§3); ACTIVE proves the START latched |
| `JS_CONFIG_NEXT` bit 11 (chain flag) | feature not present on t72x; stock branch is dead code (§2) |
| `JOB_IRQ_THROTTLE` | affects IRQ delivery only, not STATUS/RAWSTAT (stock header) |
| `JOB_IRQ_JS_STATE` not read | only counts retires when >1 atom per slot; we keep 1 |
| `JOB_IRQ_MASK` 0x70007 vs 0xFFFFFFFF | same bits for 3 slots |
| AS programming order | ours writes the same TRANSTAB/MEMATTR/UPDATE (plus TRANSCFG=0); no AS_STATUS wait, but the MMU stalls on UPDATE |
| affinity / core power | stock requests the atom's cores before START but does not wait; we power all present cores at resume and keep them on until idle |

## 8. Minimal patch to try

Three hunks in `code/gpu31/driver/0001-panfrost7580-driver-sources.patch`.
The first is the experiment; the second is one-line diagnosis that makes the
next failure self-explaining; the third fixes the mislabel.

**(a) Clear + unmask the MMU block wherever the JOB block is programmed.**
In the jobs slice's `jm_hw.h` register block (next to the
`P7580_JOB_INT_*` defines, ~line 2795):

```c
+#define P7580_MMU_INT_CLEAR	0x2004u
+#define P7580_MMU_INT_MASK	0x2008u
```

and in `p7580_job_dev_init()` (after the existing
`JOB_INT_CLEAR`/`JOB_INT_MASK` pair, ~line 3707) and in
`p7580_job_dev_rearm()` (after its pair, ~line 3743):

```c
+	/* Stock kbase_pm_enable_interrupts() and mainline
+	 * panfrost_mmu_init() both drain and unmask the MMU block after
+	 * every reset. Without it a GPU page/bus fault is silent: the
+	 * code lands in ASn_FAULTSTATUS, not JS_STATUS, and the slot
+	 * stays ACTIVE (0x08) until the 750 ms sched timeout. */
+	p7580_writel(jd, P7580_MMU_INT_CLEAR, 0xFFFFFFFFu);
+	p7580_writel(jd, P7580_MMU_INT_MASK, 0xFFFFFFFFu);
```

Vendor order is JOB then MMU (`kbase_pm_enable_interrupts`); since
`p7580_job_dev_rearm()` is called from resume and recovery, and
`p7580_job_dev_init()` from probe, the MMU block is then covered on all three
paths.

**(b) Report the fault that was previously invisible** — add to the TIMEOUT
block (~line 4310), using the AS registers already defined for the mmu slice
(`P7580_MMU_AS_BASE`/`P7580_MMU_AS_STRIDE`/`P7580_AS_FAULTSTATUS`/
`P7580_AS_FAULTADDR_*`; copy the four defines into `jm_hw.h` since the jobs
slice is standalone):

```c
+	pr_err("p7580_job: TIMEOUT-AS as=%u faultstatus=0x%08x faultaddr=0x%llx mmuraw=0x%08x gpu_fault=0x%08x\n",
+	       job->as_nr,
+	       p7580_readl(jd, P7580_MMU_AS_BASE + job->as_nr * P7580_MMU_AS_STRIDE + P7580_AS_FAULTSTATUS),
+	       (unsigned long long)(((u64)p7580_readl(jd, P7580_MMU_AS_BASE + job->as_nr * P7580_MMU_AS_STRIDE + P7580_AS_FAULTADDR_HI) << 32) |
+	                            p7580_readl(jd, P7580_MMU_AS_BASE + job->as_nr * P7580_MMU_AS_STRIDE + P7580_AS_FAULTADDR_LO)),
+	       p7580_readl(jd, 0x2000u),
+	       p7580_readl(jd, P7580_GPU_INT_RAWSTAT));
```

**(c) Stop calling 0x08 "NO_EXCEPTION"** — extend the status table (~line
2952) so the log names what the stock table names:

```c
+	{ 0x00u, "NOT_STARTED/IDLE/OK" },
+	{ 0x01u, "DONE" },
+	{ 0x08u, "ACTIVE" },
 	{ 0x02u, "INTERRUPTED" },
```

Then run a KWin session. Decision rule:

* `faultstatus != 0` → a GPU page/permission fault was the silent wedge;
  root-cause the mapping it names (`faultaddr` resolved against the job's
  `ERR-MAP` BO list). Optionally kick the slot's timeout on the MMU IRQ (the
  ERR leg already does `drm_sched_job_kick_timeout()`) so recovery is
  immediate instead of 750 ms, and/or hard-stop the slot as stock
  `kbase_mmu_report_fault_and_kill()` does.
* `faultstatus == 0` and `gpu_fault != 0` → GPU bus fault; enable
  `GPU_IRQ_MASK = 0x10581` and decode 0x3C.
* both 0 and the hang persists → add the stock post-reset engine config in
  `pf7580_soft_reset_locked()` (after the reset completes, before cores on):

```c
+	/* t72x r1p0 BASE_HW_ISSUE_10797; stock kbase_pm_hw_issues_apply()
+	 * rewrites this after every reset. */
+	writel(0x02000000u, p->regs + 0xF04);	/* SC_ENABLE_TEXGRD_FLAGS */
```

## 9. Reproduce / artifacts

```sh
# extraction + symbols (writes ~3.3 GiB tar + 16 MiB Image under <workdir>)
code/gpu31/re/stock-mali/extract-stock-kbase.sh \
    /home/asdf/Downloads/SAMFW.COM_SM-J700M_PCW_J700MUBU2BRC2_fac.zip \
    /run/media/asdf/.../j7k/stromali

# the six start writes, the IRQ masks, the status table
code/gpu31/re/stock-mali/dis-kbase.sh kernel.bin --sym kbase_job_hw_submit
code/gpu31/re/stock-mali/dis-kbase.sh kernel.bin --sym kbase_pm_enable_interrupts
code/gpu31/re/stock-mali/dis-kbase.sh kernel.bin --sym kbase_pm_init_hw
code/gpu31/re/stock-mali/dump-js-status-table.py kernel.bin
```

Committed helpers: `extract-stock-kbase.sh`, `dis-kbase.sh`,
`dump-js-status-table.py`, `kbase-symbols.txt` (all under
`code/gpu31/re/stock-mali/`). The stock archive is untouched; no device was
used.

Source citations (mediatek r7p0 tree; line numbers as fetched, used only to
name values/checks that were then confirmed in the Image):

* `backend/gpu/mali_kbase_jm_hw.c` — `kbase_job_hw_submit` :62-183,
  `JOB_CHAIN_FLAG` branch :127-139, START :184; `kbase_job_done` :231,
  throttle :247-254, JS_STATE :327;
* `backend/gpu/mali_kbase_pm_driver.c` — `kbase_pm_enable_interrupts` :817,
  `_mmu_mask` :843, `kbase_pm_disable_interrupts` :867, `kbase_pm_clock_on`
  :898, `kbase_pm_hw_issues_detect` :1061, `_apply` :1158, `kbase_pm_init_hw`
  :1178;
* `mali_base_hwconfig_issues.h` — `base_hw_issues_t72x_r1p0` :463-476 (10797
  present), enum order :25+ (index 51 = 10797);
* `mali_base_hwconfig_features.h` — `base_hw_features_t72x` :73-84 (no
  JOBCHAIN_DISAMBIGUATION, no FLUSH_REDUCTION, no PROTECTED_MODE);
* `mali_midg_regmap.h` — job/MMU register map and bits, `JS_STATUS` values,
  `JS_CONFIG` bits;
* `mali_base_kernel.h` :823-838 — `BASE_JD_EVENT_ACTIVE = 0x08`;
* `mali_kbase_mmu.c` — `kbase_mmu_interrupt_process`,
  `kbase_mmu_report_fault_and_kill` (kill path);
* mainline v5.2 `drivers/gpu/drm/panfrost/panfrost_mmu.c:365-366` — MMU
  CLEAR/MASK = ~0 at init.
