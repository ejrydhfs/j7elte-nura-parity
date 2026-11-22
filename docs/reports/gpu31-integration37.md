# gpu31-integration37 — chainsplit (driver-side per-node chain split), build fixes, all prior gates intact

Date: 2026-11-21. Owner: G31_Integrator37. Phone: on int36 (Mesa
patched); this ticket builds only — Main flashes. No flash per ticket.

Consumes gpu31-chainsplit (report `gpu31-chainsplit.md`, committed as
**6fe2491**) plus the build fixes this ticket had to make (**9da3df8**)
on top of int36 (de21448 mmaprefs): `p7580_drv_submit` walks the
submitted jc chain through the descriptor `next` pointers, isolates
every node into a driver-owned shadow BO slot (`next = NULL`,
`dep1 = dep2 = 0`, `index = pos + 1`), and queues one sched job per
node on the SUBMIT's existing slot entity. Node *i* depends on node
*i-1*'s armed fence, so each node is a fresh START and
`p7580_js_config()` (START/END `FLUSH_CLEAN_INV`) brackets every stage.
The UAPI `out_sync` is installed exactly once, on the **last** queued
node's fence, and that fence becomes the file's serial predecessor. A
one-node chain with `dep1 == dep2 == 0` (Mesa's fragment submit) keeps
the original descriptor and allocates no shadow BO.

## 0. TL;DR

The chainsplit hand-off **did not compile as committed**. Two compile
defects in driver `0001` were fixed in **9da3df8** (section 2): an
assignment of the void-returning `dma_fence_get()` of this tree's 3.10
shim, and two `goto` labels lost in the SUBMIT rewrite. The submit.c
hunk count was advanced 1965 → 1969 only because those two fixes insert
four lines (1965 was correct for the file as committed; see the
erratum in section 2). With those fixes the -j4 build is green: 20/20
gate banners PASS, 0 FAIL, 0
compiler errors, host `chainsplit_test` + `submit_fix_test` pass, and
the split is proven live in `submit.o` (`p7580_drv_gpu_read` /
`p7580_drv_shadow_put` outlined, one shadow `p7580_gem_create` and one
job alloc/arm/submit in the inlined per-node loop, single
`replace_fence`, cfg `0x83700` folded in `hw_submit`). No source changes
beyond those two fixes (plus the hunk-count bump they require) and the
int37 script + this report.

## 1. Build

Script: `code/gpu31/series/build-int37.sh` (committed **7bd0399**;
sha256 `d5e39b7a6cd1a03f6f382a61a5bb2628089f458c3f8c3b70dc2d3eddd51adb2c`).
Fresh `cp -a` of the pristine tree → `scratch-int37` (birth
22:36:14, tree mtime 22:41:08), all patches `--check` green (backport
0001–0005 ×5, Mode-B ×1, dmabuf ×1, driver 0001+0002), config =
pmaports `config-samsung-j7elte.aarch64` + d0-m1-minimal +
d4-panfrost7580 fragments + `olddefconfig`, `-j4` Image build, RC=0.

Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
KCFLAGS="-Wno-error" -j4 Image`.
Log: `…/j7k/gpu31/logs/gpu31-int37-build.log` (5393 lines, 0
`error:`, ends `OBJCOPY arch/arm64/boot/Image`; the only link
warnings are the ambient `vmlinux has a LOAD segment with RWX
permissions` ones, 3×, as in int33–int36).
Pack: stock dt.img + reference ramdisk-new.gz (never rebuilt),
27332608 B = kernel 18193592 + ramdisk 8822392 + dt 313344
(+3280 header), built by `stage-int37/mkboot.py`. Same byte length as
int36 but a different build: 10,266,992 bytes differ between
`Image-int36` and `Image-int37`.
Console: build stdout (RC=0, **20 PASS notes, 0 FAIL**); evidence:
`…/logs/gpu31-int37-evidence.log` (506 lines). Host tests:
`…/logs/gpu31-int37-hosttest.log`. Scratch + stage + host-test dir
deleted after the pack; kept: `Image-int37`, `kernel-boot-int37.img`,
the three logs, `offstage-int37`.

Source provenance (printed into the evidence log):
build-time repo HEAD **9da3df8**; driver 0001 sha256
`d8aaaf083b379fc55c586ab3c3b5302177f8ff11fa137c59530586b3a7114e94`
(the script records it before `git apply` and re-checks it at 6v, so an
in-flight patch edit fails the build). The script was committed after
the build (7bd0399) and this report after that; the driver-0001 hash is
unchanged since the build.

Freshness (evidence tail): scratch birth/mtime as above; the pristine
tree has no `drivers/gpu/drm/drm_sched.c` and no
`drivers/gpu/drm/panfrost/` directory (the shim and the driver exist
only because the patches created them).

## 2. Hand-off defects fixed in driver 0001 (9da3df8)

The chainsplit commit's regenerated `panfrost7580_submit.c` was not
buildable. A probe tree (patches + config + panfrost objects only)
reproduced two compile defects before the full run:

1. **Void `dma_fence_get()`.** `fence_ref = dma_fence_get(
   sched_job->fence);` — this tree's 3.10 shim defines
   `dma_fence_get()` as `static inline void` (`include/linux/
   dma-fence.h:120`), so the probe failed with
   `…:1490:27: error: invalid use of void expression`. Fixed by
   binding the armed fence and taking the reference in a statement
   (`fence_ref = sched_job->fence; dma_fence_get(fence_ref);`), the
   same shape `run_job` already uses.
2. **Lost exit labels.** The rewritten SUBMIT still `goto err_inf;`
   (in-sync lookup) and `goto err_objs;` (BO lookup/map) but both
   labels were dropped in the rewrite; the probe failed with
   `…:1650:33: error: label 'err_objs' used but not defined` and
   `…:1620:33: error: label 'err_inf' used but not defined`. Fixed by
   restoring `err_objs:` on the BO put loop and `err_inf:` on the
   in-fence put loop at the function's single exit.

Neither alters the split design: the probe then compiled
`panfrost7580_submit.o`, and the full build's object fingerprint
matches the intended code (section 3.3). `git apply --check` of the
fixed driver 0001 is clean, and the host `chainsplit_test` still passes
against the unchanged pure-C helpers.

**Hunk count (not a defect in 6fe2491).** The two fixes above insert
four lines, so the submit.c hunk header was advanced from `1965` to
`1969` in the same commit. The committed `1965` was correct for the
file as committed: its hunk carries exactly 1965 `+` lines and the
applied file is 1965 lines ending with the function's closing brace
(brace balance 0; G31_ChainSplit independently confirmed this in a
clean scratch tree). During the fix the header was still `1965` for a
moment after the four lines were inserted, and that intermediate state
is what produced the probe's
`panfrost7580_submit.c:1965:17: error: expected declaration or
statement at end of input` (git apply honoured the stale count and
dropped the last four lines). Earlier drafts of this report attributed
that error to the committed patch; that attribution was wrong.

**Erratum for 9da3df8's message.** The commit subject/body list the
hunk count as a defect ("declared 1965 lines for a 1966-line file").
That statement is incorrect: 1965 was right, and the count was bumped
only to track the four inserted lines. The applied content of 9da3df8
is correct and unchanged; this report is the authoritative account.

## 3. Gate evidence (computed inside the build, not inferred)

### 3.1 Prior gates intact

All 20 int36 gate sections still assert their invariants and PASS in
int37 (6b/6c/6e/6f/6g/6h/6i/6j/6m/6n/6o/6q/6v unchanged at their
original sites; 6k/6l/6p/6r/6s/6t/6u retargeted to the split's code
shape with the same invariant, section 3.2):
ops layout **ops=24 slot=232 ran=236**, single defs, numeric ST-OPS/
`runexp`, ST-RUN/bails/retire/finish, weak/strong `run_job_addr`; IRQ
(`(status|rawstat)` demux, rearm + AS0 program on recover/resume); slot
routing (FS→JS0, req0→JS1, ONLY_COMPUTE→JS2); ABI-align
(`BUILD_BUG_ON` pins, SYNC_FILE tolerance, copy caps); gpuwrite (Zero
contract, `0xCF` fops route, 6-row table); coherency (3 pre-signal call
sites, `bl` in IRQ/timedout/recover, post-signal `free_job` walk);
errtrace (`ERR`/`TIMEOUT`/`trace_arm`/`dump`, `Image p7580_job: ERR =
2`; this gate reports through its evidence lines and has no PASS
banner, as in int35/int36); instrtype (`dma_resv_add_excl_fence` in
run_job, shim-owned lock); ptewalk (snap/live owner, unconditional
8-word dump); pmget (5 sites, 1 inline `< 0`); timedout probes (`TMD-`
count = 3); subsync; syncobj-wait (WAIT_ALL re-test, writable
signaled-ops, `WAIT-RECHECK`); jddecode (`bo_count`, JDBODY count = 3);
multidep (AND node, staged errno, no `EOPNOTSUPP`); armfence;
mmaprefs 6v. Config-computed values unchanged: dma_map_ops slots
for_cpu=64/for_device=72/unmap=40, dirs TO=1/FROM=2; vm_flags
PFNMAP=0x400, MIXEDMAP|DONTCOPY=0x10020000.

### 3.2 Gates retargeted to the split code shape (same invariant)

The chainsplit rewrite legitimately moved code, so six prior gate
blocks now assert the same invariant at its new site:

- **6k iova.** `map_bo` is outlined at -O2 now (`t
  p7580_drv_map_bo`), so the two invalidate probes are counted inside
  that symbol (`bl p7580_mmu_invalidate` at `0x194` and `0x224`), and
  `p7580_drv_submit` must call `map_bo` twice (BO loop + inlined
  shadow create). `gpu_unmap`'s TLB flush is outlined into
  `p7580_gem_gpu_unmap.part.0`; exactly one invalidate there.
- **6l armfence.** Arm + the armed-fence reference live in
  `p7580_drv_split_queue` (`fence_ref = sched_job->fence;
  dma_fence_get(fence_ref);`), and the install is the split's
  `last_fence` — still exactly **one** `drm_syncobj_replace_fence()`
  call in `submit.c` (source count gated), still armed before the
  dependency attaches (line order gated), `run_job` still binds
  `sched_job->fence` and still carries no install.
- **6p submitsnap.** The descriptor snapshot is now the isolate +
  copy path (`p7580_chain_hdr_isolate(jobw, ci)` then
  `memcpy(job->desc, hdr, sizeof(job->desc))`); `desc[8]`/`sw[8]` and
  every `sw=` print are unchanged (`Image sw= count = 5`). The
  `chain_mode == 2` descriptor-bit rewrite is **retired** from SUBMIT
  (a split node has `next = NULL` and no chain bit to rewrite) and the
  gate now asserts it is absent; variant A (`chain_mode == 1`,
  CONFIG_NEXT chain bit) stays in `hw_submit`.
- **6r jddecode.** The consumer-facing stamp is per node
  (`job->bo_count = s->nr;` inside `p7580_drv_split_queue`), still the
  only place it is written; live-bo_list snapshot lookup and JDBODY
  unchanged.
- **6s multidep.** A dependency-attach failure inside
  `p7580_drv_split_queue` returns through the caller's single staged
  `queue` path (the old literal `"dep"` stage no longer exists), so the
  gate asserts `p7580_submit_err("queue", ret)` plus all three
  `add_dependency` sites bailing to `err_sched` — no silent error path.
- **6t serialfence.** Borrow (`serial_prev = df->serial_prev;` under
  `serial_lock`, line 1800), attach (`add_dependency(sched_job, prev)`,
  line 1501), queued-guarded publish (`df->serial_prev = last_fence;`
  line 1872 inside `if (queued)`, before `err_split:`), exactly two
  `serial_prev = NULL` sites (open init + fini), exactly 3
  `add_dependency` sites and 3 relocs in `p7580_drv_submit`
  (`0x22a8`, `0x230c`, `0x2394`). The old published-pointer rollback is
  gone because nothing is published when `queued == 0`.
- **6u cachedir.** The SUBMIT-time descriptor snapshot was replaced by
  the chain-walk read, so `p7580_drv_gpu_read` carries the single
  `invalidate_for_cpu` (`0x3d8`) and the gate asserts it precedes the
  `kmap_atomic` copy; object-wide count is 2 (read + JDBODY). SUBMIT's
  two `submit_sync` calls are the BO clean loop (`0x1a10`) and the
  shadow-descriptor publish (`0x29bc`); `free_job` keeps
  `job_done_sync` (`0xf40`).

### 3.3 New 6w chainsplit gates (PASS)

Tree (applied `panfrost7580_submit.c`):

- **Walk.** `walk = args->jc`, `next = p7580_chain_hdr_next(hdr[nodes -
  1])`, `if (nodes == P7580_CHAIN_MAX_JOBS)` → `p7580_submit_err(
  "chain", -E2BIG)`, `if (!p7580_jc_valid(next))`, and the grounded
  payload-size rejection `if (!p7580_chain_job_bytes(...))`.
- **Isolate/shadow.** `p7580_chain_hdr_isolate(jobw, ci)`,
  `memcpy(hdr[ci], jobw, 32)`, `p7580_drv_shadow_write(shadow->obj,
  (size_t)ci * P7580_CHAIN_SLOT_STRIDE, …)`, then
  `p7580_gem_submit_sync(shadow->obj)`. In `jm_hw.h`: isolate rewrites
  `w[4] = (w[4] & 0xffff) | ((pos + 1) << 16)` and zeroes `w[5]/w[6]/
  w[7]`; `P7580_CHAIN_MAX_JOBS 16`, `SLOT_STRIDE 256`, `HDR_WORDS 8`.
- **One-node pass-through.** `need_shadow = nodes > 1 || dep1 || dep2`
  and the shadow is only created inside `if (need_shadow)`.
- **Shadow lifetime.** `p7580_drv_shadow_create`, `atomic_set(&sh->refs,
  nodes)`, `atomic_dec_and_test(&sh->refs)` → `p7580_gem_gpu_unmap(
  sh->obj)`, and `free_job`'s `if (job && job->chain_shadow)` release.
- **Per-node queue/order/refs.** `p7580_drv_split_queue(&sctx, ci,
  jcs[ci], …)`, `prev = ci ? last_fence : serial_prev`,
  `drm_gem_object_reference(s->objs[i])`, `p7580_drv_job_pm_arm(s->drv)`
  / `…_release(s->drv)`, and the partial-queue unwind `for (ci = queued;
  ci < nodes; ci++) p7580_drv_shadow_put(shadow);` plus the err-path fence
  puts.
- **One out_sync on the last node.** `if (pos + 1 == s->nodes)
  job->out_sync = s->out_sync;` (the only `job->out_sync =` in the
  file) and the single install guard `if (queued && args->out_sync &&
  last_fence)`.
- **Probes.** `p7580_chain_trace = 8`, `p7580_sched: chain jc=… nodes=…
  shadow=… stride=…`, `p7580_sched: split seq=… pos=…/… slot=… jc=…`.
- **Per-node cfg.** `run_job` calls `p7580_job_hw_submit(&drv->jd,
  job)` once; `job.c` computes `cfg = p7580_js_config(job->as_nr)` and
  writes `P7580_JS_CONFIG_NEXT`; `p7580_js_config` carries both
  `P7580_JS_CFG_START_FLUSH_CLEAN_INV` and
  `P7580_JS_CFG_END_FLUSH_CLEAN_INV` (so every split node gets the
  clean+invalidate bracket at its own START).

Object/job evidence (the split helpers are inlined at -O2, so the
proof is the inlined fingerprint plus the two outlined helpers):

- `t p7580_drv_gpu_read` (0x320), `t p7580_drv_shadow_put` (0xde8).
- `p7580_drv_submit`: exactly 1 `p7580_gem_create` (0x24dc, the shadow
  BO), 1 `drm_sched_job_alloc` (0x207c), 1 `drm_sched_job_arm`
  (0x2228), 1 `drm_sched_job_submit` (0x2b6c), 3
  `drm_sched_job_add_dependency`, 1 `drm_syncobj_replace_fence`, 2 `bl
  <p7580_drv_gpu_read>` (0x1ad4, 0x2624: header walk + payload copy),
  1 `bl <p7580_drv_shadow_put>` (0x27b4, unwind), 2
  `p7580_gem_submit_sync`, 1 `p7580_gem_debug_page_ledger`.
- `p7580_drv_shadow_put`: `bl <p7580_gem_gpu_unmap.part.0>` (0xe78) →
  `kfree` (0xe88) → `drm_gem_object_free` (0xea0), i.e. unmap on the
  last node only.
- `p7580_drv_free_job`: 1 `bl <p7580_drv_shadow_put>` and the
  post-signal `job_done_sync` (0xf40).
- `p7580_drv_run_job`: 1 `p7580_job_hw_submit`; `job.o`
  `p7580_job_hw_submit` folds the T720 cfg to
  `mov w0, #0x3700` (0x798) + `movk w0, #0x8, lsl #16` (0x79c) + `orr`
  (0x7a0) = `0x83700` with `as_nr` ORed in.
- Strings in `submit.o` and the linked `Image`: both chain/split
  formats. `System.map`: one `T p7580_drv_submit`
  (`ffffffc00057ad20`), one `t p7580_drv_run_job`
  (`ffffffc000579bc4`).

Host model (staged copy on the external disk, real
`jobs/panfrost7580_jm_hw.h`):
`submit_fix_test: all checks passed`, `chainsplit_test: all checks
passed` (T1 decode/isolate, T2 ordering, T3 single out-fence, T4
unwinding, T5 pass-through, T6 forms/limits).

## 4. Outputs (sha256)

- `Image-int37`:
  `91e86e59a8c17e5206679496597b94494479e55af6dc7efc76e3e7e4956bb91d`
- `kernel-boot-int37.img`:
  `d3f36383ae218d522b3d7f809ca4ba5628f7518785e6189516dfb984e3503abf`
- build log:
  `c364d2d53b2a1c3e77171830ab6e0b7d1db1ae093f24e5060ab84f608b99efeb`
- evidence log:
  `277254354246e94c201e253456c999210410dd031e92a1aeac70fc9148bf953b`
- host-test log:
  `91007da2edb706a24eb9523859d7fea5cffbc64519bd157b15d1f7ce9f1d1567`

Config: pmaports base + d0-m1-minimal + d4-panfrost7580, then
`olddefconfig` closure. No flash per ticket (Main flashes).

## 5. Silicon acceptance (Main, after flashing int37)

The only changed applied file is driver `0001` (plus the chain helpers
appended to `jobs/panfrost7580_jm_hw.h`/`job.h`), so every int36
routing/gate holds; the split adds the probes below.

### 5.1 The split is active

For the first 8 split submits / 8 nodes the kernel prints:

```
p7580_sched: chain jc=0x<user jc> nodes=3 shadow=0x<iova> stride=256
p7580_sched: split seq=<n> pos=0/2 slot=1 jc=0x<iova+0x000>
p7580_sched: split seq=<n+1> pos=1/2 slot=1 jc=0x<iova+0x100>
p7580_sched: split seq=<n+2> pos=2/2 slot=1 jc=0x<iova+0x200>
```

plus three `p7580_sched: run_job … jc=…` lines and three `p7580_job:
dispatch … cfg=0x00083700 …` lines for the split nodes, with `w5=0/w6=0`
in `sw=` (next and deps cleared). Mesa's fragment submit remains one
node (no `split` lines for it). A JS1 `0x41 POWER_FAULT` on a split node
means the JM rejected the isolated descriptor: capture
`p7580_job: irq-err`, `PTEWALK` and the `split`/`dispatch` lines.

### 5.2 Control census (no fragment)

```
rootshell /data/wdarm 15 env PC2_POLLSCAN=1 PC2_REFUSE_FRAG=1 \
    PC2_SCANLOG=/data/chainsplit/scan_ctrl.txt PC2_MAX=1 FRAMES=1 \
    sh /data/polylist/run.sh
```

Pass: the scan reads the VS triangle from `Draw.Position` and
`tiles_nonempty` is **non-zero and not exactly 1** (int36 control was
0). Failure signatures: `0` (no boundary effect), exactly `1` (pre-VS
content still consumed).

### 5.3 Prefill must be superseded

```
rootshell /data/wdarm 15 env PC2_PREFILL=1 PC2_POLLSCAN=1 \
    PC2_REFUSE_FRAG=1 PC2_SCANLOG=/data/chainsplit/scan_prefill.txt \
    PC2_MAX=1 FRAMES=1 sh /data/polylist/run.sh
```

Pass: the census bins the VS triangle (many tiles), not the tiny
`(0,0),(8,0),(0,8)` prefill's single tile 0.

### 5.4 Pixel acceptance (after Defect B is fixed)

```
rootshell /data/wdarm 15 env PC2_MAX=1 FRAMES=1 sh /data/polylist/run.sh
```

With bins present the tiler need not be refused; once the FS ISA page
defect is fixed the centre pixel must be red and the corner clear. The
int36 blocker is the independent `0x52` FS fault, not this split.

### 5.5 Behaviour notes for the device run

- A SUBMIT whose 32-byte descriptor header is not inside any submitted
  BO now fails the ioctl with `submit_err stage=chain rc=-2` (fail
  closed). int36 only left `desc[]` zeroed in that case. Mesa always
  submits its batch BO, so this is an error-path tightening, not a
  functional change.
- `chain_mode=2` (descriptor-bit rewrite) is a no-op on int37; only
  `chain_mode=1` (CONFIG_NEXT chain bit) remains in `hw_submit`.
- Dependency-attach failures inside a split now print
  `submit_err stage=queue` (the per-node build stage) instead of the
  old `stage=dep`.

## 6. Script notes

- Gate thresholds were pinned with a probe tree before the full run
  (patches + config + panfrost/drm_sched objects, ~25 s). That probe is
  what exposed both compile defects (and, in its intermediate state,
  the stale hunk count that had to track the inserted lines), and it
  established the -O2 inline/outline facts the gates depend on:
  `split_queue`, `shadow_create`, `shadow_write` and `gpu_span` are
  inlined into `p7580_drv_submit`; `gpu_read` and `shadow_put` are
  outlined;
  `map_bo` is outlined (it was inlined in int36) and `gpu_unmap` is
  split into a `.part.0`. Gates are written against those facts, with
  the intent stated in each comment so a future codegen change is a
  clear gate failure, not a silent pass.
- The host test runs from a staged copy under `$EXT/hosttest-int37` so
  the repo tree stays clean; the script deletes it (and scratch/stage)
  before exiting.
- The driver-0001 provenance gate (sha256 recorded before `git apply`,
  re-checked in 6v) is what makes this report describe exactly the
  source that was built (`d8aaaf08…`, commit 9da3df8). Any later edit
  to that patch needs a rebuild before its sha256s can be quoted.
