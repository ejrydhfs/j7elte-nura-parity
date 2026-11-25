# KMS vblank timestamps (assessment)

Status: **analysis complete, no patch applied.** Verdict: keep
`dev->driver->get_vblank_timestamp` unset for M1. KWin 6.8 does not require
it, and the precise implementation is not low risk on this SoC because the
DECON scanout-position register is undocumented in the tree and cannot be
validated from the current boot.

Scope: read-only analysis. No kernel was built and nothing was flashed or
rebooted for this report. Kernel citations are `path:line` in the read-only
3.10.108 vendor tree
(`android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07`).
Driver citations refer to the KMS sources at this repo's HEAD (693e0da).
KWin citations refer to `/tmp/kwin-6.8.3` (KWin 6.8.3 source, the version
installed in the test rootfs).

---

## 1. What 3.10 offers

The 3.10 DRM core has both layers:

| Layer | API | Where |
|---|---|---|
| Driver callback, raw scanout position | `get_scanout_position(dev, crtc, &vpos, &hpos)` | `include/drm/drmP.h:824-849`, optional |
| Driver callback, precise timestamp | `get_vblank_timestamp(dev, crtc, &max_error, &tv, flags)` | `include/drm/drmP.h:851-884`, optional |
| Core helper for drivers | `drm_calc_vbltimestamp_from_scanoutpos(...)` | `drm_irq.c:581-729` |
| Core helper for timings | `drm_calc_timestamping_constants(crtc)` | `drm_irq.c:492-535` |
| Core fallback | `drm_get_last_vbltimestamp()` | `drm_irq.c:762-785` |
| Ring buffer and event delivery | `drm_handle_vblank()`, `drm_vblank_count_and_time()`, `drm_send_vblank_event()` | `drm_irq.c:1353-1405`, `:816-834`, `:861-876` |

`drm_vblank_init()` prints the caching line plus one of two capability
lines (`drm_irq.c:259-266`):

```
[drm] Supports vblank timestamp caching Rev 1 (10.10.2010).
[drm] Driver supports precise vblank timestamp query.       # callback present
[drm] No driver support for vblank timestamp query.          # callback NULL
```

The `No driver support...` line is a `DRM_INFO`, not an error. It is
emitted once, from `drm_vblank_init()` inside our `->load()`
(`code/gpu31/kms/exynos_drm7580_main.c:286`), before userspace can run. It
does not disable anything.

### 1.1 The generic fallback is a complete path, not an error path

When the callback is NULL (or when it returns `<= 0`), the core does
(`drm_irq.c:762-785`):

```c
if (dev->driver->get_vblank_timestamp && (max_error > 0)) {
        ret = dev->driver->get_vblank_timestamp(...);
        if (ret > 0)
                return (u32) ret;
}
*tvblank = get_drm_timestamp();     /* ktime_get(); monotonic by default */
return 0;
```

`get_drm_timestamp()` is `ktime_get()` (`drm_irq.c:731-740`). The timestamp
is taken in `drm_handle_vblank()` at the DECON frame interrupt
(`drm_irq.c:1381`) and stored in the timestamp ring before pending events are
sent (`:1398-1405`). `drm_send_vblank_event()` copies the cached timestamp
into `event.tv_sec/tv_usec` (`:861-876`). Therefore a flip event delivered by
our fence path always carries a real monotonic timestamp even with no driver
callback.

Defaults in this tree:

| Module parameter | Default | Meaning |
|---|---|---|
| `drm.timestamp_monotonic` | 1 | `drm_stub.c:53`; `DRM_CAP_TIMESTAMP_MONOTONIC` reports 1 (`drm_ioctl.c:290-292`) |
| `drm.timestamp_precision_usec` | 20 | `drm_stub.c:46`; only used when a callback exists (`drm_irq.c:768-772`) |

Both were read on the attached J7 as root on 2026-11-25:
`timestamp_monotonic=1`, `timestamp_precision_usec=20`. So KWin is on
`CLOCK_MONOTONIC` and `drm_timestamp_precision` is irrelevant to the current
fallback.

### 1.2 What "precise" adds

The generic fallback timestamps the interrupt, not the scanout position. Its
error is the IRQ delivery latency plus the difference between the VSYNC
interrupt (the frame interrupt is selected on VSYNC,
`decon_reg_set_int()`: `decon_reg_7580.c:569-588`) and the start of active
scanout after vblank. That is a small constant plus jitter, not a broken
timestamp.

The precise path reads a scanout position and back/forward-projects the
start of the next scanout from the mode timings:

* `crtc->hwmode`, `crtc->linedur_ns`, `crtc->pixeldur_ns`,
  `crtc->framedur_ns` must be valid (`drm_irq.c:602-622`);
* `get_scanout_position()` must return `DRM_SCANOUTPOS_VALID` (and optionally
  `INVBL`/`ACCURATE`), with `vpos >= 0` in active scanout and negative in
  vblank counting lines to the next active line (`drmP.h:833-848`,
  `drm_irq.c:640-670`);
* the core retries up to 3 times to keep the read-to-read duration under
  `max_error` (20 us by default, `drm_irq.c:630-668`).

---

## 2. What our driver does today

`code/gpu31/kms/exynos_drm7580_main.c` registers only the three mandatory
vblank hooks:

| Hook | Line | Implementation |
|---|---|---|
| `get_vblank_counter` | `:104-110` | returns `drm_vblank_count(drm, pipe)` (software count) |
| `enable_vblank` | `:127-137` | increments a local user count; the FRAME IRQ is always armed from `->load()` |
| `disable_vblank` | `:139-145` | decrements it |
| FRAME ISR | `:115-125` | `drm_handle_vblank()` + `exynos7580_vblank_irq()`, called from the vendor shim callback outside `decon->slock` |
| `drm_driver` assignment | `:392-396` | no `get_vblank_timestamp`, no `get_scanout_position` |

Returning `drm_vblank_count()` from `get_vblank_counter` is explicitly
allowed for devices without a hardware counter (`drmP.h:766-782`). With no
hardware counter the core cannot reconstruct frames that occurred while
vblank IRQs were off (`drm_update_vblank_count()` computes `diff` from this
same software count, `drm_irq.c:894-941`); that does not affect flip events,
which hold a vblank reference from arm to delivery, so every flip's event
sees the next real FRAME interrupt.

The event path is correct and is the one KWin observes:
`exynos7580_arm_vblank_event()` samples `drm_vblank_count()` under
`dev->event_lock` (`exynos_drm7580_fence.c:147-148`) and
`drm_handle_vblank()` timestamps and sends the event from the FRAME IRQ
(`fence.c:28-36`, `drm_irq.c:1353-1405`).

### 2.1 The hidden cost: `hwmode` is never set on the atomic path

This matters for any future implementation. The only 3.10 core caller of
`drm_calc_timestamping_constants()` is `drm_crtc_helper_set_mode()`: it
stores `crtc->hwmode = *adjusted_mode` at `drm_crtc_helper.c:487` and calls
the constants helper at `:493`. (The only other user of the helper is i915,
from its own driver code.) Our atomic path does not use that function.
The M0 atomic helper's `crtc_set_mode()` calls `funcs->mode_set()` directly
(`drm_atomic_helper.c:692-724`), and `exynos7580_crtc_mode_set()` does not
assign `crtc->hwmode` (`exynos_drm7580_crtc.c:225-270`).

Consequence: adding `get_scanout_position`/`get_vblank_timestamp` alone
would still fail `drm_calc_vbltimestamp_from_scanoutpos()` with `-EAGAIN`
because `vtotal/vdisplay/framedur_ns` are zero (`drm_irq.c:613-622`), and
silently fall back. A real patch must also set `crtc->hwmode` and call
`drm_calc_timestamping_constants()` from our `mode_set()`; that is a
core-visible state change which would need a boot test.

---

## 3. Candidate precise implementation (not applied)

For the record, the shape of the patch if it is ever revisited:

1. **Shim accessor** in `include/linux/exynos_decon_kms.h` /
   `drivers/video/exynos/decon_7580/decon_kms_api.c`:

   ```c
   int decon_kms_get_scanoutpos(struct decon_kms_dev *d, u32 *linecnt,
                                u32 *vstatus);
   ```

   The shim is linked in the vendor DECON object, so it can call the
   non-exported `decon_reg_get_linecnt()` and `decon_reg_get_vstatus()`
   (`decon_reg_7580.c:229-237`), which read
   `VIDCON1_LINECNT_GET(readl(VIDCON1(0)))` (13 bits,
   `regs-decon.h:187-194`). An API call is required because those helpers
   are not exported.

2. **`get_scanout_position()`** in `exynos_drm7580_main.c`:
   read line counter and vstatus, map to the core convention (`vpos >= 0`
   active, negative in vblank), set `hpos = 0` (no pixel counter), and return
   `DRM_SCANOUTPOS_VALID` plus `DRM_SCANOUTPOS_INVBL` for the vblank statuses
   (`ACCURATE` only after the mapping is measured). Only one pipe exists.

3. **`get_vblank_timestamp()`** in `exynos_drm7580_main.c`:
   validate `pipe == EXYNOS7580_PIPE` and `crtc->enabled`, then call
   `drm_calc_vbltimestamp_from_scanoutpos(dev, pipe, max_error,
   vblank_time, flags, crtc)` (`drm_irq.c:581`).

4. **`exynos7580_crtc_mode_set()`**: assign
   `crtc->hwmode = *adjusted_mode;` and call
   `drm_calc_timestamping_constants(crtc);` after the link enable succeeds,
   mirroring `drm_crtc_helper.c:487-493`. Do it before
   `drm_vblank_post_modeset()`.

5. **`exynos7580_driver`**: set `.get_vblank_timestamp` and
   `.get_scanout_position`.

Expected dmesg change is only the one INFO line:
`Driver supports precise vblank timestamp query.`

---

## 4. Why this is not low risk on J7

The code above is small, but its central input is unvalidated:

* **The line counter's origin and vblank behavior are not documented in the
  tree.** The only in-tree consumers are
  `decon_reg_wait_linecnt_is_zero_timeout()` (stop/suspend paths,
  `decon_reg_7580.c:240-264`) and the line-count operation threshold, which
  is set to `yres - 1` (`decon_reg_7580.c:193,469`). Neither proves whether
  `VIDCON1.LINECNT` counts through the porches, wraps at `vtotal`, or is only
  meaningful in video mode. J7 is `DECON_VIDEO_MODE`
  (`decon_api.h:155`, `decon_kms_api.c:367`), but that alone does not
  establish the register's frame phase.
* **A wrong phase is silently accepted by the core.** There is no check that
  the returned scanout position matches the mode; a constant offset of up to
  one frame would simply move every timestamp, and only measurement would
  reveal it.
* **The vblank window boundaries are not exposed.** `vtotal` and `vdisplay`
  are available from the mode, but the precise mapping needs the vpos sign
  convention to line up with the register. The frame IRQ is VSYNC-selected
  (`decon_reg_7580.c:581-582`), so the sampled counter sits in the vblank
  interval; without the register's definition the mapping is guesswork.
* **No hardware validation is possible from the current environment.** The
  attached J7 is currently booted on kernel `3.10.108 #37` with
  `/sys/class/graphics/fb0` and `renderD128` only - no `card0`, so no KMS
  CRTC to sample. `/dev/mem` does not exist; `mknod /tmp/mem c 1 1` followed
  by a read fails with `EPERM`, so MMIO cannot be sampled from userspace.
  The KMS images are not currently booted and no build is allowed for this
  ticket, so a patch would be both unbuilt and unmeasured.
* **`hpos` is unavailable.** The best achievable error is one scanline
  (about 12.6 us if `vtotal` is ~1320 at 59.92 Hz; `vtotal` was not read back
  here) plus read latency, not the 20 us target. That is probably fine, but
  it is a measured claim, not an assumption.
* **No precedent to copy.** Neither the vendor 3.10 Exynos DRM driver nor
  upstream Linux implements these callbacks for Exynos DECON. Upstream
  master's `exynos7_drm_decon.c` and `exynos5433_drm_decon.c` only implement
  `enable_vblank`/`disable_vblank` + `drm_crtc_handle_vblank()` (checked in
  master, fetched 2026-11-25); the vendor tree's
  `drivers/gpu/drm/exynos/` has no `get_vblank_timestamp` or
  `get_scanout_position`. Samsung's current downstream DECON
  (`google-modules/display`, android-15) likewise has no scanout-position
  callback. Upstream Exynos DRM has shipped with this path for years.
* **Register-read context.** `get_scanout_position()` is called under
  `dev->vblank_time_lock` (spinlock, IRQs off) and from the hard IRQ, so the
  new shim entry point must be a plain `readl` of already-clocked DECON
  registers: no `decon->slock`, no `pm_runtime`, no sleeping. The existing
  shim entry points are documented as process context, so an IRQ-safe API
  contract is part of the work.
* **The benefit is small.** The fallback timestamp is taken in the same
  interrupt, a few microseconds later than the vendor's own
  `vsync_info.timestamp = ktime_get()` at IRQ entry
  (`decon-int_drv.c:76-108`). A precise implementation would remove
  IRQ-delivery jitter and the VSYNC-to-scanout offset, not a functional
  failure.

---

## 5. Does KWin 6.8 require it? No

The message seen in dmesg is not from KWin: it is the kernel `DRM_INFO` from
`drm_vblank_init()` (`drm_irq.c:265`). KWin's source contains no such string
and no `drmWaitVBlank`/`DRM_VBLANK` use in its DRM backend (grep over
`/tmp/kwin-6.8.3/src`). It consumes only the page-flip event timestamp:

| KWin site | Behavior |
|---|---|
| `src/backends/drm/drm_gpu.cpp:83-88` | queries `DRM_CAP_TIMESTAMP_MONOTONIC`; 1 => `CLOCK_MONOTONIC`, else `CLOCK_REALTIME` with conversion |
| `src/backends/drm/drm_gpu.cpp:587-632` | `page_flip_handler2` path: converts `sec/usec`; only `{0, 0}` is treated as invalid, logging `Got invalid timestamp` (max 10 warnings) and substituting `steady_clock::now()` |
| `src/core/renderloop.cpp:201-213` | rejects a non-monotonic presentation timestamp, substitutes `steady_clock::now()`; does not abort |
| `src/backends/drm/drm_commit_thread.cpp:365-374` | scheduling margin = vblank time + at least 1 ms (`KWIN_DRM_OVERRIDE_SAFETY_MARGIN`), chosen experimentally because commit application time is not known precisely |
| `src/backends/drm/drm_commit_thread.cpp:40-64` | the only timing timeout error path is a page flip that never arrives within 1 s; timestamps do not affect delivery |

With no driver callback, the core supplies a monotonic `ktime_get()`
timestamp from the FRAME IRQ, so the event is not `{0, 0}` and KWin does not
take its invalid-timestamp path on page flips. Even a constant offset (for
example VSYNC versus start of scanout) is invisible to KWin's
interval-based prediction: it shifts the phase of the target but not the
cadence, and the 1 ms margin absorbs the difference.

One edge case does produce a zero timestamp, and KWin handles it: 3.10's
`drm_vblank_off()` calls `vblank_disable_and_save()`, which invalidates the
timestamp ring (`drm_irq.c:165`), then samples it for pending events
(`:1031`). An event completed around a CRTC disable can therefore carry
`{0, 0}`; KWin logs `Got invalid timestamp` (bounded) and falls back to
`steady_clock`. This is a disable-time event, not a page flip on a live
pipe, and it does not fail KWin. It is also not fixed by adding a precise
driver callback (the callback is not consulted by `drm_vblank_off()`).

Finally, vblank timestamps are unrelated to the current llvmpipe-versus-
kmsro(panfrost) issue: kmsro pairing is selected by the DRM driver name /
render node, and timestamps only affect frame pacing after a buffer reaches
the KMS device.

---

## 6. Recommendation

**Leave `get_vblank_timestamp` NULL for M1.** Do not add a no-op or
`ktime_get()`-based callback to silence the INFO line: `drm_vblank_init()`
would then print `Driver supports precise vblank timestamp query.` while the
returned timestamp is no more precise than the fallback, which is a false
capability claim.

Revisit only if all of the following become true:

1. KWin is actually observed to misbehave on this driver in a way traceable
   to timestamp jitter (recurring `Got invalid timestamp` or
   `Pageflip timed out`, or measured cadence error), not just the one-time
   kernel INFO.
2. A KMS boot is available for testing and the DECON line counter can be
   sampled, so the 0..`vtotal-1` range, the vblank sign, and the VSYNC
   phase are measured rather than assumed.
3. A build/test slot exists to verify the `hwmode`/constants change against
   real modesets.

If those conditions are met, the patch is small (the five points in section
3) and can be staged as a debug-first change: log `linecnt`/`vstatus` from
`get_scanout_position` for one boot, prove the mapping, then enable the
callback. Its number would be `0133-kms-vblank-timestamp.patch` (0132 is
already the manual page-flip state patch).

---

## 7. Evidence and limits

Commands run (all read-only; no kernel build, flash, or reboot):

```
# kernel core and driver references
grep -n "No driver support for vblank timestamp query\|get_vblank_timestamp\|get_scanout_position" \
        <vendor-3.10.108>/drivers/gpu/drm/drm_irq.c
sed -n '480,790p;1353,1405p' <vendor-3.10.108>/drivers/gpu/drm/drm_irq.c
grep -n "drm_calc_timestamping_constants" <vendor-3.10.108>/drivers/gpu/drm/drm_crtc_helper.c
grep -rn "get_vblank_timestamp\|get_scanout_position" <vendor-3.10.108>/drivers/gpu/drm/exynos
grep -n "get_vblank_counter\|enable_vblank\|disable_vblank\|drm_vblank_init" code/gpu31/kms/exynos_drm7580_main.c

# KWin 6.8.3
grep -rn "timestamp\|drmWaitVBlank\|DRM_VBLANK" /tmp/kwin-6.8.3/src/backends/drm
sed -n '83,88p;587,632p' /tmp/kwin-6.8.3/src/backends/drm/drm_gpu.cpp
sed -n '365,374p' /tmp/kwin-6.8.3/src/backends/drm/drm_commit_thread.cpp
sed -n '201,213p' /tmp/kwin-6.8.3/src/core/renderloop.cpp

# upstream comparison (master, fetched 2026-11-25)
grep -n "get_vblank_timestamp\|get_scanout_position\|drm_crtc_handle_vblank" \
        /tmp/exynos7_decon.c /tmp/exynos5433_decon.c
```

Live device checks (J7, `3.10.108 #37`, root via `/usr/local/bin/rootshell`):

```
$ uname -r
3.10.108
$ ls /sys/class/drm
renderD128
$ ls /sys/class/graphics
fb0
$ cat /sys/module/drm/parameters/timestamp_monotonic
1
$ cat /sys/module/drm/parameters/timestamp_precision_usec
20
$ mknod /tmp/mem c 1 1; dd if=/tmp/mem ...   # read fails: EPERM
```

Limits: this report does not claim that the DECON line counter cannot be
used, only that its mapping is unproven here and that the fallback already
satisfies KWin. No KMS boot was performed and no runtime timestamp sample
was captured.
