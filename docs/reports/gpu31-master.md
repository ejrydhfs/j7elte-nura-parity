# GPU31 master report — hardware 3D on j7elte (Mali-T720 / Exynos 7580 / Nura 3.10.108)

Date: 2026-11-19. Device: SM-J700M (j7elte), Nura Alpine aarch64.
Live state (verified 2026-11-19 ~06:27 UTC via locked SSH): phone **UP**,
kernel `3.10.108 #35 SMP PREEMPT` (gcc 16.2.0, built Wed Nov 18 23:36 EST),
uptime 1 min (fresh boot — do NOT reboot: cold boot is scarce, GPS/FM share
the phone). `/dev/mali0` present (`Mali-T72x MP2 r1p0 0x0720`); `/dev/dri`
absent (running kernel predates M1). Goal: hardware 3D (GL/EGL on Mali-T72x).
Today GL is llvmpipe; kbase UK 10.6 works incl. job submission.


Related: `docs/reports/gpu-verify.md` (HW/userspace ground truth),
`gpu-drm.md` (M1–M3 milestones + evidence + pending-HW marks),
`gpu-panfrost.md` (backport audit, M0 delivered), `gpu31-dt.md` (this day's
first landed slice).

## 1. Milestone status

| Milestone | Definition | Status | Evidence |
|---|---|---|---|
| M0 bring-up | kbase works, soft-GL baselined | **DONE** | `gpu-verify.md` §2–§4: kbase-probe handshake 10.6, winsys soft job (223 GPU cycles), SAME_VA BO, fence round-trip; X fbdev 720×1280, 2065-colour XWD; llvmpipe GL 4.6/GLES 3.2, glxgears 173–221 FPS, glmark2-es2 29-scene table |
| M1 `/dev/dri` + fbdev X intact | `CONFIG_MODULES=y` + minimal `CONFIG_DRM=y` fragment (6 symbols), stock `dt.img`, vendor DECON fbdev keeps panel | **BUILT, flash GATED (phone up, #35 running, no /dev/dri)** | `gpu-drm.md` M1: Image 18 094 328 B, boot img 27 234 304 B (`53acd61a…`), readiness READY; `m1-kconfig-check.sh` PASS; `m1-verify.sh` + `run-m1.sh` scripted. **Flash gate: only G31_Integrator builds; only G31_M1Verify flashes, after Main's explicit go. No reboot until then.** |
| M2 render node | Mali bound to a DRM render node (H1 panfrost UAPI preferred / H2 kbase-UK tunnel fallback) | **Designed, code in flight** | `gpu-drm.md` M2: ADAPT.md + adapt diff (dry-run clean), render-node-probe (`make check` native+qemu pass). gpu31 `wsi/tri-x11.c` (X11/EGL/GLES2 swrast path demo) present; `mesatests/` staged empty |
| M3 Mesa GL on HW | panfrost v5.2 backport (fence→resv→syncobj→sched shims + GEM/job-slot driver) + Mesa build + glmark harness | **Foundation in flight; driver not written** | `patches/drm-backport/0001-0005` (host series, check+compile PASS); gpu31 `series/` mirror (fence/resv/syncobj/sched + KMS 0001 + wiring + m1 fragment + SHA256SUMS) present. Panfrost GEM/job-slot driver itself: the honest gap (~20–30 person-days per `gpu-panfrost.md` M2) |

## 2. Day-plan slice tracker (30 slices)

Landed (committed to repo):

| Slice | Output |
|---|---|
| gpu31-dt | `docs/reports/gpu31-dt.md`: stock DT GPU surface enumerated (mali@0x11400000 + 6 props, pd-g3d, BUCK4/`vdd_g3d`, bus_mif/int, 3×TMU; no OPP/`mali-supply`/cooling/IOMMU), zero-DT-change reliance plan + per-property in-driver fallbacks |

In flight (uncommitted under `code/gpu31/`, not yet reviewed/landed):

| Slice dir | Contents (this checkout) |
|---|---|
| `series/` | `m1-kconfig.fragment` (108 lines, annotated), `0001-dma-fence-minimal-subset.patch`, `0002-dma-resv-shim.patch`, `0003-drm_syncobj-subset.patch`, `0004-drm_sched-panfrost-job-only-shim.patch`, `0005-build-wiring-for-shims.patch`, `0001-drm-exynos7580-decon-dsim-kms.patch` (3112 lines, DECON/DSIM KMS), `SHA256SUMS` |
| `iommu/` | `panfrost7580_mmu.h/.c` (116+360 lines): GPU-own-MMU decision (no sysmmu/IOMMU API, no DT `iommus`), AS_TRANSTAB/MEMATTR/TRANSCFG/COMMAND programming, dma_map identity |
| `wsi/` | `tri-x11.c` (228 lines) + `Makefile`: EGL/GLES2 X11 triangle demo (swrast path on `:0`) |
| `mesatests/` | staged, empty |

Remaining slices (no output yet in repo; owners per roster): Gem, DmaBuf,
Power, Jobs, Tests, Mesa, Devfreq, ConfigMatrix, Packaging, RenderProbe,
Firmware, Stress, Review, DrmCore, Suspend, PanfrostTarget, Thermal, Mmu,
Recovery, KbaseShim, Sched, M1Verify, Kms, PowerMeasure, MesaTests,
KbaseDocs, Integrator (builds only) + M1Verify (flashes only, after Main's
explicit go). All others are host-side (`-fsyntax-only`/`-c` read-only) +
read-only device checks via `flock /tmp/gpsdev.lock` (short holds, no reboot).

## 3. Invariants (hold for every slice)

- Shared kernel tree is never edited; slices produce patch files under
  `code/gpu31/<slice>/` — only the Integrator applies/builds.
- Stock `dt.img` is never rebuilt (S-Boot hangs on rebuilt DTBs); new code
  probes the stock node as-is with in-driver fallbacks (`gpu31-dt.md` §4).
- Phone must stay bootable with fbdev X working; GPS driver stays stock parity.
- **Do NOT reboot the phone** (cold boot is scarce; GPS cold-boot runs + FM
  audio share the device). No flash without Main's explicit go.
- Root fs 94% full: big writes go to `/data`. Device via
  `flock /tmp/gpsdev.lock -c '…ssh…'` (short holds <120 s), root via `rootshell`.

## 4. Evidence links

- `docs/reports/gpu-verify.md` — kbase/llvmpipe/X baselines + FPS tables.
- `docs/reports/gpu-drm.md` — M1–M3 config/patch diffs, flush procedure,
  per-milestone hard evidence, pending-HW marks.
- `docs/reports/gpu-panfrost.md` — backport audit (absent/present table),
  effort 80–125 d kernel-only, R1/R3/R4/R5/R7/R8/R10.
- `docs/reports/gpu31-dt.md` — stock DT tables + reliance plan.
- `code/gpu31/series/SHA256SUMS` — series hashes.
- `code/kernel/m1/m1-verify.sh`, `code/userspace/gpu/glmark-harness/run-m1.sh`
  — post-boot asserts (card0+controlD64, `dmesg` exynos init, X 720×1280).

## 5. Open risks

1. **M1 flash gate** (phone up on #35, `/dev/dri` absent as expected):
   M1Verify flashes only after Main's explicit go, then runs `m1-verify.sh`
   (`docs/RESUME.md` covers the USB-down window if the link drops again).
2. **Unreviewed in-flight code**: `series/` KMS patch (89 KiB) and shims are
   uncommitted and unaudited against the shared tree; `-fsyntax-only`/`-c`
   vs 3.10.108 headers claimed by authors, not yet verified here.
3. **GEM/job-slot driver gap**: no panfrost GEM/submission driver written;
   the critical path per `gpu-panfrost.md` R1 (fence→resv→syncobj→sched).
4. **Memattr divergence** (`0x8D884D4F48` kbase vs `0x888D88` panfrost, R4)
   + non-coherent PTW cache maintenance (R5): need GPU-read test at M1
   before Mesa.
5. **VA-bits window** (R3): T720 `MMU_FEATURES` VA/PA widths unread until M0
   runs on silicon; IOVA window sizing pending.
6. **Render-only product gap** (R7): panfrost gives headless EGL; on-screen
   GL needs the KMS/DECON gap (`series/0001-…-kms.patch` is the start).
7. **Slice collision**: ~30 concurrent slices under `code/gpu31/`; shared
   tables (OPP ladder, clock/regulator names) have single owners per
   `gpu31-dt.md` §4 cross-slice notes — duplicates must be rejected at
   integration.

## 6. Next actions

1. Main gives explicit go → M1Verify flashes M1 boot img → runs
   `m1-verify.sh` + `run-m1.sh` (card0/controlD64, dmesg, X). No reboot
   before then.
2. Land in-flight slices after review: `iommu/` (MMU header), `series/`
   shims (re-verify `-c` vs tree, SHA256SUMS match), `wsi/tri-x11` device run.
3. M2: render-minor patch (Mode B) → `render-node-probe /dev/dri/renderD128`
   → H1/H2 decision → Mesa path.
4. M3 order: M1 → render minor → backport series → GEM/job-slot driver →
   Mesa build → glmark harness (`run-m3.sh`, THRESHOLDS.md).
