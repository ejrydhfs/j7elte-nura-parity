# gpu31-integration42 — 750 ms dispatch budget + deterministic silent-hang injection; 26/26 gates

Date: 2026-11-22. Owner: G31_Integrator42. Phone: on int41; this
ticket builds host-side only. No flash per ticket (Main flashes).

Consumes **39cce8c** (gpu31-watchdog): the drm_sched dispatch timeout is
now the 750 ms seed (copied into `entity->timeout` at init and armed from
it at dispatch) instead of 5 s, with a runtime `p7580/job_timeout_ms`
knob and a `p7580/hang_inject` knob that masks the slot's DONE+ERR bits
for the next N dispatches — the deterministic form of the silent hang.
Driver 0001 at build time: sha256
`1d78f966958d5972022adb3c06b5291da9f66cb98254d53adbe3f33a8dbb56ba`,
repo HEAD `39cce8c`.

## 0. TL;DR

- **Build green**: fresh scratch tree on the external disk, `-j4`
  Image, RC 0, 0 `error:` lines, 5401-line log ending
  `OBJCOPY arch/arm64/boot/Image`; 499 warnings (same ambient count as
  int41).
- **26/26 gate banners PASS, 0 FAIL**: the 25 int41 gates unchanged
  (6ab/6aa/fault-latency included) plus new **6ac** (watchdog timeout +
  hang injection).
- **New 6ac**, in the applied tree, the driver 0001 mirror, the objects,
  System.map and the linked Image:
  - `P7580_JOB_TIMEOUT_MS 750u` in `panfrost7580_jm_hw.h:106`, no
    `5000u` left anywhere on the applied side; the value reaches
    `drm_sched_init` through
    `static unsigned int p7580_drv_job_timeout_ms` (submit.c:118,
    `.data` in `submit.o`) and `msecs_to_jiffies(...)` at submit.c:968,
    converted **before** the init call in `p7580_drv_sched_ensure`
    (object: `ldr w0,[x,#48]`, `bl msecs_to_jiffies` at 0x1400,
    `bl drm_sched_init` at 0x141c);
  - `p7580_drv_get/set_job_timeout_ms` (submit.c:983/:989): clamp
    `100..60000` (`sub w3,w1,#0x64` + `mov w2,#0xe9fc` (59900) +
    `b.ls`, `mov w0,#0xffffffea` for the reject), mutex-held update of
    the seed and of the live `sched->timeout` (`msecs_to_jiffies` at
    0x14e4), `job_timeout_ms=%u (new entities)` log;
  - `p7580_job_set_timeout_hooks()` (job.c:253) stores get/set/priv
    under `hw_lock`; `hang_arm` + the three hook fields in job.h
    (:295–301); dev_init defaults them inert (job.c:533–536); probe
    wires both accessors once (main.c:602–604);
  - `hang_inject`: in `hw_submit`, `if (jd->hang_arm) { hang_arm--;
    writel(JOB_INT_MASK, (js_present | js_present << 16) &
    ~MK_JS_MASK(slot)); }` **before** HEAD/CONFIG/START (job.c:654–667).
    Object proof: `mov w1,#0x10001`, `orr`/`lsl` (slot mask),
    `bic w0,w0,w1`, `mov x8,#0x1008`, `str w0,[x1]` in
    `p7580_job_hw_submit`; the job still runs, only the IRQ is masked,
    and recovery's `dev_rearm()` restores the mask. Default 0.
  - `debugfs p7580/hang_inject` and `p7580/job_timeout_ms` (both 0600,
    job.c:1929/:1933); `job_timeout_ms` is read-only (`-ENODEV` on
    write) until the driver wires the hooks, and reads back
    `P7580_JOB_TIMEOUT_MS` while unwired; `faultdbg … hang_arm=%u`
    (job.c:1651).
- **int41 fault-latency gate still holds**: the 0004 shim is untouched
  — exactly one `queue_delayed_work(entity->wq, &job->timeout_work,`
  and one `drm_sched_job_kick_timeout()`; the armed budget now comes
  from `entity->timeout` (the 750 ms seed).
- **Config**: all 7 d0+d4 fragment symbols survive `olddefconfig`
  verbatim.
- Outputs: `Image-int42` `ee50ff52…`, `kernel-boot-int42.img`
  `038ddc8b…` (§4).

## 1. Build

Script `code/gpu31/series/build-int42.sh` (3010 lines; sha256
`5cee37b18aed53503ca2bdc5c6aeecf67cb53ca60775aac6fbfd7518567f57c5`),
a copy of int41 with int42 names, the new **6ac** block, the 6z wording
updated to the per-job backstop (the shim no longer owns a 5 s
constant), and a rewritten header.

Fresh `cp -a` of the pristine tree →
`…/j7k/gpu31/scratch-int42` (birth 16:23:08, tree mtime 16:27:10);
scratch and stage deleted after the pack. `TMPDIR` and every output are
on the external disk; nothing was written to `/tmp` or `/`.

Patches, each `git apply --check` first: backport shims 0001–0005,
Mode-B render node 0001, dmabuf CMA writecombine 0001, driver 0001+0002.
Driver 0001 (`1d78f966…`) is hashed before apply and re-checked at 6v;
the new-file hunk `+count` integrity check ran first and passed.

Config: pmaports `config-samsung-j7elte.aarch64` + `d0-m1-minimal` +
`d4-panfrost7580`, then `yes "" | make ARCH=arm64 olddefconfig`
(7/7 closure, evidence line 1).

Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
KCFLAGS="-Wno-error" -j4 Image`.

- Build log: `…/logs/gpu31-int42-build.log` (5401 lines, 0 `error:`,
  499 `warning:`; tail `SORTEX vmlinux`, `SYSMAP System.map`,
  `OBJCOPY arch/arm64/boot/Image`). Make RC 0 and Image mtime
  (16:27:10, gated) ≥ build start are enforced.
- Gate stdout: `…/logs/gpu31-int42-gates.log` (881 lines, 26 PASS
  banners, 0 `GATE FAIL`).
- Evidence: `…/logs/gpu31-int42-evidence.log` (924 lines).
- Host tests: `…/logs/gpu31-int42-hosttest.log`
  (`submit_fix_test: all checks passed`, `chainsplit_test: all checks
  passed`), `…/logs/gpu31-int42-schedhost.log` (2× `409 checks, 0
  failures`, `gates: all PASS`, kcheck PASS), `…/logs/gpu31-int42-jobtest.log`
  (`job_test: all checks passed`, which pins
  `P7580_JOB_TIMEOUT_MS == 750u`).
- Pack: stock `dt.img` + reference `ramdisk-new.gz` (never rebuilt) +
  the built Image → 27348992 B = kernel 18209976 + ramdisk 8822392 +
  dt 313344 (+3280 header).

Source provenance (evidence:423): `repo HEAD=39cce8c driver-0001
sha256=1d78f966…`. The report commit does not touch driver 0001; any
later edit to it invalidates §4.

Freshness: the pristine tree has no `drivers/gpu/drm/drm_sched.c` and no
`drivers/gpu/drm/panfrost/` directory. `Image-int42` is 16,384 B larger
than `Image-int41` (18209976 vs 18193592); 10,854,233 bytes differ in the
common prefix (`cmp -l` stops at the shorter file's end).

## 2. What changed in the image

int41 measured 155 fps with 0 faults ×3 at `serialize_fs=1`; the open
issue was a *silent* hang (no IRQ, `JS_STATUS 0x08`, fence unsignalled)
that only the sched timeout breaks, and it cost the full 5 s budget per
occurrence. int42 bounds it to <1 s and makes it reproducible.

| Applied site | Change |
|---|---|
| `panfrost7580_jm_hw.h:106` | `P7580_JOB_TIMEOUT_MS 750u` (was `5000u`); the comment now states the budget is the drm_sched dispatch timeout and names the runtime knob. |
| `submit.c:118` | `static unsigned int p7580_drv_job_timeout_ms = P7580_JOB_TIMEOUT_MS;` — the single source the entity timeout is seeded from. |
| `submit.c:968` | `msecs_to_jiffies(p7580_drv_job_timeout_ms),` as the `drm_sched_init` budget (no hardcoded constant left). |
| `submit.c:983` | `p7580_drv_get_job_timeout_ms()` returns the seed. |
| `submit.c:989` | `p7580_drv_set_job_timeout_ms()`: reject outside 100..60000 (`-EINVAL`), under `p7580_drv_sched_lock` store the seed and, when `p7580_drv_sched_live`, set `sched->timeout = msecs_to_jiffies(ms)`; log `job_timeout_ms=%u (new entities)`. |
| `panfrost7580_drv.h:99–100` | declarations for both accessors. |
| `job.h:295–301` | `hang_arm`, `get_timeout_ms`, `set_timeout_ms`, `timeout_priv`. |
| `job.c:253` | `p7580_job_set_timeout_hooks()` forwards the three values under `hw_lock`. |
| `job.c:533–536` | dev_init: `hang_arm = 0`, hooks `NULL` (knob read-only, injection inert). |
| `job.c:654–667` | `hw_submit`: armed injection masks `JOB_INT_MASK` DONE+ERR for the slot via `(js_present | js_present<<16) & ~MK_JS_MASK(slot)` before the START, decrements `hang_arm`, logs `hang_inject slot=… seq=… irq-masked`. |
| `job.c:1651` | `faultdbg … hang_arm=%u`. |
| `job.c:1821–1872` | get/set for both knobs: hang read/write `hang_arm` with the `IRQ masked at the next N dispatches` log; timeout get falls back to `P7580_JOB_TIMEOUT_MS`, set returns `-ENODEV` while unwired, else forwards. |
| `job.c:1929/1933` | `debugfs_create_file("hang_inject"/"job_timeout_ms", 0600, …)`. |
| `main.c:602–604` | probe: `p7580_job_set_timeout_hooks(&drv->jd, p7580_drv_get_job_timeout_ms, p7580_drv_set_job_timeout_ms, drv);`. |

The injection changes no fence, BO, mapping or scheduling semantics: the
job still runs and retires in hardware, so a masked slot takes exactly
the timeout → `timedout_job` → `recover()` path that a real silent hang
takes (dump + power-on + `recover-done cycle=1`), and `dev_rearm()`
restores the mask. 750 ms is 2× the worst measured frame (512²
readback: 373 ms of CPU detile; GPU work is tens of µs), so a legitimate
frame is not classified as a hang.

## 3. Gate evidence (computed inside the build)

### 3.1 Prior gates intact (25/25 PASS)

Carried from int41: ops layout, irq, slot, abi, gpuwrite, coherency,
instrtype, iova, armfence, ptewalk, pmget, timedout, submitsnap,
syncobj-wait, jddecode, multidep, serialfence, cachedir, mmaprefs,
chainsplit, defectb, chainsplit-length, fault-latency, 6aa (power-idle +
always-power-on recovery) and 6ab (first-fault ring/page dump/hygiene/
`serialize_fs`). Unchanged computed constants: ops=24/slot=232/ran=236,
FS→JS0/req0→JS1/ONLY_COMPUTE→JS2, per-node cfg `0x83700`, the single
shmem page-get/flush/store triple, 56/192/192/64/32 chain lengths,
ev-ring symbols/strings and the recovery cycle order. The 6z gate still
finds exactly one `queue_delayed_work(entity->wq, &job->timeout_work,`
and one `drm_sched_job_kick_timeout()` — the 0004 shim is unchanged and
its armed budget is now `entity->timeout`.

### 3.2 New 6ac: 750 ms budget + hang injection (PASS)

Applied-tree anchors:

```
jm_hw.h:106    #define P7580_JOB_TIMEOUT_MS	750u
drv.h:99       unsigned int p7580_drv_get_job_timeout_ms(void *priv);
drv.h:100      int p7580_drv_set_job_timeout_ms(void *priv, unsigned int ms);
submit.c:118   static unsigned int p7580_drv_job_timeout_ms = P7580_JOB_TIMEOUT_MS;
submit.c:968       msecs_to_jiffies(p7580_drv_job_timeout_ms),
submit.c:983   unsigned int p7580_drv_get_job_timeout_ms(void *priv)
submit.c:989   int p7580_drv_set_job_timeout_ms(void *priv, unsigned int ms)
submit.c:992       if (ms < 100u || ms > 60000u)
submit.c:995       p7580_drv_job_timeout_ms = ms;
submit.c:997       p7580_drv_sched.timeout = msecs_to_jiffies(ms);
submit.c:999   pr_info("p7580_sched: job_timeout_ms=%u (new entities)\n", ms);
job.h:295      unsigned int hang_arm;
job.h:299–301  get_timeout_ms / set_timeout_ms / timeout_priv
job.c:253      void p7580_job_set_timeout_hooks(struct p7580_job_dev *jd,
job.c:533–536  jd->hang_arm = 0; jd->get_timeout_ms = NULL; … timeout_priv = NULL;
job.c:654      /* gpu31-watchdog hang_inject: … */
job.c:662          jd->hang_arm--;
job.c:663          p7580_writel(jd, P7580_JOB_INT_MASK,
job.c:664              (jd->js_present | (jd->js_present << 16)) &
job.c:665              ~P7580_MK_JS_MASK(slot));
job.c:1651     seq_printf(m, "faultdbg … hang_arm=%u\n", …);
job.c:1821     static int p7580_job_hang_get(void *data, u64 *val)
job.c:1831     static int p7580_job_hang_set(void *data, u64 val)
job.c:1846     static int p7580_job_timeout_get(void *data, u64 *val)
job.c:1860     static int p7580_job_timeout_set(void *data, u64 val)
job.c:1929     debugfs_create_file("hang_inject", 0600, dir, jd, …
job.c:1933     debugfs_create_file("job_timeout_ms", 0600, dir, jd, …
main.c:602     p7580_job_set_timeout_hooks(&drv->jd,
main.c:603         p7580_drv_get_job_timeout_ms,
main.c:604         p7580_drv_set_job_timeout_ms, drv);
```

The 6ac block refuses any `P7580_JOB_TIMEOUT_MS … 5000u` on the applied
side, in the mirror, and requires exactly one registration in main.c,
one definition of the forwarder, all four state fields, inert defaults,
the four mask terms, and mask-before-START ordering. `job_timeout_ms`
gates pin the clamp, the seed store, the live-`sched` update and the
unwired read-only behaviour.

Compiled proof (`job.o`, `submit.o`, -O2):

```
nm job.o    T p7580_job_set_timeout_hooks   0x74c   → ffffffc00057fc6c
nm submit.o T p7580_drv_get_job_timeout_ms  0x144c  → ffffffc00057aaac
            T p7580_drv_set_job_timeout_ms  0x1460  → ffffffc00057aac0
            d p7580_drv_job_timeout_ms      0x30    (.data seed)

p7580_drv_sched_ensure:
  13fc: ldr w0, [x19, #48]        (seed, not a literal)
  1400: bl  <msecs_to_jiffies>
  141c: bl  <drm_sched_init>      (converted seed handed to init)

p7580_drv_set_job_timeout_ms:
  1460: sub w3, w1, #0x64         (ms - 100)
  1464: mov w2, #0xe9fc           (59900 = 60000 - 100)
  1468: cmp w3, w2 ; b.ls         (accept 100..60000)
  1470: mov w0, #0xffffffea       (-EINVAL)
  1490: bl  <mutex_lock>  14e4: bl <msecs_to_jiffies>  14bc: bl <mutex_unlock>
  14f4: str x0, [x3, #80]         (live sched->timeout = jiffies(ms))

p7580_job_hw_submit (hang_inject):
  1008: sub w0, w0, #0x1          (hang_arm--)
  1024: mov w1, #0x10001          (P7580_MK_JS_MASK base)
  1028: orr w0, w0, w0, lsl #16   (js_present | js_present << 16)
  102c: lsl w1, w1, w24           (<< slot)
  1030: bic w0, w0, w1            (~slot mask)
  1038: mov x8, #0x1008           (JOB_INT_MASK)
  103c: add x1, x1, x8
  1040: str w0, [x1]              (writel, after dsb st at 0x1020)
```

Strings live once each in the objects and the Image (length prefix shown
by `strings`):

```
6p7580_sched: job_timeout_ms=%u (new entities)
6p7580_job: hang_inject=%u (IRQ masked at the next %u dispatches)
6p7580_job: hang_inject slot=%d seq=%u irq-masked
faultdbg ev_arm=%u ev_head=%u ev_dumped=%u serialize_fs=%u hang_arm=%u
```

Host: `job_test: all checks passed` (pins `P7580_JOB_TIMEOUT_MS == 750u`
and `RESET_TIMEOUT_MS == 3000u`), `submit_fix_test` mirror updated to
750, `chainsplit_test` unchanged — all green.

### 3.3 Config closure (d0+d4)

7/7 fragment lines survive `olddefconfig` verbatim
(`CONFIG_MODULES`, `DRM`, `DRM_KMS_HELPER`, `DRM_EXYNOS`,
`DRM_EXYNOS_DMABUF`, `DRM_EXYNOS_G2D`, `DRM_PANFROST7580=y`).

## 4. Outputs (sha256)

- `Image-int42` (18209976 B):
  `ee50ff526680d1bbb61a49e6895a159286778f05851b12a75a448770c68ba559`
- `kernel-boot-int42.img` (27348992 B):
  `038ddc8bbb92a7e1c984772e55de47f3a8d470aeef2e1bb8158735c04bce0195`
- build log:
  `90851748cf8c6a4f5fea592f13f83d1f722b64dbeb4a1222faad6f5a75ae814e`
- gate stdout log:
  `34a8f025430d54d28fc7f68b53974b96a60134fd6b2d6e32c9f7edf4670ae484`
- evidence log:
  `336e1eb46a1a2acfb350a0e5badf0bcec812d2fb9849452fbb287b63fa65f628`
- host-test log:
  `91007da2edb706a24eb9523859d7fea5cffbc64519bd157b15d1f7ce9f1d1567`
- sched-host log:
  `4772168a7168a38bf5c0e1c84273ea7ad1ee77eabc1dd628b3f0c66e9d622c00`
- jobs-host log:
  `ada85c46c81086468fc7cf33cff9ce53e221b9dbc59754e2433ec639ae10d68b`
- script:
  `5cee37b18aed53503ca2bdc5c6aeecf67cb53ca60775aac6fbfd7518567f57c5`

All paths under
`/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31/`.
No flash per ticket.

## 5. Silicon acceptance (Main/Watchdog, after flashing int42)

1. **Boot/probe**: `…/p7580/` shows `hang_inject` (reads 0) and
   `job_timeout_ms` (reads 750). A write of 50 or 70000 to
   `job_timeout_ms` must fail with `EINVAL`; a write of 2000 must log
   `job_timeout_ms=2000 (new entities)` and read back 2000 (and a new
   client run then times out at 2 s, not 5 s).
2. **Deterministic hang**: with no workload, `echo 1 > hang_inject`,
   start 300 frames at 256² with `serialize_fs=1`: the next dispatch
   must log `hang_inject slot=… seq=… irq-masked`, then **within ~1 s**
   `TIMEOUT slot=… js_status=0x8` → `EVRING`/`PAGEDUMP` capture →
   `recover-done … cycle=1`, and the run must continue to 300 frames
   with no further timeout. This proves the <1 s bound and that the
   int41 recovery path handles the silent signature.
3. **No false positives**: unarmed (`hang_inject=0`) run of 300 frames:
   `ERR=0`, `TIMEOUT=0`, `cycle=0`, fps at the int41 level (155 fps at
   256², `serialize_fs=1`); `faultdbg … hang_arm=0`.
4. **Injection budget**: `echo 3 > hang_inject`, 300 frames: exactly
   three masked dispatches, three recoveries, then clean; `hang_arm`
   reads 0 at the end (dev_rearm restores the mask each recovery).
5. **Readback**: `rdtest` at 512² (frame ~373 ms CPU detile): no timeout
   with the default 750 ms — a single frame must not be mistaken for a
   hang.

## 6. Script notes

- 6ac was dry-run against the patched tree and its real `job.o`/
  `submit.o` before the final build. Four codegen corrections came out
  of that run:
  1. the seed is a `.data` variable read via `ldr w0,[x,#48]`, not a
     symbol reloc — the gate pins `nm … d p7580_drv_job_timeout_ms` plus
     `msecs_to_jiffies` before `drm_sched_init`;
  2. the 60000 upper clamp compiles as `mov #0xe9fc` (60000-100) after
     `sub #0x64`, not `mov #0xea60`;
  3. `writel` to `JOB_INT_MASK` compiles to `mov x8,#0x1008; add;
     str [x1]`, not a `[x,#4104]` store;
  4. `grep -n`/`grep -c` without `-E` treats `+` literally, so the mask
     offset search needed `-E` (a silent empty line index otherwise).
- The 0004 shim was deliberately not touched: 6z still asserts one
  delayed-work arm and one kick, and the budget flows from
  `entity->timeout`, so the runtime knob reaches old and new entities
  without a shim edit.
- Retained: new-file hunk `+count` integrity, `make` RC 0 + Image newer
  than build start, defectb loop proof, config closure, driver-0001
  drift re-check, and all int41 gates (6aa/6ab included).
