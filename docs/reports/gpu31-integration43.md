# gpu31-integration43 — one BO publish at SUBMIT in serial order; fault capture gets fbd/jc_age; 27/27 gates

Date: 2026-11-22. Owner: G31_Integrator43. Phone: on int42; this
ticket builds host-side only. No flash per ticket (Main/Storm58 flash
and run acceptance).

Consumes **3104a90** (gpu31-storm58): the 0002 shim's
`dma_resv_add_excl_fence` REPLACES the resv's single exclusive fence, so
the last publisher must be the newest submit; the old arm-time
re-publish in `run_job` could land an older node's fence after a newer
SUBMIT's fence, and Mesa's BO-cache `WAIT_BO` then saw a BO idle while
its job was in flight and rebuilt the next frame into it (torn SFBD →
silent hang → permanent 0x58 cycle). BO exclusivity is now published
only in SUBMIT, inside `df->serial_lock`, still before the ioctl
returns. The fault capture also stops reading `desc[10]` out of bounds:
the fragment SFBD is read at the chain walk and `jc_age` comes from a
bounded per-file ledger. Driver 0001 at build time: sha256
`58972560f3a5ec422f8e9a698431000b63a9e621751f7ad4ada170b76ee05676`,
repo HEAD `3104a90`.

## 0. TL;DR

- **Build green**: fresh scratch tree on the external disk, `-j4`
  Image, RC 0, 0 `error:` lines, 5397-line log ending
  `OBJCOPY arch/arm64/boot/Image`; 499 warnings (same ambient count as
  int41/int42, including the pre-existing `submit.c` 2256-byte frame
  warning).
- **27/27 gate banners PASS, 0 FAIL**: the 26 int42 gates with 6j/6z/6w/
  6ab retargeted to the new code shape, plus new **6ad**.
- **6ad / retargets, in the tree, the mirror, the objects and the Image**:
  - exactly one `dma_resv_add_excl_fence(` in submit.c (:2092), inside
    `if (queued) {` (:2082) → `mutex_lock(&df->serial_lock)` (:2085) →
    `if (last_fence) {` (:2086) → publish (:2092) → unlock (:2101),
    before `err_split:` (:2106); the publish is still before the ioctl
    returns, and the queue-time `dma_resv_get_excl` leg (:1639) keeps
    later submits ordered behind it;
  - `publish_excl` is gone from submit.c/job.h/drv.h and the mirror;
    `p7580_drv_run_job` (submit.c:159) publishes nothing (object: 0
    `dma_resv_add_excl_fence` refs; `p7580_drv_submit`: exactly 1);
  - capture: `job->fbd` (job.h:149) and `job->jc_age` (job.h:150); the
    SFBD is read at the chain walk from descriptor word 10 for type-9
    nodes (`walk + 40u`, submit.c:1861) and the dump gates on
    `if (job->fbd)` (job.c:322); `job->desc[10]` is gone (the array is
    8 words — on int42 it dumped an unrelated page);
  - `jc_age` is the SUBMIT age of the BO holding `jc`, from a 64-entry
    direct-mapped per-file ledger (`P7580_BO_AGE_SLOTS`, drv.h:66;
    `submit_no` drv.h:97; `bo_age[]` drv.h:101;
    `p7580_drv_bo_age_touch` submit.c:1489, masked index, delta age);
    stamped once per SUBMIT (:1834) for every submitted BO (:1836);
  - `EVRING begin` now reports `fbd=0x%x jc_age=%u` (job.c:288) and
    `TIMEOUT` reports `jc_age=%u` (job.c:1158); the `PAGE` dump prints
    the whole 4 KiB as 64 lines of 64 bytes with the true offset
    (`slot=i/4`, `off=i*64`, submit.c:1122–1126) — int42's form printed
    1 KiB of every 256-byte slot with wrong offsets and skipped the
    interesting bytes.
- **Host T10** pins the publish-order rule (replace semantics: the
  newest submit's fence wins; the async re-publish makes a busy BO read
  idle); T1–T9 unchanged. `job_test`, `submit_fix_test`,
  `chainsplit_test`, the sched host suite and kcheck all green.
- **Config**: 7/7 d0+d4 fragment symbols survive `olddefconfig`
  verbatim.
- Outputs: `Image-int43` `04ba9d45…`, `kernel-boot-int43.img`
  `f9b6f10f…` (§4).

## 1. Build

Script `code/gpu31/series/build-int43.sh` (3175 lines; sha256
`ae2597bece37519c82498833894a8151bc8b230279b9a8ff04c5caaddd8775b1`),
a copy of int42 with int43 names, step **6ad**, and the 6j/6z/6w/6ab
retargets; 6aa/6ac are unchanged.

Fresh `cp -a` of the pristine tree →
`…/j7k/gpu31/scratch-int43` (birth 17:14:29, tree mtime 17:18:37);
scratch and stage deleted after the pack. `TMPDIR` and every output are
on the external disk; nothing was written to `/tmp` or `/`.

Patches, each `git apply --check` first: backport shims 0001–0005,
Mode-B render node 0001, dmabuf CMA writecombine 0001, driver 0001+0002.
Driver 0001 (`58972560…`) is hashed before apply and re-checked at 6v;
the new-file hunk `+count` integrity check ran first and passed.

Config: pmaports `config-samsung-j7elte.aarch64` + `d0-m1-minimal` +
`d4-panfrost7580`, then `yes "" | make ARCH=arm64 olddefconfig`
(7/7 closure).

Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
KCFLAGS="-Wno-error" -j4 Image`.

- Build log: `…/logs/gpu31-int43-build.log` (5397 lines, 0 `error:`,
  499 `warning:`; tail `SORTEX vmlinux`/`SYSMAP System.map`/
  `OBJCOPY arch/arm64/boot/Image`). Make RC 0 and Image mtime
  (17:18:37, gated) ≥ build start are enforced.
- Gate stdout: `…/logs/gpu31-int43-gates.log` (907 lines, 27 PASS
  banners, 0 `GATE FAIL`).
- Evidence: `…/logs/gpu31-int43-evidence.log` (950 lines).
- Host tests: `…/logs/gpu31-int43-hosttest.log`
  (`submit_fix_test: all checks passed` — T1–T10 —,
  `chainsplit_test: all checks passed`),
  `…/logs/gpu31-int43-schedhost.log` (2× `409 checks, 0 failures`,
  `gates: all PASS`, kcheck PASS),
  `…/logs/gpu31-int43-jobtest.log` (`job_test: all checks passed`).
- Pack: stock `dt.img` + reference `ramdisk-new.gz` (never rebuilt) +
  the built Image → 27348992 B = kernel 18209976 + ramdisk 8822392 +
  dt 313344 (+3280 header).

Source provenance (evidence:421): `repo HEAD=3104a90 driver-0001
sha256=58972560…`. The report commit does not touch driver 0001; any
later edit to it invalidates §4.

Freshness: the pristine tree has no `drivers/gpu/drm/drm_sched.c` and no
`drivers/gpu/drm/panfrost/` directory. `Image-int43` has the same
18209976-byte size as int42; 10,430,418 bytes differ in the common
prefix.

## 2. What changed in the image

int42's acceptance left one defect: 4 of 7 runs still entered a
0x58/0x41 storm whose first fault was silent. gpu31-storm58 traced it
to the second, asynchronous BO-fence publication (report
`gpu31-storm58.md`): with one exclusive slot and replace semantics,
the draws' tiler fence could overwrite the newer FS fence published by
SUBMIT, so `WAIT_BO` (timeout 0) reported the BO idle while the FS job
was still reading it and Mesa rebuilt the next frame into the same
page. int43 carries the fix and the capture that makes the next
occurrence self-describing.

| Applied site | Change |
|---|---|
| `submit.c:2030` comment + `:2082–2101` | BO publish moved into the `if (queued)` serial section: `mutex_lock(&df->serial_lock)`, `if (last_fence)`, per-BO `dma_resv_add_excl_fence(&to_p7580_bo(o)->resv, last_fence)`, `serial_prev` swap, unlock — one site, newest-in-serial-order wins. |
| `submit.c:159` `p7580_drv_run_job` | the arm-time excl re-publish (and `publish_excl`) removed; run_job only dispatches. |
| `submit.c:1639` (split_queue) | queue-time `dma_resv_get_excl` → `drm_sched_job_add_dependency`, unchanged, keeps later submits behind the published fence. |
| `job.h:149–150` | `uint32_t fbd; uint32_t jc_age;`. |
| `submit.c:1489–1499` | `p7580_drv_bo_age_touch()`: `(iova >> 12) & 63` slot, delta age if the same IOVA, then record the newest SUBMIT. |
| `submit.c:1818–1836` | per-SUBMIT `submit_no++` and one `ages[i]` stamp per submitted BO (bounded by `P7580_MAX_SUBMIT_BOS`, no allocation). |
| `submit.c:1549/1554` | split node stamps `fbd = s->fbd[pos]` and `jc_age` from the BO that contains `jc`. |
| `submit.c:1859–1862` | type-9 node: read descriptor word 10 (`walk + 40u`) into `fbds[nodes]` (0 when unreadable). |
| `submit.c:2006–2007` | `sctx.fbd = fbds; sctx.age = ages;`. |
| `job.c:288–294` | `EVRING begin … fbd=0x%x jc_age=%u` (fbd/jc_age from the job). |
| `job.c:322–324` | SFBD page dump gated on `job->fbd` (no `desc[10]`). |
| `job.c:1158–1162` | `TIMEOUT … seq=%u jc_age=%u jc=0x%llx …`. |
| `submit.c:1091–1128` | `PAGE` line: `off=%03x`, `slot=i/4`, `off=i*64`, 64 lines × 16 words = the whole 4 KiB. |
| `drv.h:66/97/101` | `P7580_BO_AGE_SLOTS 64`, `submit_no`, `bo_age[64]`. |

No fence lifetime, BO reference, MMU or scheduling change: the publish
site moved inside the serial lock, the get_excl dependency and the
out_sync install are untouched, and the recovery/timeout path (int42's
750 ms budget and hang inject) is unchanged.

## 3. Gate evidence (computed inside the build)

### 3.1 Prior gates: 26/26 with four retargets

Carried: ops layout, irq, slot, abi, gpuwrite, coherency, **6j
instrtype** (retargeted), iova, armfence, ptewalk, pmget, timedout,
submitsnap, syncobj-wait, jddecode, multidep, serialfence, cachedir,
mmaprefs, **6w chainsplit** (retargeted), defectb, chainsplit-length,
**6z fault-latency** (retargeted), 6aa, 6ab (retargeted), 6ac.
Unchanged constants: ops=24/slot=232/ran=236, FS→JS0/req0→JS1/
ONLY_COMPUTE→JS2, cfg `0x83700`, 56/192/192/64/32 lengths, 750 ms seed +
knobs, ev-ring/recovery invariants.

Retargets (the old assertions describe code that is now wrong):

| Gate | Was | Now |
|---|---|---|
| 6j | `bl add_excl` in `p7580_drv_run_job`; `add_excl` count 1 matched the definition only | exactly one `add_excl(` in submit.c, one `bl` in `p7580_drv_submit`, zero in `p7580_drv_run_job`, and the split_queue `job->bo_count = s->nr` + `dma_resv_get_excl` leg |
| 6z | 2 `add_excl` sites + `publish_excl` guard | 1 site, `if (last_fence)`, no `publish_excl`, mirror count 1, `p7580_drv_submit` exactly 1 |
| 6w | `p7580_drv_gpu_read` twice (walk + payload) | 3 times (walk, payload, fragment fbd capture) |
| 6ab | `PAGE … +%03x`; SFBD via `desc[4]`/`desc[10]` | `PAGE … off=%03x`; `if (job->fbd)` and **no `job->desc[10]`** in job.c |

### 3.2 New 6ad: single serial publish + fault capture (PASS)

Applied-tree anchors:

```
submit.c:2082   if (queued) {
submit.c:2085   mutex_lock(&df->serial_lock);
submit.c:2086   if (last_fence) {
submit.c:2092   dma_resv_add_excl_fence(
submit.c:2101   mutex_unlock(&df->serial_lock);
submit.c:2106   err_split:
submit.c:1639   struct dma_fence *f = dma_resv_get_excl(&bo->resv);
job.h:149       uint32_t fbd;
job.h:150       uint32_t jc_age;
drv.h:66        #define P7580_BO_AGE_SLOTS	64u
drv.h:97        uint32_t submit_no;
drv.h:101       } bo_age[P7580_BO_AGE_SLOTS];
submit.c:1489   static uint32_t p7580_drv_bo_age_touch(struct p7580_drv_file *df,
submit.c:1496–99    masked index / delta / record
submit.c:1549   job->fbd = s->fbd ? s->fbd[pos] : 0;
submit.c:1554   job->jc_age = s->age ? s->age[i] : 0;
submit.c:1834   df->submit_no++;
submit.c:1836   ages[i] = p7580_drv_bo_age_touch(
submit.c:1859   fbds[nodes] = 0;
submit.c:1861   (void)p7580_drv_gpu_read(drv, walk + 40u,
submit.c:2006   sctx.fbd = fbds;
submit.c:2007   sctx.age = ages;
job.c:288       EVRING begin … fbd=0x%x jc_age=%u
job.c:322       if (job->fbd)
job.c:1158      TIMEOUT … seq=%u jc_age=%u jc=0x%llx …
submit.c:1125   PAGE %s slot%02u off=%03x …
submit.c:1126   tag, (unsigned int)(i / 4u), (unsigned int)(i * 64u),
```

The block proves the publish is *inside* the lock and before the ioctl
returns by line order (`queued 2082 < lock 2085 < guard 2086 < publish
2092 < unlock 2101 < err_split 2106`), that no other publish exists
(tree, mirror, both objects), that `publish_excl` is gone everywhere,
and that the queue-time get_excl ordering leg remains. The capture
gates pin the masked 64-entry ledger, the once-per-SUBMIT stamp, the
word-10 read only for type-9 nodes, the two new job fields, the three
new format strings, the absence of `job->desc[10]`, and the corrected
PAGE offset math.

Compiled proof (`submit.o`, `job.o`, `-O2`):

```
submit.o   U dma_resv_add_excl_fence
p7580_drv_submit:
    2bec: bl <dma_resv_add_excl_fence>        (exactly 1)
p7580_drv_run_job:
    (0 refs to dma_resv_add_excl_fence)
p7580_drv_submit: 3 bl <p7580_drv_gpu_read>  (walk, payload, fbd)
```

Strings live in `job.o`/`submit.o` and the Image:

```
3p7580_job: EVRING begin head=%u n=%u status=0x%08x jc=0x%llx seq=%u src=0x%llx fbd=0x%x jc_age=%u
3p7580_job: TIMEOUT slot=%d js_status=0x%08x (%s) stat=0x%08x rawstat=0x%08x head=0x%llx tail=0x%llx seq=%u jc_age=%u jc=0x%llx cfg=0x%08x sw=%08x …
3p7580_job: PAGE %s slot%02u off=%03x %08x …
```

Host T10 (`run_t10`): models the replace semantics with `fake_resv`;
SUBMIT(draws) → SUBMIT(fs) leaves the FS fence; the tiler retiring does
not make the BO read idle; the old arm-time re-publish (`draws` again
after `fs`) reproduces the "idle with fs live" bug. All checks pass.

### 3.3 Config closure (d0+d4)

7/7 fragment lines survive `olddefconfig` verbatim
(`CONFIG_MODULES`, `DRM`, `DRM_KMS_HELPER`, `DRM_EXYNOS`,
`DRM_EXYNOS_DMABUF`, `DRM_EXYNOS_G2D`, `CONFIG_DRM_PANFROST7580=y`).

## 4. Outputs (sha256)

- `Image-int43` (18209976 B):
  `04ba9d45d72bd658a5e0512b4b7624afdbad5dc8116217c868977c6f68fec72e`
- `kernel-boot-int43.img` (27348992 B):
  `f9b6f10fec60067b0ba0b7c94e054c447cbc80f7a1013ef18e536039a271c948`
- build log:
  `eee2f4de195a46f87d96820d5232331dd38f84503aa4f1217ac751157b086183`
- gate stdout log:
  `b84d3beac6b4498300cef99aa2c68547def967f4c5e584051f40026a98dbd9fe`
- evidence log:
  `755af9784bfa1c8855d172a0457e496316f423e04a01ddd4d247256a68af180e`
- host-test log:
  `91007da2edb706a24eb9523859d7fea5cffbc64519bd157b15d1f7ce9f1d1567`
- sched-host log:
  `e59a1dff12bb7df24b251a4a2701ab5ff65fc2c9847f18dd9b85fdc98895ce2a`
- jobs-host log:
  `ada85c46c81086468fc7cf33cff9ce53e221b9dbc59754e2433ec639ae10d68b`
- script:
  `ae2597bece37519c82498833894a8151bc8b230279b9a8ff04c5caaddd8775b1`

All paths under
`/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31/`.
No flash per ticket.

## 5. Silicon acceptance (Storm58/Main, after flashing int43)

Loop from `gpu31-storm58.md` §6: `dmesg -c`, `ev_arm=1`, then ten runs
of 300 frames at 256² no-readback, plain (no `PAN_MESA_DEBUG`), under
`flock`. Pass = every run `ERR=0`, `TIMEOUT=0`, `EVRING=0` (no capture)
and wall 1.9–2.1 s. Any `desc-stale` line means Mesa recycled a page
that still carried a JM fault status — separate from this fix.

Optional deterministic check (still on int42, or on int43 for the
post-fix contrast): `PAN_FBD_DEBUG=1` + `hang_inject=4`; on int42 the
slab holding `jc` is FETCHed repeatedly during the 750 ms window; on
int43 it must not be. With int43, the one-shot capture (if a fault ever
fires) now reports `fbd=0x… jc_age=N` in `EVRING begin` and
`jc_age=N` in `TIMEOUT`; a `jc_age` of 1–2 at fault time on a job still
in flight is the userspace-recycle signature the fix is meant to
prevent.

## 6. Script notes

- **First full run failed at 6j on correct code**: the old gate
  asserted `rj->bo_count` in `run_job`, but the fix deletes that whole
  arm-time block, and the remaining BO walk lives in
  `p7580_drv_split_queue`. The gate now checks the split stamp
  (`job->bo_count = s->nr;`) and the queue-time get_excl leg instead.
  The scratch tree from that run was retained and the **full gate
  suite was re-run against it** (steps 6b–6ad, 0 failures) before the
  final build; the committed script is the one that produced §4.
- Other dry-run corrections:
  1. `grep -c 'dma_resv_add_excl_fence'` counts the explanatory
     comment too — the call-site count must use the opening paren;
  2. `p7580_drv_run_job`'s signature is `static struct dma_fence *`
     (the old `^static int` awk range silently matched nothing);
  3. `p7580_drv_gpu_read` is now called 3× in `p7580_drv_submit`
     (fbd capture), so 6w counts 3;
  4. a missing closing quote in the new 6ad string loop was caught by
     `bash -n`.
- Retained: hunk `+count` integrity, RC/mtime freshness, defectb loop
  proof, config closure, driver-0001 drift re-check, and every int42
  gate (6ac included, untouched).
