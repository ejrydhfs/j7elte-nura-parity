# GPU31 — GPU write proof: Zero-fill descriptor + kernel-side BO readback

Date: 2026-11-20. Owner: G31_GpuWriteProof. Phone: up, int18 (Image `c9bc0634`, boot `06b4ab33`). Ticket: `docs/reports/gpu31-gpuwrite.md` (this file).

Related: `gpu31-jobabi.md` (descriptor layout), `gpu31-coherency.md` (DMA-sync model this ticket reuses, not replaces), `gpu31-abialign.md` (ABI gates preserved byte-identical), `gpu31-irq.md` / `gpu31-pumpfix.md` (completion path).

## 0. TL;DR

Two defects, both fixed in this ticket:

1. **Descriptor encoding (the actual blocker): the old pattern test encoded `Immediate-32` (type 6), which does not exist on the T720.** The T720 is a Midgard-v4 (`PAN_ARCH 4`) part; its `Write Value Type` enum has only three values (1 Cycle Counter, 2 System Timestamp, 3 Zero). Types 4–7 (Immediate 8/16/32/64) exist only in the v6+ genxml for Bifrost parts. The old client wrote `w[10] = 6` — a reserved type on this GPU — so the job either faulted or no-opped while the fence still signalled. The fix encodes type **3 (Zero)** and pre-fills the target BO with `0xA5A5A5A5` from userspace: all-zero after completion proves the GPU wrote the page.
2. **Kernel-side BO readback (temporary debug switch):** a fops-routed `DEBUG_READ_BO` (nr `0xCF`, 16 B `{handle, offset, value OUT, pad}`) that reuses `p7580_gem_job_done_sync` (the same `dma_sync_for_cpu` maintenance `free_job` runs) then reads one u32 via `kmap_atomic`. It reports the kernel's view of the same page after completion, so `kernel-zero + user-0xA5` (visibility bug) is distinguishable from `both-0xA5` (GPU never wrote). It is **not** in the six-ioctl driver table, is marked `DEBUG-ONLY` everywhere, and is **stripped before cutover**.

Device proof is pending the int19 flash (build running at ticket close; phone still on int18 so the new `0xCF` route is not live yet). Host proof is complete: client mock suite PASS, `gem_validate` PASS, `job_test` PASS, cross-build `-Werror` PASS, 3 edited GEM TUs `-fsyntax-only` PASS, 0001 regen + `git apply --check` PASS.

> **Cutover checklist (do not land with this ticket):** remove `panfrost_debug_read_bo` (ioctl.c/h), `p7580_gem_debug_read_word` (gem.c/h), the `0xCF` route + struct + ABI pins (syncobj.c), the `pf7580_debug_read_bo` client wrapper (pf7580.c/h), and the int19 `6f` gate + build-int19.sh; restore the six-row table comment in main.c.

## 1. Re-derivation: why the old descriptor could never work on T720

### 1.1 T720 is PAN_ARCH 4 (Midgard v4), not v6

`src/panfrost/model/pan_model.h:107-124` (Mesa tree the device build came from):

```c
static inline unsigned pan_arch(uint64_t gpu_id) {
   switch (MIDGARD_PROD_ID(gpu_id)) {
   case 0x600: case 0x620: case 0x720: return 4;
   ...
```

`0x720 → arch 4`. The arch selects the genxml: v4 for T720, v5 for T760/T820, v6+ for Bifrost (G71+).

### 1.2 v4/v5 genxml has no Immediate types; v6+ does

On-device Mesa 20.3 source (`/data/mesabuild/mesa20/src/panfrost/lib/midgard.xml`) and the matching host Mesa tree agree verbatim:

| genxml | `Write Value Type` values |
|---|---|
| v4, v5 | 1 Cycle Counter, 2 System Timestamp, **3 Zero** (only) |
| v6, v7 | + 4 Immediate-8, 5 Immediate-16, **6 Immediate-32**, 7 Immediate-64 |

Grep counts on the host tree (`src/panfrost/genxml/`): `immediate` appears 0× in v4/v5, 5× in v6. The old test's claim ("Mesa 20.3's midgard.xml: type 6 = Immediate-32") is true of the *file* but false for *this GPU*: the v6 union XML documents all arches, and the v4 section actually used for T720 stops at Zero. Mesa 20.3's own emitter (`lib/pan_scoreboard.c:217-219`) only ever emits `MALI_WRITE_VALUE_TYPE_ZERO`:

```c
pan_section_pack(transfer.cpu, WRITE_VALUE_JOB, PAYLOAD, payload) {
        payload.address = polygon_list;
        payload.type = MALI_WRITE_VALUE_TYPE_ZERO;
}
```

There is **no** Mesa 20.3 producer of Immediate-32 on Midgard — it is a Bifrost-only pack path (`PAN_ARCH >= 6` guards in `pan_jc.h`).

### 1.3 Payload layout (unchanged, still 24 B)

`MALI_WRITE_VALUE_JOB_PAYLOAD_pack` (generated `midgard_pack.h`, identical on device and host):

```c
cl[0] = address[31:0]; cl[1] = address[63:32];
cl[2] = type;          cl[3] = 0;
cl[4] = immediate[31:0]; cl[5] = immediate[63:32];
```

The old client encoded `{addr_lo, addr_hi, 6, 0, 0xC0C3E8E1, 0}`. On T720 the `6` is a reserved type: the job-control unit cannot dispatch it as an Immediate write. Observed behaviour matches exactly: `SUBMIT rc=0`, `SYNCOBJ-WAIT rc=0` (fence signalled, slot retired cleanly), `result[0] == 0x00000000` with the result page pre-zeroed — the fence proves *retirement*, not *execution of a valid write*.

### 1.4 Header layout (unchanged, verified against the pack)

`MALI_JOB_HEADER_pack` (`midgard_pack.h:6571+`):

```c
cl[4] = is_64b[0] | type[1:7] | barrier[8] | invalidate_cache[9] |
        suppress_prefetch[11] | enable_texture_mapper[12] |
        relax_dependency_1[14] | relax_dependency_2[15] | index[16:31];
```

The client `w[4] = (1<<0) | (2<<1)` (is_64b=1, type=2 WRITE_VALUE, index 0, deps 0, next NULL) is correct and unchanged. Slot routing is unchanged: `requirements == 0 → JS0` (irqfix), and the pattern SUBMIT keeps `req=0`.

### 1.5 The fix

Keep the 56 B shape, change only the payload semantics:

```
w[8..13] = {addr_lo, addr_hi, 3, 0, 0, 0}   # type=3 Zero, immediate ignored
```

and pre-fill the whole result page with `0xA5A5A5A5` from userspace before SUBMIT. Truth table after fence-wait:

| user view | kernel view | verdict |
|---|---|---|
| 0x00000000 | 0x00000000 | **PASS — GPU wrote the page** |
| 0xA5A5A5A5 | 0x00000000 | visibility bug (stale CPU alias; descriptor + GPU OK) |
| 0xA5A5A5A5 | 0xA5A5A5A5 | GPU never wrote (encoding/slot); check dmesg `JS_STATUS`/MMU fault |
| other | — | partial write or fault; check dmesg |

This is strictly stronger than the old zero-vs-pattern check: the old test pre-zeroed the page, so "GPU wrote zero" and "GPU wrote nothing" were the *same* observation (`0x00000000`).

## 2. Kernel-side readback (DEBUG-ONLY, strip before cutover)

Guardrails (agreed with G31_Coherency before implementation):

1. **Reuse, not reimplementation:** the readback calls `p7580_gem_job_done_sync` (the exact per-page `dma_sync_single_for_cpu` loop `free_job` runs) and then reads one u32. No second maintenance implementation exists anywhere in the diff.
2. **Strictly DEBUG-ONLY:** fops nr `0xCF` only; the six-ioctl driver table (`p7580_drv_ioctls[]`) still has exactly 6 rows; abialign hunks (`+1,340` ioctl hunk, SYNC_FILE masks, cpin/cpout caps, abi_check placement) preserved byte-identical; the int19 build gates assert all of this (6f).
3. **Regen from e70769b** (committed coherency state) + host `make check` + `-fsyntax-only` + `apply --check`/dry-run.

### 2.1 Wire

```
userspace: IOWR(0xCF) 16 B {u32 handle, u32 offset, u32 value OUT, u32 pad}
kernel:    struct p7580_debug_read_bo (ioctl.c + syncobj.c mirrors, ABI-pinned 16 B)
```

Routing: `panfrost_syncobj_ioctl` (fops) → `p7580_syncobj_do` asize table (`0xCF → 16 B`) → `p7580_syncobj_dispatch` `case P7580_DEBUG_READ_BO_NR` → `panfrost_debug_read_bo` (ioctl.c). The core `drm_ioctl` path (`nr >= 0x40 && nr < 0xA0`) never sees it; the driver table never lists it. `0xCF` sits above the syncobj `0xBF–0xC5` block: no collision with either table.

### 2.2 Read path (ioctl.c + gem.c)

```c
/* ioctl.c */
int panfrost_debug_read_bo(dev, data, file_priv) {
        if (args->pad) return -EINVAL;
        obj = drm_gem_object_lookup(dev, file_priv, args->handle);
        if (!obj) return -ENOENT;
        ret = p7580_gem_debug_read_word(obj, args->offset, &v);
        ...
        args->value = v; return 0;
}
/* gem.c */
int p7580_gem_debug_read_word(obj, offset, out) {
        if (!obj || !out) return -EINVAL;
        if (offset + 4 > obj->size) return -EINVAL;
        p7580_gem_job_done_sync(obj);          /* <-- the reuse */
        idx = offset >> PAGE_SHIFT;
        if (idx >= bo->nr_pages || !bo->pages[idx]) return -ENOENT;
        kv = kmap_atomic(bo->pages[idx]);      /* single-arg on !HIGHMEM arm64 */
        w = *(u32 *)((char *)kv + (offset & 4095));
        kunmap_atomic(kv);
        *out = w; return 0;
}
```

Same `dma_sync_for_cpu` the completion path runs, so the kernel view cannot reproduce the userspace-alias ambiguity by construction. No MMIO, no sleeps, no locks (safe in any ioctl context). CMA/imported-never-bound BOs follow `job_done_sync`'s no-op rules and fail `ENOENT` when unmapped.

### 2.3 Client wrapper (pf7580.c/h)

`pf7580_debug_read_bo(dev, handle, offset, &value)`: mock reads the shadow dword; real issues `DRM_IOWR(0xCF)`. Offset must be 4-aligned. The pattern test calls it once after the userspace re-read and prints both views side by side (`USER:` / `KERN:` lines).

## 3. Files changed (regen from e70769b)

| File | Change |
|---|---|
| `code/gpu31/client/pf7580-selftest.c` | pattern test → Zero-fill + `0xA5` prefill + dual-view compare + HINT table; header comment rewritten (`gpu31-gpuwrite`) |
| `code/gpu31/client/pf7580.c` / `.h` | `pf7580_debug_read_bo` wrapper + `0xCF` struct/encoding |
| `code/gpu31/gem/panfrost7580_gem.c` / `.h` | `p7580_gem_debug_read_word` (reuses `job_done_sync`, `kmap_atomic` read) + decl |
| `code/gpu31/gem/panfrost7580_ioctl.c` / `.h` | `panfrost_debug_read_bo` handler + struct + decl |
| `code/gpu31/gem/panfrost7580_syncobj.c` | `0xCF` nr define + payload mirror + dispatch case + asize route + ABI pins |
| `code/gpu31/driver/0001-*.patch` | regen via `/tmp/genp/gen.sh` (snapshots above) — 0002 untouched (reverted timestamp churn) |
| `code/gpu31/series/build-int19.sh` | new (copy of int18 + header note + `6f` gpuwrite gate: Zero contract, `0xCF` route, 6-row table, debug symbols in objects + `System.map` + Image) |

## 4. Verification (exercised)

- Host: `client make check` (mock suite incl. Mesa-exact ABI rows) **PASS**; cross `aarch64 -Werror` build **PASS**; `jobs/job_test` **PASS**; `gem/gem_validate` **PASS**; `driver/submit_fix_test` **PASS**.
- Kernel syntax (`aarch64-linux-gnu-gcc -fsyntax-only -Wall -Werror`, real 3.10.108 headers + staged 0001–0004 shims, `gnu89` + `KBUILD_MODNAME`): edited `panfrost7580_gem.c`, `panfrost7580_ioctl.c`, `panfrost7580_syncobj.c`, unedited `panfrost7580_job.c` **all PASS**.
- Patch: 0001 regen applies (`git apply --check` PASS on the repo tree; 0005/KMS/dmabuf FAILs are pre-existing tree-context artefacts, identical before this ticket); abialign-relevant lines (SYNC_FILE masks, cpin/cpout, abi_check, `+1,340`-area ioctl hunks) untouched — verified by diff inspection (only timestamp lines + the 5 debug hunks differ).
- Mock under qemu-aarch64 (`-L /usr/aarch64-linux-gnu`): `--print-abi` + full suite **PASS**.
- Device: int18 pattern-test re-run on the phone (pre-fix binary still on `/data/pf7580-submit`) reproduces the baseline exactly: `SUBMIT rc=0`, `WAIT rc=0`, `actual=0x00000000 MISMATCH` — the fence-signals-but-no-write signature §1.3 predicts. int19 (Zero + `0xCF`) build launched (`build-int19.sh`, background, logs on external disk); phone still on int18 at ticket close, so **on-silicon Zero/KERN proof is NOT yet run** — Main flashes int19 and runs:
  ```
  /data/wdarm 15 /tmp/pf7580-selftest-gw --pattern-test /dev/dri/renderD128
  # expect: USER word0=0x00000000 ZERO + KERN word0=0x00000000 ZERO → PASS
  ```
  (static device binary staged at `/tmp/pf7580-selftest-gw` on the build host; copy to the phone after flash.)

## 5. What Main must do (flash + run)

1. Wait for / rerun `build-int19.sh` to completion (gates: ops/slot/ran, ST-*/runexp, JS0 route, STAT|RAWSTAT demux, abialign, freshness, **6f gpuwrite**).
2. Flash `kernel-boot-int19.img`, reboot, confirm `renderD128` + `p7580_job: dispatch` trace.
3. Copy `/tmp/pf7580-selftest-gw` to the phone, run the pattern test above; capture `USER:`/`KERN:` lines + `dmesg` `JS_STATUS`/MMU-fault lines.
4. On PASS (both views zero): strip the DEBUG-ONLY surface per the cutover checklist above and close this ticket; on `kernel-zero + user-A5`: hand to G31_Coherency (visibility bug proven, descriptor exonerated); on `both-A5` + clean status: keep with G31_GpuWriteProof (GPU never wrote — slot/DMA-address hypothesis next).
