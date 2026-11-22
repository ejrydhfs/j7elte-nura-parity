# gpu31-mmap2 — CPU-map ENXIO after dispatch: `vma->vm_pgoff` reset + errno evidence

Date: 2026-11-20. Owner: G31_MmapFix2. Ticket: `code/gpu31/client` cpu-map
step, `code/gpu31/gem/panfrost7580_gem.c` + driver 0001 patch mirror.

Running kernel: int5 + 5d97c65 (boot `5deb4c42`, `#37`), IOVA already
correct (`gpu_offset 0x2000000`). This ticket fixes the *remaining* failure.

## 1. Errno evidence (silicon, current kernel, instrumented client)

Rebuilt client prints the exact request now (`pf7580_cpu_map_errno/_off/_len`):

```
-- pf7580 device path on /dev/dri/renderD128 --
GPU_PROD_ID 0x720 (T720)
BO handle=1 gpu_offset=0x2000000
FAIL cpu map: mmap(fd=3, off=0x100000000, len=16384) errno=6 (No such device or address)
DEVICE PATH: FAILED (1)
```

Static probes (fresh, same kernel, CREATE 4K + 16K BOs):

- `mmap(fd, fake_off=0x100000000)` → `ENXIO(6)` — handler entered, mapping refused
- `mmap(fd, fake_off>>12)` → `EINVAL(22)` — GEM key lookup miss (`drm_mmap` fallback)
- `mmap(fd, 0)` → `EINVAL(22)` — same lookup miss

So 5d97c65 fixed dispatch (`gem_vm_ops` now published, handler runs); the
refusal is *after* dispatch, inside `dma_mmap_attrs`.

## 2. Root cause (3.10 contract, pristine-tree quotes)

- `drm_gem_mmap()` (3.10 `drivers/gpu/drm/drm_gem.c:658`) looks up
  `vma->vm_pgoff` as the fake key and leaves it installed on the vma.
- `dma_mmap_attrs()` → `arm_dma_mmap()` (`arch/arm/mm/dma-mapping.c:857`)
  treats `vma->vm_pgoff` as the **page index into the DMA buffer**:
  `off < count ... else return -ENXIO`.
- Our `MMAP_BO` returns `fake = hash.key << PAGE_SHIFT` (`0x100000000`,
  `vm_pgoff 0x100000`); the BO is 1–4 pages, so `off (0x100000) >= count`
  always → `-ENXIO`.
- `drm_gem_cma_mmap` never hits this (uses `remap_pfn_range`, ignores
  pgoff); exynos `mmap_buffer` uses a fresh vma at pgoff 0. Our immediate
  path kept the fake key across, hitting the bounds check.

No client offset-convention change: userspace correctly passes the byte
fake offset; only the kernel vma needs normalising after lookup.

## 3. Fix

`code/gpu31/gem/panfrost7580_gem.c` `p7580_gem_mmap()`, mirrored into
`code/gpu31/driver/0001-panfrost7580-driver-sources.patch` (regenerated
gem.c hunk; `git apply --check` clean on empty tree):

- after `drm_gem_mmap()` succeeds: `vma->vm_pgoff = 0` (fake key served its
  BO-lookup purpose; mapping starts at BO byte 0).
- temporary `pr_info` entry/exit (`p7580_gem_mmap: enter pgoff=…`,
  `drm_gem_mmap failed rc=…`, `dma_mmap_attrs rc=… size=… paddr=…`,
  `ok`/`fail`) for silicon dmesg proof. Keep or drop after PASS — one line
  removal each.
- Client: `pf7580_cpu_map()` records `errno/off/len` in
  `pf7580_cpu_map_errno/_off/_len` (declared `extern` in `pf7580.h`);
  selftest `run_device` prints
  `FAIL cpu map: mmap(fd, off, len) errno=N (strerror)`. Mock backend sets
  the same globals (`EINVAL` on rejection, `0` on success).

Host checks: `make kcheck` 3 files zero warnings, `make kobj` 3 objects,
`gem_validate` PASS, client `make check` MOCK SUITE PASS (host +
`qemu-aarch64` static binary PASS).

Rebuilt int5 series (`build-int5.sh`, `REPO=/tmp/j7repo`,
scratch-mmap2): RC=0, `panfrost7580_{main,gem}.o` + `panfrost7580.o` link,
`Image` 18144376 B
`68c76a6ed2bc8ad317f11ec6dd4a03f0c66e81370e1f96a8528e7e3f2dcd1bca`,
boot `/tmp/kernel-boot-int5.img` 27283456 B
`e6c0bf1954b736077c30e1f732a0da66350ac2c9bb13504014ae267ad1a75788`
(log `gpu31/logs/gpu31-mmap2-build.log`; panfrost CC lines present, no
panfrost errors).

## 4. Fresh device output (fix built, NOT yet flashed)

Phone still runs `5deb4c42` (this ticket never flashes — Main owns flash).
Instrumented client output above is the pre-flash pin.

## 5. Handoff

- Flash request (Main): `/tmp/kernel-boot-int5.img`
  (`e6c0bf19…7ad1a75788`), fallback `/tmp/bindboot.img`.
- Post-flash assert: instrumented client must print `CPU map R/W ok`,
  kernel dmesg must show
  `p7580_gem_mmap: enter pgoff=0x100000 … dma_mmap_attrs rc=0 …`,
  then `wait_bo(0) ok` → `DEVICE PATH: PASS`. Next step is
  `wait_bo`/submit (`t_wait`/`t_submit`).

## 6. Follow-up status (gpu31-mmaprefs, 2026-11-21)

The `vma->vm_pgoff = 0` reset and the `drm_gem_mmap()` dispatch this
ticket established are unchanged and still required. What changed later:
the SHMEM path no longer maps immediately with `remap_pfn_range` — it is
fault-driven `VM_MIXEDMAP` + `vm_insert_page` (page references the
mapping owns; see `docs/reports/gpu31-mmaprefs.md`). The `p7580_gem_mmap`
entry/exit `pr_info`s are kept; the success line now reads
`p7580_gem_mmap: ok mixedmap obj_size=… vm=… npages=…` for SHMEM BOs
(CMA/imported keep their `dma_mmap_attrs`/`remap_pfn_range` shapes and
their old line). The post-flash assert in §5 stands for the CMA/scanout
path; for CREATE_BO SHMEM mappings expect the `mixedmap` line and no
`remap_pfn_range`.
