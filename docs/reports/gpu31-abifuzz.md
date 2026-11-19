# GPU31 abifuzz — ioctl ABI fuzzer/validator findings

Date: 2026-11-19. Slice: G31_AbiFuzz. Target: v5.2 ioctl structs in
`code/gpu31/drmcore` + driver handler contract.

Tool: `code/gpu31/abifuzz/` (host-only, never in any kernel build).
Run: `cd code/gpu31/abifuzz && make check`.
Reference: upstream v5.2 `drivers/gpu/drm/panfrost/panfrost_drv.c`,
`panfrost_job.c`, `panfrost_gem.c` at tag `v5.2` (`14817897`), fetched
2026-11-19 via raw.githubusercontent.com.

Steady-state output (verbatim, `./abifuzz` exit 1 = findings present):

```
[layout] struct sizes / offsets / ioctl encodings
[table] 6 rows, order, flags
[directed] flag/pad/size/offset edges
FINDING F1: CREATE_BO pad 0x12345678: v5.2 rejects, port accepts
FINDING F2: GET_BO_OFFSET pad 0x7: v5.2 ignores, port rejects
FINDING F3: SUBMIT jc=8 (aligned): v5.2+gem accept, jobs slice rejects (inverted (jc&7)==0 test, job.c:445)
FINDING F3b: SUBMIT jc=4 (misaligned): jobs slice accepts, v5.2 accepts, gem shim rejects — slices disagree
FINDING F3c: SUBMIT jc=1: jobs slice accepts unaligned uaddr, dereferenced as u32 array (job.c:483-504)
FINDING F3d: SUBMIT jc=8 req=FS: jobs slice still rejects (jc inversion masks the happy path)
[fuzz] 20000 randomised arg buffers per ioctl
FINDING F2-fuzz: 20000/20000 GET_BO_OFFSET iters: port rejects nonzero pad (v5.2 ignores; e.g. 0x6eff9a42)
[wait] timeout-shape contract (static)
[contract] out_sync + compat-ioctl (static)
FINDING F5: SUBMIT out_sync: port validates nothing (bogus handle fails late at sync_replace); v5.2 returns -ENODEV up front (drm_syncobj_find)
FINDING F6: FOPS compat: no .compat_ioctl on either macro; 32-bit userspace ioctls fail with ENOTTY (exynos wires drm_compat_ioctl under CONFIG_COMPAT)
abifuzz: 9 FINDING(S), 0 FAIL(S)
```

`./compat_smoke` (fops/poll/flag smoke): `compat_smoke: all checks passed`
(exit 0). The FOPS byte-identity behind finding F6 is asserted there,
not in the listing above.

## PASS ledger (what the fuzzer proves healthy)

- All six `_310` struct mirrors byte-identical to the G31_Gem UAPI
  mirror (sizes 40/16/24/16/16/16, all field offsets; the mirror is
  byte-identical to upstream v5.2 per `gpu31-target.md` H1).
- All six ioctl numbers (0x00–0x05), direction bits, embedded size
  fields, and `_310` macro encodings match the UAPI macros.
- Table: 6 rows in order with the v5.2 flag contract (all
  `DRM_RENDER_ALLOW`, only SUBMIT additionally `DRM_AUTH`).
- GET_PARAM pad/param, CREATE_BO size/flags, MMAP_BO flags, WAIT_BO pad,
  SUBMIT requirements bit-mask (modulo F4 below) and count caps behave
  identically across all three validators.
- WAIT_BO absolute-timeout shape (fixed 2026-11-19, G31_Gem):
  `ktime_get_ts` + `nsecs_to_jiffies` conversion, raw wait error
  propagates (`-ETIMEDOUT`/`-EBUSY`), matching v5.2.

## Findings (owner-routed, none fixed in this slice)

- **F1 — CREATE_BO `pad` unchecked (G31_Gem).** v5.2
  (`panfrost_drv.c:panfrost_ioctl_create_bo`):
  `if (!args->size || args->flags || args->pad) return -EINVAL;`.
  Port (`gem/panfrost7580_ioctl.c:panfrost_ioctl_create_bo`) checks
  `flags` + `size` only; nonzero `pad` is accepted. Fix: add
  `|| args->pad` (one token). Class B note: Mesa always zeroes the
  struct, so stock userspace is unaffected; the hole only matters for
  hostile/untested callers.
- **F2 — GET_BO_OFFSET `pad` over-strict (G31_Gem).** v5.2
  (`panfrost_ioctl_get_bo_offset`) performs NO pad check. Port rejects
  nonzero `pad` with `-EINVAL`. Fuzz confirms 20000/20000 randomised
  buffers diverge on this single field. Fix: drop the pad check (or
  keep it deliberately and document the deviation — either way one
  line in the gem slice).
- **F3 — SUBMIT `jc` alignment test inverted in jobs slice (G31_Jobs,
  P0 — every aligned SUBMIT fails).** Gem shim
  (`gem/panfrost7580_ioctl.c:237`): `if (!args->jc || (args->jc & 7u))
  return -EINVAL;` — correct 8-alignment harden. Jobs core
  (`jobs/panfrost7580_job.c:445`):
  `if ((args->jc & 7u) == 0u || args->jc == 0u) return -EINVAL;` — the
  `== 0u` is inverted: rejects every 8-aligned address (the only kind
  the gem shim forwards), accepts 1..7. Net effect once wired: 100% of
  gem-accepted SUBMITs fail in the jobs core. Fix: `== 0u` → `!= 0u`.
  F3b/F3c document the accepted-misaligned tail (u32-array dereference
  at `job.c:483-504` reads a userspace pointer as kernel array — see
  also F7). F3d notes the happy path (`jc=8 req=FS`) is masked by F3.
- **F4 — SUBMIT `requirements` bit 10 divergence (G31_Gem vs G31_Jobs).**
  v5.2 (`panfrost_job.c:panfrost_ioctl_submit`):
  `if (args->requirements && args->requirements != PANFROST_JD_REQ_FS)`
  — only bit 0 legal. Gem shim: `& ~PANFROST_JD_REQ_FS` — same. Jobs
  core: `& ~(FS | ONLY_COMPUTE)` — additionally accepts bit 10
  (`P7580_JD_REQ_ONLY_COMPUTE`, a kernel-internal routing value per
  `jm_hw.h:113-114`, hidden behind `#if 0` upstream). Fix: either slice
  converges in one line; recommendation is the gem mask (v5.2 parity),
  with the jobs slice keeping ONLY_COMPUTE strictly internal.
- **F5 — SUBMIT `out_sync` never validated up front (G31_Gem/G31_Jobs).**
  v5.2 resolves `out_sync` first (`drm_syncobj_find`, `-ENODEV` on a
  bogus handle). Port validates nothing; a bogus handle fails late at
  `sync_replace` time after the job is already pushed. Fix: find-early
  in the gem shim (matches v5.2 order).
- **F6 — no `.compat_ioctl` on either FOPS macro (G31_DrmCore).**
  `DEFINE_DRM_GEM_FOPS` and `DEFINE_DRM_GEM_FOPS_COMPAT` in
  `drmcore/drm_compat_310.h:79-102` are byte-identical; neither wires
  `drm_compat_ioctl`, so 32-bit compat tasks get `-ENOTTY` on all six
  ioctls (exynos wires it under `CONFIG_COMPAT`; `autoconf.h:76` has it
  set). Fix: give `_COMPAT` a `.compat_ioctl = drm_compat_ioctl` entry
  under `#ifdef CONFIG_COMPAT` (one hunk, drmcore slice).
- **F7 — SUBMIT user pointers cross the shim/core boundary raw
  (G31_Gem/G31_Jobs, hardening note, no live bug today).** Current gem
  shim stages `in_syncs`/`bo_handles` with `copy_from_user` onto stack
  arrays and passes kernel pointers down (`ioctl.c:250-272`); the jobs
  core dereferences whatever it is handed (`job.c:482-523`). Safe as
  long as every caller goes through the shim, but the core's prototype
  (`const struct p7580_submit *`) cannot distinguish a staged kernel
  pointer from a raw userspace `__u64`. Recommendation: annotate or
  split the type at the boundary (e.g. `p7580_submit_kern` with
  `uint32_t *` fields) so a future direct caller cannot hand the core
  a userspace address.

## Non-findings (deliberate deviations, documented)

- SUBMIT count caps (16 in-syncs / 64 BOs): absent upstream (upstream
  trusts the counts and over-reads on violation); both port validators
  enforce them before any copy. Intentional hardening, kept.
- SUBMIT `jc` 8-alignment (gem shim): absent upstream (any nonzero
  accepted; alignment faults surface from the MMU). Intentional
  hardening, kept — the finding is only the jobs slice's inversion.
- GET_PARAM/MMAP_BO/WAIT_BO `pad` rejections: match v5.2 exactly.

## Method and coverage

- `abifuzz.c` models three validators per ioctl (upstream v5.2 / gem
  shim / jobs core) as pure-C predicates line-annotated to the kernel
  bodies they restate, then compares them over directed edge probes
  (zero/one/0xFF.../MAX/MAX+1 per field) plus 20000 deterministic
  randomised buffers per ioctl (LCG, fixed seed `0x12345678`).
- Layout/encoding/table checks are exact (`==`, not pattern matches).
- `compat_smoke.c` proves the compat macros expand and the flag/poll
  wiring behaves under a stub 3.10 environment (real gate values
  `DRM_AUTH=0x1`, `DRM_UNLOCKED=0x10`).
- Kernel-only behaviour (actual fence waits, handle lookups,
  `copy_from_user` faults) cannot execute on the host; those contracts
  are recorded as static findings (F5–F7) with file:line references.
- Incidental: while wiring the harness I found `gem/panfrost_drm.h` had
  lost its closing `*/` during a peer edit (whole header parsed as a
  comment). I repaired it locally to build; G31_Gem3 has since folded
  the repair into their commit — no action remaining.

## Files

| File | Purpose |
|---|---|
| `code/gpu31/abifuzz/abifuzz.c` | fuzzer/validator (directed + fuzz + layout/table checks) |
| `code/gpu31/abifuzz/compat_smoke.c` | compat macro smoke test |
| `code/gpu31/abifuzz/stub/drm_stub.c` | linkable stub defs (smoke only) |
| `code/gpu31/abifuzz/stub/drm.h` | legacy `drm.h` include-path shim |
| `code/gpu31/abifuzz/stub/drm/drm.h` | uapi `drm.h` subset (real `DRM_IOW(nr,type)` shape) |
| `code/gpu31/abifuzz/stub/drm/drmP.h` | 3.10 `drmP.h` subset (real gate values, fops, ioctl-desc rows) |
| `code/gpu31/abifuzz/stub/drm/drm_gem_cma_helper.h` | CMA mmap prototype |
| `code/gpu31/abifuzz/stub/linux/{types,io,delay,jiffies,fs}.h` | kernel-env stubs |
| `code/gpu31/abifuzz/Makefile` | `make check` (build + run both binaries) |
| `code/gpu31/abifuzz/README.md` | usage and exit-code contract |
