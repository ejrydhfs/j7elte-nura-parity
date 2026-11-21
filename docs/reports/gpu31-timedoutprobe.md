# gpu31-timedoutprobe — TMD-ENTER / TMD-PREWALK / TMD-WALKER pins on the timedout path

Date: 2026-11-21. Owner: G31_TimedoutProbe. Phone: on int27
(int27 with the pmget `< 0` fix still prints `TIMEOUT slot=0 ...` but
never `PTEWALK`, although the PTEWALK strings are present in
Image-int27 and the walker runs immediately before
`p7580_job_timedout()` in `p7580_drv_timedout_job`).

Ticket: three unconditional (never budget-limited) `pr_err` pins that
locate exactly where the fault-time path dies:

1. `p7580_job: TMD-ENTER sched_job=%lu job=%lu drv=%lu pmraw=%d` —
   wrapper entry, AFTER the `drv` NULL-fallback (`df->drv`) and the
   `if (!drv) return`, immediately after the raw
   `p7580_drv_pm_get(drv)` return is captured in `int pmraw`. Raw
   value (0/1 powered, negative errno dark), pointers as unsigned
   long. Cheap: one `pr_err`, no dereference beyond the already-taken
   `drv` branch.
2. `p7580_job: TMD-PREWALK job=%lu drv=%lu` — immediately before the
   `p7580_drv_ptewalk_pin(drv, job)` call, i.e. after the
   `if (pmraw < 0)` dark early-return. Present + absent PTEWALK after
   it distinguishes "dark early-return" from "walker entered but quit
   silently" from "walker line lost".
3. `p7580_job: TMD-WALKER drv=%lu job=%lu` — first statement of
   `p7580_drv_ptewalk_pin`, before the `!drv || !job` and
   `!root_dma` guards. Present + absent PTEWALK narrows the silence
   to the four silent `return`s (`!drv||!job`, `!root_dma`,
   `jc & ~PAGE_MASK`, and a mid-walk UNMAPPED/NOT-LEAF/NO-OWNER/CROSS
   line that should itself have printed — so its absence points at
   the first three).

Behaviour is otherwise unchanged: `pmraw` feeds the same
`if (pmraw < 0)` dark early-return (fence signal + return) that
`if (p7580_drv_pm_get(drv) < 0)` did; the powered path still runs
PREWALK → walker → `p7580_job_timedout` → `pm_put`.

## Diff

`code/gpu31/driver/0001-panfrost7580-driver-sources.patch`
(submit.c new-file section only; header `+1,1478` → `+1,1487`, +9
net lines):

- Wrapper `p7580_drv_timedout_job`: `void *f;` block gains
  `int pmraw;`; `if (p7580_drv_pm_get(drv) < 0)` becomes
  `pmraw = p7580_drv_pm_get(drv);` + TMD-ENTER print + unchanged
  `if (pmraw < 0)` dark early-return.
- TMD-PREWALK print inserted between the ptewalk comment and the
  `p7580_drv_ptewalk_pin(drv, job);` call.
- Walker `p7580_drv_ptewalk_pin`: TMD-WALKER print as the first
  statement after declarations, before all guards.

No other files touched; no test-file changes (host `submit_fix_test`
models the `< 0` rule, unaffected by prints).

## Checks

- Hunk accounting: every new-file `@@ -0,0 +1,N @@` header matches
  its `+`-line count (submit 1487/1487, all other 18 files unchanged).
- `git apply --check` on 0001: clean. GNU `patch -p1 --dry-run` on
  a pristine-tree copy for 0001: clean, all 19 files.
  (0002 was NOT re-checked here: it applies in the kernel tree
  against `drivers/gpu/drm/Kconfig+Makefile`, absent on this host;
  baseline `git stash` shows the same failure without this ticket's
  edits, so it is pre-existing and unrelated.)
- Applied-tree grep: TMD-ENTER at submit.c:489, TMD-PREWALK at :501,
  TMD-WALKER at :342 (walker entry).
- Brace/paren balance of both edited functions in the applied tree:
  balanced (ptewalk 13/13, timedout 2/2). Full `-fsyntax-only`
  against the real 3.10.108 headers is owned by the integrator (no
  kernel tree on this host); the edits are two `pr_err` + one
  `int pmraw` local with no include/signature impact.
- `make -C code/gpu31/driver check`: `submit_fix_test: all checks
  passed`, zero warnings.
- Silicon verdict with Main (flash + egltest): read the three TMD
  lines per the decision table below.

## Decision table (one egltest, int28 image)

- `TMD-ENTER pmraw=<0` ⇒ dark early-return taken: PM/power story
  (resume failed or `drv->dev` NULL via `-ENODEV`), walker correctly
  skipped. Fix power, not the walker.
- `TMD-ENTER pmraw>=0`, no `TMD-PREWALK` ⇒ impossible in this code
  (no branch between them); indicates the built Image predates this
  ticket — rebuild/flash check.
- `TMD-PREWALK` present, no `TMD-WALKER` ⇒ call compiled out or
  image stale (call is unconditional between the two prints).
- `TMD-WALKER` present, no `PTEWALK` ⇒ one of the three remaining
  silent guards fired (`!drv||!job` impossible here since ENTER
  printed non-NULL, so `!root_dma` or `jc & ~PAGE_MASK`); next step
  is logging those two values.
- `PTEWALK ...` present ⇒ probe landed; follow `gpu31-ptewalk.md` §3
  (MATCH = GPU-side stale fetch, MISMATCH = PT/IOVA lifetime bug).

Hand to G31_Integrator27 for the int28 build (copy of build-int27.sh
+ 6o TMD gate: TMD-ENTER/TMD-PREWALK/TMD-WALKER strings in tree +
object + Image, call inside timedout path, header count 1487).
