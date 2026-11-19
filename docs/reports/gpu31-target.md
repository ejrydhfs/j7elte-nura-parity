# GPU31 target selection — panfrost backport base (T720 / 3.10.108)

Slice: `target` (PanfrostTarget). Status: **decision locked — v5.2**.
Upstream base: `refs/tags/v5.2` (`14817897`).
Prior art: `docs/scratch/panfrost-backport-inventory.md` (dependency-by-dependency
shopping list with file:line evidence), `docs/reports/gpu-panfrost.md`,
`docs/reports/gpu-drm.md` (§M3), `docs/reports/gpu-userspace.md`.

No kernel tree edits, no device access, no flash/reboot in this slice
(host-side decision only).

## 1. Decision

**Port panfrost at Linux v5.2, UAPI byte-identical** (6 ioctls, `GPU_PROD_ID`-only
`GET_PARAM`, zeroed `CREATE_BO` flags). Internals adapted to 3.10 (CMA-backed GEM,
M0-tested LPAE encoder, vendor pm-domains notifier, legacy DRM attach) per §4.

Rejected: **v5.3** (adds unstable perfcnt ioctls + `panfrost_perfcnt.c/.h`, zero T720
benefit) and **v5.10-ish** (adds `MADVISE` + shrinker + 40 `GET_PARAM`s + heap-BO
handling — large 3.10 GEM rework for a Mesa BO-cache optimisation the Mesa side can
fall back without). Newer generations (≥6.x: `SET_LABEL_BO`, `JM_CTX_CREATE/DESTROY`,
`SYNC_BO`, `QUERY_BO_INFO`, expanded `SUBMIT` with `jm_ctx_handle`) only widen the
shim surface further and are excluded for the same reason.

H1 contract (confirmed to G31_Mesa, unchanged): `0x00 SUBMIT`, `0x01 WAIT_BO`,
`0x02 CREATE_BO`, `0x03 MMAP_BO`, `0x04 GET_PARAM`, `0x05 GET_BO_OFFSET`;
`GET_PARAM` = `DRM_PANFROST_PARAM_GPU_PROD_ID` only; `SUBMIT` ends at `requirements`
(no `jm_ctx_handle`/`pad`); `CREATE_BO.flags` reserved-zero (reject nonzero).
Modern-Mesa probes of extra params/`MADVISE` are handled Mesa-side (G31_Mesa 0005
compat patch), never by expanding the kernel UAPI.

## 2. Candidate comparison

| Candidate | Stable UAPI (ioctls / params / BO flags) | T720 complete? | Extra driver files vs v5.2 | 3.10 dependency delta vs v5.2 base | Mesa pairing | Verdict |
|---|---|---|---|---|---|---|
| **v5.2 (CHOSEN)** | 6 ioctls (`0x00–0x05`); 1 param (`GPU_PROD_ID`); no BO flags | **Yes** — T720-capable generation; match table carries `arm,mali-t720`; shmem-GEM stabilised in this window | — (base) | Base cost only (§3) | Mesa 19.3–20.x era pairs natively (T720 declared stable on Mesa 20.0); shipped Alpine `panfrost_dri.so` via G31_Mesa compat patch | **PROCEED** |
| v5.3 | 6 stable + 2 unstable (`PERFCNT_ENABLE/DUMP`, gated on `unstable_ioctls`) | Yes, nothing T720-relevant added | +`panfrost_perfcnt.c/.h` | +perfcnt session/reset/fault-recovery coupling (cf. mainline `Fix races between perfcnt and reset`) | Same userspace as v5.2; perfcnt is debug-only | **REJECT** — cost, no function |
| v5.10-ish | 9 ioctls (+`PERFCNT_*`, `MADVISE`); 40 params; `PANFROST_BO_NOEXEC/HEAP` | Yes, nothing T720-relevant added | +`panfrost_perfcnt.c/.h`, +`panfrost_gem_shrinker.c` | +shrinker/LRU + `madvise` retained-bit plumbing against 3.10 CMA GEM (which has no shrinker hooks); +expanded `GET_PARAM` decode; +heap-BO handling | Newer Mesa prefers `MADVISE` for its BO cache but runs without it (cache still works, pages just unreclaimable — acceptable on a 2 GB render-only node) | **REJECT** — largest GEM rework, optimisation only |
| ≥6.x / master | 14 ioctls (+`SET_LABEL`, `JM_CTX_CREATE/DESTROY`, `SYNC_BO`, `QUERY_BO_INFO`); 40+ params; 3 BO flags; wider `SUBMIT` | Yes, nothing T720-relevant added | scheduler split into multi-file, further coupling | +context/priority mgmt, +sync/timeline surface 3.10 cannot express | Requires JM-context-aware Mesa | **REJECT** — out of scope |

Why not older than v5.2: panfrost merged in 5.1; T720 support + shmem-GEM
stabilised in the 5.2 window — going earlier buys nothing on this tree.

## 3. Dependency cost table (3.10.108 — all candidates share this base)

Present/absent with file:line evidence: see inventory §2. Strategy summary:

| # | Dependency (v5.2 use) | In 3.10? | Disposition (all candidates) | Extra cost if NOT v5.2 |
|---|---|---|---|---|
| D1 | `drm_sched` / `drm_gpu_scheduler` (`panfrost_job.c` backend) | ABSENT | Thin single-queue FIFO shim behind the `run/dependency` callback shape first; full `sched_main.c` port only if needed (G31_Sched scope) | ≥6.x: multi-file scheduler split — shim must track more surface |
| D2+D3 | `dma-fence` + `dma-resv` (`panfrost_job.h` fences, BO resv locking) | ABSENT | Minimal in-tree fence/resv shim (~1–1.5k LOC), seeded from `drivers/staging/android/sync.c` semantics (G31 fence/resv/syncobj/sched series, `patches/drm-backport/0001-0005`) | None extra (all gens need it) |
| D4 | `drm_syncobj` (`SUBMIT` in/out sync) | ABSENT | Binary-syncobj subset only | None extra (pre-JM gens identical) |
| D5 | `drm_gem_shmem_helper` | ABSENT — `drm_gem_cma_helper` PRESENT (272 lines) | **Reuse CMA, do NOT port shmem** (G31_DrmCore: `DEFINE_DRM_GEM_FOPS` equiv + CMA mmap glue, legacy `DRM_IOCTL_DEF` rows for the 6 ioctls) | v5.10+: `MADVISE`/shrinker assumes shmem LRU — would force GEM rework; v5.2 avoids it |
| D6 | `io-pgtable` + `ARM_MALI_LPAE` | ABSENT (generic); format knowledge PRESENT in kbase | M0-tested 4-level LPAE encoder instead (G31_Iommu owns `drivers/gpu/drm/panfrost/panfrost7580_mmu.c/.h`; COHERENCY_NONE + explicit `dma_sync`, no sysmmu) | None (format identical across gens) |
| D7 | LPAE substrate (arm64 tables) | PRESENT (arch-native) | Reuse as-is | — |
| D8+D9 | GEM prime + mmap-offset helpers | PRESENT (old shape) | Adapt in place | — |
| D10 | `devfreq` + OPP | PRESENT (old API) | Port kbase Exynos7580 freq/ASV tables; do NOT port `panfrost_devfreq.c` verbatim | — |
| D11+D12 | reset-control / genpd `device_link` | ABSENT — vendor `pm_domains.c:413/431` notifier PRESENT | **Delete, do not port** (G31_DrmCore scope confirmed: no genpd/device-link) | — |
| D13 | DT (`arm,mali-t720`, clocks, interrupts) | PRESENT, cutover at land time | M0 already resolves `aclk_g3d`/`aclk_g3d_400` + `vdd_g3d`; compatible append is one line at cutover | — |
| D14 | DRM core (`drm_dev_alloc` split, `readl_poll_timeout`, atomic) | ABSENT | 3.10 `drm_platform_init` shape + local poll helper; render-only ⇒ no atomic | — |
| D15 | kbase ownership | kbase OWNS block | Cutover `CONFIG_MALI_MIDGARD=n` at SUBMIT bring-up, not before | — |

Critical path: D2 → D3 → D4 → D1 (fence → resv → syncobj → sched). The driver
itself (~3k LOC) is the easy part; newer UAPI generations add only kernel-side
shim surface on top.

## 4. Exact file list (v5.2 `drivers/gpu/drm/panfrost/` + UAPI)

LOC via `wc -l` measured 2026-11-19 against `refs/tags/v5.2` (`features`/`issues` absent from the inventory §1 table, measured here).

| File | LOC | Role in port |
|---|---|---|
| `panfrost_drv.c` | 475 | 6-ioctl table (`DRM_RENDER_ALLOW`; `SUBMIT` also `DRM_AUTH`), gem/shmem fops, probe/remove → adapt to `drm_platform_init` + legacy `DRM_IOCTL_DEF` (G31_DrmCore) |
| `panfrost_device.c` / `.h` | 257 / 125 | probe, clk/regulator/pm-runtime attach, IRQ install → drop genpd/device-link/reset paths |
| `panfrost_gpu.c` / `panfrost_gpu.h` | 367 / 19 | GPU_ID decode, SOFT+HARD reset fallback, L2→shader→tiler PWRON/READY |
| `panfrost_mmu.c` / `panfrost_mmu.h` | 394 / 17 | REFERENCE ONLY — replaced by `panfrost7580_mmu.c/.h` (G31_Iommu) |
| `panfrost_job.c` / `panfrost_job.h` | 564 / 51 | `drm_gpu_scheduler` backend, `dma_fence` job fences, syncobj in/out, JS slot submit → against D1–D4 shims |
| `panfrost_gem.c` / `panfrost_gem.h` | 96 / 30 | REFERENCE ONLY — reimplemented on `drm_gem_cma_helper` (G31_DrmCore) |
| `panfrost_devfreq.c` / `.h` | 221 / 14 | REFERENCE ONLY — kbase ASV tables → OPP/devfreq instead |
| `panfrost_regs.h` | 298 | Register map (matches kbase bit-for-bit) — keep |
| `panfrost_features.h` | 309 | GPU feature/quirks table — keep |
| `panfrost_issues.h` | 176 | HW workaround list — keep |
| `Kconfig` / `Makefile` / `TODO` | 16 / 12 / 27 | Build glue |
| `include/uapi/drm/panfrost_drm.h` | 142 | **Keep byte-identical** (the H1 contract) |
| **Total** | **~3555** (inventory §1 said 3070 — that figure predates measuring `features`/`issues`, 485 lines) | |

Per-slice file ownership (no overlaps): `gpu31-target.md` (this slice);
`code/gpu31/drmcore/` (G31_DrmCore: platform attach, 6 ioctl rows, CMA fops/mmap
glue, poll helper); `drivers/gpu/drm/panfrost/panfrost7580_mmu.c/.h` (G31_Iommu);
`0005` compat patch (G31_Mesa).

## 5. Milestone order (unchanged from inventory §6)

B0 tree+config → B1 DRM-core adaptation (6 ioctls, `GET_PARAM`/`GET_BO_OFFSET`
first) → B2 GEM-on-CMA → B3 MMU (M1 fault-injection/GPU-read test) → B4
fence/resv/syncobj → B5 scheduler + `SUBMIT` (**cutover `CONFIG_MALI_MIDGARD=n`
here**) → B6 DVFS + hardening + stock-`panfrost_dri.so` headless render test.
