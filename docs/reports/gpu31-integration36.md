# gpu31-integration36 — mmaprefs (fault-driven VM_MIXEDMAP + real page references, mapping pin, ledger probes), all prior gates intact

Date: 2026-11-21. Owner: G31_Integrator36. Phone: on int35
(Mesa patched); this ticket builds only — Main flashes. No flash
per ticket.

Consumes gpu31-mmaprefs (gpu31-mmaprefs.md; committed as
**de21448**) on top of int35 (5c386dd cachedir): local SHMEM mmap
becomes fault-driven VM_MIXEDMAP (clear VM_PFNMAP, set
VM_MIXEDMAP|VM_DONTCOPY), `p7580_gem_fault` ->
`vm_insert_page(bo->pages[off])` so every PTE owns a reference; the
mapping pins one reference per BO page on the 0->1 VMA (mmap +
`vm_ops->open`) and drops them on the 1->0 close (`put_page`), both
under `dev->struct_mutex`; CMA (`dma_mmap_attrs`) and PRIME-imported
(`remap_pfn_range`) keep their shapes with `vm_insert_pfn` on their
unreachable fault path; free asserts `vma_refs`/`map_pinned` zero
and drops a leaked pin; closure probes `p7580_gem_debug_page_ledger`
at the 0xCF read (`"0xcf"`) and per BO at SUBMIT (`"sub"`, budget 8).
No source changes beyond the int36 script (copy of int35 + renamed
names + new 6v gates + driver-0001 provenance) and this report.

Source note: the ticket named fc96b15 (the mmaprefs commit as first
pushed). While int36 was being prepared the mmaprefs owner amended
that same commit to **de21448**, adding `mapping=%p pgidx=%lu` to
the ledger line (page-cache identity in the same probe). int36 was
built from, and gates, the amended state; the 6v section records the
exact driver-0001 sha256 and fails if that file moves while the
build runs, so this report cannot silently describe a different
tree.

## 1. Build

Script: `code/gpu31/series/build-int36.sh` (committed). Fresh `cp -a`
of the pristine tree → `scratch-int36` (birth 20:41:08, tree mtime
20:45:27), all patches `--check` green (backport 0001–0005 ×5,
Mode-B ×1, dmabuf ×1, driver 0001+0002 — driver 0001 is de21448's
regenerated source), `-j4` Image build, RC=0.

Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
KCFLAGS="-Wno-error" -j4 Image`.
Log: `…/j7k/gpu31/logs/gpu31-int36-build.log` (5393 lines, 0
`error:`, ends `OBJCOPY arch/arm64/boot/Image`; the only link
warnings are the ambient `vmlinux has a LOAD segment with RWX
permissions` ones, 3×, same as int33/int34/int35).
Pack: stock dt.img + reference ramdisk-new.gz (never rebuilt),
27332608 B = kernel 18193592 + ramdisk 8822392 + dt 313344
(+3280 header), built by `stage-int36/mkboot.py`.
Console: build stdout (RC=0, 19 PASS notes, 0 FAIL); evidence:
`…/logs/gpu31-int36-evidence.log` (465 lines).
Scratch + stage deleted after the pack; kept: `Image-int36`,
`kernel-boot-int36.img`, both logs, `offstage-int36`.

Source provenance (printed into the evidence log):
repo HEAD `de21448`; driver 0001 sha256
`ec58385a349f56282cfb97a1f9f584f146e2b39cc2ad365a4ea0ed93764ca3fa`.

Freshness (evidence tail): scratch birth/mtime as above; the
pristine tree has no `drivers/gpu/drm/drm_sched.c` and no
`drivers/gpu/drm/panfrost/` directory (the shim and the driver exist
only because the patches created them).

## 2. Gate evidence (computed inside the build, not inferred)

Prior fixes intact — the 18 int35 PASS banners are unchanged: ops
layout **ops=24 slot=232 ran=236**, single defs, numeric ST-OPS/
`runexp`, ST-RUN/bails/retire/finish, weak/strong `run_job_addr`;
IRQ (`(status|rawstat)` demux, rearm + AS0 program on
recover/resume); slot routing (FS→JS0, req0→JS1,
ONLY_COMPUTE→JS2); ABI-align (BUILD_BUG_ON pins, SYNC_FILE
tolerance, copy caps); gpuwrite (Zero contract, `0xCF` fops route,
`job_done_sync` reuse, 6-row table); coherency (3 pre-signal call
sites, `bl` in IRQ/timedout/recover, post-signal free_job walk);
errtrace (`ERR`/`TIMEOUT`/`trace_arm`/`dump`, `Image p7580_job:
ERR = 2` — this gate reports through its evidence lines and has no
PASS banner, as in int35); instrtype (`dma_resv_add_excl_fence` in
run_job, shim-owned lock); iova (free-time unmap before
`iova_free`, invalidate-map-invalidate); armfence (alloc at arm,
SUBMIT out_sync install, identity pump); ptewalk (snap/live owner,
unconditional 8-word dump); pmget (5 sites, 1 inline `< 0`);
timedout probes (`TMD-` count = 3); submitsnap (`desc[8]`/`sw[8]`,
`sw=` count = 5); syncobj-wait (WAIT_ALL re-test, writable
signaled-ops); jddecode (`bo_count` stamp, JDBODY count = 3);
multidep (AND node, staged errno, no `EOPNOTSUPP`); serialfence
(`serial_prev` chain, 3 `add_dependency` relocs, publish/rollback/
fini); cachedir (submit clean-only, unbind clean-before-unmap,
snapshot/JDBODY/pre-signal routed).

This ticket's new 6v mmaprefs gates (PASS):

- Tree: `p7580_gem_fault`, `p7580_gem_mmap`,
  `p7580_gem_debug_page_ledger` defs + decls, `extern const struct
  vm_operations_struct p7580_gem_vm_ops;`, `vma_refs`/`map_pinned`
  fields; `p7580_gem_vm_ops` initialises **exactly 3** members
  (`.fault = p7580_gem_fault`, `.open = p7580_gem_vm_open`,
  `.close = p7580_gem_vm_close`), published from main.c
  (`.gem_vm_ops = &p7580_gem_vm_ops,`).
- Fault path: `p7580_map_page` keeps `return vm_insert_page(vma,
  f_vaddr, page);` for SHMEM and `return vm_insert_pfn(vma,
  f_vaddr, pfn);` for CMA/imported; `p7580_gem_fault` routes through
  it, takes **no lock**, and `p7580_fault_err` still folds
  `-EBUSY` into the retry arm.
- mmap: `vma->vm_flags &= ~VM_PFNMAP;` then `vma->vm_flags |=
  VM_MIXEDMAP | VM_DONTCOPY;` then `mutex_lock(&obj->dev->
  struct_mutex)` then `p7580_gem_vm_get(bo)` — checked as a strict
  line-order chain (flags 1280 < set 1281 < lock 1282 < get 1283 in
  the applied file); `dma_mmap_attrs(obj->dev->dev, vma` and
  `remap_pfn_range(vma, vma->vm_start + off, pfn,` still present
  (CMA / imported legs untouched).
- Pin accounting: `p7580_gem_map_pin` `get_page(bo->pages[i])` per
  page + `map_pinned = 1`; `p7580_gem_map_unpin` `put_page
  (bo->pages[i])`; `vm_get` pins on `bo->vma_refs++ == 0`, `vm_put`
  unpins on `--bo->vma_refs == 0` and keeps the `WARN_ON_ONCE(1)`
  unmatched-close guard; vm_open/vm_close take `struct_mutex`,
  account, then delegate to `drm_gem_vm_open/close`.
- Free: `WARN_ON_ONCE(bo->vma_refs != 0)`,
  `WARN_ON_ONCE(bo->map_pinned)` and the leaked-pin
  `p7580_gem_map_unpin(bo)` drop are present.
- Ledger: format string `p7580_pin: %s obj=%lu iova=0x%llx idx=%u
  count=%d mapcount=%d vma=%u pin=%u …` present; `"0xcf"` call at
  gem.c:343 after `p7580_gem_job_done_sync(obj)`; `"sub"` call at
  submit.c:1293 inside `if (pin_budget)`, `pin_budget = 8`
  (submit.c:1200).
- Tree-computed vm_flags from `include/linux/mm.h`: PFNMAP=0x400,
  clear mask `0xfffffffffffffbff`, MIXEDMAP|DONTCOPY=`0x10020000`
  (values pinned numerically, so a header change fails the gate
  instead of mis-decoding the disasm).
- gem.o: `T p7580_gem_fault` (:0x1f0), `T p7580_gem_mmap` (:0x0),
  `T p7580_gem_debug_page_ledger` (:0x900), `t p7580_gem_vm_close`
  (:0x5e0), `t p7580_gem_vm_open` (:0x70c), `t p7580_gem_map_pin`
  (:0x370), `R p7580_gem_vm_ops` (48-byte object at .rodata+0).
- `.rodata` relocations are exactly three `R_AARCH64_ABS64`:
  `+0 p7580_gem_vm_open`, `+8 p7580_gem_vm_close`,
  `+16 p7580_gem_fault` — the core's struct order, wired to the
  wrappers (no raw `drm_gem_vm_open/close` in the table).
- Fault object code: exactly 1 `vm_insert_pfn` call (:0x248) and 1
  `vm_insert_page` call (:0x338); mmap carries **no**
  `vm_insert_page`, exactly 1 `remap_pfn_range`, 1 `drm_gem_mmap`
  and 1 `p7580_gem_map_pin` (:0x22c). The compiled switch is
  `mov x1, #0x10020000` (:0x1f8) + `and x0, x0,
  #0xfffffffffffffbff` (:0x200) + `orr`, all before the pin call
  (gate checks and < orr < pin).
- vm_open: `drm_gem_vm_open` (:0x72c) + `p7580_gem_map_pin`
  (:0x794); vm_close: `drm_gem_vm_close` (:0x600) + `put_page`
  (:0x694) + `warn_slowpath_null` (:0x6f0). free_object carries a
  `put_page` (:0x1710) and its WARN paths (5 warn calls).
- gem.o undefined refs: `vm_insert_page`, `vm_insert_pfn`,
  `put_page`; main.o `U p7580_gem_vm_ops`.
- submit.o: exactly **1** `p7580_gem_debug_page_ledger` call in the
  whole object, inside `p7580_drv_submit` (:0x1958), `U` ref, and
  the `"sub"` string present.
- 0xCF read: `p7580_gem_debug_read_word` calls
  `p7580_gem_invalidate_for_cpu` (:0xb5c) **before** the ledger
  (:0xb90).
- gem.o strings: the `p7580_pin:` format and
  `p7580_gem_mmap: ok mixedmap …`; the 0xCF tag `0xcf`.
- Linked kernel: `T p7580_gem_fault` (`ffffffc00057c2f0`), `T
  p7580_gem_mmap` (`ffffffc000ad4de8`), `T
  p7580_gem_debug_page_ledger` (`ffffffc00057ca00`), `R
  p7580_gem_vm_ops` (`ffffffc000b51140`), one each in System.map.
- Image: `p7580_pin: %s obj=%lu iova=0x%llx` and
  `p7580_gem_mmap: ok mixedmap` present.

Hunk integrity (PASS): driver 0001 submit.c fully applied — late
function `int panfrost_ioctl_submit_override` present in the applied
tree, `T` in submit.o, one `T` in System.map
(`ffffffc00057bf48`).

## 3. Outputs (sha256)

- `Image-int36`:
  `c34dbec5e969104a5412aff821cf0a0ddb1cd996935ca197cac34010c7bbe3b7`
- `kernel-boot-int36.img`:
  `2ba4475b2dbc8493e1bb5241abcf66d0a4f16a82a49ac87b66f4cc16c1d91d27`
- build log:
  `80726211fe30a0a034823c196256b550137a6d70b50b07c37f6860f765ab502f`
- evidence log:
  `ff07101da3973c1dd7cdedb1eba292e05ec13201a460ed15ea1d23d1a8ac1fe1`

Config: pmaports `config-samsung-j7elte.aarch64` base + d0-m1-minimal
+ d4-panfrost7580 fragments, then `olddefconfig` closure. No flash
per ticket (Main flashes).

## 4. Silicon acceptance (Main, after flashing int36)

Per gpu31-mmaprefs.md §6: 5 Mesa frames with PAN_FBD_RECHECK off
must show zero `p7580_job: ERR ` and `p7580_pin: sub …`
count>=3/mapcount>=1/up with `w=c87a9835 01403d3d` for the FS ISA BO;
then `/data/pf7580-selftest --mmaprefs-test /dev/dri/renderD128`
PASS. A still-lost page is classified by the ledger's `mapping=`/
`pgidx=` fields plus a DEBUG_READ_BO at 0x400 (mmaprefs.md §3/§6).

## 5. Script notes

- The 6v gates were pinned against the real -O2 codegen before the
  full run: a probe tree (patches + config + only
  `panfrost7580_{gem,submit,main}.o`, ~3 s) showed the fault
  outlines `p7580_map_page` into `p7580_gem_fault`, that `map_pin`
  and the pin wrappers lose their symbols to inlining, and that the
  compiler materialises the OR immediate in a separate `mov` placed
  *before* the `and` — so the flag-order gate tracks `and` -> the
  `orr` that consumes that `mov` -> the pin call, instead of assuming
  instruction adjacency. The full build then passed on the first
  run; no in-flight gate fix was needed post-build (probe scratch +
  harness deleted).
- A driver-0001 provenance gate (sha256 recorded before `git apply`,
  re-checked in 6v) was added because the mmaprefs commit was
  amended mid-preparation (fc96b15 -> de21448). It fails the build
  if the patch file moves while the build runs, which keeps this
  report tied to the exact source it describes.
