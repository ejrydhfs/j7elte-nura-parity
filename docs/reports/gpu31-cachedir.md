# gpu31-cachedir — DMA cache-maintenance direction audit for every BO path

Date: 2026-11-21. Owner: G31_CacheDir. Phone: up on **int34**
(`3.10.108 #37 SMP PREEMPT Sat Nov 21 10:12:53 EST 2026`, i.e. the
kernel that still carries the pre-fix order). No flash performed.

## 1. Result in one paragraph

Every DMA sync on the SHMEM BO path was audited against the actual
arm64 3.10 backend in this tree (`arch/arm64/mm/dma-mapping.c` +
`cache.S`). Four sites ran the **wrong direction**: (1) the SUBMIT-time
descriptor snapshot called the completion **invalidate**
(`p7580_gem_job_done_sync`) before reading the JD body, (2) completion
invalidated without a preceding clean, (3) `dma_unmap_page` on the BO
tears down the mapping with an **invalidate** and no preceding clean,
and (4) the SUBMIT sync used `DMA_BIDIRECTIONAL`, which is not wrong for
`for_device` on this backend but documented the wrong intent and left
the invalidate path to be re-used by the snapshot. The fix introduces
two explicitly directed helpers —
`p7580_gem_clean_for_device()` (writeback) and
`p7580_gem_invalidate_for_cpu()` (**clean, then** invalidate) — routes
submit through the first, and routes completion, snapshot reads, the
fault-time `JDBODY` dump and unbind through the second. A host model
test (27 checks, `gem/cachedir_model.c`) proves the ordering and shows
the pre-fix order losing a post-bind CPU fill. A new device test
(`pf7580-selftest --cachedir-test`) proves on int34 that a CPU-filled,
shader-sized BO referenced by a real submit survives intact in **both**
the userspace and the kernel view (96/96 ISA-shaped words, 1023/1023
payload words) and that the GPU's own writeback (`JD[0] = 1`) is visible
in both views.

## 2. Backend semantics actually compiled into this kernel

`arch/arm64/mm/dma-mapping.c` (non-coherent swiotlb ops, line numbers
from the pristine tree):

| call | backend | `cache.S` | op |
|---|---|---|---|
| `dma_map_page(dir)` | `__swiotlb_map_page` :231 | `__dma_map_area` :269 | `dir == FROM` → `__dma_inv_range` ; else `__dma_clean_range` |
| `dma_unmap_page(dir)` | `__swiotlb_unmap_page` :245 | `__dma_unmap_area` :282 | `dir == TO` → `ret` (nothing) ; else `__dma_inv_range` |
| `dma_sync_single_for_device(dir)` | `__swiotlb_sync_single_for_device` :292 | `__dma_map_area` | same as map: **TO/BIDIRECTIONAL = clean**, FROM = invalidate |
| `dma_sync_single_for_cpu(dir)` | `__swiotlb_sync_single_for_cpu` :284 | `__dma_unmap_area` | **FROM/BIDIRECTIONAL = invalidate**, TO = nothing |

Consequences that drive the whole design:

- `DMA_BIDIRECTIONAL` is **clean** on `for_device` and **invalidate** on
  `for_cpu`/`unmap` — so "BIDIRECTIONAL" is not a direction at all, it
  is "clean going to the device, invalidate coming back". Fine as a
  mapping attribute, useless as a description of intent.
- The invalidate is not idempotent with respect to data: it **drops
  dirty lines**. A dirty line is the only copy of the bytes the CPU just
  wrote. Therefore: **clean before every invalidate of a CPU-writable
  range**, and an invalidate is only legal where losing dirty data is
  impossible.
- The clean ends with `dsb` (`cache.S` `__dma_clean_range`), which also
  orders the *submitting thread's* WC store buffer (userspace `mmap` is
  `MT_NORMAL_NC`, `pgprot_writecombine`), so "CPU wrote it, then SUBMIT
  returns" implies "DRAM has it".

## 3. Ordering table (what the code now does)

| # | path | function | call | cache op | why |
|---|---|---|---|---|---|
| 1 | create + CPU fill | userspace WC / kernel `kmap`/`vmap` | – | WC: no lines; kernel: dirty lines | WC writes go straight to DRAM; kernel alias can hold dirty lines |
| 2 | bind (first `map_bo`) | `p7580_gem_dma_bind` | `dma_map_page(BIDIRECTIONAL)` | clean | writeback whatever the CPU filled before the first job; PTEs use `p7580_bo_dma_addr` |
| 3 | submit | `p7580_gem_submit_sync` → `p7580_gem_clean_for_device` | `sync_for_device(TO_DEVICE)` | **clean only** | the BO body is the CPU's output; discarding it here is the bug |
| 4 | snapshot / dump read | `p7580_gem_invalidate_for_cpu` (`submit.c` snapshot, `PTEWALK` `JDBODY`) | `sync_for_device(TO)` **then** `sync_for_cpu(FROM)` | clean → invalidate | must see DRAM (WC/GPU bytes) without dropping a dirty CPU line |
| 5 | completion (pre-signal + `free_job`) | `p7580_gem_job_bos_pre_signal_sync` / `p7580_gem_job_done_sync` → `p7580_gem_invalidate_for_cpu` | same pair | clean → invalidate | GPU bytes visible to a waiter; a CPU write that raced the job is written back, not lost |
| 6 | debug read | `p7580_gem_debug_read_word` | `job_done_sync` then `kmap` | clean → invalidate | same as 4 |
| 7 | unmap / unbind / free | `p7580_gem_dma_unbind` | `clean_for_device` then `dma_unmap_page(BIDIRECTIONAL)` | clean → invalidate | a final CPU write must reach DRAM before the mapping dies |

Reads (4,5,6) intentionally keep the invalidate: the kernel linear alias
is d-cacheable, so an earlier `kmap` read leaves **clean** lines that
would otherwise mask fresher DRAM bytes. That is exactly why the fix is
"clean-then-invalidate" and not "clean instead of invalidate".

## 4. Defects found (pre-fix order)

1. `submit.c` descriptor snapshot (`p7580_lastjob` block): called
   `p7580_gem_job_done_sync(&jbo->base)` — a bare
   `for_cpu(BIDIRECTIONAL)` **invalidate** — before `kmap_atomic`-reading
   the JD body, with the comment "Same invalidate the completion path
   runs, so the snapshot sees CPU bytes". Wrong direction for a buffer
   the CPU just filled; correct only by accident while the range holds
   no dirty line. Now `p7580_gem_invalidate_for_cpu` (clean first).
2. `p7580_gem_job_done_sync` (pre-signal IRQ path, `free_job`, debug
   read): invalidated without cleaning. Any dirty line installed after
   the last submit clean was discarded. Now clean-then-invalidate.
3. `p7580_gem_dma_unbind`: `dma_unmap_page(BIDIRECTIONAL)` =
   invalidate, with no clean for a CPU write made after the last submit.
   Now clean, then unmap.
4. `p7580_gem_submit_sync`: `BIDIRECTIONAL` (clean at the backend) —
   behaviour was right, intent was mislabelled and invited exactly the
   reuse in (1). Now explicitly `DMA_TO_DEVICE` via
   `p7580_gem_clean_for_device`.

Exposure window (important, see §7): the bind-time `dma_map_page` is it-
self a clean and runs **after** the CPU fill at the first submit, so a
userspace WC fill is safe even under the old order. The real loss
window is a **post-bind CPU fill through a cached alias** — the
chain_mode==2 descriptor patch, a `kmap`/`vmap` fill, or a dmabuf
exporter filling its own BO through `p7580_gem_prime_vmap` while the
importer syncs the same physical pages.

## 5. Diff

### 5.1 `code/gpu31/gem/panfrost7580_gem.c`, `panfrost7580_gem.h`

```diff
@@ sync model header (gem.c) @@
-/* Correct discipline is the stock non-coherent DMA-API order, per     */
+/* gpu31-cachedir: the DIRECTION argument of the sync call is what      */
+/* selects the cache op ... (direction table + 6-step ordering table)  */
+/* The rule that bites: an INVALIDATE THROWS DIRTY LINES AWAY.         */
+/* ... the only safe compound op is CLEAN-THEN-INVALIDATE.             */

+static int p7580_bo_sync_ready(struct p7580_bo *bo)
+{
+	return bo->backing == P7580_BO_SHMEM && bo->pages &&
+	       bo->sync_dev && bo->dma_addrs;
+}
+
+static void p7580_bo_sync_pages(struct p7580_bo *bo, int toward_device)
+{
+	...
+		if (toward_device)
+			dma_sync_single_for_device(bo->sync_dev,
+						   bo->dma_addrs[i], PAGE_SIZE,
+						   DMA_TO_DEVICE);
+		else
+			dma_sync_single_for_cpu(bo->sync_dev,
+						bo->dma_addrs[i], PAGE_SIZE,
+						DMA_FROM_DEVICE);
+}
+
+void p7580_gem_clean_for_device(struct drm_gem_object *obj)
+{
+	struct p7580_bo *bo = to_p7580_bo(obj);
+
+	if (!obj || !p7580_bo_sync_ready(bo))
+		return;
+	p7580_bo_sync_pages(bo, 1);
+}
+
+void p7580_gem_invalidate_for_cpu(struct drm_gem_object *obj)
+{
+	struct p7580_bo *bo = to_p7580_bo(obj);
+
+	if (!obj || !p7580_bo_sync_ready(bo))
+		return;
+	p7580_bo_sync_pages(bo, 1);	/* write back dirty lines first */
+	p7580_bo_sync_pages(bo, 0);	/* now drop lines: next read hits DRAM */
+}
+
 void p7580_gem_job_done_sync(struct drm_gem_object *obj)
 {
-	struct p7580_bo *bo = to_p7580_bo(obj);
-	unsigned int i;
-
-	/* CMA (dma_alloc_attrs coherent) never needs CPU-side sync. */
-	if (bo->backing != P7580_BO_SHMEM || !bo->pages)
-		return;
-	if (!bo->sync_dev || !bo->dma_addrs)
+	if (!obj)
 		return;
-	for (i = 0; i < bo->nr_pages; i++) {
-		if (!bo->pages[i])
-			continue;
-		dma_sync_single_for_cpu(bo->sync_dev, bo->dma_addrs[i],
-					PAGE_SIZE, DMA_BIDIRECTIONAL);
-	}
+	/* gpu31-cachedir: completion = clean-then-invalidate. ... */
+	p7580_gem_invalidate_for_cpu(obj);
 }

 void p7580_gem_submit_sync(struct drm_gem_object *obj)
 {
-	... dma_sync_single_for_device(..., DMA_BIDIRECTIONAL);
+	p7580_gem_clean_for_device(obj);
 }

 void p7580_gem_dma_unbind(struct drm_gem_object *obj)
 {
 	if (!bo->dma_addrs || !bo->sync_dev)
 		return;
+	p7580_gem_clean_for_device(obj);
 	for (i = 0; i < bo->nr_pages; i++)
 		dma_unmap_page(bo->sync_dev, bo->dma_addrs[i], PAGE_SIZE,
 			       DMA_BIDIRECTIONAL);
 }

@@ gem.h @@
+void p7580_gem_clean_for_device(struct drm_gem_object *obj);
+void p7580_gem_invalidate_for_cpu(struct drm_gem_object *obj);
+phys_addr_t p7580_bo_dma_addr(struct drm_gem_object *obj, unsigned int idx);
```

The header comment contract (`submit` = clean, `completion` /
`snapshot-dump` = clean-then-invalidate, `unbind` = clean then
unmap-invalidate) and the `debug_read_word` comment were rewritten to
match. `p7580_bo_dma_addr` had drifted out of the repo copy of the
header while `0001` already declared it; the two are consistent again.
Full diff: `git show <commit> -- code/gpu31/gem/`.

### 5.2 Driver snapshot (`code/gpu31/driver/0001-...patch`, regenerated)

```diff
--- a/drivers/gpu/drm/panfrost/panfrost7580_submit.c
+++ b/drivers/gpu/drm/panfrost/panfrost7580_submit.c
@@ PTEWALK JDBODY dump @@
 		if (jd_words > 128)
 			jd_words = 128;
+		/* gpu31-cachedir: this is a kernel READ of BO bytes.
+		 * Clean-then-invalidate the owner BO when the leaf really
+		 * points at it ... */
+		if (owner && bo_ok && leaf_phys == bo_phys)
+			p7580_gem_invalidate_for_cpu(owner);
 		kv = kmap(ppg);

@@ submit BO loop @@
-			/* ... drain those lines to the point the GPU reads ... */
+			/* ... CLEAN (writeback, never invalidate) ... */
 			p7580_gem_submit_sync(obj);

@@ descriptor snapshot @@
-				/* Same invalidate the completion path
-				 * runs, so the snapshot sees CPU bytes. */
-				p7580_gem_job_done_sync(&jbo->base);
+				/* snapshot read = CLEAN-THEN-INVALIDATE ... */
+				p7580_gem_invalidate_for_cpu(&jbo->base);
 				kv = kmap_atomic(jbo->pages[idx]);
```

`panfrost7580_job.c`: pre-signal comment updated to name the pair
(clean `for_device(TO)`, then invalidate `for_cpu(FROM)`); no code
change — the three signal sites already call
`p7580_gem_job_bos_pre_signal_sync` → `job_done_sync`, which now cleans
first.

## 6. Verification (exercised, not intended)

- **Host model test** `code/gpu31/gem/cachedir_model.c` (new,
  `make -C code/gpu31/gem check`): models DRAM + cached kernel alias +
  NC user alias + GPU, implements `__dma_map_area`/`__dma_unmap_area`
  exactly as above, and a direction tracker. 27 checks, 0 failures:
  - backend direction table (clean vs discard per direction);
  - CPU-filled BO survives bind→submit→GPU read→completion→unbind in
    both views;
  - negative controls: the pre-fix submit invalidate and the pre-fix
    completion invalidate both lose a post-bind CPU fill, the fixed
    order keeps it;
  - snapshot read is fresh (stale clean alias line) **and** keeps a
    dirty line;
  - GPU writes visible after completion; unbind keeps a final write;
  - the recorded op sequence equals the ordering table, and the
    invariant "no invalidate without a preceding clean since the last
    CPU write" holds over the whole BO life.
- **Compile**: `make -C code/gpu31/gem kcheck` (aarch64, real 3.10
  headers + staged 0001-0003 shims, `-Wall -Werror`): 3 files PASS with
  zero warnings. The regenerated driver snapshot's
  `panfrost7580_submit.c`, `panfrost7580_job.c` and
  `panfrost7580_gem.c` each pass `-fsyntax-only -Wall -Werror` against
  the real headers + staged 0001-0004 shims (scratch dir
  `work-cachedir/drvstage`).
- **Patch integrity**: regenerated `0001` with whole-file sections
  rebuilt from source (`.git`-prefix and dates preserved); `git apply
  --check` **OK** and `patch -p1 --dry-run` clean on the pristine
  3.10.108 tree. Diff vs the previous `0001` is confined to the two
  hunks above plus the gem file sections; two content-neutral trailing
  blank lines at EOF of the gem files are dropped by the regeneration.
- **Userspace client**: `make` + `make CROSS_COMPILE=aarch64-linux-gnu-`
  with `-Wall -Wextra -Werror`; mock six-ioctl suite PASS. (The
  `AARCH64_SELFTEST` variable was undefined in the client Makefile so
  `make check-aarch64` could not build; one line added.)

## 7. Silicon evidence (int34, pre-fix kernel)

`/tmp/pf7580-selftest-cd --cachedir-test /dev/dri/renderD128` (pushed to
`/tmp`, run under `/data/wdarm 18` as root; new mode of
`code/gpu31/client/pf7580-selftest.c`): a 4 KiB BO holds the 32-B
`WRITE_VALUE/Zero` JD at offset 0 **and** a 96-B shader-ISA-shaped
block (tag 5, first word `0xc87a9835`) at offset 0x100 plus a
deterministic page fill; the job zeroes a second 4 KiB BO (proving the
GPU fetched the first page), then every word is compared against a
pre-submit snapshot in both views.

```
FILL: job BO h=1 gpu=0xc636000 isa[0]=0xc87a9835 result h=2 gpu=0xc637000
SUBMIT: rc=0 (OK) elapsed=0 ms
SYNCOBJ-WAIT(+8s abs): rc=0 (OK) elapsed=0 ms
RESULT: word0=0x00000000 (prefill 0xa5a5a5a5) ZERO (job ran)
USER: 1023/1023 words intact
JD0: 0x00000001 (CPU wrote 0x00000000; the GPU's next/done writeback)
KERN: 31/31 words read, 0 mismatched
CACHEDIR-TEST: PASS (CPU-filled BO intact in both views, job ran)
```

`dmesg` for that run (cleared immediately before): two
`p7580_gem_mmap: ok`, one `p7580_job: irq-enter rawstat=0x00000002
stat=0x00000002`, **no `ERR`/`TIMEOUT`, no fault decode**, then the known
`tiler/shaders/L2 did not idle on power-off` idle warnings.

Reading of this evidence:

- The CPU-filled BO (including the ISA-shaped block) is **intact** in
  the userspace view and in the kernel view after a real submit/retire
  that fetched the page — the ticket's device acceptance.
- `JD[0]` changing to `1` in **both** views is the GPU's own
  next/done writeback: positive proof that GPU writes reach the CPU via
  the completion invalidate (and that the kernel alias is refreshed).
- This runs on the **pre-fix** kernel, so it also shows the direction
  bug does **not** bite userspace WC fills (they create no dirty lines
  and the bind-time `dma_map_page` cleans anyway). The exposed window is
  the kernel-alias fill described in §4. The `0x52` FS-ISA fault of
  `gpu31-fbdtarget` therefore cannot be explained by flushing the
  userspace-written shader BO: something else zeroes that page (or the
  probe reads a recycled VA). The clean-then-invalidate fix still has
  to land because it closes the kernel-alias/dmabuf window; it is not a
  triangle fix.

## 8. Next steps (integrator)

1. Build `int35` on top of int34 with the regenerated `0001` (the only
   patch that changed). All existing build gates are structural and
   unaffected; §6's compile checks were run off-tree.
2. Flash (Main only), then re-run:
   ```
   rootshell sh -c 'dmesg -c >/dev/null'
   /data/wdarm 18 /tmp/pf7580-selftest-cd --cachedir-test /dev/dri/renderD128
   # expect CACHEDIR-TEST: PASS, JD0 non-zero, no ERR/TIMEOUT
   /tmp/pf7580-selftest --pattern-test /dev/dri/renderD128   # unchanged PASS
   /data/eglrun.sh                                            # pixels: still 26,26,102
   ```
   Expected: cachedir PASS (as on int34) and no regression. If a
   triangle appears, it is not from this ticket.
3. If the FS BO keeps reading as zeros with the new kernel, the next
   probe is BO lifetime, not caches: log `shmem_read_mapping_page`
   re-allocation of the FS page and Mesa's BO destroy/alloc around the
   transition the `WATCH` probe localises (report §5.4 shows the change
   with no submit and no mmap in between).

## 9. Files changed

- `code/gpu31/gem/panfrost7580_gem.c` — direction helpers, submit =
  clean, completion/debug read = clean-then-invalidate, unbind clean
  before unmap, header ordering table.
- `code/gpu31/gem/panfrost7580_gem.h` — helper declarations + contract.
- `code/gpu31/gem/cachedir_model.c` — new host model test (27 checks).
- `code/gpu31/gem/Makefile` — build/run `cachedir_model` in `check`.
- `code/gpu31/client/pf7580-selftest.c` — new `--cachedir-test` mode.
- `code/gpu31/client/Makefile` — define the missing
  `AARCH64_SELFTEST` target name.
- `code/gpu31/driver/0001-panfrost7580-driver-sources.patch` — regen
  (gem + submit snapshot/dump + job.c comment), applies clean.
- `docs/reports/gpu31-cachedir.md` — this file.
