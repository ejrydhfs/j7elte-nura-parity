# integration94 — frame identity + GPU submit-dependency evidence

The panel now shows content under `startplasmamobile` (blue wallpaper, then
alternating blue/black), and the GPU log explains the failures:

```
MESA: error: panfrost_batch_submit failed: 7          (kwin)
p7580_job: TIMEOUT slot=0 js_status=0x00000008 (NO_EXCEPTION) stat=0 rawstat=0 head==tail seq=299 jc_age=1
p7580_job: recover-done slot=0 latched=0x8 cycle=1
```

i.e. the render/Mesa path's silent start timeout recurs under the compositor;
frames never finish and Mesa's submit fails.  int94 adds identity logs on both
sides of the boundary so the presented-buffer question and the dependency
question are answered by one capture.

## KMS side (`patches/drm-kms/0144-kms-frame-accounting-buffer-sample.patch`)

- `exynos7580_atomic_commit()`: commit sequence counter, per-commit frame
  counter reset, `seq=N start by <comm>`, and after the tail
  `seq=N commit_tail done, frames=M`.
- `exynos7580_plane_commit_frame()`: every DECON commit logs
  `seq=N frame=M astate=… plane=… fb=… fb_id=… addr0/1/2=… dst=… nplanes=N`
  (fb from the new plane state) plus the `decon_kms_commit()` rc.  Two lines
  with frame=1,2 for one seq prove the commit path runs twice per atomic
  commit; alternating addresses name the presented BOs.
- `exynos7580_own_scanout()`: samples the first 16 bytes of the buffer about
  to be presented on every call (before the cached-address return):
  `sample import|cma fb=… id=… px=…`.  A painted AR24 frame prints its colour;
  an unpainted BO prints zeros.
- shim `decon_kms_commit()`: monotonic `call=N` with nwin/state/comm.

## GPU side (`code/gpu31/driver/0139-panfrost7580-submit-deps.patch`)

- `p7580_drv_submit()`: per SUBMIT summary (jc, slot, requirements, BO/in-fence
  counts, out_sync, submit_no, comm) and per BO the iova/size plus its resv
  exclusive fence (pointer, context, seqno, signaled); per in-fence the same
  fence state.
- `p7580_drv_split_queue()`: for node 0, dump prev (serial/node) fence, every
  in-fence and every BO resv exclusive fence with its signaled state **before**
  dependencies are added; count the dependencies actually added and log
  `deps_queued=N` immediately before `drm_sched_job_submit()`.
- Resv publication: per BO iova + the exclusive fence being added
  (context/seqno) under serial_lock, and the resulting per-file
  `serial_prev`.

## Fence audit (source, no change)

- This 3.10/M0 core has **no atomic IN_FENCE_FD property and no fence array in
  `struct drm_atomic_state`** (the only in-fences are the panfrost SUBMIT
  syncobj deps), so a compositor cannot hand its render fences to the KMS
  commit.
- The KMS dma-buf import (`exynos7580_gem_prime_import`) takes an attachment
  and an sg table only; 3.10 dma-buf has no `dma_resv`, and the KMS path adds
  no fence to the BO's resv.  A KMS-taken excl fence on a GPU BO (the
  cross-device cycle hypothesis) is therefore not produced by this code; the
  new `p7580_deps` BO lines will show any resv fence that is there and whether
  it is pending.
- A job left with head==tail and js_status 0x08 while the sched entity queued
  it is consistent with either a never-signaled dependency (visible in
  `deps_queued` + the pending-fence lines) or the known 0x08 start-timeout
  class (already recovered), now reproducible under KWin's batch shape.

## Artifacts

| file | size | sha256 |
|---|---|---|
| `Image-int94` | 18191112 | `e73c6945ce4100719d08c8a85fe8d0c0d26316b8ee03ee7c362a23997fa1bed3` |
| `kernel-boot-int94.img` | 27330560 | `2701f655e7b9679560dc5300599cec9055f89d69d62bb61fb59b46ca8956ae1d` |
| `exynos-drm7580-int94.ko` | — | `fa2f505b9caeafbdacb23dc8f62c23c8738e43e7e03f0421f5e5b22a74fc3f44` |
| `drm_kms_helper-int94.ko` | — | `807aa0c52cdf61d11c44858ee03a90c590709799227a3ea547b7d54004ef5aa9` |

Build 0 errors.  All int93 gates plus the int94 KMS-frame and GPU-submit gates
PASS (strings verified in vmlinux and the KMS .ko).  Log sha256s: build
`c288d1fd2f1019f1d97cf8ea522e1f2d55de17d69301a1618997289af63c8b1d`, gates `666ef40549867c351aebac43576877ec3dcaf887e7ce3945b24e7055425095f3`, evidence `a2c56c53400f06b702a6b677ebe922c2d26702b2e3ba2406a7388cfe2d8eee9c`, config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.

## Capture recipe (kmscube reference, then KWin)

```
dmesg | grep -E 'exynos7580_atomic_commit: seq=|exynos7580_plane_commit_frame|own_scanout: sample|decon_kms_commit: call=' > kms.txt
dmesg | grep -E 'p7580_(submit|deps|resv|serial)' > gpu.txt
```

- For each atomic commit: frames must be exactly 1, and `fb_id`/`addr0` must
  change only when the fb changes.
- `sample … px=` alternating between the wallpaper colour and
  `00000000 00000000 00000000 00000000` proves a rendered/unrendered BO
  alternation at presentation time.
- `p7580_deps` names the pending dependency; `deps_queued` plus the signaled
  states show whether the sched job was queued behind an unsatisfied fence.
  If `p7580_submit` BO lines show a pending resv excl that is not this
  device's fence, the cross-device cycle exists and the fix is to stop taking
  it (or filter it); if everything pending belongs to our own GPU, the 0x08
  is the media-driver start-timeout class and the next step is on that side.
