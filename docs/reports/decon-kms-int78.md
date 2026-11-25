# int78: trace the GEM mmap path and the PRIME/ADDFB2 path

int77 fixed the lock leak: `modetest -c` lists DSI-1 (720x1280 @59.92,
preferred+driver), `modetest -p` lists CRTC 18 ACTIVE, DECON IRQ 234 climbs.
Two user paths remain, both instrumented in this image.

## 1. GEM mmap (`0123`, built-in DRM core)

`modetest -s` fails at "failed to map buffer".  The three sides are now logged:

* `get_gem_mmap_offset()` (dumb-map ioctl):
  `drm_gem_cma: get_gem_mmap_offset obj=%p key=%u offset=%u`;
* `drm_gem_cma_mmap()`:
  `drm_gem_cma_mmap: enter pgoff=%lu size=%lu by <comm>`,
  `drm_gem_cma_mmap: drm_gem_mmap ret=%d private=%p`,
  `drm_gem_cma_mmap: paddr=%pa remap ret=%d`.

A zero/shifted offset, an offset_hash miss (`drm_gem_mmap` falling back to
`drm_mmap`) and a failing `remap_pfn_range` are each directly visible.  The
offset is registered in `drm_gem_cma_create()` via
`drm_gem_create_mmap_offset()`; the driver uses `drm_gem_cma_mmap` +
`drm_gem_cma_dumb_map_offset` (`main.c` fops), and `.gem_vm_ops =
&drm_gem_cma_vm_ops` is set, so the trace will show which side disagrees
rather than guessing.

## 2. PRIME / ADDFB2 (`0124`, module)

kmscube lands on llvmpipe.  The module now logs:

* `exynos7580_gem_prime_import: enter dma_buf=%p size=%zu by <comm>` and
  `… ok obj=%p import_attach=%p sgt=%p`;
* `exynos7580_fb_create: WxH fmt=%08x handles=… pitches=… offsets=…`
  (this 3.10 `drm_mode_fb_cmd2` has no modifier array).

With `modetest -P <crtc>:<conn>@<fmt>` the failing side is named: no import
call at all (kmsro did not pick us), an import error, or an ADDFB2
format/pitch/offset mismatch.  The driver advertises `DRIVER_PRIME`,
`gem_prime_export = drm_gem_prime_export`,
`gem_prime_import = exynos7580_gem_prime_import`; the import builds a CMA
wrapper over the exporter's sg table, and `iovmm_map()` (not
`ion_iovmm_map()`) maps it so panfrost-exported buffers are accepted.

## Artifacts (image rebuild — 0123 is in the built-in DRM core)

| file | size | sha256 |
|---|---|---|
| `Image-int78` | 18075368 | `7daf762fce8a0806753f172121e7bfdef69f82d92d330f83f736691236632112` |
| `kernel-boot-int78.img` | 27213824 | `4f84f2afb232698d9cdd704dd8d05699bcef8842a699396dff93a74af54909f2` |
| `exynos-drm7580-int78.ko` | 1434352 | `aa62291a48a08da17d37bf2f348b4e581726f4a7f3db399805a91386d74d74b6` |
| `drm_kms_helper-int78.ko` | 1443520 | `d775f89dc99c08ca4d74e3c2e36791d2b7e41dd83af84a00afd80549e372eb9f` |

## Gates (PASS)

All int77 gates plus the three built-in mmap trace strings (vmlinux), the
three module PRIME/ADDFB2 trace strings (module), and the lock_all
simplification marker (matched without the int-number prefix).

## What the next round should tell us

```
drm_gem_cma: get_gem_mmap_offset obj=… key=… offset=…
drm_gem_cma_mmap: enter pgoff=… size=… by modetest
drm_gem_cma_mmap: drm_gem_mmap ret=0 private=…
drm_gem_cma_mmap: paddr=… remap ret=0
```
`modetest -s` should then paint; a failure is named by the last of those lines.
For kmsro:
```
exynos7580_gem_prime_import: enter dma_buf=… size=… by kmscube
exynos7580_gem_prime_import: ok obj=… import_attach=… sgt=…
exynos7580_fb_create: 720x1280 fmt=… handles=…
```
No `prime_import` line at all means Mesa never chose our device as kmsro
(renderer=/EGL platform selection), which is then a userspace/device-selection
question, not an import failure.

Log sha256s: build `8682905e4a1d9e16a54c55ffb001ceb033280c15acf823aeeb3df463a29967ce`, gates `3c5d6756992a5071de7a5ee9ae12dde11e19a6d612d20915b2e31eb451540e11`, evidence `94672ce781cfc9076df63adbdccdf66852f7c19ae91558533e0975af3d5ea55b`, config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.
