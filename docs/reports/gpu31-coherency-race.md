# gpu31 coherency race — completion invalidate must precede the fence signal

Date: 2026-11-20. Owner: G31_CoherencyRace. Phone: up, int20 (slot-fix build
`fac8fef`, boot `295ff9b0`); flash blocked (no root).

## 1. Ordering defect (device-observed, mesa-render §1.1)

The pattern job's userspace re-read won a race with `free_job`'s completion
invalidate and printed `USER 0xA5 STALE` while the kernel view (after
invalidate) printed `KERN 0x00000000 ZERO`:

```
USER: word0=0xa5a5a5a5 (prefill, expect 0x00000000) STALE
KERN: word0=0x00000000 ZERO
```

The retired order was:

1. IRQ handler: `dma_fence_signal(done_fence)` — waiters wake here.
2. 0004 `finish_cb` → `finish_work` (async workqueue hop).
3. `free_job`: `p7580_gem_job_done_sync` per BO (the invalidate) + unref.

Any `SYNCOBJ-WAIT` / `WAIT_BO` / mmap-CPU read that runs between (1) and (3)
observes the pre-invalidate CPU alias. The mapping is WC
(`pgprot_writecombine`), but the kernel linear alias is d-cacheable and the
prefill/store-buffer lines are what go stale — either way the fence promise
("wait returns ⇒ data visible") did not hold.

## 2. Fix: invalidate before every fence signal, keep the free_job leg

New helper `p7580_gem_job_bos_pre_signal_sync(bos, bo_count)` (gem.c, decl in
gem.h) walks the job's owned BO pointers with `p7580_gem_job_done_sync` and
drops **no refs** — pure maintenance. Called at all three signal sites in
job.c, strictly before `dma_fence_signal`:

- IRQ DONE path (`p7580_job_irq`): sync → signal.
- `p7580_job_timedout` (post-recovery own-fence signal): sync → signal.
- `p7580_job_recover` (fault `-EIO` signal): sync → signal.

`free_job` (submit.c) keeps its post-signal `job_done_sync` + unref walk
unchanged: belt and braces for waiters that attach after the free
(mmap/CPU reads) and the only leg that drops refs.

`job->bos`/`bo_count` are private to the retiring job, refs held since
SUBMIT, so the walk takes no locks and touches no scheduler/resv/syncobj
state. `job.c` declares the helper locally (no gem.h include) to stay
decoupled from the GEM BO layout; the linker resolves it in-module.

## 3. Why it is cheap and safe (no sleeping under spinlock)

`dma_sync_single_for_cpu` on this tree is cache maintenance + a cheap
identity check, verified against the landed sources:

- arm64 backend (`arch/arm64/mm/dma-mapping.c:284-290`):
  `__swiotlb_sync_single_for_cpu` = `__dma_unmap_area` (dcache-line `dc`
  ops + `dsb`, no locks/MMIO/sleeps/allocations) + `swiotlb_sync_single`.
- `swiotlb_sync_single` (`lib/swiotlb.c:798-816`): `is_swiotlb_buffer`
  range check; direct-DRAM path does `dma_mark_clean` only. The spinlocked
  bounce-copy leg runs solely for real swiotlb bounce buffers, which
  streaming `dma_map_page` never produces here; `dma_to_phys` is identity
  (`asm/dma-mapping.h:55-58`).
- `dma-debug` is compiled out on the j7elte defconfig
  (`# CONFIG_DMA_API_DEBUG is not set`), so `debug_dma_sync_*` is a
  `global_disable`-gated no-op.
- `dma_addr`/`sync_dev` are stable while GPU-bound (set at bind, cleared
  only at unbind/free, both ordered after fence retirement); NULL entries
  are skipped, NULL job/bos is a no-op.

Holds under IRQ-off spinlock (IRQ path) and inside dma_fence callbacks by
the same argument; process-context callers (timeout/recover) inherit it.

## 4. Diff (canon + regen)

- `code/gpu31/gem/panfrost7580_gem.{c,h}`: `p7580_gem_job_bos_pre_signal_sync`
  + decl; completion-order paragraphs reworded (invalidate-before-signal);
  map/unmap contract + DEBUG_READ_BO comments updated (no new maintenance).
- `code/gpu31/jobs/panfrost7580_job.{c,h}`: IRQ/timedout/recover call the
  helper before every `dma_fence_signal`; `job->bos` comment documents both
  legs; local prototype (linker-resolved, no gem.h coupling).
- `code/gpu31/client/pf7580-selftest.c`: pattern-test comments (both views)
  reference the pre-signal IRQ invalidate instead of free_job.
- `code/gpu31/fault/`: mock gains `cpu_visible` per BO (GPU write clears,
  mock demux/recover set before signalling) + F13 `signal implies
  CPU-visible` on DONE and fault paths; `NCASES` 12 → 13.
- `code/gpu31/driver/0001-panfrost7580-driver-sources.patch`: regen via
  `/tmp/genp/gen.sh` after syncing canon → `/tmp/pkg/drv`; 0002 untouched
  (timestamp-only churn reverted); `git apply --check` PASS on pristine tree.

## 5. Verification (exercised)

- Host: `gem/gem_validate` PASS, `jobs/job_test` PASS,
  `fault/fault_matrix` **13/13 PASS** (incl. new F13).
- Kernel syntax (`-fsyntax-only -Wall -Werror -std=gnu89` vs real 3.10.108
  headers + staged 0001–0004 shims): `panfrost7580_gem.c`,
  `panfrost7580_ioctl.c`, `panfrost7580_syncobj.c`, `panfrost7580_job.c`,
  `panfrost7580_mmu.c` all PASS (job.c needs the jobstage shims for 0004,
  gem TUs the gemstage shims — same as the Makefiles).
- 0001 `git apply --check` PASS against the pristine tree.

## 6. Device proof (NOT yet run — needs root flash)

Phone is up on int20; this ticket changes the driver, so Main rebuilds
(int21) and flashes, then:

```
dmesg -c >/dev/null
/data/wdarm 15 /tmp/pf7580-selftest --pattern-test /dev/dri/renderD128
# expect: SUBMIT rc=0, SYNCOBJ-WAIT rc=0, PATTERN MATCH 0x00000000
# (USER zero now — the stale window is closed)
```

## 7. Files changed

- `code/gpu31/gem/panfrost7580_gem.c` (+helper, +comment order)
- `code/gpu31/gem/panfrost7580_gem.h` (+decl, +order comment, debug reword)
- `code/gpu31/gem/panfrost7580_ioctl.{c,h}` (debug comment reword)
- `code/gpu31/jobs/panfrost7580_job.c` (+3 pre-signal calls, +prototype)
- `code/gpu31/jobs/panfrost7580_job.h` (bos two-leg comment)
- `code/gpu31/client/pf7580-selftest.c` (2 comment rewords)
- `code/gpu31/fault/pf7580_mock_gpu.{h,c}` (+cpu_visible model + hooks)
- `code/gpu31/fault/pf7580_fault.h` (NCASES 13)
- `code/gpu31/fault/fault_matrix.c` (F13 + header + NCASES check)
- `code/gpu31/driver/0001-panfrost7580-driver-sources.patch` (regen)
- `docs/reports/gpu31-coherency-race.md` (this file)
