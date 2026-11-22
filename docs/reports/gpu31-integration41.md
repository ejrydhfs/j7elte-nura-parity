# gpu31-integration41 — first-fault ring + one-shot page dump, desc hygiene fail-closed recovery, serialize_fs A/B; 25/25 gates

Date: 2026-11-22. Owner: G31_Integrator41. Phone: on int40; this
ticket builds host-side only. No flash per ticket (Main flashes).

Consumes **69b4a49** (gpu31-firstfault, folded into driver 0001): the
128-entry in-RAM event ring and one-shot 4 KiB descriptor-page dump
behind `debugfs p7580/ev_arm`, descriptor words 0..3 hygiene in every
shadow copy plus `desc-stale` shadowing of a recycled single-node chain,
a recovery that always ends in a complete core power-on for **every**
status (including the silent `NO_EXCEPTION 0x08`), and the
`serialize_fs` A/B knob. Driver 0001 at build time: sha256
`cfa4f59fbbbc4bfd1f2653f7d0a995a3acd36e6caee32ae88fac89aa47d01a82`,
repo HEAD `69b4a49`.

## 0. TL;DR

- **Build green**: fresh scratch tree on the external disk, `-j4`
  Image, RC 0, 0 `error:` lines, 5401-line log ending
  `OBJCOPY arch/arm64/boot/Image`; 499 warnings (int40: 498), i.e. the
  same ambient configured `-Wno-error` noise.
- **25/25 gate banners PASS, 0 FAIL**: the 24 int40 gates (23 carried
  verbatim + 6aa **retargeted** to the new recovery rule) plus new
  **6ab** (first-fault ring + page dump + hygiene + `serialize_fs`).
- **New 6ab**, in the applied tree, the objects and the linked Image:
  - `struct p7580_ev ev[128]` (`P7580_EV_MAX 128`) in `p7580_job_dev`;
    `p7580_job_ev()` stamps `ktime_to_ns(ktime_get())`, takes `hw_lock`
    and returns early while `!ev_arm` (no stores, no MMIO);
  - the dispatch records `JS_STATUS` before START (with the
    occupied-slot bit when `in_flight[slot]` was non-NULL),
    `COMMAND_NEXT` and `JS_STATUS` read back after START (job.c:616,
    :714); DONE (:854), ERR (:959), TIMEOUT (:1090), CYCLE (:1457) and
    RECOVER (:1487) each get exactly one record;
  - `p7580_job_first_fault()` (:257) is one-shot (`ev_dumped`), walks
    the ring oldest-first, prints `EVRING begin` / `EV …` (`dt` in µs) /
    `EVRING end`, then dumps four 4 KiB pages: `jc`, the live `src_jc`
    when it differs, the fragment SFBD (`fbd`), and the first page of
    the largest ≥ 1 MiB BO (`heap`). The timeout work calls it
    (:1092) **before** `p7580_job_recover()`;
  - `p7580_drv_desc_dump()` (submit.c:1078) reads each page with
    `p7580_drv_gpu_read` (clean→invalidate) and prints 16 lines of 16
    words, one line per 256-byte job slot; registered at probe
    (main.c:600). `src_jc` is stamped on every split node (:1492);
  - descriptor words 0..3 are zeroed in the shadow copy after
    `p7580_chain_hdr_isolate()` and before the header copy
    (submit.c:1857–1860), and a single-node chain whose source header
    carries a stale word 0..3 is forced through a shadow with a
    `desc-stale` line (:1818–1820);
  - `debugfs p7580/ev_arm` (:1813) and `p7580/serialize_fs` (:1818)
    both default 0; `serialize_fs=1` queues the slot-0 submit on
    entity[1] (submit.c:1684) while the hardware slot stays `s->slot`.
- **6aa retargeted**: `p7580_job_recover()` no longer decides per
  status. `do_cycle = (jd->power_cycle && jd->pm_dev) ? 1 : 0;`
  (job.c:1427) is the whole decision; the bare soft reset survives only
  as the unwired fallback (:1428); the power-on, AS0, rearm and
  `recover-done` order is unchanged (:1451–1476). The int40 predicates
  (`status == 0x41`, surviving-latch rescan) are now gate failures, and
  `job.o` carries no `0x41` compare in `p7580_job_recover`.
- **Config**: all 7 d0+d4 fragment symbols survive `olddefconfig`
  verbatim, including `CONFIG_DRM_PANFROST7580=y`.
- Outputs: `Image-int41` `c6ec1809…`, `kernel-boot-int41.img`
  `105bf35f…` (§4).

## 1. Build

Script `code/gpu31/series/build-int41.sh` (2791 lines; sha256
`d2b342a66fbb343232608994a94bd275b7493b94b5c684f58e725e76901704d2`),
a copy of int40 with int41 names, the retargeted **6aa**, the new
**6ab** gate block and a rewritten header; gates, freshness and pack
options unchanged.

Fresh `cp -a` of the pristine tree →
`…/j7k/gpu31/scratch-int41` (birth 15:54:24, tree mtime 15:58:34);
scratch and stage deleted after the pack. `TMPDIR` and every output are
on the external disk (`/run/media/asdf/db69437b…/j7k/gpu31`); nothing
was written to `/tmp` or `/`.

Patches, each `git apply --check` first: backport shims 0001–0005,
Mode-B render node 0001, dmabuf CMA writecombine 0001, driver 0001+0002.
The driver patch is hashed before apply (`cfa4f59f…`) and re-checked at
6v before the gates; the script's new-file hunk `+count` integrity check
runs first and passed.

Config: pmaports `config-samsung-j7elte.aarch64` + `d0-m1-minimal` +
`d4-panfrost7580`, then `yes "" | make ARCH=arm64 olddefconfig`
(closure gated 7/7, §3.4).

Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
KCFLAGS="-Wno-error" -j4 Image`.

- Build log: `…/logs/gpu31-int41-build.log` (5401 lines, 0 `error:`,
  499 `warning:`; int40: 5393/498; tail `SORTEX vmlinux`,
  `SYSMAP System.map`, `OBJCOPY arch/arm64/boot/Image`). Make RC 0 and
  Image mtime (15:58:34, gated) ≥ build start are enforced.
- Gate stdout: `…/logs/gpu31-int41-gates.log` (823 lines, 25 PASS
  banners, 0 `GATE FAIL`).
- Evidence: `…/logs/gpu31-int41-evidence.log` (864 lines).
- Host tests: `…/logs/gpu31-int41-hosttest.log`
  (`submit_fix_test: all checks passed`, `chainsplit_test: all checks
  passed`), `…/logs/gpu31-int41-schedhost.log` (2× `409 checks, 0
  failures`, `gates: all PASS`, kcheck PASS).
- Pack: stock `dt.img` + reference `ramdisk-new.gz` (never rebuilt) +
  the built Image → 27332608 B = kernel 18193592 + ramdisk 8822392 +
  dt 313344 (+3280 header) by `stage-int41/mkboot.py`.

Source provenance (evidence:419): `repo HEAD=69b4a49 driver-0001
sha256=cfa4f59f…`. The report commit does not touch driver 0001; any
later edit to it invalidates §4.

Freshness: the pristine tree has no `drivers/gpu/drm/drm_sched.c` and
no `drivers/gpu/drm/panfrost/` directory (both are created by the
patches). `Image-int41` keeps int40's 18193592-byte size and differs in
10,429,266 bytes.

## 2. What changed in the image

int40's finding (`gpu31-firstfault.md` §3/§4): the long-run first fault
is a fragment job (silent `0x08` or `DATA_INVALID 0x58`) whose
descriptor is clean at SUBMIT; the cascade is the recovery, because a
bare GPU soft reset re-latches `POWER_FAULT 0x41` on the next START and
the conditional cycle then fires one fault too late. The descriptor
words 0..3 are the JM's own output fields. int41 carries the fix and
the instrumentation that decides the remaining unknowns.

| Applied site | Change |
|---|---|
| `job.h:165` | `P7580_EV_MAX 128`; ten `P7580_EV_*` kinds; `ev[128]`, `ev_head`, `ev_arm`, `ev_dumped`, `desc_dump`/`desc_dump_priv`, `serialize_fs` in `p7580_job_dev` (:274–287). |
| `job.c:198` `p7580_job_ev()` | no-op while disarmed; otherwise one record under `hw_lock`; `e->ns = (uint64_t)ktime_to_ns(ktime_get())` (3.10 has no `ktime_get_ns()`). |
| `job.c:222` `p7580_job_ev_arm()` | resets head/`ev_dumped` and clears the ring, then stores the arm flag; drives the `ev_arm` knob. |
| `job.c:257` `p7580_job_first_fault()` | armed + not yet dumped; clamps to 128, starts at `ev_head - n`, walks `(start+i) & 127`; dumps `jc`, `src_jc` (when different), SFBD via `desc[4]` type 9 + `desc[10]`, and the largest BO when `>= 0x100000`. |
| `job.c:616`, `:714` | armed probes in `hw_submit`: previous `in_flight[slot]`, `JS_STATUS` pre-START, `COMMAND_NEXT`/`JS_STATUS` post-START, one START record with the occupied-slot bit. |
| `job.c:854/959/1090/1457/1487` | DONE / ERR / TIMEOUT / CYCLE / RECOVER records (one each). |
| `job.c:1092` | `timedout` runs the dump before `p7580_job_recover()`, so the capture sees the fault state. |
| `job.c:1728–1818` | `ev_arm`/`serialize_fs` get/set + `DEFINE_SIMPLE_ATTRIBUTE`, both `0600`, created under `p7580/`. |
| `submit.c:1078` | `p7580_drv_desc_dump()`: 4096-byte kmalloc, one `p7580_drv_gpu_read` (clean→invalidate), aligned base, `PAGEDUMP` + 16×`PAGE` lines; `PAGEDUMP … rc=` when unmapped. |
| `submit.c:1492` | split nodes stamp `src_jc = (s->src && s->shadow) ? s->src[pos] : 0`. |
| `main.c:600` | probe registers the dumper (`p7580_job_set_desc_dump(&drv->jd, p7580_drv_desc_dump, drv)`). |
| `submit.c:1817–1820` | single-node chains with `hdr[ci][0..3]` non-zero are shadowed (`need_shadow`) with `desc-stale nodes=… (shadowed+sanitised)`. |
| `submit.c:1857–1860` | after `isolate()`, `jobw[0..3] = 0` before `memcpy(hdr[ci], …)` and the shadow write, so the four JM output words never enter a dispatch. |
| `submit.c:1684` | `serialize_fs` (default 0): slot-0 submit rides `entity[1]` when the chain slot is present; `job->slot` stays `s->slot` (:1488), so the hardware slot is unchanged. |
| `job.c:1427–1454` | recovery: unconditional `do_cycle` from the hook, bare reset only when unwired, one `power_cycle`, `cycled = 1`, `recover-cycle FAIL` on error. |

No fence, BO lifetime, scheduling order or MMU mapping semantics
changed; the int39 fault-latency kick, the int40 power-idle fail-closed
suspend and every earlier invariant still gate.

## 3. Gate evidence (computed inside the build)

### 3.1 Prior gates intact (24/24 PASS)

Carried from int40: ops layout, irq, slot, abi, gpuwrite, coherency,
instrtype, iova, armfence, ptewalk, pmget, timedout, submitsnap,
syncobj-wait, jddecode, multidep, serialfence, cachedir, mmaprefs,
chainsplit, defectb, chainsplit-length, fault-latency and 6aa (§3.3,
retargeted). Unchanged computed constants include ops=24/slot=232/ran=236,
FS→JS0 / req0→JS1 / ONLY_COMPUTE→JS2, per-node cfg `0x83700`, the single
shmem page-get/flush/store triple (object and linked vmlinux), 56/192/
192/64/32 chain lengths, and the single delay-0 timeout kick with the
5 s backstop. `p7580_drv_submit` still carries exactly one `gem_create`,
one `drm_sched_job_alloc`, one `drm_sched_job_submit`, two
`p7580_drv_gpu_read` and one `shadow_put` call site.

### 3.2 New 6ab: first-fault instrumentation + hygiene + serialize_fs (PASS)

Applied-tree anchors (line numbers are the applied files):

```
job.h:165   #define P7580_EV_MAX	128u
job.h:274   struct p7580_ev ev[P7580_EV_MAX];
job.h:276   unsigned int ev_arm;
job.h:277   unsigned int ev_dumped;
job.h:281   int (*desc_dump)(void *priv, uint64_t gpu, const char *tag);
job.h:287   unsigned int serialize_fs;
job.c:198   void p7580_job_ev(struct p7580_job_dev *jd, unsigned int kind, int slot,
job.c:222   void p7580_job_ev_arm(struct p7580_job_dev *jd, unsigned int on)
job.c:257   void p7580_job_first_fault(struct p7580_job_dev *jd,
job.c:265       jd->ev_dumped = 1;
job.c:616       int armed = jd->ev_arm;
job.c:714           p7580_job_ev(jd, P7580_EV_START, slot, job->seqno, job->jc,
job.c:1090  p7580_job_ev(jd, P7580_EV_TIMEOUT, slot, job->seqno, job->jc,
job.c:1092  p7580_job_first_fault(jd, job, status);
job.c:1813  f = debugfs_create_file("ev_arm", 0600, dir, jd,
job.c:1818  f = debugfs_create_file("serialize_fs", 0600, dir, jd,
job.c:1427  do_cycle = (jd->power_cycle && jd->pm_dev) ? 1 : 0;
job.c:1452      int crc = jd->power_cycle(jd->pm_dev);
job.c:1454      cycled = 1;
job.c:1468  p7580_mmu_program_as(jd->regs, 0, jd->pgd_dma,
job.c:1470  p7580_job_dev_rearm(jd);
job.c:1476  pr_err("p7580_job: recover-done slot=%d latched=0x%08x cycle=%d
submit.c:1078  int p7580_drv_desc_dump(void *priv, uint64_t gpu, const char *tag)
submit.c:1492  job->src_jc = (s->src && s->shadow) ? s->src[pos] : 0;
submit.c:1684  if (slot == 0 && drv->jd.serialize_fs &&
submit.c:1686      entity = &df->entity[1];
submit.c:1817  for (ci = 0; ci < nodes && !need_shadow; ci++) {
submit.c:1818      if (hdr[ci][0] | hdr[ci][1] | hdr[ci][2] | hdr[ci][3]) {
submit.c:1820  pr_info("p7580_job: desc-stale nodes=%u pos=%u src=0x%llx w=%08x %08x %08x %08x (shadowed+sanitised)\n",
submit.c:1857  jobw[0] = 0;   (…:1860 jobw[3] = 0;)
main.c:600  p7580_job_set_desc_dump(&drv->jd, p7580_drv_desc_dump, drv);
```

The recorder body is gated line by line (disarmed early return, masked
ring index, head bump, `spin_lock_irqsave(&jd->hw_lock)`, and the only
timestamp being `ktime_to_ns(ktime_get())`; no `ktime_get_ns(` call
anywhere outside the comment) and each of the six legs exactly once.
The dump gates pin the one-shot guard, the oldest-first masked walk,
all four page tags, the `src_jc != jc` condition, the SFBD type test and
the ≥ 1 MiB heap threshold. The hygiene gates pin `jobw[0..3]` each
exactly once, in the order `isolate → zero → hdr copy → shadow write`,
and the `desc-stale` scan between the pass-through decision and the
shadow allocation. `serialize_fs` gates pin the knob, the setter, the
default 0, the entity override between the entity pick and the in-fence
loop, and the single `job->slot = s->slot` hardware stamp.

Compiled proof (`job.o`, `submit.o`, -O2):

```
nm job.o    T p7580_job_ev 0x460   T p7580_job_ev_arm 0x520
            T p7580_job_first_fault 0x5ec  T p7580_job_set_desc_dump 0x5a0
nm submit.o T p7580_drv_desc_dump 0x16a0
System.map  ffffffc00057f8a0 T p7580_job_ev
            ffffffc00057fa2c T p7580_job_first_fault
            ffffffc00057ace0 T p7580_drv_desc_dump

p7580_job_ev:
  4bc: bl <ktime_get>                     (1 call; ns via ktime_to_ns)
  500: ldr w0, [x8, #5408]                (ev_head)
  504: add w0, w0, #0x1
  508: str w0, [x8, #5408]
  4f8: str w6, [x9, #320]  … 4fc: str w7, [x9, #324]

p7580_job_hw_submit:
  d4c: bl 460 <p7580_job_ev>              (exactly 1 dispatch record)

p7580_drv_desc_dump:
  16c0: adrp x0, <kmalloc_caches>          (GFP_KERNEL)
  16d8: mov x2, #0x1000                    (4096-byte page)
  16fc: bl 328 <p7580_drv_gpu_read>        (one clean→invalidate read)

p7580_job_recover:
  1ea0: blr x1                              (power_cycle hook, indirect)
  no cmp/… #0x41 in the whole function       (cycle is not status-gated)
```

Strings live once each in `job.o`/`submit.o` and in the Image (the
`strings` length prefix shows as `3p7580_job: …`/`6p7580_job: …`):

```
3p7580_job: EVRING begin head=%u n=%u status=0x%08x jc=0x%llx seq=%u src=0x%llx
3p7580_job: EV %-12s dt=%8llu us slot=%u seq=%u jc=0x%llx a=%08x b=%08x c=%08x
3p7580_job: EVRING end
3p7580_job: PAGEDUMP %s gpu=0x%llx rc=%d (unmapped)
3p7580_job: PAGEDUMP %s gpu=0x%llx page=0x%llx off=0x%llx
3p7580_job: PAGE %s slot%02u +%03x …
6p7580_job: desc-stale nodes=%u pos=%u src=0x%llx w=%08x %08x %08x %08x (shadowed+sanitised)
6p7580_job: serialize_fs=%u
6p7580_job: ev_arm=%d (ring %u entries, dump one-shot)
faultdbg ev_arm=%u ev_head=%u ev_dumped=%u serialize_fs=%u
```

Host models: `submit_fix_test` T9 now wires the hook in all four cases
(`l.hook_wired = 1`) and expects one `cycle_calls == 1` for each,
including the silent `JS_NO_EXCEPTION` (0x08, two cases) and our own
`JS_STOPPED`; `chainsplit_test` T1 pins that `isolate()` leaves words
0..3 alone and the shadow copy zeroes them. Both pass (§1).

### 3.3 6aa retargeted: every recovery ends in a full power-on (PASS)

The int40 rule ("cycle on 0x41 or a surviving latch") compiled into a
status compare in `p7580_job_recover()`. int41 replaces it with the hook
test alone, so the gates now assert:

- `do_cycle = (jd->power_cycle && jd->pm_dev) ? 1 : 0;` (job.c:1427);
- `if (!do_cycle) {` bare-reset fallback (:1428) **after** the decision
  and **before** `if (do_cycle) {` (:1451);
- `jd->power_cycle(jd->pm_dev)` (:1452), `cycled = 1` (:1454), AS0
  (:1468), `p7580_job_dev_rearm` (:1470), `recover-done` (:1476);
- **no** `status == P7580_JS_STATUS_POWER_FAULT` and **no**
  `p7580_job_slots_latched_fault(jd)` inside recover (the rescan is
  resume-only, main.c), and no `#0x41` immediate in the compiled
  function;
- the resume path still cycles a latched fault before AS0.

The rest of int40's 6aa (fail-closed cores-off with three PWRON writes +
`-EBUSY`, `runtime_suspend` propagating before `clk_off`, the driver
guard before dvfs/power, two ordered `dsb ld` JS_STATUS reads in
`is_idle`, `power_cycle` doing cores-off → clk-off → power-on) is
unchanged and passes.

### 3.4 Config closure (d0+d4)

After `olddefconfig`, every non-comment line of both fragments survives
verbatim (7/7):

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

- `Image-int41` (18193592 B):
  `c6ec1809b7e44de56fbf7f001b257d14b05fa2d9f994ed7d1b6f91b7858a28e0`
- `kernel-boot-int41.img` (27332608 B):
  `105bf35fe940848f74496927c8ad2d9a066b46db0e8730ddc361bbf20ed55134`
- build log:
  `4f203ae5064c688f3746768ef6d99f2f8f966dcd8d38aa63d0f0c3edd3cbb540`
- gate stdout log:
  `a13492083af731091fe62c57ba9185a6185e905c341d2550b69490655857f314`
- evidence log:
  `27802017e9485cd5d1102d03a15b224a5fd2b452dc16f29077dbacb94b71584e`
- host-test log:
  `91007da2edb706a24eb9523859d7fea5cffbc64519bd157b15d1f7ce9f1d1567`
- sched-host log:
  `31b47baf637c08cca84a07ecc09cd5b7b8e4348c13c0217bb8f62f747678459d`
- script:
  `d2b342a66fbb343232608994a94bd275b7493b94b5c684f58e725e76901704d2`

All paths under
`/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31/`.
No flash per ticket.

## 5. Silicon acceptance (Main, after flashing int41)

Same plan as `gpu31-firstfault.md` §9, unchanged by this build:

1. **Boot/probe**: render node present, driver bound, no probe/MMIO
   errors; `ls …/p7580/` shows `ev_arm`, `serialize_fs` (both read 0).
2. **Capture**: `dmesg -c`, `echo 1 > …/p7580/ev_arm`, 300 frames at
   256² no-readback with `serialize_fs=0`; at the first fault/timeout
   expect exactly one `EVRING begin … EVRING end` sequence and
   `PAGEDUMP jc` (+ `src`/`fbd`/`heap` when the job ran from a shadow),
   each as 16 `PAGE` lines. `faultdbg ev_arm=1 …` in the dump knob
   confirms the state. A second fault must not print a second capture
   until `ev_arm` is written again.
3. **Ordering A/B**: three runs of 300 frames with `serialize_fs=1`;
   acceptance is `ERR=0`, `TIMEOUT=0`, `cycle=0` and fps comparable to
   the int40 clean baseline (~21 fps 256² no-readback). If 1 is clean
   and 0 faults, the FS/tiler ordering hypothesis in
   `gpu31-firstfault.md` §6.3 is confirmed.
4. **Recovery**: any genuine fault must now show
   `recover-done … cycle=1` for **every** status (including
   `latched=0x00000008`), `js0`/`js1` clear and no `0x41` ERR on the
   next START; a `0x41` one job later contradicts §3.3.
5. **Hygiene**: a `desc-stale … (shadowed+sanitised)` line is expected
   only when Mesa recycles a descriptor page that still carries a JM
   fault status; its absence in a clean run is correct.

## 6. Script notes

- **6aa is a retarget, not an addition**: the int40 block asserted the
  conditional cycle, which the driver no longer contains; leaving it
  would have failed the build on correct code. The replacement asserts
  the opposite property (no status gate, hook-only cycle, bare reset
  only as fallback) in tree, patch mirror and object.
- **Dry-run corrections before the final build** (all found by
  pre-staging the patches and compiling `job.o`/`submit.o` from the
  applied tree, then running the 6ab text gates against it):
  1. the dumper is registered in **main.c:600**, not submit.c;
  2. `kmalloc(4096, GFP_KERNEL)` inlines to `kmem_cache_alloc_trace`
     (`adrp kmalloc_caches`), so the alloc gate matches that too;
  3. `grep -c 'job->slot = s->slot;'` also matches
     `sched_job->slot = s->slot;` — the stamp gate is now anchored at
     line start;
  4. the `ktime_get_ns` absence check matched the explanatory comment;
     it now ignores comment lines and looks for a call;
  5. `job.h` reuses the `JOBH` variable for `jm_hw.h` in 6y, so 6ab
     uses its own `FFH`/`FFC`/`FFS`.
- Retained from int38–int40: new-file hunk `+count` integrity before any
  `git apply`, `make` RC 0 + Image newer than build start, defectb
  loop-adjacency proof, fault-latency kick, config closure, and the
  driver-0001 drift re-check at 6v.
- Host tests run from `$EXT/hosttest-int41` (deleted); sched tests run
  in-tree under `code/gpu31/sched`. Scratch, stage and host-test dirs
  are deleted after the pack; logs, Image and boot.img remain on the
  external disk.
