# gpu31-firstfault — first fault in a long run: the descriptor page is clean; the writer of word 0 is the JM; the cascade is recovery

Date: 2026-11-22 (device clock). Owner: G31_FirstFault. Device: int40
(`3.10.108 #37 SMP PREEMPT Sun Nov 22 15:12:15 EST 2026`, flashed image
`40bf9948…`). Mesa 20.3.5 at `/data/mesabuild/mesa20-b` (tree
`/data/mesabuild/mesa20`), driver 0001 only. All device work as root over
ssh under `flock /tmp/gpsdev.lock`; plain runs (never
`PAN_MESA_DEBUG=sync|trace`), **no reboot, no unbind**. Status: measurement
complete, driver change drafted (see §7) but **not built, not in patch 0001,
not flashed** — acceptance (0 ERR / 300 frames on silicon) is NOT met yet.

## 0. Verdict

1. **The descriptor page is not the fault, and `word0 == 1` does not exist on
   int40.** 300-frame runs, the dmesg-drained run with register polling, and
   a traced run: **every** dispatch's SUBMIT-time snapshot has words 0..3
   = 0 (`sw=00000000 00000000 00000000 00000000`), including the faulting
   descriptors. The int39 `word0 == 1` does not reproduce with the chain
   split in place; whatever wrote it then, it is not the initiator of the
   long-run fault.
2. **The writer of word 0 is the JM itself.** At a fault the descriptor holds
   `00010258 00000000 04e905c0 00000000` in words 0..3: word 0 =
   `exception_status` (`0x58` DATA_INVALID + status bits), word 2/3 =
   `fault_pointer` = the descriptor's own GPU address. The SUBMIT-time
   snapshot of the same 8 header words is byte-identical except words 0..3
   (zero). Not Mesa (`pan_pack(JOB_HEADER)` is a zero-initialised struct
   literal, so the CPU writes 0), not the split shadow, not a SUBMIT-time
   aliasing copy.
3. **The first fault is a fragment job (slot 0) that stops for good.** Two
   silicon signatures, both on the fragment job:
   * **silent**: `TIMEOUT slot=0 js_status=0x08 NO_EXCEPTION stat=0
     rawstat=0 head=tail=jc` — no IRQ ever, so the 5 s sched timeout is the
     only way out. `COMMAND_NEXT` had already drained (recovery's drain loop
     returns 0), so this is *not* a swallowed START.
   * **immediate**: `ERR slot=0 js_status=0x58 DATA_INVALID_FAULT` at
     dispatch with the descriptor intact and its payload correct
     (`word9 = 0x000f000f` = bounds 0,0..15,15 = full 256², `word10 =
     0x…480` = SFBD in the same 4 KiB page).
4. **The cascade is the recovery, not the fault.** After *any* GPU-level
   SOFT_RESET the next START latches `POWER_FAULT 0x41` on the other slot
   within ~1 ms with both slots reading 0 at `recover-done` (measured
   repeatedly: `latched=0x58 cycle=0` → 1 ms later `ERR slot=1 0x41`);
   the conditional power cycle then fires on that 0x41, resets the
   tiler/polygon state, and the next fragment job faults 0x58 again —
   a self-sustaining 0x58/0x41 storm until the process ends.
5. **Not power management, not DVFS.** Pinning
   `11400000.mali/power/control=on` (no runtime autosuspend) still faults
   (28 ERR/10 TIMEOUT, then 14/6); `did not idle` count is 0 in every run
   (the int39 fix holds). There is no GPU devfreq device on this kernel
   (`/sys/class/devfreq` has only `bus_int`, `bus_mif`, `bus_isp`), so
   `pf7580_set_rate` never runs at runtime.

## 1. Clean matrix (int40)

| run | frames | read | control | ERR | TIMEOUT | cycle=1 | wall | note |
|---|---|---|---|---|---|---|---|---|
| traced 256² | 60 | 0 | auto | **0** | 0 | 0 | 2.8 s | trace armed (3000), every `sw[0..3]=0` |
| A | 300 | 0 | on | 28 | 10 | 5 | 15.4 s | autosuspend disabled |
| B | 300 | 0 | on | 14 | 6 | 3 | 20.9 s | repeat |
| C | 300 | 0 | auto | 28 | 10 | 5 | 16.1 s | |
| D | 300 | 0 | auto | **0** | 0 | 0 | 14.3 s | clean run |
| E | 300 | 1 | auto | 0 | 0 | 0 | 10.6 s | frame 34 took **5031 ms** (the silent stall, no driver ERR) |
| F | 300 | 0 | auto | 34 | 12 | 5 | 16.1 s | with dmesg drain + register polling |

fps: 300 frames 256² no-readback = **18.6 fps** faulting, **21 fps** clean
(D). The GPU work itself is ~5 ms/frame; the rest is submit/`glFinish`
overhead. Fault frequency: first fault at seqno 99, 103, 291, 347 (per-file
seqno, 4 jobs/frame) → a random frame in 24..87, one fault per ~1-3 runs
of 300.

## 2. The descriptor page, at fault (run F, first fault)

```
ERR slot=0 js_status=0x00000058 (DATA_INVALID_FAULT) stat=0x00010000
    head=0x4e905c0 tail=0x4e905c0 seq=291 jc=0x4e905c0
    sw=00000000 00000000 00000000 00000000 00010013 00000000 00000000 00000000
JDBODY seq=291 jc=0x4e905c0  00010258 00000000 04e905c0 00000000
                             00010013 00000000 00000000 00000000
                             00000000 000f000f 04e90480 00000000 …
PTEWALK  leaf=… MATCH snap=0x4e90000/0x10000 live=0x4e90000/0x10000
         w=00010258 00000000 04e905c0 00000000 00010013 …
```

Decoded: word 4 `0x00010013` = `is64|FRAGMENT|index 1`; word 9
`0x000f000f` = bound min (0,0) / max (15,15); word 10 `0x04e90480` = SFBD,
in the same 4 KiB page (offset 0x480), 0x140 bytes below the descriptor.
The page belongs to a 64 KiB Mesa batch-pool BO (`h6=0x4e90000/0x10000` in
ERR-MAP, page ledger `count=4 mapcount=1 vma=1 pin=1 dirty`). The
mapping matches the live binding (`PTEWALK MATCH`), so neither a recycle
alias nor a stale TLB.

**Writer attribution:**
* Mesa: `pan_pack(…, JOB_HEADER, header)` builds `struct MALI_JOB_HEADER
  = { .is_64b = true }` and `MALI_JOB_HEADER_pack()` writes **all eight**
  words, so the CPU writes 0 to words 0..3 on every dispatch.
* Driver SUBMIT: the chain walk reads the header with clean-then-invalidate
  (`p7580_drv_gpu_read`) and copies it; the shadow path rewrites only words
  4..7 (`p7580_chain_hdr_isolate`). No kernel write to words 0..3.
* GPU: words 0..3 after the fault are exactly the JM's
  `exception_status/first_incomplete_task/fault_pointer` encoding.
  → the JM is the writer, and only on fault.

So of the ticket's three candidates (stale scoreboard marker reused as
exception_status / aliasing copy / CPU race with the descriptor write):
**none applies on int40.** The int39 `word0 == 1` (the only non-zero word 0
in 212 dispatches) was a pre-fault value that the chain split made
impossible to observe; it did not initiate the fault (the faulting
descriptor here has word 0 = 0 at SUBMIT).

## 3. The silent signature (run of the 300-frame baseline)

```
TIMEOUT slot=0 js_status=0x00000008 (NO_EXCEPTION) stat=0x00000000
        rawstat=0x00000000 head=0x3cc05c0 tail=0x3cc05c0 seq=111 jc=0x3cc05c0
        sw=00000000 … 00010013 …
recover-done slot=0 latched=0x00000008 cycle=0 … drain_rc=0
1 ms later: ERR slot=1 js_status=0x00000041 (POWER_FAULT) … seq=112 (WRITE_VALUE)
```
`drain_rc=0` proves `COMMAND_NEXT` was already 0 → the JM consumed the
START; `JOB_INT_STAT/RAWSTAT` are 0 → the JM never posted DONE or ERR;
`head==tail==jc` and status 0x08 (no ACTIVE bit) → the slot is idle from the
JM's point of view while the software still holds the job in flight. The
fragment job either (a) waits forever on the tiler-heap producer/consumer
handshake (the SFBD's tiler descriptor names the polygon list and the 1 MiB
`dev->tiler_heap`, and a fragment job that never sees the tiler's "done"
marker just polls), or (b) was killed by a power event. The observed
`POWER_FAULT` is always *one job later* on the other slot, i.e. it is the
reset's own footprint (§4), so (a) is the leading explanation; this needs the
first-fault page dump (SFBD + heap header) to be decided, which is the
instrumentation drafted in §7.

## 4. The cascade, and why recovery makes it worse

Every recovery that ends in a plain GPU SOFT_RESET leaves the JM in the
power-fault condition: after `recover-done … cycle=0 js0=0 js1=0`, the very
next START (on the *other* slot, ~1 ms later) reports `0x41 POWER_FAULT`
with `stat=0x00020000` (ERR bit). Then:

```
ERR slot=1 0x41 → recover-done cycle=1 (power cycle) → js0=js1=0
→ ERR slot=0 0x58 (fragment, same page) → recover-done cycle=0
→ ERR slot=1 0x41 → cycle=1 → ERR slot=0 0x58 → …
```
34 ERR / 12 TIMEOUT per 300 frames, i.e. 0x58 and 0x41 strictly alternate and
the chain's WRITE_VALUE/VERTEX/TILER nodes never get to run (they are
hard-stopped while queued), so the tiler never rewrites the polygon list and
the fragment job keeps faulting. The power cycle *is* clearing the 0x41
(`recover-done js0/js1 = 0`), but the *unconditional* soft reset of the next
recovery re-creates it. This is the mechanism the int39 report left as
"a latched POWER_FAULT survives a reset": it is not merely surviving, it is
being **re-latched by the reset** and the conditional cycle only fires one
fault too late.

Consequence for the fix: recovery must **always** end in a complete
power-on (reset + cores), never in a bare GPU reset, otherwise one fault
always costs a burst.

## 5. What is ruled out (silicon A/B, int40)

| variable | test | result |
|---|---|---|
| runtime autosuspend | `power/control=on` for the whole run | still faults (28/10, 14/6) |
| cores powered off under load | `did not idle` count | **0** in all 6 runs (int39 fix holds) |
| GPU DVFS | `/sys/class/devfreq` | no GPU device; only bus_int/mif/isp |
| descriptor contents | SUBMIT snapshot vs fault dump | identical except HW-written words 0..3 |
| descriptor mapping | PTEWALK | `MATCH`, snap == live, same BO size |
| lost START | recovery drain (`COMMAND_NEXT`) | drained before recovery (`drain_rc=0`) |
| Mesa write of word 0 | `MALI_JOB_HEADER_pack` | always writes 0 |

## 6. Residual unknowns (what the dump must answer)

1. The SFBD at `jc-0x140`: are `heap_start/heap_end/polygon_list` (midgard
   tiler descriptor) inside the submitted BO set? A stale/garbage polygon
   list would explain both signatures.
2. The 1 MiB tiler heap's first page at fault time: does it hold a valid
   tiler "done" marker for the frame's polygon list (producer/consumer
   handshake), or a marker left over from a frame the fragment job already
   consumed?
3. Whether a fragment job ever *starts* before its tiler has completed: the
   cross-slot ordering is only a 0004 shim dependency
   (`serial_prev`), and the shim's pump "gates only the FIFO head on
   dep_fence". The `serialize_fs` A/B (§7) decides this with one boot.

## 7. Driver change drafted (NOT built, NOT flashed)

Scratch tree (editable copy of driver 0001):
`/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31/firstfault/t/`
(patch 0001 applied there; **patch 0001 itself has not been regenerated**, so
the repo state is unchanged).

Implemented there:
1. **Event ring** (`struct p7580_ev ev[128]`, `p7580_job_ev`) in RAM, no
   printk on the hot path, recording START (armed: JS_STATUS before,
   `COMMAND_NEXT` readback after, JS_STATUS after, previous in-flight
   occupant), DONE, ERR, TIMEOUT, RECOVER, CYCLE, and the suspend/resume
   verdicts. One-shot dump at the first fault/timeout
   (`p7580_job_first_fault`), armed by a new `p7580/ev_arm` debugfs knob.
2. **4 KiB descriptor-page dump** (`p7580_drv_desc_dump`, registered via
   `p7580_job_set_desc_dump`): 16 lines × 16 words, one line per 256-byte
   slot, for `jc`, the live `src_jc` when the job ran from a shadow, the
   fragment payload's framebuffer address (SFBD + tiler descriptor), and the
   first page of the largest BO in the submit snapshot (1 MiB tiler heap).
   `src_jc` is newly stamped on every split node.
3. **Descriptor hygiene**: words 0..3 are zeroed in every shadow copy, and a
   single-node chain whose source header has a nonzero word 0..3 is now
   routed through a shadow (with a `desc-stale` log line), so a recycled
   page's stale status can never be fed back to the JM. (Not the int40
   cause, but it removes the int39 hazard class for good.)
4. **Recovery ends in a full power-on**: the bare GPU reset is replaced by
   the core power cycle whenever the hook is wired (§4), so no recovery can
   leave the JM in the power-fault condition. `recover-done` keeps reporting
   `cycle=1`.
5. **`serialize_fs` debugfs knob (A/B)**: queues the FS (slot 0) submit on
   the chain slot's entity so the fragment job is ordered behind the tiler
   by one FIFO instead of a cross-entity shim dependency; the hardware slot
   stays 0. One boot tests the ordering hypothesis: 300 frames at 0
   (expected: fault + dump) and at 1 (expected: 0 ERR).

## 8. Next steps (blocked on a build+flash, which Main owns)

1. `git apply --check` the regenerated 0001, build the Image with
   `code/gpu31/series/build-int41.sh` (copy of int40, driver 0001 changed),
   run `make -C code/gpu31/driver check`, and hand the image to Main to flash.
2. On the flashed image: `dmesg -c`, `echo 1 > …/p7580/ev_arm`, then
   300 frames 256² no-readback; expect exactly one `PAGEDUMP`/`EVRING`
   capture at the first fault, with `serialize_fs=0`.
3. Repeat with `serialize_fs=1` ×3: acceptance is `ERR=0`, `TIMEOUT=0`,
   `cycle=1=0` over 300 frames, plus fps before/after
   (18.6 fps faulting / 21 fps clean measured here).
4. Then walk the `PAGEDUMP`/`EVRING` capture for the SFBD's
   `polygon_list`/`heap_start` and the tiler heap's first page; if the
   ordering A/B is the answer, keep `serialize_fs` as the default (1) and
   delete the knob.
