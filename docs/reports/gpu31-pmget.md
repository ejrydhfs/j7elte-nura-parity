# gpu31-pmget — `p7580_drv_pm_get` normalisation audit (1-is-powered fix)

Date: 2026-11-21. Owner: G31_PmGetAudit.
Ticket: `p7580_drv_pm_get()` returned the raw `pm_runtime_get_sync()`
value (1 = already active, 0 = resumed, negative errno = dark), while
`p7580_drv_timedout_job` tested it with truthiness (`if (...)`), so a
perfectly powered device (raw 1) took the dark early-return: recovery
never ran and the PTEWALK probe never executed (silicon: TIMEOUT line
printed, no PTEWALK). Same latent hazard in `run_job` and the ioctl
override. Fix: keep the helper normalised (0 powered / negative dark)
AND test `< 0` at every call site (belt and suspenders), plus a host
test proving raw 1 takes the success path.

## 1. Contract (after fix)

- `p7580_drv_pm_get(drv)` returns **0 = powered** (raw 0 resumed-now
  and raw 1 already-active both normalised to 0), **negative errno =
  dark** (failed get balanced internally with `pm_runtime_put_noidle`;
  caller must touch no register and must NOT put). `p7580_drv_pm_put`
  is only ever called for a ref this file took via a 0-returning get.
- **Every** `p7580_drv_pm_get` caller tests `< 0`, never truthiness, so
  even a future helper change that passes 1 through cannot take the
  dark path by accident.
- `p7580_drv_teardown_hold` returns 1 = held / 0 = dark (unchanged
  boolean shape; internally `== 0` on the normalised helper, so raw 1
  counts as held).
- `p7580_job_pm_get_void` (jobs-slice dump hook) was already normalised
  (`< 0` → put_noidle + errno, else 0); dump tests `!pm`, unaffected.

## 2. Call-site audit (all sites in `code/gpu31/driver/0001-*.patch`)

| # | Site (file / function) | Test (before → after) | Verdict |
|---|---|---|---|
| 1 | submit.c `p7580_drv_pm_get` def | `return ret` → `return 0` after `ret >= 0` (was already normalised; added explicit normalisation comment) | powered for raw 0 AND 1; dark only on `< 0`. Correct |
| 2 | submit.c `p7580_drv_run_job` (async wq) | `if (p7580_drv_pm_get(drv))` → `if (... < 0)` | **was the bug class**: raw 1 would fail the job (`ERR_PTR(-EIO)`) on a powered block. Fixed; success path prints `pm_get HELD` and programs HEAD/START |
| 3 | submit.c `p7580_drv_timedout_job` (timeout wq) | `if (p7580_drv_pm_get(drv))` → `if (... < 0)` | **the reported bug**: raw 1 signalled the fence and returned before `p7580_drv_ptewalk_pin` + `p7580_job_timedout` recover. Fixed; powered path runs PTEWALK then recover then put |
| 4 | submit.c ioctl override (`p7580_drv_submit` wrapper) | `if (ret)` → `if (ret < 0)` | latent hazard (normalised helper made it harmless; now explicit). Fixed |
| 5 | submit.c `p7580_drv_teardown_hold` def | `return p7580_drv_pm_get(drv) == 0` (unchanged) | already correct under normalisation (1→0→held). No change needed; documented |
| 6 | submit.c `p7580_drv_job_pm_arm` | `(void)p7580_drv_pm_get(drv)` return ignored (ref held for the async leg; dark leg covered by run_job/timeout re-gets) | unaffected by truthiness; no change |
| 7 | submit.c `postclose` / `file_fini` / `remove` / `p7580_gem_gpu_unmap` | via `teardown_hold` boolean (see #5) | correct; no change |
| 8 | submit.c `p7580_drv_unmap_bo` | takes `invalidate` int from caller (CPU table writes always, TLB invalidate MMIO only if held) | no get of its own; no change |
| 9 | main.c `p7580_drv_probe` | raw `pm_runtime_get_sync(&pdev->dev)` + `if (ret < 0)` | already `< 0`; raw 1 holds the probe power ref correctly. No change |
| 10 | main.c `p7580_drv_remove` error unwinds (`err_pm`) | `mark_last_busy + put_autosuspend` balance the probe hold | no get-result test; no change |
| 11 | power.c `pf7580_power_probe` | raw `pm_runtime_get_sync(dev)` + `if (ret < 0)` | already `< 0`; raw 1 keeps the block powered correctly. No change |
| 12 | power.c `pf7580_power_remove` | raw `pm_runtime_get_sync(dev)` ignored, balanced by `put_noidle` (force-resume for sync suspend) | intentional; no change |
| 13 | jobs `p7580_job_dump_show` via `p7580_job_pm_get_void` | `pm = jd->pm_get(...)`, MMIO guarded by `if (!pm)` | callee already normalised; shadows the name `pm` but tests `!pm` == 0-means-powered, correct. No change |
| 14 | gem / ioctl / mmu / devfreq / suspend slices | no `pm_runtime_*` / `pm_get` calls (verified by grep; GEM coherency paths are CPU-side `dma_sync` only) | no change |

GEM coherency paths: `bind` (`dma_map`), SUBMIT (`sync_for_device`),
IRQ pre-signal invalidate, `free_job` (`job_done_sync` =
`sync_for_cpu`), `unbind` — none touch `pm_runtime` or `pm_get`; power
is provided by the submit.c legs audited above. No changes there.

## 3. Diff

`code/gpu31/driver/0001-panfrost7580-driver-sources.patch` (submit.c
hunks only; no line-count changes — replacements are comment/test
swaps inside existing hunks, `@@ -0,0 +1,N @@` counts untouched):

- `run_job`: `if (p7580_drv_pm_get(drv) < 0)` + dark-comment notes the
  1-normalisation.
- `timedout_job`: `if (p7580_drv_pm_get(drv) < 0)` (dark early-return
  now errno-only, so PTEWALK + recover always run when powered).
- Helper contract comment: documents 0-powered/negative-dark and the
  `< 0`-everywhere rule; `return 0` gains a normalisation comment.
- Dump-wrapper comment: notes 1→0 normalisation.
- Ioctl override: `if (ret < 0)`.

`code/gpu31/driver/submit_fix_test.c`: T2 updated to the normalised
helper shape (failed get balances itself, no put owed) + new T8
(`model_pm_get` raw 1→0 / 0→0 / −EIO→−EIO with put_noidle accounting,
and `model_worker_dark` proving helper 0 skips the dark early-return
while −EIO takes it). `make check` passes with `-Wall -Wextra
-std=gnu89`, zero warnings.

## 4. Checks

- `make -C code/gpu31/driver check` → `submit_fix_test: all checks
  passed`, no warnings.
- `git apply --check` on the full 19-file patch: clean.
- GNU `patch -p1 --dry-run` on a pristine-tree copy: clean (patch
  applies; hunk counts unchanged since edits are in-place).
- `-fsyntax-only` of the patched `panfrost7580_submit.c`: owned by the
  integrator (no kernel tree on this host); the edits are
  expression-level (`< 0`, comments) with no include/signature impact.
- Silicon: PTEWALK verdict still with Main (flash + egltest); this fix
  removes the blocker that suppressed the probe (powered timeouts no
  longer early-return dark).
