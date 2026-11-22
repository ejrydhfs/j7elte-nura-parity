# gpu31-shaderbo — fragment-shader ISA BO: identity, population, lifetime

Date: 2026-11-21. Owner: G31_ShaderBo. Device: int34 kernel
(`3.10.108 #37 SMP PREEMPT Sat Nov 21 10:12:53 EST 2026`), device Mesa 20.3.5
at `/data/mesabuild/mesa20` built into `/data/mesabuild/mesa20-b` (diag build).
No flash; all device work under `flock /tmp/gpsdev.lock`, short holds.

Scope: the fragment submit's `0x52 INSTR_TYPE_MISMATCH` whose `fault_pointer`
is the FS ISA address (`0x7032005` = 4 KiB BO + tag 5). This report resolves
that address, traces the BO lifecycle on silicon, and states exactly what is
proven and what still needs the next flash.

## 0. TL;DR

1. **Address resolution (proven, silicon).** `fault_pointer & ~0xFFF` is the
   fragment shader's ISA BO. It is a 4 KiB SHMEM `CREATE_BO` object, present in
   the submit as `ERR-MAP … 8=0x<fs>/0x1000` (handle 8), mapped into AS 0 by
   `p7580_drv_map_bo` (bo_list entry, `iova` = the fault address), and named by
   the fragment Renderer State: `meta FS shader=0x<fs>005 tag=5`. Nothing about
   the address chain is wrong.
2. **Population (proven, silicon).** Mesa writes the 96-byte ISA immediately
   after `panfrost_bo_create()` (`compile stage=4 … bytes=c87a9835 …`), and the
   write is read back from the same mapping right after the `memcpy`
   (`WATCH`/`CPFN` probe sees the bytes). So the BO *is* populated.
3. **The content is then replaced, wholesale, by zeros (proven, silicon).**
   A second marker written at BO offset 0x400 at compile time (`0xcafe1234`)
   is also gone at the first emit (`m256=00000000`), and both the user mapping
   and the kernel's own view (`0xCF` debug read of `bo->pages[0]`) agree after
   the BO is bound. So the whole page — not just the ISA words — is zeroed
   while the BO is live.
4. **The GPU reads exactly that page (proven, silicon).** Rewriting the 96-byte
   ISA into the mapping at every meta emit (`PAN_FBD_RECHECK`) makes the
   `0x52` disappear completely (`dmesg | grep -c 'p7580_job: ERR '` = 0 across
   a 5-frame run). The fault is therefore caused by the lost page content,
   not by a bad address, a bad tag, a bad meta, or an unmapped BO.
5. **The loss is not a cache-direction problem (proven).** It happens with no
   submit in the window (strace), no GEM_CLOSE / `bo FREE`, no `munmap`, no
   swap (`Swap: 0`, `total_swap_pages == 0`), no migration
   (`pgmigrate_success = 0`), and no BO-cache eviction. It is a
   page-reuse/zeroing signature: a whole-page `clear_highpage`-class event on a
   page the BO still references.
6. **Fix.** The driver's page pin (one `shmem_read_mapping_page()` reference per
   page, dropped in `p7580_shmem_free`) is the only thing keeping the page
   alive; the user mapping is `remap_pfn_range()/VM_PFNMAP` and contributes no
   reference. The fix is to make the **mapping itself** hold the page: a
   fault-driven `VM_MIXEDMAP` VMA with per-fault `vm_insert_page`/`get_page`
   and `put_page` in `close` (the `drm_gem_shmem_mmap()` shape), so page reuse
   is impossible regardless of which path drops the BO's own reference. A
   small diagnostic patch (0004) is specified to prove the freer on the next
   flash: page pin count and flags of the BO page at every shader submit.

## 1. Address resolution at the faulting fragment submit

From the clean-run dmesg of the failing client (`egltest --offscreen-only`,
int34):

```
p7580_job: ERR slot=0 js_status=0x00000052 (INSTR_TYPE_MISMATCH) seq=121
           jc=0xb135600 sw=… 00010013 …
p7580_job: JDBODY … 00000052 00000000 07032005 00000000 00010013 …
                            │        │
                            │        └ fault_pointer = 0x07032005
                            └ exception_status
```

`0x7032005` is `0x7032000 | 5`: a 4 KiB-aligned base with the Midgard
first-bundle tag in the low nibble — the shape of a `Renderer State.Shader`
address, not of a data pointer. Resolution through the submit set and the
driver's own state:

| Quantity | Value (this run) | Source |
|---|---|---|
| FS ISA BO IOVA | `0x7032000` (`0xc80e000` in the FBD run) | `fault_pointer & ~0xFFF`; `FBDDBG compile stage=4 bo=…` |
| Kernel object | SHMEM, 4 KiB, `CREATE_BO`, flags 0 | `FBDDBG bo create gpu=… size=4096 flags=0x1` (Mesa `PAN_BO_EXECUTE`) |
| GEM handle / submit set | `8` / `0xc80e000/0x1000` | `p7580_job: ERR-MAP … 8=0xc80e000/0x1000` |
| AS 0 | mapped, leaf present | `p7580_drv_map_bo()` bo_list scan; `p7580_iova: map` gate; PTEWALK owner lookup |
| Meta pointer | `Draw.state → Renderer State.Shader = 0x<c80e>005` | `FBDDBG meta FS … shader=0xc80e005 tag=5` |
| ISA content as read by the FS `Renderer State` | first 8 words | `FBDDBG isa FS …` |

So the fault address resolves, through `bo_list`, to the fragment shader's ISA
BO; the object is in the submit and in AS 0; and the shader meta points at it
with tag 5. The address chain is correct.

## 2. Lifecycle trace (silicon, int34, no kernel change)

Probes (device Mesa 20.3.5, diagnostic, all behind env vars):
`FBDDBG bo create/mmap/munmap/PUT/FETCH/FREE` in `pan_bo.c`,
`WATCH`/`WCHK` (first 16 BO bytes) at `bo_mmap-post`/`bo_create`/`submit-*`,
`FBDDBG compile` (compile-time bytes from the compiler buffer),
`FBDDBG fsbo/vsbo` (meta emit, bytes read from the BO mapping),
`FBDDBG census` (page walk), `FBDDBG magic` (write at 0x400 + kernel read via
the driver's `0xCF` debug ioctl), `FBDDBG magic2` (a **second, independent
`mmap()` of the same BO**), `FBDDBG recheck` (rewrite the ISA, then user+kernel
read-back).

Ordered trace of one failing run (`frames=1`; `[..]` = truncated):

```
FBDDBG bo mmap gpu=0x21424000 size=4096 cpu=0x7fb6532000 first=00000000 off=0x11f424000
FBDDBG bo create gpu=0x21424000 size=4096 flags=0x1 cpu=0x7fb6532000     <- FS BO (handle 8)
FBDDBG compile stage=4 size=96 tag=5 bo=0x21424000 bytes=c87a9835 01403d3d 00000030 …
FBDDBG bo mmap gpu=0x21425000 size=4096 cpu=0x7fb64ff000 first=00000000 off=0x11f425000
FBDDBG WATCH bo_create: shader bo=0x21424000 changed c87a9835 01403d3d -> 00000000 00000000
FBDDBG bo create gpu=0x21425000 size=4096 flags=0x1 cpu=0x7fb64ff000     <- VS BO (handle 9)
FBDDBG compile stage=0 size=96 tag=5 bo=0x21425000 bytes=c87a9495 …
…
FBDDBG vsbo gpu=0x21425000 size=4096 isa=c87a9495 08003c3d …
FBDDBG fsbo gpu=0x21424000 size=4096 isa=00000000 00000000 … m256=00000000
```

Facts established by this trace and its controls:

* **The write lands.** `compile` is printed from the compiler buffer, and the
  `WATCH` baseline is taken from `bo->ptr.cpu` immediately after the `memcpy`;
  it contains `c87a9835 01403d3d`. A `CPFN` probe reading `bo->ptr.cpu` right
  after the `memcpy` also returns the ISA words.
* **Nothing frees or unmaps the BO.** Across the window there is no
  `FBDDBG bo FREE` (GEM_CLOSE), no `bo PUT` of the FS object, no
  `bo munmap gpu=0x21424000`, and no `SUBMIT` (verified independently with
  `strace -f -tt -e trace=mmap,munmap,ioctl`: between the FS BO's `mmap` and
  the VS BO's `mmap` there are only anonymous `mmap/munmap` pairs and the two
  `CREATE_BO`/`MMAP_BO` ioctls).
* **Fine-grained placement.** With `watch_check()` inserted at
  `post-create-ioctl` (inside `panfrost_bo_alloc`) and `post-mmap-ioctl`
  (inside `panfrost_bo_mmap`), the transition is observed inside the *next*
  BO's creation: at `post-create-ioctl` in most runs, at `bo_create` (i.e.
  after `os_mmap`) in others. That variability is the signature of an
  asynchronous agent (page allocator) rather than of a single syscall.
* **The whole page is zeroed, not just the ISA.** A compile-time marker at
  offset 0x400 (`0xcafe1234`, well outside the 96-byte ISA) is `00000000` at
  the first emit (`m256=00000000`). `clear_highpage()` on a freshly allocated
  page is the only in-tree operation that produces this shape.
* **It is the BO's own page.** `magic2` takes a second, independent `mmap()`
  of the *same* BO (same fake offset → same `drm_gem_object` → same
  `bo->pages[]`) and reads the same contents as the first mapping
  (`w0=00000000 w256=5a5aa5a5` after the probe's own write). After the BO is
  bound, the driver's kernel-side `0xCF` read (`kmap_atomic(bo->pages[0])`
  after `job_done_sync`) returns the probe's write too (`kern=5a5aa5a5`).
  So the user mapping, the kernel's page array, and the GPU's PTE all name the
  same page, and that page lost its content.
* **No reclaim mechanism is available.** `/proc/swaps` is empty and
  `Swap: 0` (`shmem_writepage()` cannot swap; it redirties), and
  `pgmigrate_success = 0` (no migration; with the driver's page reference
  present, `migrate_page_move_mapping()`'s `page_count == 2` test would fail
  anyway). The page is not *supposed* to be free.
* **The GPU reads this page.** With `PAN_FBD_RECHECK` (rewrite the saved ISA
  into the mapping at every meta emit) the `0x52` vanishes: 0
  `p7580_job: ERR` lines over a 5-frame run, and the recheck probe shows
  `u=c87a9835 … k=c87a9835 …` (user and kernel) right after the rewrite.

## 3. Reading of the evidence

The BO is populated, is in the submit, is mapped in AS 0, and its page is the
one the GPU fetches. The page's bytes are nevertheless replaced by zeros
between the compile and the first emit, with no submit, no BO close, no
unmap, no swap, no migration. The only kernel operation with that effect is
the page allocator zeroing a page it believes is free
(`clear_highpage()`/`__GFP_ZERO`).

That means the page the BO points at is being handed out again while the BO
still holds it. The driver's page reference comes from exactly one
`shmem_read_mapping_page()` per page (`p7580_shmem_alloc`, F-GEM4 form) and is
dropped once in `p7580_shmem_free`; the user mapping is
`remap_pfn_range(vma, …, page_to_pfn(bo->pages[i]), …, pgprot_writecombine())`
with `VM_PFNMAP`, which contributes **no reference of its own**. So the page
lives on a single reference, held by the BO object — and the observations show
that this single reference is, in this configuration, not sufficient to keep
the page out of the allocator (the freer is asynchronous and not on any
syscall the strace sees; the driver itself performs no `put_page` in the
window).

`[INFERENCE]` The concrete freeing path (shmem inode eviction vs. a page-cache
removal from another user) cannot be decided from userspace: the device has no
`/proc/kcore`, no `/dev/mem` (`CONFIG_DEVMEM` off), and `/proc/<pid>/pagemap`
returns no PFN for `VM_PFNMAP` VMAs. The instrumented patch in §5 answers it in
one run.

## 4. What this rules out (all on silicon, this ticket)

| Candidate | Verdict |
|---|---|
| Wrong BO / address reuse / not in the submit | No — `ERR-MAP` handle 8 + meta `shader=` + `bo_list` resolve to this BO |
| BO never written | No — read-back of the mapping right after `memcpy` shows the ISA |
| Wrong tag / meta packing | No — tag 5 identical in the compiled binary and the meta; rewrite at emit fixes the fault |
| Cache-direction (cachedir) | No — save/restore a CPU write across a submit already passed (`gpu31-cachedir`); the loss here happens with no submit in the window |
| Swap / migration / reclaim | No — no swap, `pgmigrate_success = 0` |
| BO close / BO-cache eviction | No — no `GEM_CLOSE` (`FBDDBG bo FREE`) in the window |
| VMA pointing at another page | No — second independent mmap and the kernel `0xCF` read agree |
| Lost *write* only (partial page) | No — a marker at 0x400 is gone too; the whole page is zeroed |

## 5. Fix and the one measurement that closes the case

### 5.1 Diagnostic patch 0004 (needs the next flash; text below)

Add to the driver's `0xCF` `p7580_gem_debug_read_word()` and to
`p7580_drv_submit()` (per BO, once per submit, budget 8 lines) a page ledger:

```c
/* p7580_gem_debug_read_word(): before kmap */
pr_info("p7580_pin: obj=%lu iova=0x%llx idx=%u count=%d mapcount=%d %s%s%s%s\n",
        (unsigned long)obj, (unsigned long long)bo->iova, idx,
        page_count(bo->pages[idx]), page_mapcount(bo->pages[idx]),
        PageUptodate(bo->pages[idx]) ? "up " : "!up ",
        PageLRU(bo->pages[idx]) ? "lru " : "!lru ",
        PageSwapBacked(bo->pages[idx]) ? "swapb " : "",
        PageDirty(bo->pages[idx]) ? "dirty" : "clean");
/* p7580_drv_submit(): after p7580_gem_submit_sync(obj), first page only */
if (p7580_bo_npages(obj) && to_p7580_bo(obj)->pages)
        pr_info("p7580_pin: sub obj=%lu iova=0x%llx count=%d w=%08x %08x %08x %08x\n",
                (unsigned long)obj, (unsigned long long)to_p7580_bo(obj)->iova,
                page_count(to_p7580_bo(obj)->pages[0]),
                ...first four words via kmap...);
```

If `count` for the shader BO falls to the page-cache count (1) or the page is
seen `!up`/`!lru`/`swapb` and gets zeroed, the freer is a page-cache removal,
not a BO reference drop. If `count` is 2 and the page is still zeroed, the
reference is being lost in the shmem layer (inode eviction path), which the
same line identifies by `!up`/`!lru`.

### 5.2 The fix (driver, needs flash)

**Status: implemented, `docs/reports/gpu31-mmaprefs.md` (2026-11-21).**
Exactly this shape is now in the driver (`VM_MIXEDMAP` + `vm_insert_page`
in the fault handler, `put_page` when the last VMA closes, WC prot and
the clean-then-invalidate discipline untouched), plus the §5.1 ledger as
`p7580_gem_debug_page_ledger()` at the `0xCF` read and at SUBMIT, and a
mapping-lifetime client test (`--mmaprefs-test`). It still needs the
flash: the silicon acceptance below is the one to run, and
`gpu31-mmaprefs.md` §6 has the exact command lines and the ledger values
to expect.

Make the user mapping hold the pages itself, so the CPU-written page cannot be
recycled even if the BO's own reference is lost, and so the mapping is
coherent with the kernel's view:

* map BOs with **`VM_MIXEDMAP`** (`vma->vm_flags |= VM_MIXEDMAP` in
  `p7580_gem_mmap`, `vma->vm_ops->fault` installed, no `remap_pfn_range` for
  the SHMEM path);
* in `p7580_gem_fault()`, resolve `bo->pages[page_offset]`, `get_page()` it and
  `vm_insert_page(vma, addr, page)` (or `vmf_insert_mixed` with the page's
  pfn *and* a `get_page()`), so every PTE owns a reference;
* in the VMA `close` hook, drop the per-PTE references (keep the
  `bo->pages[i]` array as the BO's own reference, unchanged);
* keep the existing clean-then-invalidate discipline: the VMA keeps the
  `pgprot_writecombine` (NC) attribute, and submit still cleans the pages to
  the point of coherency before the GPU's PTE is used.

That is the `drm_gem_shmem_mmap()`/`drm_gem_shmem_fault()` shape the port
documents as the v5.2 reference; the current `remap_pfn_range`/`VM_PFNMAP`
shape is the only part of the GEM port that does not hold per-mapping page
references, and it is exactly the page this ticket shows being recycled.

Silicon acceptance for the next flash (no Mesa change, no `RECHECK`):

```
rootshell /bin/sh -c 'dmesg -c >/dev/null'
/data/egltest --offscreen-only --frames 5     # or /data/eglrun.sh
sleep 6
dmesg | grep -E 'p7580_job: ERR |p7580_pin: sub'
# expect: no ERR lines; p7580_pin shows count >= 3 for the shader BO pages,
#         and the ISA words c87a9835 01403d3d … at the FS submit
```

The `RECHECK` experiment already proves the target state on silicon: with the
ISA present at submit, the fragment `0x52` is gone. The remaining pixel gap
(triangle absent) is the empty polygon list of `gpu31-fbdtarget.md` §5.3 and is
independent of this ticket.

## 6. Reproduce (device, int34, current state)

```
# Mesa probes are in /data/mesabuild/mesa20 (diag), built into mesa20-b
B=/data/mesabuild/mesa20-b
export LD_LIBRARY_PATH=$B/src/egl:$B/src/mapi/es2api:$B/src/gbm:$B/src/gallium/targets/dri
export LIBGL_DRIVERS_PATH=$B/src/gallium/targets/dri
export MESA_LOADER_DRIVER_OVERRIDE=panfrost EGL_PLATFORM=surfaceless

PAN_FBD_DEBUG=1 /data/egltest --offscreen-only --frames 1        # zeroing
PAN_FBD_DEBUG=1 PAN_FBD_MAGIC=1 …                                # 0x400 marker + kernel read
PAN_FBD_DEBUG=1 PAN_FBD_RECHECK=1 …                              # ISA rewritten; 0 ERR
```

`strace -f -tt -e trace=mmap,munmap,ioctl -o /tmp/tr1.log /data/egltest
--offscreen-only --frames 1` reproduces the no-submit window (FS mmap →
compile → VS mmap).

## 7. Files / state

- This report only *at the time of the trace*. The fix it specifies is now
  committed as `gpu31-mmaprefs` (driver gem mmap/fault/close + the §5.1
  ledger + the `--mmaprefs-test` client mode), still unflashed: see
  `docs/reports/gpu31-mmaprefs.md`. The device tree
  `/data/mesabuild/mesa20` currently carries the diagnostic probes (all
  env-gated; behaviour unchanged when the variables are unset).
  `pan_bo.c` / `pan_cmdstream.c` / `pan_assemble.c` there are
  diagnostic-only, and the `PAN_FBD_RECHECK` rewrite is no longer needed
  once the driver fix is flashed (compare with it off).
- The `0xCF` debug ioctl, `p7580_drv_submit`'s `ERR-MAP`, and the fault-time
  `PTEWALK`/`JDBODY` are the kernel instruments this trace used; none of them
  needed a rebuild. The `p7580_pin:` ledger lines named in §5.1 exist after
  the `gpu31-mmaprefs` flash.
