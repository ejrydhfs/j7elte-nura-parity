# Panfrost on j7elte — final report (2026-11-21)

## What works (verified on silicon, kernel #37 int25)
- **Render-only DRM node**: `/dev/dri/renderD128` only; no `card0`; Xorg/fbdev untouched.
- **Full v5.2 ioctl surface** through our kernel: GET_PARAM (`0x720`), CREATE_BO (shmem-backed,
  16 MiB OK), MMAP_BO, GET_BO_OFFSET, SUBMIT, WAIT_BO, plus generic DRM syncobj (7 ioctls).
- **Job dispatch and completion**: JS0/JS1 routing per mainline (req0 chain -> JS1, FS -> JS0),
  `JS_CONFIG` per jobabi, IRQ demux (STAT|RAWSTAT), fence signal on IRQ, bounded timeouts,
  PM lifetime + teardown guards, clean close.
- **GPU memory writes**: Zero-fill WRITE_VALUE over a page pre-filled `0xA5A5A5A5` -> both
  kernel (`DEBUG_READ_BO`) and user views read 0. Coherency model (dma_map bind,
  sync_for_device submit, pre-signal sync_for_cpu) verified.
- **Mesa 20.3.5 drives the GPU**: `GL vendor=Panfrost`, `renderer=Mali T720 (Panfrost)`,
  GLES 3.0; hundreds of submits per run; **framebuffer clear is pixel-exact**
  (`RGBA=26,26,102,255`).

## The one open defect
Per frame, exactly one submit faults:
```
p7580_job: ERR slot=0 js_status=0x00000052 (INSTR_TYPE_MISMATCH)
    head=tail=0x21c5600 jc=0x21c5600 seq=121 w4=0x00010013 w5=0 cfg=0x00083700
p7580_job: ERR-MAP ... h6=0x21c5000/0x10000 ... (no overlapping live mappings)
```
- Descriptor bytes are legal (64-bit header, type 9 FRAGMENT), byte-identical in shape to
  fragment jobs that complete.
- Deterministic: same slot, same type, always the last submit of Mesa's flush batch; code
  varies slightly (0x52/0x58) with freed-page contents.
- Fixes tried and verified as *not* the cause: slot routing, JOBCHAIN bit, excl-fence publish,
  IOVA overlap (0 overlaps), arm-time fence install.
- Leading hypothesis after all measurements: the GPU's view of the instruction buffer differs
  from the kernel's view at execution time (BO reuse / PTE-TLB lifetime across Mesa's batch
  pool), i.e. a *visibility/timing* defect, not an encoding one.

## Next experiment (single, decisive)
Instrument the **PTE path at fault time**: on ERR, walk the AS0 page tables for `jc` and compare
the leaf's physical page against the owning BO's current page array, then also re-read that
physical page via `kmap` in the same handler. If they differ, it is a page-table/IOVA lifetime
bug (fix in map/unmap + invalidate ordering); if they match, the GPU's fetch is stale only in
its own cache/TLB and needs a stronger invalidate before `START`.

## Delivered patches (repo)
- `patches/drm-backport/0001-0005` (fence/resv/syncobj/sched shims, 280 host checks)
- `code/gpu31/kms` Mode-B v2 render-only
- `code/gpu31/{gem,jobs,mmu,iommu,power,devfreq,dmabuf}` + driver 0001/0002
- Reports: `gpu31-*.md` (target, iommu, mmu, jobs, gem, dmabuf, kms, drmcore, sched, pm-order,
  close-fix, submit-hw, jobtrace, probernumeric, opsfix, abialign, gpuwrite, coherency,
  mesa-render, tilerchain, primitive, errtrace, instrtype, iova, armfence, integrations 1-25)

## Status summary
| Area | State |
|---|---|
| Kernel driver | complete and stable (render-only, PM, close, syncobj, shmem) |
| GPU execution | works (jobs, IRQ, fences, memory writes) |
| Mesa bring-up | works (renderer, submits, clear) |
| Draw geometry | **open**: one deterministic fragment fault per frame |
| 2D / UI | unaffected, fbdev X healthy |
