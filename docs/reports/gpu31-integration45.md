# gpu31-integration45 — bo_list entries are weak (no GEM ref), walkers use kref_get_unless_zero, failed binds roll their leaves back; 29/29 gates

Date: 2026-11-22. Owner: G31_Integrator45. Phone: on int44; this ticket
builds host-side only. No flash per ticket (Main/BOLeak flash and run
acceptance).

Consumes **078daa3** (gpu31-boleak; supersedes 8a493b5, which was never
built into an artifact — see §6): `p7580_drv_map_bo` no longer takes a
GEM reference for its `bo_list` entry, and neither
`p7580_gem_gpu_unmap()` nor the main.c remove walker unrefs it. The entry
is now **weak/non-owning**. The old list-owned reference could never be
dropped: its only unlink runs from `p7580_gem_free_object()` →
`p7580_gem_gpu_unmap()`, and `free_object` only runs at refcount zero,
which the list reference itself prevented — every submitted BO (pages +
private shmem inode + IOVA) leaked forever (measured: 1.16 MB/frame of
unevictable Shmem, which is what starved the 10×300 acceptance runs).
Walkers that must use an entry object now take it with
`p7580_drv_gem_try_get()` (`kref_get_unless_zero`) under `bo_lock`, and a
failed bind rolls back the leaves it programmed so "leaves programmed"
stays exactly equivalent to "entry linked". Driver 0001 at build time:
sha256 `20cd0884e182f6823d1be75f8beb400fc6498702d738457e267738830f88cb10`,
repo HEAD `078daa3`; the submit.c new-file hunk is `+1,2258`.

## 0. TL;DR

- **Build green**: fresh scratch tree on the external disk, `-j4`
  Image, RC 0, 0 `error:` lines, 5397-line log ending
  `OBJCOPY arch/arm64/boot/Image`; 499 warnings (same ambient count as
  int41–int44, including the pre-existing `submit.c` frame-size warning).
- **29/29 gate banners PASS, 0 FAIL**: the 28 int44 gates with **6k**
  (map_bo invalidate count) and **6m** (ptewalk live pin) retargeted,
  plus new **6af**.
- **6af / retargets, in the tree, the mirror and the objects**:
  - `drv.h:56` `struct drm_gem_object *obj; /* weak: valid while linked */`;
  - `map_bo` (submit.c:1208) links the entry with **no**
    `drm_gem_object_reference(obj);` — the comment at :1283 explains the
    leak — and `gpu_unmap` (:885) unlinks (`list_del(&e->node);` :918)
    and frees (:920) **without** a put; the main.c remove walker
    (:719–721) is unmap → unlink → free with no unref, and main.c
    contains no `drm_gem_object_unreference` at all;
  - `p7580_drv_gem_try_get()` (submit.c:170) is
    `kref_get_unless_zero(&obj->refcount)` (:174), the **only**
    resurrection point in the file; used by the PTEWALK live pin
    (`if (!p7580_drv_gem_try_get(le->obj))` :469) and `gpu_span`
    (`if (!p7580_drv_gem_try_get(e->obj))` :1387), both inside
    `bo_lock` (gpu_span ordering gated: lock < try_get < unlock);
  - the remaining `drm_gem_object_reference(` call sites are exactly
    two: the shadow's per-node refs (:1489) and `job->bos` (:1646);
  - **rollback**: all three map failures (`page-phys -EFAULT`,
    `mmu_map -ENOMEM`, entry `kzalloc -ENOMEM`) `goto rollback`
    (:1266/:1273/:1283); `rollback:` (:1313) walks 0..i-1
    (`while (k--)` :1325), unmaps them (:1326), publishes the clear with
    an invalidate (:1329) and ends in `return ret;` (:1331) — so a
    failed bind cannot leave PTEs naming pages nobody will unmap.
- **Retargets** (old assertions describe code that is now wrong):
  - 6m: `drm_gem_object_reference(le->obj)` → `p7580_drv_gem_try_get(le->obj)`;
  - 6k: map_bo `p7580_mmu_invalidate(drv->regs` sites 2 → **3** in the
    source (purge, publish, rollback publish); the object carries 4
    relocs because the compiler duplicates one publish path, so the
    compiled gate asserts `>= 3` invalidates **and** exactly one
    `p7580_mmu_unmap` in map_bo (only the rollback unmaps), with the
    invalidate after it.
- **Everything int44 added still holds**: WAIT_BO's 0 → `-ETIMEDOUT`
  (6ae), the single serial BO publish (6j/6z/6ad), fbd/jc_age capture,
  the 750 ms budget + hang_inject, and the ev-ring/recovery rules.
- **Config**: 7/7 d0+d4 fragment symbols survive `olddefconfig`
  verbatim.
- Outputs: `Image-int45` `1fc118af…`, `kernel-boot-int45.img`
  `0855ad12…` (§4).

## 1. Build

Script `code/gpu31/series/build-int45.sh` (3435 lines; sha256
`6cae3d760117d4cb9dd206a53d1104354a24162e4c692b2e5eb3c0378bc1b5e6`),
a copy of int44 with int45 names, step **6af**, the 6k/6m retargets and
a rewritten header.

Fresh `cp -a` of the pristine tree →
`…/j7k/gpu31/scratch-int45` (birth 18:06:20, tree mtime 18:10:37);
scratch and stage deleted after the pack. `TMPDIR` and every output are
on the external disk; nothing was written to `/tmp` or `/`.

Patches, each `git apply --check` first: backport shims 0001–0005,
Mode-B render node 0001, dmabuf CMA writecombine 0001, driver 0001+0002.
Driver 0001 (`20cd0884…`) is hashed before apply and re-checked at 6v;
the new-file hunk `+count` integrity check ran first and passed
(submit.c `+1,2258`).

Config: pmaports `config-samsung-j7elte.aarch64` + `d0-m1-minimal` +
`d4-panfrost7580`, then `yes "" | make ARCH=arm64 olddefconfig`
(7/7 closure).

Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
KCFLAGS="-Wno-error" -j4 Image`.

- Build log: `…/logs/gpu31-int45-build.log` (5397 lines, 0 `error:`,
  499 `warning:`; tail `SORTEX vmlinux`/`SYSMAP System.map`/
  `OBJCOPY arch/arm64/boot/Image`). Make RC 0 and Image mtime
  (18:10:37, gated) ≥ build start are enforced.
- Gate stdout: `…/logs/gpu31-int45-gates.log` (1003 lines, 29 PASS
  banners, 0 `GATE FAIL`).
- Evidence: `…/logs/gpu31-int45-evidence.log` (1046 lines).
- Host tests: `…/logs/gpu31-int45-hosttest.log`
  (`submit_fix_test: all checks passed` — T1–T11 —,
  `chainsplit_test: all checks passed`),
  `…/logs/gpu31-int45-schedhost.log` (2× `409 checks, 0 failures`,
  `gates: all PASS`, kcheck PASS),
  `…/logs/gpu31-int45-jobtest.log` (`job_test: all checks passed`).
- Pack: stock `dt.img` + reference `ramdisk-new.gz` (never rebuilt) +
  the built Image → 27348992 B = kernel 18209976 + ramdisk 8822392 +
  dt 313344 (+3280 header).

Source provenance (evidence:430): `repo HEAD=078daa3 driver-0001
sha256=20cd0884…`. The report commit does not touch driver 0001; any
later edit to it invalidates §4.

Freshness: the pristine tree has no `drivers/gpu/drm/drm_sched.c` and no
`drivers/gpu/drm/panfrost/` directory. `Image-int45` has the same
18209976-byte size as int44; 7,200,215 bytes differ in the common
prefix.

## 2. What changed in the image

The int42/43/44 work fixed *when* a fence is published and *what* a poll
is told; int45 fixes why the phone ran out of memory while those were
being accepted. The leak was structural: a `bo_list` entry owned a
reference to its own object, and the only code that unlinked the entry
ran from the object destructor — which could not run while the reference
existed. Every submitted BO therefore stayed alive for the life of the
process (or until the file closed? no: the list is driver-global, so for
the life of the *driver*), including its private shmem inode, pages and
IOVA range.

| Applied site | Change |
|---|---|
| `drv.h:56` | entry object is documented and treated as weak ("valid while linked"). |
| `submit.c:1208` map_bo | links the entry (`e->obj = obj;` :1296, `list_add_tail` :1299) with no `drm_gem_object_reference`; the only failure paths (page-phys, mmu_map, entry alloc) jump to `rollback`. |
| `submit.c:1313` rollback | unmaps leaves 0..i-1, publishes the clear with an invalidate, returns the error — restores "leaves programmed ⇔ entry linked". |
| `submit.c:885` gpu_unmap | unlinks the entry (:918) and frees it (:919) with no put; the PTEs are cleared and the TLB published while the IOVA is still linked (order unchanged). |
| `main.c:719–721` | remove walker unmaps, unlinks, frees; no unref anywhere in main.c. |
| `submit.c:170` try_get / :469 / :1387 | `kref_get_unless_zero` helper; PTEWALK live pin and `gpu_span` take references through it under `bo_lock` (a dying object is skipped, not resurrected). |
| `submit.c:1489 / :1646` | the only two `drm_gem_object_reference` sites: shadow per-node refs and `job->bos`. |

No fence, scheduling, MMU mapping or power behaviour changed.

## 3. Gate evidence (computed inside the build)

### 3.1 Prior gates: 28/28 PASS, two retargeted

Carried with 6k/6m retargeted, everything else unchanged: 6j/6z/6ad
(single serial BO publish), 6w (3 `p7580_drv_gpu_read` sites), 6ab
(`off=%03x` PAGE, `job->fbd`), 6aa (always power-on recovery), 6ac
(750 ms budget + hang_inject), 6ae (WAIT_BO `0 → -ETIMEDOUT`), plus the
older layout/ABI/IRQ/fence/MMU/cachedir gates. Unchanged constants:
ops=24/slot=232/ran=236, cfg `0x83700`, 56/192/192/64/32,
`P7580_BO_AGE_SLOTS 64`, `P7580_JOB_TIMEOUT_MS 750u`.

### 3.2 New 6af: weak entry, try_get, rollback (PASS)

Applied-tree anchors:

```
drv.h:56     struct drm_gem_object *obj;	/* weak: valid while linked */
submit.c:170 static int p7580_drv_gem_try_get(struct drm_gem_object *obj)
submit.c:174     return kref_get_unless_zero(&obj->refcount);
submit.c:469     if (!p7580_drv_gem_try_get(le->obj))
submit.c:1208 static int p7580_drv_map_bo(struct p7580_drv *drv, struct drm_gem_object *obj)
submit.c:1213    int ret = 0;
submit.c:1258 p7580_mmu_invalidate(drv->regs, …);   /* purge before */
submit.c:1266     goto rollback;                    /* page-phys -EFAULT */
submit.c:1273     goto rollback;                    /* mmu_map error */
submit.c:1283     goto rollback;                    /* entry kzalloc -ENOMEM */
submit.c:1313 rollback:
submit.c:1325     while (k--)
submit.c:1326         p7580_mmu_unmap(&drv->mmu, bo->iova + …
submit.c:1329 p7580_mmu_invalidate(drv->regs, …);   /* publish cleared */
submit.c:1331     return ret;
submit.c:885  void p7580_gem_gpu_unmap(struct drm_gem_object *obj)
submit.c:918      list_del(&e->node);
submit.c:1387     if (!p7580_drv_gem_try_get(e->obj))
submit.c:1489     drm_gem_object_reference(obj);          /* shadow */
submit.c:1646     drm_gem_object_reference(s->objs[i]);   /* job->bos */
main.c:719    p7580_drv_unmap_bo(drv, e->obj, pm_held);
main.c:720    list_del(&e->node);
main.c:721    kfree(e);
```

6af refuses: a reference call inside `map_bo`; any
`drm_gem_object_unreference` in `gpu_unmap` or main.c; a missing
unlink/free in either unlink path; a missing `while (k--)`/unmap/publish
in the rollback block; a rollback that does not end in `return ret;`;
more or fewer than three `goto rollback;`; a `return -EFAULT`/
`-ENOMEM` that leaves leaves programmed; a try_get outside `bo_lock`; a
second `kref_get_unless_zero(&obj->refcount)` site; and any
`drm_gem_object_reference` site beyond the two owners. The 6k source
count is 3 invalidates in `map_bo`; the compiled gate asserts `>= 3`
invalidate relocs plus exactly one `p7580_mmu_unmap` in the function and
the invalidate after it.

Compiled evidence (`submit.o`):

```
t p7580_drv_gem_try_get            (outlined, offset 0x0)
p7580_drv_map_bo:
    1fc: bl <p7580_mmu_invalidate>    purge
    29c: bl <p7580_mmu_unmap>         rollback (only unmap site)
    2c0: bl <p7580_mmu_invalidate>    rollback publish
    2fc: bl <p7580_mmu_invalidate>    publish (success)
    388: bl <p7580_mmu_invalidate>    publish (duplicated block)
p7580_gem_gpu_unmap.part.0:
    1 bl <p7580_mmu_invalidate>
```

The four invalidates are three source sites plus one compiler-duplicated
publish path; the decisive property is the single `mmu_unmap` (rollback)
followed by an invalidate, which the gate asserts by line order.

### 3.3 Config closure (d0+d4)

7/7 fragment lines survive `olddefconfig` verbatim
(`CONFIG_MODULES`, `DRM`, `DRM_KMS_HELPER`, `DRM_EXYNOS`,
`DRM_EXYNOS_DMABUF`, `DRM_EXYNOS_G2D`, `CONFIG_DRM_PANFROST7580=y`).

## 4. Outputs (sha256)

- `Image-int45` (18209976 B):
  `1fc118afdb608f23cc1d029338624b047f295bae4e9c3515240b418d5bfaac68`
- `kernel-boot-int45.img` (27348992 B):
  `0855ad1259760ca16806c8bc7b7013159d1b81c8cd12e49c44de39af11b83f57`
- build log:
  `886ffec90c00fdaa8302e02027220146e15bcd23c25c301babcd855b3ae2b5c5`
- gate stdout log:
  `2d7f205e6aeb0911406de88c12462a58b36639a09263431502e434925e772af6`
- evidence log:
  `1cfb5160624cbece9f2acb8594447a2d94f97b25bc95667fba0bf059a9582099`
- host-test log:
  `91007da2edb706a24eb9523859d7fea5cffbc64519bd157b15d1f7ce9f1d1567`
- sched-host log:
  `c2bc138926e98044c775aa20b4e711ad54b9d8b3b4260378f001b6c963069fc1`
- jobs-host log:
  `ada85c46c81086468fc7cf33cff9ce53e221b9dbc59754e2433ec639ae10d68b`
- script:
  `6cae3d760117d4cb9dd206a53d1104354a24162e4c692b2e5eb3c0378bc1b5e6`

All paths under
`/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31/`.
No flash per ticket.

## 5. Silicon acceptance (Main/BOLeak, after flashing int45)

1. **Leak is gone**: over 3×300 frames at 256², `Shmem:` in
   `/proc/meminfo` must stay flat (int44 grew ~1.16 MB/frame =
   ~350 MB per run) and `MemAvailable` must not collapse; `dmesg` must
   show no new `lowmemorykiller` lines during the runs. This is the
   acceptance datum that the fix exists for.
2. **Clean runs**: each 300-frame run `ERR=0`, `TIMEOUT=0`, `EVRING=0`,
   wall 1.9–2.1 s; a 512² readback (30 frames) still passes with no
   timeout (373 ms detile frames).
3. **Free path**: after a client exits, the BOs it created must be gone
   from the driver list (the `p7580_iova: unmap obj=` count must match
   the map count over the process lifetime, and `/proc/slabinfo`
   `kmalloc-*`/shmem inode counts must return to baseline).
4. **Failed-bind rollback** is not directly triggerable on silicon;
   the static gate (6af + 6k) is the evidence, and a fault-free run must
   not regress the OVERLAP/`NO-OWNER` PTEWALK lines.

## 6. Script notes

- **First int45 run was discarded, not patched over**: it started on
  8a493b5 (`5cd389e2…`) and gpu31-boleak committed 078daa3
  (`20cd0884…`, map_bo rollback) while it built. The retained tree had
  already applied the *new* patch, so the old gates failed at 6k
  ("map_bo must carry exactly 2 mmu_invalidate calls") — the 6v drift
  check would have rejected it anyway because the recorded sha no longer
  matched. Nothing from that run is quoted here; the full gate suite was
  re-run on the retained tree after the retargets (29/29) and the final
  clean build produced §4.
- **Compiled invalidate count is not 1:1 with source sites**: GCC
  duplicates one publish block, so map_bo shows 4 invalidate relocs for
  3 source calls. The disasm gate therefore asserts `>= 3` invalidates
  plus the rollback's unique `mmu_unmap` and its following publish,
  instead of an exact reloc count.
- Retained: hunk `+count` integrity (submit.c `+1,2258`), RC/mtime
  freshness, defectb loop proof, config closure, driver-0001 drift
  re-check, and every int44 gate (6ae included).
