# DECON KMS memory budget — Plasma Mobile 6.8 on 1.4 GB

Slice 9 of `decon-kms-plan.md`. Scope: measure the **current** baseline on the J7,
estimate what Plasma Mobile 6.8 needs, specify zram/swap/allocator/session
configuration, and define the memory gate for M4. Kernel tree untouched; every
device interaction was read-only (`flock /tmp/gpsdev.lock`, no reboot, no flash,
no sysctl/swap/zram writes). Measurement tool: `code/gpu31/kms/tools/memwatch.sh`.

Device at measurement time: `nura-j7elte`, Linux 3.10.108 #37 SMP PREEMPT,
Nura edge (Alpine, musl 1.2.6), 8× Cortex-A53, 720×1280 panel,
X11 `nura-shell` on `decon_fb`, no KMS node, no swap.

---

## 1. Bottom line

| Item | Value |
|---|---|
| Usable RAM (`MemTotal`) | 1,407,564 kB = **1,374.6 MiB** |
| Measured idle free+reclaimable (`mem_avail_calc`, §3.1) | **1,183–1,206 MiB** |
| Current UI cost (Xorg + nura-shell) | 42.9 MiB RSS |
| Current non-UI daemons | 94.6 MiB RSS |
| Kernel-side (slab stacks tables vmalloc) | ≈ 90–110 MiB, ~46 MiB of it dma-buf |
| Plasma session components (kwin + plasmashell + kded6 + kactivitymanagerd + portals, no apps) | **270–450 MiB** [INFERENCE] |
| Boot-to-home-screen total (session + 95 MiB daemons + 100 MiB kernel/ion) | **≈ 530 MiB typical, 645 MiB worst** [INFERENCE] |
| zram: algorithm / size | **lz4** / **687 MiB** uncompressed (50 % of RAM) |
| zram measured compression on this device (X11 anon pages) | 22.4:1 weighted, 5.2:1 on the most mixed region (§3.2) |
| swappiness / page-cluster / min_free_kbytes | **180 / 0 / 100000 kB** (distro script does this) |
| `MALLOC_ARENA_MAX` | **no effect on this image** — musl, not glibc (§5.4) |
| Hard gate before any Plasma measurement | `kwin_wayland` does not load (§2.1) |
| Session gate | §7 |

Two findings change the plan's assumption:

1. **Plasma cannot start at all today**: `kwin_wayland` fails to relocate against
   Qt 6.12 (`libkwin.so.6.8.3-r0` was built against Qt 6.8). Memory measurement of a
   live Plasma session is therefore impossible until that is fixed — it belongs to the
   `PlasmaSession` slice (which pins `kwin=6.8.3-r1`), recorded here because it gates
   M4.
2. **`MALLOC_ARENA_MAX` is inert**: the image is musl-based; the knob is a glibc
   malloc tunable. The plan's "zram + `MALLOC_ARENA_MAX`" becomes "zram + no glibc
   arenas at all".
3. **zswap cannot be turned off at runtime** (`enabled` is a read-only module
   parameter, `mm/zswap.c:84-85`). It must be disabled on the kernel command line, or
   starved with `max_pool_percent=0` before zram comes up (§5.2).

---

## 2. What is running now (measured)

### 2.1 Pre-flight: the Plasma stack's loadability on this image

`kwin_wayland --version` (6.8.3-r0 as installed at measurement time):

```
Error relocating /lib/libkwin.so.6:
  _ZN22QWindowSystemInterface36handleTabletEnterLeaveProximityEventE...: symbol not found
Error relocating /lib/libkwin.so.6:
  _ZN22QWindowSystemInterface17handleTabletEventE...dddddi...: symbol not found
```

Cause: `libQt6Gui.so.6` is **6.12.0** while `libkwin.so.6` is **6.8.3**, and
`QWindowSystemInterface` is Qt *private* API — the 6.12 signature gained one
`double` (five `d` where kwin asks for four plus an `int`), so the symbol is gone:

```
$ nm -D /lib/libQt6Gui.so.6 | grep -c handleTabletEvent          -> 4
   ...handleTabletEvent(QWindow*, ulong, QPointingDevice const*, QPointF, QPointF,
      QFlags<MouseButton>, d d d d d, QFlags<KeyboardModifier>)   # Qt 6.12
kwin wants the same with d d d d i                               # Qt 6.8
```

| Binary | Result |
|---|---|
| `plasmashell`, `kded6`, `ksmserver`, `plasma-mobile-envmanager`, `plasma_waitforname` | load (fail only for lack of a display) |
| `kwin_wayland` (links `libkwin.so.6`) | **relocation failure — cannot run** |
| `plasma_session` | does **not** link `libkwin` (`ldd` has no libkwin) — my probe's error text came from the `kwin_wayland_wrapper` child it execs, so `plasma_session --version` is **not** a valid health check; use `kwin_wayland --version` |

Fix status: the `PlasmaSession` slice pins `kwin=6.8.3-r1`, built against Qt 6.12
(verified from the APK); the installed package at measurement time was **6.8.3-r0**.
Until r1 is installed the only measurable session is the X11 one below.

### 2.2 Memory map of the idle phone (first snapshot, before any host-side scanning)

```
MemTotal       1407564   Buffers          12684   Slab            42472
MemFree        1045920   Cached          167464   SReclaimable    15452
SwapTotal            0   Active           92472   SUnreclaim      27020
AnonPages        59704   Inactive        147456   KernelStack      3312
Mapped           42708   Active(anon)     61340   PageTables       1652
Shmem             2384   Inactive(anon)     812   VmallocUsed     56892
                         Active(file)     31132   CommitLimit    703780
                         Inactive(file)  146644   Committed_AS   494020
```

`min_free_kbytes=4569`, `swappiness=60`, `overcommit_memory=0`,
`vfs_cache_pressure=100`, `dirty_ratio=0` (Android-style writeback),
`page-cluster=3`, `extra_free_kbytes=0`, 8 CPUs online.

`free -m` (procps-ng 4.0.7) reports `used 353, free 1021, shared 2,
buff/cache 191, available 1021`. That `available` is **wrong on this kernel**:
procps falls back to `MemFree` when `/proc/meminfo` has no `MemAvailable`
(`library/meminfo.c:727-728`: `if (0 == mHr(MemAvailable)) mHr(MemAvailable) =
mHr(MemFree);`), so it ignores 167 MiB of cache plus 15 MiB of reclaimable slab and
understates what the session may use by ~180 MiB. Use `mem_avail_calc` instead.

### 2.3 Userspace baseline

`ps -eo rss` sum = **139,700 kB (136.4 MiB)**, of which the UI is only 42.9 MiB:

| Process | RSS | VmData | VmExe | Notes |
|---|---|---|---|---|
| `Xorg :0` (`99-fbdev.conf`, shadow) | 34,488 kB | 13,400 | 1,868 | anon 8,328 / file 26,200, PSS 33,830 |
| `nura-shell` | 8,368 kB | 4,904 | 12 | VmHWM 8,364 — flat, no growth |
| `sleep-inhibitor` (python) | 19,440 kB | | | elogind helper |
| `NetworkManager` | 9,924 kB | | | |
| `logbookd` | 9,044 kB | | | logs to `/var/log/logbookd.db` |
| `wpa_supplicant` ×2 | 8,916 kB | | | one per interface |
| `gnome-keyring-daemon` ×4 | 8,124 kB | | | **four instances** (2 MiB each) — duplicate daemons |
| daemons, gettys, sshd | rest | | | |

Both UI processes have `VmHWM == VmRSS`: no leak in the current shell.

`fbwin` (`code/experience/fbwin.c`) is not part of the session — it is a DECON
window-config probe: it allocates one `720*1280*4 = 3,686,400 B` ion buffer, fills it
and commits it as window 0 through `S3CFB_WIN_CONFIG`. It was **not** run for this
measurement (it takes over the display, i.e. a write, not a read), so there is no
`ps` number for it; its steady-state cost is that 3.6 MiB dma-buf plus the mapping
(same pages, shared) and well under 1 MiB of binary/heap. It is therefore not part of
the baseline and must not be counted as UI headroom for the Plasma session.

### 2.4 Kernel-side and pinned memory

| Item | Bytes | Meaning |
|---|---|---|
| dma-buf total | 47,951,872 (45.7 MiB) | 2 objects, 2 attached-device lists |
| `ion/common` → client `decon0` | 3,686,400 (3.52 MiB) | the fb0 scanout buffer, 720×1280×4 |
| `ion/ion_noncontig_heap` → `14580000.fimc_is` | 44,265,472 (42.2 MiB) | camera heap, allocated at boot and **never freed** |
| `Slab` | 42,472 kB | SUnreclaim 27,020 |
| `VmallocUsed` | 56,892 kB | vmap areas, not RAM-resident as such |
| `KernelStack` + `PageTables` | 3,312 + 1,652 kB | |

The 42.2 MiB `fimc_is` heap is 3 % of RAM held by an idle camera driver. It is not
reclaimable (`Shmem` is only 2,384 kB, so these pages are not swap-backed), and it
should be released or the camera power domain kept off for the Plasma bring-up.

fb0 geometry from sysfs: `virtual_size 720,1280`, `stride 2880` → 3,686,400 B,
matching the `decon0` ion allocation.

### 2.5 Swap state: none, zram inert, zswap **on** (hazard)

```
/proc/swaps          -> empty;  SwapTotal 0
/swapfile            -> does not exist
rc-status default    -> "swapfile [started]" (no file -> no-op),
                        "postmarketos-zram-swap [stopped]"
/sys/block/zram0     -> disksize 0, initstate 0, mm_stat all zero
                        comp_algorithm "[lzo] lz4", max_comp_streams 1, mem_limit 0
/sys/module/zswap/parameters/enabled          -> Y
/sys/module/zswap/parameters/compressor       -> lzo
/sys/module/zswap/parameters/zpool            -> zsmalloc
/sys/module/zswap/parameters/max_pool_percent -> 50
```

zram itself is built into the kernel (`lsmod` empty, `/sys/block/zram0` present;
`CONFIG_ZRAM=y`, `CONFIG_ZRAM_LZ4_COMPRESS=y`, `CONFIG_ZSMALLOC=y`,
`CONFIG_PGTABLE_MAPPING=y` in the built `.config`). Only **lzo and lz4** backends
exist in this tree (`drivers/block/zram/zcomp.c:24-27`); `zstd`, `lz4hc`, `842`,
`deflate` are absent, so the distro default `zstd` would fail.

**zswap is enabled with a 50 % pool.** If zram swap is enabled as-is, zswap
intercepts evicted pages first, compresses them into its own zsmalloc pool (lzo,
up to 687 MiB) and zram is left with the overflow — double compression, double pool
accounting, and the pool is not reclaimable. This must be disabled before zram is
used (ArchWiki zram §Usage as swap makes the same point).

---

## 3. Measurement method details (what the numbers mean)

### 3.1 There is no `MemAvailable` on this kernel

3.10 predates `MemAvailable` (3.14); the string is absent from
`fs/proc/meminfo.c` in this tree and from `/proc/meminfo` (33 keys, none matching
"avail"). The 3.14-era definition in
`Documentation/filesystems/proc.txt` is "calculated from MemFree, SReclaimable,
the size of the file LRU lists, and the low watermarks", i.e.

```
mem_avail_calc = MemFree + Active(file) + Inactive(file) + SReclaimable - min_free_kbytes
```

which is what `memwatch.sh` implements (floored at 0). First snapshot:
`1,045,920 + 31,132 + 146,644 + 15,452 - 4,569 = 1,234,579 kB = 1,205.6 MiB`.

Validation: the closure scan in §3.3 read ~700 MiB of Plasma/Qt libraries, dropping
`MemFree` from 1,045,920 kB to ~57,000 kB and raising `Cached` to ~1,000,000 kB and
`SReclaimable` to ~124,000 kB. `mem_avail_calc` moved only from **1,206 MiB to
1,154–1,199 MiB** across that churn (18 samples / 91 s, then 13 samples / 64 s, both
`memwatch.sh`), i.e. the derived metric behaves like `MemAvailable` should: page cache
and reclaimable slab count as available, the rest does not. The final run idled at
`min_avail` **1,193 MiB** with `pgscan_direct` 0, `pgmajfault` +0…+2 per 5 s, no OOM,
no process restart, `#RESULT PASS`.

Also absent on 3.10: `/proc/pressure/memory` (4.20) and an `oom_kill` counter in
`/proc/vmstat`; there is no cgroup memory controller on this image either
(`/sys/fs/cgroup` holds only elogind's named hierarchy). OOM evidence is therefore
`dmesg` only, and the ring is short: **1,359 lines / 43 s** at the j7elte log rate
(`sm5703-fuelgauge` spam). `memwatch.sh` defaults to a 5 s interval for that reason
and warns in its header; OOM lines can be missed if `-i` is above ~15 s.

### 3.2 How well does this device's data compress? (lz4, actual anon pages)

Read (as root, read-only) five anonymous mappings of the Xorg process and compressed
them with `lz4 -1` on the host — same algorithm family the kernel's
`zcomp_lz4` uses (`drivers/block/zram/zcomp_lz4.c:35-43`, `LZ4_MEM_COMPRESS =
4096*sizeof(unsigned char*) = 32 KiB`, `include/linux/lz4.h:12`):

| Region | raw | lz4 | ratio | zero bytes |
|---|---|---|---|---|
| 1 | 4,055,040 | 82,219 | 49.3:1 | 8.8 % |
| 2 | 2,535,424 | 146,244 | 17.3:1 | 91.7 % |
| 3 | 1,888,256 | 8,930 | 211:1 | 99.9 % |
| 4 | 753,664 | 145,665 | 5.2:1 | 76.2 % |
| 5 | 434,176 | 48,222 | 9.0:1 | 87.5 % |
| **sum** | **9,666,560** | **431,280** | **22.4:1** | 37 % |

An X11 shell's heap is dominated by framebuffer-shaped and zero-filled buffers, so
this is an optimistic bound. The most "mixed" region still compressed 5.2:1 with 76 %
zeros. Budgeting zram at **2:1** for Plasma (QML object graphs, strings, GL command
streams are far denser) is conservative and consistent with ArchWiki's "conservative
1:2, real world 1:3".

### 3.3 Static demand of the Plasma 6.8 binaries on this image

Computed from `ldd` + `size` over the full dependency closure (read-only, on device):

| Closure | objects | file text | private data+bss |
|---|---|---|---|
| `/usr/bin/plasmashell` | 272 | 331,261,071 B (315.9 MiB) | **8.9 MiB** |
| `/usr/bin/kwin_wayland` | 312 | 345,680,938 B (329.7 MiB) | **9.5 MiB** |

Largest private writable contributors: `libgallium-26.3.0` 3.36 MB, `libLLVM.so.23.1`
2.17 MB (the llvmpipe path), `libcrypto` 0.48 MB, `libQt6Quick` 0.41 MB,
`libkwin.so.6` 0.44 MB. The 315–330 MiB of text is page cache, not RSS; it is the
cold-start I/O demand and it is reclaimable. Additional on-disk QML/config demand:
`/usr/lib/qt6/qml` **65.4 MiB**, `/usr/share/plasma` 14.6 MiB,
`/usr/share/plasma/shells/org.kde.plasma.mobileshell` only 236 KiB (the shell's own
QML is thin; it pulls QML modules).

> Risk noted from the same data: `libgallium` + `libLLVM` are in kwin's closure. If
> kwin's EGL/GBM path falls back to **llvmpipe**, expect +100–200 MiB of JIT code and
> buffers. The M2/M4 gate must assert `GL_RENDERER` is panfrost, not llvmpipe.

### 3.4 Measured Qt Quick / QML floor

`qmlscene -platform offscreen` loading the real Plasma Mobile lock-screen component
(`.../mobileshell/contents/lockscreen/Clock.qml` — imports QtQuick, QML Layouts,
Controls, Kirigami, `org.kde.plasma.components`, `org.kde.plasma.private.mobileshell`),
with QML and shader disk caches disabled, peaked at:

```
VmHWM 86,784 kB   VmRSS 86,784 kB   Threads 7
```

That is the **floor for any Qt Quick + Kirigami UI on this device** with software
rendering and no GL buffers: ≈ 85 MiB per process. plasmashell will be above this
(it adds the shell, panels, dataengines and a GL scene graph).

---

## 4. Plasma 6.8 demand estimate

Every row is labelled by basis: **[M]** measured on this device, **[C]** cited
upstream, **[I]** inference/estimate.

| Component | Estimate | Basis |
|---|---|---|
| Kernel + pinned ion (fimc 42.2 + decon fb 3.5) + slab/stacks | 90–110 MiB | [M] §2.4; ion is unreclaimable, slab is partly reclaimable |
| Non-UI daemons (NetworkManager, wpa, elogind, logbookd, keyring…) | 95 MiB | [M] 139,700 − 34,488 − 8,368 |
| `kwin_wayland` (720×1280, single output, GL via panfrost) | 60–110 MiB | [I] from 9.5 MiB private data + runtime heaps; kwin leak reports are multi-GPU/multi-output (bugs 496469, 520377, 500792) and do not apply per se |
| kwin scanout swapchain (GBM/imported BOs, shmem) | 3 buffers × 3.52 = 10.6 MiB, +1 cursor | [M] buffer size = 720·1280·4 = 3,686,400 B; `panfrost7580_gem.c` CREATE_BO uses SHMEM (`shmem_file_setup` + `shmem_read_mapping_page`), so these pages appear in `/proc/meminfo:Shmem` |
| `plasmashell` (mobileshell, plasma-framework, dataengines) | 130–200 MiB | [M] 85 MiB QML floor + shell/dataengines; [C] desktop `plasmashell` PSS 305,669 kB / USS 281,428 kB with a terminal and a browser open (discuss.kde.org topic 44952, post 11) |
| Qt scene-graph GPU buffers (atlas, VBOs, glyph cache) | 8–24 MiB | [C] Qt atlas heuristics + `QSG_ATLAS_WIDTH/HEIGHT/SIZE_LIMIT`; 1024²×4 = 4 MiB per atlas texture; [M] buffers live in shmem, counted in `Shmem` |
| `kded6` | 20–30 MiB | [I] — links Qt6 Core/DBus only, no Quick |
| `kactivitymanagerd` (`/usr/lib/libexec/kactivitymanagerd`, package installed) | 15–25 MiB | [I] — D-Bus activated at session start |
| portals + polkit agent + pipewire/pulse client | 25–40 MiB | [I] — `xdg-desktop-portal-kde`, `xdg-desktop-portal`, `polkit` agent, `pipewire` |
| `kscreenlocker_greet` (only while locked) | 50–80 MiB | [I] — QML + logind, separate process, can be preloaded or not |
| `plasma-keyboard` / maliit (only when shown) | 50–70 MiB | [I] — `plasma-keyboard 6.8.3` installed; input-method process is a second QML engine |
| One app (plasma-settings / phone dialer) | 40–70 MiB | [I] — QML app, same floor as §3.4 |
| Browser (optional) | 150–300 MiB | [I] — not installed today |

Boot-to-home-screen total, unlocked, keyboard hidden, no apps:

```
95 (daemons) + 100 (kernel/ion) + 85 (kwin mid) + 140 (plasmashell mid)
+ 15 (kwin buffers) + 16 (scene graph) + 25 (kded6) + 20 (kactivitymanagerd)
+ 32 (portals)                                                      ≈ 528 MiB
worst case of the ranges                                            ≈ 645 MiB
```

Against `mem_avail_calc` ≈ 1,190 MiB that leaves ~545–660 MiB for page cache and
apps, which is the margin zram is for. With two apps open (settings + dialer,
+110 MiB) and the keyboard shown (+60 MiB) the session reaches ~700–820 MiB —
still inside RAM, but only without a browser.

---

## 5. Configuration

Nothing in this section was applied (device was read-only). Order matters.

### 5.1 zram: algorithm `lz4`, size 687 MiB, priority 300

Use the image's own mechanism — `postmarketos-zram-swap` → `/usr/bin/zramstart`
(package `postmarketos-zram-4-r0`, installed but **stopped**). Override via
`/etc/deviceinfo` (sourced by `zramstart`):

```
# /etc/deviceinfo  (append; read by /usr/bin/zramstart)
deviceinfo_zram_swap_pct=50
deviceinfo_zram_swap_algo=lz4
```

then `rc-update add postmarketos-zram-swap default` and start it.

Why these values:

* **`lz4`** — the only alternatives in this kernel are `lzo` and `lz4`
  (`zcomp.c:24-27`, `comp_algorithm` = `[lzo] lz4`). `zramstart`'s default is
  `zstd`, which `zcomp_create()` rejects with `-EINVAL`, so the service would create
  no swap at all on this kernel. lz4 is the faster of the two at equal-or-better
  ratio and its per-CPU work memory is 32 KiB vs lzo's 16 KiB — see below.
* **50 % of RAM = 687 MiB** uncompressed capacity. `zramstart`'s built-in default is
  150 % for devices under 32 GB = **2,061 MiB** (its own comment cites pmaports
  issue 4250). At a 2:1 lz4 ratio that default could pin ~1,030 MiB of physical
  RAM — 75 % of the device — which cannot work. ArchWiki/zram-generator default to
  `min(ram/2, 4 GiB)`, i.e. 687 MiB here, which at 2:1 costs ≤ 343 MiB and at 3:1
  ≈ 229 MiB. `disksize` is the *uncompressed* capacity, not a reservation
  (kernel zram doc).
* **Optional hard cap**: this kernel also supports `mem_limit`
  (`/sys/block/zram0/mem_limit`, bytes or suffix; it is a deprecated-but-working
  attribute in `zram_drv.c:159-197`). If the session later proves it can thrash,
  `echo 384M > /sys/block/zram0/mem_limit` bounds zram's physical footprint at
  28 % of RAM. Not needed with 687 MiB, and `zramstart` does not set it.
* **Priority 300** and `page-cluster=0` come from `zramstart` itself.
* zram's own RAM overhead, from this tree: one compression stream **per CPU**
  (`zcomp_init()` allocates per-CPU streams; `zcomp_set_max_streams()` is a no-op
  returning true, so the `max_comp_streams=1` attribute is nominal). Per stream:
  2 pages output buffer (`zcomp_strm_alloc`, `zcomp.c:47-70`) + `LZ4_MEM_COMPRESS`
  32 KiB = ~40 KiB; × 8 CPUs ≈ **328 KiB** for lz4 (lzo would be ~200 KiB).
  Negligible either way — which is also why the `max_comp_streams` attribute can be
  ignored on this kernel.

### 5.2 zswap must be off before zram is started — and cannot be turned off at runtime

`/sys/module/zswap/parameters/enabled` is `Y` today with `max_pool_percent=50`,
`zpool=zsmalloc`, `compressor=lzo`. This kernel declares the switch **read-only** —
`module_param_named(enabled, zswap_enabled, bool, 0444)` (`mm/zswap.c:84-85`), sysfs
node `-r--r--r--`, no store handler — so `echo 0 > .../enabled` is not possible, by
root or otherwise. Two real options:

1. **Correct fix — command line `zswap.enabled=0`.** The current `cmdline` is only
   `console=ttySAC1`, so this is a boot.img change (`KmsBoot` owns it). Only this
   actually disables the feature.
2. **Stopgap with no flash — starve the pool.** `max_pool_percent` *is* writable
   (`uint, 0644`, `mm/zswap.c:93-95`) and the pool is empty today
   (`/sys/kernel/debug/zswap/pool_pages = 0`, `stored_pages = 0` — there has never
   been a swap device):
   `echo 0 > /sys/module/zswap/parameters/max_pool_percent`
   `zswap_is_full()` tests `totalram_pages * max_pool_percent / 100 <
   pool_total_size` (`mm/zswap.c:432`), so a 0 % pool accepts no further stores once
   it holds anything. It does not tear down an existing pool (irrelevant: this one is
   empty) and it must happen **before** `swapon /dev/zram0`.

`postmarketos-zram-swap` has no hook for either step; the session slice ships zram
via `/etc/deviceinfo` and warns about zswap instead of pretending to disable it
(`docs/reports/decon-kms-session.md`).

Verification: either `enabled` reads `N` (cmdline fix), or `max_pool_percent=0` with
`/sys/kernel/debug/zswap/stored_pages` staying 0 while `/proc/swaps` shows
`/dev/zram0`.

### 5.3 swappiness 180, page-cluster 0, min_free_kbytes 100000

`zramstart` applies exactly this (read from `/usr/bin/zramstart` on the device):

```
sysctl -w vm.page-cluster=0 vm.swappiness=180
sysctl -w vm.min_free_kbytes=100000   # only if current < 100000, only if RAM >= 1 GB
```

* 180 is allowed on this kernel: `CONFIG_INCREASE_MAXIMUM_SWAPPINESS=y` in the
  built `.config` (`kernel/sysctl.c:138,1214-1216` raises the cap from 100 to 200),
  and the kernel docs recommend >100 when swap is as fast as RAM.
* `page-cluster=0` is the zram recommendation (ArchWiki, Pop!_OS tuning).
* `min_free_kbytes=100000` = 7.1 % of RAM (vs the 4,569 kB default = 0.33 %). pmOS
  chose it so reclaim starts early enough to use zram instead of hitting the OOM
  killer. Cost: the watermarks (min/low/high ≈ 100/125/150 MiB) reserve ~150 MiB
  from the allocator. `memwatch.sh` subtracts `min_free_kbytes` in
  `mem_avail_calc`, so the thresholds in §7 already account for it.
* Do **not** add `vm.watermark_scale_factor` / `watermark_boost_factor` from the
  ArchWiki recipe: neither exists in this kernel (`/proc/sys/vm` has no such
  files).

### 5.4 `MALLOC_ARENA_MAX`: not applicable (musl) — value 2 if glibc ever returns

Measured libc: `musl-1.2.6-r6`, `/lib/ld-musl-aarch64.so.1`, no `/lib/ld-linux*`,
`plasmashell` NEEDED `libc.musl-aarch64.so.1`. `MALLOC_ARENA_MAX` is a **glibc**
malloc tunable (`M_ARENA_MAX`, man 3 mallopt; glibc's default arena limit is derived
from CPU count, 8×nproc = 64 on this 8-core SoC). musl has no arenas and no
environment-driven malloc tuning, so setting the variable here changes nothing.

Decision: **do not set it** in the Plasma session. If a glibc-based image is ever
built (e.g. a hybris variant), set `MALLOC_ARENA_MAX=2` in the session environment —
that is the value to keep the 64-arena worst case down without serialising Qt's
main+render threads.

What actually reduces allocator pressure here instead: fewer QML engines (§5.6),
`vm.overcommit_memory=0` left alone, and zram for cold anonymous pages. musl returns
freed large blocks with `munmap` immediately, so a leak shows up as RSS growth — which
is exactly what the `RSS:`/`RESTART:` flags in `memwatch.sh` watch for.

### 5.5 tmpfs sizing (`/tmp` and `/dev/shm` are unbounded)

Measured mounts: `tmpfs /tmp` and `shm /dev/shm` with no `size=` (default = half of
RAM each, ~687 MiB), plus `tmpfs /run/user/10000 size=140756k`. `/proc/meminfo:Shmem`
is only 2,384 kB today, but any app that writes 500 MiB into `/tmp` or `/dev/shm`
takes it out of the same 1.37 GiB. Recommendation for the Plasma image:

```
tmpfs /tmp     tmpfs nosuid,nodev,size=64M     0 0
tmpfs /dev/shm tmpfs nosuid,nodev,size=128M    0 0
```

(`/run/user/<uid>` at 10 % of RAM is already bounded and fine.) This is a fstab
change in the `PlasmaSession`/image slice, not something M4 needs on day one — but
`Shmem` is also where panfrost's GEM buffers appear, so an unbounded `/dev/shm` and
GL memory are the same accounting bucket.

### 5.6 Session services to disable on 1.4 GB

Measured from `/etc/xdg/autostart` and `apk list -I` on the device. **XWayland is not
installed** (`/usr/bin/Xwayland` and `/usr/libexec/Xwayland` both absent), so every
X11 autostart entry below is dead weight in a kwin_wayland session — it cannot
connect, and if Xwayland is ever added these would each cost 10–40 MiB:

| Autostart entry | Installed binary | Action | Why |
|---|---|---|---|
| `lxqt-panel.desktop`, `lxqt-globalkeyshortcuts.desktop`, `lxqt-desktop.desktop`, `lxqt-xscreensaver-autostart.desktop` | `/usr/bin/lxqt-panel` (1.4 MB) | remove/mask | X11 panel, replaces plasmashell's UI |
| `xfce4-power-manager.desktop` | `/usr/bin/xfce4-power-manager` | mask (until `powerdevil` exists) | X11 applet; **`powerdevil` is not installed** — pick one, not both |
| `xfce4-notifyd.desktop` | — | mask | Plasma has its own notification service |
| `xfce4-screensaver.desktop` | — | mask | conflicts with `kscreenlocker` |
| `nm-applet.desktop` | `/usr/bin/nm-applet` | mask | `plasma-nm` provides the applet |
| `blueman.desktop` | `/usr/bin/blueman-applet` | mask | `bluez-qt` / Plasma applet |
| `xembedsniproxy.desktop` | `/usr/bin/xembedsniproxy` | mask | XEmbed only |
| `gmenudbusmenuproxy.desktop` | — | mask | X11 menu proxy |
| `mpris-proxy.desktop` | `/usr/bin/mpris-proxy` | keep (small) | MPRIS/AVRCP needs it |
| `at-spi-dbus-bus.desktop` | `at-spi2-core` | mask unless accessibility is needed | GTK bridge, not used by Qt |
| `pipewire.desktop`, `pulseaudio.desktop` | — | keep | audio |
| `gnome-keyring-*.desktop` | 4 instances, 8 MiB | replace with `kwallet` | duplicate daemons observed on the running system |

Also:

* `logbookd` (9.0 MiB) journals to `/var/log/logbookd.db`; the fuel-gauge driver
  spams `dmesg` at ~30 lines/s, which is what shortens the kernel ring to 43 s
  (§3.1) and blinds OOM detection at long sample intervals. Dropping it for the
  milestone both frees 9 MiB and restores a usable `dmesg` window.
* `baloo`: only `baloo-libs` is installed, no indexer — nothing to disable today.
* `kactivitymanagerd` is installed (`/usr/lib/libexec/kactivitymanagerd`, D-Bus
  activated, ~20 MiB). It is not needed by the mobile shell's home screen; if
  activities are unused, keep it from being activated (it costs one process plus
  its plugins). `kscreenlocker_greet` only runs on lock.
* Do **not** install `plasma-browser-integration`, `discover`/`packagekit`,
  `kdeconnect` or `akonadi` for the milestone.

---

## 6. Measurement tool: `code/gpu31/kms/tools/memwatch.sh`

POSIX `sh`, runs on the phone (busybox) or from the host over ssh without writing
anything on the device:

```
# on device
memwatch.sh -t 1800 -i 5 -o /tmp/mem.csv
# from the host (no device write; script fed on stdin)
flock /tmp/gpsdev.lock -c 'SSH_ASKPASS=/tmp/ap.sh DISPLAY=:0 setsid -w \
  ssh user@172.16.42.1 "sh -s -- -t 1800 -i 5"' < code/gpu31/kms/tools/memwatch.sh > mem.csv
```

Flags: `-i` interval (default 5 s), `-n` samples, `-t` duration, `-o` file,
`-p` processes to track, `-L NAME=MiB` RSS ceiling (repeatable),
`--low-mb` (default 300), `--crit-mb` (default 150), `-q`.
Exit: `0` pass, `1` threshold exceeded, `2` OOM seen, `3` setup error.

CSV columns (one row per sample):

```
ts, elapsed_s, mem_total, mem_free, mem_avail_calc, cached, active_file,
inactive_file, sreclaimable, shmem, anon, slab, sunreclaim, swap_total,
swap_free, zram_disksize, zram_orig, zram_compr, zram_mem_used,
zram_mem_limit, zram_used_max, pgscan_direct, pgsteal_direct, pgmajfault,
oom_total, flags, procs
```

* `mem_avail_calc` — §3.1 formula (this kernel has no `MemAvailable`).
* `zram_orig/compr/mem_used/limit/used_max` — `mm_stat` fields 1–5; `mem_used` is
  the physical cost, `zram_orig/compr` the ratio.
* `pgscan_direct`/`pgsteal_direct`/`pgmajfault` — deltas over the interval
  (direct reclaim = the session is failing to keep up; kswapd is normal).
* `flags` — space separated, `ok` when clean: `OOM` (new `Out of memory|oom-kill|
  Killed process` line since start), `LOW`, `CRIT`, `RSS:<name>` (ceiling),
  `RESTART:<name>` (tracked pid changed = session died).
* `procs` — `name=pid/rss/hwm;…` for the tracked set.
* `#SUMMARY` lines at the end carry min/max aggregates, zram ratio and per-process
  first/last pid, then `#RESULT PASS|FAIL rc=N`.

Default ceilings: `plasmashell=220`, `kwin_wayland=160` MiB; `Xorg`/`nura-shell`
are tracked without a ceiling. Validated on the device: 13 samples / 64 s
(`#RESULT PASS`, min_avail 1,193 MiB, no false `RESTART`) and a 32 s run after the
process-name handling was fixed; the 91 s run over the page-cache churn exercised the
same paths.

---

## 7. Acceptance thresholds for the session milestone (M4)

Run each gate from a **fresh boot** (page cache comparability), with zram and zswap
configured as in §5, `memwatch.sh` at `-i 5`, and `dmesg_restrict=0` (already so).

### Gate A — home screen, 5 min idle

| Check | Threshold |
|---|---|
| `mem_avail_calc` minimum | **≥ 350 MiB** |
| `plasmashell` VmHWM | **≤ 220 MiB** (flag `RSS:plasmashell` must stay clear) |
| `kwin_wayland` VmHWM | **≤ 160 MiB** |
| `zram_mem_used` after 5 min | **≤ 128 MiB** |
| `oom_new`, `RESTART` | **0** |
| `GL_RENDERER` of the session | panfrost, **not** llvmpipe (§3.3) |

### Gate B — 30 min soak (home screen; launch+close one app and screen off/on every 5 min)

| Check | Threshold |
|---|---|
| `mem_avail_calc` minimum | **≥ 150 MiB** |
| `oom_new` | **0** |
| `plasmashell` RSS growth after minute 5 | **≤ 10 %** (leak gate) |
| `kwin_wayland` RSS growth after minute 5 | **≤ 20 %** |
| mean `pgscan_direct` | **≤ 5 pages/s** |
| `zram` ratio `orig/compr` once `compr ≥ 16 MiB` | **≥ 2.0** |

### Gate C — settings + dialer + keyboard shown (10 min)

| Check | Threshold |
|---|---|
| `mem_avail_calc` minimum | **≥ 80 MiB** |
| `zram_mem_used` (physical) | **≤ 300 MiB** — at the 2:1 design point the 687 MiB device caps at ≈ 343 MiB |
| `oom_new`, `RESTART` | **0** |
| `Shmem` (GL + scanout buffers) | ≤ 220 MiB, and not growing when idle |

### Gate D — lock/unlock (5 cycles)

`kscreenlocker_greet` VmHWM ≤ 80 MiB, `mem_avail_calc` returns to its pre-lock value
± 30 MiB, no `OOM`.

Failure triage, in order: `GL_RENDERER=llvmpipe` → fix the EGL/GBM path before
touching memory config; `plasmashell` growth > 10 % → treat as a leak (cf. KDE bugs
436061, 496469); `zram_mem_used` at the cap with `pgscan_direct` high → trim session
services (§5.6) or shrink `disksize` and accept app kills; `OOM` with
`mem_avail_calc` still > 150 MiB → allocation burst (e.g. QML compile), re-check the
`oom_dump_tasks` log for the victim.

---

## 8. Residual risks

| Risk | Effect | Mitigation |
|---|---|---|
| kwin/Qt private-ABI break (§2.1) | nothing to measure, session cannot start | rebuild kwin against Qt 6.12 or pin Qt 6.8; verify with `kwin_wayland --version` |
| kwin falls back to llvmpipe | +100–200 MiB, unusable | gate on `GL_RENDERER`, `MESA_LOADER_DRIVER_OVERRIDE=panfrost` during bring-up |
| kwin compositor leaks (upstream reports are multi-monitor; single 720p output untested on 3.10) | unbounded growth | Gate B growth checks; `kmemleak` is not enabled on this kernel, so use RSS curves |
| 42.2 MiB `fimc_is` ion heap held from boot | 3 % of RAM lost | release the camera heap or keep its PD off; kernel-side owner |
| zram at 687 MiB with 2:1 → 343 MiB physical | steals RAM from the page cache | `mem_limit` (§5.1) if knee found |
| zswap re-enabled at boot before zram (`enabled` is read-only) | pages compressed twice, pool unreclaimable | cmdline `zswap.enabled=0`; until then `max_pool_percent=0` and watch `stored_pages` (§5.2) |
| `min_free_kbytes=100000` | 150 MiB of watermarks unavailable to apps | measured by `mem_avail_calc`; revisit to 65536 only if Gate C fails on `mem_avail_calc` while zram is still empty |
| `/` has 69.6 MiB free (96 % used) | cannot install extra debug packages during M4 | use `/data` (4.6 GiB free) for tools/logs, as the bring-up doc already requires |
| dmesg ring 43 s | OOM can be missed at `-i > 15` | keep `-i 5`; drop `logbookd`/fuel-gauge spam |

---

## 9. Evidence index

Device (all read-only, under `flock /tmp/gpsdev.lock`):
`/proc/meminfo`, `/proc/vmstat`, `/proc/swaps`, `/proc/sys/vm/*`,
`/sys/block/zram0/{disksize,mm_stat,comp_algorithm,max_comp_streams,mem_limit}`,
`/sys/module/zswap/parameters/*`, `/sys/kernel/debug/{dma_buf/bufinfo,ion/heaps/*}`
(as root), `/proc/<pid>/{status,smaps,maps}`, `/etc/deviceinfo`,
`/usr/bin/zramstart`, `/etc/init.d/postmarketos-zram-swap`, `rc-status`,
`apk list -I`, `/etc/xdg/autostart/*`, `ldd`/`size` closures, `qmlscene` offscreen
run, `kwin_wayland --version`, `ldd plasma_session`, `/sys/kernel/debug/zswap/*`
(as root).

Kernel tree (read-only):
`drivers/block/zram/zram_drv.c:159-197,228-250,839-850,1075-1111`,
`drivers/block/zram/zcomp.c:24-27,47-70,95-98`,
`drivers/block/zram/zcomp_lz4.c:22-43`, `include/linux/lz4.h:12`,
`include/linux/lzo.h:17-18`, `kernel/sysctl.c:138-139,1208-1218`,
`mm/zswap.c:84-100,432` (`enabled` is `0444`, `max_pool_percent` is `0644`,
`zswap_is_full()`),
`.config` (`CONFIG_INCREASE_MAXIMUM_SWAPPINESS=y`, `CONFIG_ZRAM=y`,
`CONFIG_ZRAM_LZ4_COMPRESS=y`, `CONFIG_ZSMALLOC=y`, `CONFIG_PGTABLE_MAPPING=y`,
`CONFIG_ZSWAP=y`, `CONFIG_FRONTSWAP=y`, `CONFIG_CMA_SIZE_MBYTES=0`),
`code/gpu31/driver/0001-panfrost7580-driver-sources.patch:186-215,2490-2496`
(SHMEM BO backing).

Upstream:
[kernel zram doc](https://www.kernel.org/doc/Documentation/admin-guide/blockdev/zram.rst),
[kernel proc.txt `MemAvailable`](https://www.kernel.org/doc/Documentation/filesystems/proc.txt),
[ArchWiki zram](https://wiki.archlinux.org/title/Zram),
[man 3 mallopt (`M_ARENA_MAX`)](https://man7.org/linux/man-pages/man3/mallopt.3.html),
[Qt Quick scene graph renderer (atlas/VBO)](https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph-renderer.html),
[plasmashell PSS 305 MB measurement](https://discuss.kde.org/t/possibly-high-idle-memory-usage-from-plasmashell/44952),
[kwin memory bug 496469](https://bugs.kde.org/show_bug.cgi?id=496469),
[kwin memory bug 520377](https://bugs.kde.org/show_bug.cgi?id=520377),
[pmaports zram default rationale (issue 1133)](https://gitlab.com/postmarketOS/pmaports/-/issues/1133),
[pmaports zram sizing discussion (issue 4250, cited by `/usr/bin/zramstart`)](https://gitlab.com/postmarketOS/pmaports/-/issues/4250),
[procps `free` MemAvailable fallback (`library/meminfo.c:727`)](https://gitlab.com/procps-ng/procps/-/blob/master/library/meminfo.c).
