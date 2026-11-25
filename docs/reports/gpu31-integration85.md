# integration85 — panfrost render-node mmap instrumentation (int84 + 0137)

int84 pairs card0 with renderD128 (`PRIME=0x3`, MODE_ID on CRTC 18) and
Mesa20-kmsro reports **Mali T720 (Panfrost)**, but the first
`panfrost_bo_mmap()` fails: `DRM_IOCTL_PANFROST_MMAP_BO` succeeds and then
`mmap(2)` returns `MAP_FAILED` with `ENOMEM`
(`../mesa20/src/panfrost/lib/pan_bo.c:417`).

No return path in `p7580_gem_mmap()` produces `-ENOMEM` (it returns
-ENODEV/-EINVAL, or the rc of `drm_mmap()`, `dma_mmap_attrs()`,
`remap_pfn_range()` or `-ENODEV` for a page-less BO), so the first step is
evidence: int85 is int84 plus `code/gpu31/driver/0137-panfrost7580-mmap-instrument.patch`,
which logs the mmap path, the fake-offset ioctls, the offset allocator and
the fault handler.  No behavioural change.

## Series

int84 series unchanged (int83 KMS 0104..0130/0133/0134 + 0135 blob/MODE_ID +
drm-backport 0001-0004 + mode-b + panfrost driver 0001/0002 + 0136 PRIME
hooks), with 0137 applied after driver 0002.

0137 logs:

- `p7580_gem_mmap`: `->mmap` entry — `f_op`, `f_op->mmap`,
  `f_op->unlocked_ioctl`, minor type/index, driver name, `pgoff`, vm size,
  vm_flags, `gem_vm_ops`; offset-hash hit/miss and the `drm_mmap()` fallback
  rc; on a hit: obj size, `offset_valid`, `offset_node.start`, `backing`,
  `sgt`, `pages`, `nr_pages`, `cache_attr`; then the rc of the CMA arm
  (`dma_mmap_attrs`), each sgt chunk (`remap_pfn_range`) or the SHMEM
  `VM_MIXEDMAP` arm, and the final `FAIL branch rc`;
- `panfrost_ioctl_mmap_bo`: handle, `offset_valid`, `offset_node.start`,
  fake_offset, iova, lookup/no-offset failures;
- `panfrost_ioctl_create_bo`: size, flags, handle, iova, `offset_valid`,
  `offset_node.start`;
- `p7580_offset_alloc`: npages plus the rc of the hole search, the drm_mm
  insert and the hash insert, and the assigned start;
- `p7580_gem_fault` (ratelimited): page index, vaddr, vm_flags, backing,
  pages/sgt, rc.

## Artifacts

| file | size | sha256 |
|---|---|---|
| `Image-int85` | 18191112 | `b0eea052ff46094f201f3920ee02795d067ddfd8dc52ab709b82c8d3f9304a2c` |
| `kernel-boot-int85.img` | 27330560 | `b0d825e3a090f4252dd85a21f1b13462264567e8a29487bdbef5fc9c32691be1` |
| `exynos-drm7580-int85.ko` | — | `4bcf7ae88eef3a135f4d4c3e33d8fcc5dfc7b361e27e9a0aa8b479c5f2fdbb45` |
| `drm_kms_helper-int85.ko` | — | `ea4615ccae848f1e16c4f88a6661c1374d30fa4b592b816ba62962d1c3c650ba` |

Build log `int85-kms-build.log`: 0 errors.  Gates PASS: every int84 gate plus
the new `int85 gate: panfrost mmap instrumentation` (each log string present
in `panfrost7580_gem.c` / `panfrost7580_ioctl.c` and the two vmlinux strings
`p7580_gem_mmap: enter`, `p7580_mmap_bo: handle=`), two .ko, imports
resolved, stock-dt boot, pstore/lockup cmdline.

Log sha256s: build `56835354df6ae84e3865585543d821e5a3b2a3e3335d8f990e374ffe0ad79ff9`, gates `d2a5d7535d436ac27995203443086a1a813451f8fdfd94a4498bf1f648bb3b9c`, evidence `f0b463dc4de2e89207899321d1455b6a8014be362ef9b1295ad26ae9187b65db`, config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.

## Expected log lines (dmesg, `dmesg -w | grep p7580_`)

Normal BO create + CPU map:

```
p7580_create_bo: size=0x1000 flags=0x0 handle=1 iova=0x2000000 offset_valid=1 node_start=0x10000 rc=0
p7580_offset_alloc: npages=1 start=0x10000 rc=0
p7580_mmap_bo: handle=1 offset_valid=1 node_start=0x10000 fake_offset=0x10000000 iova=0x2000000
p7580_gem_mmap: enter fop=p7580_drv_fops fop_mmap=p7580_gem_mmap fop_ioctl=panfrost_syncobj_ioctl pgoff=0x10000 vm=0x1000 vflags=0x... minor_type=2 minor_idx=128 drv=panfrost gem_vm_ops=...
p7580_gem_mmap: hit obj_size=0x1000 offset_valid=1 node_start=0x10000 backing=1 sgt=0 pages=... nr_pages=1 cache_attr=0x...
p7580_gem_mmap: mixedmap vflags=0x... vma_refs(before)=0 npages=1 rc=0
p7580_gem_fault: page=0x0 vaddr=0x... vflags=0x... backing=1 pages=... sgt=0 rc=0
```

Discriminators for the ENOMEM:

- **no `enter` line for the failing map** → the syscall never reaches this
  driver's `->mmap` (wrong fd/fops; `fop=` in the adjacent successes shows
  what the node uses);
- **`offset_lookup MISS` + `drm_mmap fallback rc=`** → the fake offset the
  ioctl returned is not the key the hash holds (or the BO is already
  dying); the fallback rc will be -EACCES on an unauthenticated node,
  -EINVAL otherwise — not -ENOMEM, so this would also mean the ioctl and
  mmap disagree on encoding;
- **`hit` then `bad request ... rc=-EINVAL`** → `mmap` length exceeds the
  GEM object size;
- **`hit` + `mixedmap ... rc=0` and still ENOMEM in userspace** → the
  failure is after `->mmap` returns (mmap_region VM machinery/accounting),
  not in this driver;
- **`FAIL branch rc=-ENODEV` for a page-less BO** → wrong backing for a
  mapped create.

Note on UAPI shape: `CREATE_BO` returns the GPU iova (0x2000000, Mesa stores
it as `bo->ptr.gpu`), while `MMAP_BO` returns the separate drm_mm fake offset
(0x10000000); Mesa20 maps with the `MMAP_BO` value, so both logs must be
read together.  Upstream panfrost instead hands out one offset (the GPU VA)
for both, which is the likely fix direction if the logs show a mismatch.

## Handoff

Flash `kernel-boot-int85.img` (`b0d825e3…32691be1`) with the two int85 .ko,
run the same Mesa20-kmsro probe/kmscube and capture
`dmesg | grep -E 'p7580_(gem_mmap|mmap_bo|create_bo|offset_alloc|gem_fault)'`.
Next kernel change follows from the discriminator above.
