# gpu31-integration49 — prof_arm now exists (reuse the driver's p7580 debugfs dir); 33/33 gates

Date: 2026-11-22. Owner: G31_Integrator49. Phone: on int48; host-side
build only. No flash per ticket.

Consumes **0a2e44e** (this ticket's overlay fix; int48's overlay was
`952ff750`, now `7de60d03…`). Driver 0001 unchanged at **04db08d**
(`e7474670…`); repo HEAD `0a2e44e`; 10 patches.

## 0. What was wrong (runtime, from the flashed int48)

`p7580_prof_debugfs_init()` called `debugfs_create_dir("p7580", NULL)`.
The job slice already created that directory in
`p7580_job_dev_init()` and stores it in `jd->dbg_dir`, so the second
create returned `-EEXIST` **as an ERR_PTR**; `IS_ERR_OR_NULL()` therefore
reported true and the function returned before `debugfs_create_file()`
ever ran. On the flashed image Main saw
`can't create /sys/kernel/debug/p7580/prof_arm: Permission denied`, and
`ls` showed `chain_mode dump ev_arm hang_inject iova_arm
job_timeout_ms serialize_fs trace_arm` — everything except `prof_arm`.

My 6ai gates did not catch it because they checked that the call and the
probe wiring existed, not that the root directory was already owned by
someone else: a source-level gate cannot see an `-EEXIST` at runtime.
That is the gate gap this ticket closes (6aj).

## 1. Fix (overlay only)

`p7580_prof_debugfs_init()` now:

```c
dir = (struct dentry *)drv->jd.dbg_dir;      /* created by job slice */
if (!dir) {
	pr_info("p7580perf: prof_arm unavailable (no p7580 debugfs dir)\n");
	return;
}
f = debugfs_create_file("prof_arm", 0600, dir, drv, &p7580_prof_arm_fops);
if (IS_ERR_OR_NULL(f)) {
	pr_err("p7580perf: prof_arm knob failed (%ld)\n",
	       IS_ERR(f) ? PTR_ERR(f) : -ENODEV);
	return;
}
pr_info("p7580perf: prof_arm ready\n");
```

No second root is created; every path logs; success logs
`p7580perf: prof_arm ready`, so dmesg itself proves the knob exists.
`p7580_job_dev_init()` runs earlier in probe (source order gated), so
`jd->dbg_dir` is populated before this runs. Overlay hunk counts were
re-audited after the edit (the submit.c hunk is `+1777,124`).

## 2. Build

Script `code/gpu31/series/build-int49.sh` (3919 lines; sha256
`7e89616514979766bab9808f4272afd3b848c3b6f597f483b13b6bc96f56a8e0`),
a copy of int48 with int49 names, the pinned overlay sha
(`7de60d03…`), step **6aj** and a rewritten header.

Fresh scratch tree (birth 23:12:52, tree mtime 23:17:04), `-j4` Image,
RC 0, 0 `error:`, **499 warnings** (ambient), 5397-line log ending
`OBJCOPY arch/arm64/boot/Image`; Image mtime 23:17:04 gated. Patch
inputs were verified clean before scratch creation; inventory: 10
patches, all hunk counts exact.

- Gate stdout `…/logs/gpu31-int49-gates.log` (1368 lines): **33/33 PASS,
  0 FAIL**.
- Evidence `…/logs/gpu31-int49-evidence.log` (1411 lines); provenance
  `repo HEAD=0a2e44e driver-0001 sha256=e7474670…`.
- Host: driver T1–T11, chainsplit, sched 412 ×2 + gates + kcheck,
  job_test — all green.
- Pack: boot.img 27348992 B = kernel 18209976 + ramdisk 8822392 +
  dt 313344 (+3280).

## 3. New 6aj gate: the knob is created in the existing dir

- the overlay source must contain **no** `= debugfs_create_dir(` (the
  int48 defect) and driver 0001 must still own the root exactly once
  (`= debugfs_create_dir("p7580"` count 1);
- applied `submit.c` must have
  `dir = (struct dentry *)drv->jd.dbg_dir;`, must not test
  `IS_ERR_OR_NULL(dir)`, and must create `prof_arm` **after** that
  assignment;
- every failure path logs: the missing-dir `pr_info`, the create-failure
  `pr_err(… %ld …)`; success logs `p7580perf: prof_arm ready`; a
  silent-return scan rejects a `return;` more than 3 lines after the
  last `pr_*` (the NULL-guard return before any log is exempt);
- probe order: `p7580_job_dev_init(&drv->jd` precedes
  `p7580_prof_debugfs_init(drv);` in main.c;
- all three `p7580perf:` strings are in `submit.o` and the Image, and
  `p7580_prof_arm_fops` is still in `submit.o`.

Runtime verification (Main, one line each): `ls /sys/kernel/debug/p7580/`
must contain `prof_arm`, and `dmesg | grep p7580perf` must show
`p7580perf: prof_arm ready`. Writing 1 and reading back proves the knob
end to end.

## 4. Outputs (sha256)

- `Image-int49` (18209976 B):
  `23e391657ff4a1afa1355d0b6fd40c8bb10a1836f6c29305845aaee761dbd21b`
- `kernel-boot-int49.img` (27348992 B):
  `62377e6573ed5045e54c8afc4b229b72961e11bc8fe774106155314248b15e73`
- build log:
  `c07fe8325e875416e8c7dd855154854ee4e4bcb70aa35b9908846ad4b0a4e2ae`
- gate stdout log:
  `dbde299ab2ac15435a730d09f507496b83503e27c4d2cf49434044c9139273a7`
- evidence log:
  `91d01713703d03af037934e78d035d8292b808c65a9438b34accefbd8cf81217`
- host-test log:
  `91007da2edb706a24eb9523859d7fea5cffbc64519bd157b15d1f7ce9f1d1567`
- sched-host log:
  `1e8a4dffec79a16f0666134c35ce693d6527881ba7abfe82a9266057c32727cf`
- jobs-host log:
  `ada85c46c81086468fc7cf33cff9ce53e221b9dbc59754e2433ec639ae10d68b`
- script:
  `7e89616514979766bab9808f4272afd3b848c3b6f597f483b13b6bc96f56a8e0`
- overlay (in-repo):
  `7de60d039f22fd039ed34eed607ecc22a9035db0750941cef50cd1e59860ff37`

All paths under
`/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31/`.
No flash per ticket.

## 5. Acceptance

int48's unarmed numbers remain valid for behaviour (Main: 4×300 =
158.8 / 105.8 / 155.5 / 151.4 fps, 0 aborts, 0 ERR/TO; 4×512²×40 readback
60.6 / 54.9 / 38.3 / 39.8 fps, all clean — the storm did not reproduce in
that window), but they carry **no** prof windows because the knob did not
exist. int49 is behaviourally identical (only the debugfs init differs),
so:

1. **Knob check** (before any bench): `ls …/p7580/` shows `prof_arm`;
   `dmesg | grep p7580perf` shows `prof_arm ready`. If it says
   `unavailable` or `knob failed`, stop — that is the 6aj class again.
2. **Boot-window/contended loop, armed**: `echo 1 > ev_arm` and
   `echo 1 > prof_arm`, 300 frames at 256². Expect the first
   `p7580prof: window=…` within 100 global SUBMITs, per-class µs, and on
   a silent 0x08 the TIMEOUT → recover-done → any
   `p7580_sched: submit_err stage=… rc=… n=…`.
3. **Matrix, unarmed**: `prof_arm=0`, 3×300 at 256² — the same binary,
   inert, aiming at the 154–158 steady state.
4. **512² pre-finish readback armed**: capture payload if the silent
   0x08 fires; an `-EAGAIN`/`-EBUSY` in submit_err is a recovery-vs-next-
   submit ordering candidate, not part of the storm.

## 6. Script notes

- The first int49 gate run failed on a gate I wrote backwards: it
  asserted driver 0001 has *no* `debugfs_create_dir`, but 0001 is the
  legitimate owner (job.c). The gate now requires exactly one root
  creation in 0001 and none in the overlay. Whole suite re-run on the
  retained built tree (33/33) before the final build.
- Overlay hunk bookkeeping: inserting the reuse block (+14 lines) changed the
  submit.c hunk's new-side count; the header was re-audited
  (`+1777,124`) and every hunk in the overlay now balances
  (context+`+`/context+`-`), which is the same class of error the
  all-patch inventory gate catches for new-file patches.
