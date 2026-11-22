# gpu31-integration38 — defectb shmem flush + 192-byte VERTEX split, build-integrity guards, 22/22 gates

Date: 2026-11-21. Owner: G31_Integrator38. Phone: on int36/int37
state, user power-cycling after a GPU-idle shutdown hang; this ticket
builds host-side only. No flash per ticket (Main flashes).

Consumes **9dd7542** (gpu31-defectb: `__flush_dcache_area()` after every
`shmem_read_mapping_page()` in `p7580_shmem_alloc()`, mirrored into driver
0001) and **742dc52** (gpu31-chainsplit follow-up: the VERTEX/COMPUTE
aggregate is 192 B; shadow lengths VERTEX/TILER/FUSED 192, WRITE_VALUE
56, FRAGMENT 64, NULL 32) on top of int37 (d824f29 chainsplit, whose
build script this one is a copy of). The ticket also had to fix a
build-blocking defect the defectb hand-off left in driver 0001 (§2) and
adds two build-integrity guards (§6).

## 0. TL;DR

- **Build green**: fresh scratch, `-j4` Image, RC 0, 0 `error:` lines,
  5393-line log ending `OBJCOPY arch/arm64/boot/Image`; 498 warnings,
  the same count as int37 (ambient `-Wno-error` noise plus the 3×
  `vmlinux has a LOAD segment with RWX permissions` link warnings).
- **22/22 gate banners PASS, 0 FAIL** (the 20 int37 gates unchanged in
  invariant, plus new **6x** defectb and **6y** chainsplit-length).
- **6x**: the applied gem.c calls `__flush_dcache_area(page_address(page),
  PAGE_SIZE)` exactly once, inside the per-page loop between the page get
  and `pages[i] = page` (source lines 652/681/682). `gem.o` carries
  exactly one `R_AARCH64_CALL26 __flush_dcache_area` (0x10a8), in
  `p7580_gem_create` — the symbol holding the single page-get reloc
  (`shmem_read_mapping_page_gfp`, 0x10cc) — and the call is immediately
  followed by the `pages[i] = page` store `str x28, [x24, x27, lsl #3]`
  (0x10ac), `i++`, the loop compare and a branch back to the page get.
  The linked vmlinux has the same single flush call
  (`ffffffc00057d8e8` → `ffffffc0002189c0 <__flush_dcache_area>`) and the
  same store at `ffffffc00057d8ec`. U ref in `gem.o`, one `T` def in
  `System.map`, one driver-0001 `+` line.
- **6y**: applied `jm_hw.h` returns 56/192/192/64/32 for
  WRITE_VALUE/VERTEX/TILER+FUSED/FRAGMENT/NULL and documents
  `MALI_COMPUTE_JOB_LENGTH 192`; the stale `return 96u;` is gone. Host
  T1 pins the same numbers.
- **Hand-off defect fixed (61bd92e)**: 9dd7542 added 20 comment lines to
  the gem.c body but left its new-file hunk at `@@ -0,0 +1,1638 @@` for
  1658 `+` lines, so `git apply` dropped the last 20 lines and the first
  int38 attempt died at `gem.c:1638` (§2).
- Outputs: `Image-int38` `3bbcc91a…`, `kernel-boot-int38.img`
  `42d3a160…` (§4).

## 1. Build

Script `code/gpu31/series/build-int38.sh` (committed **61bd92e**, gate
semantics refined in **ba49eba**; sha256
`dae9917c8e849354f2c842dbeddecdee6ee9738186ab10e044ff9cf03089664c` at
the build). A copy of int37 (d824f29 / 7bd0399) with int38 names, the
two new gate blocks and the two integrity guards.

Fresh `cp -a` of the pristine tree → `scratch-int38` (birth 23:14:43,
tree mtime 23:19:30). Patches: backport shims 0001–0005, Mode-B render
node 0001, dmabuf CMA writecombine 0001, driver 0001+0002 — each applied
with `git apply --check` first. Config: pmaports
`config-samsung-j7elte.aarch64` + `d0-m1-minimal.fragment` +
`d4-panfrost7580.fragment`, then `yes "" | make ARCH=arm64 olddefconfig`.

Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
KCFLAGS="-Wno-error" -j4 Image`.

- Build log: `…/j7k/gpu31/logs/gpu31-int38-build.log` (5393 lines,
  0 `error:`; tail `SORTEX vmlinux`, `SYSMAP System.map`,
  `OBJCOPY arch/arm64/boot/Image`). Make RC is now gated to 0 and the
  Image must be newer than the build start (§6).
- Gate stdout/evidence: `…/logs/gpu31-int38-gates.log` (22 PASS, 0 FAIL),
  `…/logs/gpu31-int38-evidence.log` (536 lines). Host tests:
  `…/logs/gpu31-int38-hosttest.log` (`submit_fix_test: all checks
  passed`, `chainsplit_test: all checks passed`).
- Pack: stock `dt.img` + reference `ramdisk-new.gz` (never rebuilt),
  27332608 B = kernel 18193592 + ramdisk 8822392 + dt 313344 (+3280
  header), built by `stage-int38/mkboot.py`. Scratch, stage and
  host-test dir deleted after the pack.

Source provenance (printed into evidence line 393): build-time repo HEAD
**ba49eba**, driver-0001 sha256
`1703d93c792ad3f120785d3da04d35a7d90d0f86a1eea6ee530ecc4ecac52e87`
(recorded before `git apply`, re-checked at 6v). The driver-0001 hash
quoted here is the built one; the report commit does not touch it.
Freshness: the pristine tree has no `drivers/gpu/drm/drm_sched.c` and no
`drivers/gpu/drm/panfrost/` directory.

`Image-int38` differs from `Image-int37` in 5,351,078 bytes (same
18193592-byte size; different build).

## 2. Hand-off defect fixed: gem.c hunk count left stale by 9dd7542

9dd7542 inserted the 20-line `__flush_dcache_area` rationale comment into
`panfrost7580_gem.c` but did not advance that file's new-file hunk header:
the patch still declared `@@ -0,0 +1,1638 @@` while the hunk carries
**1658** `+` lines (1638 + 20). git apply honours the declared count and
drops the tail, so the applied file stopped at the `goto fail_detach;`
inside `p7580_gem_prime_import` and lost the rest: the `}` of the
`IS_ERR_OR_NULL(sgt)` block, the `gem_prime_import_sg_table` call, the
three BO field stores, the `fail_unmap:`/`fail_detach:` labels and the
function's closing brace. The first int38
attempt failed in that file (observed in its build log; the log path was
later overwritten by the green run):

```
drivers/gpu/drm/panfrost/panfrost7580_gem.c:1638:17: error: expected declaration or statement at end of input
drivers/gpu/drm/panfrost/panfrost7580_gem.c:1638:17: error: label 'fail_detach' used but not defined
make[4]: *** [scripts/Makefile.build:308: drivers/gpu/drm/panfrost/panfrost7580_gem.o] Error 1
```

Fixed in **61bd92e**: header `1638 → 1658` (content unchanged; hunk
arithmetic is 1638 declared + the 20 defectb comment lines). The defectb
code itself now compiles and is proven by 6x.

That attempt also exposed a second hazard: `make` was not checked, and
the pristine tree ships a prebuilt `Image`/`System.map`, so the gate
section briefly read the pristine Image (dated Nov 18 23:55, sha
`9a38de83…`) as the build's output and failed on an unrelated System.map
count. The script now asserts `BUILD_RC == 0` and
`Image mtime >= BUILD_START`, and (before applying anything) checks that
every new-file hunk's declared `+count` equals its actual `+` lines. The
first int38 run after the *gate* fix (not the patch fix) showed the -O2
block layout that the 6x adjacency gate now encodes (§3.2).

## 3. Gate evidence (computed inside the build)

### 3.1 Prior gates intact (20/20 PASS)

The 20 int37 gates are carried verbatim (ops/irq/slot/abi/gpuwrite/
coherency/instrtype/iova/armfence/ptewalk/pmget/timedout/submitsnap/
syncobj-wait/jddecode/multidep/serialfence/cachedir/mmaprefs/chainsplit)
and all PASS. Unchanged computed constants: ops layout **ops=24
slot=232 ran=236**; `dma_map_ops` slots for_cpu=64 / for_device=72 /
unmap=40, dirs TO=1/FROM=2; vm_flags PFNMAP=0x400,
MIXEDMAP|DONTCOPY=0x10020000; slot routing FS→JS0, req0→JS1,
ONLY_COMPUTE→JS2; JS1 zero-req; 6w chainsplit object fingerprint
(`t p7580_drv_gpu_read` 0x320, `t p7580_drv_shadow_put` 0xde8, one
shadow `p7580_gem_create` 0x24dc, one `drm_sched_job_alloc` 0x207c,
one arm 0x2228, one submit 0x2b6c, 3 `add_dependency`, 1
`drm_syncobj_replace_fence`, 2 `gpu_read` at 0x1ad4/0x2624, 1
`shadow_put` at 0x27b4, `hw_submit` cfg `0x3700 | 0x80000` = `0x83700`).
6w's host tests now also pin the 742dc52 lengths T1 56/192/192/64/32.

### 3.2 New 6x defectb gates (PASS)

Tree (applied `drivers/gpu/drm/panfrost/panfrost7580_gem.c`):

```
page-get = 652   struct page *page = shmem_read_mapping_page(
flush    = 681   __flush_dcache_area(page_address(page), PAGE_SIZE);
store    = 682   pages[i] = page;
```

Exactly one flush call in the file and one in driver 0001's `+` lines;
the page-get/flush/store span (30 lines) is the loop body.

Object (`panfrost7580_gem.o`, `-dr`):

```
    10a8: bl 0 <__flush_dcache_area>
          R_AARCH64_CALL26  __flush_dcache_area      (exactly 1 in the object)
    10ac: str x28, [x24, x27, lsl #3]                 (pages[i] = page)
    10b0: add x27, x27, #1                            (i++)
    10b4: cmp w25, w27
    10b8: b.ls ...                                    (exit when i > nr)
    10bc: ldr x0, [x23, #208]
    10cc: bl 0 <shmem_read_mapping_page_gfp>
          R_AARCH64_CALL26  shmem_read_mapping_page_gfp  (exactly 1 in the object)
```

`nm`: `U __flush_dcache_area` in `gem.o`; one `T __flush_dcache_area` in
`System.map` (`ffffffc0002189c0`). Note the block layout: **the flush
sits at 0x10a8, below the loop head at 0x10bc**, because the success edge
branches from the page-get's `cmn/b.ls` into the flush block and
falls through into the store, `i++`, compare and the next page get. The
gate therefore proves *loop-body adjacency plus the store*, not
ascending addresses (an address-order gate failed on this correct code
until ba49eba).

Linked kernel (`vmlinux`, the Image's source; `p7580_gem_create` in
both):

```
ffffffc00057d8e8: bl ffffffc0002189c0 <__flush_dcache_area>   (1 in the function)
ffffffc00057d8ec: str x28, [x24, x27, lsl #3]                  (pages[i] = page)
ffffffc00057d8f8: b.ls ffffffc00057dac8
ffffffc00057d8fc: ldr x0, [x23, #208]
ffffffc00057d900: mov x1, x27
```

The page-get call is in the same function, 9 disassembly lines from the
flush (evidence: page-get line 93, flush line 84 of the function) and the
gate also requires the store within the next 6 instructions.

So the flashed Image executes one clean+invalidate of `page_address(page)`
for every page the loop obtains, after the page is returned and before it
is stored in the BO's page array — the defectb ordering.

### 3.3 New 6y chainsplit-length gates (PASS)

Applied `panfrost7580_jm_hw.h` (from 742dc52):

```
MALI_WRITE_VALUE_JOB_LENGTH 56, MALI_COMPUTE_JOB_LENGTH 192 (the
VERTEX job: header 32 + Invocation 8 + Parameters 8 + Draw 128 + padding 16)
case P7580_JOB_TYPE_NULL:          return P7580_JOB_DESC_HDR_SIZE;  (32)
case P7580_JOB_TYPE_WRITE_VALUE:   return 56u;
case P7580_JOB_TYPE_VERTEX:
case P7580_JOB_TYPE_TILER:
case P7580_JOB_TYPE_FUSED:         return 192u;
case P7580_JOB_TYPE_FRAGMENT:      return 64u;
```

No `return 96u;` remains. Host `chainsplit_test` T1 checks the same
five aggregate sizes against the real header.

## 4. Outputs (sha256)

- `Image-int38`:
  `3bbcc91a08e5f1af6ae6e9e2aa221a4a113ee1a94a58e5d93f22b04b3b0ed8a4`
- `kernel-boot-int38.img`:
  `42d3a160ea028535fe28404b0d089b440f71becf28368949a4f392c9e5c17a9f`
- build log:
  `b1ebdfcbd440ac81ecf8b74328ec43ec98185c3cbedaef15182b24bc21241511`
- evidence log:
  `165c1dae2c4407845c71fcbe39c19c6d630cb5591344ed5e715767244417a7d2`
- gate stdout log:
  `4de6e603d706588d6bd290318e3ce9e4b3958e79b4a025aec5e60c5165bba3a8`
- host-test log:
  `91007da2edb706a24eb9523859d7fea5cffbc64519bd157b15d1f7ce9f1d1567`

Config: pmaports base + d0-m1-minimal + d4-panfrost7580, `olddefconfig`
closure. No flash per ticket.

## 5. Silicon acceptance (Main, after flashing int38)

int38 is the first image with either fix: Defect B (shmem flush) and the
192-byte VERTEX shadow copy. Both must be checked; the previous images
(int36/int37) cannot show them.

### 5.1 Defect B is gone

- `pf7580-selftest --defectb-test`: 32/32 trials keep the written words;
  the immediate in-kernel read must agree with the user alias (int36
  showed 31/32 lost with direct alias disagreement).
- Ferrofluid frames: `p7580_pin: sub … w=c87a9835 01403d3d …` for the FS
  ISA BO, no `p7580_job: ERR` over 5 frames, and the 0xCF read of the ISA
  BO is non-zero.
- If the page still zeroes, capture the BO's `p7580_pin` line plus the
  first `p7580_job: dispatch … sw=` and stop: that would contradict the
  single-copy proof in §3.2 and needs a new investigation, not a re-flash.

### 5.2 The split runs a complete VERTEX node

For the first 8 split submits:

```
p7580_sched: chain jc=0x<user jc> nodes=3 shadow=0x<iova> stride=256
p7580_sched: split seq=<n> pos=0/2 slot=1 jc=0x<iova+0x000>
p7580_sched: split seq=<n+1> pos=1/2 slot=1 jc=0x<iova+0x100>
p7580_sched: split seq=<n+2> pos=2/2 slot=1 jc=0x<iova+0x200>
```

plus `p7580_job: dispatch … cfg=0x00083700 … sw=` with `w5=0/w6=0`. On
int37 the 96-byte copy truncated Draw at Textures onward and the
standalone vertex node faulted/hung before the tiler; with 192 B the
control census must now produce bins.

### 5.3 Census and pixel checks (int37 §5.2–5.4 unchanged)

Run the control census (`PC2_POLLSCAN=1 PC2_REFUSE_FRAG=1 PC2_MAX=1
FRAMES=1 sh /data/polylist/run.sh`): the scan must read the VS triangle
from `Draw.Position` and `tiles_nonempty` must be non-zero and not
exactly 1. Then the prefill supersession run must bin the VS triangle,
not the prefill's single tile. With Defect B fixed, the plain run's
centre pixel must be red and the corner clear. A split-node
`0x41 POWER_FAULT` now means the 192-byte shadow or its IOVA is wrong:
capture `p7580_sched: split`, `p7580_job: dispatch`/`irq-err`,
`PTEWALK`.

## 6. Script notes

- **Hunk integrity (new, before any `git apply`)**: every new-file hunk's
  declared `+count` must equal its actual `+` lines. This is the class of
  defect that truncated int37's submit.c and 9dd7542's gem.c; the check
  now fails closed instead of building a silently truncated file.
- **Real-build guards (new)**: `make` RC must be 0 and
  `arch/arm64/boot/Image` mtime must be ≥ the build start, so the
  pristine tree's prebuilt Image/System.map can never be quoted as this
  build (that happened once during this ticket, §2).
- **6x adjacency, not address order**: at -O2 the per-page loop is laid
  out as `head(page get) → success edge → flush block → store → i++ →
  head`, so the flush call address is lower than the page get. The gate
  asserts one flush reloc in the same symbol as the one page-get reloc,
  the two within 12 disassembly lines (loop body), the pages-array store
  within the next 6 instructions of the flush, and the same pair+store in
  the linked vmlinux. A future codegen change that moves the flush out of
  the loop fails the gate loudly instead of passing on address order.
- Host tests run from `$EXT/hosttest-int38` (deleted); the `perl`
  fallback-free `make check` output is appended to the evidence log.
- Driver-0001 provenance: sha256 recorded before apply and re-checked at
  6v; the build used `1703d93c…` (HEAD ba49eba). Any later edit to that
  patch needs a rebuild before its sha256s can be quoted.
