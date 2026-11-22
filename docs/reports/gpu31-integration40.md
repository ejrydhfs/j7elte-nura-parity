# gpu31-integration40 — power-off fails closed, POWER_FAULT latch is cycled, 24/24 gates

Date: 2026-11-22. Owner: G31_Integrator40. Phone: on int39 state;
this ticket builds host-side only. No flash per ticket (Main flashes).

Consumes **161993d** (gpu31-readback): a busy-core power-off no longer
gates the block (`-EBUSY`, cores restored), `p7580_job_is_idle()` also
requires no present slot ACTIVE in `JS_STATUS`, and a new
`pf7580_power_cycle()` clears the `POWER_FAULT` latch from recovery and
resume. Driver 0001 at build time: sha256
`e9cb91a7086e526f106b07c2abe3cbf551bef57024e03341dd05aeb95d6dcfe8`,
repo HEAD `161993d`.

## 0. TL;DR

- **Build green**: fresh scratch tree on the external disk, `-j4`
  Image, RC 0, 0 `error:` lines, 5393-line log ending
  `OBJCOPY arch/arm64/boot/Image`; 498 warnings, the same ambient count
  as int38/int39 (configured `-Wno-error` noise + 3×
  `vmlinux has a LOAD segment with RWX permissions`).
- **24/24 gate banners PASS, 0 FAIL**: the 23 int39 gates unchanged in
  invariant plus new **6aa** (power-idle + power-cycle recovery).
- **6aa**, in the applied tree, the objects and the linked Image:
  - `pf7580_cores_off_locked()` powers the three cores back on and
    returns `-EBUSY` (power.c:304 busy label, :316 return);
  - `pf7580_runtime_suspend()` returns that error before
    `pf7580_clk_off_locked()` (power.c:398);
  - `p7580_drv_runtime_suspend()` refuses up front while
    `!p7580_job_is_idle()` (main.c:332) and otherwise propagates the
    power-slice result;
  - `p7580_job_is_idle()` reads `JS_STATUS` (job.c:403) and treats
    ACTIVE-with-a-non-stop-code as busy — two ordered MMIO reads
    (`dsb ld`) in `job.o`, plus the `(st - 2) <= 2` stop exemption
    (0x02/0x03/0x04);
  - `pf7580_power_cycle()` (power.c:380) does cores-off (warn and
    continue when busy), clock gate, full power-on; the jobs slice calls
    it from `p7580_job_recover()` on `status == 0x41` or a non-stop
    post-reset fault (job.c:1155), then reprograms AS0 and re-arms
    JOB_INT, and always prints `recover-done`
    (`slot/latched/cycle/js0/js1/intmask/drain_rc`, job.c:1248); resume
    cycles a latched fault before its AS0/re-arm (main.c:362–376);
  - `recover-cycle FAIL` and `resume-cycle` telemetry live in
    `job.o`/`main.o` and the Image; host T9 pins the cycle-decision
    table.
- **Config**: all 7 d0+d4 fragment symbols survive `olddefconfig`
  verbatim, including `CONFIG_DRM_PANFROST7580=y`.
- Outputs: `Image-int40` `720d9746…`, `kernel-boot-int40.img`
  `40bf9948…` (§4).

## 1. Build

Script `code/gpu31/series/build-int40.sh` (2449 lines; sha256
`9ebfc736024c466cba4214b942898e5ddf21f78829ef21c9ac491f576735a210`),
a copy of int39 (efbba4e) with int40 names, the new **6aa** gate block
and a **config closure** gate (§3.3).

Fresh `cp -a` of the pristine tree →
`…/j7k/gpu31/scratch-int40` (birth 15:08:21, tree mtime 15:12:24);
scratch and stage deleted after the pack. `TMPDIR` and every output are
on the external disk (`/run/media/asdf/db69437b…/j7k/gpu31`); nothing was
written to `/tmp` or `/`.

Patches, each `git apply --check` first: backport shims 0001–0005,
Mode-B render node 0001, dmabuf CMA writecombine 0001, driver 0001+0002.
Config: pmaports `config-samsung-j7elte.aarch64` + `d0-m1-minimal` +
`d4-panfrost7580`, then `yes "" | make ARCH=arm64 olddefconfig`
(closure gated, §3.3).

Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
KCFLAGS="-Wno-error" -j4 Image`.

- Build log: `…/logs/gpu31-int40-build.log` (5393 lines, 0 `error:`,
  498 `warning:`; tail `SORTEX vmlinux`, `SYSMAP System.map`,
  `OBJCOPY arch/arm64/boot/Image`). Make RC 0 and Image mtime
  (15:12:24) ≥ build start are gated.
- Gate stdout: `…/logs/gpu31-int40-driver.log` (697 lines, 24 PASS
  banners, 0 `GATE FAIL`).
- Evidence: `…/logs/gpu31-int40-evidence.log` (735 lines).
- Host tests: `…/logs/gpu31-int40-hosttest.log`
  (`submit_fix_test: all checks passed`, `chainsplit_test: all checks
  passed`), `…/logs/gpu31-int40-schedhost.log` (2× `409 checks, 0
  failures`, `gates: all PASS`, kcheck PASS).
- Pack: stock `dt.img` + reference `ramdisk-new.gz` (never rebuilt) +
  the built Image → 27332608 B = kernel 18193592 + ramdisk 8822392 +
  dt 313344 (+3280 header) by `stage-int40/mkboot.py`.

Source provenance (driver.log:413): `repo HEAD=161993d
driver-0001 sha256=e9cb91a7…`. The patch is recorded before `git apply`
and re-checked before the gates; the report commit does not touch it.

Freshness: the pristine tree has no `drivers/gpu/drm/drm_sched.c` and
no `drivers/gpu/drm/panfrost/` directory (both are created by the
patches). `Image-int40` differs from `Image-int39` in 9,168,204 bytes
(same 18193592-byte size).

## 2. What changed in the image

The int39 failure mode (measured in `gpu31-readback.md`): runtime
suspend powered the cores down under a live JM (22 × `tiler/shaders/L2
did not idle on power-off` per bench run); the JM latched
`JS_STATUS 0x41 POWER_FAULT` for the victim job; the GPU-level SOFT/HARD
reset in `p7580_job_recover()` does not clear that latch, so every later
job on the slot re-reported 0x41 (36/72/144-job cascades) until a
suspend happened to cycle the block. int40 carries the fix; the applied
tree (not just the patch) is what 6aa proves.

| Applied site | Change |
|---|---|
| `power.c:272` `pf7580_cores_off_locked()` | on any READY!=0 poll, jump to `busy:` (:304): re-assert L2, shader and tiler `PWRON` from `PRESENT`, then `return -EBUSY` (:316). The block is never left half-powered and the domain/clock stay up. |
| `power.c:398` `pf7580_runtime_suspend()` | `ret = pf7580_cores_off_locked(p)`; if non-zero, unlock and return it **before** `pf7580_clk_off_locked()` (source line order gated, compiled -16 return gated). |
| `main.c:332` guard in `p7580_drv_runtime_suspend()` | `if (drv && !p7580_job_is_idle(&drv->jd)) return -EBUSY;` before any dvfs/power call; otherwise `return pf7580_runtime_suspend(drv->power)`. |
| `job.c:403` `p7580_job_is_idle()` | keeps `COMMAND_NEXT` and `in_flight`; for each present slot reads `JS_STATUS` and returns 0 when `(st & 1) && !p7580_js_status_is_stop(st)` — i.e. an ACTIVE latch is busy unless it is our own INTERRUPTED/STOPPED/TERMINATED result. |
| `power.c:380` `pf7580_power_cycle()` | under `p->lock`: `pf7580_cores_off_locked()` (warn `"power cycle with busy cores"` and continue — the following power-on issues the reset that clears the stuck state), `pf7580_clk_off_locked()`, `pf7580_power_on_locked()`. Shared power-on with resume. Declared in `power.h`. |
| `job.h` / `main.c` wrapper | `power_cycle(void *dev)` hook in `p7580_job_dev`, `p7580_job_dev_init()` parameter and `p7580_job_power_cycle_void()` → `pf7580_power_cycle(drv->power)`, passed at probe. |
| `job.c:1155` `p7580_job_recover()` | after the GPU reset, if `jd->power_cycle && jd->pm_dev && (status == 0x41 || p7580_job_slots_latched_fault(jd))`, run the cycle once (`cycled = 1`, `recover-cycle FAIL rc=` on error), then AS0 (`:1240`) + `p7580_job_dev_rearm()`, then the always-on `recover-done` line (`:1248`). |
| `main.c:362` latch path in `p7580_drv_runtime_resume()` | after power-on, if `p7580_job_slots_latched_fault(&drv->jd)` call the same hook (`resume-cycle rc=`), then reprogram AS0 and re-arm. |

No fence, BO lifetime, scheduling-order or MMU mapping semantics changed;
int39's fault-latency kick and every earlier invariant still gate.

## 3. Gate evidence (computed inside the build)

### 3.1 Prior gates intact (23/23 PASS)

Carried verbatim from int39: ops layout, irq, slot, abi, gpuwrite,
coherency, instrtype, iova, armfence, ptewalk, pmget, timedout,
submitsnap, syncobj-wait, jddecode, multidep, serialfence, cachedir,
mmaprefs, chainsplit, defectb, chainsplit-length, fault-latency. All
PASS. Unchanged computed constants include ops=24/slot=232/ran=236,
slot routing FS→JS0 / req0→JS1 / ONLY_COMPUTE→JS2, per-node cfg
`0x83700`, the single shmem page-get/flush/store triple (object and
linked vmlinux), 56/192/192/64/32 chain lengths, and the single
delay-0 timeout kick with the 5 s backstop.

### 3.2 New 6aa: power-idle + power-cycle recovery (PASS)

Applied-tree anchors recorded in the evidence log:

```
drivers/gpu/drm/panfrost/panfrost7580_power.c:272:static int pf7580_cores_off_locked(struct pf7580_power *p)
drivers/gpu/drm/panfrost/panfrost7580_power.c:304:busy:
drivers/gpu/drm/panfrost/panfrost7580_power.c:316:	return -EBUSY;
drivers/gpu/drm/panfrost/panfrost7580_power.c:380:int pf7580_power_cycle(struct pf7580_power *p)
drivers/gpu/drm/panfrost/panfrost7580_power.c:398:int pf7580_runtime_suspend(struct pf7580_power *p)
drivers/gpu/drm/panfrost/panfrost7580_job.c:403:int p7580_job_is_idle(struct p7580_job_dev *jd)
drivers/gpu/drm/panfrost/panfrost7580_job.c:1138:int p7580_job_slots_latched_fault(struct p7580_job_dev *jd)
drivers/gpu/drm/panfrost/panfrost7580_job.c:1155:int p7580_job_recover(struct p7580_job_dev *jd, int fault_slot,
drivers/gpu/drm/panfrost/panfrost7580_job.c:1240:	p7580_mmu_program_as(jd->regs, 0, jd->pgd_dma,
drivers/gpu/drm/panfrost/panfrost7580_job.c:1248:	pr_err("p7580_job: recover-done ... drain_rc=%d\n",
drivers/gpu/drm/panfrost/panfrost7580_main.c:332:		return -EBUSY;
drivers/gpu/drm/panfrost/panfrost7580_main.c:362:	    p7580_job_slots_latched_fault(&drv->jd)) {
drivers/gpu/drm/panfrost/panfrost7580_main.c:376:		p7580_mmu_program_as(drv->regs, P7580_DRV_AS_NR,
```

The busy block is gated line by line: exactly one `return -EBUSY;` in
power.c, the label present, three `PWRON_LO` writes inside it, and that
return as its last statement. The `busy:` body:

```
busy:
	/* Put the cores back on while the domain and clock are still up,
	 * so an aborted suspend never leaves a half-powered block. */
	writel(readl(p->regs + PF7580_L2_PRESENT_LO),
	       p->regs + PF7580_L2_PWRON_LO);
	writel(0, p->regs + PF7580_L2_PWRON_HI);
	writel(readl(p->regs + PF7580_SHADER_PRESENT_LO),
	       p->regs + PF7580_SHADER_PWRON_LO);
	writel(0, p->regs + PF7580_SHADER_PWRON_HI);
	writel(readl(p->regs + PF7580_TILER_PRESENT_LO),
	       p->regs + PF7580_TILER_PWRON_LO);
	writel(0, p->regs + PF7580_TILER_PWRON_HI);
	return -EBUSY;
```

`runtime_suspend` source order is `cores_off → if (ret) return ret` at
a line before `clk_off`; the recovery order is
`cycle → AS0 → rearm → recover-done`; resume tests/cycles before its
AS0. The patch mirror carries the same definitions, the recover-done
and resume-cycle lines, and three `return -EBUSY` sites (cores-off,
driver guard, pre-existing dvfs-init busy).

Compiled proof (`power.o`, `main.o`, `job.o`, -O2):

```
pf7580_runtime_suspend:
  6a0: bl   ac <pf7580_cores_off_locked>
  6a4: mov  w20, w0
  6a8: cbnz w0, 6ec
  ...
  6f0: mov  w20, #0xfffffff0          // #-16  (-EBUSY, before clk_disable/unlock)

pf7580_power_cycle:
  600: bl   ac <pf7580_cores_off_locked>
  604: cbnz w0, 65c
  ...
  65c: bl   <dev_warn>                // "power cycle with busy cores", then continues
  618: bl   <clk_disable>
  63c: bl   270 <pf7580_power_on_locked.part.0>

p7580_drv_runtime_suspend:
  2a4: bl   <p7580_job_is_idle>
  2a8: cbz  w0, 2e0
  2b8: bl   <pf7580_dvfs_suspend>
  2c8: bl   <pf7580_runtime_suspend>
  2e0: mov  w0, #0xfffffff0           // #-16

p7580_job_is_idle:
  688: ldr  w1, [x1]                  // JS_COMMAND_NEXT
  68c: dsb  ld
  690: cbnz w1, busy
  694: ldr  x1, [x5, x2, lsl #3]       // in_flight[i]
  698: cbnz x1, busy
  69c: ldr  w4, [x0, #8]              // js_present
  6a0: lsl  w1, w6, w2
  6a4: tst  w1, w4
  6a8: b.eq next
  6b8: ldr  w1, [x1]                  // JS_STATUS
  6bc: dsb  ld
  6c0: tbz  w1, #0, next              // not ACTIVE -> idle
  6c4: sub  w1, w1, #0x2              // 0x02/0x03/0x04 stop exemption
  6c8: cmp  w1, #0x2
  6cc: b.hi busy
```

So the flashed code performs two ordered device reads per slot and only
declares idle when `JS_STATUS` has no ACTIVE bit or carries our own stop
code. `power_cycle` deliberately has no `-EBUSY` exit: when the cores
cannot be idled it warns and still runs the power-on reset, which is the
state-clearing step.

Symbols (object offset / linked address):

```
power.o  T pf7580_power_cycle        0x5e0    ffffffc000582500
power.o  T pf7580_runtime_suspend    0x680    ffffffc0005825a0
job.o    T p7580_job_is_idle         0x66c    ffffffc00057f7ac
job.o    T p7580_job_slots_latched_fault 0x1728 ffffffc000580868
main.o   t p7580_drv_runtime_suspend 0x284
main.o   t p7580_job_power_cycle_void 0x6b0
```

`job.o` and `main.o` contain exactly one copy of each always-on string;
the Image contains them too (the leading length byte makes `strings`
print `3p7580_job: …`, the gate greps the string body):

```
p7580_job: recover-done slot=%d latched=0x%08x cycle=%d js0=0x%08x js1=0x%08x intmask=0x%08x drain_rc=%d
p7580_job: recover-cycle FAIL rc=%d
p7580_job: resume-cycle rc=%d (latched JS fault)
```

Host `submit_fix_test` T9 (`model_recover_latch`) checks the decision
table: 0x41 latched → one cycle + AS0 + rearm; NO_EXCEPTION → no cycle,
still AS0+rearm; post-reset STOPPED (0x03) → no cycle; post-reset 0x52
(non-stop fault) → one cycle + rearm. All checks pass (§1).

### 3.3 Config closure (d0+d4)

The script now asserts, after `olddefconfig`, that every non-comment
line of both fragments survives verbatim (7/7), and records them. Exact
lines in this build:

```
CONFIG_MODULES=y
CONFIG_DRM=y
CONFIG_DRM_KMS_HELPER=y
CONFIG_DRM_EXYNOS=y
CONFIG_DRM_EXYNOS_DMABUF=y
CONFIG_DRM_EXYNOS_G2D=y
CONFIG_DRM_PANFROST7580=y
```

## 4. Outputs (sha256)

- `Image-int40` (18193592 B):
  `720d9746363cb21456c808dcb6e67909c3a393a06961bead9551f6cbf42b86ac`
- `kernel-boot-int40.img` (27332608 B):
  `40bf99488bc99ad8f5bd719a238c64787d02fcb21890c2f253b73e22a1253834`
- build log:
  `73b5b60e2bc94d9e17d978918fa4735aa3149d35168a28d74c6f63042ecc43c3`
- gate stdout log:
  `734725eb7cc31bdb4f86f81f9d66ec594fd34bb9d060877b9a2b1ac12963b593`
- evidence log:
  `ebb70b102793643d49a23ed3a9be043c55c2fee0d1032aeb9d449eb16725b119`
- host-test log:
  `91007da2edb706a24eb9523859d7fea5cffbc64519bd157b15d1f7ce9f1d1567`
- sched-host log:
  `9583de1c19c342c01e0c4d9930dbe43a389d13b450e4c39bda98be018394c23e`
- script:
  `9ebfc736024c466cba4214b942898e5ddf21f78829ef21c9ac491f576735a210`

All paths under
`/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31/`.
No flash per ticket.

## 5. Silicon acceptance (Main, after flashing int40)

`Image-int40` is the first image with the fail-closed power-off and the
latch cycle. Check, in order:

1. **Probe/boot**: render node present, driver bound to `mali@11400000`,
   no probe or MMIO errors in dmesg.
2. **No busy power-off**: under the control census / a long bench, the
   int39 signature `tiler/shaders/L2 did not idle on power-off` must be
   absent (it was 22× per run). Expect instead that runtime suspend is
   refused while work is queued and retried after the job retires; a
   `runtime_status` of `active` during a run is correct, not a hang.
   `pf7580: power cycle with busy cores` should only appear on a real
   fault-recovery cycle, not in steady state.
3. **Fault recovery is one cycle, not a cascade**: after a genuine job
   fault there must be one `recover-done slot=… latched=0x00000041
   cycle=1 js0=… js1=… intmask=… drain_rc=…` line (or `cycle=1` for a
   surviving non-stop code). `js0`/`js1` must no longer read 0x41,
   `intmask` must cover the present slots, and the next frame's jobs
   must dispatch/finish. Repeated `irq-err 0x41` on subsequent jobs is
   the old defect and contradicts §3.2.
4. **Cycle failure**: `recover-cycle FAIL rc=<negative>` means the
   shared power-on failed; capture the number with the `recover-done`
   line instead of retrying — that contradicts the PWRON/READY polling
   proven by the build.
5. **Resume**: if a fault was latched across a suspend/resume, expect
   exactly one `resume-cycle rc=0 (latched JS fault)` before the first
   post-resume dispatch; no cycle line means no latch.
6. **Readback**: rerun `rdtest` at 256² and 512² (plain, no
   `PAN_MESA_DEBUG=sync|trace`): 30/30 frames clean, 0 ERR/TIMEOUT, and
   pixel-correct, with no POWER_FAULT after the run. int39 needed a
   suspend between processes to clear the latch; int40 must not.

## 6. Script notes

- **6aa object gates use the real codegen**: `pf7580_cores_off_locked`
  is outlined (`t`, 0xac), so `runtime_suspend` is gated on the call
  plus the `#0xfffffff0` (-16) return; `power_cycle` is gated on the
  busy warning + `dev_warn` and the `pf7580_power_on_locked.part.0`
  call (no `-EBUSY` exit — it must proceed); `is_idle` on exactly two
  `dsb ld` device reads and the `sub #2 / cmp #2` stop mapping. A gate
  keyed on `mov w0, #0xfffffff0` alone would have missed the actual
  `w20`/return-in-`w0` form, and a `cmp #0x4`-only stop test would miss
  the compiler's `(st-2)<=2` transform.
- **One dry-run correction**: the first 6aa draft anchored "the last
  idle exit" on the *first* `return 1;`, which is the NULL-argument
  early return; the gate failed on the retained scratch tree
  (`st=17 ret1=6`). The check now uses the last `return 1;`, and the
  6aa block was dry-run against the retained object tree before the
  final full build. The committed script is the one that produced §4.
- **Config closure gate** (new): 7/7 fragment symbols must survive
  `olddefconfig`; the values are echoed into the evidence log. This
  makes "d0+d4 configured" a gate rather than a claim.
- Retained from int38/int39: new-file hunk `+count` integrity before
  any `git apply`, `make` RC 0 + Image newer than build start, the
  defectb loop-adjacency proof, and the fault-latency kick.
- Host tests run from `$EXT/hosttest-int40` (deleted); sched tests run
  in-tree under `code/gpu31/sched`. Scratch, stage and host-test dirs
  are deleted after the pack; logs, Image and boot.img remain on the
  external disk.
- Driver 0001 provenance is printed before apply and re-checked at 6v;
  this report commit does not modify it. Any later edit to that patch
  needs a rebuild before these sha256s can be quoted.
