# GPU31 stale-TLB IOVA lifetime fix — free-time unmap + reuse fence + fault-time mapping dump

Date: 2026-11-21. Owner: G31_IovaFinish. Phone: up, int23-era kernel
(`3.10.108`). Mesa 20.3.5 on device (`/data/mesabuild/mesa20` + `-b`),
`egltest --offscreen-only` (via `/data/eglrun.sh`).

Related: `gpu31-instrtype.md` (excl-fence serialisation, landed),
`gpu31-lastjob.md` (descriptor/Mesa-origin analysis + dump PM-hold),
`gpu31-errtrace.md` (ERR pins + `trace_arm`/`dump` knobs).

## 0. Root cause (silicon, `/tmp/g31iova.log` device-only, not committed)

`p7580_gem_free_object` returned the IOVA range (`p7580_iova_free`)
and released the pages **without clearing page-table leaves or
invalidating the TLB**; the `bo_list` entry and its PTEs leaked (the
remove-walk at driver remove was the only unmap). A recycled IOVA
then got fresh leaves at SUBMIT with no pre-invalidate, so the GPU
walked stale PTEs and fetched bytes of a freed BO — the deterministic
`INSTR_TYPE_MISMATCH` on the last submit of a frame.

Method: `/tmp/g31iova.c` + `/tmp/g31iova.so` (device `gcc -shared
-fPIC -ldl`, stay on phone) hooks `drmIoctl` (Mesa 20.3 path):
tracks `CREATE_BO` (handle->iova/size), `GEM_CLOSE`, every SUBMIT
(seq, jc, req, nbos, rc, per-handle iova, w4/w5/type, jc-owning
handle + kernelside 8-word match flag at `jc`), IOVA map
(handle->iova first-seen) + dup/overlap flags. `0xCF` reads run
AFTER submit returns (descriptor bytes are CPU-written before
submit, so fence retirement is irrelevant for them).

Signature (representative; IOVAs vary per run):

```
SUBMIT#122 jc=0x65e1600 req=1 nbos=12 rc=0 own=h4@0x65e1000 dup=0
  cpu w4=0x00010013 w5=0x00000000 64b=1 type=9(FRAGMENT) idx=1 dep=0,0 next=0x0
  h[6]=4(RT 0x41000) h[7]=133(heap 1M) h[9]=6(pool 64K)
  kern h=4 off=0x500 match=1 kv=...00010013...
CLOSE h=6
SUBMIT#... jc=<recycled pool IOVA> ... dup=0
...
p7580_job: ERR slot=0 js_status=0x00000052 (INSTR_TYPE_MISMATCH) ... seq=121 jc=<recycled>
```

`dup=0` on every submit (no two live BOs ever share an IOVA — the
allocator is correct); the faulting `jc` sits inside a **recycled**
range whose previous owner's leaves were never cleared. No live
alias, only a dead one: stale TLB, not a double allocation.

## 1. Ordering contract (the whole fix; no behaviour change beyond this)

- free: leaf-clear (CPU-side, PM-safe) + **PM-held TLB invalidate** +
  `bo_list` unlink + ref drop + DMA unbind, all **BEFORE** `iova_free`
  returns the range and before pages are released.
- map: invalidate-before (purge any entry surviving a dark free or a
  racing PTW) + map (write new leaves) + invalidate-after (publish),
  matching the kbase LOCK/FLUSH/UPDATE discipline.
- Dark block: leaves + list/DMA torn down CPU-side; the TLB flush is
  skipped and re-covered by resume (TRANSTAB + invalidate before the
  next job — same contract as the remove-walk dark leg).

Locking: free path runs under `dev->struct_mutex` (from
`drm_gem_object_free`), takes `bo_lock` (leaf; same order as
`map_bo`) and at most one PM get/put; never sleeps beyond those.
Never-bound BOs (never on `bo_list`) no-op.

## 2. Code changes

`gem/panfrost7580_gem.[ch]` (slice = in-tree source of truth):
- `p7580_gem_free_object` calls `p7580_gem_gpu_unmap(obj)` first,
  then `p7580_iova_free` (restored — an intermediate draft dropped
  the call), then the existing DMA-unbind/page-release legs.
- `p7580_gem_gpu_unmap` is a forward decl here; defined in the
  driver snapshot (`submit.c`, PM-held invalidate + `bo_list` drop).
  `p7580_gem_mmu_register_notify` is a link-compat no-op stub.
- Header restores `p7580_gem_dma_unbind` + `p7580_bo_dma_addr` decls
  (still used by the driver TU) and adds the two decls above.

`jobs/panfrost7580_job.[ch]` (slice = in-tree source of truth):
- `struct p7580_submit_job`: `iova_handles/addrs/sizes` +
  `iova_bo_count` snapshot (handles + IOVA/size while the job owns
  refs); `p7580_job_free` releases all three plus the pre-existing
  `bos`/`bo_handles` frees (also restored) and NULLs + zeroes.
- `struct p7580_job_dev` (kernel branch): `iova_resolve` hook +
  priv. `p7580_job_set_iova_resolver()` registers under `hw_lock`;
  `p7580_job_register_iova_budget()` exposes the driver counter to
  the new `iova_arm` debugfs knob.
- ERR pin: `p7580_job_err_pin()` takes the resolver + snapshot and
  prints `p7580_job: ERR-MAP slot=%d seq=%u nbos=%u <h..=..>` after
  the historic ERR line; NULL hook/snapshot keeps the old form.
  DONE-with-ERR, ERR-only, and TIMEOUT legs all thread it (timeout
  renders inline, process context, same IRQ-safe hook).
- `iova_arm` debugfs file beside `trace_arm` (same get/set shape).
- Incidental: fixed a `void*/int` comment that broke
  `-fsyntax-only` (`Opaque void * plus int-returning`).

`driver/0001-panfrost7580-driver-sources.patch` (regenerated from
the slices + `/tmp/submit_work.c`; no hand-duplicated helpers):
- `drv.h`: `struct p7580_drv_bo` gains `iova` + `size`; decls for
  `p7580_gem_gpu_unmap` + `p7580_drv_register_iova_trace`.
- `submit.c` (= `/tmp/submit_work.c` fold): `p7580_iova_trace`
  budget (64); `p7580_gem_gpu_unmap` (snapshot range under
  `bo_lock`, unlink + unref, leaf-clear, PM-held invalidate, DMA
  unbind, one trace line; never-bound returns via DMA-only leg);
  `p7580_drv_iova_resolve` (IRQ-safe snprintf renderer);
  `p7580_drv_map_bo` gains the overlap `pr_warn` detector + the
  invalidate-map-invalidate fence + map trace line; SUBMIT stamps
  the per-job snapshot (`hsave[]` hoist — `uh` is block-scoped);
  `hsave`/`bos`/`iova_*` all freed on every error leg via
  `p7580_job_free`.
- `main.c`: probe registers the resolver + the trace budget.
- `gem.c`/`gem.h`/`job.c`/`job.h` patch blocks are byte-identical
  to the slice sources (modulo the patch timestamp line).

## 3. Evidence (host; no flash per ticket)

```
code/gpu31/jobs$  make check        # job_test: all checks passed
code/gpu31/gem$   make check        # gem_validate: all checks passed
code/gpu31/driver$ make check       # submit_fix_test: all checks passed
code/gpu31/gem$   STAGE=/tmp/gemstage K=<3.10.108 tree> make kcheck kobj
  # kcheck: 3 files PASS -fsyntax-only (zero warnings)
  # kobj: 3 objects built
code/gpu31/jobs$  <KINC -std=gnu89 -I. -I../iommu> aarch64-linux-gnu-gcc \
                    -fsyntax-only/-c panfrost7580_job.c   # SYNTAX OK / OBJ OK
/tmp/ktree$ git apply --check --unsafe-paths \
              --directory=/tmp/ktree <0001 patch>  # PATCH APPLIES CLEAN
```

`--check` ran against a pristine clone (`git clone /tmp/j7repo`,
`checkout HEAD -- code/gpu31/driver/`); the working tree keeps the
regenerated patch. Slice `Makefile`s have no `kcheck` target for
jobs (host `check` + manual cross commands above); `gem` has both.

## 4. Verification required (Main, int24 build + flash)

1. Rebuild int24 (0001 must survive `--check` + gate:
   `grep -c p7580_gem_gpu_unmap panfrost7580_submit.c` == 2 —
   definition + `p7580_drv_register_iova_trace` caller side — and
   `grep p7580_drv_iova_resolve panfrost7580_submit.c`).
2. `dmesg -c`; `echo 64 > /sys/kernel/debug/p7580/iova_arm`;
   `/data/eglrun.sh`; expect `p7580_iova: map ...` /
   `p7580_iova: unmap ... dark=0` lines pairing per BO, **zero
   `OVERLAP` warns**, zero `ERR`/`ERR-MAP`/TIMEOUT, then
   `egltest RENDER_OK`.
3. If any ERR survives: capture the `ERR` + `ERR-MAP` pair —
   `ERR-MAP` names the faulting submit's handles + IOVAs, so a
   recycled range is decidable from dmesg alone (no shim needed).
   A still-varying fault code with intact `ERR-MAP` reopens the
   visibility leg, not the lifetime leg.

Device-only artefacts (not committed, stay on phone): `/tmp/g31iova.c`
+ `/tmp/g31iova.so` + `/tmp/g31iova.log`, `/tmp/submit_work.c`
(superseded by the folded patch + `/tmp/iovafold/submit_folded.c`
build copy).

## 5. Worklog

- Reconciled the 4 uncommitted slice files (kept header/free work,
  restored 3 dropped calls/frees/decls the draft had clobbered:
  `iova_free`, `bos`/`bo_handles` frees, `dma_unbind`/`dma_addr`).
- Completed the jobs ERR-pin resolver (DONE/ERR/TIMEOUT legs +
  `iova_arm` knob + setter/budget registration) and the driver fold
  (resolver renderer, snapshot stamp with `hsave` hoist, probe
  registration, `drv.h` fields/decls), regenerated 0001 (no content
  removals — only hunk-header/timestamp lines changed), ran host +
  kcheck + pristine `--check` above.
