# DECON KMS dma-buf import, framebuffers and scanout addresses (KmsDmabuf)

Status: **complete** — M1/M2 slice 14 of `decon-kms-plan.md` §6. Source:
`code/gpu31/kms/exynos_drm7580_dmabuf.c` (installed as
`drivers/gpu/drm/exynos7580/exynos_drm7580_dmabuf.c`), core prerequisite:
`patches/drm-kms/0005-core-drm-gem-cma-import-free.patch`. Slice patch:
`patches/drm-kms/0001-kmsdmabuf-import-and-framebuffer.patch`. Host test:
`code/gpu31/kms/tools/dmabuf/`.

Owner: `KmsDmabuf`. Interface: the shared contract header
`code/gpu31/kms/exynos_drm7580.h` (at `f1a770e`/`f1edfaf`) — this slice
implements exactly the three functions it assigns to the file:

```c
struct drm_gem_object *exynos7580_gem_prime_import(struct drm_device *, struct dma_buf *);
struct drm_framebuffer *exynos7580_fb_create(struct drm_device *, struct drm_file *,
					     struct drm_mode_fb_cmd2 *);
int exynos7580_fb_scanout(struct drm_framebuffer *, unsigned int plane, dma_addr_t *);
```

No header change, no vendor-tree change beyond the one core helper patch, no
new cross-slice global.

## 1. Verdict

| Requirement (`decon-kms-req.md`) | This slice |
|---|---|
| 18 — `drmPrimeFDToHandle()` on the KMS fd for a panfrost dma-buf | `exynos7580_gem_prime_import()`, driven by `drm_gem_prime_fd_to_handle()`; exporter-agnostic attach/map, no ION |
| 19 — `drmModeAddFB2()` accepts the imported GEM, XRGB8888/ARGB8888, pitch/offset | `exynos7580_fb_create()` with format/pitch/offset/size validation and a private `drm_framebuffer` |
| 19 — `drmModeAddFB2WithModifiers` only if advertised | 3.10 UAPI has no modifiers at all; all non-zero `mode_cmd->flags` are rejected |
| M2 — DECON scans the buffer | `exynos7580_fb_scanout()` returns a real DECON IOVA from `iovmm_map()`, for imported dma-bufs, CMA dumb buffers **and** the fbdev CMA framebuffer |

The path a KWin/kmscube frame takes:

```
panfrost renders DRM_FORMAT_XRGB8888 GEM -> dma-buf fd
  drmPrimeFDToHandle(card0)  -> exynos7580_gem_prime_import()
                                  dma_buf_attach(decon dev)
                                  dma_buf_map_attachment(DMA_BIDIRECTIONAL)
                                  drm_gem_cma_object wrapper (import_attach,
                                                            sgt in driver_private)
  drmModeAddFB2(handle)      -> exynos7580_fb_create()
                                  exy7580_fb_check(): format/pitch/offset/size
                                  drm_framebuffer_init()
  drmCloseBufferHandle       -> handle ref gone, fb keeps the GEM alive
  atomic commit plane update -> exynos7580_fb_scanout()
                                  iovmm_map(decon dev, sgt, offset, pitch*h,
                                            DMA_TO_DEVICE, 0) -> IOVA
  decon_kms_commit()         -> DECON windows program the IOVA
```

## 2. Object model: CMA, because the driver's GEM backend is CMA

`exynos_drm7580_main.c` (KmsMain) defines the driver as

```c
.gem_free_object   = drm_gem_cma_free_object,
.gem_vm_ops        = &drm_gem_cma_vm_ops,
.dumb_create       = drm_gem_cma_dumb_create,
.gem_prime_import  = exynos7580_gem_prime_import,
```

so **every** object the device can hand to userspace is a
`struct drm_gem_cma_object`, and this slice produces that type:

| Object | Backing | `import_attach` | where the pages are described |
|---|---|---|---|
| imported dma-buf | exporter-owned, shared zero-copy | set | `base.driver_private` = `struct sg_table *` from `dma_buf_map_attachment()` |
| dumb buffer (`CREATE_DUMB`) | `dma_alloc_attrs()` CMA | NULL | `cma_obj->vaddr` / `paddr`, `base.size` |

Two consequences worth recording:

1. **Vendor `struct exynos_drm_gem_obj` is not used.** A first version of this
   slice built the vendor object (as `exynos_dmabuf_prime_import()` does,
   `exynos_drm_dmabuf.c:212-260`) to reuse
   `exynos_drm_gem_free_object()`. That is incompatible with the assembled
   driver: the CMA free hook would `dma_free_attrs()` the wrapper anyway, and
   `to_drm_gem_cma_obj()` (used by main's PRIME export and the plane/fbdev
   paths) would read past the wrong object. The CMA object is what the rest of
   the assembled driver assumes.

2. **`drm_gem_prime_import()` cannot be called as is.** The 3.10 helper
   delegates to `dev->driver->gem_prime_import_sg_table`
   (`drivers/gpu/drm/drm_prime.c:254-307`), and the contract header assigns no
   such callback to this driver (main does not set one). The sequence in
   `exynos7580_gem_prime_import()` is byte-for-byte the generic helper's
   attach/get/map/create path with the sg-table callback replaced by the
   wrapper allocation. If the integration seat later adds
   `.gem_prime_import_sg_table`, this file can be reduced to a one-line wrapper;
   until then the manual path is the only one that works.

### 2.1 Core prerequisite: freeing imported CMA objects

3.10's `drm_gem_cma_free_object()` predates PRIME import: it unconditionally
calls `drm_gem_cma_buf_destroy()` → `dma_free_attrs(dev, size, vaddr, paddr)`
(`drm_gem_cma_helper.c:148-162`). For an imported wrapper `vaddr == NULL` and
`paddr == sg_dma_address()`; on arm64 `dma_free_attrs()` computes
`phys_to_page(dma_to_phys(dev, 0))` and would release physical page 0 /
swiotlb memory, while leaking the attachment, the dma-buf reference and the
mapped sg table. Upstream fixed this when PRIME import arrived (4.x: check
`import_attach`, call `drm_prime_gem_destroy()`).

`0005-core-drm-gem-cma-import-free.patch` does the same here:

```c
	if (gem_obj->import_attach) {
		drm_prime_gem_destroy(gem_obj, gem_obj->driver_private);
		gem_obj->driver_private = NULL;
	} else {
		drm_gem_cma_buf_destroy(gem_obj->dev, cma_obj);
	}
```

`driver_private` is the sg-table carrier because 3.10's
`struct drm_gem_cma_object` has no `sgt` field; the field is unused by the CMA
helper, `drm_prime.c` and `drm_gem.c` (grep-clean). With this patch,
`.gem_free_object = drm_gem_cma_free_object` is correct for both object kinds.

## 3. Lifecycle and reference counts

### 3.1 Import (`DRM_IOCTL_PRIME_FD_TO_HANDLE`)

`drm_gem_prime_fd_to_handle()` (`drm_prime.c:309-352`) calls
`exynos7580_gem_prime_import()` only for a dma-buf not already in the file's
PRIME table.

Success path, references:

| # | Event | dma-buf refs | GEM refs | attachment |
|---|---|---|---|---|
| 1 | `drm_prime_fd_to_handle`: `dma_buf_get(fd)` | 1 (caller) | — | — |
| 2 | import: `dma_buf_attach(drm->dev)` | 1 | — | 1 |
| 3 | import: `get_dma_buf()` | 2 (ours) | — | 1 |
| 4 | import: `dma_buf_map_attachment(DMA_BIDIRECTIONAL)` | 2 | — | 1, sgt mapped |
| 5 | import: `drm_gem_object_init()` | 2 | 1 | 1 |
| 6 | `drm_gem_handle_create()` | 2 | 2 | 1, handle_count 1 |
| 7 | helper drops the import reference | 2 | 1 | 1 |
| 8 | `drm_prime_add_buf_handle()` (file cache) `get_dma_buf()` | 3 | 1 | 1 |

The caller's ref (#1) is dropped when the helper returns; the fd itself keeps
its own reference until userspace closes it. Refs #3 (attachment) and #5 (GEM)
are dropped by the object's death path:

```
drm_gem_handle_delete -> drm_gem_remove_prime_handles (drops #8)
                      -> drm_gem_object_handle_unreference
                      -> GEM ref 0
                      -> drm_gem_cma_free_object()
                           drm_gem_free_mmap_offset (if any)
                           drm_gem_object_release()
                           drm_prime_gem_destroy(obj, driver_private)
                               dma_buf_unmap_attachment
                               dma_buf_detach            (attachment gone)
                               dma_buf_put               (drops #3)
                           kfree(wrapper)
```

KWin calls `drmCloseBufferHandle()` immediately after `ADDFB2`
(`drm_gpu.cpp:975-1035`); if an fb holds the GEM, the object survives the
handle close and dies in `exynos7580_fb_destroy()` instead. If no fb was
created, it dies at handle close. Both paths are the same code above.

Failure paths in the import unwind in order and leak nothing:

| failure | unwind |
|---|---|
| `dma_buf_attach` | — (nothing taken) |
| `dma_buf_map_attachment` | `dma_buf_detach` + `dma_buf_put` (#3) |
| wrapper `kzalloc` / `drm_gem_object_init` | `dma_buf_unmap_attachment` + detach + put |

No self-import shortcut: this driver exports only dumb buffers, and no
supported user feeds one back into `card0` (KWin exports them to the *render*
GPU). A same-driver re-import would wrap the buffer in a second object over the
same pages — correct, merely not deduplicated. Documented as a known
non-shortcut, not a correctness hole.

### 3.2 Framebuffer (`DRM_IOCTL_MODE_ADDFB2`)

`exynos7580_fb_create()` holds one GEM reference (`drm_gem_object_lookup()`)
in the private fb from creation to `exynos7580_fb_destroy()`. The core owns the
framebuffer object-table reference (`drm_framebuffer_init()`:
`kref_init` + `drm_framebuffer_reference`, `drm_framebuffer.c:56-81,148-153`);
`drm_mode_addfb2()` links the fb into `file_priv->fbs` and `drm_mode_rmfb()`
unlinks it and calls `drm_framebuffer_remove()`, which drops the userspace
reference. Plane/commit states take their own framebuffer references
(`drm_atomic_set_fb_for_plane()`), so `.destroy` only runs when no state and no
client references the fb.

No IOVA is created at ADDFB2 time; the first `exynos7580_fb_scanout()` call
maps it, so an fb that is created and never scanned out costs no IOVMM space
(KWin creates one fb per buffer and scans each one, so this is one map per
buffer lifetime, not per frame). `exynos7580_fb_destroy()` unmaps every cached
plane mapping first, then cleans the fb up and drops the GEM reference. Because
the destroy can only run after all states have dropped the fb, and the DECON
commit is synchronous (`decon_kms_commit()` returns after the frame latched),
the last scanout of the buffer precedes the unmap by a completed commit.

Failure path (validation or `drm_framebuffer_init` failure): `kfree` of the
private fb, GEM lookup reference dropped, no mappings exist.

### 3.3 Scanout mapping lifetime

`iovmm_map()` allocates one IOVM region per call; there is no reference count
inside IOVMM. This slice therefore owns exactly one region per mapped
framebuffer plane, caches it in the private fb (`scanout[plane]`), and frees it
with `iovmm_unmap()` in `exynos7580_fb_destroy()`. Repeated commits with the
same fb hit the cache; 300 flips of 300 buffers create and destroy 300
regions, never accumulate.

The fbdev framebuffer is the one object this file did not allocate, so it gets
a bounded cache instead (`exynos7580_foreign_maps[4]`, keyed by
`(fb, plane)`). It pins its fb with `drm_framebuffer_reference()` — the CMA
helper's fb has no destroy hook this file can extend — so the mapping and that
single fb survive until the driver goes away. On this device that is one
770 KiB fb0 for the built-in driver's lifetime; the cache holds four entries so
a retried `drm_fbdev_cma_init()` after a failed probe cannot misinterpret a new
fb as an old one. Documented as a bounded teardown leak, not a per-frame one.

## 4. IOVA vs DMA address (the question the contract entry point raises)

A DECON window register (`VIDW_ADDx`) is programmed with an address in the
DECON device's System-MMU address space, not a physical address:

* the vendor fbdev/ION path maps every buffer with `ion_iovmm_map()` and stores
  that result in `fbi->fix.smem_start` (`decon_core.c:1325,1676`);
* the arm64 IOVMM (`CONFIG_EXYNOS_IOVMM_V6=y`) hands out IOVAs starting at
  `IOVA_START_V6 == 0x10000000` (`exynos-iovmm-v6.c:29`), and the DECON device
  is a client of `iommu-domain_disp` (`exynos7580.dtsi:1535-1558`), so its
  SYSMMU translations come from that domain.

Therefore:

* `sg_dma_address()` on a mapped dma-buf attachment and `drm_gem_cma_object.paddr`
  are **not** window addresses (DMA-direct/physical); programming them faults
  in the SYSMMU. `exynos7580_fb_scanout()` never returns them.
* Every address it returns comes from
  `iovmm_map(dev, sgt->sgl, offset, pitch * height, DMA_TO_DEVICE, id)` on the
  DECON device (`fb->dev->dev`, i.e. the platform device `drm_platform_init()`
  bound). On the arm64 v6 implementation `id` is not interpreted
  (`exynos-iovmm-v6.c:276-320`); 0 is passed. The 32-bit v3 implementation
  would require `0 <= id < MAX_NUM_PLANE` and offsets it for output planes
  (`exynos-iovmm.c:298-331`), which is why callers must not pass a window
  number blindly if this driver is ever built for armv7.
* `DMA_TO_DEVICE` (device reads memory) is the correct direction; v6 ignores
  it, v3 uses it to pick the input-plane IOVM.
* `ion_iovmm_map()` must not be used: it starts with
  `BUG_ON(dmabuf->ops != &dma_buf_ops)` (`ion.c:2643`), so a panfrost dma-buf
  panics the kernel. `iovmm_map()` is the same primitive `ion_iovmm_map()`
  calls internally (`ion.c:2604`).

## 5. ADDFB2 validation

The core (`drm_mode_addfb2()`/`framebuffer_check()`) already rejects bad flags
except `DRM_MODE_FB_INTERLACED`, out-of-range width/height against
`mode_config`, missing handles, `pitch < width*cpp`, and
`height*pitch + offset > UINT_MAX`. Unknown fourcc codes pass the core
(`drm_format_num_planes()` defaults to 1), so the driver must validate:

| Check | Rule | Why |
|---|---|---|
| flags | must be 0 | no modifiers (`DRM_MODE_FB_MODIFIERS` does not exist in this UAPI) and no interlace |
| format | XRGB8888, XBGR8888, RGBX8888, BGRX8888, ARGB8888, ABGR8888, RGBA8888, BGRA8888, RGB565, RGBA5551 | the one-plane RGB set the DECON shim can program (`decon_api.h` `@fourcc`) |
| planes | only `handles/pitches/offsets[0]`; 1..3 must be zero | single-plane formats only, matching the M2 userspace |
| width/height | non-zero, `<= mode_config.max_width/max_height` | core limits repeated with a 4096 fallback when the field is 0 |
| width bytes | `width * bpp/8 >= 128` | DECON rejects windows narrower than 128 bytes (`decon_core.c:1560-1568`) |
| pitch | non-zero, `>= width*cpp`, multiple of `cpp` | whole pixels, no short stride |
| offset | multiple of `cpp`, `< obj->size` | whole pixels, inside the buffer |
| extent | `offset + pitch*(height-1) + width*cpp <= obj->size` | the last row must exist (u64 arithmetic; 0xffffffff height/offset cannot wrap) |
| sg table | imported objects must carry `driver_private` | catches a wrapper not produced by this import |

The size check uses `obj->size`, which is `PAGE_ALIGN(dma_buf->size)` for an
import and the dumb-create size for CMA buffers; the page slack is at most
4 KiB and cannot hide a short scanout row.

Accepted modes are then handed to the plane, which enforces the remaining
window rules (scaling forbidden, x alignment, blending, bandwidth). 720x1280
XRGB8888 (pitch 2880) and ARGB8888 pass; NV12/NV21 are deliberately rejected
until the plane slice gets a two-plane address story — the plane already
tolerates `addr[1] == 0` for the interleaved case
(`0001-plane-primary-overlays-and-formats.patch:279-300`).

The validation itself is a pure function (`exy7580_fb_check()`) between
`BEGIN/END exynos7580_fb_check` markers and is compiled verbatim on the host
(§7), so the test cannot drift from the kernel code.

## 6. Interaction with the other slices

`exynos7580_fb_scanout(fb, plane, &addr)` is the only interface the planes and
fbdev use; they never reach into this file's framebuffer type.

* Our fbs: `plane` must be 0 (`-EINVAL` otherwise).
* fbdev's `drm_fb_cma` fb: handled through `drm_fb_cma_get_gem_obj()`, so
  KmsFbdev does not have to publish `paddr`/`fix.smem_start` as an IOVA.
  A non-CMA, non-our fb returns `-ENODEV`; in this driver there is no third
  producer (user ADDFB2 goes to `exynos7580_fb_create`, fbdev uses
  `drm_fbdev_cma`).
* Return codes: `0`, `-EINVAL` (bad fb/plane/geometry), `-ENODEV` (no DECON
  device/IOVMM domain, missing sg table, `iovmm_map()` failure, foreign cache
  full). DeconPlane treats `-ENODEV` as "cannot scan this buffer" and blanks
  the window; it is not retried per frame.
* Process context: the first call may sleep in `iovmm_map()`; it is called from
  atomic `->atomic_update`, which is process context in this design (the M0
  commit tail runs in the committing task).

## 7. Host-side unit test

`code/gpu31/kms/tools/dmabuf/` extracts the validation block with `awk`
(asserting exactly one BEGIN and one END marker) and runs 30 cases:

```
$ make -C code/gpu31/kms/tools/dmabuf
...
ok   XRGB8888 720x1280                          (0)
ok   RGB565 720x1280                            (0)
ok   nonzero flags (modifier/interlace)         (-22)
ok   NV12 planar                                (-22)
ok   window narrower than 128 bytes             (-22)
...
all format/pitch validation cases passed
```

Coverage: the ten accepted formats, minimum 128-byte width, padded stride,
zero/non-zero flags, planar/unknown/24bpp formats, missing/second handle,
second pitch, zero dimensions, over-max dimensions, unset max, short/narrow/
unaligned pitch, unaligned/out-of-range offset, one-row-short buffer, zero
buffer, 0xffffffff height and offset (u64 overflow), NULL descriptor.

## 8. Build evidence

Tree: a copy of `android_kernel_samsung_universal7580@2a746c1b`
(`…/j7k/kms/KmsDmabuf`), M0 `patches/drm-atomic/0001..0005` applied, then the
sibling sources as currently delivered (`exynos_drm7580_{main,crtc,plane,dsi,
fence,fbdev}.c`, with the uncommitted current `main.c` from the repo), the core
patches (`0001`, `0002`, `0003`, `0005`) and this slice. The DECON shim
(`0100-decon-export-shim.patch`) targets `drivers/video/`, which is outside
`drivers/gpu/drm/`, so its symbols are unresolved in this compile-only target —
as for every other slice that consumes `decon_api.h`.

```
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS=-Wno-error -j8 drivers/gpu/drm/
  CC drivers/gpu/drm/exynos7580/exynos_drm7580_dmabuf.o
  CC drivers/gpu/drm/drm_gem_cma_helper.o
  LD drivers/gpu/drm/exynos7580/exynos-drm7580.o
  ... exit 0
```

Both configurations compile the file:

* `CONFIG_DRM_KMS_CMA_HELPER=y` (the KMS image, `DRM_EXYNOS7580` selects it):
  the fbdev/CMA branch and `drm_fb_cma_get_gem_obj()` are compiled in;
* `CONFIG_DRM_KMS_CMA_HELPER=n`: the foreign-fb branch compiles to
  `return -ENODEV` and the file has no unused-variable warning.

No error and no warning in this file in either configuration (`-Wformat`,
`-Wunused*`, `-Wint-to-pointer-cast` clean). Remaining warnings in the DRM tree
are pre-existing (`exynos_drm_fb.h` header guard, `exynos_drm_gem.c` and
`exynos_drm_g2d.c` pointer casts, the vendor `exynos_drm_dmabuf.c` `%x`
format) or come from the in-flight DSI patch, not from this slice.

Patch check:

```
$ cd <pristine+M0>; git apply --check patches/drm-kms/0001-kmsdmabuf-import-and-framebuffer.patch
$ cd <pristine+M0>; git apply --check patches/drm-kms/0005-core-drm-gem-cma-import-free.patch
```

Both clean. The slice patch adds only
`drivers/gpu/drm/exynos7580/exynos_drm7580_dmabuf.c`; KmsMain's
`0200-kms-main-bind.patch` already lists `exynos_drm7580_dmabuf.o` in the
`exynos-drm7580-y` group and `CONFIG_DRM_EXYNOS7580` selects
`DRM_KMS_CMA_HELPER`, so no Kconfig/Makefile hunk is needed here.

## 9. Required integration wiring (main slice) and findings for the seat

Already satisfied by `0200-kms-main-bind.patch`:

* `.gem_prime_import = exynos7580_gem_prime_import` and DRIVER_PRIME;
* `.gem_free_object = drm_gem_cma_free_object` **with**
  `0005-core-drm-gem-cma-import-free.patch` applied (otherwise imported
  objects corrupt memory on free);
* `.fb_create = exynos7580_fb_create` via `mode_config.funcs`;
* `exynos-drm7580-y` contains `exynos_drm7580_dmabuf.o`.

Two issues found outside this slice, for the assembly seat:

1. `exynos7580_gem_prime_get_sg_table()` in `exynos_drm7580_main.c` builds the
   export sg table with `sg_alloc_table()` + `sg_dma_address()/sg_dma_len()`
   only. `sg_page()` stays NULL, so the exporter path
   (`drm_gem_map_dma_buf()` → `dma_map_sg()`) dereferences a NULL page when
   panfrost imports the exported dumb buffer (KWin's CREATE_DUMB →
   PRIME export → render-GPU import, requirement 21). It needs
   `sg_set_page()` (or `sg_set_buf(sgt->sgl, cma_obj->vaddr, obj->size)`)
   before the DMA fields. This does not affect any path in this file (CMA
   dumb fbs are mapped from `cma_obj->vaddr`, not from that export sg).
2. The vendor GEM object is not used anywhere; `exynos_drm_gem_free_object()`
   must not be wired as `.gem_free_object` on this driver (it would treat the
   CMA wrapper as an exynos object). Keep `drm_gem_cma_free_object`.

## 10. Risks and deliberate limitations

| Item | Status |
|---|---|
| fb destroyed while still programmed | prevented by the core's fb references held by plane/commit state; DECON commit is synchronous, so the IOVA unmap in `.destroy` happens after the replacement frame latched. KWin's one-fb-per-buffer + release-after-replacement ordering matches. |
| foreign (fb0) mapping pins its fb | bounded: one 770 KiB fb + one IOVM region for the built-in driver's lifetime (cache 4 slots) |
| NV12/NV21 scanout | rejected at ADDFB2; needs a two-plane address story in the plane slice |
| multiple fbs over one imported dma-buf | not deduplicated: each fb maps its own IOVM region from the shared sg table (correct, just more IOVAs). KWin creates one fb per buffer. |
| explicit fences / `IN_FENCE_FD` | not invented (contract, requirement 31): implicit dma-buf fences only |
| modifiers / IN_FORMATS | not advertised; ADDFB2 flags must be 0 |
| `iovmm_map` id | 0; v6 ignores it (verified), v3 would need a real plane id |
| teardown while an fb is mapped | `drm_fbdev_cma_fini()` leaves the pinned fb0 and its mapping; no per-frame leak |

## 11. Verification the integrator should run

```sh
# after assembling the series on a pristine tree
git apply --check patches/drm-kms/0001-kmsdmabuf-import-and-framebuffer.patch
git apply --check patches/drm-kms/0005-core-drm-gem-cma-import-free.patch
make -C code/gpu31/kms/tools/dmabuf          # host validation unit test
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS=-Wno-error -j8 drivers/gpu/drm/

# on the phone, with card0 from the KMS image (no flash here):
modetest -M exynos -c                        # DSI connected, 720x1280
kmscube -A -D /dev/dri/card0 -c 300          # panfrost -> PRIME -> ADDFB2 -> scanout
dmesg | grep -E "iovmm|SYSMMU|Pageflip timed out|timeout of updating decon"
```

Pass: no SYSMMU page faults on the scanout addresses, no `Pageflip timed out`,
no `timeout of updating decon registers`, colours match the rendered buffer.
`/sys/kernel/debug/dma_buf/bufinfo` size and the IOVMM debugfs region count
should be flat at steady state (no growth per flip).
