# gpu31-mmap — CPU-map failure: root cause, fix, device evidence

Date: 2026-11-20. Owner: G31_MmapFix. Ticket: GEM `panfrost7580_gem.c`
(+ioctl), driver mmap wiring, client cpu-map step.

Prior state (silicon, kernel `3.10.108 #37`, int5 render-only boot):
`code/gpu31/client` selftest `--device /dev/dri/renderD128` passed
open, GET_PARAM (`GPU_PROD_ID 0x720`), CREATE_BO(4K) (handle 1,
`gpu_offset 0x2000000000`), MMAP_BO (`map_offset 0x100000000`), then
failed at the CPU map (`FAIL cpu map`, exit 1, no kernel trace, no hang).

## 1. Root cause (two driver defects, client is innocent)

**R1 (the EINVAL): `driver->gem_vm_ops == NULL`.** The driver's
`struct drm_driver` set `.gem_vm_ops = NULL`. On 3.10,
`drm_gem_mmap()` (drivers/gpu/drm/drm_gem.c) does the fake-offset
lookup itself and returns `-EINVAL` when `driver->gem_vm_ops` is NULL
— before any driver handler runs. The driver's own `p7580_gem_vm_ops`
table existed but was never published to the core, so every `mmap()`
on the render node failed with `EINVAL` and fault never ran (hence the
silent dmesg). Proved on silicon with static probes: `mmap(fd,
fake_off)` **and** `mmap(fd, fake_off >> 12)` both return
`EINVAL`; `mmap(fd, 0)` also `EINVAL` — i.e. the failure is in the
core lookup/dispatch, not in the offset encoding.

**R2 (latent, same function): post-hoc `VM_MIXEDMAP` + `vm_insert_mixed`
fault path breaks the 3.10 core contract.** `p7580_gem_mmap()` called
`drm_gem_mmap()` (which sets `VM_PFNMAP|VM_DONTEXPAND` and installs
`driver->gem_vm_ops` itself), then cleared `VM_PFNMAP`, set
`VM_MIXEDMAP`, and swapped in a second `vm_ops`. The working shape on
this tree is `exynos_drm_gem_mmap_buffer` / `drm_gem_cma_mmap`: keep
the core-installed vma, select the WC prot (exynos
`update_vm_cache_attr` shape), and map the pages **immediately** —
`dma_mmap_attrs()` for the CMA page, `remap_pfn_range()` per page for
SHMEM/imported — with a size check. The old fault helper
(`p7580_gem_fault`/`p7580_map_page`) is kept for reference only.

> **Correction (gpu31-mmap2 + gpu31-mmaprefs, 2026-11-21).** R2's
> diagnosis was wrong about the flags: clearing `VM_PFNMAP` and setting
> `VM_MIXEDMAP` after `drm_gem_mmap()` is exactly what the in-tree
> `udl_gem_mmap()` does, and the fault path works when the core-installed
> `gem_vm_ops` table is kept (no second table is needed). The actual
> `-EINVAL`/`-ENXIO` came from `dma_mmap_attrs()` reading the fake
> `vma->vm_pgoff` as a buffer page index (`gpu31-mmap2.md` §2, fixed by
> the `vm_pgoff = 0` reset). Because `remap_pfn_range`/`VM_PFNMAP`
> mappings hold no page reference at all, the immediate shape was
> reverted to the fault-driven `VM_MIXEDMAP` + `vm_insert_page` one for
> local SHMEM BOs in `gpu31-mmaprefs.md` (the gpu31-shaderbo page-reuse
> fix); the CMA and PRIME-imported immediate shapes described above
> remain.

**R3 (wrong GPU alias, same symptom family): IOVA `drm_mm` units.**
`p7580_dev_init()` passed raw byte `P7580_IOVA_START/SIZE` to
`drm_mm_init()`, but `drm_mm` counts **pages** (`node.start` is a
PFN). Every BO landed near `0x2000000000` — past the 4 GiB window —
while MMAP_BO (separate GEM offset manager) kept working, so
open/CREATE/GET_BO all passed with only the GPU alias wrong. The MMU
hook programs `bo->iova` into the Mali tables, so this also corrupts
first SUBMIT. Fix: init with `>> PAGE_SHIFT`; `iova =
node.start << PAGE_SHIFT` is then the PFN-to-bytes conversion (as
written). `CREATE_BO` on the fixed kernel must return an IOVA in
`[0x2000000, 0x100000000)`.

Client (`pf7580_cpu_map`: `mmap(fd, map_offset)`) already uses the
correct byte-offset convention — **no client change**.

## 2. Fix

`code/gpu31/gem/panfrost7580_gem.{c,h}`, mirrored into
`code/gpu31/driver/0001-panfrost7580-driver-sources.patch`
(hunk counts recounted; `git apply --check` clean on an empty tree):

- `p7580_gem_mmap`: immediate mapping (WC prot; `dma_mmap_attrs` with
  plain `WRITE_COMBINE` only for CMA — F-GEM3 holds, arm64 honours
  exactly this attr; `remap_pfn_range` per SHMEM/imported page;
  `vm_size > obj->size` rejected; errors unwind via
  `drm_gem_vm_close`).
- `p7580_gem_vm_ops`: non-static, declared `extern` in the header,
  published as `driver->gem_vm_ops` in `main.c` (was NULL).
- `p7580_dev_init`: `drm_mm_init(... START >> PAGE_SHIFT,
  SIZE >> PAGE_SHIFT)`.

Host checks: `make kcheck` (3 files `-fsyntax-only`, zero warnings),
`make kobj` (3 objects), `gem_validate` PASS.

Rebuilt int5 series (`build-int5.sh`, scratch-mmapfix): RC=0,
`panfrost7580_{main,gem}.o` + `panfrost7580.o` link,
`Image` 18144376 B
`0b18039aac181e71877820fd03ff1611d3ec9a78b399ca689e86cdb27eb48b65`,
boot `/tmp/kernel-boot-int5.img` 27283456 B
`5deb4c4284ed94b0d091e30c66a9221446a57dd03ef208552c7fefb70b7ed765`
(log `gpu31/logs/gpu31-mmapfix-build.log`).

## 3. Fresh device output (fix built, NOT yet flashed)

The phone still runs #37 (int5 pre-fix; this ticket never flashes —
Main owns flash). Re-ran the in-tree selftest to pin the step:

```
-- pf7580 device path on /dev/dri/renderD128 --
GPU_PROD_ID 0x720 (T720)
BO handle=1 gpu_offset=0x2000000000
FAIL cpu map
DEVICE PATH: FAILED (1)
```

Plus static probes: `mmap_bo: fake_off=0x100000000`,
`mmap(fd, fake_off)` → `EINVAL`, `mmap(fd, fake_off>>12)` →
`EINVAL`, `mmap(fd, 0)` → `EINVAL`, `GET_BO_OFFSET == CREATE offset`.
Verdict: **CPU map still FAIL on the running kernel (expected)** —
the fix is built and ready; after Main flashes the image above,
`gpu_offset` must fall inside `[0x2000000, 0x100000000)` and the
next step is `CPU map R/W ok` → `wait_bo(0) ok` → `DEVICE PATH: PASS`.

## 4. Handoff

- Flash request (Main): `/tmp/kernel-boot-int5.img`
  (`5deb4c42…b7ed765`), fallback `/tmp/bindboot.img`.
- Post-flash assert: `CREATE_BO` IOVA in-window, then full
  `--device` selftest; `X` stays on fbdev (render node only).
