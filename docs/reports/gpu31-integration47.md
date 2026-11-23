# gpu31-integration47 — a 0-jiffy poll of a signaled fence returns >0 (BO cache reusable); 31/31 gates

Date: 2026-11-22. Owner: G31_Integrator47. Phone: on int45/int46measure;
host-side build only. No flash per ticket.

Consumes **5950b7e** (gpu31-fpssplit) plus the build-input repair
**d2c82ab** (this ticket, see §6 — 5950b7e did not build as committed).
Driver 0001 is unchanged from int46: **04db08d**
(`e7474670…`). Repo HEAD at build: `d2c82ab`.

`dma_fence_wait_timeout()` short-circuits on an already-signaled fence
and returned the bare budget. For the 0-jiffy poll that Mesa's BO-cache
fetch issues (`panfrost_bo_wait(bo, 0, RW)` → WAIT_BO with a past
absolute timeout), that is 0 — which is also what "budget ran out"
returns. Since gpu31-storm58 (98640d5) **correctly** maps 0 to
`-ETIMEDOUT`, every poll of an **idle** BO was answered "busy":
`panfrost_bo_cache_fetch()` never reused a cached BO, so each frame
allocated and freed a full BO set. Measured on silicon (int46prof,
256², 300 frames): WAIT_BO 794 calls/frame × 6.0 µs = 4.9 ms/frame of
the 5.2 ms/frame render-thread system time, CREATE_BO 4.1/frame,
map_bo 21/frame, Shmem +235 MB/run, 98–110 fps. int41's 154–160 fps is
the no-churn baseline.

## 0. TL;DR

- **Build green**: fresh scratch tree on the external disk (birth
  19:29:48, tree mtime 19:33:56), `-j4` Image, RC 0, 0 `error:` lines,
  5397-line log ending `OBJCOPY arch/arm64/boot/Image`; 499 warnings
  (same ambient count as int41–int46).
- **31/31 gate banners PASS, 0 FAIL**: the 30 int46 gates plus new
  **6ah**; the early integrity gate now scans **all 9 applied patches**
  and reports `patch inventory: 9 patches, all new-file hunk counts
  exact`.
- **6ah**, in the applied shim sources, both patch bodies, the sched-slice
  mirrors and the compiled objects:
  - `drivers/base/dma-fence.c`:
    `signed long ret = timeout ? timeout : 1;` in
    `dma_fence_default_wait` with a signaled early exit (`return ret;`),
    and `return timeout ? timeout : 1;` in the signaled path of
    `dma_fence_wait_timeout`; no bare `return timeout;` left in the file;
  - `drivers/base/dma-resv.c`: the remainder starts at
    `timeout ? timeout : 1`; `if (excl || nshared) ret = timeout;` keeps
    an **empty** resv idle with a positive remainder while a waited
    fence set still reports the remaining budget (0 = timed out);
  - compiled: `cmp x2, #0x0` + `csinc x0, x2, xzr, ne` in
    `dma_fence_wait_timeout` and `dma_fence_default_wait`, `tbnz` on the
    signaled bit in both, and `cmp x3, #0x0` + `csinc x23, x3, xzr, ne`
    in `dma_resv_wait_timeout`;
  - host sched suite: **412 checks, 0 failures** ×2 (native +
    qemu-aarch64), `gates: all PASS`, kcheck PASS — the three new checks
    are signaled+0 → 1, pending+0 → 0, empty resv+0 → >0.
- **Input repair (d2c82ab)**: 5950b7e added lines to two new-file bodies
  without advancing their `@@ -0,0 +1,N @@` headers, so `git apply`
  truncated `drivers/base/dma-fence.c` at 235/253 lines and
  `dma-resv.c` at 300/307; the first staged compile failed with
  *“expected declaration or statement at end of input”*. Counts fixed in
  all three mirror copies (bodies unchanged); the build script now
  refuses any such patch before applying anything.
- **Config**: 7/7 d0+d4 fragment symbols survive `olddefconfig`
  verbatim.
- Outputs: `Image-int47` `2a5e9416…`, `kernel-boot-int47.img`
  `80ff0f9e…` (§4). This image carries int46's race fix **and** the fps
  fix; int46 is recorded as the race-fix-only acceptance.

## 1. Build

Script `code/gpu31/series/build-int47.sh` (3735 lines; sha256
`7c86222f7056e1a8b429c69677be012a058c0ed5ac946e41c754b166b0702b98`),
a copy of int46 with int47 names, step **6ah**, the all-patches
integrity gate and a rewritten header.

Fresh `cp -a` of the pristine tree → `…/j7k/gpu31/scratch-int47`;
`TMPDIR` and every output on the external disk. Tree was clean of patch
inputs at build start (`git status` showed only untracked build
artifacts and FpsSplit's uncommitted `code/gpu31/stress/Makefile`, which
is not a kernel build input); Main's no-build-from-dirty-patches rule
was checked immediately before scratch creation.

Patches, each `git apply --check` first: backport shims 0001–0005,
Mode-B render node 0001, dmabuf CMA writecombine 0001, driver 0001+0002
= 9 patches, all new-file hunk counts exact (gated). Driver 0001
(`e7474670…`) hashed before apply and re-checked at 6v.

Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
KCFLAGS="-Wno-error" -j4 Image`.

- Build log: `…/logs/gpu31-int47-build.log` (5397 lines, 0 `error:`,
  499 `warning:`; tail `SORTEX vmlinux`/`SYSMAP System.map`/
  `OBJCOPY arch/arm64/boot/Image`). Make RC 0 and Image mtime
  (19:33:56, gated) ≥ build start enforced.
- Gate stdout: `…/logs/gpu31-int47-gates.log` (1229 lines, 31 PASS,
  0 FAIL; line 1 is the patch inventory).
- Evidence: `…/logs/gpu31-int47-evidence.log` (1272 lines).
- Host tests: `…/logs/gpu31-int47-hosttest.log` (driver T1–T11,
  chainsplit), `…/logs/gpu31-int47-schedhost.log` (412 ×2, gates PASS,
  kcheck PASS), `…/logs/gpu31-int47-jobtest.log`.
- Pack: stock `dt.img` + reference `ramdisk-new.gz` (never rebuilt) +
  the built Image → 27348992 B = kernel 18209976 + ramdisk 8822392 +
  dt 313344 (+3280 header).

Source provenance (evidence:435): `repo HEAD=d2c82ab driver-0001
sha256=e7474670…`.

Freshness: `Image-int47` has the same 18209976-byte size as int46;
2,383,256 bytes differ in the common prefix (the shim is small; the
Image embeds a new build timestamp too).

## 2. What changed in the image

| Applied site | Change |
|---|---|
| `drivers/base/dma-fence.c` `dma_fence_default_wait` | `signed long ret = timeout ? timeout : 1;`; `if (test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &fence->flags)) return ret;` before any wait; the wait paths still map a timeout to `-ETIME`/0. |
| `drivers/base/dma-fence.c` `dma_fence_wait_timeout` | signaled path returns `timeout ? timeout : 1` (mainline contract), so a 0-jiffy poll of an idle fence is positive; a pending fence still returns 0 or the remainder. |
| `drivers/base/dma-resv.c` `dma_resv_wait_timeout` | remainder initialised to `timeout ? timeout : 1`; unchanged through the fence waits; `if (excl || nshared) ret = timeout;` — an empty resv is idle (positive), a waited set reports 0 on exhaustion. |
| `code/gpu31/sched/src/*`, `code/gpu31/sched/patches/*` | mirrors of the same contract; sched host suite +3 checks. |

No driver, BO, MMU, scheduling or power semantics changed. int46's
owner-manager and int45's weak-entry/rollback work are unchanged.

## 3. Gate evidence (computed inside the build)

### 3.1 Prior gates: 30/30 PASS

All int46 gates unchanged (6aa–6ag, 6j/6z/6k/6m, 6w retargeted for the
owner manager, and the older layout/ABI/IRQ/fence/MMU/cachedir gates).
6ae (WAIT_BO `0 → -ETIMEDOUT`) remains correct and is exactly what makes
this shim contract load-bearing.

### 3.2 New 6ah: signaled 0-jiffy poll returns >0 (PASS)

Source gates: one initialiser and two signaled tests in dma-fence.c, one
`return timeout ? timeout : 1;`, **zero** bare `return timeout;`;
default_wait's test precedes its `return ret;`; wait_timeout's test
precedes its return; resv's `if (excl || nshared)` precedes
`ret = timeout;`; the resv initialiser is the `?:1` form. Mirrors
(`code/gpu31/sched/src/dma-fence.c`, `dma-resv.c`) and both patch bodies
are gated to carry the same lines, so the three copies cannot drift.

Compiled gates:

```
dma_fence_wait_timeout:  cmp x2, #0x0 ; csinc x0, x2, xzr, ne
                         tbnz w5, #0, <signaled return>
dma_fence_default_wait:  csinc x0, x2, xzr, ne
                         tbnz w4, #0, <signaled return>
dma_resv_wait_timeout:   cmp x3, #0x0 ; csinc x23, x3, xzr, ne
```

The `csinc xN, xArg, xzr, ne` is the compiler's `timeout ? timeout : 1`:
a zero budget yields exactly 1, which WAIT_BO maps to success (idle).

Host: `412 checks, 0 failures` appears exactly twice, `gates: all PASS`,
kcheck PASS.

### 3.3 Config closure (d0+d4)

7/7 fragment lines survive `olddefconfig` verbatim.

## 4. Outputs (sha256)

- `Image-int47` (18209976 B):
  `2a5e941619d3c5e8ce7d5473bc7ea02c084e9d2e1a829cc2b7ef100f21f228e0`
- `kernel-boot-int47.img` (27348992 B):
  `80ff0f9ee1a4e5b8cf5468ee278e6ce74cf833d5d5a3b9c0cdde283a46e82a28`
- build log:
  `eb1c2c1ec5b18df53337344322557915fd29d1fd0b3456a0ca806afb928d86cd`
- gate stdout log:
  `68426e6293466465fc4569628bef3b1a3b25e84a624a8a6a528ec77ef08c9dfa`
- evidence log:
  `9950e2663520aa9e21844d231fe6004cc6e7e64a479d35789f07702217cbb87d`
- host-test log:
  `91007da2edb706a24eb9523859d7fea5cffbc64519bd157b15d1f7ce9f1d1567`
- sched-host log:
  `659843454cbf241ddd2eec6ae6f7ad9aedbba76132b36d9281e939fdf681c29e`
- jobs-host log:
  `ada85c46c81086468fc7cf33cff9ce53e221b9dbc59754e2433ec639ae10d68b`
- script:
  `7c86222f7056e1a8b429c69677be012a058c0ed5ac946e41c754b166b0702b98`

All paths under
`/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31/`.
No flash per ticket.

## 5. Silicon acceptance (Main, after flashing int47)

**Flash path**: boot partition is `/dev/mmcblk0p10`; `dd` to
`/dev/block/mmcblk0p10` writes into devtmpfs and does not touch the
partition.

1. **fps**: 3×300 frames at 256², plain, under `flock`. Target ≥150 fps
   (int41 154–160; int45 101.5; int46prof measured the 4.9 ms/frame
   WAIT_BO cost). Expect `ERR=0`, `TIMEOUT=0`, `EVRING=0` and
   `Shmem:` flat across the 3 runs (int46prof grew +235 MB/run).
2. **WAIT_BO semantics**: raw UAPI probe on a fresh BO — a 0-jiffy poll
   of an idle BO must return success (rc=0); after submitting and
   without waiting, a 0-jiffy poll of a pending BO must return
   `-ETIMEDOUT`. This is the pair the shim fix and int44's mapping
   together define.
3. **Readback**: 30-frame 256² read and 10-frame 512² read, both pass
   with 0 faults.
4. **Churn stress**: kernel log must stay clean (no drm_mm/poison, no
   CREATE_BO ENOSPC). The client can still SIGSEGV under 4-thread
   churn; that is a separately owned userspace lifetime bug
   (G31_MMLock's discriminator: fault lands in a hole between renderD128
   VMAs, no MMAP_BO ENOENT). Do not call stress clean until it is fixed.
5. **Manage expectations on the two prior images**: int46 is the
   race-fix-only acceptance record; int47 supersedes it for behaviour
   (fps + race fix together).

## 6. Script notes

- **5950b7e did not build as committed.** It added 18 lines to
  `drivers/base/dma-fence.c` (declared `+1,235`, actual 253) and 7 to
  `drivers/base/dma-resv.c` (declared `+1,300`, actual 307) without
  advancing the new-file hunk headers. `git apply` honours the declared
  count, so the applied files ended mid-function and the staged compile
  failed with “expected declaration or statement at end of input” on both
  (the int37/int38 truncation class). Repair commit **d2c82ab** updates
  the counts in all three mirrors
  (`patches/drm-backport`, `code/gpu31/series`,
  `code/gpu31/sched/patches`); bodies are byte-identical before and
  after.
- **Integrity gate extended**: the pre-apply check no longer looks only
  at driver 0001 — it scans all 9 applied patches and prints
  `patch inventory: 9 patches, all new-file hunk counts exact` before
  applying anything, so this class fails with a clear message instead of
  a compiler error.
- **One rebuild was done for the log, not the code**: the first int47
  build emitted its patch-inventory line through `note`, which is only
  defined later in the script, so the line silently vanished. The line
  now uses `echo | tee -a "$EVID"` and the committed script is the one
  that produced §4 (Image shas differ from the first pass only by the
  embedded build timestamp).
- The tree was verified clean of patch inputs immediately before scratch
  creation; FpsSplit's uncommitted `code/gpu31/stress/Makefile` (churn
  target) is not part of the kernel build.
