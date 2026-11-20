# gpu31 coherency — COHERENCY_NONE DMA sync (GPU writes visible to CPU)

Date: 2026-11-20. Owner: G31_Coherency. Phone: up, int17 (Image
`15cea50a…`, boot `a8c8d091…`).

## 1. Symptom (device evidence, pre-fix baseline)

- `/data/eglrun.sh` (`--offscreen-only`, Mesa 20.3.5 panfrost):
  `60 draws in 0.021 s = 2906.1 FPS`, but
  `readback corner(2,2) RGBA=0,0,0,0 (expect ~26,26,102)` and
  `readback center(128,128) RGBA=0,0,0,0 (expect ~255,0,0)` →
  `RENDER_MISMATCH`, exit 3.
- The GPU is executing: `panfrost7580-job` IRQ 96 → 199 across the run,
  `dmesg` shows an `irq-enter rawstat=0x00010000` storm plus the
  `p7580_gem_mmap` ok rows. No `p7580_sched` trace rows (trace budget
  consumed at boot), but dispatch + IRQ + fence retire are proven by
  the IRQ count increment.

## 2. Root cause

The old completion hook was a no-op on this SoC:

- `p7580_gem_job_done_sync` gated on `PageHighMem` + `flush_dcache_page`.
- arm64 has no HIGHMEM, so the gate never fires.
- Worse, on arm64 `flush_dcache_page()` is only a `PG_dcache_clean`
  flag clear (`arch/arm64/mm/flush.c`) for the icache-alias case — it
  performs no cache maintenance for GPU-written data.

Alias picture (COHERENCY_NONE: kbase forces `system_coherency NONE`,
no ACE, DT has no `system-coherency` property):

- CPU mmap is `MT_NORMAL_NC` (WC, `pgprot_writecombine`) — no cached
  lines there.
- But the kernel linear alias (`page_address`/`kmap`) IS d-cacheable,
  and the GPU port bypasses the CPU caches. Without explicit
  maintenance, CPU reads after GPU writes hit stale lines.
- The correct primitive is the DMA API: `dma_map_page` at bind,
  `dma_sync_single_for_device` at SUBMIT (CPU→GPU drain),
  `dma_sync_single_for_cpu` at completion (GPU→CPU invalidate). On
  this tree (non-coherent swiotlb ops) map == cache maintenance, and
  `sync_single_for_cpu` is exactly
  `__dma_unmap_area(phys_to_virt(dma_to_phys(dev, dma)), …)` +
  `swiotlb_sync_single_for_cpu`.

## 3. Model (what this ticket implements)

Per SHMEM BO, while GPU-bound:

1. **Bind** (`p7580_gem_dma_bind`, before the AS0 page walk):
   `dma_map_page(dev, page, 0, PAGE_SIZE, DMA_BIDIRECTIONAL)` per page,
   cached in `bo->dma_addrs[]` + `bo->sync_dev`. Idempotent; partial
   failure unwinds. `SKIP_CPU_SYNC` never set.
2. **Map**: PTEs carry the bus address from `p7580_bo_dma_addr`
   (bind cache, `page_to_phys` fallback only when never bound), never
   `page_to_phys`/`sg_phys` directly.
3. **Submit** (`p7580_gem_submit_sync`, after CPU fills
   descriptors/payloads, before queue): `dma_sync_single_for_device`
   per bound page, `DMA_BIDIRECTIONAL`.
4. **Completion** (`p7580_gem_job_done_sync`, free_job after the fence
   signals): `dma_sync_single_for_cpu` per bound page,
   `DMA_BIDIRECTIONAL`. No MMIO, no sleeps, no locks. CMA/imported/
   never-bound BOs are no-ops.
5. **Unbind** (`p7580_gem_dma_unbind` in `p7580_drv_unmap_bo` and the
   free path): `dma_unmap_page` per page, NULL/NULL after.

## 4. Diff (canon + regen)

- `code/gpu31/gem/panfrost7580_gem.{c,h}`: sync model (§3);
  `struct p7580_bo`: `dma_addrs` + `sync_dev` replace `kmap_flushed`;
  `p7580_bo_page_phys` SHMEM arm routes via `p7580_bo_dma_addr`;
  free tears the mapping down. Also restored the `p7580_bo_npages`
  declaration the header had lost (submit.c needs it).
- `code/gpu31/client/pf7580-selftest.c`: `--pattern-test PATH`
  ("coherency proof"): 56-byte WRITE_VALUE job (Mesa 20.3
  `midgard.xml`: type 2, payload `u64 addr / u32 type=6 Immediate-32 /
  u64 imm`) writing `0xC0C3E8E1` to a second 4 KiB result BO;
  SUBMIT (req=0 → JS1, both BOs, out_sync) + fence-wait + unmap +
  fresh re-mmap + compare. MATCH proves CPU→sync→GPU→IRQ→sync→CPU.
  MISMATCH-with-fence-signalled = stale lines; MISMATCH + JS_STATUS
  fault in dmesg = bad job/VA.
- `code/gpu31/driver/0001-panfrost7580-driver-sources.patch`: regen
  from canon (gem + submit.c driver-side: bind in `map_bo`,
  `submit_sync` per BO after map, `dma_unbind` in `unmap_bo`,
  F-JT2 comment update). Abialign hunks preserved byte-identical
  (`+1,340` ioctl hunk, SYNC_FILE masks, cpin/cpout caps, abi_check
  placement); `git apply --check` + `patch -p1 --dry-run` PASS.
- `code/gpu31/series/build-int18.sh`: copy of int17 with int18 names +
  header noting the coherency change.

## 5. Verification (exercised)

- Host: `gem/gem_validate` PASS, `jobs/job_test` PASS,
  `client` mock suite PASS (incl. Mesa-exact ABI rows); new
  `--pattern-test` code path compiles (host + aarch64, `-Werror`).
- Kernel syntax (`aarch64-linux-gnu-gcc -fsyntax-only -Wall -Werror`
  vs real 3.10.108 headers + staged 0001–0004 shims):
  `panfrost7580_gem.c`, `panfrost7580_ioctl.c`,
  `panfrost7580_syncobj.c`, `panfrost7580_job.c`,
  `panfrost7580_mmu.c`, `panfrost7580_submit.c` all PASS.
- Image: int18 build from pristine tree (9 patches `--check` clean,
  `-j4` RC=0): `Image-int18` 18160760 B
  (`c9bc0634…`), `kernel-boot-int18.img` 27299840 B
  (`06b4ab33…` = kernel 18160760 + ramdisk 8822392 + dt 313344,
  stock dt.img + reference ramdisk-new.gz). Pushed to the phone as
  `/home/user/boot-int18.img` (sha256 match). Gates: int17's
  ops/slot/ran + IRQ + ABI gate logic unchanged in the script; the
  build ran through OBJCOPY (gates run before pack — rerun the script
  to regenerate evidence.log if the pack step's ramdisk seed raced).

## 6. Device proof (NOT yet run — needs root flash)

The phone is up on int17; `/home/user/boot-int18.img` is staged but
flashing `/dev/mmcblk0p10` needs root (`su` prompts for a password,
`sudo` config broken). After flash (Main):

```
dmesg -c >/dev/null
/data/wdarm 15 /tmp/pf7580-selftest --pattern-test /dev/dri/renderD128
# expect: SUBMIT rc=0, SYNCOBJ-WAIT rc=0, PATTERN MATCH 0xC0C3E8E1
/data/eglrun.sh   # expect RENDER_OK, non-zero pixels
```

## 7. Files changed

- `code/gpu31/gem/panfrost7580_gem.c` (+bind/submit-sync/done-sync/unbind/dma-addr, page_phys route, free teardown)
- `code/gpu31/gem/panfrost7580_gem.h` (+decls, −kmap_flushed, +npages restore)
- `code/gpu31/client/pf7580-selftest.c` (+`--pattern-test`)
- `code/gpu31/driver/0001-panfrost7580-driver-sources.patch` (regen)
- `code/gpu31/series/build-int18.sh` (new)
- `docs/reports/gpu31-coherency.md` (this file)
