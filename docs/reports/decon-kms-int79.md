# int79: 64-bit CMA mmap offset — the last user-visible KMS blocker

int78's trace localised the GEM mmap failure:

```
drm_gem_cma: get_gem_mmap_offset obj=0 key=1049476 offset=3686400
drm_gem_cma_mmap: enter pgoff=900 size=3686400 by modetest
drm_gem_cma_mmap: drm_gem_mmap ret=-22 private=0
```

## Cause

The core keeps the fake offset in **pages** (`drm_gem.c:370`,
`list->hash.key = list->file_offset_node->start`) and `drm_gem_mmap()` looks it
up with `vma->vm_pgoff`, also in pages.  `get_gem_mmap_offset()` was

```c
return (unsigned int)obj->map_list.hash.key << PAGE_SHIFT;
```

so with `key = 1049476` the shift overflowed 32 bits
(`1049476 << 12 = 3686400 mod 2^32`).  Userspace mmap()ed byte offset
3686400 → `vm_pgoff = 900`, the offset_hash lookup for 900 missed, the core
fell back to the legacy `drm_mmap()` path and returned `-EINVAL`.

## Fix (`0125`, built-in DRM core)

```c
static unsigned long get_gem_mmap_offset(struct drm_gem_object *obj)
{
	unsigned long key = obj->map_list.hash.key;
	unsigned long offset = key << PAGE_SHIFT;
	...
	return offset;
}
```

The byte offset is now 64-bit, `vm_pgoff` becomes the page key (1049476) and
the hash lookup hits; `drm_gem_cma_mmap()` proceeds to `remap_pfn_range()`.
The trace keeps `%p/%lu/%lu` so the values stay visible.

## Artifacts (image rebuild — 0125 is built-in DRM core)

| file | size | sha256 |
|---|---|---|
| `Image-int79` | 18075368 | `ce23564b9913ca199db964c11c0bf354c2f46a48abbee25fd35de6aeceda70c4` |
| `kernel-boot-int79.img` | 27213824 | `558e42fc8d89fb4bf8002eb8e3bf7d3b3a9f7a07553bcc402f8a3879883cfa24` |
| `exynos-drm7580-int79.ko` | 1434352 | `210b42ef87076cdf6b62c64cc9c3b50fc9b3e3765a0c98e92db74ad2695f98de` |
| `drm_kms_helper-int79.ko` | 1443520 | `ab0f621ea57768b1ffb132f5ae8670f7238791bf2a47ee3758ff81113ad27d78` |

## Gates (PASS)

All int78 gates plus: `get_gem_mmap_offset` returns `unsigned long`, the shift
is stored in an `unsigned long`, no 32-bit key remains, the log uses `%lu`,
and the trace string is in vmlinux.

## Expected next round

```
drm_gem_cma: get_gem_mmap_offset obj=… key=1049476 offset=4298653696
drm_gem_cma_mmap: enter pgoff=1049476 size=3686400 by modetest
drm_gem_cma_mmap: drm_gem_mmap ret=0 private=…
drm_gem_cma_mmap: paddr=… remap ret=0
```

`modetest -s 20@18:720x1280` should paint the pattern with IRQ 234 climbing;
`dd` to `/dev/fb0` already returns (66 MB/s on int78).  For kmscube the
`prime_import`/`fb_create` traces from int78 say whether Mesa uses our import
path at all (`renderer:` line decides llvmpipe vs panfrost).

Log sha256s: build `a3b5ace13139178644285c022c1cd884dd82dadbaa4e1802ff8d70f4e0a98e13`, gates `b79a568b4c98a52e1862ee98a68fa029073898e1cce3c465aaad6b30b8acb99d`, evidence `8d8d40effdb4d41159e7e1449703160ef4692ef3e4dfbfe62853ae184f5a6ed4`, config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.
