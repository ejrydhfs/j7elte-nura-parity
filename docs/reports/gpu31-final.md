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

## Update 2026-11-21 (int31, decisive comparison)
Submit-time snapshot vs fault-time bytes at the same address:
```
sw: 00000000 00000000 00000000 00000000  00010013 00000000 00000000 00000000
w : 00000052 00000000 021d6005 00000000  00010013 00000000 00000000 00000000
```
- Page tables are **correct** (`PTEWALK ... leaf_phys==bo_phys MATCH`), so IOVA/TLB lifetime is not the cause.
- At SUBMIT the first four words are zero and the header-like word sits at offset 0x10; at fault the GPU
  has written status (`0x52`) and a fault pointer (`0x021d6005`) into w0/w2.
- This points to a **descriptor-layout mismatch**: Mesa's `mali_job_header_packed` is at the start of the
  job's CPU buffer, yet the block we program at `JS_HEAD` shows a valid type word only at +0x10.
  Open question: whether our submit must point HEAD at `jc` or whether the snapshot/offset used for
  compare is off by 0x10 for this batch.

### Next single experiment
Decode the exact JD layout from Mesa 20.3 `pan_job.c` / `mali_job_header_packed` writers and compare with
`panfrost_job_hw_submit` in v5.2 (which sets `HEAD_NEXT = jc`); instrument one draw to print `jc`, the
first 16 words the *CPU* wrote at that address, and the words the driver programs. That resolves
"offset vs writeback" without further speculation.

### Correction (same day)
Mesa sets `submit.jc = first_job_desc` (pan_job.c:960), i.e. the header **is** at offset 0 of the
job's CPU buffer. Therefore the submit-time `sw` line showing `w0..w3 = 0` is an artefact of the
snapshot's owner lookup (it read the wrong BO), not the job's real content. The fault-time `w0=0x52`
is the hardware's exception-status writeback into the header.

Net: the descriptor is valid, the tables are correct, and the GPU still reports
INSTR_TYPE_MISMATCH while executing the fragment job's command stream. Resolving this needs
genxml-level decoding of the JD body (FBD/shader pointers) against Mesa 20.3's writer — a
substantially larger effort than the bring-up done here.

## Update 2026-11-22 (int38 staged, device off USB)
Root causes found since the previous update, all fixed in code:

1. **Vertex-job truncation (chain split)**: `p7580_chain_job_bytes()` returned 96 for a
   `MALI_COMPUTE_JOB_LENGTH`=192 vertex aggregate, so the split shadow dropped Draw fields
   from Textures onward (samplers, uniforms, attribute/varying pointers, viewport, FBD).
   Fixed in 742dc52 (VERTEX/TILER 192, WRITE_VALUE 56, FRAGMENT 64, NULL 32).
2. **Shader page zeroing (Defect B)**: `shmem_getpage_gfp()` leaves a dirty zero line in the
   kernel's Normal-WB linear alias; this arm64 tree's `flush_dcache_page()` only clears
   `PG_dcache_clean`, and the BO VMA is WC, so the CPU's ISA stores reach DRAM while the stale
   line later evicts over them (whole page zeroed 0-5 ms later; a 2 s pre-sleep prevents it).
   Fixed in 9dd7542 with `__flush_dcache_area()` after each `shmem_read_mapping_page()`.
3. **Chain walker** proven correct on a 3-node NULL-job chain (`chain nodes=3`, three splits,
   `cfg=0x83700`, `next` = w6|w7<<32 with Is64=w4 bit0).

Independent audits: ISA decode shows **no T720-illegal encodings**; the TILER descriptor is
byte-exact against kbase/Mesa (FBD at 44*4, tiler section at FBD+192, heap/PLS/primitive valid);
`JS_CONFIG 0x83700` matches kbase r8p0.

Image **int38** (`42d3a160…`, kernel `3bbcc91a…`) contains both fixes and is staged. Flashing and
the acceptance runs (`--defectb-test`, chain-split trace, polygon census, FS-only matrix) are
**blocked on physical power-cycle**: the phone is off USB (boot stalls before sshd).

## SUCCESS 2026-11-22 — hardware 3D renders on j7elte
Kernel **int38** (`42d3a160…`, kernel `3bbcc91a…`) on silicon:

- `pf7580-selftest --defectb-test` → **PASS 32/32**, `lost-immediate=0`, `lost-after-3000us=0`
  (the shmem linear-alias dirty-line defect is fixed by `__flush_dcache_area()`).
- `egltest --offscreen-only` (Mesa 20.3.5, `MESA_LOADER_DRIVER_OVERRIDE=panfrost`) →
  `GL vendor='Panfrost'`, `renderer='Mali T720 (Panfrost)'`, GLES 3.0,
  corner `RGBA=26,26,102,255` (clear), centre `RGBA=255,0,0,255` (triangle),
  **`RENDER_OK` / `PASS`**, three consecutive runs, `ERRCOUNT=0` GPU faults.
- Throughput in that test: 60 draws at 256x256 in ~0.17 s (~350 FPS incl. readback).

Winning fixes that made it work (all in this tree):
1. render-only DRM node (no `card0`), full v5.2 ioctls + syncobj, PM lifetime/teardown guards.
2. job submission: slot routing, IRQ STAT|RAWSTAT demux, arm-time fence + syncobj semantics,
   multi-in-fence dependencies, per-file serialisation.
3. GEM: shmem-backed BOs, `VM_MIXEDMAP` page references, correct cache directions, and the
   `__flush_dcache_area()` fix for the shmem zero-line inheriting over WC stores.
4. driver-side chain split with correct 192-byte VERTEX/TILER payloads, giving the
   VS→tiler submit boundary (flush) that Midgard needs.
5. Mesa 20.3.5 paired with the v5.2 UAPI; attribute-array bounds fix (0007).

Known follow-ups: glmark2 cannot run surfaceless on this stack (canvas only), so benchmark
numbers must come from our own EGL clients; debug probes (`0xCF`, JDBODY/PTEWALK/ledger,
`chain_mode`) are marked for strip before cutover.

## Performance (int38, 256x256 triangle, Mesa 20.3.5)
| Engine | Workload | FPS | ms/frame |
|---|---|---|---|
| Panfrost (Mali T720) | 300 frames, glFinish each | 24.4 | 41 |
| llvmpipe (8x A53) | same | 2365 | 0.42 |
| Panfrost | 60 draws in one batch + readback (egltest) | ~357 | 2.8 |

Interpretation: shading is not the bottleneck — per-flush submit latency is (~40 ms/frame:
submit -> sched -> split jobs -> IRQ -> fence -> user wait). Batching hides it; per-frame
flushes expose it. Reducing that latency is the next step (`gpu31-latency.md`).
Also: 512x512 is **not** an MFBD path — T720 is SFBD-only and a 512² target
uses a single SFBD (tiles 0,0..31,31); it renders clean via `glFinish`. The
reported 512²/readback "timeouts" are fault-recovery latency, not FBD type
(`gpu31-mfbd.md`, `gpu31-latency.md`).

## After int39 (2026-11-22) — latency fixed, readback faults remain
- Fault-IRQ timeout kick works: 30 readback frames went from ~45 s to **5.58 s** (fences signalled in ms;
  no more 5 s stalls).
- No-readback bench (glFinish/frame, 256², 300 frames): **141 fps** (7 ms/frame) vs llvmpipe ~1000 fps.
- Per-frame `glReadPixels` still faults the fragment job: `POWER_FAULT 0x41` (36/36 at 256², 19/19 at 512²);
  the clean ordering is `glFinish()` **before** readback (verified at 512²). Tracked in `gpu31-readback.md`.

## int40 (2026-11-22) — power-recovery fix; long runs still fault
- Cores-off busy guard + power-cycle recovery landed: `recover-done ... cycle=1 ... intmask=0x00070007`,
  zero "did not idle" warnings, no wedges requiring power cycles.
- Short runs are clean: 256² readback 51 fps / 0 ERR; 512² readback 11.6 fps / 0 ERR; `RENDER_OK`.
- Long runs still fault: 300-frame no-readback = 55 ERR/TIMEOUT and 17.6 fps (0x58 DATA_INVALID_FAULT,
  0x41 POWER_FAULT). First fault's descriptor had word0 (exception_status)=1 on a 4 KiB pool BO —
  origin under investigation (`gpu31-firstfault.md`).

## int41 (2026-11-22) — stable GPU, 155 fps sustained
- Descriptor hygiene (words 0..3 zeroed in shadows) removed the long-run fault storm:
  **3 x 300 frames with 0 ERR / 0 TIMEOUT / 0 RECOVER at ~155 fps** (256², glFinish/frame).
- `RENDER_OK` (clear + red triangle) still holds; 512² readback 0 faults (12.4 fps, CPU detile bound).
- `serialize_fs=1` is harmful (34 faults + "did not idle"): default stays 0.
- Residual: occasional silent hang (`js_status=0x08`, no IRQ) costs the 5 s sched timeout; a
  dispatch watchdog slice is in flight (`gpu31-watchdog.md`).

## int49 (2026-11-22) — armed diagnostics; GPU clean in 19 consecutive runs
- prof_arm restored (int48 registered the knob into a dir it did not own): `ls` lists `prof_arm`,
  write 1 reads back 1.
- Armed 300-frame loop, 256²: 152.4 / 155.8 / 155.6 / 153.2 fps, `suberr=0`, no TIMEOUT/EVRING.
- 15-run armed hunt: **0 faults / 15 runs**, 151.4-156.3 fps; two runs at ~105 fps with no
  logged fault (the known slow window).
- 512² x40 readback: 6/6 clean, 39-58 fps.
- Window counters per 100 SUBMITs (ns totals): submit 6.7 us/SUBMIT, wait 1.09 ms/213 calls,
  freejob 7.0 us/200, jobsync 26 us/4000, create 0.45 us/50.
- Residual: intermittent silent `0x08` (~1 in 4-6 runs, last seen before int49) not reproduced in
  these 19 runs; push to GitHub blocked by credentials (403, account `alosslessdev`).
