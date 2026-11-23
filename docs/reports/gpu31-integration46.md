# gpu31-integration46 — driver-owned serialised fake-mmap-offset manager; 30/30 gates

Date: 2026-11-22. Owner: G31_Integrator46. Phone: on int45 (Main
rebooting); host-side build only. No flash per ticket.

Consumes **04db08d** (gpu31-mmlock, repo HEAD `cb7ec49`): the 3.10 core
fake-mmap-offset manager (`dev->mm_private`: a bare `drm_mm` plus a bare
`drm_ht`, no locking in either) is **replaced**, not wrapped. The driver
now owns the offset space with `p7580_dev.offset_mm` +
`offset_hash` + `mmap_offset_lock`, per-BO `offset_node/offset_hash/
offset_valid`, and three helpers (`p7580_offset_alloc/free/lookup`) that
are the only users of the manager. `.mmap` resolves through the driver
lookup with `kref_get_unless_zero` instead of calling
`drm_gem_mmap()`; `MMAP_BO` reads the offset under the lock. This closes
the crash class int45 exposed (paging fault at `dead000000000130` in
`drm_mm_insert_helper` ← `drm_gem_create_mmap_offset` ← CREATE_BO, with
free running from the sched `free_job` workqueue) and the reader side
(core `drm_gem_mmap()` took an unreserved reference and could resurrect
a refcount-0 object, and its hashtab walk ran under `struct_mutex` only).

Driver 0001 at build time: sha256
`e74746706e2320a7e4dfff8d978473bec5bc3cadbfa7e48068abd586bfc4be48`;
gem.c `+1,1807`, gem.h `+294`, ioctl.c `+401`, submit.c `+2258`
unchanged.

## 0. TL;DR

- **Build green**: fresh scratch tree on the external disk, `-j4`
  Image, RC 0, 0 `error:` lines, 5397-line log ending
  `OBJCOPY arch/arm64/boot/Image`; 499 warnings (same ambient count as
  int41–int45).
- **30/30 gate banners PASS, 0 FAIL**: the 29 int45 gates with **6w**
  retargeted (the per-mmap `pr_info` pair and the core `drm_gem_mmap`
  call are gone) plus new **6ag**.
- **6ag**, in the tree, the mirror, `gem.o` and `ioctl.o`:
  - state/window: `gem.h:142–144` (per-BO `offset_node`, `offset_hash`,
    `offset_valid`), `gem.h:165–167` (`offset_mm`, `offset_hash`,
    `mmap_offset_lock`); `P7580_OFFSET_START/SIZE` (`gem.c:527–528`,
    the same page-index window the core used);
  - lifecycle: `mutex_init` (`:538`), `drm_ht_create(&gd->offset_hash,
    12)` (`:548`), `drm_mm_init(&gd->offset_mm, …)` (`:553`), takedown
    (`:568–569`);
  - `p7580_offset_alloc` (`:635`): `search_free` + `insert_node` +
    `offset_hash.key = offset_node.start` + `ht_insert_item` + `valid=1`
    all inside one critical section — no half-built entry is visible and
    two creators cannot take the same hole;
  - `p7580_offset_free` (`:665`): `ht_remove_item` + `remove_node` +
    `valid=0` under the same lock — what makes a concurrent `.mmap`
    miss rather than resurrect a dying BO;
  - `p7580_offset_lookup` (`:678`): `drm_ht_find_item` +
    `kref_get_unless_zero(&bo->base.refcount)` (`:694`) under the lock;
  - call sites: create/import `alloc` (`:890`, `:1611`), err-undo frees
    (`:902`, `:1641`), `free_object` gated on `offset_valid` (`:1171`,
    `:1177`); no `base.map_list` use anywhere in gem.c;
  - `.mmap`: `obj = p7580_offset_lookup(gd, vma->vm_pgoff)` (`:1364`),
    `return drm_mmap(filp, vma)` only for offsets we never handed out
    (`:1366`), then the VMA shape + `drm_vm_open_locked` (`:1384`);
    no `drm_gem_mmap(filp` call;
  - `MMAP_BO` (`ioctl.c:216`): `-ENODEV` without `gd` (`:227`), lock +
    `offset_valid` + `offset_node.start << PAGE_SHIFT` (`:241–244`),
    `-ENOENT` when invalid (`:246`), `args->offset = fake_offset`
    (`:249`);
  - import fails closed (`:1592–1593`, `-ENODEV` without dev_private).
- **Object proof**: `gem.o` references **no** `drm_gem_mmap`,
  `drm_gem_create_mmap_offset` or `drm_gem_free_mmap_offset`, and does
  reference `drm_mm_search_free_generic`, `drm_mm_insert_node`,
  `drm_mm_remove_node`, `drm_ht_insert_item`, `drm_ht_find_item`,
  `drm_ht_remove_item` plus the legacy `drm_mmap` fallback. `.mmap`'s
  disassembly has `drm_ht_find_item`=1, `drm_mmap`=1,
  `drm_gem_mmap`=0, `remap_pfn_range`=1.
- **Config**: 7/7 d0+d4 fragment symbols survive `olddefconfig`
  verbatim.
- Outputs: `Image-int46` `77da2ed1…`, `kernel-boot-int46.img`
  `2a554a9a…` (§4). This supersedes G31_MMLock's own validation build
  (`Image-int46fix` `a720b7cd…` / boot `258f3004…`).

## 1. Build

Script `code/gpu31/series/build-int46.sh` (3634 lines; sha256
`4305280caf69d370ff547adba08ec410667248bd5f5ec9cae90dbcf3c01cf84e`),
a copy of int45 with int46 names, step **6ag**, the **6w** retargets and
a rewritten header.

Fresh `cp -a` of the pristine tree →
`…/j7k/gpu31/scratch-int46` (birth 19:06:51, tree mtime 19:13:21);
scratch and stage deleted after the pack. `TMPDIR` and every output are
on the external disk; nothing was written to `/tmp` or `/`.

Patches, each `git apply --check` first: backport shims 0001–0005,
Mode-B render node 0001, dmabuf CMA writecombine 0001, driver 0001+0002.
Driver 0001 (`e7474670…`) is hashed before apply and re-checked at 6v;
the new-file hunk `+count` integrity check ran first and passed.

Config: pmaports `config-samsung-j7elte.aarch64` + `d0-m1-minimal` +
`d4-panfrost7580`, then `yes "" | make ARCH=arm64 olddefconfig`
(7/7 closure).

Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
KCFLAGS="-Wno-error" -j4 Image`.

- Build log: `…/logs/gpu31-int46-build.log` (5397 lines, 0 `error:`,
  499 `warning:`; tail `SORTEX vmlinux`/`SYSMAP System.map`/
  `OBJCOPY arch/arm64/boot/Image`). Make RC 0 and Image mtime
  (19:13:21, gated) ≥ build start are enforced.
- Gate stdout: `…/logs/gpu31-int46-gates.log` (1123 lines, 30 PASS
  banners, 0 `GATE FAIL`).
- Evidence: `…/logs/gpu31-int46-evidence.log` (1166 lines).
- Host tests: `…/logs/gpu31-int46-hosttest.log`
  (`submit_fix_test` T1–T11, `chainsplit_test`),
  `…/logs/gpu31-int46-schedhost.log` (2× `409 checks, 0 failures`,
  `gates: all PASS`, kcheck PASS),
  `…/logs/gpu31-int46-jobtest.log` (`job_test`).
- Pack: stock `dt.img` + reference `ramdisk-new.gz` (never rebuilt) +
  the built Image → 27348992 B = kernel 18209976 + ramdisk 8822392 +
  dt 313344 (+3280 header).

Source provenance (evidence:430): `repo HEAD=cb7ec49 driver-0001
sha256=e7474670…`.

Freshness: the pristine tree has no `drivers/gpu/drm/drm_sched.c` and no
`drivers/gpu/drm/panfrost/` directory. `Image-int46` has the same
18209976-byte size as int45; 8,772,845 bytes differ in the common
prefix.

## 2. What changed in the image

int45 made BO frees real, which exposed two independent defects in the
3.10 offset manager: writers (create/import from ioctl, free from the
sched workqueue) corrupted `drm_mm`/`drm_ht`, and the core `.mmap` reader
took an unreserved reference under a lock the writers never held. My
first pass (committed as `223b4c8`) only serialised the writers, which
was insufficient — `dead000000000130` in `drm_mm_insert_helper` is a
write-write crash, but the reader window and the resurrect-a-dying-BO
hazard remained, and the import path could dereference a NULL
`dev_private`. `04db08d` replaces the manager instead.

| Applied site | Change |
|---|---|
| `gem.c:527–528` | `P7580_OFFSET_START/SIZE`: page-index window identical to the core's `DRM_FILE_PAGE_OFFSET_*`, so userspace offsets keep the same encoding. |
| `gem.c:548–556` | `dev_init`: create `offset_hash` (drm_ht, 12), init `offset_mm`, unwinding `iova_mm` on failure. |
| `gem.c:568–569` | `dev_fini`: `drm_mm_takedown(&gd->offset_mm)` + `drm_ht_remove(&gd->offset_hash)`. |
| `gem.c:635–662` `p7580_offset_alloc` | search → memset node → insert → key → ht insert → `valid=1`, one lock; `-ENOSPC` / `-ENOMEM` unwind the node. |
| `gem.c:665–675` `p7580_offset_free` | `ht_remove_item` + `remove_node` + `valid=0`, one lock; tolerant of a never-allocated BO. |
| `gem.c:678–699` `p7580_offset_lookup` | `drm_ht_find_item` + `drm_hash_entry` + `kref_get_unless_zero` under the lock; returns a referenced object or NULL. |
| `gem.c:890/902, 1611/1641, 1171/1177` | create/import use alloc (+ err free), `free_object` frees when `offset_valid`. |
| `gem.c:1341–1455` `p7580_gem_mmap` | driver lookup + legacy `drm_mmap()` fallback; VMA flags/ops/prot, `drm_vm_open_locked` under struct_mutex; CMA `dma_mmap_attrs`, imported `remap_pfn_range`, SHMEM fault-driven `VM_MIXEDMAP` arms unchanged. |
| `gem.c:1592–1593` | import fails closed (`-ENODEV`) when `dev->dev_private` is NULL. |
| `ioctl.c:216–249` `panfrost_ioctl_mmap_bo` | lock, `offset_valid`, `offset_node.start << PAGE_SHIFT`, unlock, `-ENOENT` if zero. |

No fence, BO lifetime, scheduling, MMU or power semantics changed; the
int45 weak-entry/try_get/rollback work is untouched, and submit.c is
byte-identical to int45.

## 3. Gate evidence (computed inside the build)

### 3.1 Prior gates: 29/29, one retarget (6w)

6aa (always power-on recovery), 6ab (`off=%03x` PAGE, `job->fbd`),
6ac (750 ms budget + hang_inject), 6ad (single serial BO publish +
fbd/jc_age), 6ae (WAIT_BO `0 → -ETIMEDOUT`), 6af (weak bo_list entry,
try_get, rollback), 6j/6z (one add_excl at SUBMIT), 6k (3 map_bo
invalidates, rollback unmap), 6m (ptewalk try_get), and the older
layout/ABI/IRQ/fence/MMU/cachedir gates.

6w retargets, all caused by removing the core `.mmap` call:
- `p7580_gem_mmap` disassembly: `drm_gem_mmap` 0, `drm_mmap` 1,
  `drm_ht_find_item` 1 (was `drm_gem_mmap` 1);
- the `p7580_gem_mmap: ok mixedmap` string gates (object and Image) are
  removed with the `pr_info` pair; the mixedmap shape itself is still
  gated by the source order `clear PFNMAP → set MIXEDMAP|DONTCOPY →
  struct_mutex → p7580_gem_vm_get`, and the ledger
  `p7580_pin: %s obj=%lu iova=0x%llx` string is still required in the
  Image.

### 3.2 New 6ag: driver-owned manager (PASS)

Applied anchors (see §0). The gate is deliberately structural:

- state and window macros present; `dev_init`/`dev_fini` symmetric;
- each helper defined once, takes and drops `mmap_offset_lock` exactly
  once, with lock first and unlock last;
- alloc contains all four manager operations; free contains both
  removes + `valid=0`; lookup contains `drm_ht_find_item`,
  `drm_hash_entry` and `kref_get_unless_zero`;
- exactly 2 alloc sites, 3 free sites, `free_object` gated on
  `offset_valid`, and **no** `map_list` in gem.c;
- `.mmap` uses the lookup, keeps the `drm_mmap` fallback, never calls
  `drm_gem_mmap(filp`, takes no reference itself, and resolves before
  the `struct_mutex` VMA registration;
- `MMAP_BO` reads the offset strictly inside the lock and returns
  `-ENOENT` for an invalid one; import fails closed;
- comment-stripped scan of the applied gem.c: no statement calls the
  core create/free functions or `drm_gem_mmap(filp`;
- mirror carries every new line;
- **compiled proof** (decisive): `gem.o` has no `U` reference to
  `drm_gem_mmap`, `drm_gem_create_mmap_offset` or
  `drm_gem_free_mmap_offset`, and has `U` references to
  `drm_mm_search_free_generic`, `drm_mm_insert_node`,
  `drm_mm_remove_node`, `drm_ht_insert_item`, `drm_ht_find_item`,
  `drm_ht_remove_item` and `drm_mmap`. `p7580_offset_alloc` is
  outlined (`t`); free/lookup are inlined into their callers, which is
  why the U-symbol set (not helper symbols) is the proof.

```
nm gem.o: U drm_ht_create / drm_ht_find_item / drm_ht_insert_item /
          drm_ht_remove / drm_ht_remove_item / drm_mmap /
          drm_mm_init / drm_mm_insert_node / drm_mm_remove_node /
          drm_mm_search_free_generic / drm_mm_takedown
          t p7580_offset_alloc
p7580_gem_mmap disasm: drm_ht_find_item=1 drm_mmap=1 remap_pfn_range=1
          drm_gem_mmap=0  drm_vm_open_locked=1  p7580_gem_map_pin (local bl)
```

### 3.3 Config closure (d0+d4)

7/7 fragment lines survive `olddefconfig` verbatim.

## 4. Outputs (sha256)

- `Image-int46` (18209976 B):
  `77da2ed1e3643b421dbbc4a2bab209ed689bde8e01cf41d372387b48090cc63a`
- `kernel-boot-int46.img` (27348992 B):
  `2a554a9ac424e54faacdb53baca977955157e7e0bb0b17b882634b5e612b44c9`
- build log:
  `54a0199b1f0ada623f0941242420ca98581529fbe19b31d04a8569ba260038d1`
- gate stdout log:
  `a6655115e22c2abac530591c5030845a26e510e1fc0dad4c12906b5cb59faa79`
- evidence log:
  `312f80f50fb4e4203796f4c6b64ccf2950e9b4420788aff34c8185551abb5498`
- host-test log:
  `91007da2edb706a24eb9523859d7fea5cffbc64519bd157b15d1f7ce9f1d1567`
- sched-host log:
  `ac12637e0d9a4a81367cc9d356354415ee156b0bd28152515ff3357be776d16b`
- jobs-host log:
  `ada85c46c81086468fc7cf33cff9ce53e221b9dbc59754e2433ec639ae10d68b`
- script:
  `4305280caf69d370ff547adba08ec410667248bd5f5ec9cae90dbcf3c01cf84e`

All paths under
`/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31/`.
No flash per ticket.

## 5. Silicon acceptance (Main/G31_MMLock, after flashing int46)

**Flash path**: this userland exposes the boot partition as
`/dev/mmcblk0p10` (259:2, 32 MiB). `dd` to `/dev/block/mmcblk0p10` writes
into the 10 MiB devtmpfs (creates a file) and does **not** touch the
partition — use `/dev/mmcblk0p10`.

1. **Race class**: repeat block — 3×300 frames at 256² followed by a
   second 3×300 (or 10×30 back-to-back), under `flock`, plain. Pass =
   every run `ERR=0`, `TIMEOUT=0`, `EVRING=0`, no `drm_mm`/poison
   signature in dmesg, and no wedge/ENOSPC in CREATE_BO. The oops this
   fixes needed repeated runs after a clean acceptance, hence the repeat.
2. **Churn stress**: the kernel signature is gone, but the churn client
   still SIGSEGVs in userspace (G31_MMLock's report lists the crash-VMA
   vs `/proc/pid/maps` discriminator). **Do not call the stress clean**
   until that is settled; int46 only claims the kernel-side race is
   closed.
3. **mmap sanity**: 30-frame 256² readback and 10-frame 512² read must
   pass; MMAP_BO of a valid BO must return a non-zero offset whose
   `mmap()` resolves (offset_valid path), and a bogus offset must fall
   through to `drm_mmap()` and fail with the core error, not a crash.
4. **Open item (not ours here)**: 300-frame fps on int45 is 101.5 vs
   int41's 154-160. G31_MMLock measured per-frame teardown at
   0.57 ms/frame against a 3.5 ms gap and ~5.2 ms/frame client sys CPU,
   so teardown is not the cause; the item stays open as its own ticket.

## 6. Script notes

- Two false starts are recorded so the artifacts are not confused:
  1. my writer-side mutex (commit `223b4c8`) built successfully but was
     explicitly superseded by G31_MMLock's `04db08d`; the in-flight int46
     build from `223b4c8` was killed before packing and its scratch
     removed — nothing from it is quoted here;
  2. G31_MMLock's own `Image-int46fix` (`a720b7cd…`, boot `258f3004…`)
     was a validation build; §4 supersedes it.
- Gate retargets needed by the new `.mmap` (all verified against a
  retained built tree — 30/30 — before the final clean build):
  - `drm_gem_mmap` reloc 1 → 0, plus `drm_mmap` =1 and
    `drm_ht_find_item` =1 in `p7580_gem_mmap`;
  - the `p7580_gem_mmap: ok mixedmap` object and Image string gates
    removed with the `pr_info` pair (the shape stays gated in source);
  - 6ag replaces the earlier writer-side block entirely.
- One codegen surprise worth keeping: `drm_mm_search_free` inlines to
  `drm_mm_search_free_generic`, and `p7580_offset_free/lookup` are
  inlined, so 6ag's object proof is the U-symbol set rather than helper
  symbols.
- Retained: hunk `+count` integrity, RC/mtime freshness, defectb loop
  proof, config closure, driver-0001 drift re-check, and every int45
  gate (6af included).
