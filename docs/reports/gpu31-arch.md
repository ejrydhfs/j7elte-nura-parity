# GPU31 architecture — Mali-T720 panfrost port on j7elte (Exynos 7580 / 3.10.108)

Date: 2026-11-19. Phone: SM-J700M (j7elte), Nura Alpine aarch64,
kernel `3.10.108 #35 SMP PREEMPT` (gcc 16.2.0). `/dev/mali0` present
(`Mali-T72x MP2 r1p0 0x0720`, DDK r15p0 / UK 10.6); `/dev/dri` absent
(running kernel predates M1). Target: upstream panfrost **v5.2**
(`14817897`), UAPI byte-identical, adapted to 3.10 internals.

This document is the integrator's map. It states the DT contract, the
render-minor design, the GEM/MMU/job data path, power/DVFS, fault
recovery, and the exact userspace contract, with the residual gap list.
Detail lives in the per-slice reports (see §11); nothing here replaces
them. No tree was edited to write this; no flash/reboot performed.

Invariants (hold for every slice): shared kernel tree is never edited
(slices ship patch files; only G31_Integrator applies/builds); stock
`dt.img` is never rebuilt (S-Boot hangs on rebuilt DTBs — probe stock
as-is with in-driver fallbacks); phone stays bootable with fbdev X
working; GPS/FM share the device (no reboot without Main's go; flash
only by G31_M1Verify on Main's explicit go); big files stay off `/tmp`
tmpfs and off the 94%-full rootfs.

## 0. System picture

```
 Userspace                      Kernel 3.10.108                    HW
 ─────────                      ──────────────                    ──
 Mesa panfrost_dri.so (19.3-20.x era + G31_Mesa 0005 compat)
   │  6 ioctls, render fd only
   ▼
 /dev/dri/renderD128 ──► panfrost7580 DRM driver ──┬── GEM/CMA+SHMEM (BO, IOVA, resv, mmap)
 /dev/dri/card0 (modeset client holds master)     ├── GPU MMU (own 4-level LPAE, AS0..N)
 /dev/fb0 + X fbdev (vendor DECON fbdev, intact)  ├── JM job slots JS0..2 (SUBMIT/IRQ/reset)
 kbase /dev/mali0 (UK 10.6, until cutover)        ├── power (pd-g3d, aclk_g3d, vdd_g3d)
 tri-x11 / glmark2-es2 / kbase-stress            └── devfreq simple_ondemand + TMU max-lock
         ▲                                              ▲
         │ EGL-X11/GLX today (llvmpipe); GBM after M2  │ genpd + dev_pm_ops, no second notifier
```

Display and render are decoupled: vendor DECON fbdev keeps `/dev/fb0`
and X 720×1280 through the whole bring-up (Mode A → Mode B). The KMS
swap path (Mode C) is a separate, deferred decision — never mixed with
the render-node path in one image (review §9: alternatives, not complements).

Milestone order: B0 tree+config → B1 DRM-core (6 ioctls,
GET_PARAM/GET_BO_OFFSET first) → B2 GEM-on-CMA → B3 MMU (fault-inject /
GPU-read test) → B4 fence/resv/syncobj → B5 scheduler + SUBMIT
(**cutover `CONFIG_MALI_MIDGARD=n` here**) → B6 DVFS + hardening + stock
`panfrost_dri.so` headless render test. M1 = `/dev/dri` card0+controlD64
with fbdev intact; M2 = renderD128 + H1/H2 decision; M3 = Mesa GL on HW.

## 1. DT binding (zero-change rule)

Source: decompiled stock DTBs (`stock_19009536.dts` et al, all three
identical on GPU content) + read-only kbase/exynos7580 tree sources.
Full tables: `gpu31-dt.md` §§1–2; reliance plan §4.

Stock GPU surface:

- `mali@0x11400000` (20 KiB G3D SFR): `compatible = "arm,mali"`
  (single string), `reg <0 0x11400000 0 0x5000>`, 3 `interrupts`
  (GIC SPI 282/283/281 → Linux 314/315/313, order = JOB/MMU/GPU ABI;
  `interrupt-names` present but ignored — kbase indexes by position),
  6 `clocks` (`fin_pll`, `fout_g3d_pll`, `aclk_g3d`, `mout_g3d`,
  `dout_aclk_g3d`, `aclk_g3d_400` — these strings are the clock ABI),
  `samsung,power-domain = <&pd_g3d>`.
- `pd-g3d@10464060` (`samsung,exynos-pd`, `pd-option 1`, phandle `0x60`).
- PMIC `s2mpu03`: `g3d_en = 1`, `BUCK4 regulator-name "vdd_g3d"`
  (the regulator ABI — global-name lookup, no DT phandle), 600–1300 mV,
  `always-on + boot-on`. No `mali-supply` phandle anywhere.
- `bus_mif` / `bus_int` devfreq buses exist (MIF QoS tiers driven from C,
  not DT phandles); `bus_isp` is camera-only. Three TMU nodes exist with
  **no** GPU `cooling-device` / thermal-zone linkage. No GPU carveout in
  `reserved-memory`; no `sysmmu`/`iommus` for G3D (correct — the T720 has
  its own MMU); no `operating-points-v2` anywhere (CPU `operating-points`
  are legacy and unrelated); no second `compatible`, no `memory-region`,
  no `clock-rates`, no `status` (absent = enabled).

What new code may rely on vs fall back to (driver probes stock as-is):

| Need | Stock? | Rule |
|---|---|---|
| OF match | `arm,mali` only | Match `arm,mali` as Exynos7580/T720 alias (SoC-gated) plus `arm,mali-t720`; never require the new string; last resort probe by `reg` address |
| `reg` 0x11400000/0x5000 | yes | Hard requirement; `-ENOENT` if absent |
| 3 IRQs JOB/MMU/GPU | yes, in order | Index mapping 0/1/2; require count == 3 |
| 6 clocks | yes, by name | `devm_clk_get(dev, <stock name>)` × 6; optional 7th (`bus`) tolerated as NULL; never spin on `-EPROBE_DEFER` |
| `vdd_g3d` rail | rail yes, phandle no | `devm_regulator_get_optional(dev,"mali")` then legacy `regulator_get(NULL,"vdd_g3d")`; tolerate `-ENODEV` (rail is always-on/boot-on; keep C voltage ladder, skip voltage writes) |
| OPP table | **no** | Static C ladder (160/266/350/440/534/668/734/800 MHz; 900/900/1000/1000/1000/1100/1100/1100 mV; MIF 416/416/559/667/728/825/825/825 kHz); parse `operating-points-v2` opportunistically, fall back to static; cap top at 800 MHz |
| Power domain | yes | Generic `samsung,power-domain` binding; kbase-style raw PMU `LOCAL_PWR_CFG` sequence as backup |
| Bus QoS (`bus_mif`/`bus_int`) | nodes yes, phandle no | C-side pm_qos pattern; on lookup failure clamp to 416 MHz MIF floor |
| Thermal/TMU | nodes yes, GPU map no | C throttle ladder (800→160 MHz) via TMU notifier; on registration failure pin ≤534 MHz + log |
| IOMMU / `memory-region` | none (correct) | GPU-internal MMU only; `-ENODEV` on any `iommus` lookup is the expected path |

Cross-slice ownership: G31_Power owns the `vdd_g3d`/BUCK4 window
([600000, 1300000]); G31_Devfreq owns the static OPP/governor tables
(single copy — power/panfrost consume, never duplicate); G31_Kms/G31_Gem
must not add `memory-region`/`iommus` phandles.

## 2. Render-minor design (Mode B)

Modes: **A** (M1, built) = vendor DECON fbdev keeps panel, card0 +
controlD64 via exynos+G2D, no display swap. **B** (M2, this design) =
add `renderD128` next to Mode A for unprivileged GPU clients (kbase
shim now, panfrost later), decoupled from modeset mastership.
**C** (later) = full KMS DECON/DSIM + panel + DT compatible flip +
vendor-fbdev off-switches together. Patch: `code/gpu31/kms/
0001-drm-3.10-render-node-mode-b.patch` (+37/−2, dry-run clean,
per-file `-fsyntax-only` + `-c` PASS). Reference: `DESIGN.md` (PASS).

```
 drm_get_platform_dev (drm_platform.c)
   ├─ drm_fill_in_dev + drm_get_minor(PRIMARY)  → card0
   ├─ drm_get_minor(CONTROL)                    → controlD64
   ├─ drm_get_minor(RENDER, &dev->render)  ★ NEW (before ->load())
   └─ ->load() (exynos; G2D-only on stock DT → load OK, 0 connectors)
         primary node survives; fbdev early-returns on num_connector==0
 unwind: err_g4 render → err_g3 primary → err_g2 control
 release: drm_put_dev / drm_unplug_dev NULL-guarded render put/unplug
```

Why this shape: 3.10 has `dev->primary/control` but no `dev->render`
holder — the new `struct drm_minor *render` keeps platform/stub unwind
symmetric with no exynos driver change (sole-writer handshake with
G31_DrmCore: their headers are new-files-only). Render alloc before
`->load()` means a failed display bring-up unwinds the node too.
pci/usb paths intentionally untouched (M1/M2 bind only the exynos
platform device).

Permission gating (3.10 has no `DRM_RENDER_ALLOW` bit):

- `drm_open_helper`: render opens are **always authenticated**
  (era render semantics — authenticated, never master). card0 keeps the
  AUTH_MAGIC dance.
- `drm_ioctl` dispatcher: any `DRM_MASTER`-flagged ioctl on a render
  node → `-EACCES`. REQUIRED: mastership is per-minor (first opener of
  *each* node becomes that minor's master), so the plain
  `DRM_MASTER && !is_master` test would pass a render opener through to
  SETCRTC/SETPLANE/PAGE_FLIP. The existing `!CONTROL_ALLOW` gate is
  untouched. Buffer ioctls (GEM CREATE/MAP, PRIME HANDLE_TO_FD/
  FD_TO_HANDLE: `DRM_AUTH|DRM_UNLOCKED`) stay render-reachable; read-only
  KMS (`DRM_CONTROL_ALLOW|DRM_UNLOCKED`) stays callable (harmless, era
  semantics). No dispatcher ABI change for card0/controlD64.
- No fbdev conflict by construction: no DT edit, no compatible matched,
  no DECON/DSIM MMIO/clocks/domains/IRQs, no CRTC/encoder/connector
  claim. Minor source is the existing carve-out (`drm_minor_get_id`:
  render 128–383 → first node minor 128 → `renderD128` via wired
  `renderD%d` sysfs naming).

M1Verify asserts (post-flash, not run here): `card0 controlD64
renderD128` present; unprivileged PRIME HANDLE_TO_FD + GEM map from the
render fd while a modeset client holds card0 master → success, no master
stolen; SETCRTC/PAGE_FLIP from render fd → `-EACCES`; X 720×1280 + fbdev
unchanged; `render-node-probe renderD128` → H1/H2 gates the Mesa path.

## 3. GEM / MMU / job flow

### 3.1 GEM (BO model on CMA)

Reference: v5.2 `panfrost_gem.{c,h}` (96/30 LOC, shmem-based). This tree
has no shmem helper — port reuses the present 272-line
`drm_gem_cma_helper` + a private page-array shmem path
(`gpu31-target.md` D5). Files: `code/gpu31/gem/` (`panfrost_drm.h`
byte-identical UAPI mirror; `panfrost7580_gem.{c,h}`;
`panfrost7580_ioctl.{c,h}`; `panfrost7580_devfreq.{c,h}` GEM-internal
hook sketch — dedup against the standalone DVFS slice at land time).

```
 CREATE_BO(size) ──► p7580_gem_create (CMA: dma_alloc_attrs WC │ SHMEM: shmem file + pages)
                      ├─ embed drm_gem_object + embedded dma_resv (0002 shim layout)
                      ├─ drm_mm IOVA alloc: window [32 MiB, 4 GiB) (T720 legacy 32-bit; VA 33-bit from MMU_FEATURES)
                      └─ handle create; args->offset = bo->iova (nonzero guaranteed — HW treats 0 as unmapped)
 MMAP_BO(handle) ──► fake offset = map_list.hash.key << PAGE_SHIFT (same encoding as cma_dumb_map_offset)
                      └─ fault handler maps WC (P7580_GEM_WC) / cachable per cache_attr
 GET_BO_OFFSET ──► bo->iova (same value as CREATE returned on this fd)
 WAIT_BO ──► p7580_gem_wait → resv lock → snapshot fences → wait (get under lock, sleep after unlock)
 PRIME ──► export: dma_buf ops + get_sg_table (pinned pages, honours BIDIRECTIONAL)
           import: wrap exporter sg (attachment-backed, never copy) + driver_private stash;
                   vmap takes a GEM ref (vunmap drops it) — no unpinned-pointer UAF
```

BO backing selector (`P7580_BO_CMA/SHMEM`) is driver-private — never on
the UAPI wire (v5.2 has no BO flags; `CREATE_BO.flags` nonzero →
`-EINVAL`). Free path: CMA `dma_free_attrs` / SHMEM page release +
`drm_prime_gem_destroy` for imports + `dma_resv` release; teardown
contract: unmap all before free (no child-walk in free; GEM layer carries
the mapped-count `WARN_ON`). Cache attribute: `DMA_ATTR_WRITE_COMBINE →
MT_NORMAL_NC` (arm64); coherent_walk = false + explicit `dma_sync` after
every table/data share (COHERENCY_NONE, §3.2). `NO_KERNEL_MAPPING /
NON_CONSISTENT / FORCE_CONTIGUOUS` are ignored by arm64 dma-mapping — do
not copy the exynos no-map-yet-dereference pattern for CPU-touched memory.

### 3.2 MMU (GPU-own MMU, no sysmmu)

Decision (locked): operate panfrost with its own GPU MMU + DMA-API
direct mappings (`dma_map_page`, identity `dma == phys`); **no IOMMU API,
no exynos-sysmmu adapter, no DT `iommus`, no `CONFIG_*IOMMU*` change.**
Five grounds: the T720 MMU *is* its IOMMU (AS_TRANSTAB/MEMATTR/TRANSCFG +
UPDATE programming, kbase-proven on this silicon); no GPU sysmmu exists
in DT (four `iommu-domain_*` buses cover fimc-is/decon/scaler/mfc only);
sysmmu-v6 format (1 MiB/64 KiB + 4 KiB) ≠ Mali PTW (4×512×9-bit 4 KiB,
ATE=1/PTE=3, AF/RD/WR/SH/XN); the kbase chain already implements exactly
this (per-context 4-level tables, `dma_sync_single_for_device` under
COHERENCY_NONE, `TRANSTAB = pgd & mask | TABLE | READ_INNER`, legacy
`transcfg = 0`); the adapter alternative needs a fictitious DT node +
double translation + IOVMM churn for zero benefit.
Files: `code/gpu31/iommu/panfrost7580_mmu.{c,h}` + build-wiring patch;
host model + AS helpers: `code/gpu31/mmu/{pf7580_as.h,README}`,
`code/userspace/gpu/panfrost/{mali_hw.h,mali_mmu.h}` (185 host checks PASS).

```
 map:    4-level walk → dma_sync_single_for_device after every parent/leaf write
         (may sleep allocating ≤3 tables; 4K align, memattr_idx ≤ 7)
 invalidate: LOCK(iova,size) → FLUSH_PT (mapping) / FLUSH_MEM (full TLB: LOCK(0,~0))
             → wait !AS_ACTIVE (100 000 bound, mali_kbase_defs.h:1682) → UNLOCK
             (mirrors kbase_mmu_hw_do_operation; -ETIMEDOUT → GPU-reset escalation;
              no 9630 double-UNLOCK — absent from T720 r1p0 errata)
 program AS: TRANSTAB_LO/HI ← pgd | TABLE(3) | READ_INNER(4) (never SHARE_OUTER)
             MEMATTR ← 0x8D884D4F48 (kbase LPAE; upstream 0x888D88 kept defined for A/B)
             TRANSCFG ← 0, wmb, COMMAND ← UPDATE gated on !ACTIVE before+after
 fault:  mask MMU IRQs → per-set AS read FAULTADDR_HI/LO + FAULTSTATUS → MMU_INT_CLEAR
         → re-enable; low-byte class, bits 9:8 access (ATOMIC→UNKNOWN on LPAE),
         bit 10 decoder/slave, bits 31:16 source ID; 33-bit window [0, 2^33), L0[0] only
```

Coherency rules the M2/M3 driver MUST follow: `system_coherency` is
`COHERENCY_NONE` (kbase forces it; DT override never fires — no
`system-coherency` property; live `0x1f`); `dma_map == maintenance`
(global arm64 ops are noncoherent swiotlb); `DMA_ATTR_SKIP_CPU_SYNC`
must NOT be set on the GEM path (honoured only in `__swiotlb_map_sg_attrs`);
fault on identity break (`dma_map_page != page_to_phys`, kbase `WARN_ON`s
it): fail closed (`-EIO` + unmap + free), fallback is sg-walked PTE fills,
never a sysmmu. R4 (memattr `0x8D884D4F48` vs `0x888D88`): default kbase
table on this silicon; validate with a GPU-read test at M1 before Mesa
(wrong policy shows as garbage-under-load, not faults — the dangerous kind).
R5 (PTW visibility): every table write synced; audit any fast path
against `kbase_mmu_sync_pgd`. Coexistence: both drivers program
`AS_TRANSTAB` — never enable alongside kbase (cutover
`CONFIG_MALI_MIDGARD=n` at M2/M3 only).

### 3.3 Job submission (JM slots, scheduler, IRQ, recovery)

Hardware: 3 slots (`BASE_JM_MAX_NR_SLOTS 3`, live `js_present 0x7`),
slot base `0x1800 + n*0x80` (kbase JOB_SLOT_REG == mainline JS_BASE).
Routing (v5.2 `panfrost_job_get_slot`): `PANFROST_JD_REQ_FS` → JS0
(fragment); everything else → JS1 (vertex/tiler); JS2 wired/idle (no
compute-only bit in v5.2 UAPI). T720 quirks (from `base_hwfeatures/
issues_model_t72x`): **no** FLUSH_REDUCTION / JOBCHAIN_DISAMBIGUATION /
PROTECTED_MODE in `JS_CONFIG`; `START_MMU` always (10649).
Descriptor: 32-B `p7580_job_desc` (exception_status, first_incomplete,
fault_pointer, size/type/barrier, job_index, dep 1/2, next_job) —
MMU-validated before HW sees it. `CONFIG_NEXT = as_nr | START/END
CLEAN_INVALIDATE | START_MMU | THREAD_PRI(8)`; affinity = full
`shader_present` (live 2 cores); submit = HEAD_NEXT/AFFINITY_NEXT/
CONFIG_NEXT then `COMMAND_NEXT = START`; `COMMAND_NEXT != 0` guard
(`WARN_ON` + recovery, never double-START).
Files: `code/gpu31/jobs/` (hw header, job header, ~640-line slot
manager, 60-check host test PASS, kernel `-fsyntax-only` + `-c` PASS).

```
 SUBMIT ioctl ──► validate (jc 8-aligned/nonzero; requirements ⊆ {FS};
 │                           in_sync ≤ 16, bo_handles ≤ 64; slot present)
 ├─ explicit deps: in_syncs handles → syncobj find_fence (DrmCore 0003 mgr, owned here, freed in free)
 ├─ BO reservation: handles → resv (GEM 0002 locking discipline; this file never touches resv lists)
 ├─ sched_submit(slot) → per-slot entity FIFO (0004 shim) → run_job:
 │                       per-slot ordered fence (context/slot, monotonic seqno), program slot
 └─ out_sync: render_done_fence → syncobj replace_fence
 JOB_INT IRQ (no sleeps/mutexes): read STAT; per set bit CLEAR; ERR → COMMAND_NEXT=NOP (freeze, dump later);
                                  DONE → pull in_flight under hw_lock, dma_fence_signal(done_fence)
 timeout (5 s, pinned to shim pump until entity->sched lands): spurious guard (signalled→bail)
 recovery (process ctx only, reset_lock serialised): HARD_STOP faulting (+ any COMMAND_NEXT!=0) slots
   → poll COMMAND_NEXT==0 ≤3 s → GPU SOFT_RESET → poll RESET_COMPLETED ≤3 s → HARD_RESET fallback
   → re-arm INT CLEAR + MASK present DONE+ERR → MMU re-programs AS_TRANSTAB (Mmu/Iommu-owned call)
   → slots IDLE (no sched stop/resubmit/karma — shim has none; no per-context guilty tracking;
                  Mesa re-submits the batch)
```

Single in-fence contract end to end (matches Mesa backend). No
ERR_PTR-drop (run returns fence or ERR_PTR; OOM path frees nothing
unlinked). `JS_STATUS` table gates recovery (`status >= 0x40` = fault;
INTERRUPTED/STOPPED/TERMINATED benign vs CONFIG/POWER/READ/WRITE/
AFFINITY/BUS/INSTR/DATA/TILE_RANGE/ADDRESS_RANGE/OUT_OF_MEMORY/UNKNOWN).
Review adaptations (§6): this code compiles against either version of
the 0001–0004 shims — the REQUIRED fixes belong to G31_Sched (fence
init/lock/wait; resv lock + pin-across-wait; sched entity/dependency/
callback/ERR/timeout/push_job), stated explicitly in `gpu31-jobs.md` §6.

## 4. Power / DVFS

Full sequence: `gpu31-power.md` §2 (cloned from the stock kbase backend
that already drives this SoC). Files: `code/gpu31/power/
panfrost7580_power.{c,h}`. Companion DVFS: `code/gpu31/devfreq/
pf7580_devfreq.{c,h}` (standalone slice; OPP ladder + governor profile +
thermal max-lock; GEM-internal `panfrost7580_devfreq` sketch dedups at
land time) + firmware/perfcnt probe init `code/gpu31/firmware/
panfrost7580_fw.{c,h}` (T720 = Midgard → **no firmware blob**: zero
`request_firmware` in all kbase, no GPU file in `/vendor/firmware`,
Valhall `mali_csffw.bin` postdates this GPU — loader is a documenting
stub returning "not required").

Probe: `devm_kzalloc` + `mutex_init` → `clk_get` × 6 in kbase order
(`fin_pll`, `fout_g3d_pll`, `aclk_g3d`, `mout_g3d`, `dout_aclk_g3d`,
`aclk_g3d_400`; abort + unwind on any failure; no `sclk_g3d/gate_pclk`
— they do not exist on 7580) → `regulator_get(NULL,"vdd_g3d")` (NULL
device mandatory — no `vdd_g3d-supply` phandle) + `regulator_enable`
→ read `clk_get_rate(aclk_g3d)`, map to OPP voltage, `set_voltage` to
match the CMU's left-behind rate → `pm_runtime` autosuspend 50 ms +
`pm_runtime_enable` → `pm_runtime_get_sync` (genpd powers pd-g3d via
`BUS_NOTIFY_BIND_DRIVER` + `samsung,power-domain`; never touch PMU
CONFIGURATION) → `put_autosuspend`.

Runtime resume (power on): `clk_prepare_enable(aclk_g3d)` → SOFT_RESET,
poll `RESET_COMPLETED` ≤100 ms → HARD_RESET fallback → clear/unmask →
PWRON **L2 → shader → tiler**, polling each `READY` ≤100 ms (fail =
gate clock + errno). Runtime suspend (power off): cores off reverse
(**tiler → shader → L2**, `PWRON=0`, poll `READY==0`; a stuck stage
only warns — the clock must still be gated) → `clk_disable_unprepare`
→ genpd drops pd-g3d. Never `regulator_disable(vdd_g3d)` on the runtime
path (s2mpu03 disregards it under `g3d_en=1`; would only corrupt the
refcount — disable only in `remove`). System sleep: suspend = runtime
path if not already suspended; resume = unconditional full power-on
(state lost). No kbase-style `pm_notifier` copy (genpd + dev_pm_ops own
it; a second notifier would double-drive), no `pm_suspend_ignore_children`.

Re-rate/voltage (`GPU_SET_CLK_VOL` order is law): rate refuses `-EAGAIN`
while gated; park `mout_g3d → aclk_g3d_400` → `clk_set_rate(fout_g3d_pll)`
→ parent back → unpark. **Up: voltage then clock; down: clock then
voltage** (534→668 and 668→800 cross the 1.00→1.10 V corner). ABB/DVS
absent on 7580 (`GPU_DYNAMIC_ABB=0`, `GPU_DVS=0`) — not ported.
LDO10 trap (F1): while BUCK4/LDO10 tracking is armed, LDO10's enable is
slaved to BUCK4 — any `regulator_get("vdd_ldo10")` from GPU code risks
rail glitches mid-job. No LDO10 handle exists in this patch by design.
Other traps: register touch under a runtime-PM ref only (F2); never copy
the infinite `gpu_power_set_reg` spin (F5 — all polls bounded); exactly
the six stock clocks (F6); dev-based regulator lookup fails (F7);
bring-up pins 266 MHz, gate 734/800 on v1 silicon + cooling (F9).

DVFS: 8-row static ladder (Hz/uV: 160M/900k, 266M/900k, 350M/1000k,
440M/1000k, 534M/1000k, 668M/1100k, 734M/1100k, 800M/1100k — all inside
BUCK4 600–1300 mV), registered via 3.10 `opp_add` for `simple_ondemand`
(upthreshold 90 / downdifferential 5 / weight 100, NULL governor data
legal, polling 100 ms); boot OPP 350 MHz pinned first, then polling on;
`target()` = kbase voltage/clock order + `devfreq_recommended_opp`;
suspend/resume + sysfs `max_freq` max-lock; busy/idle accumulators fed
from submit/retire fences (`get_dev_status` drains them). Bus QoS via
C-side pm_qos MIF tiers, not DT. Perfcnt probe: `PRFCNT_CONFIG ← OFF`,
BASE_LO/HI + JM/SHADER/TILER/MMU_L2 masks, `CONFIG ← MANUAL`,
`-EBUSY` while `GPU_STATUS.PRFCNT_ACTIVE`; sampling + IRQ 16 + 8186
quirk belong to the perfcnt ioctl slice.

Live proof the whole chain tracks (powermetric, kbase tree, #35):
idle parks 266 MHz / rail 812 500 uV `disabled` / GPU `suspended`;
30 s soft-job burst (362 100 jobs) → `active`, 266 MHz @ 57–58%,
rail 812 500 `enabled`, TIS accrues only on step 266; forced-668 →
rail 1 100 000 `enabled`, restore parks again. Correct governor hold
(57% < 78/85 up-thresholds + 420 MHz@95 highspeed gate). Battery/fuel-
gauge current is charger-state-machine noise at Full charge — the
rail + `runtime_active_time` + `time_in_state` trio is the proxy, not
battery current. Suspend slice (kbase tree): dummies in `kbase_pm_ops`
wired to real bodies via thin wrappers (`.suspend/.freeze/.poweroff`,
`.resume/.thaw/.restore` + unchanged runtime legs), one-file patch,
`--dry-run` + `-fsyntax-only` clean, fbdev/DECON untouched, notifier kept
as idempotent redundant second call. Thermal slice: graded TMU path was
dead (`GPU_THROTTLING=7` fell in `default: "wrong event"`) + clamp/OPP
slot mismatch — two confined platform patches + verify script; GPU
`zone2/temp` empty read + 160 MHz `freq_tab` vs live 668-table residual
left to integrator follow-up.

## 5. Fault recovery (panfrost + kbase reference)

```
 slot: IDLE → SUBMITTED (in_flight set, COMMAND_NEXT=START)
            → DONE (IRQ, fence signalled)
            → FAULT (ERR bit, STATUS latched) → RECOVERING (reset_lock) → IDLE
 kbase ref (soft jobs never reach slots): JS soft-stop 300 ms / hard-stop 350 ms /
         GPU soft-reset 400 ms; soft-event-wait cancel 3000 ms; Samsung fence timer
         1000 ms (logs "Release fence is not signalled", does NOT cancel).
```

Panfrost IRQ does the minimum (STAT read, CLEAR, NOP-freeze on ERR,
signal on DONE); recovery runs in process context (timeout workqueue or
submit-error path): stop faulting + busy slots → drain poll → SOFT_RESET
→ HARD_RESET fallback → re-arm INT → MMU re-programs AS_TRANSTAB →
fence accounting (faulting job keeps `done_fence` unsignalled until the
retry policy decides; put in free, never leaked on ERR). No cross-slot
dependency replay (single address space, FIFO per slot, one in-fence).
Suspend drains via `p7580_job_is_idle`; close recycles contexts; a stuck
context is contained by canary-on-fresh-fd + recycle (deadline on every
wait — never bare `poll(-1)`; distinguish *rejected* `JOB_INVALID/
CONFIG_FAULT` → propagate vs *stuck* → suspect + canary).
Stress proof (kbase winsys, #35, full battery PASS): 200 soft jobs avg
0.08 ms, 300-iter BO churn zero leak, fence-timeout contained with live
canary, soft-event cancel exactly 3.01 s, bad-stride/dep triple 3/3
contained + canary OK, no JS hard-stop / GPU reset / oops in dmesg.
DRM-path ready-to-run plan (same five shapes with render-fd/syncobj/GEM
nouns + hang-recovery assertion) is specified in `gpu31-stress.md` §4 —
it runs once renderD128 exists. Boot fallback (recovery slice): initramfs
boot-count on CACHE (`mmcblk0p21`) + cached known-good `boot.img` (≤32 MiB):
`boot_count>3` + valid copy → `dd` restore + reboot, else boot normally;
fail-open everywhere (no cache/copy/restore-failure ever subtracts a
boot); pre-initramfs panics stay TWRP-only; watchdog deliberately NOT
armed on bring-up (would reset slow-but-healthy boots — revisit after M1
timings); `boot-guard.sh` (X 720×1280 + milestone device alive) resets
the counter from userspace.

## 6. Userspace contract (exact)

UAPI is panfrost v5.2 **byte-identical** (`code/gpu31/gem/panfrost_drm.h`
mirrors `refs/tags/v5.2 include/uapi/drm/panfrost_drm.h`; DrmCore
`panfrost_ioctls_310.h` numbers verified against the same source).
Rejected: v5.3 (+ unstable perfcnt ioctls), v5.10-ish (+ MADVISE /
shrinker / 40 params / heap BOs — GEM rework for a Mesa BO-cache
optimisation that falls back cleanly), ≥6.x (+ SET_LABEL / JM_CTX_* /
SYNC_BO / QUERY_BO_INFO / wider SUBMIT — needs JM-aware Mesa).

```
 #  ioctl            nr    dir   struct (ioctl arg)              key rule
 0  SUBMIT           0x00  W     drm_panfrost_submit             jc 8-aligned/nonzero;
                                  {jc,in_syncs,in_sync_count,     requirements ⊆ {FS};
                                   out_sync,bo_handles,           in_sync ≤ 16, bo_handles ≤ 64
                                   bo_handle_count,requirements}  (ends here — no jm_ctx_handle/pad)
 1  WAIT_BO          0x01  W     {handle,pad,timeout_ns}         timeout_ns ABSOLUTE (CLOCK_MONOTONIC);
                                                                      pad nonzero → -EINVAL
 2  CREATE_BO        0x02  WR    {size,flags→handle,pad,offset}   flags reserved-zero (nonzero → -EINVAL);
                                                                      size 0 → -EINVAL; offset nonzero guaranteed
 3  MMAP_BO          0x03  WR    {handle,flags→offset}           no flags; returns fake mmap offset
                                                                      (map_list.hash.key << PAGE_SHIFT);
                                                                      real mmap on the DRM fd at that offset
 4  GET_PARAM        0x04  WR    {param,pad→value}               ONLY DRM_PANFROST_PARAM_GPU_PROD_ID (0);
                                                                      anything else → -EINVAL; value = 0x720
 5  GET_BO_OFFSET    0x05  WR    {handle,pad→offset}             same iova as CREATE on this fd
 ioctl numbers: DRM_COMMAND_BASE + 0x00..0x05. SUBMIT needs DRM_AUTH;
        all six need DRM_UNLOCKED. No DRM_RENDER_ALLOW bit on 3.10
        (DrmCore maps it → DRM_UNLOCKED; render reachability comes from
         the Mode-B minor gating in §2, not from a flag).
```

Open/auth: render fd (`renderD128`) always authenticated, never master
(§2); card0 keeps the AUTH_MAGIC dance. Buffer ioctls are render-
reachable; `DRM_MASTER` ioctls fail `-EACCES` on the render node.
Modern-Mesa probes of extra params / `MADVISE` are handled Mesa-side
(G31_Mesa 0005 compat — never by widening the kernel UAPI).
Mesa pairing: 19.3–20.x era natively (T720 declared stable on 20.0);
shipped Alpine `panfrost_dri.so` via the compat patch.

H2 reference (kbase winsys, current production): `/dev/mali0`, UK 10.6,
size-selected ioctl (`_IOC_SIZE` only; `CALL_MAX_SIZE` 568), handshake
(CHECK_VERSION → SET_FLAGS(0) → GET_VERSION/DDK `K:r15p0-00rel0(GPL)`)
→ 8 live calls (CHECK_VERSION, SET_FLAGS, GET_VERSION, GET_CONTEXT_ID,
GPU_PROPS_REG_DUMP, MEM_ALLOC/MEM_FREE, STREAM_CREATE, JOB_SUBMIT) +
24-B `read()` events; 64-B atoms (`stride == 64` enforced;
`core_req@44`, SAME_VA cookie `(idx+64)<<12`, tracking page `0x3000`,
flags `0x300f` live). Full validated table: `gpu31-kbase-abi.md` §§2–3.
Classifier: `render-node-probe` v2 (exit 0 H1 incl. hybrid, 3 H2-only,
4 NEITHER, 2 NO_DEVICE, 1 ERROR; `--json` with `h1/h2/t720/params/kbase/
verdict/exit`) gates which backend the harness runs.

Test gates (harness implements, THRESHOLDS.md-conformant): G1 renderer
string (software/hardware/hardware-unknown/unknown; `--expect` bar);
G2 glmark2-es2 score floor (`max(baseline×0.5,5)`, WARN-only, heavy four
must exceed 1 FPS); G3 2D surface (parseable XWD, geometry match,
distinct ≥ 100); G4 GPU sanity (software ⇒ idle `util=0`/TIS flat;
hardware ⇒ activity or WARN-with-explanation); G5 verdict artifact
(`verdict.json` schema `glmark-harness/1` + `verdict.txt` + `RESULT`).

## 7. Gap list (honest, owned)

Blocking (no HW GL until closed):

1. **GEM/job-slot driver assembly.** GEM core (`p7580_gem_*`,
   six ioctl bodies — SUBMIT still validate-only), MMU helper,
   job manager, power/devfreq, DrmCore rows, Mode-B minor, and the
   0001–0005 shim series are written and separately compile-checked but
   not linked into one `drivers/gpu/drm/panfrost/` driver, not Image-
   linked, not booted. First-HW-submit recipe: AS0 program + one mapped
   page → soft-job jc in a mapped BO → SUBMIT no in-syncs → expect
   DONE + `done_fence` + HEAD/TAIL advance; then unmapped-jc
   `ADDRESS_RANGE_FAULT` → ERR + STATUS → `p7580_job_recover` → IDLE.
2. **Shim REQUIRED fixes (G31_Sched/G31_Gem).** dma-fence: add
   `dma_fence_init`, delete dangling `lock`/`default_work`/`status`,
   return `wait_for_completion_*` remainder. dma-resv: take
   `resv->lock` in every mutator + snapshot-refs-under-lock in
   test/wait (`dma_fence_get` under lock, wait after unlock).
   syncobj: stop `idr_remove`-while-iterating in manager fini.
   sched: separate membership list from job FIFO; gate pump on dep
   readiness; split dep vs completion callbacks (or document one-dep);
   free/requeue on ERR_PTR; thread `sched->timeout` through
   (`entity->sched`); implement or delete `push_job`. Until fixed: no
   SUBMIT test (workarounds in jobs slice §6 cover compile only).
3. **PRIME real-sharing (G31_DmaBuf — fixed in revision, needs
   Image+device proof).** 0001 WC-attrs adaptation + 0002
   attachment-backed import (no private copy) + pinned vmap + 0003
   coherency note are `git apply --check` clean with a 30-check host
   contract — still needs the M2 on-device PRIME fd → kbase UMM import
   smoke. Display-side `import_sg_table` copy semantics must never
   regress (zero-copy scanout breaks silently).
4. **M1 flash gate (G31_M1Verify, Main's go only).** Candidate
   (`Image` 18110712 B `8a7d0452…`, boot `cf25ed70…`, stock dt+ramdisk)
   is READY: expects card0+controlD64, zero connectors, fbdev X intact.
   Post-flash asserts: `m1-verify.sh` 7/7, `run-m1.sh`, dmesg exynos
   init, GPU-read + fault-injection (memattr A/B, PTW visibility),
   `/dev/dri` node presence.
5. **Mesa backend fixes (G31_Mesa).** `n_slots = 2 + wait` (currently
   short by one — uninitialised atom slots → atom-table collisions);
   r7p0 48/56 stride must become 64 + per-atom expansion (kernel
   rejects anything else with `-EINVAL`); single in-fence limit stays
   explicit end to end.

Non-blocking / follow-up (do not gate first render):

6. CMA 0 MB vs 32 MB: M1 needs no scanout (fb helpers early-return);
   the swap path's 32 MB reservation costs zero BOOT bytes (RAM only);
   all 7 config variants clear 32 MiB BOOT with ≥4.9 MB headroom
   (`check-matrix.sh` ALLPASS; GSC silently dropped without ARCH_EXYNOS5
   is caught, CMA-helper requests reverting is documented).
7. Thermal residuals: GPU `zone2/temp` empty read (TMU valid-bit path);
   clamp entries 800/734 vs live 5-level ASV table (fail closed today);
   `freq_tab[6]=160 MHz` in neither table; `CONFIG_DEVFREQ_THERMAL`
   cooling stays out of scope on the kbase tree.
8. KMS Mode C + DT single-`compatible` + `ARCH_EXYNOS`/IRQ-count/iovmm/
   clk_put/LVDS-vs-DSI build verifications (integrator re-checks; all
   fail loudly at compile except §12 F1/§11 F1 which are silent and
   hence REQUIRED-before-submit).
9. WSI GBM rerun once renderD128 exists (EGL-X11/GLX on llvmpipe proven:
   60 frames RENDER_OK + on-screen crop; GBM needs a real `drmGetVersion`
   answer from the node).
10. Suspend sleep-cycle trace on hardware + lost-context generation
    counter for Mesa (follow-up slice); `boot-guard`/`cache-snapshot`
    tuning (`FAIL_MAX=3`, `UI_TIMEOUT=300`) after M1 timings; CACHE ext4
    redundancy stays TWRP-covered.

## 8. Bring-up checklist (integrator / M1Verify)

```
 Integrator (serialised, scratch on external drive):
   git apply --check 0001..0005 (fence→resv→syncobj→sched)   # first
   + Mode-B render patch (5 files) + DrmCore headers (include-only)
   + GEM + MMU + jobs + power + devfreq + firmware (new files under drm/panfrost/)
   + thermal 0001/0002 (platform files) + suspend dev_pm_ops (kbase file)
   + D0 (= M1 6-symbol) fragment ONLY  — never with the swap-path fragment
   oldconfig → Image (KCFLAGS="-Wno-error") → mkboot (stock dt.img + ramdisk-new.gz)
   assert: dma_fence/sched/syncobj/resv + exynos_drm_platform_probe in vmlinux;
           strict -Werror stock warnings (aes-glue-ce, cpu.c, setup.c) unchanged
 M1Verify (after Main's go ONLY): flash → 5-min SSH wait → m1-verify.sh 7/7
   → GPU-read + fault-inject (§3.2) → thermal-verify.sh --inject → glmark harness
   → cache-snapshot.sh (seeds CACHE known-good) → install boot-guard
 Never: make in shared tree from a slice; flash without go; reboot for Anything Else;
        second compatible string; CONFIG_MALI_MIDGARD=n before SUBMIT bring-up.
```

## 9. Power numbers worth quoting

- BOOT headroom: stock Image 17 234 120 B → M1 18 094 328 B (+876 592 B);
  M1 boot 27 250 688 B (25.99 MiB), 6.30 MB headroom; maximal D2
  ~28.3 MB, 4.9 MB headroom. RAM: 1.1 GB free at idle; BO churn
  300 iters Δ +0 KiB; soft-job ~0.08 ms/job, ~12 070 jobs/s sustained.
- Rail: idle 812 500 uV `disabled`/suspended; load 812 500 `enabled`/
  active @ 57–58%; forced-668 1 100 000 `enabled`. Thresholds 78/85
  (top 99/100, bottom 10/20), highspeed 420 MHz @ 95.
- Thermal: CPU zones 44→65 °C under llvmpipe soak; GPU zone2 `curr_temp`
  5–6 °C (sensor not tracking — trips driven by stale/low reading until
  the valid-bit path is fixed); cdev `max_state=5`, 8 trips 76–115 °C.

## 10. File map (where everything lives)

| Area | Code | Report |
|---|---|---|
| Decision (v5.2, 6 ioctls, dep table D1–D15, file list, milestones) | — | gpu31-target.md |
| Day plan + invariants + evidence links | — | gpu31-master.md |
| Candidate build (series, config diff, hashes) | `code/gpu31/series/` + `code/kernel/build.sh` | gpu31-integration.md |
| DT binding + fallbacks | — | gpu31-dt.md |
| DRM-core compat (fops, 6 ioctl rows, poll helper) | `code/gpu31/drmcore/` | gpu31-drmcore.md |
| Render minor Mode B | `code/gpu31/kms/` | gpu31-kms.md |
| Probe classifier | `code/gpu31/probe/` (+ v1 `code/userspace/gpu/mesa-render/`) | gpu31-probe.md |
| GEM + 6 ioctl bodies + UAPI mirror | `code/gpu31/gem/` | (this doc §3.1; per-slice docs in code) |
| MMU helper + build wiring | `code/gpu31/iommu/` | gpu31-iommu.md |
| MMU host model + AS helpers | `code/gpu31/mmu/` + `code/userspace/gpu/panfrost/` | gpu31-mmu.md |
| Job slots + SUBMIT + IRQ + recovery | `code/gpu31/jobs/` | gpu31-jobs.md |
| Fence/resv/syncobj/sched shims | `patches/drm-backport/0001-0005` + `code/gpu31/series/` mirror | gpu31-review.md §§1–5 |
| Power/clock/reset/rail | `code/gpu31/power/` | gpu31-power.md |
| DVFS/OPP/governor | `code/gpu31/devfreq/` | (header-provenance + this doc §4) |
| Firmware skip + OPP/perfcnt probe | `code/gpu31/firmware/` | gpu31-firmware.md |
| Suspend (kbase dev_pm_ops) | `code/gpu31/suspend/` | gpu31-suspend.md |
| Thermal cdev + TMU table | `code/gpu31/thermal/` | gpu31-thermal.md |
| Powermetric (rail/TIS proxy) | `code/gpu31/powermetric/` | gpu31-powermetric.md |
| DMA-BUF/CMA/attrs/PRIME | `code/gpu31/dmabuf/` | gpu31-dmabuf.md |
| Config matrix (7 variants + proofs) | `code/gpu31/config/` | gpu31-config.md |
| kbase UK 10.6 ABI (H2 reference) | `code/userspace/gpu/kbase*/` | gpu31-kbase-abi.md |
| Stress + watchdog policy | `code/gpu31/stress/` | gpu31-stress.md |
| GL harness (G1–G5) + scenes | `code/gpu31/tests/` + `code/userspace/gpu/glmark-harness/` | gpu31-tests.md |
| Mesa sweep collector | `code/gpu31/mesatests/` | (sibling of tests) |
| WSI triangle demo | `code/gpu31/wsi/` | gpu31-wsi.md |
| M1 boot gate + baseline | `code/kernel/m1/` + `/tmp/kernel-boot.img` | gpu31-m1-boot.md |
| Packaging (soft-GL wins today) + series regen | `code/gpu31/package/` + `code/pmaports/.../j7elte-gpu-soft/` | gpu31-package.md |
| Boot-count fallback (CACHE) | `code/kernel/recovery/` | gpu31-recovery.md |
| All-patch review (verdicts per slice) | — | gpu31-review.md |

## 11. Related reading (priority order)

`gpu31-target.md` (decision) → `gpu31-dt.md` (binding) →
`gpu31-drmcore.md` + `gpu31-kms.md` + `gpu31-probe.md` (node path) →
`gpu31-iommu.md` + `gpu31-mmu.md` + `gpu31-jobs.md` (+ `gem/` sources)
(data path) → `gpu31-power.md` + `gpu31-thermal.md` +
`gpu31-powermetric.md` + `gpu31-suspend.md` (power) →
`gpu31-kbase-abi.md` (H2 contract) → `gpu31-review.md` (all findings) →
`gpu31-integration.md` + `gpu31-m1-boot.md` + `gpu31-config.md` (build
gates) → `gpu31-dmabuf.md` + `gpu31-firmware.md` + `gpu31-stress.md` +
`gpu31-tests.md` + `gpu31-wsi.md` + `gpu31-package.md` +
`gpu31-recovery.md` (integration evidence).
