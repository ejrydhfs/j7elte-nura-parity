# integration49 rebuild (gpu31-perf overlay fix: p7580/prof_arm must reuse
# the driver's existing debugfs dir -- int48's init silently returned on
# -EEXIST, so the knob never existed on the flashed image)
# on top of int48 (perf/diagnostic overlay) and int47 (fpssplit shim).
# Consumes 07443e8 + this ticket's overlay delta (authored here on Main's
# report; overlay sha now 7c76b412...):
#   - int48's p7580_prof_debugfs_init() called
#     debugfs_create_dir("p7580", NULL). The job slice already created
#     that dir in p7580_job_dev_init (stored in jd->dbg_dir), and a second
#     create returns -EEXIST -- an ERR_PTR -- so IS_ERR_OR_NULL() returned
#     early and prof_arm was never created. Runtime evidence on the
#     flashed int48: `ls` showed chain_mode/dump/ev_arm/hang_inject/
#     iova_arm/job_timeout_ms/serialize_fs/trace_arm but no prof_arm.
#   - the init now reuses `(struct dentry *)drv->jd.dbg_dir` (the job dev
#     init runs earlier in probe), and every failure is logged:
#     "p7580perf: prof_arm unavailable (no p7580 debugfs dir)" or
#     "p7580perf: prof_arm knob failed (%ld)"; success logs
#     "p7580perf: prof_arm ready" so dmesg proves the knob exists.
#   - step 6aj pins the reuse (no debugfs_create_dir in our code), the
#     non-silent error paths, the probe ordering (job dev init before
#     prof init), the strings in submit.o/Image, and the debugfs file.
# Everything int48 gated is unchanged; driver 0001 stays 04db08d.
# ----- int48 (consumed unchanged apart from the overlay delta) -----

# Consumes 078daa3 (driver 0001, sha256 20cd0884...; supersedes
# 8a493b5):
#   - p7580_drv_map_bo no longer takes drm_gem_object_reference() for its
#     bo_list entry, and neither p7580_gem_gpu_unmap() nor the main.c
#     remove walker unrefs it. The entry is NON-OWNING. The old
#     list-owned reference could never be dropped: its only unlink runs
#     in free_object -> gpu_unmap, and free_object only runs at refcount
#     zero, which the list reference itself prevented -- every submitted
#     BO (pages + private shmem inode + IOVA) leaked forever (measured:
#     1.16 MB/frame shmem).
#   - new static p7580_drv_gem_try_get() = kref_get_unless_zero(), used
#     by p7580_drv_gpu_span()'s live lookup and the PTEWALK live-view pin
#     (both under bo_lock): "entry still linked" no longer implies a live
#     object during the free_object window, and try_get loses that race
#     cleanly instead of resurrecting a dying BO. Callers that already
#     hold a reference (handle lookup, job->bos) need none.
#   - comments only elsewhere (drv.h struct p7580_drv_bo, gem.c/gem.h).
# Step 6af pins the weak-entry invariant + try_get; the 6m ptewalk gate is
# retargeted from drm_gem_object_reference(le->obj) to try_get. Every
# other 6k/6m assertion (gpu_unmap before iova_free, T/U linkage, map_bo's
# two invalidates, PTEWALK format, unref of the pinned live_obj) holds.
# ----- int44 (consumed unchanged apart from 6m) -----

# in serial order; fault capture with fbd/jc_age).
# Consumes 98640d5 (driver 0001, sha256 1c295834...):
#   - dma_resv_wait_timeout() returns >0 (idle, jiffies left), 0 (budget
#     ran out) and <0 (error). panfrost_ioctl_wait_bo mapped only <0 to
#     an error and returned 0 for BOTH 0 and >0, so a 0-jiffy poll (Mesa's
#     BO-cache fetch: panfrost_bo_wait(bo, 0, RW)) was always told
#     "ready" -- even for a BO an in-flight submit still owned. Measured
#     on int42 with hang_inject=4 + job_timeout_ms=60000: the submit
#     fence stayed pending 60.1 s while the same 64 KiB BO was
#     PUT/FETCHed 4 times.
#   - the handler now maps 0 to -ETIMEDOUT (v5.2 panfrost_wait_bo
#     contract) and keeps <0 propagation and >0 success.
#   - host T11 pins the three-way mapping.
# Step 6ae pins the one new return and its position after the
# p7580_gem_wait() call; every int43 gate (publish order, capture,
# timeout/hang) is unchanged by this commit.
# ----- int43 (consumed unchanged) -----

# deterministic silent-hang injection).
# Consumes 3104a90 (driver 0001, sha256 58972560...):
#   - the 0002 shim's dma_resv_add_excl_fence REPLACES the resv's single
#     exclusive fence, so the last publisher must be the newest submit.
#     run_job's async re-publish (gpu31-instrtype "arm-time" copy) could
#     land an OLDER node fence after a newer SUBMIT's fence; Mesa's
#     BO-cache WAIT_BO then saw the BO idle while its job was in flight
#     and rebuilt the next frame into it (torn SFBD -> silent hang ->
#     permanent 0x58 cycle: the int42 storm). The publish now lives only
#     in SUBMIT, inside df->serial_lock, still before the ioctl returns.
#     publish_excl and the run_job stamp are gone; the queue-time
#     get_excl leg still orders later submits behind it.
#   - fault capture: job->fbd (descriptor word 10 read at the chain walk
#     for fragment nodes; desc[] is 8 words, so the old desc[10] read was
#     out of bounds) and job->jc_age (SUBMITs since this file last used
#     the BO holding jc, via a bounded 64-entry direct-mapped ledger).
#     EVRING begin gains fbd/jc_age, TIMEOUT gains jc_age, and the PAGE
#     dump now prints 64 lines of 64 bytes with the true offset
#     (slot = i/4, off = i*64) instead of 16 lines with wrong offsets.
# Step 6ad pins the single serial publish + capture; 6j/6z/6ab are
# retargeted to the new shape (the old "2 add_excl sites"/run_job bl/
# "+%03x PAGE"/desc[10] assertions would fail on correct code).
# ----- int42 (consumed unchanged apart from those retargets) -----

#   - P7580_JOB_TIMEOUT_MS 5000u -> 750u in panfrost7580_jm_hw.h. The
#     value is the drm_sched dispatch budget (copied into entity->timeout
#     at entity_init, armed from it at dispatch), so a silent hang (no
#     IRQ, NO_EXCEPTION 0x08) is broken in <1 s including one recovery
#     instead of the old 5 s; 750 ms is still 2x the worst measured frame
#     (512^2 readback, 373 ms of CPU detile);
#   - runtime seed p7580_drv_job_timeout_ms (default 750) passed to
#     drm_sched_init; debugfs p7580/job_timeout_ms get/set (clamp
#     100..60000 ms, updates the live sched->timeout and seeds entities
#     created later);
#   - debugfs p7580/hang_inject: write N to mask the slot's DONE+ERR bits
#     at the next N STARTs -- the deterministic form of the silent hang
#     (job runs/retires in hardware, no IRQ, fence waits the timeout);
#     0 (default) leaves the mask untouched and recovery's dev_rearm
#     restores it;
#   - job.h hang_arm + get/set_timeout_ms/timeout_priv, the
#     p7580_job_set_timeout_hooks() forwarder, and main.c probe wiring of
#     the driver's accessors.
# Step 6ac pins all of it in tree, driver 0001 mirror, job.o/submit.o,
# System.map and the linked Image; the int41 fault-latency gate still
# asserts the shim owns exactly one delayed-work arm and one kick (the
# budget now comes from entity->timeout, no shim edit this ticket).
# ----- int41 (consumed unchanged) -----


#   - pf7580_cores_off_locked() fails closed: it powers the cores back on
#     and returns -EBUSY instead of letting runtime_suspend gate the clock
#     and drop pd-g3d under a busy JM (which latched JS_STATUS POWER_FAULT
#     for the victim job; measured 22 power-offs with "did not idle" per
#     bench run and 36/72/144-job fault cascades afterwards);
#   - pf7580_runtime_suspend() propagates that -EBUSY, and
#     p7580_drv_runtime_suspend() refuses up front while !job_is_idle();
#   - p7580_job_is_idle() also requires no present slot ACTIVE in JS_STATUS
#     (bit 0), except our own HARD_STOP result 0x03;
#   - new pf7580_power_cycle(): cores off + clock gate + the full power-on
#     shared with resume; exported to the jobs slice as the power_cycle
#     hook. int40 made p7580_job_recover() cycle when the latch was
#     POWER_FAULT (0x41) or when a non-stop fault survived the GPU reset;
#     int49 replaced that predicate with "always, whenever the hook is
#     wired" (6aa retargeted), then reprograms AS0 and re-arms JOB_INT;
#     resume clears a latched fault with the same cycle;
#   - always-on recovery telemetry: "recover-done slot/latched/cycle/js0/
#     js1/intmask/drain_rc" (plus recover-cycle FAIL / resume-cycle).
# Step 6aa pins the applied tree, the patch mirror, the objects, the linked
# Image and the host T9 cycle-decision table; every int39 gate stays.
#
# integration38 rebuild (gpu31-defectb: clean+invalidate every SHMEM
# page at allocation) on top of int37 (gpu31-chainsplit). Consumes
# 9dd7542: p7580_shmem_alloc() calls
# __flush_dcache_area(page_address(page), PAGE_SIZE) after EACH
# shmem_read_mapping_page(), because this arm64 tree's
# flush_dcache_page() only clears PG_dcache_clean (no clean/invalidate),
# so shmem_getpage_gfp()'s clear_highpage() zeroes stay as a dirty line
# in the kernel's cached linear alias while the BO is mapped WC
# (MT_NORMAL_NC) to userspace; the stale line later evicts over the
# NC-written ISA bytes (gpu31-defectb: whole-page zero loss with every
# reference intact). The fix is mirrored byte-for-byte into driver 0001,
# so the applied panfrost7580_gem.c is what the 6x gates prove (tree
# order, object reloc/order, vmlinux call site).
#
# 742dc52 (gpu31-chainsplit follow-up) is consumed too: the VERTEX
# (COMPUTE_JOB) aggregate is MALI_COMPUTE_JOB_LENGTH 192 B, not 96 B, so
# the splitter's shadow copy carries the whole Draw block (generated
# lengths: VERTEX 192, WRITE_VALUE 56, TILER/FUSED 192, FRAGMENT 64,
# NULL 32). The 6y gate pins the applied jm_hw.h and the host T1 test
# pins 56/192/192/64/32.
#
# The defectb hand-off (9dd7542) did not build as committed: it inserted
# the 20-line flush comment into the gem.c body without advancing that
# file's new-file hunk header, still `@@ -0,0 +1,1638 @@` for 1658 '+'
# lines. git apply honours the declared count and drops the tail, so the
# applied panfrost7580_gem.c ended at line 1638 without
# fail_unmap/fail_detach and the closing brace, and the build died with
# `gem.c:1638:17: error: expected declaration or statement at end of
# input` + `label 'fail_detach' used but not defined`. int39 fixes the
# header (1638 -> 1658; content unchanged) and refuses the whole class
# before applying anything: every new-file hunk's declared +count is
# compared with its actual '+' lines. It also fails the gate on a
# non-zero make rc or an Image older than the build start (the pristine
# tree ships a prebuilt Image/System.map, which the first attempt of
# this ticket briefly read as the build's output).
#
# gpu31-latency (this ticket): the reported ~40 ms/frame is not steady
# state. On int38, a clean 256x256 gfdemo 300-frame run is 3.3-3.5 ms
# (286-305 fps); a run that catches a job fault reports 24-29 fps
# because the IRQ ERR-without-DONE leg never signalled the job fence --
# it waited the full 5 s drm_sched timeout (timedout_job -> recover).
# Two stalls x 5 s + 300 x 3.5 ms = 10.4 s = 28.7 fps, exactly the
# reported figure; readback runs stall the same way per frame. int39:
# p7580_submit_job carries an opaque sched_job backpointer (stamped at
# queue time), the IRQ fault leg calls drm_sched_job_kick_timeout(),
# and the 0004 shim re-arms the job's timeout_work at delay 0 on the
# same ordered workqueue -- recovery still signals the fence and keeps
# FIFO order, but in milliseconds; the 5 s timer remains the backstop.
# No fence/BO lifetime or sync ordering changes.
# ----- int37 (consumed unchanged) -----
# integration37: driver-side per-node chain split. p7580_drv_submit
# walks the submitted jc chain through the
# descriptor `next` pointers, copies every node into a driver-owned
# shadow BO slot with next=NULL, dep1=dep2=0 and index=pos+1, and queues
# one sched job per node on the SUBMIT's existing slot entity. Node i
# depends on node i-1's armed fence, so every node is a fresh START and
# p7580_js_config() (START/END_FLUSH_CLEAN_INV) brackets each stage.
# The UAPI out_sync is installed exactly once, on the LAST queued node's
# fence (partial queue failure: the last node the GPU accepted), and
# that fence becomes the file's serial predecessor. A one-node chain
# with dep1==dep2==0 (Mesa's fragment submit) keeps the original
# descriptor and allocates no shadow BO. Per-node BO refs and a
# job-lifetime PM ref are taken per queued node and released in that
# node's free_job; the last retiring node unmaps the shadow. Consumes
# 6fe2491 (gpu31-chainsplit) on top of int36 (de21448 mmaprefs).
#
# The chainsplit hand-off did not compile as committed; int37 fixed
# three defects in driver 0001 before building (gpu31-integration37.md
# documents the fixes and the build error evidence):
#   - the submit.c hunk header declared 1965 lines for a 1966-line file,
#     so git apply truncated the last line (a closing brace) -> header
#     updated to the true count (1969 after the two source fixes);
#   - `fence_ref = dma_fence_get(...)` assigned a void expression (this
#     tree's 3.10 shim defines dma_fence_get() as void) -> bind the
#     pointer, then take the reference in a statement;
#   - the rewritten SUBMIT still `goto err_inf;`/`goto err_objs;` but
#     both labels were lost in the rewrite -> restored on the common
#     in-fence/BO put loops.
#
# All prior gates (6b/6c/6e/6f/6g/6h/6i/6j/6m/6n/6o/6q/6s/6v) unchanged;
# 6k/6l/6p/6r/6t/6u retargeted to the new code shape with the same
# invariant (map_bo's 2 invalidates now live in the outlined symbol,
# out_sync installs the split's last_fence, the descriptor snapshot is
# the isolate+memcpy path, bo_count is s->nr per node, the serial
# publish is guarded by `queued`, the descriptor read is
# p7580_drv_gpu_read); + new 6x defectb gates (applied-tree call order
# read -> flush -> page store inside the per-page loop, exactly one
# __flush_dcache_area reloc in gem.o inside the shmem_read_mapping_page
# symbol and after the read, U/T linkage, and the same one call site in
# the linked vmlinux) + 6w chainsplit gates (chain walk/isolate/
# shadow/per-node queue/single out_sync/one-node pass-through/per-node
# cfg/per-node lifetime in tree; inlined-fingerprint + shadow_put/
# gpu_read symbols in submit.o; hw_submit cfg immediate in job.o; host
# chainsplit_test; Image strings).
# submit.c hunk integrity (late function
# panfrost_ioctl_submit_override in tree/object/System.map).
# gem_vm_ops mmap 5d97c65, vm_pgoff fix 5f8ecd2, shmem-backed CREATE_BO
# dcd5171, job-lifetime PM model c81e8a7, teardown PM guards 5c52f1f + close fix 476b75d
# + jobtrace 1244082: bounded dispatch/IRQ/fence trace, out_sync
# NULL-install fix, completion dma_sync_single_for_cpu (gpu31-coherency:
# bind-time dma_map + SUBMIT sync_for_device + free_job sync_for_cpu;
# flush_dcache_page was a no-op on arm64/no-HIGHMEM) + jobtrace2 1450d16:
# run_job out_sync install retired by armfence 49f7df2 (F-JT1 stash gone:
# alloc at arm, SUBMIT-time replace_fence of the armed fence, run_job
# binds the same fence, pump enforces identity),
# job->bos refcount transfer on SUBMIT (F-JT2 active) + schedtrace 8fd555e:
# bounded ST-IOCTL/ST-QUEUE/ST-RUN/ST-PMGET/ST-RETIRE probes in submit.c
# and ST-PUSH/ST-PUMP/ST-FENCE in the 0004 shim (budget 8 each) + pumpfix de07c37:
# 0004 pump preserves SUBMIT slot, ST-OPS (ops/run_job/slot) + ST-FINISH (ran/slot)
# probes, NULL-guarded run_job call, IS_ERR_OR_NULL/ran stamp, retire-after-fence;
# driver stamps sched_job->slot at SUBMIT, run_job trusts it + opsfix df7c4b9:
# ops/slot/ran BUILD_BUG_ON pins in both TUs (ops=24/slot=232/ran=236),
# alloc round-trip BUG_ON, ST-OPS opsoff probe, submit call-site guard
# + probenumeric 3bcffae: numeric %lu ST-OPS
# (job/opsoff/ops/run/runexp/owner/slot) with runexp= from
# p7580_drv_run_job_addr() (weak NULL default in shim, strong in submit.c),
# ST-FINISH owner=%lu, ST-RUN-entry first-statement print, run_seq stamp,
# file_priv/drv_priv stamped at SUBMIT (run_job/timedout/free read stamps;
# entity math range-checked fallback only), per-guard run_bail
# NULLJOB/BADSLOT/BADENT/NODRV prints, ST-RETIRE ran=/owner=)
# + irqfix d852459 (SUPERSEDED routing: req==0 -> JS1 per 2936796):
# STAT|RAWSTAT IRQ demux ((status|rawstat) & m), JOB_INT_CLEAR re-arm,
# AS0 program_as + p7580_job_dev_rearm() on recover/resume, irq-arm
# (rawstat/stat/mask/js_present) + irq-enter (rawstat/stat) + post-start
# (rawstat/js_status) + irq/irq-err/fence-signal numeric probes + d0+d4.
# Copy of build-int40.sh with int49 names. Two changes against int40:
#   - step 6aa is retargeted to the gpu31-firstfault recovery rule: a
#     recovery ALWAYS ends in a complete core power-on whenever the hook
#     is wired (never a bare GPU soft reset), for every latched status
#     including the silent NO_EXCEPTION 0x08 and our own stop codes.
#     The int40 predicates (cycle only on POWER_FAULT 0x41 or on a
#     latched fault surviving the reset) are now the failure, not the
#     gate: measured on int40, a bare soft reset re-latches 0x41 on the
#     next START and turns one fault into a storm.
#   - new step 6ab first-fault gates: 128-entry in-RAM event ring +
#     one-shot 4 KiB page dump behind debugfs p7580/ev_arm (jc, live
#     src_jc, SFBD, 1 MiB tiler heap), descriptor words 0..3 hygiene in
#     the shadow copy + desc-stale shadowing of a recycled single-node
#     chain, and the serialize_fs A/B knob (default 0: FS submit rides
#     the chain-slot entity; hardware slot unchanged).
# Unchanged int40 step 6aa power gates (unchanged
# numeric ST-OPS/runexp, ST-RUN entry, 4 run_bail kinds + run_fallback,
# ST-RETIRE ran/owner, ST-FINISH owner, file_priv/drv_priv stamps,
# weak/strong run_job_addr linkage; JS1 zero-req (2936796), STAT|RAWSTAT demux,
# re-arm/program_as in recover+resume, job probes) + new step 6e
# ABI-align gates (de142b3: BUILD_BUG_ON ABI gates in syncobj + ioctl
# TUs, SYNC_FILE bit-0 tolerance on HTOF/FTOH with other bits rejected,
# dispatcher copy-in truncate / copy-back cap) + 6d freshness, step 8
# + gpuwrite (int20): Zero-fill pattern-test fix (v4/v5 genxml
# has no Immediate-32; T720 is PAN_ARCH 4) + DEBUG_READ_BO 0xCF fops
# route with p7580_gem_debug_read_word (reuses job_done_sync).
# Step 4b (PM-ordering overlay) retired: the
# job-lifetime PM model (live-job ref armed before drm_sched_job_submit,
# released in free_job; re-get/put in run_job/timedout_job; probe
# power-first; suspend -EBUSY while non-idle) plus teardown holds
# (teardown_hold/teardown_release/unmap_bo for postclose/file_fini/remove)
# are folded into driver 0001 (c81e8a7 + 5c52f1f), so no
# code/gpu31/pm-order/*.patch overlay exists or is needed.
# Syncobj ioctl (G31_SyncobjIoctl) is NOT waited for: build without it if
# absent (probe fills num_ioctls from the driver ioctl table at probe).
# Fresh scratch copy of the pristine kernel tree; host-side only (-j4
# allowed). Scratch tree, TMPDIR and ALL outputs live on the external
# disk (host / and /tmp are full/tmpfs): nothing is written to /tmp or /.
# Packs boot.img with the STOCK dt.img + reference ramdisk (never rebuilt),
# staged onto the external disk (read-only seed copies from /tmp/bimg).
EXT=/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31
REPO=${REPO:-$(cd "$(dirname "$0")/../../.." && pwd)}
PRISTINE=${PRISTINE:-/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07}
SCR=${SCR:-$EXT/scratch-int49}
LOG=${LOG:-$EXT/logs/gpu31-int49-build.log}
EVID=${EVID:-$EXT/logs/gpu31-int49-evidence.log}
export TMPDIR=${TMPDIR:-$EXT/tmp}
mkdir -p "$TMPDIR"
: > "$EVID"

rm -rf "$SCR"
cp -a "$PRISTINE" "$SCR"
cd "$SCR"

# Source provenance: driver 0001 is the only patch that moves between
# tickets (its gem.c/.h/submit.c sections are this ticket's source).
# Record the exact bytes applied; 6v refuses if the file changed while
# the build ran (a silent mid-build amend would make the report lie).
DRV1=$(echo "$REPO"/code/gpu31/driver/0001-*.patch)
DRV1_SHA=$(sha256sum "$DRV1" | cut -d' ' -f1)
REPO_HEAD=$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)

# Patch integrity: a new-file hunk's declared +count must equal its actual
# '+' line count. git apply honours the declared count and silently drops
# the tail when it under-declares, so a truncated file compiles into a
# "declaration at end of input" error at the last line. Seen in int37's
# submit.c, the defectb gem.c (1638 for 1658), and int49's fpssplit shims
# (dma-fence.c 235 for 253, dma-resv.c 300 for 307) before the repair
# commit. Every patch the build applies is scanned here, not just driver
# 0001, and the build refuses before applying anything.
HUNK_BAD=""
for pf in "$REPO"/patches/drm-backport/000[1-5]-*.patch \
	  "$REPO"/code/gpu31/kms/0001-*.patch \
	  "$REPO"/code/gpu31/dmabuf/0001-*.patch \
	  "$REPO"/code/gpu31/driver/000[12]-*.patch \
	  "$REPO"/code/gpu31/perf/0001-*.patch; do
	[ -f "$pf" ] || { echo "applied patch missing: $pf" | tee -a "$EVID"; exit 1; }
	one=$(awk -v name="$(basename "$pf")" '
/^diff / { if (hdr && cnt != hdr) printf "%s line %d: declared %d, plus-lines %d (%s)\n", name, hl, hdr, cnt, file; hdr=0; cnt=0; file=""; next }
/^\+\+\+ / { file=$0; next }
/^@@ -0,0 \+1,([0-9]+) @@/ { hdr=substr($0, index($0,"+1,")+3); sub(/ @@.*/,"",hdr); hl=NR; cnt=0; next }
hdr && /^\+/ { cnt++ }
END { if (hdr && cnt != hdr) printf "%s line %d: declared %d, plus-lines %d (%s)\n", name, hl, hdr, cnt, file }
' "$pf")
	[ -n "$one" ] && HUNK_BAD="$HUNK_BAD$one"
done
if [ -n "$HUNK_BAD" ]; then
	echo "int49 GATE FAIL: new-file hunk count mismatch (git apply would truncate)"
	echo "$HUNK_BAD" | tee -a "$EVID"
	exit 1
fi
echo "patch inventory: $(ls "$REPO"/patches/drm-backport/000[1-5]-*.patch "$REPO"/code/gpu31/kms/0001-*.patch "$REPO"/code/gpu31/dmabuf/0001-*.patch "$REPO"/code/gpu31/driver/000[12]-*.patch "$REPO"/code/gpu31/perf/0001-*.patch | wc -l) patches, all new-file hunk counts exact" | tee -a "$EVID"

# 1. backport shims 0001-0005 (patches/drm-backport, f698920 + format repair)
for p in "$REPO"/patches/drm-backport/000[1-5]-*.patch; do
	git apply --check "$p" && git apply "$p"
done
# 2. Mode-B v2 render-ONLY node (02bd790: DRIVER_RENDER_ONLY, no card0)
git apply --check "$REPO"/code/gpu31/kms/0001-drm-3.10-render-node-mode-b.patch \
&& git apply "$REPO"/code/gpu31/kms/0001-drm-3.10-render-node-mode-b.patch
# 3. dmabuf CMA writecombine port (REQUIRED: arm64 has no
#    dma_alloc_writecombine; int4 §4 item 1 — fatal without it)
git apply --check "$REPO"/code/gpu31/dmabuf/0001-drm-gem-cma-use-dma-attrs-writecombine.patch \
&& git apply "$REPO"/code/gpu31/dmabuf/0001-drm-gem-cma-use-dma-attrs-writecombine.patch
# 4. panfrost7580 render-only driver (02bd790 on top of 20ea233:
#    submit deadlock/timeout fixes + open sched-ensure + render-only
#    DRIVER_RENDER_ONLY companion; gem_vm_ops mmap 5d97c65; vm_pgoff
#    fix 5f8ecd2; shmem-backed CREATE_BO dcd5171; job-lifetime PM
#    model c81e8a7 + teardown PM guards 5c52f1f + close fix 476b75d +
#    jobtrace 1244082 + jobtrace2 1450d16 (run_job out_sync install,
#    free_job job_done_sync walk, job->bos refcount transfer) folded
#    into driver 0001; opsfix df7c4b9 regen: submit.c +1,897 with
#    layout pin + call-site guard).
for p in "$REPO"/code/gpu31/driver/000[12]-*.patch; do
	git apply --check "$p" && git apply "$p"
done

# 4c. gpu31-perf overlay (ff078f3): a SEPARATE patch applied after driver
# 0001/0002, so driver 0001 stays 04db08d. debugfs p7580/prof_arm + the
# submit_err error-only counter; inert when unarmed.
OVL=$REPO/code/gpu31/perf/0001-panfrost7580-prof-overlay.patch
[ -f "$OVL" ] || { echo "int49 GATE FAIL: perf overlay missing ($OVL)" | tee -a "$EVID"; exit 1; }
OVL_SHA=$(sha256sum "$OVL" | cut -d' ' -f1)
[ "$OVL_SHA" = "7de60d039f22fd039ed34eed607ecc22a9035db0750941cef50cd1e59860ff37" ] \
	|| { echo "int49 GATE FAIL: perf overlay sha changed ($OVL_SHA)" | tee -a "$EVID"; exit 1; }
git apply --check "$OVL" && git apply "$OVL"
echo "perf overlay applied: $OVL_SHA" | tee -a "$EVID"

# 4b. retired (was PM-ordering overlay in int6): folded into driver 0001
# at c81e8a7 (job-lifetime PM) + 5c52f1f (teardown holds). Kept as a
# log line only.
echo "int49: PM-ordering folded in driver 0001 (c81e8a7 + 5c52f1f); no overlay applied"

# 5. config: pmaports base + d0-m1-minimal + d4-panfrost7580, then closure
cp "$REPO"/code/pmaports/device/archived/linux-samsung-j7elte/config-samsung-j7elte.aarch64 .config
for f in "$REPO"/code/gpu31/config/fragments/d0-m1-minimal.fragment \
	 "$REPO"/code/gpu31/config/fragments/d4-panfrost7580.fragment; do
	grep -v '^#' "$f" | grep -v '^$' | cut -d= -f1 | while read -r sym; do
		grep -v "^${sym}=\|^# ${sym} is not set" .config > .config.tmp && mv .config.tmp .config
	done
	grep -v '^#' "$f" | grep -v '^$' >> .config
done
yes "" | make ARCH=arm64 olddefconfig >/dev/null

# 5b. config closure gate: every non-comment line of d0-m1-minimal and
# d4-panfrost7580 must survive olddefconfig verbatim (the Image is only
# meaningful if the render-only driver and the M1 DRM base are set as the
# fragments ask). Record the closure in the evidence log.
CONFIG_MISS=""
CONFIG_N=0
for f in "$REPO"/code/gpu31/config/fragments/d0-m1-minimal.fragment \
	 "$REPO"/code/gpu31/config/fragments/d4-panfrost7580.fragment; do
	while IFS= read -r line; do
		[ -n "$line" ] || continue
		CONFIG_N=$((CONFIG_N + 1))
		grep -qxF "$line" .config || CONFIG_MISS="$CONFIG_MISS [$f] $line"
	done <<EOF
$(grep -v '^#' "$f" | grep -v '^$')
EOF
done
if [ -n "$CONFIG_MISS" ]; then
	echo "int49 GATE FAIL: config closure after olddefconfig:$CONFIG_MISS" | tee -a "$EVID"
	exit 1
fi
echo "config closure: $CONFIG_N fragment symbols exact after olddefconfig" | tee -a "$EVID"
grep -E '^CONFIG_(MODULES|DRM|DRM_KMS_HELPER|DRM_EXYNOS|DRM_EXYNOS_G2D|DRM_EXYNOS_DMABUF|DRM_PANFROST7580)=' .config | tee -a "$EVID"

# 6. build
BUILD_START=$(date +%s)
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS="-Wno-error" -j4 Image > "$LOG" 2>&1
BUILD_RC=$?
ls -l arch/arm64/boot/Image
sha256sum arch/arm64/boot/Image

# 6b. ops-layout + probenumeric gate (opsfix df7c4b9 + 3bcffae):
# compute the layout inside the build and FAIL (exit 1, scratch kept
# for diagnosis) on any disagreement. pahole-free: (a) offsetof getters
# compiled with the real kbuild include chain, immediates read back;
# (b) objdump of both objects for the compiler-emitted offset
# immediates; (c) nm single-definition + __ksymtab checks in objects
# and System.map; (d) applied-tree pin/round-trip/numeric ST-OPS +
# ST-RUN/bail/retire presence + Image strings; (e) weak/strong
# run_job_addr linkage (p7580_drv_run_job_addr in both objects'
# symtab, absolute in the linked Image System.map).
note() { echo "$1"; echo "$1" >> "$EVID"; }
gate_fail() { note "int49 GATE FAIL: $1"; exit 1; }
SCHED_O=drivers/gpu/drm/drm_sched.o
SUBMIT_O=drivers/gpu/drm/panfrost/panfrost7580_submit.o
# int49: the perf overlay splits three hot functions into a timing wrapper
# + _body so the unarmed path is a direct branch. Submit-internal gates
# target the body symbol, which is where the code they check now lives.
SUBMIT_BODY=p7580_drv_submit_body
MAPBO_BODY=p7580_drv_map_bo_body
DONESYNC_BODY=p7580_gem_job_done_sync_body
[ -f arch/arm64/boot/Image ] || gate_fail "Image missing (build failed; see $LOG)"
[ "$BUILD_RC" = 0 ] || gate_fail "make -j4 Image failed (rc=$BUILD_RC; see $LOG)"
[ "$(stat -c %Y arch/arm64/boot/Image)" -ge "$BUILD_START" ] \
	|| gate_fail "arch/arm64/boot/Image predates this build (stale artifact from the pristine copy)"
note "build rc=0, Image mtime $(stat -c %y arch/arm64/boot/Image)"
[ -f "$SCHED_O" ] || gate_fail "$SCHED_O missing"
[ -f "$SUBMIT_O" ] || gate_fail "$SUBMIT_O missing"
note "== int49 gate: objects present =="
ls -l --time-style=full-iso "$SCHED_O" "$SUBMIT_O" arch/arm64/boot/Image | tee -a "$EVID"
# (c1) single definitions: exactly one alloc, one run_job, one ops table
[ "$(aarch64-linux-gnu-nm "$SCHED_O" | grep -c ' T drm_sched_job_alloc$')" = 1 ] \
	|| gate_fail "drm_sched_job_alloc not exactly once in drm_sched.o"
[ "$(aarch64-linux-gnu-nm "$SUBMIT_O" | grep -c ' [tT] p7580_drv_run_job$')" = 1 ] \
	|| gate_fail "p7580_drv_run_job not exactly once in submit.o"
[ "$(aarch64-linux-gnu-nm "$SUBMIT_O" | grep -c ' [rRdD] p7580_drv_sched_ops$')" = 1 ] \
	|| gate_fail "p7580_drv_sched_ops not exactly once in submit.o"
aarch64-linux-gnu-nm "$SCHED_O" | grep ' drm_sched_job_alloc$' | tee -a "$EVID"
aarch64-linux-gnu-nm "$SUBMIT_O" | grep -E ' p7580_drv_(run_job|sched_ops)$' | tee -a "$EVID"
[ "$(grep -c ' [Tt] drm_sched_job_alloc$' System.map)" = 1 ] \
	|| gate_fail "System.map drm_sched_job_alloc count != 1 (duplicate shim copy?)"
[ "$(grep -c ' [tT] p7580_drv_run_job$' System.map)" = 1 ] \
	|| gate_fail "System.map p7580_drv_run_job count != 1"
[ "$(grep -c ' [rR] p7580_drv_sched_ops$' System.map)" = 1 ] \
	|| gate_fail "System.map p7580_drv_sched_ops count != 1"
grep -E ' (drm_sched_job_alloc|p7580_drv_run_job|p7580_drv_sched_ops)$' System.map | tee -a "$EVID"
# (c2) __ksymtab export present for the alloc entry point
aarch64-linux-gnu-nm "$SCHED_O" | grep -q '__ksymtab_drm_sched_job_alloc' \
	|| gate_fail "no __ksymtab_drm_sched_job_alloc in drm_sched.o"
aarch64-linux-gnu-nm "$SCHED_O" | grep -E '__ksymtab_drm_sched_job_alloc$' | tee -a "$EVID"
# (a) compute the true in-tree layout: stage the shim headers (0004 is
# authoritative; drivers/gpu/drm/drm_sched.h shadows the tree's own
# 3.10 drm_sched.h-less drivers/gpu/drm) and compile offsetof getters
# against the real headers, exactly the kcheck include chain
# (code/gpu31/sched/Makefile). Immediates read back from the .s.
GSTAGE=$EXT/offstage-int49
rm -rf "$GSTAGE"
mkdir -p "$GSTAGE/include/linux" "$GSTAGE/drivers/gpu/drm"
cp drivers/gpu/drm/drm_sched.h "$GSTAGE/drivers/gpu/drm/"
cp drivers/gpu/drm/drm_syncobj.h "$GSTAGE/drivers/gpu/drm/" 2>/dev/null || true
cp include/linux/dma-fence.h include/linux/dma-resv.h "$GSTAGE/include/linux/" 2>/dev/null || true
cat > "$EXT/offcheck33.c" <<'EOF'
#include <drm_sched.h>
int off_ops(void) { return (int)offsetof(struct drm_sched_job, ops); }
int off_slot(void) { return (int)offsetof(struct drm_sched_job, slot); }
int off_ran(void) { return (int)offsetof(struct drm_sched_job, ran); }
EOF
GCC_SYSINC=$(aarch64-linux-gnu-gcc -print-file-name=include)
aarch64-linux-gnu-gcc -nostdinc -isystem "$GCC_SYSINC" \
	-std=gnu89 -Iarch/arm64/include -Iarch/arm64/include/generated \
	-Iinclude -Iinclude/uapi -Iarch/arm64/include/uapi \
	-I"$GSTAGE/include" -I"$GSTAGE/drivers/gpu/drm" \
	-D__KERNEL__ -include linux/kconfig.h -DKBUILD_MODNAME='"offcheck33"' \
	-S -O0 -o "$EXT/offcheck33.s" "$EXT/offcheck33.c" \
	|| gate_fail "offset getter compile failed"
OFF_OPS=$(grep -A 6 '^off_ops:' "$EXT/offcheck33.s" | sed -n 's/.*mov.*w0, *#\?//p' | sed 's/[^0-9].*//' | head -1)
OFF_SLOT=$(grep -A 6 '^off_slot:' "$EXT/offcheck33.s" | sed -n 's/.*mov.*w0, *#\?//p' | sed 's/[^0-9].*//' | head -1)
OFF_RAN=$(grep -A 6 '^off_ran:' "$EXT/offcheck33.s" | sed -n 's/.*mov.*w0, *#\?//p' | sed 's/[^0-9].*//' | head -1)
note "computed in-tree layout: ops=$OFF_OPS slot=$OFF_SLOT ran=$OFF_RAN"
[ "$OFF_OPS" = 24 ] || gate_fail "computed ops offset $OFF_OPS != 24"
[ "$OFF_SLOT" = 232 ] || gate_fail "computed slot offset $OFF_SLOT != 232"
[ "$OFF_RAN" = 236 ] || gate_fail "computed ran offset $OFF_RAN != 236"
# (b) compiler-emitted offset immediates: the alloc TU stores entity+ops
# as ONE stp at #16 (ops word = base+24). run_job (3bcffae) reads the
# SUBMIT-stamped slot from the backend job (slot is word 2 of struct
# p7580_submit_job, seqno u32 + slot int -> ldr wN, [xN, #4]) with a
# range check against P7580_NUM_JOB_SLOTS (cmp #0x2); the sched slot at
# #232 survives only as sslot= in the run_fallback print (range-checked
# fallback path), so #232 is informational here, not required.
aarch64-linux-gnu-objdump -d --disassemble=drm_sched_job_alloc "$SCHED_O" \
	| grep -q 'stp.*\[x[0-9]*, #16\]' || gate_fail "no entity+ops stp #16 in drm_sched_job_alloc disasm"
aarch64-linux-gnu-objdump -d --disassemble=drm_sched_job_alloc "$SCHED_O" \
	| grep 'stp.*\[x[0-9]*, #16\]' | tee -a "$EVID"
aarch64-linux-gnu-objdump -d --disassemble=p7580_drv_run_job "$SUBMIT_O" \
	| grep -q '\[x[0-9]*, #4\]' || gate_fail "no #4 stamped-slot load in p7580_drv_run_job disasm"
aarch64-linux-gnu-objdump -d --disassemble=p7580_drv_run_job "$SUBMIT_O" \
	| grep '\[x[0-9]*, #4\]' | tee -a "$EVID"
aarch64-linux-gnu-objdump -d --disassemble=p7580_drv_run_job "$SUBMIT_O" \
	| grep -q 'cmp.*#0x2' || gate_fail "no slot range check (cmp #0x2) in p7580_drv_run_job disasm"
grep -q 'offsetof(struct drm_sched_job, slot) != 232' drivers/gpu/drm/drm_sched.h \
	|| gate_fail "slot pin missing in applied drm_sched.h"
grep -q 'offsetof(struct drm_sched_job, ran) != 236' drivers/gpu/drm/drm_sched.h \
	|| gate_fail "ran pin missing in applied drm_sched.h"
grep -q 'offsetof(struct drm_sched_job, slot) != 232' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "slot pin missing in applied submit.c"
grep -q 'offsetof(struct drm_sched_job, ran) != 236' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "ran pin missing in applied submit.c"
grep -q 'BUG_ON(job->ops != ops)' drivers/gpu/drm/drm_sched.c \
	|| gate_fail "alloc round-trip BUG_ON missing in applied drm_sched.c"
grep -q 'BUILD_BUG_ON(offsetof(struct drm_sched_job, ops) != 24)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "ops pin missing in applied submit.c"
grep -q 'p7580_sched: ops job=%lu opsoff=%lu ops=%lu run=%lu runexp=%lu owner=%lu slot=%d' drivers/gpu/drm/drm_sched.c \
	|| gate_fail "ST-OPS numeric probe missing in applied drm_sched.c"
aarch64-linux-gnu-strings "$SCHED_O" | grep -q 'p7580_sched: ops job=%lu opsoff=%lu ops=%lu run=%lu runexp=%lu owner=%lu slot=%d' \
	|| gate_fail "ST-OPS numeric string missing in drm_sched.o"
aarch64-linux-gnu-strings "$SCHED_O" | grep 'p7580_sched: ops' | tee -a "$EVID"
aarch64-linux-gnu-strings "$SCHED_O" | grep -q 'p7580_sched: finish ran=%d slot=%d owner=%lu' \
	|| gate_fail "ST-FINISH owner string missing in drm_sched.o"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'p7580_sched: ops job=%lu opsoff=%lu ops=%lu run=%lu runexp=%lu owner=%lu slot=%d' \
	|| gate_fail "ST-OPS numeric string missing in Image"
# ST-RUN entry + per-guard bail + retire ran/owner (submit.c TU)
grep -q 'p7580_sched: run job=%lu ent=%lu owner=%lu ops=%lu runexp=%lu' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "ST-RUN entry probe missing in applied submit.c"
for b in NULLJOB BADSLOT BADENT NODRV; do
	aarch64-linux-gnu-strings "$SUBMIT_O" | grep -q "p7580_sched: run_bail $b" \
		|| gate_fail "run_bail $b missing in submit.o"
done
aarch64-linux-gnu-strings "$SUBMIT_O" | grep -q 'p7580_sched: run_fallback' \
	|| gate_fail "run_fallback missing in submit.o"
aarch64-linux-gnu-strings "$SUBMIT_O" | grep -q 'p7580_sched: retire seq=%u slot=%d sslot=%d ran=%u owner=%lu' \
	|| gate_fail "ST-RETIRE ran/owner missing in submit.o"
aarch64-linux-gnu-strings "$SUBMIT_O" | grep -q 'p7580_sched: run_job' \
	|| gate_fail "ST run_job missing in submit.o"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'p7580_sched: run job=%lu ent=%lu owner=%lu ops=%lu runexp=%lu' \
	|| gate_fail "ST-RUN entry missing in Image"
for b in NULLJOB BADSLOT BADENT NODRV; do
	aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q "p7580_sched: run_bail $b" \
		|| gate_fail "run_bail $b missing in Image"
done
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'p7580_sched: retire seq=%u slot=%d sslot=%d ran=%u owner=%lu' \
	|| gate_fail "ST-RETIRE ran/owner missing in Image"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'p7580_sched: finish ran=%d slot=%d owner=%lu' \
	|| gate_fail "ST-FINISH owner missing in Image"
# file_priv/drv_priv stamps in submit.c job struct; run_job_addr linkage
grep -q 'file_priv' drivers/gpu/drm/panfrost/panfrost7580_job.h \
	|| gate_fail "file_priv stamp missing in applied job.h"
grep -q 'drv_priv' drivers/gpu/drm/panfrost/panfrost7580_job.h \
	|| gate_fail "drv_priv stamp missing in applied job.h"
grep -q 'p7580_drv_run_job_addr' drivers/gpu/drm/drm_sched.c \
	|| gate_fail "run_job_addr hook missing in applied drm_sched.c"
grep -q 'p7580_drv_run_job_addr' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "run_job_addr strong def missing in applied submit.c"
aarch64-linux-gnu-nm "$SCHED_O" | grep -q ' W p7580_drv_run_job_addr$' \
	|| gate_fail "p7580_drv_run_job_addr not weak (W) in drm_sched.o"
aarch64-linux-gnu-nm "$SUBMIT_O" | grep -q ' T p7580_drv_run_job_addr$' \
	|| gate_fail "p7580_drv_run_job_addr not strong text (T) in submit.o"
aarch64-linux-gnu-nm "$SCHED_O" "$SUBMIT_O" | grep ' p7580_drv_run_job_addr$' | tee -a "$EVID"
grep -q ' T p7580_drv_run_job_addr$' System.map \
	|| gate_fail "p7580_drv_run_job_addr not T in System.map (strong def must win link)"
grep ' p7580_drv_run_job_addr$' System.map | tee -a "$EVID"
note "Image p7580_job: count = $(aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -c 'p7580_job:')"
note "Image p7580_job: dispatch = $(aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -c 'p7580_job: dispatch')"
note "Image p7580_job: irq = $(aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -c 'p7580_job: irq')"
note "== int49 gate: PASS (ops=24/slot=232/ran=236, single defs, numeric ST-OPS + runexp, ST-RUN/bail/retire live) =="

# 6c. IRQ/completion gates (irqfix d852459): JS1 zero-req selection (2936796),
# STAT|RAWSTAT demux, re-arm + AS0 program on recover/resume, job probes.
JOB_O=drivers/gpu/drm/panfrost/panfrost7580_job.o
[ -f "$JOB_O" ] || gate_fail "$JOB_O missing"
note "== int49 gate: job object present =="
ls -l --time-style=full-iso "$JOB_O" | tee -a "$EVID"
# JS1 for requirement-less jobs (2936796: mainline v5.2 slot rule; SUPERSEDES
# irqfix d852459's req==0 -> JS0). Applied tree must route FS -> JS0,
# everything else (incl. req==0 chain) -> JS1, ONLY_COMPUTE -> JS2.
grep -q 'return P7580_SLOT_VERTEX_TILER' drivers/gpu/drm/panfrost/panfrost7580_jm_hw.h \
	|| gate_fail "req==0 -> JS1 tail return missing in applied jm_hw.h"
grep -q 'return P7580_SLOT_FRAGMENT' drivers/gpu/drm/panfrost/panfrost7580_jm_hw.h \
	|| gate_fail "FS -> JS0 route missing in applied jm_hw.h"
grep -q 'return P7580_SLOT_COMPUTE' drivers/gpu/drm/panfrost/panfrost7580_jm_hw.h \
	|| gate_fail "ONLY_COMPUTE -> JS2 route missing in applied jm_hw.h"
grep -B2 -A8 'static inline int p7580_job_slot' drivers/gpu/drm/panfrost/panfrost7580_jm_hw.h | tee -a "$EVID"
# STAT|RAWSTAT demux: union read in the applied IRQ handler
grep -q '(status | rawstat)' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "STAT|RAWSTAT demux missing in applied job.c"
grep -c '(status | rawstat)' drivers/gpu/drm/panfrost/panfrost7580_job.c | tee -a "$EVID"
# re-arm helper declared + defined + used in recover; program_as on both paths
grep -q 'p7580_job_dev_rearm' drivers/gpu/drm/panfrost/panfrost7580_job.h \
	|| gate_fail "rearm decl missing in applied job.h"
grep -q 'void p7580_job_dev_rearm' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "rearm def missing in applied job.c"
grep -q 'p7580_job_dev_rearm' drivers/gpu/drm/panfrost/panfrost7580_main.c \
	|| gate_fail "rearm use missing in applied main.c (resume)"
grep -q 'p7580_mmu_program_as' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "program_as missing in applied job.c (recover)"
grep -q 'p7580_mmu_program_as' drivers/gpu/drm/panfrost/panfrost7580_main.c \
	|| gate_fail "program_as missing in applied main.c (resume)"
grep -q 'P7580_JS_STATUS' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "JS_STATUS read missing in applied job.c"
# job probe strings in applied tree
for s in 'p7580_job: irq-arm' 'p7580_job: irq-enter' 'p7580_job: post-start' 'p7580_job: irq slot=' 'p7580_job: fence signal' 'p7580_job: irq-err'; do
	grep -q "$s" drivers/gpu/drm/panfrost/panfrost7580_job.c \
		|| gate_fail "probe '$s' missing in applied job.c"
done
# job probe strings in job.o
for s in 'p7580_job: irq-arm' 'p7580_job: irq-enter' 'p7580_job: post-start' 'p7580_job: irq slot=' 'p7580_job: fence signal' 'p7580_job: irq-err' 'p7580_job: dispatch'; do
	aarch64-linux-gnu-strings "$JOB_O" | grep -q "$s" \
		|| gate_fail "probe '$s' missing in job.o"
done
aarch64-linux-gnu-strings "$JOB_O" | grep 'p7580_job:' | tee -a "$EVID"
# job probe strings in the linked Image
for s in 'p7580_job: irq-arm' 'p7580_job: irq-enter' 'p7580_job: post-start' 'p7580_job: irq slot=' 'p7580_job: fence signal' 'p7580_job: irq-err' 'p7580_job: dispatch'; do
	aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q "$s" \
		|| gate_fail "probe '$s' missing in Image"
done
note "== int49 irq gate: PASS (JS1 zero-req per 2936796, STAT|RAWSTAT demux, rearm+AS0 recover/resume, job probes live) =="

# 6g. slot-routing gate (mesa-render 2936796): req==0 -> JS1 in the
# applied tree AND compiled into job.o AND visible in the linked Image.
# FAIL (exit 1, scratch kept) on any disagreement.
grep -q '(requirements == 0)' drivers/gpu/drm/panfrost/panfrost7580_jm_hw.h \
	&& gate_fail "stale irqfix req==0 special-case still present in applied jm_hw.h"
grep -q 'SLOT_VERTEX_TILER' drivers/gpu/drm/panfrost/panfrost7580_jm_hw.h \
	|| gate_fail "VERTEX_TILER define missing in applied jm_hw.h"
aarch64-linux-gnu-strings "$JOB_O" | grep -q 'p7580_job: dispatch' \
	|| gate_fail "dispatch probe missing in job.o (slot evidence path)"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'p7580_job: dispatch' \
	|| gate_fail "dispatch probe missing in Image"
note "slot routing: req0->JS1 (2936796) live in jm_hw.h + job.o + Image"
note "== int49 slot gate: PASS (FS->JS0, req0->JS1, ONLY_COMPUTE->JS2) =="


# 6e. ABI-align gates (de142b3): BUILD_BUG_ON ABI gates present in both
# TUs, SYNC_FILE bit-0 tolerance on HTOF/FTOH (other bits rejected),
# dispatcher copy-in truncate + copy-back cap. FAIL (exit 1, scratch
# kept) on any disagreement.
SYNC_O=drivers/gpu/drm/panfrost/panfrost7580_syncobj.o
IOCTL_O=drivers/gpu/drm/panfrost/panfrost7580_ioctl.o
[ -f "$SYNC_O" ] || gate_fail "$SYNC_O missing"
[ -f "$IOCTL_O" ] || gate_fail "$IOCTL_O missing"
note "== int49 gate: syncobj/ioctl objects present =="
ls -l --time-style=full-iso "$SYNC_O" "$IOCTL_O" | tee -a "$EVID"
# ABI BUILD_BUG_ON gates in the syncobj TU (applied tree)
grep -q 'BUILD_BUG_ON(sizeof(struct p7580_syncobj_handle) != 16)' drivers/gpu/drm/panfrost/panfrost7580_syncobj.c \
	|| gate_fail "16 B syncobj_handle pin missing in applied syncobj.c"
grep -q 'BUILD_BUG_ON(sizeof(struct p7580_syncobj_wait) != 32)' drivers/gpu/drm/panfrost/panfrost7580_syncobj.c \
	|| gate_fail "32 B syncobj_wait pin missing in applied syncobj.c"
grep -q 'BUILD_BUG_ON(P7580_SYNCOBJ_HTOF_EXPORT_SYNC_FILE != (1u << 0))' drivers/gpu/drm/panfrost/panfrost7580_syncobj.c \
	|| gate_fail "HTOF SYNC_FILE bit pin missing in applied syncobj.c"
grep -q 'BUILD_BUG_ON(P7580_SYNCOBJ_FTOH_IMPORT_SYNC_FILE != (1u << 0))' drivers/gpu/drm/panfrost/panfrost7580_syncobj.c \
	|| gate_fail "FTOH SYNC_FILE bit pin missing in applied syncobj.c"
# ABI BUILD_BUG_ON gates in the ioctl TU (applied tree)
grep -q 'BUILD_BUG_ON(sizeof(struct drm_panfrost_get_param) != 16)' drivers/gpu/drm/panfrost/panfrost7580_ioctl.c \
	|| gate_fail "16 B get_param pin missing in applied ioctl.c"
grep -q 'BUILD_BUG_ON(sizeof(struct drm_panfrost_submit) != 40)' drivers/gpu/drm/panfrost/panfrost7580_ioctl.c \
	|| gate_fail "40 B submit pin missing in applied ioctl.c"
# SYNC_FILE tolerance path: bit-0 mask kept, other bits rejected (both handlers)
grep -q 'args->flags & ~P7580_SYNCOBJ_HTOF_EXPORT_SYNC_FILE' drivers/gpu/drm/panfrost/panfrost7580_syncobj.c \
	|| gate_fail "HTOF SYNC_FILE tolerance missing in applied syncobj.c"
grep -q 'args->flags & ~P7580_SYNCOBJ_FTOH_IMPORT_SYNC_FILE' drivers/gpu/drm/panfrost/panfrost7580_syncobj.c \
	|| gate_fail "FTOH SYNC_FILE tolerance missing in applied syncobj.c"
# copy-size capping in the dispatcher (truncate copy-in, cap copy-back)
grep -q 'unsigned int cpin = usize < asize ? usize : asize' drivers/gpu/drm/panfrost/panfrost7580_syncobj.c \
	|| gate_fail "copy-in truncate missing in applied syncobj.c"
grep -q 'unsigned int cpout = usize < asize ? usize : asize' drivers/gpu/drm/panfrost/panfrost7580_syncobj.c \
	|| gate_fail "copy-back cap missing in applied syncobj.c"
# handler entry points live in the object + linked Image (nm, not strings:
# this TU carries no printk probes)
aarch64-linux-gnu-nm "$SYNC_O" | grep -q ' T panfrost_syncobj_handle_to_fd$' \
	|| gate_fail "panfrost_syncobj_handle_to_fd missing in syncobj.o"
aarch64-linux-gnu-nm "$SYNC_O" | grep -q ' T panfrost_syncobj_fd_to_handle$' \
	|| gate_fail "panfrost_syncobj_fd_to_handle missing in syncobj.o"
aarch64-linux-gnu-nm "$SYNC_O" | grep -q ' T panfrost_syncobj_ioctl$' \
	|| gate_fail "panfrost_syncobj_ioctl missing in syncobj.o"
grep -q ' T panfrost_syncobj_handle_to_fd$' System.map \
	|| gate_fail "panfrost_syncobj_handle_to_fd missing in System.map"
grep -q ' T panfrost_syncobj_fd_to_handle$' System.map \
	|| gate_fail "panfrost_syncobj_fd_to_handle missing in System.map"
aarch64-linux-gnu-nm "$SYNC_O" "$IOCTL_O" | grep -E ' panfrost_syncobj_(handle_to_fd|fd_to_handle|ioctl)$' | tee -a "$EVID"
note "== int49 abi gate: PASS (BUILD_BUG_ON pins, SYNC_FILE tolerance, copy-size caps live) =="
# 6f. gpuwrite gates (int20): Zero-fill descriptor contract +
# DEBUG_READ_BO 0xCF fops route; six-ioctl driver table untouched;
# debug readback reuses job_done_sync (no second maintenance impl).
# FAIL (exit 1, scratch kept) on any disagreement.
GEM_O=drivers/gpu/drm/panfrost/panfrost7580_gem.o
[ -f "$GEM_O" ] || gate_fail "$GEM_O missing"
note "== int49 gate: gem object present =="
ls -l --time-style=full-iso "$GEM_O" | tee -a "$EVID"
# Zero is the only T720-valid write type (v4/v5 genxml: 1/2/3; no 4..7):
# the pattern-test must encode w10=3 and zero the immediate words.
grep -q 'w\[10\] = 3u' drivers/gpu/drm/panfrost/panfrost7580_gem.c \
	&& gate_fail "pattern-test leaked into gem.c (must live in the client only)"
grep -q 'P7580_DEBUG_READ_BO_NR.*0xCFu' drivers/gpu/drm/panfrost/panfrost7580_syncobj.c \
	|| gate_fail "0xCF debug nr missing in applied syncobj.c"
grep -q 'panfrost_debug_read_bo' drivers/gpu/drm/panfrost/panfrost7580_syncobj.c \
	|| gate_fail "debug dispatch missing in applied syncobj.c"
grep -q 'p7580_gem_debug_read_word' drivers/gpu/drm/panfrost/panfrost7580_gem.c \
	|| gate_fail "debug read word missing in applied gem.c"
grep -q 'p7580_gem_job_done_sync(obj);' drivers/gpu/drm/panfrost/panfrost7580_gem.c \
	|| gate_fail "debug path must reuse job_done_sync in applied gem.c"
grep -q 'int panfrost_debug_read_bo' drivers/gpu/drm/panfrost/panfrost7580_ioctl.c \
	|| gate_fail "debug handler missing in applied ioctl.c"
grep -q 'panfrost_debug_read_bo' drivers/gpu/drm/panfrost/panfrost7580_ioctl.h \
	|| gate_fail "debug decl missing in applied ioctl.h"
# six-ioctl table untouched: exactly the 6 v5.2 rows, no 0xCF row.
[ "$(grep -c 'DRM_IOCTL_DEF_DRV(PANFROST_' drivers/gpu/drm/panfrost/panfrost7580_main.c)" = 6 ] \
	|| gate_fail "driver ioctl table must stay at 6 rows (debug is fops-only)"
grep -q 'DRM_IOCTL_DEF_DRV(PANFROST_SUBMIT' drivers/gpu/drm/panfrost/panfrost7580_main.c \
	|| gate_fail "SUBMIT row missing in applied main.c"
# debug symbols live in objects + Image.
aarch64-linux-gnu-nm "$GEM_O" | grep -q ' T p7580_gem_debug_read_word$' \
	|| gate_fail "p7580_gem_debug_read_word missing in gem.o"
aarch64-linux-gnu-nm "$IOCTL_O" | grep -q ' T panfrost_debug_read_bo$' \
	|| gate_fail "panfrost_debug_read_bo missing in ioctl.o"
aarch64-linux-gnu-strings "$SYNC_O" | grep -q 'p7580_debug_read_bo\|DEBUG_READ_BO\|0xCF\|debug' \
	|| note "debug strings optional in syncobj.o (handler may be string-free)"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'p7580_gem_debug_read_word\|panfrost_debug_read_bo' \
	&& note "debug symbols live in Image" \
	|| note "debug symbols not string-visible in Image (nm-gated below instead)"
grep -q ' T p7580_gem_debug_read_word$' System.map \
	|| gate_fail "p7580_gem_debug_read_word missing in System.map"
grep -q ' T panfrost_debug_read_bo$' System.map \
	|| gate_fail "panfrost_debug_read_bo missing in System.map"
note "== int49 gpuwrite gate: PASS (Zero contract, 0xCF fops route, 6-row table, debug symbols live) =="

# 6h. coherency gates (c207bdd): pre-signal invalidate BEFORE every
# fence signal (IRQ DONE, timedout own-fence, recover -EIO); free_job
# keeps its post-signal job_done_sync + unref walk. FAIL (exit 1,
# scratch kept) on any disagreement.
note "== int49 gate: coherency objects present =="
ls -l --time-style=full-iso "$JOB_O" "$GEM_O" "$SUBMIT_O" | tee -a "$EVID"
# applied-tree call sites: 3x pre_signal_sync calls (IRQ job->bos,
# timedout job->bos, recover fault_job->bos) plus decl + comment mentions.
[ "$(grep -c 'p7580_gem_job_bos_pre_signal_sync(.*->bos,$' drivers/gpu/drm/panfrost/panfrost7580_job.c)" = 3 ] \
	|| gate_fail "job.c must carry 3 pre_signal call sites, got $(grep -c 'p7580_gem_job_bos_pre_signal_sync(.*->bos,$' drivers/gpu/drm/panfrost/panfrost7580_job.c)"
grep -n 'p7580_gem_job_bos_pre_signal_sync' drivers/gpu/drm/panfrost/panfrost7580_job.c | tee -a "$EVID"
# applied-tree free_job: post-signal job_done_sync + unref walk.
grep -q 'p7580_gem_job_done_sync' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "free_job job_done_sync walk missing in applied submit.c"
grep -n 'p7580_gem_job_done_sync\|drm_gem_object_unreference_unlocked' drivers/gpu/drm/panfrost/panfrost7580_submit.c | tee -a "$EVID"
# gem TU: helper def + per-page for_cpu invalidate inside job_done_sync.
grep -q 'void p7580_gem_job_bos_pre_signal_sync' drivers/gpu/drm/panfrost/panfrost7580_gem.c \
	|| gate_fail "pre_signal_sync def missing in applied gem.c"
grep -q 'dma_sync_single_for_cpu' drivers/gpu/drm/panfrost/panfrost7580_gem.c \
	|| gate_fail "for_cpu invalidate missing in applied gem.c"
# symbols live in objects: helper def in gem.o, 3 undef refs in job.o.
aarch64-linux-gnu-nm "$GEM_O" | grep -q ' T p7580_gem_job_bos_pre_signal_sync$' \
	|| gate_fail "pre_signal_sync not T in gem.o"
[ "$(aarch64-linux-gnu-nm "$JOB_O" | grep -c ' U p7580_gem_job_bos_pre_signal_sync$')" = 1 ] \
	|| gate_fail "pre_signal_sync undef ref missing in job.o"
aarch64-linux-gnu-nm "$GEM_O" "$JOB_O" | grep 'p7580_gem_job_bos_pre_signal_sync' | tee -a "$EVID"
# objdump: call-site evidence -- bl to the helper in the IRQ handler,
# timedout, and recover paths of job.o.
for fn in p7580_job_irq p7580_job_timedout p7580_job_recover; do
	aarch64-linux-gnu-objdump -d --disassemble=$fn "$JOB_O" \
		| grep -q 'bl.*<p7580_gem_job_bos_pre_signal_sync' \
		|| gate_fail "no bl to pre_signal_sync in $fn disasm"
	aarch64-linux-gnu-objdump -d --disassemble=$fn "$JOB_O" \
		| grep 'bl.*<p7580_gem_job_bos_pre_signal_sync' | tee -a "$EVID"
done
# linked Image: exactly one definition wins the link.
[ "$(grep -c ' T p7580_gem_job_bos_pre_signal_sync$' System.map)" = 1 ] \
	|| gate_fail "System.map pre_signal_sync count != 1"
grep ' p7580_gem_job_bos_pre_signal_sync$' System.map | tee -a "$EVID"
note "== int49 coherency gate: PASS (pre-signal IRQ/timeout/recover, post-signal free_job intact) =="

# 6i. errtrace gates (7a5bf9b): unconditional ERR/TIMEOUT diagnostics +
# debugfs trace_arm/dump knobs. FAIL (exit 1, scratch kept) on any
# disagreement.
note "== int49 gate: errtrace objects present =="
ls -l --time-style=full-iso "$JOB_O" | tee -a "$EVID"
# applied-tree: ERR format string with full pin set (slot/js_status/stat/
# head/tail/jc) present in job.c; err helper def present.
grep -q 'p7580_job: ERR slot=' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "ERR format string missing in applied job.c"
grep -q 'js_status=0x%08x' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "ERR js_status pin missing in applied job.c"
grep -q 'head=0x%llx tail=0x%llx' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "ERR head/tail pins missing in applied job.c"
grep -q 'p7580_job_err_pin' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "err_pin helper missing in applied job.c"
grep -q 'TIMEOUT slot=' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "TIMEOUT line missing in applied job.c"
grep -n 'p7580_job: ERR slot=\|TIMEOUT slot=' drivers/gpu/drm/panfrost/panfrost7580_job.c | tee -a "$EVID"
# applied-tree: debugfs knobs created (p7580 dir + trace_arm + dump files).
grep -q 'debugfs_create_dir' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "debugfs dir create missing in applied job.c"
grep -q '"trace_arm"' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "trace_arm knob missing in applied job.c"
grep -q '"dump"' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "dump knob missing in applied job.c"
# symbols/strings live in job.o: ERR string + knob names + TIMEOUT.
aarch64-linux-gnu-strings "$JOB_O" | grep -q 'p7580_job: ERR slot=' \
	|| gate_fail "ERR string missing in job.o"
aarch64-linux-gnu-strings "$JOB_O" | grep -q 'trace_arm' \
	|| gate_fail "trace_arm string missing in job.o"
aarch64-linux-gnu-strings "$JOB_O" | grep -q 'TIMEOUT slot=' \
	|| gate_fail "TIMEOUT string missing in job.o"
aarch64-linux-gnu-strings "$JOB_O" | grep -E 'p7580_job: (ERR|TIMEOUT)|trace_arm|^dump$' | tee -a "$EVID"
aarch64-linux-gnu-nm "$JOB_O" | grep -q ' t p7580_job_err_pin$' \
	|| gate_fail "p7580_job_err_pin static missing in job.o nm"
# linked Image: ERR + TIMEOUT + knob strings visible; helper inlined/static
# (nm-gated in job.o above, strings-gated here).
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'p7580_job: ERR slot=' \
	|| gate_fail "ERR string missing in Image"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'TIMEOUT slot=' \
	|| gate_fail "TIMEOUT string missing in Image"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'trace_arm' \
	|| gate_fail "trace_arm string missing in Image"
note "Image p7580_job: ERR = $(aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -c 'p7580_job: ERR')"
# 6j. instrtype gates (71a9412, retargeted by gpu31-storm58): BO
# exclusivity is published in SUBMIT, under df->serial_lock, for every
# submitted BO -- one site only; resv->lock is taken inside add_excl
# (never by the driver: SUBMIT-DEADLOCK rule), and run_job publishes
# nothing (the arm-time re-publish was the storm's cause). FAIL (exit 1,
# scratch kept) on any disagreement.
note "== int49 gate: instrtype objects present =="
ls -l --time-style=full-iso "$SUBMIT_O" | tee -a "$EVID"
# applied tree: exactly one add_excl site, in the serial publish block.
[ "$(grep -c 'dma_resv_add_excl_fence(' drivers/gpu/drm/panfrost/panfrost7580_submit.c)" = 1 ] \
	|| gate_fail "add_excl publish must appear exactly once in applied submit.c"
grep -q 'last_fence);' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "published fence is not the split's last_fence"
SQJ=$(awk '/^static int p7580_drv_split_queue/,/^}/' drivers/gpu/drm/panfrost/panfrost7580_submit.c)
printf '%s\n' "$SQJ" | grep -q 'job->bo_count = s->nr;' \
	|| gate_fail "split node does not stamp bo_count in applied submit.c"
printf '%s\n' "$SQJ" | grep -q 'dma_resv_get_excl(&bo->resv)' \
	|| gate_fail "queue-time get_excl ordering leg missing from split_queue"
grep -n 'dma_resv_add_excl_fence' drivers/gpu/drm/panfrost/panfrost7580_submit.c | tee -a "$EVID"
# locking discipline: the driver must NOT take resv->lock itself; the
# serial_lock it does take is the file's, around the publish.
grep -A30 'dma_resv_add_excl_fence' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	| grep -q 'to_p7580_bo(o)->resv.lock' \
	&& gate_fail "driver must not lock resv itself (shim locks inside add_excl)"
# shim owns the lock: applied-tree dma-resv.c add_excl locks resv->lock.
grep -q 'void dma_resv_add_excl_fence' drivers/base/dma-resv.c \
	|| gate_fail "add_excl def missing in applied dma-resv.c"
grep -A12 'void dma_resv_add_excl_fence' drivers/base/dma-resv.c | grep -q 'mutex_lock(&obj->lock)' \
	|| gate_fail "add_excl must lock resv->lock inside the shim"
grep -A12 'void dma_resv_add_excl_fence' drivers/base/dma-resv.c | tee -a "$EVID"
# object evidence: submit.o carries an UNDEF ref (call site), gem/shim TU
# (drivers/base/built-in.o or dma-resv.o) carries the T definition.
aarch64-linux-gnu-nm "$SUBMIT_O" | grep -q ' U dma_resv_add_excl_fence$' \
	|| gate_fail "U dma_resv_add_excl_fence missing in submit.o (no call site)"
aarch64-linux-gnu-nm "$SUBMIT_O" | grep 'dma_resv_add_excl_fence' | tee -a "$EVID"
[ "$(aarch64-linux-gnu-objdump -d --disassemble=$SUBMIT_BODY "$SUBMIT_O" \
	| grep -c 'bl.*<dma_resv_add_excl_fence>')" = 1 ] \
	|| gate_fail "p7580_drv_submit must carry exactly one add_excl call"
aarch64-linux-gnu-objdump -d --disassemble=$SUBMIT_BODY "$SUBMIT_O" \
	| grep 'dma_resv_add_excl_fence' | tee -a "$EVID"
[ "$(aarch64-linux-gnu-objdump -d --disassemble=p7580_drv_run_job "$SUBMIT_O" \
	| grep -c 'dma_resv_add_excl_fence')" = 0 ] \
	|| gate_fail "p7580_drv_run_job still publishes BO exclusivity (storm cause)"
# linked Image: exactly one T definition wins the link.
[ "$(grep -c ' T dma_resv_add_excl_fence$' System.map)" = 1 ] \
	|| gate_fail "System.map dma_resv_add_excl_fence count != 1"
grep ' dma_resv_add_excl_fence$' System.map | tee -a "$EVID"
note "== int49 instrtype gate: PASS (single excl publish at SUBMIT, shim-owned resv lock, run_job publishes nothing, one call site in submit.o, one T in System.map) =="

# 6k. iova gates (2dd207c): free-time gpu_unmap before iova_free,
# invalidate-before/after at map (reuse fence), OVERLAP detector,
# ERR-MAP dump + iova_arm knob. FAIL (exit 1, scratch kept) on any
# disagreement.
note "== int49 gate: iova objects present =="
ls -l --time-style=full-iso "$GEM_O" "$SUBMIT_O" "$JOB_O" | tee -a "$EVID"
# applied tree: free calls gpu_unmap BEFORE iova_free (order matters:
# the IOVA must be unmapped everywhere before it is reusable).
grep -q 'p7580_gem_gpu_unmap(obj);' drivers/gpu/drm/panfrost/panfrost7580_gem.c \
	|| gate_fail "gpu_unmap call missing in applied gem.c free path"
grep -q 'p7580_iova_free(gd, bo);' drivers/gpu/drm/panfrost/panfrost7580_gem.c \
	|| gate_fail "iova_free call missing in applied gem.c free path"
[ "$(grep -n 'p7580_gem_gpu_unmap(obj);' drivers/gpu/drm/panfrost/panfrost7580_gem.c | head -1 | cut -d: -f1)" -lt \
  "$(grep -n 'p7580_iova_free(gd, bo);' drivers/gpu/drm/panfrost/panfrost7580_gem.c | head -1 | cut -d: -f1)" ] \
	|| gate_fail "free path must call gpu_unmap before iova_free"
grep -n 'p7580_gem_gpu_unmap(obj);\|p7580_iova_free(gd, bo);' drivers/gpu/drm/panfrost/panfrost7580_gem.c | tee -a "$EVID"
# applied tree: submit TU carries the unmap def + map_bo reuse fence.
grep -q '^void p7580_gem_gpu_unmap' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "gpu_unmap def missing in applied submit.c"
grep -q 'p7580_drv_register_iova_trace' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "register_iova_trace missing in applied submit.c"
grep -q 'p7580_iova: OVERLAP' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "OVERLAP detector missing in applied submit.c"
grep -q 'p7580_iova: map obj=' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "map trace missing in applied submit.c"
grep -q 'p7580_iova: unmap obj=' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "unmap trace missing in applied submit.c"
# invalidate-map-invalidate: three p7580_mmu_invalidate calls in the
# map_bo function body -- purge-before, publish-after, and the
# gpu31-boleak rollback publish (a failed bind must clear the leaves it
# programmed, because no bo_list entry exists yet for the free-time
# unmap to key on).
[ "$(sed -n '/^static int p7580_drv_map_bo_body/,/^}/p' drivers/gpu/drm/panfrost/panfrost7580_submit.c | grep -c 'p7580_mmu_invalidate(drv->regs')" = 3 ] \
	|| gate_fail "map_bo must carry exactly 3 mmu_invalidate calls (purge, publish, rollback publish)"
sed -n '/^static int p7580_drv_map_bo_body/,/^}/p' drivers/gpu/drm/panfrost/panfrost7580_submit.c | grep -n 'p7580_mmu_invalidate' | tee -a "$EVID"
# applied tree: jobs TU carries ERR-MAP dump + iova_arm knob + resolver.
grep -q 'p7580_job: ERR-MAP' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "ERR-MAP missing in applied job.c"
grep -q '"iova_arm"' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "iova_arm knob missing in applied job.c"
grep -q 'p7580_job_set_iova_resolver\|p7580_job_register_iova_budget' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "iova resolver/budget hooks missing in applied job.c"
grep -n 'p7580_job: ERR-MAP\|"iova_arm"\|p7580_job_set_iova_resolver\|p7580_job_register_iova_budget' drivers/gpu/drm/panfrost/panfrost7580_job.c | tee -a "$EVID"
# object evidence: gpu_unmap T in submit.o, U in gem.o; free path
# calls it; map_bo calls invalidate twice.
aarch64-linux-gnu-nm "$SUBMIT_O" | grep -q ' T p7580_gem_gpu_unmap$' \
	|| gate_fail "p7580_gem_gpu_unmap not T in submit.o"
aarch64-linux-gnu-nm "$GEM_O" | grep -q ' U p7580_gem_gpu_unmap$' \
	|| gate_fail "U p7580_gem_gpu_unmap missing in gem.o (free path must call it)"
aarch64-linux-gnu-nm "$GEM_O" "$SUBMIT_O" | grep 'p7580_gem_gpu_unmap' | tee -a "$EVID"
aarch64-linux-gnu-objdump -d --disassemble=p7580_gem_free_object "$GEM_O" \
	| grep -q 'bl.*<p7580_gem_gpu_unmap>' \
	|| gate_fail "no bl to p7580_gem_gpu_unmap in p7580_gem_free_object disasm"
aarch64-linux-gnu-objdump -d --disassemble=p7580_gem_free_object "$GEM_O" \
	| grep 'p7580_gem_gpu_unmap' | tee -a "$EVID"
# map_bo is static; the compiler outlines it (t p7580_drv_map_bo), so
# the three bl sites (purge-before, publish-after, rollback publish) are
# asserted in that symbol instead of in the caller (reloc-gated, robust
# to branch-range stubs). The two caller sites are the BO loop and the
# inlined shadow_create (which maps its own BO).
MAP_DIS=$(aarch64-linux-gnu-objdump -dr --disassemble=$MAPBO_BODY "$SUBMIT_O")
[ "$(printf '%s\n' "$MAP_DIS" | grep -c 'R_AARCH64_CALL26[[:space:]]*p7580_mmu_invalidate')" -ge 3 ] \
	|| gate_fail "p7580_drv_map_bo needs >=3 mmu_invalidate relocs (purge, publish, rollback publish), got $(printf '%s\n' "$MAP_DIS" | grep -c 'R_AARCH64_CALL26[[:space:]]*p7580_mmu_invalidate')"
[ "$(printf '%s\n' "$MAP_DIS" | grep -c 'R_AARCH64_CALL26[[:space:]]*p7580_mmu_unmap')" = 1 ] \
	|| gate_fail "p7580_drv_map_bo must carry exactly one mmu_unmap (the rollback), got $(printf '%s\n' "$MAP_DIS" | grep -c 'R_AARCH64_CALL26[[:space:]]*p7580_mmu_unmap')"
MU_LN=$(printf '%s\n' "$MAP_DIS" | grep -n 'R_AARCH64_CALL26[[:space:]]*p7580_mmu_unmap' | cut -d: -f1 | head -1)
INV_LN=$(printf '%s\n' "$MAP_DIS" | grep -n 'R_AARCH64_CALL26[[:space:]]*p7580_mmu_invalidate' | cut -d: -f1 | awk -v m="$MU_LN" '$1 > m' | head -1)
[ -n "$MU_LN" ] && [ -n "$INV_LN" ] && [ "$MU_LN" -lt "$INV_LN" ] \
	|| gate_fail "map_bo rollback does not publish the cleared leaves (unmap=$MU_LN invalidate=$INV_LN)"
printf '%s\n' "$MAP_DIS" | grep -E 'p7580_mmu_(invalidate|unmap)' | tee -a "$EVID"
[ "$(aarch64-linux-gnu-objdump -dr --disassemble=$SUBMIT_BODY "$SUBMIT_O" | grep -c 'bl.*<p7580_drv_map_bo>')" = 2 ] \
	|| gate_fail "p7580_drv_submit must map via p7580_drv_map_bo twice (BO loop + shadow BO)"
# gpu_unmap's TLB flush is outlined by the compiler into
# p7580_gem_gpu_unmap.part.0 (the exported symbol keeps the
# snapshot/unlink prologue): one invalidate in the part symbol.
[ "$(aarch64-linux-gnu-objdump -dr --disassemble=p7580_gem_gpu_unmap.part.0 "$SUBMIT_O" | grep -c 'R_AARCH64_CALL26[[:space:]]*p7580_mmu_invalidate')" = 1 ] \
	|| gate_fail "p7580_gem_gpu_unmap.part.0 disasm must bl p7580_mmu_invalidate once"
# trace/ERR-MAP strings live in objects + linked Image.
for s in 'p7580_iova: map obj=' 'p7580_iova: unmap obj=' 'p7580_iova: OVERLAP' 'p7580_job: ERR-MAP' 'iova_arm'; do \
	aarch64-linux-gnu-strings "$SUBMIT_O" "$JOB_O" | grep -q "$s" \
	|| gate_fail "$s missing in submit.o/job.o"; \
	aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q "$s" \
	|| gate_fail "$s missing in Image"; \
done
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -E 'p7580_iova: (map|unmap|OVERLAP)|p7580_job: ERR-MAP' | tee -a "$EVID"
# linked Image: exactly one gpu_unmap definition wins the link.
[ "$(grep -c ' T p7580_gem_gpu_unmap$' System.map)" = 1 ] \
	|| gate_fail "System.map p7580_gem_gpu_unmap count != 1"
grep ' p7580_gem_gpu_unmap$' System.map | tee -a "$EVID"
note "== int49 iova gate: PASS (free-time unmap before iova_free, invalidate-map-invalidate, OVERLAP + ERR-MAP live) =="
# 6l. armfence gates (49f7df2: arm-time completion fence — the
# INSTR_TYPE_MISMATCH root cause: the 0004 shim used to create the fence
# in run_job, so SUBMIT-time sched_job->fence was NULL and the out_sync
# install was a no-op; Mesa recycled the batch BO while the GPU read it.
# New contract: alloc at arm (ops->alloc_fence), install armed fence on
# out_sync during the ioctl, run_job binds the same fence, pump enforces
# identity (F4 path on mismatch). FAIL (exit 1, scratch kept) on any
# disagreement.
note "== int49 gate: armfence objects present =="
ls -l --time-style=full-iso "$SCHED_O" "$SUBMIT_O" "$JOB_O" | tee -a "$EVID"
# applied tree: arm hook declared + defined in the shim (arm before deps).
grep -q 'void drm_sched_job_arm' drivers/gpu/drm/drm_sched.c \
	|| gate_fail "drm_sched_job_arm def missing in applied drm_sched.c"
grep -q 'alloc_fence(job)' drivers/gpu/drm/drm_sched.c \
	|| gate_fail "alloc_fence call missing in applied drm_sched.c (arm)"
grep -q 'job->fence = fence' drivers/gpu/drm/drm_sched.c \
	|| gate_fail "fence assignment missing in applied drm_sched.c (arm)"
grep -n 'drm_sched_job_arm\|alloc_fence' drivers/gpu/drm/drm_sched.c | tee -a "$EVID"
grep -q 'alloc_fence' drivers/gpu/drm/drm_sched.h \
	|| gate_fail "alloc_fence hook missing in applied drm_sched.h"
# applied tree: pump identity enforcement (mismatch frees the job, never publishes).
grep -q 'run_fence != job->fence' drivers/gpu/drm/drm_sched.c \
	|| gate_fail "pump identity check missing in applied drm_sched.c"
grep -q 'struct dma_fence \*run_fence' drivers/gpu/drm/drm_sched.c \
	|| gate_fail "run_fence decl missing in applied drm_sched.c pump"
# applied tree: SUBMIT arms before the dep loops and installs the armed
# fence on out_sync during the ioctl. int39: the arm + the armed-fence
# reference live in p7580_drv_split_queue (one per node); the install is
# the split's last queued node's fence (last_fence), still once, still
# during the ioctl.
grep -q 'drm_sched_job_arm(sched_job)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "SUBMIT arm call missing in applied submit.c"
grep -q 'fence_ref = sched_job->fence;' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "per-node armed-fence capture missing in applied submit.c"
grep -q 'dma_fence_get(fence_ref);' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "per-node armed-fence reference missing in applied submit.c"
# the install target is the split's last_fence (single node: that node).
grep -A1 'int oret = drm_syncobj_replace_fence(s,' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	| grep -q 'last_fence);' \
	|| gate_fail "SUBMIT replace_fence of the armed last_fence missing in applied submit.c"
[ "$(grep -c 'drm_syncobj_replace_fence(' drivers/gpu/drm/panfrost/panfrost7580_submit.c)" = 1 ] \
	|| gate_fail "submit.c must carry exactly 1 replace_fence call (the split's out_sync install), got $(grep -c 'drm_syncobj_replace_fence(' drivers/gpu/drm/panfrost/panfrost7580_submit.c)"
# arm precedes the longest-running dependency attaches in the node build.
ARM_LN=$(grep -n 'drm_sched_job_arm(sched_job)' drivers/gpu/drm/panfrost/panfrost7580_submit.c | head -1 | cut -d: -f1)
DEP_LN=$(grep -n 'drm_sched_job_add_dependency(sched_job, prev)' drivers/gpu/drm/panfrost/panfrost7580_submit.c | head -1 | cut -d: -f1)
[ -n "$ARM_LN" ] && [ -n "$DEP_LN" ] && [ "$ARM_LN" -lt "$DEP_LN" ] \
	|| gate_fail "arm must precede the dependency attaches (arm=$ARM_LN dep=$DEP_LN)"
# applied tree: run_job binds sched_job->fence (one shared ref, no second
# allocation); the F-JT1 stash (run_job-time install) is gone.
grep -q 'struct dma_fence \*armed = sched_job->fence' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "run_job armed-fence bind missing in applied submit.c"
grep -q 'dma_fence_get(armed)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "run_job shared-ref get missing in applied submit.c"
grep -q 'No out_sync work here' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "run_job no-out_sync-work marker missing in applied submit.c"
awk '/^static struct dma_fence \*p7580_drv_run_job/,/^}/' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	| grep -q 'drm_syncobj_replace_fence' \
	&& gate_fail "run_job must not call replace_fence (install is SUBMIT-time now)"
# applied tree: jobs TU owns contexts + the ONLY allocator; run binds.
grep -q 'dma_fence_context_alloc(1)' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "fence context init missing in applied job.c"
grep -q 'struct dma_fence \*p7580_job_alloc_fence' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "p7580_job_alloc_fence def missing in applied job.c"
grep -q 'p7580_job_alloc_fence' drivers/gpu/drm/panfrost/panfrost7580_job.h \
	|| gate_fail "p7580_job_alloc_fence decl missing in applied job.h"
grep -n 'p7580_job_alloc_fence\|dma_fence_context_alloc' drivers/gpu/drm/panfrost/panfrost7580_job.c | tee -a "$EVID"
# driver 0001: alloc hook + ops table entry + call-site guard extension.
grep -q 'p7580_drv_alloc_fence' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "p7580_drv_alloc_fence missing in applied submit.c"
grep -q '\.alloc_fence = p7580_drv_alloc_fence' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "alloc_fence ops entry missing in applied submit.c"
grep -q 'sched_job->ops->alloc_fence' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "ops call-site guard must cover alloc_fence in applied submit.c"
# object evidence: arm T in sched.o, U in submit.o; alloc T in job.o + drv hook T in submit.o.
aarch64-linux-gnu-nm "$SCHED_O" | grep -q ' T drm_sched_job_arm$' \
	|| gate_fail "drm_sched_job_arm not T in drm_sched.o"
aarch64-linux-gnu-nm "$SUBMIT_O" | grep -q ' U drm_sched_job_arm$' \
	|| gate_fail "U drm_sched_job_arm missing in submit.o (SUBMIT must arm)"
aarch64-linux-gnu-nm "$JOB_O" | grep -q ' T p7580_job_alloc_fence$' \
	|| gate_fail "p7580_job_alloc_fence not T in job.o"
aarch64-linux-gnu-nm "$SUBMIT_O" | grep -q ' [tT] p7580_drv_alloc_fence$' \
	|| gate_fail "p7580_drv_alloc_fence missing in submit.o (static t expected)"
aarch64-linux-gnu-nm "$SCHED_O" "$SUBMIT_O" "$JOB_O" | grep -E 'drm_sched_job_arm|alloc_fence' | tee -a "$EVID"
aarch64-linux-gnu-objdump -d --disassemble=$SUBMIT_BODY "$SUBMIT_O" \
	| grep -q 'bl.*<drm_sched_job_arm' \
	|| gate_fail "no bl to drm_sched_job_arm in p7580_drv_submit disasm"
aarch64-linux-gnu-objdump -d --disassemble=$SUBMIT_BODY "$SUBMIT_O" \
	| grep 'drm_sched_job_arm' | tee -a "$EVID"
aarch64-linux-gnu-objdump -d --disassemble=p7580_drv_run_job "$SUBMIT_O" \
	| grep -q 'bl.*<drm_syncobj_replace_fence' \
	&& gate_fail "run_job must not install out_sync (install is SUBMIT-time now)"
aarch64-linux-gnu-objdump -d --disassemble=drm_sched_pump "$SCHED_O" \
	| grep -c 'bl.*<dma_fence_put' | tee -a "$EVID"
# Image sanity: ST-OPS/ST-RUN family still live after the 0004 regen.
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'p7580_sched: ops job=%lu opsoff=%lu ops=%lu run=%lu runexp=%lu owner=%lu slot=%d' \
	|| gate_fail "ST-OPS family missing in Image after armfence regen"
# linked Image: exactly one arm + one allocator win the link.
[ "$(grep -c ' T drm_sched_job_arm$' System.map)" = 1 ] \
	|| gate_fail "System.map drm_sched_job_arm count != 1"
[ "$(grep -c ' T p7580_job_alloc_fence$' System.map)" = 1 ] \
	|| gate_fail "System.map p7580_job_alloc_fence count != 1"
grep -E ' (drm_sched_job_arm|p7580_job_alloc_fence)$' System.map | tee -a "$EVID"
note "== int49 armfence gate: PASS (alloc at arm, SUBMIT out_sync install, identity pump, contexts live) =="
 # 6m. ptewalk gates (06943c1: walker scans live bo_list for owner
 # resolution (snapshot + live), always re-reads the 8 descriptor words
 # from leaf_phys + page_off (even NO-OWNER), every line prints snap= and
 # live= ranges; jc_page walk from 9231f17 + CROSS-PAGE bound unchanged.
 # Fault-time AS0 PT walk in the PM-held timedout path BEFORE recover
 # rebinds AS0. FAIL (exit 1, scratch kept) on any disagreement.
 note "== int49 gate: ptewalk walker present =="
 grep -q 'jc_page = jc & PAGE_MASK' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	|| gate_fail "ptewalk guard jc_page mask missing in applied submit.c"
 grep -q '((jc_page >> shift) & 0x1FFu)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	|| gate_fail "ptewalk guard jc_page mid-level index missing in applied submit.c"
 grep -q '((jc_page >> 12) & 0x1FFu)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	|| gate_fail "ptewalk guard jc_page leaf index missing in applied submit.c"
 grep -q 'if (jc & ~PAGE_MASK)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	&& gate_fail "stale silent non-aligned early return still present in applied submit.c"
 grep -q 'page_off + 32 > PAGE_SIZE' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	|| gate_fail "ptewalk page_off CROSS-PAGE bound missing in applied submit.c"
 grep -q 'p7580_drv_ptewalk_pin' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	|| gate_fail "p7580_drv_ptewalk_pin missing in applied submit.c"
 grep -q 'p7580_job: PTEWALK' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	|| gate_fail "PTEWALK pin line missing in applied submit.c"
 grep -q 'leaf_phys=0x%llx bo_phys=0x%llx' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	|| gate_fail "PTEWALK leaf-vs-BO comparison missing in applied submit.c"
 grep -q 'p7580_drv_ptewalk_pin(drv, job);' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	|| gate_fail "PTEWALK call missing before recover in applied submit.c"
 awk '/p7580_drv_timedout_job\(struct drm_sched_job/,/^}/' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	| grep -q 'p7580_drv_ptewalk_pin(drv, job);' \
 	|| gate_fail "PTEWALK call not inside timedout path in applied submit.c"
 # 06943c1 new: snap/live owner resolution + unconditional leaf dump.
 grep -q 'snap_iova' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	|| gate_fail "ptewalk snap_iova missing in applied submit.c"
 grep -q 'live_iova' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	|| gate_fail "ptewalk live_iova missing in applied submit.c"
 grep -q 'mutex_lock(&drv->bo_lock)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	|| gate_fail "ptewalk live bo_list scan (bo_lock) missing in applied submit.c"
 grep -q 'list_for_each_entry(le, &drv->bo_list, node)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	|| gate_fail "ptewalk bo_list walk missing in applied submit.c"
 grep -q 'if (!p7580_drv_gem_try_get(le->obj))' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	|| gate_fail "ptewalk live GEM ref missing in applied submit.c (want kref_get_unless_zero via try_get)"
 grep -q 'if (!owner && live_bo)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	|| gate_fail "ptewalk live-owner fallback missing in applied submit.c"
 grep -q 'snap=0x%llx/0x%zx live=0x%llx/0x%zx' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	|| gate_fail "ptewalk snap=/live= print missing in applied submit.c"
 grep -q 'NO-OWNER snap=' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	|| gate_fail "ptewalk NO-OWNER snap= line missing in applied submit.c"
 grep -q 'w=%08x %08x %08x %08x %08x %08x %08x %08x' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	|| gate_fail "ptewalk 8-word dump format missing in applied submit.c"
 awk '/NO-OWNER snap=/,/\);/' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	| grep -q 'pw\[0\]' \
 	|| gate_fail "ptewalk NO-OWNER line must still dump the 8 words (unconditional dump)"
 grep -q 'drm_gem_object_unreference_unlocked(live_obj)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	|| gate_fail "ptewalk live GEM unref missing in applied submit.c"
 aarch64-linux-gnu-nm "$SUBMIT_O" | grep -q ' [tT] p7580_drv_ptewalk_pin$' \
 	|| aarch64-linux-gnu-objdump -d --disassemble=p7580_drv_timedout_job "$SUBMIT_O" \
 		| grep -q 'PTEWALK\|ptewalk' \
 	|| gate_fail "p7580_drv_ptewalk_pin missing in submit.o (nm + disasm)"
 aarch64-linux-gnu-strings "$SUBMIT_O" | grep -q 'snap=' \
 	|| gate_fail "snap= string missing in submit.o"
 aarch64-linux-gnu-strings "$SUBMIT_O" | grep -q 'live=' \
 	|| gate_fail "live= string missing in submit.o"
 aarch64-linux-gnu-strings "$SUBMIT_O" | grep -q 'NO-OWNER' \
 	|| gate_fail "NO-OWNER string missing in submit.o"
 aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'p7580_job: PTEWALK' \
 	|| gate_fail "PTEWALK string missing in Image"
 aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'snap=' \
 	|| gate_fail "snap= string missing in Image"
 aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'live=' \
 	|| gate_fail "live= string missing in Image"
 aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'NO-OWNER' \
 	|| gate_fail "NO-OWNER string missing in Image"
 grep -n 'p7580_drv_ptewalk_pin\|PTEWALK\|snap_iova\|live_iova\|bo_lock' drivers/gpu/drm/panfrost/panfrost7580_submit.c | tee -a "$EVID"
 note "== int49 ptewalk gate: PASS (jc_page walk intact, snap/live owner + unconditional leaf dump live) =="
# 6n. pmget gates (0d42bcd: normalise pm_get 1-is-powered to 0; every
# caller tests `< 0` — raw 1 "already active" is success, only negative
# errno is dark. run_job ST-PMGET fail + timedout dark early-return +
# ioctl override must all use `< 0`; helper must normalise 1 to 0.
# FAIL (exit 1, scratch kept) on any disagreement.
note "== int49 gate: pmget call sites present =="
grep -q 'if (p7580_drv_pm_get(drv) < 0)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "run_job pm_get < 0 missing in applied submit.c"
grep -q 'pmraw = p7580_drv_pm_get(drv);' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "timedout pmraw capture missing in applied submit.c"
grep -q 'if (pmraw < 0)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "timedout pmraw < 0 missing in applied submit.c"
[ "$(grep -c 'p7580_drv_pm_get(drv)' drivers/gpu/drm/panfrost/panfrost7580_submit.c)" = 5 ] \
	|| gate_fail "submit.c must carry exactly 5 pm_get call sites (run_job + timedout capture + pm_arm + teardown_hold + ioctl override), got $(grep -c 'p7580_drv_pm_get(drv)' drivers/gpu/drm/panfrost/panfrost7580_submit.c)"
[ "$(grep -c 'p7580_drv_pm_get(drv) < 0' drivers/gpu/drm/panfrost/panfrost7580_submit.c)" = 1 ] \
	|| gate_fail "only run_job keeps the inline pm_get < 0 form (timedout uses pmraw now), got $(grep -c 'p7580_drv_pm_get(drv) < 0' drivers/gpu/drm/panfrost/panfrost7580_submit.c)"
grep -q 'if (ret < 0)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "ioctl override ret < 0 missing in applied submit.c"
grep -q 'Normalise pm_runtime_get_sync' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "pm_get 1-to-0 normalisation missing in applied submit.c"
grep -n 'p7580_drv_pm_get(drv)\|if (pmraw < 0)\|if (ret < 0)\|Normalise pm_runtime_get_sync' drivers/gpu/drm/panfrost/panfrost7580_submit.c | tee -a "$EVID"
aarch64-linux-gnu-objdump -d --disassemble=p7580_drv_pm_get "$SUBMIT_O" \
	| grep -q 'cmp.*#0x1\|mov.*#0x1\|cmp.*#1' \
	|| note "pm_get normalisation compare not pattern-visible (source-gated above)"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'p7580_sched: pm_get FAIL' \
	|| gate_fail "ST-PMGET fail string missing in Image"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'p7580_job: PTEWALK' \
	|| gate_fail "PTEWALK string missing in Image (pmget must not drop ptewalk)"
note "== int49 pmget gate: PASS (run_job + timedout + ioctl < 0, 1 normalised, PTEWALK live) =="
# 6o. timedout-probe gates (b023f4b: three unconditional pr_err pins in
# the PM-held timedout path -- TMD-ENTER at wrapper entry (sched_job/job/
# drv/pmraw captured at declaration), TMD-PREWALK before the PT walk,
# TMD-WALKER at walker entry. `< 0` pm_get semantics unchanged.
# FAIL (exit 1, scratch kept) on any disagreement.
note "== int49 gate: timedout-probe pins present =="
grep -q 'p7580_job: TMD-ENTER sched_job=%lu job=%lu drv=%lu pmraw=%d' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "TMD-ENTER pin missing in applied submit.c"
grep -q 'p7580_job: TMD-PREWALK job=%lu drv=%lu' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "TMD-PREWALK pin missing in applied submit.c"
grep -q 'p7580_job: TMD-WALKER drv=%lu job=%lu' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "TMD-WALKER pin missing in applied submit.c"
grep -q 'pmraw = p7580_drv_pm_get(drv);' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "TMD-ENTER pmraw capture missing in applied submit.c"
awk '/p7580_drv_timedout_job\(struct drm_sched_job/,/^}/' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	| grep -q 'p7580_job: TMD-ENTER' \
	|| gate_fail "TMD-ENTER not inside timedout path in applied submit.c"
awk '/p7580_drv_timedout_job\(struct drm_sched_job/,/^}/' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	| grep -q 'p7580_job: TMD-PREWALK' \
	|| gate_fail "TMD-PREWALK not inside timedout path in applied submit.c"
awk '/p7580_drv_ptewalk_pin\(struct p7580_drv/,/^}/' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	| grep -q 'p7580_job: TMD-WALKER' \
	|| gate_fail "TMD-WALKER not inside walker in applied submit.c"
aarch64-linux-gnu-strings "$SUBMIT_O" | grep -q 'p7580_job: TMD-ENTER' \
	|| gate_fail "TMD-ENTER missing in submit.o"
aarch64-linux-gnu-strings "$SUBMIT_O" | grep -q 'p7580_job: TMD-PREWALK' \
	|| gate_fail "TMD-PREWALK missing in submit.o"
aarch64-linux-gnu-strings "$SUBMIT_O" | grep -q 'p7580_job: TMD-WALKER' \
	|| gate_fail "TMD-WALKER missing in submit.o"
aarch64-linux-gnu-strings "$SUBMIT_O" | grep 'p7580_job: TMD-' | tee -a "$EVID"
for s in 'p7580_job: TMD-ENTER' 'p7580_job: TMD-PREWALK' 'p7580_job: TMD-WALKER'; do \
	aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q "$s" \
		|| gate_fail "probe '$s' missing in Image"; \
done
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep 'p7580_job: TMD-' | tee -a "$EVID"
note "Image p7580_job: TMD- count = $(aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -c 'p7580_job: TMD-')"
# hunk integrity: driver 0001 submit.c fully applied -- late function
# panfrost_ioctl_submit_override (last def in the 1594-line hunk) must
# exist in the applied tree and the object, so a silent truncation
# (the int27 first-run failure mode) fails loudly here.
grep -q 'int panfrost_ioctl_submit_override' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "late function panfrost_ioctl_submit_override missing in applied submit.c (hunk truncated?)"
aarch64-linux-gnu-nm "$SUBMIT_O" | grep -q ' T panfrost_ioctl_submit_override$' \
	|| gate_fail "panfrost_ioctl_submit_override missing in submit.o nm (hunk truncated?)"
grep -q ' T panfrost_ioctl_submit_override$' System.map \
	|| gate_fail "panfrost_ioctl_submit_override missing in System.map"
grep ' panfrost_ioctl_submit_override$' System.map | tee -a "$EVID"
note "== int49 timedout-probe gate: PASS (TMD-ENTER/PREWALK/WALKER in tree + submit.o + Image, submit.c hunk intact) =="
# 6p. submitsnap gates (c95f7b9: SUBMIT-time w0..w7 snapshot in
# job->desc[8], sw= on ERR/TIMEOUT/dispatch/dump, PTEWALK gains sw=,
# JS_HEAD=jc verdict (offset 0 correct, no dispatch change)).
# FAIL (exit 1, scratch kept) on any disagreement.
note "== int49 gate: submitsnap snapshot present =="
grep -q 'uint32_t desc\[8\]' drivers/gpu/drm/panfrost/panfrost7580_job.h \
	|| gate_fail "desc[8] snapshot missing in applied job.h"
grep -q 'uint32_t sw\[8\]' drivers/gpu/drm/panfrost/panfrost7580_job.h \
	|| gate_fail "last.sw[8] missing in applied job.h"
grep -q 'memcpy(job->desc, hdr, sizeof(job->desc));' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "SUBMIT descriptor snapshot (isolate + memcpy into job->desc) missing in applied submit.c"
grep -q 'p7580_chain_hdr_isolate(jobw, ci);' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "chain-node isolate call missing in applied submit.c (snapshot source)"
grep -q 'jd->last.sw' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "dispatch last.sw copy missing in applied job.c"
grep -q 'sw=%08x %08x %08x %08x %08x %08x %08x %08x' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "sw= 8-word print missing in applied job.c (ERR/dispatch/TIMEOUT)"
grep -q 'sw=%08x %08x %08x %08x %08x %08x %08x %08x' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "PTEWALK sw= compare missing in applied submit.c"
# int39: the chain_mode==2 descriptor-bit rewrite is retired from SUBMIT
# (the split isolates every node: next/deps cleared, index renumbered,
# so there is no chain bit left to rewrite). Variant A (mode 1,
# CONFIG_NEXT chain bit) stays in job.c's hw_submit.
grep -q 'chain_mode == 2' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	&& gate_fail "retired chain_mode==2 descriptor rewrite still present in applied submit.c"
grep -q 'desc_w4' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	&& gate_fail "stale desc_w4 token still present in applied job.c"
grep -q 'desc_w4' drivers/gpu/drm/panfrost/panfrost7580_job.h \
	&& gate_fail "stale desc_w4 token still present in applied job.h"
aarch64-linux-gnu-strings "$JOB_O" | grep -q 'sw=' \
	|| gate_fail "sw= missing in job.o"
aarch64-linux-gnu-strings "$SUBMIT_O" | grep -q 'sw=' \
	|| gate_fail "sw= missing in submit.o"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'sw=' \
	|| gate_fail "sw= missing in Image"
grep -n 'sw=' drivers/gpu/drm/panfrost/panfrost7580_job.c | tee -a "$EVID"
grep -n 'sw=' drivers/gpu/drm/panfrost/panfrost7580_submit.c | tee -a "$EVID"
note "Image sw= count = $(aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -c 'sw=')"
note "== int49 submitsnap gate: PASS (desc[8] snapshot, sw= on ERR/TIMEOUT/dispatch/PTEWALK live) =="
# 6q. syncobj-wait gates (b79c502: WAIT_ALL zero-budget false-success
# fixed via is_signaled re-test, writable signaled-ops, WAIT-RECHECK
# device lines in the client). FAIL (exit 1, scratch kept) on any
# disagreement.
note "== int49 gate: syncobj-wait fix present =="
grep -q 'if (!dma_fence_is_signaled(fences\[i\]))' drivers/gpu/drm/panfrost/panfrost7580_syncobj.c \
	|| gate_fail "WAIT_ALL is_signaled re-test missing in applied syncobj.c"
grep -q 'Only a signaled fence counts' drivers/gpu/drm/panfrost/panfrost7580_syncobj.c \
	|| gate_fail "WAIT_ALL re-test comment missing in applied syncobj.c"
grep -q 'static struct dma_fence_ops p7580_sync_signaled_ops' drivers/gpu/drm/panfrost/panfrost7580_syncobj.c \
	|| gate_fail "writable signaled-ops table missing in applied syncobj.c"
grep -q 'static const struct dma_fence_ops p7580_sync_signaled_ops' drivers/gpu/drm/panfrost/panfrost7580_syncobj.c \
	&& gate_fail "stale const signaled-ops still present in applied syncobj.c"
grep -q '\.release = p7580_sync_signaled_release' drivers/gpu/drm/panfrost/panfrost7580_syncobj.c \
	|| gate_fail "signaled release-at-definition missing in applied syncobj.c"
aarch64-linux-gnu-nm "$SYNC_O" | grep -q ' T panfrost_syncobj_ioctl$' \
	|| gate_fail "panfrost_syncobj_ioctl missing in syncobj.o (wait path)"
grep -q 'WAIT-RECHECK' "$REPO/code/gpu31/client/pf7580-selftest.c" \
	|| gate_fail "WAIT-RECHECK device line missing in client pf7580-selftest.c"
grep -n 'WAIT-RECHECK' "$REPO/code/gpu31/client/pf7580-selftest.c" | tee -a "$EVID"
note "== int49 syncobj-wait gate: PASS (WAIT_ALL re-test, writable signaled-ops, WAIT-RECHECK lines) =="
# 6r. jddecode gates (fed4e0c: bo_count stamp re-arms excl/PTEWALK/
# sync/unref; snapshot owner lookup scans live bo_list; 512 B JDBODY
# dump). FAIL (exit 1, scratch kept) on any disagreement.
note "== int49 gate: jddecode bo_count stamp present =="
# int39: every split node takes its own BO refs, so the stamp is s->nr
# inside p7580_drv_split_queue (one per node; still the single place the
# consumer-facing count is written).
grep -q 'job->bo_count = s->nr;' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "bo_count stamp missing in applied submit.c"
grep -q 'Per-job BO references (gpu31-chainsplit)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "per-node bo_count stamp comment missing in applied submit.c"
grep -n 'job->bo_count = s->nr;' drivers/gpu/drm/panfrost/panfrost7580_submit.c | tee -a "$EVID"
# disasm evidence: the stamp lands inside p7580_drv_submit.
aarch64-linux-gnu-objdump -d --disassemble=$SUBMIT_BODY "$SUBMIT_O" \
	| grep -q 'str.*\[x[0-9]*, #' \
	|| gate_fail "no store in p7580_drv_submit disasm (bo_count stamp path)"
# snapshot owner lookup now scans the live bo_list (same scan as PTEWALK pin).
grep -q 'list_for_each_entry(le, &drv->bo_list, node)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "live bo_list snapshot lookup missing in applied submit.c"
grep -q 'p7580_job: JDBODY' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "JDBODY dump missing in applied submit.c"
aarch64-linux-gnu-strings "$SUBMIT_O" | grep -q 'JDBODY' \
	|| gate_fail "JDBODY missing in submit.o"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'JDBODY' \
	|| gate_fail "JDBODY missing in Image"
aarch64-linux-gnu-strings "$SUBMIT_O" | grep 'JDBODY' | tee -a "$EVID"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -c 'JDBODY' | tee -a "$EVID"
note "== int49 jddecode gate: PASS (bo_count stamp, live bo_list snapshot lookup, JDBODY live) =="
# 6s. multidep gates (7e71c82: wait-all AND node for N in_syncs
# (no more single-dep -EOPNOTSUPP), bounded stage+errno on every early
# error path in the shim (drm_sched_job_err) and the driver 0001
# (p7580_submit_err)). FAIL (exit 1, scratch kept) on any disagreement.
note "== int49 gate: multidep wait-all dep path present =="
# applied tree: AND-node struct + ops + merge helper + err helpers.
grep -q 'struct drm_sched_dep_and' drivers/gpu/drm/drm_sched.c \
	|| gate_fail "AND-node struct missing in applied drm_sched.c"
grep -q 'drm_sched_dep_and_ops' drivers/gpu/drm/drm_sched.c \
	|| gate_fail "AND-node ops missing in applied drm_sched.c"
grep -q 'drm_sched_dep_merge' drivers/gpu/drm/drm_sched.c \
	|| gate_fail "AND merge helper missing in applied drm_sched.c"
grep -q 'drm_sched_job_add_dependency' drivers/gpu/drm/drm_sched.c \
	|| gate_fail "add_dependency missing in applied drm_sched.c"
grep -q 'drm_sched_job_err' drivers/gpu/drm/drm_sched.c \
	|| gate_fail "shim err helper missing in applied drm_sched.c"
grep -q 'p7580_submit_err' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "driver submit_err helper missing in applied submit.c"
# old single-dep reject must be gone from the dependency path: no
# -EOPNOTSUPP return in the applied drm_sched.c.
grep -q 'EOPNOTSUPP' drivers/gpu/drm/drm_sched.c \
	&& gate_fail "stale -EOPNOTSUPP reject still present in applied drm_sched.c"
# stage errno strings in the applied tree.
grep -q 'p7580_sched: err stage=%s rc=%ld' drivers/gpu/drm/drm_sched.c \
	|| gate_fail "shim err stage string missing in applied drm_sched.c"
grep -q 'p7580_sched: submit_err stage=%s rc=%ld' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "driver submit_err stage string missing in applied submit.c"
# staged call sites: int39's split consolidates the per-node build in
# p7580_drv_split_queue, so a dependency-attach failure returns through
# the caller's single staged `queue` path (no silent errno). The three
# attach sites themselves must all bail to err_sched.
grep -q 'p7580_submit_err("queue", ret);' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "per-node queue staged error missing in applied submit.c"
SQ_SRC=$(awk '/^static int p7580_drv_split_queue/,/^}/' drivers/gpu/drm/panfrost/panfrost7580_submit.c)
[ "$(echo "$SQ_SRC" | grep -c 'drm_sched_job_add_dependency(')" = 3 ] \
	|| gate_fail "split_queue must carry 3 add_dependency sites, got $(echo "$SQ_SRC" | grep -c 'drm_sched_job_add_dependency(')"
[ "$(echo "$SQ_SRC" | grep -c 'goto err_sched;')" -ge 3 ] \
	|| gate_fail "split_queue dep/queue failures must bail to err_sched"
# symbols/relocs live in the objects (merge + err helpers are static and
# inlined into the single exported add_dependency at -O2, so the nm
# evidence is the static AND-node symbols + the exported entry point;
# the reloc evidence is the inlined body's dma_fence_init /
# printk / leaf-callback references inside add_dependency's disasm).
aarch64-linux-gnu-nm "$SCHED_O" | grep -q ' r drm_sched_dep_and_ops$' \
	|| gate_fail "drm_sched_dep_and_ops missing in drm_sched.o"
aarch64-linux-gnu-nm "$SCHED_O" | grep -q ' t drm_sched_dep_and_leaf_fired$' \
	|| gate_fail "drm_sched_dep_and_leaf_fired missing in drm_sched.o"
aarch64-linux-gnu-nm "$SCHED_O" | grep -q ' T drm_sched_job_add_dependency$' \
	|| gate_fail "drm_sched_job_add_dependency missing in drm_sched.o"
aarch64-linux-gnu-nm "$SCHED_O" | grep -E 'drm_sched_dep_and_(ops|leaf_fired)|drm_sched_job_add_dependency' | tee -a "$EVID"
# the inlined merge body inside add_dependency references the AND node
# (dma_fence_init for the node alloc, leaf callbacks for the trampolines,
# printk for the inlined err-helper stage prints).
aarch64-linux-gnu-objdump -dr --disassemble=drm_sched_job_add_dependency "$SCHED_O" \
	| grep -q 'R_AARCH64_CALL26[[:space:]]*dma_fence_init' \
	|| gate_fail "no dma_fence_init reloc in add_dependency disasm (AND-node alloc path)"
aarch64-linux-gnu-objdump -dr --disassemble=drm_sched_job_add_dependency "$SCHED_O" \
	| grep -q 'drm_sched_dep_and_leaf_cb_' \
	|| gate_fail "no leaf-callback reloc in add_dependency disasm (AND-node trampolines)"
aarch64-linux-gnu-objdump -dr --disassemble=drm_sched_job_add_dependency "$SCHED_O" \
	| grep -q 'R_AARCH64_CALL26[[:space:]]*printk' \
	|| gate_fail "no printk reloc in add_dependency disasm (inlined err-helper stage prints)"
aarch64-linux-gnu-objdump -dr --disassemble=drm_sched_job_add_dependency "$SCHED_O" \
	| grep -E 'dma_fence_init|dep_and_leaf_cb_|printk' | tee -a "$EVID"
# AND-node strings + stage errno strings live in the objects.
aarch64-linux-gnu-strings "$SCHED_O" | grep -q 'p7580_sched: err stage=' \
	|| gate_fail "err stage= missing in drm_sched.o"
aarch64-linux-gnu-strings "$SCHED_O" | grep -q 'drm_sched_dep_and' \
	|| gate_fail "dep_and driver name missing in drm_sched.o"
aarch64-linux-gnu-strings "$SUBMIT_O" | grep -q 'p7580_sched: submit_err stage=' \
	|| gate_fail "submit_err stage= missing in submit.o"
# linked Image: stage errno strings live (AND-node ops is static data,
# checked in the object above; the linked evidence is the strings).
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'p7580_sched: err stage=' \
	|| gate_fail "err stage= missing in Image"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'p7580_sched: submit_err stage=' \
	|| gate_fail "submit_err stage= missing in Image"
grep -q ' T drm_sched_job_add_dependency$' System.map \
	|| gate_fail "drm_sched_job_add_dependency missing in System.map"
grep 'drm_sched_job_add_dependency$' System.map | tee -a "$EVID"
note "Image err stage= count = $(aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -c 'p7580_sched: err stage=')"
note "Image submit_err stage= count = $(aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -c 'p7580_sched: submit_err stage=')"
note "== int49 multidep gate: PASS (wait-all AND node live, stage errno strings live, no EOPNOTSUPP) =="
# 6t. serialfence gates (0072a74: per-file implicit serialization --
# each SUBMIT after arm borrows the previous submit's armed fence
# (serial_prev under serial_lock) and attaches it as a dependency
# when still unsignaled, then publishes its own armed fence; rollback
# clears the slot on error paths, fini drops the last slot ref; slot
# assignment untouched). FAIL (exit 1, scratch kept) on disagreement.
note "== int49 gate: serial wiring present =="
grep -q 'struct mutex serial_lock;' drivers/gpu/drm/panfrost/panfrost7580_drv.h \
	|| gate_fail "serial_lock missing in applied drv.h"
grep -q 'struct dma_fence \*serial_prev;' drivers/gpu/drm/panfrost/panfrost7580_drv.h \
	|| gate_fail "serial_prev missing in applied drv.h"
grep -q 'mutex_init(&df->serial_lock);' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "serial_lock init missing in applied submit.c"
grep -q 'df->serial_prev = NULL;' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "serial_prev NULL init missing in applied submit.c"
grep -q 'serial_prev = df->serial_prev;' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "serial_prev borrow missing in applied submit.c"
grep -q '!dma_fence_is_signaled(prev)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "signaled-skip guard missing in applied submit.c"
grep -q 'drm_sched_job_add_dependency(sched_job, prev)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "serial implicit dep attach missing in applied submit.c"
grep -q 'df->serial_prev = last_fence;' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "serial_prev publish missing in applied submit.c"
# int39: the split publishes the LAST queued node's fence, so the
# publish sits inside `if (queued)` (after the queue loop, before the
# err_split label). A split that queued nothing publishes nothing and
# the err path only drops its local refs, which is why the old
# published-pointer rollback is no longer needed.
QUEUE_GUARD=$(grep -n 'if (queued) {' drivers/gpu/drm/panfrost/panfrost7580_submit.c | head -1 | cut -d: -f1)
PUB_LN=$(grep -n 'df->serial_prev = last_fence;' drivers/gpu/drm/panfrost/panfrost7580_submit.c | cut -d: -f1)
ERRSPLIT=$(grep -n '^err_split:' drivers/gpu/drm/panfrost/panfrost7580_submit.c | cut -d: -f1)
[ -n "$QUEUE_GUARD" ] && [ -n "$PUB_LN" ] && [ -n "$ERRSPLIT" ] && \
	[ "$QUEUE_GUARD" -lt "$PUB_LN" ] && [ "$PUB_LN" -lt "$ERRSPLIT" ] \
	|| gate_fail "serial publish is not the guarded post-queue block (guard=$QUEUE_GUARD pub=$PUB_LN err=$ERRSPLIT)"
[ "$(grep -c 'df->serial_prev = NULL;' drivers/gpu/drm/panfrost/panfrost7580_submit.c)" = 2 ] \
	|| gate_fail "serial_prev must be NULLed exactly twice (open init + fini), got $(grep -c 'df->serial_prev = NULL;' drivers/gpu/drm/panfrost/panfrost7580_submit.c)"
[ "$(grep -c 'drm_sched_job_add_dependency(' drivers/gpu/drm/panfrost/panfrost7580_submit.c)" = 3 ] \
	|| gate_fail "submit.c must carry exactly 3 add_dependency sites (in_sync loop, excl loop, prev), got $(grep -c 'drm_sched_job_add_dependency(' drivers/gpu/drm/panfrost/panfrost7580_submit.c)"
grep -n 'serial_lock\|serial_prev\|last_fence\|add_dependency(sched_job, prev)' drivers/gpu/drm/panfrost/panfrost7580_submit.c | tee -a "$EVID"
# disasm evidence: the serial attach path inside p7580_drv_submit calls
# add_dependency 3x (in_sync loop + excl + serial prev). The
# dma_fence_is_signaled skip-guard is a static inline (test_bit on
# fence->flags in dma-fence.h): no out-of-line call at -O2, so the
# compiled evidence is the tree guard (gated above) plus the
# add_dependency relocs + the U ref; source-gate the exact inline use.
aarch64-linux-gnu-objdump -dr --disassemble=$SUBMIT_BODY "$SUBMIT_O" \
	| grep -q 'R_AARCH64_CALL26[[:space:]]*drm_sched_job_add_dependency' \
	|| gate_fail "no add_dependency reloc in p7580_drv_submit disasm (serial attach path)"
[ "$(aarch64-linux-gnu-objdump -dr --disassemble=$SUBMIT_BODY "$SUBMIT_O" \
	| grep -c 'R_AARCH64_CALL26[[:space:]]*drm_sched_job_add_dependency')" = 3 ] \
	|| gate_fail "expected exactly 3 add_dependency relocs in p7580_drv_submit (in_sync/excl/prev), got $(aarch64-linux-gnu-objdump -dr --disassemble=$SUBMIT_BODY "$SUBMIT_O" | grep -c 'R_AARCH64_CALL26[[:space:]]*drm_sched_job_add_dependency')"
grep -q '!dma_fence_is_signaled(prev)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "serial is_signaled inline guard missing in applied submit.c"
aarch64-linux-gnu-objdump -dr --disassemble=$SUBMIT_BODY "$SUBMIT_O" \
	| grep 'drm_sched_job_add_dependency' | tee -a "$EVID"
# fini drop + object linkage: serial fields referenced from submit.o,
# dep target resolved in the linked kernel.
aarch64-linux-gnu-nm "$SUBMIT_O" | grep -q ' U drm_sched_job_add_dependency$' \
	|| gate_fail "add_dependency U ref missing in submit.o"
aarch64-linux-gnu-nm "$SUBMIT_O" | grep -E 'drm_sched_job_add_dependency' | tee -a "$EVID"
grep -q ' T drm_sched_job_add_dependency$' System.map \
	|| gate_fail "add_dependency missing in System.map (serial dep target)"
note "== int49 serialfence gate: PASS (serial_prev borrow under serial_lock, prev dep attach, queued-guarded last_fence publish, fini drop live) =="
# 6u. cachedir gates (6d187b2: direction-correct DMA cache maintenance;
# CLEAN before ANY invalidate). Tree: helper defs/decls, clean-then-
# invalidate order, SUBMIT clean-only, unbind clean-before-unmap, no
# bare sync_for_cpu(BIDIRECTIONAL) left; objects: direction immediates
# (TO_DEVICE clean / FROM_DEVICE invalidate) through the dma_map_ops
# slots COMPUTED from the header, call order inside each helper,
# submit.o call sites (snapshot + JDBODY invalidate, no bare
# job_done_sync in the snapshot), System.map exports. FAIL (exit 1,
# scratch kept) on any disagreement.
note "== int49 gate: cachedir helpers present =="
GEMC=drivers/gpu/drm/panfrost/panfrost7580_gem.c
GEMH=drivers/gpu/drm/panfrost/panfrost7580_gem.h
SUBC=drivers/gpu/drm/panfrost/panfrost7580_submit.c
grep -q '^void p7580_gem_clean_for_device(struct drm_gem_object \*obj)$' "$GEMC" \
	|| gate_fail "p7580_gem_clean_for_device def missing in applied gem.c"
grep -q '^void p7580_gem_invalidate_for_cpu(struct drm_gem_object \*obj)$' "$GEMC" \
	|| gate_fail "p7580_gem_invalidate_for_cpu def missing in applied gem.c"
grep -q '^void p7580_gem_clean_for_device(struct drm_gem_object \*obj);$' "$GEMH" \
	|| gate_fail "clean_for_device decl missing in applied gem.h"
grep -q '^void p7580_gem_invalidate_for_cpu(struct drm_gem_object \*obj);$' "$GEMH" \
	|| gate_fail "invalidate_for_cpu decl missing in applied gem.h"
# the one shared page walk carries both directions and nothing else:
# exactly 2 dma_sync calls, DMA_TO_DEVICE on the for_device leg,
# DMA_FROM_DEVICE on the for_cpu leg, NEVER DMA_BIDIRECTIONAL.
SYNC_SRC=$(awk '/^static void p7580_bo_sync_pages/,/^}/' "$GEMC")
[ "$(echo "$SYNC_SRC" | grep -c 'dma_sync_single_for_')" = 2 ] \
	|| gate_fail "p7580_bo_sync_pages must carry exactly 2 dma_sync calls, got $(echo "$SYNC_SRC" | grep -c 'dma_sync_single_for_')"
echo "$SYNC_SRC" | grep -q 'DMA_TO_DEVICE' \
	|| gate_fail "DMA_TO_DEVICE missing on the for_device leg in applied gem.c"
echo "$SYNC_SRC" | grep -q 'DMA_FROM_DEVICE' \
	|| gate_fail "DMA_FROM_DEVICE missing on the for_cpu leg in applied gem.c"
echo "$SYNC_SRC" | grep -q 'DMA_BIDIRECTIONAL' \
	&& gate_fail "stale DMA_BIDIRECTIONAL sync still present in p7580_bo_sync_pages (direction-correct only)"
# invalidate = clean THEN invalidate (the order is the contract).
INV_SRC=$(awk '/^void p7580_gem_invalidate_for_cpu/,/^}/' "$GEMC")
INV_CLEAN=$(echo "$INV_SRC" | grep -n 'p7580_bo_sync_pages(bo, 1);' | cut -d: -f1)
INV_DROP=$(echo "$INV_SRC" | grep -n 'p7580_bo_sync_pages(bo, 0);' | cut -d: -f1)
[ -n "$INV_CLEAN" ] && [ -n "$INV_DROP" ] && [ "$INV_CLEAN" -lt "$INV_DROP" ] \
	|| gate_fail "invalidate_for_cpu is not clean-then-invalidate in applied gem.c (clean=$INV_CLEAN drop=$INV_DROP)"
# SUBMIT = clean only (never an invalidate: the BO body is the CPU's output).
SUB_SRC=$(awk '/^void p7580_gem_submit_sync/,/^}/' "$GEMC")
echo "$SUB_SRC" | grep -q 'p7580_gem_clean_for_device(obj);' \
	|| gate_fail "submit_sync must call clean_for_device in applied gem.c"
echo "$SUB_SRC" | grep -q 'invalidate_for_cpu\|DMA_FROM_DEVICE\|p7580_bo_sync_pages(bo, 0)' \
	&& gate_fail "submit_sync still invalidates (must be clean only) in applied gem.c"
# completion = clean-then-invalidate via the helper.
DONE_SRC=$(awk '/^static void p7580_gem_job_done_sync_body/,/^}/' "$GEMC")
echo "$DONE_SRC" | grep -q 'p7580_gem_invalidate_for_cpu(obj);' \
	|| gate_fail "job_done_sync must call invalidate_for_cpu in applied gem.c"
echo "$DONE_SRC" | grep -q 'dma_sync_single_for_cpu(bo->sync_dev' \
	&& gate_fail "job_done_sync still runs a bare sync_for_cpu in applied gem.c"
# unbind: clean BEFORE dma_unmap_page (BIDIRECTIONAL unmap invalidates).
UB_SRC=$(awk '/^void p7580_gem_dma_unbind/,/^}/' "$GEMC")
UB_CLEAN=$(echo "$UB_SRC" | grep -n 'p7580_gem_clean_for_device(obj);' | cut -d: -f1)
UB_UNMAP=$(echo "$UB_SRC" | grep -n 'dma_unmap_page(' | cut -d: -f1)
[ -n "$UB_CLEAN" ] && [ -n "$UB_UNMAP" ] && [ "$UB_CLEAN" -lt "$UB_UNMAP" ] \
	|| gate_fail "dma_unbind must clean before dma_unmap_page (clean=$UB_CLEAN unmap=$UB_UNMAP)"
grep -n 'p7580_gem_invalidate_for_cpu\|p7580_gem_clean_for_device\|dma_sync_single_for_' "$GEMC" | tee -a "$EVID"
# driver 0001 call sites: the chain-walk descriptor read (gpu_read)
# invalidates before the kmap copy, the JDBODY owner BO is invalidated
# before its dump, and SUBMIT carries no bare job_done_sync.
grep -q 'p7580_gem_invalidate_for_cpu(obj);' "$SUBC" \
	|| gate_fail "descriptor-read invalidate_for_cpu call missing in applied submit.c"
grep -q 'p7580_gem_invalidate_for_cpu(owner);' "$SUBC" \
	|| gate_fail "PTEWALK JDBODY invalidate_for_cpu call missing in applied submit.c"
[ "$(grep -c 'p7580_gem_job_done_sync(' "$SUBC")" = 1 ] \
	|| gate_fail "submit.c must carry exactly 1 job_done_sync call (free_job only), got $(grep -c 'p7580_gem_job_done_sync(' "$SUBC")"
# int39: the chain walk reads each descriptor through p7580_drv_gpu_read,
# which must clean-then-invalidate the BO before the kmap copy.
GR_SRC=$(awk '/^static int p7580_drv_gpu_read/,/^}/' "$SUBC")
GR_INV=$(echo "$GR_SRC" | grep -n 'p7580_gem_invalidate_for_cpu(obj);' | cut -d: -f1)
GR_RD=$(echo "$GR_SRC" | grep -n 'kv = kmap_atomic(bo->pages\[idx\]);' | cut -d: -f1)
[ -n "$GR_INV" ] && [ -n "$GR_RD" ] && [ "$GR_INV" -lt "$GR_RD" ] \
	|| gate_fail "descriptor read must invalidate the owner BO before the kmap copy (inv=$GR_INV read=$GR_RD)"
OWN_INV=$(grep -n 'p7580_gem_invalidate_for_cpu(owner);' "$SUBC" | cut -d: -f1)
JDBODY_LN=$(grep -n 'p7580_job: JDBODY' "$SUBC" | head -1 | cut -d: -f1)
[ -n "$OWN_INV" ] && [ -n "$JDBODY_LN" ] && [ "$OWN_INV" -lt "$JDBODY_LN" ] \
	|| gate_fail "JDBODY dump must invalidate the owner BO before the kmap read (inv=$OWN_INV dump=$JDBODY_LN)"
grep -n 'p7580_gem_invalidate_for_cpu\|p7580_gem_job_done_sync\|p7580_gem_submit_sync\|p7580_drv_gpu_read' "$SUBC" | tee -a "$EVID"
# tree-computed direction values + dma_map_ops slots (pahole-free): the
# immediates/slots the compiler emits must match the headers the tree
# was built with.
DIR_TO=$(awk '/enum dma_data_direction/,/};/' include/linux/dma-direction.h \
	| sed -n 's/.*DMA_TO_DEVICE = \([0-9][0-9]*\).*/\1/p')
DIR_FROM=$(awk '/enum dma_data_direction/,/};/' include/linux/dma-direction.h \
	| sed -n 's/.*DMA_FROM_DEVICE = \([0-9][0-9]*\).*/\1/p')
[ "$DIR_TO" = 1 ] || gate_fail "DMA_TO_DEVICE computed $DIR_TO != 1"
[ "$DIR_FROM" = 2 ] || gate_fail "DMA_FROM_DEVICE computed $DIR_FROM != 2"
DMAOPS_OFF=$(awk '/^struct dma_map_ops \{/,/^\};/' include/linux/dma-mapping.h \
	| awk 'match($0, /\(\*[a-z_0-9]+\)/) {
		n = substr($0, RSTART + 2, RLENGTH - 3);
		if (n == "sync_single_for_cpu") print "cpu=" off;
		if (n == "sync_single_for_device") print "dev=" off;
		if (n == "unmap_page") print "unmap=" off;
		off += 8 }')
OFF_CPU=$(echo "$DMAOPS_OFF" | sed -n 's/^cpu=//p')
OFF_DEV=$(echo "$DMAOPS_OFF" | sed -n 's/^dev=//p')
OFF_UNMAP=$(echo "$DMAOPS_OFF" | sed -n 's/^unmap=//p')
[ -n "$OFF_CPU" ] && [ -n "$OFF_DEV" ] && [ -n "$OFF_UNMAP" ] \
	|| gate_fail "could not compute dma_map_ops slots from include/linux/dma-mapping.h"
note "computed dma_map_ops slots: for_cpu=$OFF_CPU for_device=$OFF_DEV unmap=$OFF_UNMAP; dirs TO=$DIR_TO FROM=$DIR_FROM"
# object evidence: helper T symbols + the outlined page walk.
aarch64-linux-gnu-nm "$GEM_O" | grep -q ' T p7580_gem_clean_for_device$' \
	|| gate_fail "p7580_gem_clean_for_device not T in gem.o"
aarch64-linux-gnu-nm "$GEM_O" | grep -q ' T p7580_gem_invalidate_for_cpu$' \
	|| gate_fail "p7580_gem_invalidate_for_cpu not T in gem.o"
aarch64-linux-gnu-nm "$GEM_O" | grep -q ' t p7580_bo_sync_pages$' \
	|| gate_fail "p7580_bo_sync_pages not t in gem.o"
aarch64-linux-gnu-nm "$GEM_O" | grep -E ' p7580_(gem_(clean_for_device|invalidate_for_cpu)|bo_sync_pages)$' | tee -a "$EVID"
# the directions and slots the compiled walk really passes.
SYNC_DIS=$(aarch64-linux-gnu-objdump -dr --disassemble=p7580_bo_sync_pages "$GEM_O")
echo "$SYNC_DIS" | grep -Eq "mov[[:space:]]+w3, #0x$DIR_TO([^0-9a-f]|\$)" \
	|| gate_fail "no DMA_TO_DEVICE (w3=#0x$DIR_TO) immediate in p7580_bo_sync_pages disasm"
echo "$SYNC_DIS" | grep -Eq "mov[[:space:]]+w3, #0x$DIR_FROM([^0-9a-f]|\$)" \
	|| gate_fail "no DMA_FROM_DEVICE (w3=#0x$DIR_FROM) immediate in p7580_bo_sync_pages disasm"
echo "$SYNC_DIS" | grep -Eq "mov[[:space:]]+w3, #0x0([^0-9a-f]|\$)" \
	&& gate_fail "p7580_bo_sync_pages still passes DMA_BIDIRECTIONAL (w3=#0x0)"
echo "$SYNC_DIS" | grep -q "\[x[0-9]*, #$OFF_DEV\]" \
	|| gate_fail "no dma_ops sync_single_for_device slot (#$OFF_DEV) load in p7580_bo_sync_pages disasm"
echo "$SYNC_DIS" | grep -q "\[x[0-9]*, #$OFF_CPU\]" \
	|| gate_fail "no dma_ops sync_single_for_cpu slot (#$OFF_CPU) load in p7580_bo_sync_pages disasm"
# slot load -> direction immediate pairing (same basic block): the
# for_device leg must pass TO_DEVICE, the for_cpu leg FROM_DEVICE.
echo "$SYNC_DIS" | awk '
	/ldr[[:space:]]+x[0-9]*, \[x[0-9]*, #'"$OFF_DEV"'\]/ { want = 1; devline = NR }
	/ldr[[:space:]]+x[0-9]*, \[x[0-9]*, #'"$OFF_CPU"'\]/ { want = 2; cpuline = NR }
	/mov[[:space:]]+w3, #0x1([^0-9a-f]|$)/ { if (want == 1 && NR - devline <= 6) okdev = 1; want = 0 }
	/mov[[:space:]]+w3, #0x2([^0-9a-f]|$)/ { if (want == 2 && NR - cpuline <= 6) okcpu = 1; want = 0 }
	END { exit !(okdev && okcpu) }' \
	|| gate_fail "direction immediate not paired with its dma_ops slot in p7580_bo_sync_pages disasm"
echo "$SYNC_DIS" | grep -E 'ldr[[:space:]]+x[0-9]*, \[x[0-9]*, #'"$OFF_DEV"'\]|ldr[[:space:]]+x[0-9]*, \[x[0-9]*, #'"$OFF_CPU"'\]|mov[[:space:]]+w3, #0x[0-9]' | tee -a "$EVID"
# per-helper call counts + order (clean first, then the for_cpu leg).
CL_DIS=$(aarch64-linux-gnu-objdump -dr --disassemble=p7580_gem_clean_for_device "$GEM_O")
INV_DIS=$(aarch64-linux-gnu-objdump -dr --disassemble=p7580_gem_invalidate_for_cpu "$GEM_O")
SUB_DIS=$(aarch64-linux-gnu-objdump -dr --disassemble=p7580_gem_submit_sync "$GEM_O")
DONE_DIS=$(aarch64-linux-gnu-objdump -dr --disassemble=p7580_gem_job_done_sync "$GEM_O")
UB_DIS=$(aarch64-linux-gnu-objdump -dr --disassemble=p7580_gem_dma_unbind "$GEM_O")
[ "$(echo "$CL_DIS" | grep -c '<p7580_bo_sync_pages>')" = 1 ] \
	|| gate_fail "clean_for_device must make exactly 1 sync_pages call (clean only)"
echo "$CL_DIS" | grep -Eq 'mov[[:space:]]+w1, #0x0' \
	&& gate_fail "clean_for_device passes toward_device=0 (wrong direction)"
[ "$(echo "$SUB_DIS" | grep -c '<p7580_bo_sync_pages>')" = 1 ] \
	|| gate_fail "submit_sync must make exactly 1 sync_pages call (clean only)"
echo "$SUB_DIS" | grep -Eq 'mov[[:space:]]+w1, #0x0' \
	&& gate_fail "submit_sync passes toward_device=0 (must be clean only)"
[ "$(echo "$INV_DIS" | grep -c '<p7580_bo_sync_pages>')" = 2 ] \
	|| gate_fail "invalidate_for_cpu must make exactly 2 sync_pages calls (clean + invalidate)"
echo "$INV_DIS" | awk '
	/<p7580_bo_sync_pages>/ { n++; if (n == 1) first = NR; if (n == 2) second = NR }
	/mov[[:space:]]+w1, #0x0/ { z = NR }
	END { exit !(first && second && z && z > first && second > z) }' \
	|| gate_fail "clean-then-invalidate call order broken in invalidate_for_cpu (object)"
# int49: with the perf overlay, job_done_sync's unarmed path is the
# compiler-specialised direct call to invalidate_for_cpu (the body is
# inlined, so no sync_pages calls appear in this function); the helper's
# own 2-call order above is the clean-then-invalidate proof.
[ "$(echo "$DONE_DIS" | grep -c '<p7580_gem_invalidate_for_cpu>')" -ge 1 ] \
	|| gate_fail "job_done_sync does not route completion through invalidate_for_cpu (1 unarmed, 2 with the armed body)"
echo "$INV_DIS" | grep -E '<p7580_bo_sync_pages>|mov[[:space:]]+w1, #0x0' | tee -a "$EVID"
# unbind: helper call before the unmap_page dma_ops slot, BIDIRECTIONAL
# (w3=#0x0) on the unmap itself.
UB_CALL=$(echo "$UB_DIS" | grep -n '<p7580_gem_clean_for_device>' | cut -d: -f1)
UB_SLOT=$(echo "$UB_DIS" | grep -n "\[x[0-9]*, #$OFF_UNMAP\]" | head -1 | cut -d: -f1)
[ -n "$UB_CALL" ] && [ -n "$UB_SLOT" ] && [ "$UB_CALL" -lt "$UB_SLOT" ] \
	|| gate_fail "dma_unbind must call clean_for_device before the unmap_page slot (call=$UB_CALL slot=$UB_SLOT)"
echo "$UB_DIS" | grep -Eq 'mov[[:space:]]+w3, #0x0([^0-9a-f]|$)' \
	|| gate_fail "dma_unbind unmap is not DMA_BIDIRECTIONAL (w3=#0x0) in object"
echo "$UB_DIS" | grep -E '<p7580_gem_clean_for_device>|\[x[0-9]*, #'"$OFF_UNMAP"'\]|mov[[:space:]]+w3, #0x0' | tee -a "$EVID"
# debug read + pre-signal completion both route through the
# clean-then-invalidate helper (no bare invalidate anywhere).
aarch64-linux-gnu-objdump -dr --disassemble=p7580_gem_debug_read_word "$GEM_O" \
	| grep -qE '<p7580_gem_(invalidate_for_cpu|job_done_sync)>' \
	|| gate_fail "debug_read_word does not route through the completion sync (invalidate_for_cpu via job_done_sync)"
aarch64-linux-gnu-objdump -dr --disassemble=p7580_gem_job_bos_pre_signal_sync "$GEM_O" \
	| grep -qE '<p7580_gem_(invalidate_for_cpu|job_done_sync)>' \
	|| gate_fail "pre_signal completion does not route through the completion sync"
# submit.o: descriptor-read + JDBODY call sites; free_job keeps
# job_done_sync. int39: the descriptor read (gpu_read) is outlined and
# carries the single invalidate; SUBMIT's two submit_sync calls are the
# BO clean loop and the shadow-descriptor publish (isolate rewrite).
aarch64-linux-gnu-nm "$SUBMIT_O" | grep -q ' U p7580_gem_invalidate_for_cpu$' \
	|| gate_fail "invalidate_for_cpu U ref missing in submit.o"
aarch64-linux-gnu-nm "$SUBMIT_O" | grep -q ' U p7580_gem_submit_sync$' \
	|| gate_fail "submit_sync U ref missing in submit.o"
[ "$(aarch64-linux-gnu-objdump -dr --disassemble=p7580_drv_gpu_read "$SUBMIT_O" \
	| grep -c 'R_AARCH64_CALL26[[:space:]]*p7580_gem_invalidate_for_cpu')" = 1 ] \
	|| gate_fail "descriptor read must carry exactly 1 invalidate_for_cpu call in p7580_drv_gpu_read"
[ "$(aarch64-linux-gnu-objdump -dr "$SUBMIT_O" \
	| grep -c 'R_AARCH64_CALL26[[:space:]]*p7580_gem_invalidate_for_cpu')" = 2 ] \
	|| gate_fail "submit.o must carry exactly 2 invalidate_for_cpu calls (descriptor read + JDBODY), got $(aarch64-linux-gnu-objdump -dr "$SUBMIT_O" | grep -c 'R_AARCH64_CALL26[[:space:]]*p7580_gem_invalidate_for_cpu')"
[ "$(aarch64-linux-gnu-objdump -dr --disassemble=$SUBMIT_BODY "$SUBMIT_O" \
	| grep -c 'R_AARCH64_CALL26[[:space:]]*p7580_gem_submit_sync')" = 2 ] \
	|| gate_fail "p7580_drv_submit must carry 2 submit_sync calls (BO clean loop + shadow publish)"
aarch64-linux-gnu-objdump -dr --disassemble=$SUBMIT_BODY "$SUBMIT_O" \
	| grep -q 'job_done_sync' \
	&& gate_fail "snapshot still runs the bare job_done_sync in p7580_drv_submit"
aarch64-linux-gnu-objdump -dr --disassemble=p7580_drv_ptewalk_pin "$SUBMIT_O" \
	| grep -q 'R_AARCH64_CALL26[[:space:]]*p7580_gem_invalidate_for_cpu' \
	|| gate_fail "JDBODY dump does not invalidate the owner BO in p7580_drv_ptewalk_pin"
aarch64-linux-gnu-objdump -dr --disassemble=p7580_drv_free_job "$SUBMIT_O" \
	| grep -q 'R_AARCH64_CALL26[[:space:]]*p7580_gem_job_done_sync' \
	|| gate_fail "free_job post-signal job_done_sync lost"
aarch64-linux-gnu-objdump -dr --disassemble=$SUBMIT_BODY "$SUBMIT_O" \
	| grep -E 'p7580_gem_(invalidate_for_cpu|submit_sync)' | tee -a "$EVID"
aarch64-linux-gnu-objdump -dr --disassemble=p7580_drv_ptewalk_pin "$SUBMIT_O" \
	| grep -E 'p7580_gem_invalidate_for_cpu' | tee -a "$EVID"
aarch64-linux-gnu-objdump -dr --disassemble=p7580_drv_free_job "$SUBMIT_O" \
	| grep -E 'p7580_gem_job_done_sync' | tee -a "$EVID"
# linked kernel: exactly one strong def each.
[ "$(grep -c ' T p7580_gem_clean_for_device$' System.map)" = 1 ] \
	|| gate_fail "System.map p7580_gem_clean_for_device count != 1"
[ "$(grep -c ' T p7580_gem_invalidate_for_cpu$' System.map)" = 1 ] \
	|| gate_fail "System.map p7580_gem_invalidate_for_cpu count != 1"
grep -E ' (p7580_gem_clean_for_device|p7580_gem_invalidate_for_cpu)$' System.map | tee -a "$EVID"
note "== int49 cachedir gate: PASS (clean-for-device TO_DEVICE + clean-then-invalidate FROM_DEVICE, submit clean-only, unbind clean-before-unmap, descriptor-read/JDBODY/pre-signal routed, no bare BIDIRECTIONAL) =="

# 6v. mmaprefs gates (de21448 gpu31-mmaprefs: user mappings hold real
# page references). Tree: fault/mmap/ledger defs+decls, vm_ops table
# wired to the fault/open/close hooks, mmap's VM_PFNMAP->VM_MIXEDMAP|
# VM_DONTCOPY switch before the 0->1 mapping pin, vm_insert_page in the
# fault insert path (vm_insert_pfn only for the PFNMAP CMA/import
# shapes), per-page get_page/put_page around the VMA lifetime, the
# free-path zero guards, and both ledger calls. Object: symbols,
# .rodata vm_ops reloc order open/close/fault, the flag masks
# recomputed from include/linux/mm.h, fault's insert_page, mmap's pin
# order, open->map_pin / close->put_page+warn, submit.o's single "sub"
# ledger call, System.map exports, Image strings. FAIL (exit 1,
# scratch kept) on any disagreement.
note "== int49 gate: mmaprefs sources present =="
[ "$(sha256sum "$DRV1" | cut -d' ' -f1)" = "$DRV1_SHA" ] \
	|| gate_fail "driver 0001 changed while the build ran (source drift; rebuilt report needed)"
note "source: repo HEAD=$REPO_HEAD driver-0001 sha256=$DRV1_SHA"
grep -q '^int p7580_gem_fault(struct vm_area_struct \*vma, struct vm_fault \*vmf)$' "$GEMC" \
	|| gate_fail "p7580_gem_fault def missing in applied gem.c"
grep -q '^int p7580_gem_mmap(struct file \*filp, struct vm_area_struct \*vma)$' "$GEMC" \
	|| gate_fail "p7580_gem_mmap def missing in applied gem.c"
grep -q '^void p7580_gem_debug_page_ledger(struct drm_gem_object \*obj, const char \*tag,$' "$GEMC" \
	|| gate_fail "p7580_gem_debug_page_ledger def missing in applied gem.c"
grep -q '^const struct vm_operations_struct p7580_gem_vm_ops = {' "$GEMC" \
	|| gate_fail "p7580_gem_vm_ops table missing in applied gem.c"
grep -q '^int p7580_gem_fault(struct vm_area_struct \*vma, struct vm_fault \*vmf);$' "$GEMH" \
	|| gate_fail "p7580_gem_fault decl missing in applied gem.h"
grep -q '^int p7580_gem_mmap(struct file \*filp, struct vm_area_struct \*vma);$' "$GEMH" \
	|| gate_fail "p7580_gem_mmap decl missing in applied gem.h"
grep -q '^extern const struct vm_operations_struct p7580_gem_vm_ops;$' "$GEMH" \
	|| gate_fail "p7580_gem_vm_ops extern decl missing in applied gem.h"
grep -q '^void p7580_gem_debug_page_ledger' "$GEMH" \
	|| gate_fail "p7580_gem_debug_page_ledger decl missing in applied gem.h"
grep -q '	unsigned int vma_refs;' "$GEMH" \
	|| gate_fail "bo->vma_refs missing in applied gem.h"
grep -q '	unsigned int map_pinned;' "$GEMH" \
	|| gate_fail "bo->map_pinned missing in applied gem.h"
# vm_ops carries exactly the three hooks, wired to our wrappers.
VMOPS_SRC=$(awk '/^const struct vm_operations_struct p7580_gem_vm_ops = \{/,/^\};/' "$GEMC")
[ "$(echo "$VMOPS_SRC" | grep -c '^	\.')" = 3 ] \
	|| gate_fail "p7580_gem_vm_ops must initialise exactly 3 members, got $(echo "$VMOPS_SRC" | grep -c '^	\.')"
echo "$VMOPS_SRC" | grep -q '^	.fault = p7580_gem_fault,$' \
	|| gate_fail "vm_ops .fault is not p7580_gem_fault in applied gem.c"
echo "$VMOPS_SRC" | grep -q '^	.open = p7580_gem_vm_open,$' \
	|| gate_fail "vm_ops .open is not the pin wrapper in applied gem.c"
echo "$VMOPS_SRC" | grep -q '^	.close = p7580_gem_vm_close,$' \
	|| gate_fail "vm_ops .close is not the unpin wrapper in applied gem.c"
grep -q '	.gem_vm_ops = &p7580_gem_vm_ops,' drivers/gpu/drm/panfrost/panfrost7580_main.c \
	|| gate_fail "driver main.c does not publish p7580_gem_vm_ops"
# fault: page insert for SHMEM, pfn insert only for CMA/imported.
MAP_SRC=$(awk '/^static int p7580_map_page/,/^}/' "$GEMC")
echo "$MAP_SRC" | grep -q 'return vm_insert_page(vma, f_vaddr, page);' \
	|| gate_fail "SHMEM fault path does not vm_insert_page in applied gem.c"
echo "$MAP_SRC" | grep -q 'return vm_insert_pfn(vma, f_vaddr, pfn);' \
	|| gate_fail "CMA/imported fault path lost vm_insert_pfn in applied gem.c"
FAULT_SRC=$(awk '/^int p7580_gem_fault/,/^}/' "$GEMC")
echo "$FAULT_SRC" | grep -q 'p7580_map_page(bo, vma, f_vaddr, page_offset);' \
	|| gate_fail "p7580_gem_fault does not route through p7580_map_page"
echo "$FAULT_SRC" | grep -q 'mutex_lock' \
	&& gate_fail "fault path takes a lock (must stay lock-free; state immutable)"
grep -q '	case -EBUSY:' "$GEMC" \
	|| gate_fail "fault retry arm lost -EBUSY (raced vm_insert_page)"
# mmap: flag switch before the 0->1 mapping pin, mutex-held; the
# immediate mappings (dma_mmap_attrs / remap_pfn_range) stay untouched.
MMAP_SRC=$(awk '/^int p7580_gem_mmap/,/^}/' "$GEMC")
MM_FLAGS=$(echo "$MMAP_SRC" | grep -n 'vma->vm_flags &= ~VM_PFNMAP;' | cut -d: -f1)
MM_SET=$(echo "$MMAP_SRC" | grep -n 'vma->vm_flags |= VM_MIXEDMAP | VM_DONTCOPY;' | cut -d: -f1)
MM_GET=$(echo "$MMAP_SRC" | grep -n 'p7580_gem_vm_get(bo);' | cut -d: -f1)
MM_LOCK=$(echo "$MMAP_SRC" | grep -n 'mutex_lock(&obj->dev->struct_mutex);' | cut -d: -f1)
[ -n "$MM_FLAGS" ] && [ -n "$MM_SET" ] && [ -n "$MM_GET" ] && [ -n "$MM_LOCK" ] \
	&& [ "$MM_FLAGS" -lt "$MM_SET" ] && [ "$MM_SET" -lt "$MM_LOCK" ] && [ "$MM_LOCK" -lt "$MM_GET" ] \
	|| gate_fail "mmap flag switch/pin order broken (pfnmap=$MM_FLAGS set=$MM_SET lock=$MM_LOCK get=$MM_GET)"
echo "$MMAP_SRC" | grep -q 'dma_mmap_attrs(obj->dev->dev, vma' \
	|| gate_fail "CMA dma_mmap_attrs path lost in applied gem.c"
echo "$MMAP_SRC" | grep -q 'remap_pfn_range(vma, vma->vm_start + off, pfn,' \
	|| gate_fail "PRIME-imported remap_pfn_range path lost in applied gem.c"
# pin accounting: per-page get/put, counted by vma_refs, serialised.
PIN_SRC=$(awk '/^static void p7580_gem_map_pin/,/^}/' "$GEMC")
UNPIN_SRC=$(awk '/^static void p7580_gem_map_unpin/,/^}/' "$GEMC")
VGET_SRC=$(awk '/^static void p7580_gem_vm_get/,/^}/' "$GEMC")
VPUT_SRC=$(awk '/^static void p7580_gem_vm_put/,/^}/' "$GEMC")
echo "$PIN_SRC" | grep -q 'get_page(bo->pages\[i\]);' \
	|| gate_fail "map_pin does not get_page every BO page"
echo "$PIN_SRC" | grep -q 'bo->map_pinned = 1;' \
	|| gate_fail "map_pin does not set map_pinned"
echo "$UNPIN_SRC" | grep -q 'put_page(bo->pages\[i\]);' \
	|| gate_fail "map_unpin does not put_page every BO page"
echo "$VGET_SRC" | grep -q 'if (bo->vma_refs++ == 0)' \
	&& echo "$VGET_SRC" | grep -q 'p7580_gem_map_pin(bo);' \
	|| gate_fail "vm_get does not pin on the 0->1 VMA"
echo "$VPUT_SRC" | grep -q 'if (--bo->vma_refs == 0)' \
	&& echo "$VPUT_SRC" | grep -q 'p7580_gem_map_unpin(bo);' \
	|| gate_fail "vm_put does not unpin on the 1->0 close"
echo "$VPUT_SRC" | grep -q 'WARN_ON_ONCE(1);' \
	|| gate_fail "vm_put lost the guard for a close without a take"
OPEN_SRC=$(awk '/^static void p7580_gem_vm_open/,/^}/' "$GEMC")
CLOSE_SRC=$(awk '/^static void p7580_gem_vm_close/,/^}/' "$GEMC")
echo "$OPEN_SRC" | grep -q 'p7580_gem_vm_get(bo);' \
	&& echo "$OPEN_SRC" | grep -q 'drm_gem_vm_open(vma);' \
	|| gate_fail "vm_open does not pin then delegate to drm_gem_vm_open"
echo "$CLOSE_SRC" | grep -q 'p7580_gem_vm_put(bo);' \
	&& echo "$CLOSE_SRC" | grep -q 'drm_gem_vm_close(vma);' \
	|| gate_fail "vm_close does not unpin then delegate to drm_gem_vm_close"
echo "$OPEN_SRC" | grep -q 'mutex_lock(&dev->struct_mutex);' \
	&& echo "$CLOSE_SRC" | grep -q 'mutex_lock(&dev->struct_mutex);' \
	|| gate_fail "vm_open/close lost the struct_mutex serialisation"
# free: zero guards + leaked-pin drop.
FREE_SRC=$(awk '/^void p7580_gem_free_object/,/^}/' "$GEMC")
echo "$FREE_SRC" | grep -q 'WARN_ON_ONCE(bo->vma_refs != 0)' \
	|| gate_fail "free lost the vma_refs zero guard"
echo "$FREE_SRC" | grep -q 'WARN_ON_ONCE(bo->map_pinned)' \
	|| gate_fail "free lost the map_pinned zero guard"
echo "$FREE_SRC" | grep -q 'p7580_gem_map_unpin(bo);' \
	|| gate_fail "free does not drop a leaked mapping pin"
# ledger: format + both call sites ("0xcf" after the completion sync,
# "sub" per BO at SUBMIT under a 8-line budget).
LEDGER_SRC=$(awk '/^void p7580_gem_debug_page_ledger/,/^}/' "$GEMC")
echo "$LEDGER_SRC" | grep -q 'p7580_pin: %s obj=%lu iova=0x%llx idx=%u count=%d mapcount=%d vma=%u pin=%u' \
	|| gate_fail "ledger format string lost in applied gem.c"
grep -q 'p7580_gem_debug_page_ledger(obj, "0xcf", idx);' "$GEMC" \
	|| gate_fail "0xCF ledger call missing in applied gem.c"
DRW_SRC=$(awk '/^int p7580_gem_debug_read_word/,/^}/' "$GEMC")
DRW_INV=$(echo "$DRW_SRC" | grep -n 'p7580_gem_job_done_sync(obj);' | cut -d: -f1)
DRW_LED=$(echo "$DRW_SRC" | grep -n 'p7580_gem_debug_page_ledger(obj, "0xcf", idx);' | cut -d: -f1)
[ -n "$DRW_INV" ] && [ -n "$DRW_LED" ] && [ "$DRW_INV" -lt "$DRW_LED" ] \
	|| gate_fail "0xCF ledger does not follow the completion sync (inv=$DRW_INV led=$DRW_LED)"
grep -q 'p7580_gem_debug_page_ledger(obj, "sub", 0);' "$SUBC" \
	|| gate_fail "SUBMIT ledger call missing in applied submit.c"
grep -q '	unsigned int pin_budget = 8;' "$SUBC" \
	|| gate_fail "SUBMIT ledger budget (8) missing in applied submit.c"
SUB_LED=$(grep -n 'p7580_gem_debug_page_ledger(obj, "sub", 0);' "$SUBC" | cut -d: -f1)
SUB_BUD=$(grep -n 'if (pin_budget) {' "$SUBC" | cut -d: -f1)
[ -n "$SUB_LED" ] && [ -n "$SUB_BUD" ] && [ "$SUB_BUD" -lt "$SUB_LED" ] \
	|| gate_fail "SUBMIT ledger call is not budgeted (budget=$SUB_BUD call=$SUB_LED)"
grep -n 'p7580_gem_debug_page_ledger\|pin_budget' "$GEMC" "$SUBC" | tee -a "$EVID"
# computed flag values (mm.h is the tree the objects were built against).
VM_PFNMAP=$(awk '$1 == "#define" && $2 == "VM_PFNMAP" { print $3; exit }' include/linux/mm.h)
VM_MIXEDMAP=$(awk '$1 == "#define" && $2 == "VM_MIXEDMAP" { print $3; exit }' include/linux/mm.h)
VM_DONTCOPY=$(awk '$1 == "#define" && $2 == "VM_DONTCOPY" { print $3; exit }' include/linux/mm.h)
[ -n "$VM_PFNMAP" ] && [ -n "$VM_MIXEDMAP" ] && [ -n "$VM_DONTCOPY" ] \
	|| gate_fail "could not parse VM_PFNMAP/VM_MIXEDMAP/VM_DONTCOPY from include/linux/mm.h"
[ $((VM_PFNMAP)) -eq 1024 ] && [ $((VM_MIXEDMAP)) -eq 268435456 ] && [ $((VM_DONTCOPY)) -eq 131072 ] \
	|| gate_fail "unexpected vm_flags values: pfnmap=$VM_PFNMAP mixedmap=$VM_MIXEDMAP dontcopy=$VM_DONTCOPY"
CLR_MASK=$(printf '%x' $(( ~VM_PFNMAP )))
SET_MASK=$(printf '%x' $(( VM_MIXEDMAP | VM_DONTCOPY )))
note "computed vm_flags: PFNMAP=$VM_PFNMAP clear=0x$CLR_MASK MIXEDMAP|DONTCOPY=0x$SET_MASK"
# object evidence: symbols + vm_ops reloc order + insert/pin call sites.
for sym in 'T p7580_gem_fault$' 'T p7580_gem_mmap$' 'T p7580_gem_debug_page_ledger$' \
	   't p7580_gem_vm_open$' 't p7580_gem_vm_close$' 't p7580_gem_map_pin$' 'R p7580_gem_vm_ops$'; do
	aarch64-linux-gnu-nm "$GEM_O" | grep -q " $sym" \
		|| gate_fail "gem.o symbol '$sym' missing"
done
[ "$(aarch64-linux-gnu-readelf -sW "$GEM_O" | awk '$8 == "p7580_gem_vm_ops" { print $3 }')" = 48 ] \
	|| gate_fail "p7580_gem_vm_ops is not a 48-byte object in gem.o"
VMREL=$(aarch64-linux-gnu-objdump -r -j .rodata "$GEM_O")
[ "$(echo "$VMREL" | grep -c 'R_AARCH64_ABS64')" = 3 ] \
	|| gate_fail "vm_ops .rodata must carry exactly 3 function pointers, got $(echo "$VMREL" | grep -c 'R_AARCH64_ABS64')"
echo "$VMREL" | grep -Eq '^0000000000000000[[:space:]]+R_AARCH64_ABS64[[:space:]]+p7580_gem_vm_open$' \
	|| gate_fail "vm_ops[0] is not p7580_gem_vm_open (unpinned open?)"
echo "$VMREL" | grep -Eq '^0000000000000008[[:space:]]+R_AARCH64_ABS64[[:space:]]+p7580_gem_vm_close$' \
	|| gate_fail "vm_ops[1] is not p7580_gem_vm_close (unpinned close?)"
echo "$VMREL" | grep -Eq '^0000000000000010[[:space:]]+R_AARCH64_ABS64[[:space:]]+p7580_gem_fault$' \
	|| gate_fail "vm_ops[2] is not p7580_gem_fault"
echo "$VMREL" | tee -a "$EVID"
aarch64-linux-gnu-nm "$GEM_O" | grep -E ' (p7580_gem_(fault|mmap|debug_page_ledger|vm_open|vm_close|map_pin|vm_ops))$' | tee -a "$EVID"
# fault: exactly one vm_insert_page (+ the PFNMAP fallback), mmap: none.
FAULT_DIS=$(aarch64-linux-gnu-objdump -dr --disassemble=p7580_gem_fault "$GEM_O")
[ "$(echo "$FAULT_DIS" | grep -c 'R_AARCH64_CALL26[[:space:]]*vm_insert_page')" = 1 ] \
	|| gate_fail "fault must carry exactly 1 vm_insert_page call, got $(echo "$FAULT_DIS" | grep -c 'R_AARCH64_CALL26[[:space:]]*vm_insert_page')"
[ "$(echo "$FAULT_DIS" | grep -c 'R_AARCH64_CALL26[[:space:]]*vm_insert_pfn')" = 1 ] \
	|| gate_fail "fault must keep exactly 1 vm_insert_pfn call (CMA/imported shape)"
echo "$FAULT_DIS" | grep -E 'vm_insert_(page|pfn)' | tee -a "$EVID"
MMAP_DIS=$(aarch64-linux-gnu-objdump -dr --disassemble=p7580_gem_mmap "$GEM_O")
echo "$MMAP_DIS" | grep -q 'vm_insert_page' \
	&& gate_fail "mmap still inserts pages eagerly (must be fault-driven)"
[ "$(echo "$MMAP_DIS" | grep -c 'R_AARCH64_CALL26[[:space:]]*remap_pfn_range')" = 1 ] \
	|| gate_fail "mmap lost the imported remap_pfn_range leg"
[ "$(echo "$MMAP_DIS" | grep -c 'R_AARCH64_CALL26[[:space:]]*drm_gem_mmap')" = 0 ] \
	|| gate_fail "mmap still calls core drm_gem_mmap() (gpu31-mmlock)"
[ "$(echo "$MMAP_DIS" | grep -c 'R_AARCH64_CALL26[[:space:]]*drm_mmap')" = 1 ] \
	|| gate_fail "mmap lost the legacy drm_mmap fallback for unknown offsets"
[ "$(echo "$MMAP_DIS" | grep -c 'R_AARCH64_CALL26[[:space:]]*drm_ht_find_item')" = 1 ] \
	|| gate_fail "mmap does not resolve through the driver-owned offset hash"
[ "$(echo "$MMAP_DIS" | grep -c '<p7580_gem_map_pin>')" = 1 ] \
	|| gate_fail "mmap must take exactly 1 mapping pin, got $(echo "$MMAP_DIS" | grep -c '<p7580_gem_map_pin>')"
# the compiled flag switch: clear PFNMAP then OR in MIXEDMAP|DONTCOPY,
# both before the pin call (the OR source immediate is materialised in
# a separate mov, so match the and, then the orr that consumes it).
MM_ORD=$(echo "$MMAP_DIS" | awk -v clr="#0x$CLR_MASK" -v set="#0x$SET_MASK" '
	/and[[:space:]]+x[0-9]+, x[0-9]+, / && index($0, clr) { if (!a) a = NR }
	/mov[[:space:]]+x[0-9]+, / && index($0, set) { m = NR }
	m && NR > m && /[[:space:]]orr[[:space:]]/ { o = NR; m = 0 }
	/<p7580_gem_map_pin>/ { p = NR }
	END { printf "%d %d %d", a, o, p }')
MM_AND=$(echo "$MM_ORD" | cut -d' ' -f1)
MM_ORR=$(echo "$MM_ORD" | cut -d' ' -f2)
MM_PIN=$(echo "$MM_ORD" | cut -d' ' -f3)
[ "$MM_AND" -gt 0 ] && [ "$MM_ORR" -gt 0 ] && [ "$MM_PIN" -gt 0 ] \
	&& [ "$MM_AND" -lt "$MM_ORR" ] && [ "$MM_ORR" -lt "$MM_PIN" ] \
	|| gate_fail "compiled mmap does not clear PFNMAP + set MIXEDMAP|DONTCOPY before the pin (and=$MM_AND orr=$MM_ORR pin=$MM_PIN)"
echo "$MMAP_DIS" | grep -E "#0x$CLR_MASK|#0x$SET_MASK|<p7580_gem_map_pin>" | tee -a "$EVID"
# open pins, close puts (and keeps the unmatched-close WARN).
OPEN_DIS=$(aarch64-linux-gnu-objdump -dr --disassemble=p7580_gem_vm_open "$GEM_O")
CLOSE_DIS=$(aarch64-linux-gnu-objdump -dr --disassemble=p7580_gem_vm_close "$GEM_O")
echo "$OPEN_DIS" | grep -q 'R_AARCH64_CALL26[[:space:]]*drm_gem_vm_open' \
	&& echo "$OPEN_DIS" | grep -q '<p7580_gem_map_pin>' \
	|| gate_fail "object vm_open does not pin + drm_gem_vm_open"
echo "$CLOSE_DIS" | grep -q 'R_AARCH64_CALL26[[:space:]]*drm_gem_vm_close' \
	&& echo "$CLOSE_DIS" | grep -q 'R_AARCH64_CALL26[[:space:]]*put_page' \
	|| gate_fail "object vm_close does not put_page + drm_gem_vm_close"
echo "$CLOSE_DIS" | grep -q 'warn_slowpath' \
	|| gate_fail "object vm_close lost the unmatched-close WARN"
echo "$OPEN_DIS" | grep -E '<p7580_gem_map_pin>|drm_gem_vm_open' | tee -a "$EVID"
echo "$CLOSE_DIS" | grep -E 'put_page|drm_gem_vm_close|warn_slowpath' | tee -a "$EVID"
# free drops a leaked pin (put_page) and keeps its guards (WARN paths).
FREE_DIS=$(aarch64-linux-gnu-objdump -dr --disassemble=p7580_gem_free_object "$GEM_O")
echo "$FREE_DIS" | grep -q 'R_AARCH64_CALL26[[:space:]]*put_page' \
	|| gate_fail "free does not drop a leaked mapping pin (no put_page)"
[ "$(echo "$FREE_DIS" | grep -c 'warn_slowpath')" -ge 2 ] \
	|| gate_fail "free lost its WARN guards"
echo "$FREE_DIS" | grep -E 'put_page|warn_slowpath' | tee -a "$EVID"
# refcount primitives linked from the objects (vm_insert_page/get_page
# are the new references).
for u in 'U vm_insert_page$' 'U vm_insert_pfn$' 'U put_page$'; do
	aarch64-linux-gnu-nm "$GEM_O" | grep -q " $u" \
		|| gate_fail "gem.o undefined ref '$u' missing"
done
MAIN_O=drivers/gpu/drm/panfrost/panfrost7580_main.o
[ -f "$MAIN_O" ] || gate_fail "$MAIN_O missing"
aarch64-linux-gnu-nm "$MAIN_O" | grep -q ' U p7580_gem_vm_ops$' \
	|| gate_fail "main.o does not reference p7580_gem_vm_ops"
# submit.o: exactly one ledger call in the whole object, inside SUBMIT.
[ "$(aarch64-linux-gnu-objdump -dr "$SUBMIT_O" | grep -c 'R_AARCH64_CALL26[[:space:]]*p7580_gem_debug_page_ledger')" = 1 ] \
	|| gate_fail "submit.o must carry exactly 1 ledger call, got $(aarch64-linux-gnu-objdump -dr "$SUBMIT_O" | grep -c 'R_AARCH64_CALL26[[:space:]]*p7580_gem_debug_page_ledger')"
aarch64-linux-gnu-nm "$SUBMIT_O" | grep -q ' U p7580_gem_debug_page_ledger$' \
	|| gate_fail "submit.o ledger U ref missing"
aarch64-linux-gnu-objdump -dr --disassemble=$SUBMIT_BODY "$SUBMIT_O" \
	| grep -q 'R_AARCH64_CALL26[[:space:]]*p7580_gem_debug_page_ledger' \
	|| gate_fail "SUBMIT does not call the page ledger"
aarch64-linux-gnu-strings -n 3 "$SUBMIT_O" | grep -qx 'sub' \
	|| gate_fail "submit.o lost the \"sub\" ledger tag"
aarch64-linux-gnu-objdump -dr --disassemble=$SUBMIT_BODY "$SUBMIT_O" \
	| grep -B10 'p7580_gem_debug_page_ledger' | tee -a "$EVID"
# the 0xCF read routes through the ledger after the sync.
DRW_DIS=$(aarch64-linux-gnu-objdump -dr --disassemble=p7580_gem_debug_read_word "$GEM_O")
# int49: with the overlay, job_done_sync is a real call (not inlined);
# it is gated above to route through invalidate_for_cpu, so accept either
# the helper call or the sync call as the completion leg.
DRW_I=$(echo "$DRW_DIS" | grep -n 'R_AARCH64_CALL26[[:space:]]*p7580_gem_\(invalidate_for_cpu\|job_done_sync\)' | head -1 | cut -d: -f1)
DRW_L=$(echo "$DRW_DIS" | grep -n 'R_AARCH64_CALL26[[:space:]]*p7580_gem_debug_page_ledger' | head -1 | cut -d: -f1)
[ -n "$DRW_I" ] && [ -n "$DRW_L" ] && [ "$DRW_I" -lt "$DRW_L" ] \
	|| gate_fail "0xCF read does not sync before the ledger (sync=$DRW_I led=$DRW_L)"
echo "$DRW_DIS" | grep -E 'invalidate_for_cpu|debug_page_ledger' | tee -a "$EVID"
# object strings: the ledger format + both tags are compiled in.
aarch64-linux-gnu-strings "$GEM_O" | grep -qF 'p7580_pin: %s obj=%lu iova=0x%llx idx=%u count=%d mapcount=%d vma=%u pin=%u' \
	|| gate_fail "ledger format string missing from gem.o"
aarch64-linux-gnu-strings -n 3 "$GEM_O" | grep -qx '0xcf' \
	|| gate_fail "0xCF ledger tag missing from gem.o"
# gpu31-mmlock: the per-mmap pr_info pair was removed with the core
# drm_gem_mmap() call; the mixedmap shape is gated above and the owner
# lookup is gated in 6ag.
aarch64-linux-gnu-strings "$GEM_O" | grep -E 'p7580_pin:' | tee -a "$EVID"
# linked kernel: one def each + the const vm_ops table.
[ "$(grep -c ' T p7580_gem_fault$' System.map)" = 1 ] \
	|| gate_fail "System.map p7580_gem_fault count != 1"
[ "$(grep -c ' T p7580_gem_mmap$' System.map)" = 1 ] \
	|| gate_fail "System.map p7580_gem_mmap count != 1"
[ "$(grep -c ' T p7580_gem_debug_page_ledger$' System.map)" = 1 ] \
	|| gate_fail "System.map p7580_gem_debug_page_ledger count != 1"
[ "$(grep -c ' R p7580_gem_vm_ops$' System.map)" = 1 ] \
	|| gate_fail "System.map p7580_gem_vm_ops not exactly one R"
grep -E ' (p7580_gem_(fault|mmap|debug_page_ledger|vm_ops))$' System.map | tee -a "$EVID"
# Image carries the ledger format (runtime dmesg evidence the flashed
# kernel can produce). The per-mmap "ok mixedmap" line is gone with the
# core drm_gem_mmap() call (gpu31-mmlock); the mixedmap shape is gated
# in source and object above.
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -qF 'p7580_pin: %s obj=%lu iova=0x%llx' \
	|| gate_fail "Image lost the p7580_pin ledger format"
note "== int49 mmaprefs gate: PASS (VM_MIXEDMAP fault-driven vm_insert_page, mapping pin per BO page on 0->1 VMA / put_page on last close, VM_DONTCOPY+clear PFNMAP, vm_ops open/close wired, free guards, 0xCF+sub ledger probes live) =="

# 6w. chainsplit gates (6fe2491 gpu31-chainsplit: driver-side per-node
# chain split at the VS/tiler boundary). Tree: chain walk (descriptor
# next pointers, 16-node cap, grounded job types), shadow isolate
# (next=NULL, dep1=dep2=0, index=pos+1) + shadow BO create/write/clean,
# one sched job per node on the SUBMIT entity ordered by the previous
# node's armed fence, a single out_sync install on the last queued node's
# fence, one-node pass-through (no shadow BO), per-node BO/PM refs with
# partial-queue unwind, and the per-node cfg path (every run_job ->
# p7580_job_hw_submit -> p7580_js_config with START/END_FLUSH_CLEAN_INV).
# Object: at -O2 split_queue/shadow_create/shadow_write/gpu_span are
# inlined into p7580_drv_submit, so the split is proven by the inlined
# fingerprint (one shadow gem_create, one job alloc/arm/submit, two
# descriptor reads, one shadow_put on the unwind path) plus the two
# outlined helpers (p7580_drv_gpu_read, p7580_drv_shadow_put) and
# hw_submit's folded cfg immediate in job.o. Host: the real-header
# chainsplit_test + submit_fix_test. FAIL (exit 1, scratch kept) on any
# disagreement.
note "== int49 gate: chainsplit sources present =="
# chain walk + bounds (start at args->jc; follow next; cap at 16).
grep -q 'uint64_t walk = args->jc;' "$SUBC" \
	|| gate_fail "chain walk start (walk = args->jc) missing in applied submit.c"
grep -q 'next = p7580_chain_hdr_next(hdr\[nodes - 1\]);' "$SUBC" \
	|| gate_fail "chain next-pointer decode missing in applied submit.c"
grep -q 'if (nodes == P7580_CHAIN_MAX_JOBS)' "$SUBC" \
	|| gate_fail "chain node cap missing in applied submit.c"
grep -q 'p7580_submit_err("chain", -E2BIG)' "$SUBC" \
	|| gate_fail "chain cap errno (-E2BIG) missing in applied submit.c"
grep -q 'if (!p7580_jc_valid(next))' "$SUBC" \
	|| gate_fail "chain next alignment check missing in applied submit.c"
grep -q 'if (!p7580_chain_job_bytes(' "$SUBC" \
	|| gate_fail "grounded payload-size rejection missing in applied submit.c"
# isolate + shadow fill/publish.
grep -q 'p7580_chain_hdr_isolate(jobw, ci);' "$SUBC" \
	|| gate_fail "chain-node isolate missing in applied submit.c"
grep -q 'memcpy(hdr\[ci\], jobw,' "$SUBC" \
	|| gate_fail "isolated header hand-off missing in applied submit.c"
grep -q 'p7580_drv_shadow_write(shadow->obj,' "$SUBC" \
	|| gate_fail "shadow descriptor write missing in applied submit.c"
grep -q 'p7580_gem_submit_sync(shadow->obj);' "$SUBC" \
	|| gate_fail "shadow descriptor clean (submit_sync) missing in applied submit.c"
grep -q '(size_t)ci \* P7580_CHAIN_SLOT_STRIDE,' "$SUBC" \
	|| gate_fail "per-node shadow slot offset missing in applied submit.c"
# one-node pass-through: no shadow unless nodes>1 or deps present.
grep -q 'need_shadow = nodes > 1 ||' "$SUBC" \
	|| gate_fail "one-node pass-through decision missing in applied submit.c"
grep -q 'p7580_chain_hdr_dep1(hdr\[0\]) ||' "$SUBC" \
	|| gate_fail "one-node dep1 pass-through condition missing in applied submit.c"
grep -q 'p7580_chain_hdr_dep2(hdr\[0\]);' "$SUBC" \
	|| gate_fail "one-node dep2 pass-through condition missing in applied submit.c"
grep -q 'if (need_shadow) {' "$SUBC" \
	|| gate_fail "shadow allocation guard missing in applied submit.c"
# shadow lifetime: create / one ref per node / last release unmaps.
grep -q 'shadow = p7580_drv_shadow_create(drv, dev, nodes);' "$SUBC" \
	|| gate_fail "shadow BO create missing in applied submit.c"
grep -q 'atomic_dec_and_test(&sh->refs)' "$SUBC" \
	|| gate_fail "shadow refcount release missing in applied submit.c"
grep -q 'p7580_gem_gpu_unmap(sh->obj);' "$SUBC" \
	|| gate_fail "shadow unmap on last release missing in applied submit.c"
grep -q 'atomic_set(&sh->refs, nodes);' "$SUBC" \
	|| gate_fail "per-node shadow reference count missing in applied submit.c"
# per-node queue helper + ordering + lifetime.
grep -q 'static int p7580_drv_split_queue(struct p7580_split_ctx \*s,' "$SUBC" \
	|| gate_fail "p7580_drv_split_queue def missing in applied submit.c"
grep -q 'p7580_drv_split_queue(&sctx, ci, jcs\[ci\],' "$SUBC" \
	|| gate_fail "p7580_drv_split_queue call missing in applied submit.c"
grep -q 'ci ? last_fence : serial_prev;' "$SUBC" \
	|| gate_fail "per-node previous-fence ordering missing in applied submit.c"
grep -q 'drm_gem_object_reference(s->objs\[i\]);' "$SUBC" \
	|| gate_fail "per-node BO reference missing in applied submit.c"
grep -q 'p7580_drv_job_pm_arm(s->drv);' "$SUBC" \
	|| gate_fail "per-node PM arm missing in applied submit.c"
grep -q 'p7580_drv_job_pm_release(s->drv);' "$SUBC" \
	|| gate_fail "per-node PM release on queue failure missing in applied submit.c"
grep -q 'for (ci = queued; ci < nodes; ci++)' "$SUBC" \
	|| gate_fail "partial-queue shadow unwind missing in applied submit.c"
grep -q 'p7580_drv_shadow_put(shadow);' "$SUBC" \
	|| gate_fail "shadow put on the unwind path missing in applied submit.c"
grep -q 'if (job && job->chain_shadow)' "$SUBC" \
	|| gate_fail "free_job shadow release missing in applied submit.c"
# out_sync: only the last node carries the handle; installed once.
grep -q 'if (pos + 1 == s->nodes)' "$SUBC" \
	|| gate_fail "last-node out_sync selection missing in applied submit.c"
grep -q 'job->out_sync = s->out_sync;' "$SUBC" \
	|| gate_fail "last-node out_sync stamp missing in applied submit.c"
grep -q 'if (queued && args->out_sync && last_fence) {' "$SUBC" \
	|| gate_fail "single out_sync install guard missing in applied submit.c"
[ "$(grep -c 'job->out_sync =' "$SUBC")" = 1 ] \
	|| gate_fail "submit.c must stamp out_sync on exactly one node path, got $(grep -c 'job->out_sync =' "$SUBC")"
# trace budget + the two new probe strings.
grep -q 'static unsigned int p7580_chain_trace = 8;' "$SUBC" \
	|| gate_fail "chain trace budget (8) missing in applied submit.c"
grep -q 'p7580_sched: chain jc=0x%llx nodes=%u shadow=0x%llx stride=%u' "$SUBC" \
	|| gate_fail "chain trace line missing in applied submit.c"
grep -q 'p7580_sched: split seq=%u pos=%u/%u slot=%d jc=0x%llx rc=0' "$SUBC" \
	|| gate_fail "split trace line missing in applied submit.c"
# pure-C chain surface in the applied jm_hw.h: cap/stride/rewrites.
grep -q '^#define P7580_CHAIN_MAX_JOBS[[:space:]]*16u' drivers/gpu/drm/panfrost/panfrost7580_jm_hw.h \
	|| gate_fail "P7580_CHAIN_MAX_JOBS 16 missing in applied jm_hw.h"
grep -q '^#define P7580_CHAIN_SLOT_STRIDE[[:space:]]*256u' drivers/gpu/drm/panfrost/panfrost7580_jm_hw.h \
	|| gate_fail "P7580_CHAIN_SLOT_STRIDE 256 missing in applied jm_hw.h"
grep -q '^#define P7580_CHAIN_HDR_WORDS[[:space:]]*8u' drivers/gpu/drm/panfrost/panfrost7580_jm_hw.h \
	|| gate_fail "P7580_CHAIN_HDR_WORDS 8 missing in applied jm_hw.h"
ISOL_SRC=$(awk '/^static inline void p7580_chain_hdr_isolate/,/^}/' drivers/gpu/drm/panfrost/panfrost7580_jm_hw.h)
echo "$ISOL_SRC" | grep -q 'w\[4\] = (w\[4\] & 0x0000ffffu) | ((uint32_t)(pos + 1u) << 16);' \
	|| gate_fail "isolate does not renumber job_index (pos+1) in applied jm_hw.h"
echo "$ISOL_SRC" | grep -q '^	w\[5\] = 0;$' \
	&& echo "$ISOL_SRC" | grep -q '^	w\[6\] = 0;$' \
	&& echo "$ISOL_SRC" | grep -q '^	w\[7\] = 0;$' \
	|| gate_fail "isolate must clear dep1/dep2 (w5) and both next halves (w6/w7)"
# per-node split stamps on the backend job.
grep -q 'void \*chain_shadow;' drivers/gpu/drm/panfrost/panfrost7580_job.h \
	|| gate_fail "job.h chain_shadow stamp missing in applied job.h"
grep -q 'uint32_t chain_pos;' drivers/gpu/drm/panfrost/panfrost7580_job.h \
	|| gate_fail "job.h chain_pos stamp missing in applied job.h"
grep -q 'uint32_t chain_len;' drivers/gpu/drm/panfrost/panfrost7580_job.h \
	|| gate_fail "job.h chain_len stamp missing in applied job.h"
# per-node cfg path: run_job -> hw_submit -> p7580_js_config (both flush bits).
grep -q 'p7580_job_hw_submit(&drv->jd, job);' "$SUBC" \
	|| gate_fail "run_job hw_submit call missing in applied submit.c"
grep -q 'cfg = p7580_js_config(job->as_nr);' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "hw_submit js_config call missing in applied job.c"
grep -q 'p7580_js_write(jd, slot, P7580_JS_CONFIG_NEXT, cfg);' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "hw_submit CONFIG_NEXT write missing in applied job.c"
CFG_SRC=$(awk '/^static inline uint32_t p7580_js_config/,/^}/' drivers/gpu/drm/panfrost/panfrost7580_jm_hw.h)
echo "$CFG_SRC" | grep -q 'P7580_JS_CFG_START_FLUSH_CLEAN_INV |' \
	&& echo "$CFG_SRC" | grep -q 'P7580_JS_CFG_END_FLUSH_CLEAN_INV |' \
	|| gate_fail "js_config must carry START and END FLUSH_CLEAN_INV"
grep -q '#define P7580_JS_CFG_START_FLUSH_CLEAN_INV' drivers/gpu/drm/panfrost/panfrost7580_jm_hw.h \
	|| gate_fail "START_FLUSH_CLEAN_INV define missing in applied jm_hw.h"
grep -q '#define P7580_JS_CFG_END_FLUSH_CLEAN_INV' drivers/gpu/drm/panfrost/panfrost7580_jm_hw.h \
	|| gate_fail "END_FLUSH_CLEAN_INV define missing in applied jm_hw.h"
# object: outlined helpers + inlined split fingerprint.
aarch64-linux-gnu-nm "$SUBMIT_O" | grep -q ' t p7580_drv_gpu_read$' \
	|| gate_fail "p7580_drv_gpu_read not outlined (t) in submit.o"
aarch64-linux-gnu-nm "$SUBMIT_O" | grep -q ' t p7580_drv_shadow_put$' \
	|| gate_fail "p7580_drv_shadow_put not outlined (t) in submit.o"
aarch64-linux-gnu-nm "$SUBMIT_O" | grep -E ' p7580_drv_(gpu_read|shadow_put)$' | tee -a "$EVID"
SPLIT_DIS=$(aarch64-linux-gnu-objdump -dr --disassemble=$SUBMIT_BODY "$SUBMIT_O")
[ "$(echo "$SPLIT_DIS" | grep -c 'R_AARCH64_CALL26[[:space:]]*p7580_gem_create')" = 1 ] \
	|| gate_fail "shadow allocation (p7580_gem_create) must appear once in p7580_drv_submit, got $(echo "$SPLIT_DIS" | grep -c 'R_AARCH64_CALL26[[:space:]]*p7580_gem_create')"
[ "$(echo "$SPLIT_DIS" | grep -c 'R_AARCH64_CALL26[[:space:]]*drm_sched_job_alloc')" = 1 ] \
	|| gate_fail "per-node job alloc must appear once in p7580_drv_submit (inlined loop), got $(echo "$SPLIT_DIS" | grep -c 'R_AARCH64_CALL26[[:space:]]*drm_sched_job_alloc')"
[ "$(echo "$SPLIT_DIS" | grep -c 'R_AARCH64_CALL26[[:space:]]*drm_sched_job_submit')" = 1 ] \
	|| gate_fail "per-node job submit must appear once in p7580_drv_submit, got $(echo "$SPLIT_DIS" | grep -c 'R_AARCH64_CALL26[[:space:]]*drm_sched_job_submit')"
[ "$(echo "$SPLIT_DIS" | grep -c 'bl.*<p7580_drv_gpu_read>')" = 3 ] \
	|| gate_fail "submit must read through p7580_drv_gpu_read 3 times (chain-header walk, per-node payload copy, fragment SFBD word-10 capture), got $(echo "$SPLIT_DIS" | grep -c 'bl.*<p7580_drv_gpu_read>')"
[ "$(echo "$SPLIT_DIS" | grep -c 'bl.*<p7580_drv_shadow_put>')" = 1 ] \
	|| gate_fail "submit must carry exactly 1 shadow_put (partial-queue unwind), got $(echo "$SPLIT_DIS" | grep -c 'bl.*<p7580_drv_shadow_put>')"
echo "$SPLIT_DIS" | grep -E 'p7580_gem_create|drm_sched_job_(alloc|submit)|p7580_drv_(gpu_read|shadow_put)' | tee -a "$EVID"
SPUT_DIS=$(aarch64-linux-gnu-objdump -dr --disassemble=p7580_drv_shadow_put "$SUBMIT_O")
[ "$(echo "$SPUT_DIS" | grep -c 'bl.*<p7580_gem_gpu_unmap')" = 1 ] \
	|| gate_fail "shadow_put must unmap on the last reference (1 gpu_unmap call)"
echo "$SPUT_DIS" | grep -q '<kfree>' \
	|| gate_fail "shadow_put does not free its bookkeeping struct"
echo "$SPUT_DIS" | grep -q 'drm_gem_object_free' \
	|| gate_fail "shadow_put does not drop the shadow object (no GEM free path)"
echo "$SPUT_DIS" | grep -E 'gpu_unmap|kfree|drm_gem_object' | tee -a "$EVID"
# free_job releases the node's shadow reference.
[ "$(aarch64-linux-gnu-objdump -dr --disassemble=p7580_drv_free_job "$SUBMIT_O" | grep -c 'bl.*<p7580_drv_shadow_put>')" = 1 ] \
	|| gate_fail "free_job must release the node's shadow reference exactly once"
# per-node cfg: one hw_submit per run_job, cfg folded to 0x83700 (as 0)
# in job.o (mov #0x3700 + movk #0x8 -> orr with as_nr).
[ "$(aarch64-linux-gnu-objdump -dr --disassemble=p7580_drv_run_job "$SUBMIT_O" | grep -c 'R_AARCH64_CALL26[[:space:]]*p7580_job_hw_submit')" = 1 ] \
	|| gate_fail "run_job must call p7580_job_hw_submit exactly once (one START per node)"
HW_DIS=$(aarch64-linux-gnu-objdump -d --disassemble=p7580_job_hw_submit "$JOB_O")
echo "$HW_DIS" | grep -q 'mov[[:space:]]*w[0-9]*, #0x3700' \
	|| gate_fail "hw_submit cfg low half (#0x3700 = flush/MMU bits) missing in job.o"
echo "$HW_DIS" | grep -q 'movk[[:space:]]*w[0-9]*, #0x8, lsl #16' \
	|| gate_fail "hw_submit cfg high half (#0x80000 = THREAD_PRI(8)) missing in job.o"
echo "$HW_DIS" | grep -q '[[:space:]]orr[[:space:]]' \
	|| gate_fail "hw_submit does not OR as_nr into the cfg in job.o"
echo "$HW_DIS" | grep -E '#0x3700|#0x8, lsl #16|orr' | tee -a "$EVID"
# compiled/trace evidence: strings in submit.o and the linked Image.
for s in 'p7580_sched: chain jc=0x%llx nodes=%u shadow=0x%llx stride=%u' \
	 'p7580_sched: split seq=%u pos=%u/%u slot=%d jc=0x%llx rc=0'; do
	aarch64-linux-gnu-strings "$SUBMIT_O" | grep -qF "$s" \
		|| gate_fail "chain probe string missing from submit.o: $s"
	aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -qF "$s" \
		|| gate_fail "chain probe string missing from Image: $s"
done
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -E 'p7580_sched: (chain|split)' | tee -a "$EVID"
[ "$(grep -c ' T p7580_drv_submit$' System.map)" = 1 ] \
	|| gate_fail "System.map p7580_drv_submit count != 1"
grep -E ' (p7580_drv_submit|p7580_drv_run_job)$' System.map | tee -a "$EVID"
# host model: the REAL jm_hw chain helpers + the split scheduler model.
HT=$EXT/hosttest-int49
rm -rf "$HT"
mkdir -p "$HT/driver" "$HT/jobs"
cp "$REPO/code/gpu31/driver/Makefile" "$REPO/code/gpu31/driver/chainsplit_test.c" \
   "$REPO/code/gpu31/driver/submit_fix_test.c" "$HT/driver/" \
	|| gate_fail "could not stage the host tests"
cp "$REPO/code/gpu31/jobs/panfrost7580_jm_hw.h" "$HT/jobs/" \
	|| gate_fail "could not stage the host jm_hw.h"
( cd "$HT/driver" && make check ) > "$EXT/logs/gpu31-int49-hosttest.log" 2>&1 \
	|| gate_fail "host tests failed (see $EXT/logs/gpu31-int49-hosttest.log)"
grep -q 'chainsplit_test: all checks passed' "$EXT/logs/gpu31-int49-hosttest.log" \
	|| gate_fail "chainsplit_test did not pass"
grep -q 'submit_fix_test: all checks passed' "$EXT/logs/gpu31-int49-hosttest.log" \
	|| gate_fail "submit_fix_test did not pass"
cat "$EXT/logs/gpu31-int49-hosttest.log" >> "$EVID"
rm -rf "$HT"
note "== int49 chainsplit gate: PASS (walk+isolate+shadow, per-node queue on one slot with prev-fence order, single out_sync on the last node, one-node pass-through, per-node BO/PM refs + unwind, per-node cfg 0x83700, host chainsplit_test) =="

# 6x. defectb gates (9dd7542 gpu31-defectb: clean+invalidate each SHMEM
# page at allocation). Source order inside the per-page loop (page get ->
# flush -> pages[i] = page), then the compiled proof: exactly one
# __flush_dcache_area call reloc in gem.o, in the symbol that carries the
# single shmem page-get call (shmem_read_mapping_page is the inline that
# emits shmem_read_mapping_page_gfp), and the pages[i] = page store
# immediately follows the flush. -O2 lays the loop out as
# head(page get) -> success edge -> flush block -> store -> i++ -> head,
# so the flush can sit at a LOWER address than the read: the gate proves
# the pair is the loop body (adjacent call sites) and the store follows
# the flush, not that addresses ascend. Then U ref in the object, T def in
# System.map, and the same single call site + store in the linked vmlinux
# (not only in the unlinked object). FAIL (exit 1, scratch kept) on any
# disagreement.
note "== int49 gate: defectb shmem flush present =="
[ "$(grep -c '__flush_dcache_area(page_address(page), PAGE_SIZE);' "$GEMC")" = 1 ] \
	|| gate_fail "applied gem.c must call __flush_dcache_area(page_address(page), PAGE_SIZE) exactly once, got $(grep -c '__flush_dcache_area(page_address(page), PAGE_SIZE);' "$GEMC")"
SI_READ=$(grep -n 'struct page \*page = shmem_read_mapping_page(' "$GEMC" | cut -d: -f1 | head -1)
SI_FLUSH=$(grep -n '__flush_dcache_area(page_address(page), PAGE_SIZE);' "$GEMC" | cut -d: -f1 | head -1)
SI_STORE=$(grep -n 'pages\[i\] = page;' "$GEMC" | cut -d: -f1 | head -1)
[ -n "$SI_READ" ] && [ -n "$SI_FLUSH" ] && [ -n "$SI_STORE" ] \
	|| gate_fail "shmem alloc page-get/flush/page-store lines missing in applied gem.c"
[ "$SI_READ" -lt "$SI_FLUSH" ] && [ "$SI_FLUSH" -lt "$SI_STORE" ] \
	|| gate_fail "applied gem.c order is not page-get($SI_READ) -> flush($SI_FLUSH) -> store($SI_STORE)"
[ $((SI_STORE - SI_READ)) -lt 48 ] \
	|| gate_fail "flush is not in the per-page loop body (page-get/flush/store span $((SI_STORE - SI_READ)) lines)"
note "applied gem.c shmem loop: page-get=$SI_READ flush=$SI_FLUSH store=$SI_STORE"
# driver 0001 mirror: the patch carries the same single line.
[ "$(grep -c '^+[[:space:]]*__flush_dcache_area(page_address(page), PAGE_SIZE);$' "$DRV1")" = 1 ] \
	|| gate_fail "driver 0001 must carry exactly one +__flush_dcache_area line, got $(grep -c '^+[[:space:]]*__flush_dcache_area(page_address(page), PAGE_SIZE);$' "$DRV1")"
note "driver 0001 mirror: $(grep -c '^+[[:space:]]*__flush_dcache_area(page_address(page), PAGE_SIZE);$' "$DRV1") flush line"
# object: one call reloc, in the shmem page-get symbol, and the store to
# the pages array immediately after the flush (the loop body).
aarch64-linux-gnu-nm "$GEM_O" | grep -q ' U __flush_dcache_area$' \
	|| gate_fail "gem.o does not reference __flush_dcache_area (arm64 flusher not linked in)"
[ "$(aarch64-linux-gnu-objdump -dr "$GEM_O" | grep -c 'R_AARCH64_CALL26[[:space:]]*__flush_dcache_area')" = 1 ] \
	|| gate_fail "gem.o must carry exactly 1 __flush_dcache_area call reloc, got $(aarch64-linux-gnu-objdump -dr "$GEM_O" | grep -c 'R_AARCH64_CALL26[[:space:]]*__flush_dcache_area')"
GEMDIS=$(aarch64-linux-gnu-objdump -dr "$GEM_O")
SHMFUNC=$(printf '%s\n' "$GEMDIS" | awk '
	/^[0-9a-f]+ <.*>:$/ { f=$2 }
	/R_AARCH64_CALL26[[:space:]]*shmem_read_mapping_page/ { sub(/^</,"",f); sub(/>:$/,"",f); print f; exit }')
[ -n "$SHMFUNC" ] || gate_fail "no shmem page-get call site in gem.o"
FLUSH_FUNC=$(printf '%s\n' "$GEMDIS" | awk '
	/^[0-9a-f]+ <.*>:$/ { f=$2 }
	/R_AARCH64_CALL26[[:space:]]*__flush_dcache_area/ { sub(/^</,"",f); sub(/>:$/,"",f); print f; exit }')
[ "$SHMFUNC" = "$FLUSH_FUNC" ] \
	|| gate_fail "flush is not in the shmem page-get symbol ($FLUSH_FUNC != $SHMFUNC)"
FDIS=$(printf '%s\n' "$GEMDIS" | awk -v h="<$SHMFUNC>:" '
	/^[0-9a-f]+ </ { if (p) exit; if ($2==h) p=1 }
	p { print }')
[ "$(printf '%s\n' "$FDIS" | grep -c 'R_AARCH64_CALL26[[:space:]]*shmem_read_mapping_page')" = 1 ] \
	|| gate_fail "$SHMFUNC must carry exactly 1 shmem page-get call"
FO_READ=$(printf '%s\n' "$FDIS" | grep -n 'R_AARCH64_CALL26[[:space:]]*shmem_read_mapping_page' | cut -d: -f1)
FO_FLUSH=$(printf '%s\n' "$FDIS" | grep -n 'R_AARCH64_CALL26[[:space:]]*__flush_dcache_area' | cut -d: -f1)
[ -n "$FO_FLUSH" ] || gate_fail "no flush reloc in $SHMFUNC"
FO_D=$((FO_FLUSH - FO_READ)); FO_AD=${FO_D#-}
[ "$FO_AD" -le 12 ] \
	|| gate_fail "flush is not adjacent to the page get in $SHMFUNC (page-get line $FO_READ, flush line $FO_FLUSH)"
printf '%s\n' "$FDIS" | sed -n "$((FO_FLUSH-1)),$((FO_FLUSH+6))p" | tee -a "$EVID"
printf '%s\n' "$FDIS" | sed -n "$((FO_FLUSH+1)),$((FO_FLUSH+6))p" \
	| grep -q 'str.*\[x[0-9]*, x[0-9]*, lsl #3\]' \
	|| gate_fail "flush is not immediately followed by the pages[i] = page store in $SHMFUNC"
note "object: $SHMFUNC page-get reloc line $FO_READ, flush reloc line $FO_FLUSH, pages[i] store follows the flush"
# linked kernel: the flusher is defined once and the same call site
# survives the link (vmlinux is the Image's source).
[ "$(grep -c ' [Tt] __flush_dcache_area$' System.map)" = 1 ] \
	|| gate_fail "System.map __flush_dcache_area definition count != 1"
grep ' __flush_dcache_area$' System.map | tee -a "$EVID"
VMLF=""
for cand in $(aarch64-linux-gnu-nm vmlinux | awk -v s="$SHMFUNC" '$2 ~ /[tT]/ && index($3, s) == 1 { print $3 }'); do
	if aarch64-linux-gnu-objdump -d --disassemble="$cand" vmlinux \
		| grep -q 'bl.*<shmem_read_mapping_page'; then
		VMLF=$cand
		break
	fi
done
[ -n "$VMLF" ] || gate_fail "linked vmlinux has no $SHMFUNC symbol carrying the shmem page get"
VDIS=$(aarch64-linux-gnu-objdump -d --disassemble="$VMLF" vmlinux)
[ "$(printf '%s\n' "$VDIS" | grep -c 'bl.*<__flush_dcache_area>')" = 1 ] \
	|| gate_fail "linked vmlinux $VMLF must call __flush_dcache_area exactly once, got $(printf '%s\n' "$VDIS" | grep -c 'bl.*<__flush_dcache_area>')"
[ "$(printf '%s\n' "$VDIS" | grep -c 'bl.*<shmem_read_mapping_page')" = 1 ] \
	|| gate_fail "linked vmlinux $VMLF must carry exactly 1 shmem page-get call"
VR=$(printf '%s\n' "$VDIS" | grep -n 'bl.*<shmem_read_mapping_page' | cut -d: -f1)
VF=$(printf '%s\n' "$VDIS" | grep -n 'bl.*<__flush_dcache_area>' | cut -d: -f1)
[ -n "$VR" ] && [ -n "$VF" ] || gate_fail "linked vmlinux $VMLF lost a call site (page-get=$VR flush=$VF)"
VD=$((VF - VR)); VAD=${VD#-}
[ "$VAD" -le 12 ] \
	|| gate_fail "linked vmlinux flush is not adjacent to the page get in $VMLF (page-get line $VR, flush line $VF)"
printf '%s\n' "$VDIS" | sed -n "$((VF-1)),$((VF+6))p" | tee -a "$EVID"
printf '%s\n' "$VDIS" | sed -n "$((VF+1)),$((VF+6))p" \
	| grep -q 'str.*\[x[0-9]*, x[0-9]*, lsl #3\]' \
	|| gate_fail "linked vmlinux flush is not immediately followed by the pages[i] = page store in $VMLF"
note "linked vmlinux: $VMLF page-get line $VR, flush line $VF, pages[i] store follows the flush"
note "== int49 defectb gate: PASS (each shmem page get is paired with __flush_dcache_area(page_address(page), PAGE_SIZE) inside the per-page loop and the pages[i] = page store follows it, in tree, gem.o and linked vmlinux; driver 0001 mirror) =="

# 6y. chainsplit length pin (742dc52): the VERTEX (COMPUTE_JOB)
# aggregate is MALI_COMPUTE_JOB_LENGTH 192 B (32 header + Invocation 8 +
# Parameters 8 + Draw 128 + pad 16), so a shadow slot must copy the whole
# Draw block; the generated lengths are WRITE_VALUE 56, TILER/FUSED 192,
# FRAGMENT 64, NULL 32. int37's 96 B truncated Draw at Textures onward ->
# the standalone vertex node read zeros and faulted. Host T1 pins the same
# numbers; this gate pins the applied tree the image is built from.
note "== int49 gate: chainsplit job lengths present =="
JOBH=drivers/gpu/drm/panfrost/panfrost7580_jm_hw.h
JBF=$(awk '/^static inline unsigned int p7580_chain_job_bytes/,/^}/' "$JOBH")
[ -n "$JBF" ] || gate_fail "p7580_chain_job_bytes missing in applied jm_hw.h"
printf '%s\n' "$JBF" | grep -q 'case P7580_JOB_TYPE_VERTEX:' \
	&& grep -q 'MALI_COMPUTE_JOB_LENGTH 192' "$JOBH" \
	|| gate_fail "jm_hw.h does not document the 192 B COMPUTE_JOB aggregate (742dc52)"
printf '%s\n' "$JBF" | grep -A2 'case P7580_JOB_TYPE_WRITE_VALUE:' | grep -q 'return 56u;' \
	|| gate_fail "WRITE_VALUE shadow length is not 56u in applied jm_hw.h"
printf '%s\n' "$JBF" | grep -A3 'case P7580_JOB_TYPE_VERTEX:' | grep -q 'return 192u;' \
	|| gate_fail "VERTEX shadow length is not 192u (TILER/FUSED group) in applied jm_hw.h"
printf '%s\n' "$JBF" | grep -A5 'case P7580_JOB_TYPE_FRAGMENT:' | grep -q 'return 64u;' \
	|| gate_fail "FRAGMENT shadow length is not 64u in applied jm_hw.h"
printf '%s\n' "$JBF" | grep -q 'return 96u;' \
	&& gate_fail "stale 96u chain job length still in applied jm_hw.h"
printf '%s\n' "$JBF" | grep -E 'MALI_COMPUTE_JOB_LENGTH|return (56|64|192)u;|P7580_JOB_DESC_HDR_SIZE;' | tee -a "$EVID"
note "== int49 chainsplit-length gate: PASS (VERTEX/TILER/FUSED 192, WRITE_VALUE 56, FRAGMENT 64, NULL 32 B; 96u gone) =="

# 6z. fault-latency fix (gpu31-latency): the IRQ ERR-without-DONE leg
# used to leave the faulted job's fence unsignalled until the full sched
# timeout (timedout_job -> recover). It now kicks timeout_work at delay
# 0 through the 0004 shim, on the same ordered wq, so recovery signals
# the fence in ms while the per-job timeout work stays as the backstop
# (its budget is entity->timeout, i.e. the 750 ms seed since int49). Pin
# the applied tree, both objects, the linked Image and the patch mirrors.
note "== int49 gate: fault-latency kick present =="
JB=drivers/gpu/drm/panfrost/panfrost7580_job.c
SUB=drivers/gpu/drm/panfrost/panfrost7580_submit.c
DS=drivers/gpu/drm/drm_sched.c
JOB_O=drivers/gpu/drm/panfrost/panfrost7580_job.o
grep -q 'job->sched_job = sched_job;' "$SUB" \
	|| gate_fail "submit.c does not stamp the sched-job backpointer"
[ "$(grep -c 'job->sched_job = sched_job;' "$SUB")" = 1 ] \
	|| gate_fail "submit.c sched_job stamp count != 1"
[ "$(grep -c 'drm_sched_job_kick_timeout(' "$JB")" = 1 ] \
	|| gate_fail "job.c must call drm_sched_job_kick_timeout exactly once, got $(grep -c 'drm_sched_job_kick_timeout(' "$JB")"
awk '/^int p7580_job_irq\(/,/^}/' "$JB" | grep -q 'drm_sched_job_kick_timeout(' \
	|| gate_fail "fault kick is not inside p7580_job_irq"
grep -q 'mod_delayed_work(job->entity->wq, &job->timeout_work, 0);' "$DS" \
	|| gate_fail "drm_sched.c kick does not re-arm timeout_work at delay 0"
[ "$(grep -c 'queue_delayed_work(entity->wq, &job->timeout_work,' "$DS")" = 1 ] \
	|| gate_fail "per-job timeout backstop was removed/changed in drm_sched.c"
grep -q 'if (last_fence) {' "$SUB" \
	|| gate_fail "submit.c BO publish is not guarded by last_fence"
grep -q 'job->publish_excl' "$SUB" \
	&& gate_fail "submit.c still carries the run_job publish_excl scope (storm cause)"
[ "$(grep -c 'dma_resv_add_excl_fence(' "$SUB")" = 1 ] \
	|| gate_fail "submit.c must carry exactly 1 add_excl site (SUBMIT serial publish only), got $(grep -c 'dma_resv_add_excl_fence(' "$SUB")"
QUEUED=$(grep -n 'last_fence = fence;' "$SUB" | cut -d: -f1)
STAMP=$(grep -n 'gpu31-latency (MFBD hand-off)' "$SUB" | cut -d: -f1)
ERRSP=$(grep -n '^err_split:' "$SUB" | cut -d: -f1)
[ -n "$QUEUED" ] && [ -n "$STAMP" ] && [ -n "$ERRSP" ] && \
	[ "$QUEUED" -lt "$STAMP" ] && [ "$STAMP" -lt "$ERRSP" ] \
	|| gate_fail "SUBMIT-time excl stamp is not between the queue loop and err_split (queued=$QUEUED stamp=$STAMP err=$ERRSP)"
grep -q '^+.*gpu31-latency (MFBD hand-off)' "$DRV1" \
	|| gate_fail "driver 0001 mirror lost the SUBMIT-time excl stamp"
[ "$(grep -c '^+.*dma_resv_add_excl_fence(' "$DRV1")" = 1 ] \
	|| gate_fail "driver 0001 mirror add_excl count != 1"
[ "$(aarch64-linux-gnu-objdump -d --disassemble=$SUBMIT_BODY "$SUBMIT_O" | grep -c 'bl.*<dma_resv_add_excl_fence>')" = 1 ] \
	|| gate_fail "p7580_drv_submit does not carry exactly one add_excl call (SUBMIT stamp)"
grep -q 'void drm_sched_job_kick_timeout(struct drm_sched_job \*job);' drivers/gpu/drm/drm_sched.h \
	|| gate_fail "drm_sched.h does not declare drm_sched_job_kick_timeout"
aarch64-linux-gnu-nm "$SCHED_O" | grep -q ' T drm_sched_job_kick_timeout$' \
	|| gate_fail "drm_sched.o has no drm_sched_job_kick_timeout definition"
[ "$(aarch64-linux-gnu-objdump -dr "$JOB_O" | grep -c 'R_AARCH64_CALL26[[:space:]]*drm_sched_job_kick_timeout')" = 1 ] \
	|| gate_fail "job.o must carry exactly 1 drm_sched_job_kick_timeout call reloc, got $(aarch64-linux-gnu-objdump -dr "$JOB_O" | grep -c 'R_AARCH64_CALL26[[:space:]]*drm_sched_job_kick_timeout')"
grep -q ' T drm_sched_job_kick_timeout$' System.map \
	|| gate_fail "linked System.map has no drm_sched_job_kick_timeout"
[ "$(grep -c '^+.*drm_sched_job_kick_timeout(' "$DRV1")" = 1 ] \
	|| gate_fail "driver 0001 mirror lost the fault kick call"
grep -q '^+	void \*sched_job;' "$DRV1" \
	|| gate_fail "driver 0001 mirror lost the sched_job backpointer"
grep -q '^+void drm_sched_job_kick_timeout(struct drm_sched_job \*job)' \
	"$REPO/patches/drm-backport/0004-drm_sched-panfrost-job-only-shim.patch" \
	|| gate_fail "0004 mirror lost drm_sched_job_kick_timeout"
note "applied: submit stamp + IRQ kick + delay-0 mod_delayed_work + per-job timeout backstop"
# Host model: the shim-side contract for the kick (407 checks incl. the
# new test_fault_kick_timeout), plus the slice structural gates.
( cd "$REPO/code/gpu31/sched" && make check ) > "$EXT/logs/gpu31-int49-schedhost.log" 2>&1 \
	|| gate_fail "sched host tests failed (see $EXT/logs/gpu31-int49-schedhost.log)"
[ "$(grep -c 'checks, 0 failures' "$EXT/logs/gpu31-int49-schedhost.log")" = 2 ] \
	|| gate_fail "sched host test did not report 0 failures for native AND qemu-aarch64"
grep -q '^gates: all PASS$' "$EXT/logs/gpu31-int49-schedhost.log" \
	|| gate_fail "sched structural gates did not pass"
grep -q 'kcheck: 4 files syntax-only + object compile PASS' "$EXT/logs/gpu31-int49-schedhost.log" \
	|| gate_fail "sched kcheck did not pass"
cat "$EXT/logs/gpu31-int49-schedhost.log" >> "$EVID"

note "== int49 fault-latency gate: PASS (sched_job stamp, single IRQ kick, delay-0 timeout_work re-arm on the entity wq, per-job timeout backstop intact (budget = entity->timeout, 750 ms seed), symbol in job.o/drm_sched.o/System.map, both patch mirrors) =="

# 6aa. gpu31-readback power-idle + power-cycle recovery gates (161993d),
# retargeted for gpu31-firstfault: recover() no longer decides per status.
# Three invariants, each pinned in the applied tree, the compiled objects
# and the linked Image:
#   (1) a busy core fails the power-off closed: -EBUSY with the cores
#       powered back on, propagated by runtime_suspend, and refused
#       before any clock/domain change while !p7580_job_is_idle();
#   (2) is_idle() consults JS_STATUS ACTIVE (bit 0) for present slots,
#       not our own STOPPED (0x03) result;
#   (3) pf7580_power_cycle() exists and EVERY recovery ends in it when
#       the hook is wired: do_cycle is the hook check alone, the bare
#       soft reset survives only as the unwired fallback (int40 cycled
#       only on 0x41 / a surviving latch, which left the re-latch in
#       place), then AS0 +
#       JOB_INT re-arm, with the recover-done telemetry always emitted.
note "== int49 gate: power-idle + power-cycle recovery present =="
POWC=drivers/gpu/drm/panfrost/panfrost7580_power.c
POWH=drivers/gpu/drm/panfrost/panfrost7580_power.h
MAINC=drivers/gpu/drm/panfrost/panfrost7580_main.c
JOBC=drivers/gpu/drm/panfrost/panfrost7580_job.c
JOBH=drivers/gpu/drm/panfrost/panfrost7580_job.h
JMH=drivers/gpu/drm/panfrost/panfrost7580_jm_hw.h
POW_O=drivers/gpu/drm/panfrost/panfrost7580_power.o
MAIN_O=drivers/gpu/drm/panfrost/panfrost7580_main.o
for f in "$POWC" "$POWH" "$MAINC" "$JOBC" "$JOBH" "$JMH" "$POW_O" "$MAIN_O"; do
	[ -f "$f" ] || gate_fail "$f missing"
done
note "applied source anchors (file:line):"
grep -n '^static int pf7580_cores_off_locked\|^busy:\|^int pf7580_power_cycle\|^int pf7580_runtime_suspend\|^int p7580_job_is_idle\|^int p7580_job_slots_latched_fault\|^int p7580_job_recover\|recover-done slot=\|p7580_job_slots_latched_fault(&drv->jd)\|p7580_mmu_program_as' \
	"$POWC" "$JOBC" "$MAINC" "$JMH" | tee -a "$EVID"
grep -n 'return -EBUSY;' "$POWC" "$MAINC" | tee -a "$EVID"
# (1a) cores-off busy label: exactly one -EBUSY in power.c, reached from
# pf7580_cores_off_locked, and the three cores are powered back on first.
[ "$(grep -c 'return -EBUSY;' "$POWC")" = 1 ] \
	|| gate_fail "power.c must return -EBUSY exactly once (cores-off busy guard), got $(grep -c 'return -EBUSY;' "$POWC")"
COFF=$(awk '/^static int pf7580_cores_off_locked/,/^}/' "$POWC")
[ -n "$COFF" ] || gate_fail "pf7580_cores_off_locked missing in applied power.c"
printf '%s\n' "$COFF" | grep -q '^busy:' \
	|| gate_fail "cores-off busy label missing"
BUSB=$(printf '%s\n' "$COFF" | sed -n '/^busy:/,$p' | grep -v '^}')
[ "$(printf '%s\n' "$BUSB" | grep -c 'PWRON_LO);')" = 3 ] \
	|| gate_fail "cores-off busy path must power all 3 cores back on (PWRON_LO writes), got $(printf '%s\n' "$BUSB" | grep -c 'PWRON_LO);')"
printf '%s\n' "$BUSB" | awk 'NF{last=$0} END{exit(last ~ /return -EBUSY;/ ? 0 : 1)}' \
	|| gate_fail "cores-off busy path must end in return -EBUSY (fail closed)"
printf '%s\n' "$BUSB" | tee -a "$EVID"
# (1b) runtime_suspend propagates it BEFORE gating the clock.
RS=$(awk '/^int pf7580_runtime_suspend/,/^}/' "$POWC")
[ -n "$RS" ] || gate_fail "pf7580_runtime_suspend missing in applied power.c"
printf '%s\n' "$RS" | grep -q 'ret = pf7580_cores_off_locked(p);' \
	|| gate_fail "runtime_suspend does not call cores_off"
printf '%s\n' "$RS" | grep -q 'if (ret) {' \
	|| gate_fail "runtime_suspend does not test the cores-off result"
RS_RET=$(printf '%s\n' "$RS" | grep -n '^[[:space:]]*return ret;' | cut -d: -f1 | head -1)
RS_CLK=$(printf '%s\n' "$RS" | grep -n 'pf7580_clk_off_locked(p);' | cut -d: -f1 | head -1)
[ -n "$RS_RET" ] && [ -n "$RS_CLK" ] && [ "$RS_RET" -lt "$RS_CLK" ] \
	|| gate_fail "runtime_suspend does not return -EBUSY before clk_off (ret=$RS_RET clk=$RS_CLK)"
printf '%s\n' "$RS" | grep -E 'cores_off_locked|if \(ret\)|return ret|clk_off_locked' | tee -a "$EVID"
# (1c) driver suspend refuses up front while the JM is not idle, before
# dvfs/power are touched; the power error is propagated.
DRS=$(awk '/^static int p7580_drv_runtime_suspend/,/^}/' "$MAINC")
[ -n "$DRS" ] || gate_fail "p7580_drv_runtime_suspend missing in applied main.c"
printf '%s\n' "$DRS" | grep -q 'if (drv && !p7580_job_is_idle(&drv->jd))' \
	|| gate_fail "drv_runtime_suspend does not consult p7580_job_is_idle"
printf '%s\n' "$DRS" | grep -A1 'if (drv && !p7580_job_is_idle(&drv->jd))' | grep -q 'return -EBUSY;' \
	|| gate_fail "drv_runtime_suspend does not refuse with -EBUSY while busy"
printf '%s\n' "$DRS" | grep -q 'return pf7580_runtime_suspend(drv->power);' \
	|| gate_fail "drv_runtime_suspend does not propagate the power-slice result"
DRS_I=$(printf '%s\n' "$DRS" | grep -n 'p7580_job_is_idle' | cut -d: -f1 | head -1)
DRS_P=$(printf '%s\n' "$DRS" | grep -n 'pf7580_runtime_suspend(drv->power)' | cut -d: -f1 | head -1)
[ -n "$DRS_I" ] && [ -n "$DRS_P" ] && [ "$DRS_I" -lt "$DRS_P" ] \
	|| gate_fail "idle guard is not before the suspend call (idle=$DRS_I susp=$DRS_P)"
printf '%s\n' "$DRS" | grep -E 'is_idle|return -EBUSY|pf7580_runtime_suspend' | tee -a "$EVID"
# (2) is_idle: JS_STATUS ACTIVE bit for present slots, with our own stop
# result exempt; in_flight and COMMAND_NEXT stay.
IDLEB=$(awk '/^int p7580_job_is_idle/,/^}/' "$JOBC")
[ -n "$IDLEB" ] || gate_fail "p7580_job_is_idle missing in applied job.c"
printf '%s\n' "$IDLEB" | grep -q 'if (p7580_js_read(jd, i, P7580_JS_COMMAND_NEXT))' \
	|| gate_fail "is_idle lost the COMMAND_NEXT check"
printf '%s\n' "$IDLEB" | grep -q 'if (jd->in_flight\[i\])' \
	|| gate_fail "is_idle lost the in_flight check"
printf '%s\n' "$IDLEB" | grep -q 'st = p7580_js_read(jd, i, P7580_JS_STATUS);' \
	|| gate_fail "is_idle does not read JS_STATUS"
printf '%s\n' "$IDLEB" | grep -q '(st & 0x1u) && !p7580_js_status_is_stop(st)' \
	|| gate_fail "is_idle does not treat a non-stop ACTIVE slot as busy"
IDLE_ST=$(printf '%s\n' "$IDLEB" | grep -n 'js_status_is_stop(st)' | cut -d: -f1 | head -1)
IDLE_R1=$(printf '%s\n' "$IDLEB" | grep -n '^[[:space:]]*return 1;' | cut -d: -f1 | tail -1)
[ -n "$IDLE_ST" ] && [ -n "$IDLE_R1" ] && [ "$IDLE_ST" -lt "$IDLE_R1" ] \
	|| gate_fail "JS_STATUS busy test is not on the idle decision path (st=$IDLE_ST ret1=$IDLE_R1)"
grep -qE '#define P7580_JS_STATUS_POWER_FAULT[[:space:]]+0x41u' "$JMH" \
	|| gate_fail "P7580_JS_STATUS_POWER_FAULT 0x41 missing in applied job.c"
STOPB=$(awk '/^static inline int p7580_js_status_is_stop/,/^}/' "$JMH")
for s in 0x02u 0x03u 0x04u; do
	printf '%s\n' "$STOPB" | grep -q "$s" \
		|| gate_fail "js_status_is_stop does not cover $s"
done
printf '%s\n' "$IDLEB" | grep -E 'COMMAND_NEXT|in_flight|JS_STATUS|st & 0x1u|return' | tee -a "$EVID"
# (3a) pf7580_power_cycle: one definition, declared, full off->on sequence.
[ "$(grep -c '^int pf7580_power_cycle(struct pf7580_power \*p)$' "$POWC")" = 1 ] \
	|| gate_fail "pf7580_power_cycle definition count != 1"
grep -q '^int pf7580_power_cycle(struct pf7580_power \*p);' "$POWH" \
	|| gate_fail "pf7580_power_cycle declaration missing in applied power.h"
PCB=$(awk '/^int pf7580_power_cycle/,/^}/' "$POWC")
for c in 'pf7580_cores_off_locked(p)' 'pf7580_clk_off_locked(p);' 'pf7580_power_on_locked(p)' 'return ret;'; do
	printf '%s\n' "$PCB" | grep -qF "$c" \
		|| gate_fail "pf7580_power_cycle lost '$c'"
done
printf '%s\n' "$PCB" | grep -E 'cores_off_locked|clk_off_locked|power_on_locked|return ret' | tee -a "$EVID"
# (3b) power_cycle hook wired into the jobs slice and the driver.
grep -q 'int (\*power_cycle)(void \*dev);' "$JOBH" \
	|| gate_fail "job.h lost the power_cycle hook field"
grep -q 'int (\*power_cycle)(void \*dev))' "$JOBH" \
	|| gate_fail "job.h dev_init lost the power_cycle parameter"
PCV=$(awk '/^static int p7580_job_power_cycle_void/,/^}/' "$MAINC")
[ -n "$PCV" ] || gate_fail "p7580_job_power_cycle_void missing in applied main.c"
printf '%s\n' "$PCV" | grep -q 'return pf7580_power_cycle(drv->power);' \
	|| gate_fail "power_cycle wrapper does not call pf7580_power_cycle(drv->power)"
printf '%s\n' "$PCV" | tee -a "$EVID"
# (3c) recovery: cycle decision, then AS0 + JOB_INT re-arm, then the
# always-on recover-done line.
LATCHB=$(awk '/^int p7580_job_slots_latched_fault/,/^}/' "$JOBC")
[ -n "$LATCHB" ] || gate_fail "p7580_job_slots_latched_fault missing in applied job.c"
printf '%s\n' "$LATCHB" | grep -q 'p7580_js_status_is_fault(st)' \
	|| gate_fail "latched-fault scan lost the fault test"
printf '%s\n' "$LATCHB" | grep -q '!p7580_js_status_is_stop(st)' \
	|| gate_fail "latched-fault scan does not exempt our own stop codes"
printf '%s\n' "$LATCHB" | grep -q 'jd->js_present & (1u << i)' \
	|| gate_fail "latched-fault scan does not restrict itself to present slots"
RECB=$(awk '/^int p7580_job_recover/,/^}/' "$JOBC")
[ -n "$RECB" ] || gate_fail "p7580_job_recover missing in applied job.c"
printf '%s\n' "$RECB" | grep -q 'do_cycle = (jd->power_cycle && jd->pm_dev) ? 1 : 0;' \
	|| gate_fail "recover does not decide the power cycle unconditionally from the hook"
printf '%s\n' "$RECB" | grep -q 'status == P7580_JS_STATUS_POWER_FAULT' \
	&& gate_fail "recover still gates the power cycle on a POWER_FAULT status (gpu31-firstfault: every recovery must end in a full power-on, incl. the silent 0x08)"
printf '%s\n' "$RECB" | grep -q 'p7580_job_slots_latched_fault(jd)' \
	&& gate_fail "recover still gates the power cycle on a latched-fault rescan (the rescan is resume-only now)"
printf '%s\n' "$RECB" | grep -q 'if (!do_cycle) {' \
	|| gate_fail "recover lost the bare-reset fallback for an unwired hook"
printf '%s\n' "$RECB" | grep -q 'jd->power_cycle(jd->pm_dev)' \
	|| gate_fail "recover does not invoke the power_cycle hook"
printf '%s\n' "$RECB" | grep -q 'cycled = 1;' \
	|| gate_fail "recover does not stamp the cycle for recover-done"
REC_DC=$(printf '%s\n' "$RECB" | grep -n 'do_cycle = (jd->power_cycle' | cut -d: -f1 | head -1)
REC_FB=$(printf '%s\n' "$RECB" | grep -n 'if (!do_cycle) {' | cut -d: -f1 | head -1)
REC_CB=$(printf '%s\n' "$RECB" | grep -n 'if (do_cycle) {' | cut -d: -f1 | head -1)
[ -n "$REC_DC" ] && [ -n "$REC_FB" ] && [ -n "$REC_CB" ] && \
	[ "$REC_DC" -lt "$REC_FB" ] && [ "$REC_FB" -lt "$REC_CB" ] \
	|| gate_fail "recover cycle decision order is not decide($REC_DC) -> bare-reset fallback($REC_FB) -> power-on($REC_CB)"
REC_CYC=$(printf '%s\n' "$RECB" | grep -n 'jd->power_cycle(jd->pm_dev)' | cut -d: -f1 | head -1)
REC_AS=$(printf '%s\n' "$RECB" | grep -n 'p7580_mmu_program_as(' | cut -d: -f1 | head -1)
REC_RM=$(printf '%s\n' "$RECB" | grep -n 'p7580_job_dev_rearm(jd)' | cut -d: -f1 | head -1)
REC_DONE=$(printf '%s\n' "$RECB" | grep -n 'recover-done slot=' | cut -d: -f1 | head -1)
[ -n "$REC_CYC" ] && [ -n "$REC_AS" ] && [ -n "$REC_RM" ] && [ -n "$REC_DONE" ] && \
	[ "$REC_CYC" -lt "$REC_AS" ] && [ "$REC_AS" -lt "$REC_RM" ] && [ "$REC_RM" -lt "$REC_DONE" ] \
	|| gate_fail "recover order is not cycle($REC_CYC) -> AS0($REC_AS) -> rearm($REC_RM) -> recover-done($REC_DONE)"
grep -qF 'p7580_job: recover-done slot=%d latched=0x%08x cycle=%d js0=0x%08x js1=0x%08x intmask=0x%08x drain_rc=%d' "$JOBC" \
	|| gate_fail "recover-done telemetry missing in applied job.c"
grep -qF 'p7580_job: recover-cycle FAIL rc=%d' "$JOBC" \
	|| gate_fail "recover-cycle FAIL telemetry missing in applied job.c"
printf '%s\n' "$RECB" | grep -E 'POWER_FAULT|slots_latched_fault|power_cycle|cycled|program_as|dev_rearm|recover-done' | tee -a "$EVID"
# (3d) resume clears a latched fault with the same cycle, before AS0.
RESB=$(awk '/^static int p7580_drv_runtime_resume/,/^}/' "$MAINC")
[ -n "$RESB" ] || gate_fail "p7580_drv_runtime_resume missing in applied main.c"
printf '%s\n' "$RESB" | grep -q 'p7580_job_slots_latched_fault(&drv->jd)' \
	|| gate_fail "resume does not test for a latched fault"
printf '%s\n' "$RESB" | grep -q 'drv->jd.power_cycle(drv->jd.pm_dev)' \
	|| gate_fail "resume does not cycle a latched fault"
printf '%s\n' "$RESB" | grep -qF 'p7580_job: resume-cycle rc=%d (latched JS fault)' \
	|| gate_fail "resume-cycle telemetry missing in applied main.c"
RES_CYC=$(printf '%s\n' "$RESB" | grep -n 'drv->jd.power_cycle(drv->jd.pm_dev)' | cut -d: -f1 | head -1)
RES_AS=$(printf '%s\n' "$RESB" | grep -n 'p7580_mmu_program_as(drv->regs' | cut -d: -f1 | head -1)
[ -n "$RES_CYC" ] && [ -n "$RES_AS" ] && [ "$RES_CYC" -lt "$RES_AS" ] \
	|| gate_fail "resume cycle is not before the AS0 reprogram (cycle=$RES_CYC as0=$RES_AS)"
printf '%s\n' "$RESB" | grep -E 'slots_latched_fault|power_cycle|program_as|dev_rearm|resume-cycle' | tee -a "$EVID"
# (3e) driver 0001 mirror of every new line.
[ "$(grep -c '^+int pf7580_power_cycle(struct pf7580_power \*p)$' "$DRV1")" = 1 ] \
	|| gate_fail "driver 0001 mirror lost pf7580_power_cycle definition"
[ "$(grep -c '^+int p7580_job_slots_latched_fault(struct p7580_job_dev \*jd)$' "$DRV1")" = 1 ] \
	|| gate_fail "driver 0001 mirror lost p7580_job_slots_latched_fault"
grep -q '^+.*recover-done slot=%d latched=0x%08x cycle=%d' "$DRV1" \
	|| gate_fail "driver 0001 mirror lost the recover-done telemetry"
grep -q '^+.*resume-cycle rc=%d (latched JS fault)' "$DRV1" \
	|| gate_fail "driver 0001 mirror lost the resume-cycle telemetry"
grep -q '^+static int pf7580_cores_off_locked(struct pf7580_power \*p)$' "$DRV1" \
	|| gate_fail "driver 0001 mirror lost pf7580_cores_off_locked"
grep -q '^+.*return -EBUSY;' "$DRV1" \
	|| gate_fail "driver 0001 mirror lost the -EBUSY fail-closed return"
note "driver 0001 mirror: $(grep -c '^+.*return -EBUSY;' "$DRV1") -EBUSY sites (cores-off, drv guard, dvfs init)"
# (3f) compiled proof.
aarch64-linux-gnu-nm "$POW_O" | grep -q ' T pf7580_power_cycle$' \
	|| gate_fail "pf7580_power_cycle not T in power.o"
aarch64-linux-gnu-nm "$POW_O" | grep -q ' T pf7580_runtime_suspend$' \
	|| gate_fail "pf7580_runtime_suspend not T in power.o"
aarch64-linux-gnu-nm "$MAIN_O" | grep -q ' [tT] p7580_drv_runtime_suspend$' \
	|| gate_fail "p7580_drv_runtime_suspend not in main.o"
aarch64-linux-gnu-nm "$MAIN_O" | grep -q ' [tT] p7580_job_power_cycle_void$' \
	|| gate_fail "p7580_job_power_cycle_void not in main.o"
aarch64-linux-gnu-nm "$JOB_O" | grep -q ' T p7580_job_is_idle$' \
	|| gate_fail "p7580_job_is_idle not T in job.o"
aarch64-linux-gnu-nm "$JOB_O" | grep -q ' T p7580_job_slots_latched_fault$' \
	|| gate_fail "p7580_job_slots_latched_fault not T in job.o"
aarch64-linux-gnu-nm "$POW_O" "$MAIN_O" "$JOB_O" \
	| grep -E ' (pf7580_power_cycle|pf7580_runtime_suspend|p7580_drv_runtime_suspend|p7580_job_power_cycle_void|p7580_job_is_idle|p7580_job_slots_latched_fault)$' | tee -a "$EVID"
# -EBUSY (-16) survives compilation: cores_off is a real outlined call and
# runtime_suspend branches to a -16 return before the clock-off block.
NM16='movn[[:space:]]*w[0-9]+, #0xf|mov[[:space:]]*w[0-9]+, #0xfffffff0'
RSDIS=$(aarch64-linux-gnu-objdump -d --disassemble=pf7580_runtime_suspend "$POW_O")
printf '%s\n' "$RSDIS" | grep -q 'bl.*<pf7580_cores_off_locked>' \
	|| gate_fail "power.o runtime_suspend does not call the outlined cores-off helper"
printf '%s\n' "$RSDIS" | grep -qE "$NM16" \
	|| gate_fail "power.o runtime_suspend lost the immediate -EBUSY return"
printf '%s\n' "$RSDIS" | grep -E 'bl.*<pf7580_cores_off_locked>|bl.*<clk_disable>|mov|#0xfffffff0' | tee -a "$EVID"
# power_cycle must NOT abort on busy cores (the power-on reset is the fix):
# it warns and continues to the clock gate + power-on.
[ "$(aarch64-linux-gnu-strings "$POW_O" | grep -c 'power cycle with busy cores')" = 1 ] \
	|| gate_fail "power.o power_cycle lost the busy-core warning"
PCDIS=$(aarch64-linux-gnu-objdump -d --disassemble=pf7580_power_cycle "$POW_O")
printf '%s\n' "$PCDIS" | grep -q 'bl.*<dev_warn>' \
	|| gate_fail "power.o power_cycle does not warn on busy cores"
printf '%s\n' "$PCDIS" | grep -qE 'bl.*<pf7580_power_on_locked' \
	|| gate_fail "power.o power_cycle does not run the full power-on after the cycle"
printf '%s\n' "$PCDIS" | grep -E 'bl.*<(pf7580_cores_off_locked|dev_warn|clk_disable|pf7580_power_on_locked)' | tee -a "$EVID"
# main.o: idle guard first, -EBUSY on busy, else the power-slice call.
DRSDIS=$(aarch64-linux-gnu-objdump -d --disassemble=p7580_drv_runtime_suspend "$MAIN_O")
printf '%s\n' "$DRSDIS" | grep -q 'bl.*<p7580_job_is_idle>' \
	|| gate_fail "main.o drv_runtime_suspend does not call p7580_job_is_idle"
printf '%s\n' "$DRSDIS" | grep -qE "$NM16" \
	|| gate_fail "main.o drv_runtime_suspend lost the -EBUSY idle guard"
printf '%s\n' "$DRSDIS" | grep -q 'bl.*<pf7580_runtime_suspend>' \
	|| gate_fail "main.o drv_runtime_suspend does not propagate to the power slice"
DRS_IOBJ=$(printf '%s\n' "$DRSDIS" | grep -n 'bl.*<p7580_job_is_idle>' | cut -d: -f1 | head -1)
DRS_POBJ=$(printf '%s\n' "$DRSDIS" | grep -n 'bl.*<pf7580_runtime_suspend>' | cut -d: -f1 | head -1)
[ -n "$DRS_IOBJ" ] && [ -n "$DRS_POBJ" ] && [ "$DRS_IOBJ" -lt "$DRS_POBJ" ] \
	|| gate_fail "compiled idle guard is not before the suspend call (idle=$DRS_IOBJ susp=$DRS_POBJ)"
printf '%s\n' "$DRSDIS" | grep -E 'bl.*<(p7580_job_is_idle|pf7580_runtime_suspend|pf7580_dvfs_suspend)>|#0xfffffff0' | tee -a "$EVID"
# is_idle object: two device reads (COMMAND_NEXT + JS_STATUS, each behind a
# dsb) and the (st - 2) <= 2 stop exemption; the JS_STATUS path cannot be
# compiled out.
IDLEDIS=$(aarch64-linux-gnu-objdump -d --disassemble=p7580_job_is_idle "$JOB_O")
[ "$(printf '%s\n' "$IDLEDIS" | grep -c 'dsb[[:space:]]*ld')" = 2 ] \
	|| gate_fail "job.o is_idle must perform 2 ordered device reads (COMMAND_NEXT + JS_STATUS), got $(printf '%s\n' "$IDLEDIS" | grep -c 'dsb[[:space:]]*ld')"
printf '%s\n' "$IDLEDIS" | grep -qE 'sub[[:space:]]*w[0-9]+, w[0-9]+, #0x2' \
	|| gate_fail "job.o is_idle has no (status - 2) stop-code mapping"
printf '%s\n' "$IDLEDIS" | grep -qE 'cmp[[:space:]]*w[0-9]+, #0x2' \
	|| gate_fail "job.o is_idle has no stop-code range check"
printf '%s\n' "$IDLEDIS" | grep -E 'ldr|dsb|sub|cmp|tbz|cbnz|b\.hi' | tee -a "$EVID"
# linked Image: symbols and always-on recovery telemetry.
for s in 'p7580_job: recover-done slot=%d latched=0x%08x cycle=%d js0=0x%08x js1=0x%08x intmask=0x%08x drain_rc=%d' \
	 'p7580_job: recover-cycle FAIL rc=%d' \
	 'p7580_job: resume-cycle rc=%d (latched JS fault)'; do
	aarch64-linux-gnu-strings "$JOB_O" "$MAIN_O" | grep -qF "$s" \
		|| gate_fail "telemetry string missing from a compiled object: $s"
	aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -qF "$s" \
		|| gate_fail "telemetry string missing from Image: $s"
done
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -E 'recover-done|recover-cycle|resume-cycle' | tee -a "$EVID"
for s in ' T pf7580_power_cycle$' ' T pf7580_runtime_suspend$' ' T p7580_job_is_idle$' ' T p7580_job_slots_latched_fault$'; do
	[ "$(grep -c "$s" System.map)" = 1 ] \
		|| gate_fail "System.map $s count != 1"
done
grep -E ' (pf7580_power_cycle|pf7580_runtime_suspend|p7580_job_is_idle|p7580_job_slots_latched_fault)$' System.map | tee -a "$EVID"
# host T9 (submit_fix_test.c): the cycle decision table.
grep -q 'run_t9();' "$REPO/code/gpu31/driver/submit_fix_test.c" \
	|| gate_fail "host submit_fix_test has no T9 (latch cycle decision table)"
grep -q 'model_recover_latch' "$REPO/code/gpu31/driver/submit_fix_test.c" \
	|| gate_fail "host submit_fix_test T9 model missing"
note "== int49 power-idle gate: PASS (cores-off fails closed with cores back on + -EBUSY, runtime_suspend propagates before clk_off, driver refuses while !is_idle; is_idle reads JS_STATUS ACTIVE except STOPPED; power_cycle wired through job/recover/resume and recover() ends in it for EVERY status when the hook is present (bare reset only as the unwired fallback), recover-done/recover-cycle/resume-cycle live in tree+objects+Image; host T9 present) =="

# 6ab. gpu31-firstfault gates (69b4a49, folded into driver 0001). The int49
# Image must carry, on top of int40:
#   (1) a 128-entry in-RAM event ring fed by the dispatch/IRQ/timeout/
#       cycle/recover legs, dumped exactly once at the first fault;
#   (2) the ev_arm debugfs knob: disarmed (default) the dispatch path does
#       no extra MMIO and no ring stores; one write re-arms ring + dump;
#   (3) the one-shot 4 KiB page dump for jc, the live src_jc of a shadow
#       node, the fragment SFBD and the >= 1 MiB tiler heap, taken BEFORE
#       recover() mutates the slots;
#   (4) descriptor words 0..3 (JM outputs) zeroed in every shadow copy
#       and a stale-output single-node chain routed through a shadow;
#   (5) serialize_fs (default 0) queues the FS submit on the chain slot's
#       entity for the one-boot ordering A/B; the hardware slot stays 0.
note "== int49 gate: first-fault instrumentation present =="
FFH=drivers/gpu/drm/panfrost/panfrost7580_job.h
FFC=drivers/gpu/drm/panfrost/panfrost7580_job.c
FFS=drivers/gpu/drm/panfrost/panfrost7580_submit.c
for f in "$FFH" "$FFC" "$FFS" "$JOB_O" "$SUBMIT_O"; do
	[ -f "$f" ] || gate_fail "$f missing"
done
# (1) ring surface in job.h: 128 entries, all ten kinds, ring/knob state,
# the dumper hook and the API declarations.
grep -qE '^#define P7580_EV_MAX[[:space:]]+128u' "$FFH" \
	|| gate_fail "P7580_EV_MAX 128 missing in applied job.h"
for k in START DONE ERR TIMEOUT RECOVER SUSPEND SUSPEND_BUSY RESUME CYCLE DUMP; do
	grep -q "P7580_EV_$k" "$FFH" \
		|| gate_fail "event kind P7580_EV_$k missing in applied job.h"
done
grep -q 'struct p7580_ev ev\[P7580_EV_MAX\];' "$FFH" \
	|| gate_fail "128-entry ring field missing in applied job.h"
for fld in 'unsigned int ev_head;' 'unsigned int ev_arm;' 'unsigned int ev_dumped;' \
	'unsigned int serialize_fs;' 'void *desc_dump_priv;'; do
	grep -qF "$fld" "$FFH" || gate_fail "job.h lost '$fld'"
done
grep -q 'int (\*desc_dump)(void \*priv, uint64_t gpu, const char \*tag);' "$FFH" \
	|| gate_fail "job.h lost the desc_dump hook"
for d in 'void p7580_job_ev(struct p7580_job_dev *jd, unsigned int kind, int slot,' \
	'void p7580_job_ev_arm(struct p7580_job_dev *jd, unsigned int on);' \
	'void p7580_job_set_desc_dump(struct p7580_job_dev *jd,' \
	'void p7580_job_first_fault(struct p7580_job_dev *jd,'; do
	grep -qF "$d" "$FFH" || gate_fail "job.h lost declaration '$d'"
done
grep -q 'jd->ev_arm = 0;' "$FFC" || gate_fail "dev_init does not default ev_arm=0"
grep -q 'jd->serialize_fs = 0;' "$FFC" || gate_fail "dev_init does not default serialize_fs=0"
grep -q 'jd->desc_dump = NULL;' "$FFC" || gate_fail "dev_init does not clear the desc_dump hook"
# (2) recorder: ktime in ns (3.10 has no ktime_get_ns), IRQ-safe stores
# under hw_lock, a pure no-op while disarmed.
[ "$(grep -c '^void p7580_job_ev(struct p7580_job_dev \*jd, unsigned int kind, int slot,$' "$FFC")" = 1 ] \
	|| gate_fail "p7580_job_ev definition count != 1"
grep -q '#include <linux/ktime.h>' "$FFC" || gate_fail "job.c lost the ktime include"
grep -q 'e->ns = (uint64_t)ktime_to_ns(ktime_get());' "$FFC" \
	|| gate_fail "ring timestamp is not ktime_to_ns(ktime_get())"
if awk '{ l=$0; sub(/^[[:space:]]*/, "", l); if (l ~ /^(\*|\/\*)/) next; if (index($0, "ktime_get_ns(")) print }' "$FFC" | grep -q .; then
	gate_fail "job.c calls ktime_get_ns (3.10 has no such API; use ktime_to_ns(ktime_get()))"
fi
EVB=$(awk '/^void p7580_job_ev\(struct p7580_job_dev \*jd, unsigned int kind, int slot,/,/^}/' "$FFC")
[ -n "$EVB" ] || gate_fail "p7580_job_ev body not found in applied job.c"
printf '%s\n' "$EVB" | grep -q 'if (!jd || !jd->ev_arm)' \
	|| gate_fail "p7580_job_ev is not a no-op when disarmed"
printf '%s\n' "$EVB" | grep -q 'e = &jd->ev\[jd->ev_head & (P7580_EV_MAX - 1u)\];' \
	|| gate_fail "ring index is not the power-of-two mask"
printf '%s\n' "$EVB" | grep -q 'jd->ev_head++;' \
	|| gate_fail "ring head is not advanced"
printf '%s\n' "$EVB" | grep -q 'spin_lock_irqsave(&jd->hw_lock, flags);' \
	|| gate_fail "ring stores are not serialised by hw_lock"
printf '%s\n' "$EVB" | tee -a "$EVID"
ARMB=$(awk '/^void p7580_job_ev_arm\(struct p7580_job_dev \*jd, unsigned int on\)/,/^}/' "$FFC")
printf '%s\n' "$ARMB" | grep -q 'jd->ev_head = 0;' || gate_fail "ev_arm does not reset the ring head"
printf '%s\n' "$ARMB" | grep -q 'jd->ev_dumped = 0;' || gate_fail "ev_arm does not re-open the one-shot dump"
printf '%s\n' "$ARMB" | grep -q 'jd->ev_arm = on ? 1u : 0u;' || gate_fail "ev_arm does not store the arm flag"
printf '%s\n' "$ARMB" | grep -q 'memset(jd->ev, 0, sizeof(jd->ev));' || gate_fail "ev_arm does not clear the ring"
# one record per leg that decides the cascade.
for k in START DONE ERR TIMEOUT CYCLE RECOVER; do
	[ "$(grep -c "p7580_job_ev(jd, P7580_EV_$k," "$FFC")" = 1 ] \
		|| gate_fail "p7580_job_ev P7580_EV_$k call count != 1, got $(grep -c "p7580_job_ev(jd, P7580_EV_$k," "$FFC")"
done
# armed-only dispatch probes: the pre-START JS_STATUS, the occupied-slot bit
# (a non-NULL previous in-flight job), and COMMAND_NEXT/JS_STATUS readback.
HWB=$(awk '/^void p7580_job_hw_submit\(struct p7580_job_dev \*jd,/,/^}/' "$FFC")
printf '%s\n' "$HWB" | grep -q 'int armed = jd->ev_arm;' || gate_fail "hw_submit does not snapshot the arm flag"
printf '%s\n' "$HWB" | grep -q 'prev = jd->in_flight\[slot\];' || gate_fail "hw_submit does not check the previous occupant"
printf '%s\n' "$HWB" | grep -q 'st_pre = p7580_js_read(jd, slot, P7580_JS_STATUS);' || gate_fail "hw_submit does not probe JS_STATUS pre-START"
printf '%s\n' "$HWB" | grep -q 'if (armed) {' || gate_fail "hw_submit probes are not gated on the arm flag"
printf '%s\n' "$HWB" | grep -q 'st_pre | (prev ? 0x80000000u : 0u),' || gate_fail "dispatch event does not carry the occupied-slot bit"
printf '%s\n' "$HWB" | grep -q 'cmd_post = p7580_js_read(jd, slot, P7580_JS_COMMAND_NEXT);' || gate_fail "hw_submit does not read COMMAND_NEXT back after START"
printf '%s\n' "$HWB" | grep -q 'st_post = p7580_js_read(jd, slot, P7580_JS_STATUS);' || gate_fail "hw_submit does not read JS_STATUS back after START"
# (3) the one-shot dump: armed + undumped, oldest-first, four pages, then
# the pre-recover ordering in the timeout work.
FFB=$(awk '/^void p7580_job_first_fault\(struct p7580_job_dev \*jd,/,/^}/' "$FFC")
[ -n "$FFB" ] || gate_fail "p7580_job_first_fault missing in applied job.c"
printf '%s\n' "$FFB" | grep -q 'if (!jd || !jd->ev_arm || jd->ev_dumped)' \
	|| gate_fail "first_fault is not gated on armed + undumped"
printf '%s\n' "$FFB" | grep -q 'jd->ev_dumped = 1;' || gate_fail "first_fault is not one-shot"
printf '%s\n' "$FFB" | grep -q 'n = jd->ev_head < P7580_EV_MAX ? jd->ev_head : P7580_EV_MAX;' \
	|| gate_fail "first_fault does not clamp the ring length"
printf '%s\n' "$FFB" | grep -q 'start = jd->ev_head - n;' || gate_fail "first_fault does not start at the oldest entry"
printf '%s\n' "$FFB" | grep -q '&jd->ev\[(start + i) & (P7580_EV_MAX - 1u)\];' \
	|| gate_fail "first_fault ring walk is not the masked oldest-first sequence"
for tg in jc src fbd heap; do
	printf '%s\n' "$FFB" | grep -q "\"$tg\");" \
		|| gate_fail "first_fault lost the '$tg' page dump"
done
printf '%s\n' "$FFB" | grep -q 'if (job->src_jc && job->src_jc != job->jc)' \
	|| gate_fail "first_fault dumps src_jc unconditionally (must differ from jc)"
printf '%s\n' "$FFB" | grep -q 'if (job->fbd)' \
	|| gate_fail "first_fault lost the fragment SFBD page selection (job->fbd)"
grep -q 'job->desc\[10\]' "$FFC" \
	&& gate_fail "job.c still reads desc[10] (8-word array; the fbd must be captured at SUBMIT)"
printf '%s\n' "$FFB" | grep -q 'if (bsz >= 0x100000u)' \
	|| gate_fail "first_fault lost the >= 1 MiB tiler-heap page selection"
printf '%s\n' "$FFB" | grep -E 'EVRING begin|EV %|desc_dump\(' | tee -a "$EVID"
TMD=$(awk '/^void p7580_job_timedout\(struct p7580_job_dev \*jd,/,/^}/' "$FFC")
[ -n "$TMD" ] || gate_fail "p7580_job_timedout missing in applied job.c"
printf '%s\n' "$TMD" | grep -q 'p7580_job_first_fault(jd, job, status);' \
	|| gate_fail "timedout does not run the first-fault dump"
T_EV=$(printf '%s\n' "$TMD" | grep -n 'p7580_job_ev(jd, P7580_EV_TIMEOUT,' | cut -d: -f1 | head -1)
T_FF=$(printf '%s\n' "$TMD" | grep -n 'p7580_job_first_fault(jd, job, status);' | cut -d: -f1 | head -1)
T_REC=$(printf '%s\n' "$TMD" | grep -n 'p7580_job_recover(jd, slot, status);' | cut -d: -f1 | head -1)
[ -n "$T_EV" ] && [ -n "$T_FF" ] && [ -n "$T_REC" ] && \
	[ "$T_EV" -lt "$T_FF" ] && [ "$T_FF" -lt "$T_REC" ] \
	|| gate_fail "timedout order is not TIMEOUT event($T_EV) -> dump($T_FF) -> recover($T_REC)"
# dump wiring: registered at probe, descriptor page reader in submit.c,
# src_jc stamped on every split node.
[ "$(grep -c 'p7580_job_set_desc_dump(&drv->jd, p7580_drv_desc_dump, drv);' "$MAINC")" = 1 ] \
	|| gate_fail "probe does not register p7580_drv_desc_dump (applied main.c)"
[ "$(grep -c '^int p7580_drv_desc_dump(void \*priv, uint64_t gpu, const char \*tag)$' "$FFS")" = 1 ] \
	|| gate_fail "p7580_drv_desc_dump definition count != 1"
grep -qE '^#define P7580_PAGE_DUMP_LEN[[:space:]]+4096u' "$FFS" \
	|| gate_fail "P7580_PAGE_DUMP_LEN 4096 missing in applied submit.c"
grep -q 'base = gpu & ~(uint64_t)(P7580_PAGE_DUMP_LEN - 1u);' "$FFS" \
	|| gate_fail "desc dump does not page-align the GPU address"
grep -q 'for (i = 0; i < P7580_PAGE_DUMP_LEN / 4u / 16u; i++)' "$FFS" \
	|| gate_fail "desc dump does not emit the whole 4 KiB as 64 lines of 16 words"
grep -q 'p7580_job: PAGE %s slot%02u off=%03x' "$FFS" \
	|| gate_fail "PAGE line missing in applied submit.c"
grep -q 'job->src_jc = (s->src && s->shadow) ? s->src\[pos\] : 0;' "$FFS" \
	|| gate_fail "split node does not stamp src_jc"
grep -q 'uint64_t src_jc;' drivers/gpu/drm/panfrost/panfrost7580_job.h \
	|| gate_fail "job.h lost the src_jc field"
# (4) descriptor hygiene: words 0..3 are JM outputs. Four zero stores after
# the isolate and before the header copy / shadow write; stale-output
# single-node chains are forced through a shadow.
for w in 0 1 2 3; do
	[ "$(grep -c "jobw\[$w\] = 0;" "$FFS")" = 1 ] \
		|| gate_fail "submit.c jobw[$w] = 0 count != 1, got $(grep -c "jobw\[$w\] = 0;" "$FFS")"
done
ISO_LN=$(grep -n 'p7580_chain_hdr_isolate(jobw, ci);' "$FFS" | cut -d: -f1 | head -1)
H0_LN=$(grep -n 'jobw\[0\] = 0;' "$FFS" | cut -d: -f1 | head -1)
H3_LN=$(grep -n 'jobw\[3\] = 0;' "$FFS" | cut -d: -f1 | head -1)
CP_LN=$(grep -n 'memcpy(hdr\[ci\], jobw,' "$FFS" | cut -d: -f1 | head -1)
SW_LN=$(grep -n 'p7580_drv_shadow_write(shadow->obj,' "$FFS" | cut -d: -f1 | head -1)
[ -n "$ISO_LN" ] && [ -n "$H0_LN" ] && [ -n "$H3_LN" ] && [ -n "$CP_LN" ] && [ -n "$SW_LN" ] && \
	[ "$ISO_LN" -lt "$H0_LN" ] && [ "$H0_LN" -lt "$H3_LN" ] && [ "$H3_LN" -lt "$CP_LN" ] && [ "$CP_LN" -lt "$SW_LN" ] \
	|| gate_fail "hygiene order is not isolate($ISO_LN) -> w0..w3 zero($H3_LN) -> hdr copy($CP_LN) -> shadow write($SW_LN)"
grep -q 'for (ci = 0; ci < nodes && !need_shadow; ci++) {' "$FFS" \
	|| gate_fail "desc-stale scan missing in applied submit.c"
grep -q 'if (hdr\[ci\]\[0\] | hdr\[ci\]\[1\] | hdr\[ci\]\[2\] | hdr\[ci\]\[3\]) {' "$FFS" \
	|| gate_fail "desc-stale word 0..3 test missing in applied submit.c"
[ "$(grep -c 'p7580_job: desc-stale nodes=' "$FFS")" = 1 ] \
	|| gate_fail "desc-stale log line count != 1, got $(grep -c 'p7580_job: desc-stale nodes=' "$FFS")"
grep -qF 'p7580_job: desc-stale nodes=%u pos=%u src=0x%llx w=%08x %08x %08x %08x (shadowed+sanitised)' "$FFS" \
	|| gate_fail "desc-stale log line missing in applied submit.c"
NS_LN=$(grep -n 'need_shadow = nodes > 1 ||' "$FFS" | cut -d: -f1 | head -1)
SC_LN=$(grep -n 'for (ci = 0; ci < nodes && !need_shadow; ci++) {' "$FFS" | cut -d: -f1 | head -1)
SH_LN=$(grep -n 'if (need_shadow) {' "$FFS" | cut -d: -f1 | head -1)
[ -n "$NS_LN" ] && [ -n "$SC_LN" ] && [ -n "$SH_LN" ] && \
	[ "$NS_LN" -lt "$SC_LN" ] && [ "$SC_LN" -lt "$SH_LN" ] \
	|| gate_fail "desc-stale scan is not between the pass-through decision($NS_LN) and the shadow allocation($SH_LN)"
printf '%s\n' "$HWB" | grep -E 'ev_arm|prev = jd->in_flight|st_pre|cmd_post|st_post' | tee -a "$EVID"
grep -n 'jobw\[[0-3]\] = 0;\|desc-stale' "$FFS" | tee -a "$EVID"
# (5) serialize_fs: knob, default 0, FS-on-chain-slot entity only.
grep -q 'jd->serialize_fs = val ? 1u : 0u;' "$FFC" || gate_fail "serialize_fs setter missing in applied job.c"
grep -q 'DEFINE_SIMPLE_ATTRIBUTE(p7580_job_serialize_fs_fops,' "$FFC" || gate_fail "serialize_fs fops missing in applied job.c"
grep -q 'debugfs_create_file("serialize_fs", 0600, dir, jd,' "$FFC" || gate_fail "serialize_fs debugfs file missing in applied job.c"
grep -q 'debugfs_create_file("ev_arm", 0600, dir, jd,' "$FFC" || gate_fail "ev_arm debugfs file missing in applied job.c"
grep -q 'DEFINE_SIMPLE_ATTRIBUTE(p7580_job_ev_arm_fops,' "$FFC" || gate_fail "ev_arm fops missing in applied job.c"
grep -q 'if (slot == 0 && drv->jd.serialize_fs &&' "$FFS" \
	|| gate_fail "serialize_fs queue override missing in applied submit.c"
grep -q 'entity = &df->entity\[1\];' "$FFS" \
	|| gate_fail "serialize_fs does not ride the chain-slot entity"
grep -q '(drv->jd.js_present & (1u << 1)))' "$FFS" \
	|| gate_fail "serialize_fs does not require the chain slot to be present"
[ "$(grep -cE '^[[:space:]]*job->slot = s->slot;' "$FFS")" = 1 ] \
	|| gate_fail "the hardware slot stamp (job->slot = s->slot) is not exactly once"
ENT_LN=$(grep -n 'entity = &df->entity\[slot\];' "$FFS" | cut -d: -f1 | head -1)
SER_LN=$(grep -n 'if (slot == 0 && drv->jd.serialize_fs &&' "$FFS" | cut -d: -f1 | head -1)
INF_LN=$(grep -n 'if (args->in_sync_count) {' "$FFS" | cut -d: -f1 | head -1)
[ -n "$ENT_LN" ] && [ -n "$SER_LN" ] && [ -n "$INF_LN" ] && \
	[ "$ENT_LN" -lt "$SER_LN" ] && [ "$SER_LN" -lt "$INF_LN" ] \
	|| gate_fail "serialize_fs override is not between the entity pick($ENT_LN) and the in-fence loop($INF_LN)"
# driver 0001 mirror of every new line (the applied tree is what the Image
# is built from; the mirror is what the next ticket re-applies).
for pat in '^+#define P7580_EV_MAX' \
	'^+void p7580_job_ev(struct p7580_job_dev \*jd,' \
	'^+void p7580_job_ev_arm(struct p7580_job_dev \*jd, unsigned int on)' \
	'^+void p7580_job_first_fault(struct p7580_job_dev \*jd,' \
	'^+int p7580_drv_desc_dump(void \*priv, uint64_t gpu, const char \*tag)' \
	'^+.*EVRING begin head=' \
	'^+.*PAGEDUMP %s gpu=0x%llx page=0x%llx off=0x%llx' \
	'^+.*desc-stale nodes=' \
	'^+.*do_cycle = (jd->power_cycle && jd->pm_dev) ? 1 : 0;' \
	'^+.*entity = &df->entity\[1\];' \
	'^+.*job->src_jc = (s->src && s->shadow) ? s->src\[pos\] : 0;' \
	'^+.*ktime_to_ns(ktime_get())'; do
	grep -q "$pat" "$DRV1" || gate_fail "driver 0001 mirror lost '$pat'"
done
[ "$(grep -c '^+.*jobw\[[0-3]\] = 0;$' "$DRV1")" = 4 ] \
	|| gate_fail "driver 0001 mirror must carry the 4 jobw[0..3] zero stores"
grep -q '^+.*status == P7580_JS_STATUS_POWER_FAULT' "$DRV1" \
	&& gate_fail "driver 0001 mirror still gates recovery on the POWER_FAULT status"
# compiled proof: symbols, string literals and the ring's ktime call.
aarch64-linux-gnu-nm "$JOB_O" | grep -q ' T p7580_job_ev$' || gate_fail "p7580_job_ev not T in job.o"
aarch64-linux-gnu-nm "$JOB_O" | grep -q ' T p7580_job_ev_arm$' || gate_fail "p7580_job_ev_arm not T in job.o"
aarch64-linux-gnu-nm "$JOB_O" | grep -q ' T p7580_job_first_fault$' || gate_fail "p7580_job_first_fault not T in job.o"
aarch64-linux-gnu-nm "$JOB_O" | grep -q ' T p7580_job_set_desc_dump$' || gate_fail "p7580_job_set_desc_dump not T in job.o"
aarch64-linux-gnu-nm "$SUBMIT_O" | grep -q ' T p7580_drv_desc_dump$' || gate_fail "p7580_drv_desc_dump not T in submit.o"
aarch64-linux-gnu-nm "$JOB_O" "$SUBMIT_O" \
	| grep -E ' (p7580_job_(ev|ev_arm|first_fault|set_desc_dump)|p7580_drv_desc_dump)$' | tee -a "$EVID"
for s in ' T p7580_job_ev$' ' T p7580_job_first_fault$' ' T p7580_drv_desc_dump$'; do
	[ "$(grep -c "$s" System.map)" = 1 ] \
		|| gate_fail "System.map $s count != 1"
done
grep -E ' (p7580_job_ev|p7580_job_first_fault|p7580_drv_desc_dump)$' System.map | tee -a "$EVID"
EV_DIS=$(aarch64-linux-gnu-objdump -dr --disassemble=p7580_job_ev "$JOB_O")
[ "$(printf '%s\n' "$EV_DIS" | grep -c 'R_AARCH64_CALL26[[:space:]]*ktime_get')" = 1 ] \
	|| gate_fail "p7580_job_ev must timestamp via one ktime_get call"
printf '%s\n' "$EV_DIS" | grep -E 'ktime_get|ldr|str|add' | tee -a "$EVID"
HWEV=$(aarch64-linux-gnu-objdump -dr --disassemble=p7580_job_hw_submit "$JOB_O")
[ "$(printf '%s\n' "$HWEV" | grep -c 'R_AARCH64_CALL26[[:space:]]*p7580_job_ev')" = 1 ] \
	|| gate_fail "hw_submit must ring exactly one dispatch event, got $(printf '%s\n' "$HWEV" | grep -c 'R_AARCH64_CALL26[[:space:]]*p7580_job_ev')"
printf '%s\n' "$HWEV" | grep -E 'p7580_job_ev|js_read|ev_arm' | head -12 | tee -a "$EVID"
DD_DIS=$(aarch64-linux-gnu-objdump -dr --disassemble=p7580_drv_desc_dump "$SUBMIT_O")
[ "$(printf '%s\n' "$DD_DIS" | grep -c 'bl.*<p7580_drv_gpu_read>')" = 1 ] \
	|| gate_fail "desc dump must read the page once via p7580_drv_gpu_read, got $(printf '%s\n' "$DD_DIS" | grep -c 'bl.*<p7580_drv_gpu_read>')"
printf '%s\n' "$DD_DIS" | grep -qE 'bl.*<(kmem_cache_alloc_trace|__kmalloc|kmalloc)>' \
	|| gate_fail "desc dump does not allocate its 4 KiB buffer"
printf '%s\n' "$DD_DIS" | grep -qE 'mov[[:space:]]*[xw][0-9]+, #0x1000' \
	|| gate_fail "desc dump does not use a 4096-byte page"
printf '%s\n' "$DD_DIS" | grep -E '#0x1000|kmalloc|gpu_read|mov' | head -12 | tee -a "$EVID"
RDIS=$(aarch64-linux-gnu-objdump -d --disassemble=p7580_job_recover "$JOB_O")
printf '%s\n' "$RDIS" | grep -q 'blr[[:space:]]*x' \
	|| gate_fail "recover carries no indirect power_cycle hook call in job.o"
printf '%s\n' "$RDIS" | grep -q '#0x41' \
	&& gate_fail "job.o recover still carries a 0x41 comparison (power cycle must not be status-gated)"
printf '%s\n' "$RDIS" | grep -E 'blr|cmp|cbz|cbnz' | head -20 | tee -a "$EVID"
for s in 'p7580_job: EVRING begin head=%u n=%u status=0x%08x jc=0x%llx seq=%u src=0x%llx fbd=0x%x jc_age=%u' \
	'p7580_job: EV %-12s dt=%8llu us slot=%u seq=%u jc=0x%llx a=%08x b=%08x c=%08x' \
	'p7580_job: EVRING end' \
	'p7580_job: PAGEDUMP %s gpu=0x%llx page=0x%llx off=0x%llx' \
	'p7580_job: PAGE %s slot%02u off=%03x' \
	'p7580_job: desc-stale nodes=%u pos=%u src=0x%llx w=%08x %08x %08x %08x (shadowed+sanitised)' \
	'faultdbg ev_arm=%u ev_head=%u ev_dumped=%u serialize_fs=%u' \
	'p7580_job: ev_arm=%d (ring %u entries, dump one-shot)' \
	'p7580_job: serialize_fs=%u'; do
	aarch64-linux-gnu-strings "$JOB_O" "$SUBMIT_O" | grep -qF "$s" \
		|| gate_fail "string missing from a compiled object: $s"
	aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -qF "$s" \
		|| gate_fail "string missing from Image: $s"
done
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -E 'EVRING|PAGEDUMP|desc-stale|faultdbg|ev_arm=|serialize_fs=' | tee -a "$EVID"
for s in 'EVRING begin' 'PAGEDUMP %s gpu=0x%llx page=' 'desc-stale nodes=' \
	'p7580_job: serialize_fs=%u'; do
	[ "$(aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -cF "$s")" = 1 ] \
		|| gate_fail "Image '$s' count != 1"
done
# host models: T9 now pins the always-power-on rule (four wired cases, the
# silent 0x08 and our stop codes included) and T1 the word 0..3 hygiene.
FFT=$REPO/code/gpu31/driver/submit_fix_test.c
[ "$(grep -c 'l.hook_wired = 1;' "$FFT")" = 4 ] \
	|| gate_fail "host T9 does not wire the hook in all four cases"
grep -q 'if (l->hook_wired)' "$FFT" || gate_fail "host T9 model does not cycle on the hook alone"
[ "$(grep -c 'l.latched_status = JS_NO_EXCEPTION;' "$FFT")" = 2 ] \
	|| gate_fail "host T9 lost the silent NO_EXCEPTION (0x08) cases"
[ "$(grep -c 'CHECK(l.cycle_calls == 1);' "$FFT")" = 4 ] \
	|| gate_fail "host T9 does not expect one power-on per wired recovery"
grep -q 'l.latched_status = JS_STOPPED;' "$FFT" || gate_fail "host T9 lost the stop-code case"
if [ -f "$EXT/logs/gpu31-int49-hosttest.log" ]; then
	grep -q 'submit_fix_test: all checks passed' "$EXT/logs/gpu31-int49-hosttest.log" \
		|| gate_fail "host submit_fix_test did not pass (see log)"
fi
FFC1=$REPO/code/gpu31/driver/chainsplit_test.c
grep -q 't\[0\] = t\[1\] = t\[2\] = t\[3\] = 0;' "$FFC1" \
	|| gate_fail "host T1 does not pin the shadow-copy word 0..3 zeroing"
grep -q 'CHECK(t\[0\] == 1);' "$FFC1" \
	|| gate_fail "host T1 does not pin isolate() leaving the outputs alone"
note "== int49 first-fault gate: PASS (128-entry ev ring + ktime_to_ns(ktime_get()) + ev_arm knob/dump gating; one-shot 4 KiB EVRING/PAGEDUMP capture for jc/src_jc/SFBD/heap before recover; desc words 0..3 zeroed in every shadow copy + desc-stale shadowing; recovery cycle unconditional on the hook (no 0x41 gate), bare reset only as fallback; serialize_fs default 0 rides the chain-slot entity with the hardware slot unchanged; symbols/strings in job.o/submit.o/System.map/Image; host T9 0x08 + T1 hygiene) =="

# 6ac. gpu31-watchdog gates (39cce8c). The silent-hang budget is now the
# 750 ms drm_sched dispatch timeout (entity->timeout, seeded at
# drm_sched_init from a runtime variable), and two debugfs knobs make the
# silent hang deterministic and the budget tunable at runtime:
#   (1) P7580_JOB_TIMEOUT_MS 750u and no shadowing hardcoded 5000u; the
#       budget reaches drm_sched_init through p7580_drv_job_timeout_ms;
#   (2) job_timeout_ms get/set (clamp 100..60000, live sched->timeout,
#       seeds later entities), read-only when the driver is unwired;
#   (3) hang_inject: mask the slot's DONE+ERR bits BEFORE START for N
#       dispatches -- the job still runs, the fence waits the timeout;
#       default 0 and recovery's dev_rearm restores the mask;
#   (4) probe wiring + driver 0001 mirror + object/System.map/Image proof.
note "== int49 gate: watchdog timeout + hang injection present =="
WH=drivers/gpu/drm/panfrost/panfrost7580_jm_hw.h
WC=drivers/gpu/drm/panfrost/panfrost7580_job.c
WS=drivers/gpu/drm/panfrost/panfrost7580_submit.c
WM=drivers/gpu/drm/panfrost/panfrost7580_main.c
WD=drivers/gpu/drm/panfrost/panfrost7580_drv.h
WHH=drivers/gpu/drm/panfrost/panfrost7580_job.h
for f in "$WH" "$WC" "$WS" "$WM" "$WD" "$WHH" "$JOB_O" "$SUBMIT_O"; do
	[ -f "$f" ] || gate_fail "$f missing"
done
# (1) 750 ms dispatch budget, seeded at runtime.
grep -qE '^#define P7580_JOB_TIMEOUT_MS[[:space:]]+750u' "$WH" \
	|| gate_fail "P7580_JOB_TIMEOUT_MS 750u missing in applied jm_hw.h"
grep -qE '^#define P7580_JOB_TIMEOUT_MS[[:space:]]+5000u' "$WH" \
	&& gate_fail "stale 5000u dispatch timeout still in applied jm_hw.h"
grep -q 'static unsigned int p7580_drv_job_timeout_ms = P7580_JOB_TIMEOUT_MS;' "$WS" \
	|| gate_fail "runtime sched-timeout seed missing in applied submit.c"
grep -q 'msecs_to_jiffies(p7580_drv_job_timeout_ms),' "$WS" \
	|| gate_fail "drm_sched_init budget is not the runtime seed"
grep -q 'msecs_to_jiffies(P7580_JOB_TIMEOUT_MS)' "$WS" \
	&& gate_fail "drm_sched_init still uses the hardcoded constant"
grep -qE '^#define P7580_JOB_TIMEOUT_MS[[:space:]]+750u' "$REPO/code/gpu31/jobs/panfrost7580_jm_hw.h" \
	|| gate_fail "host jm_hw.h mirror is not 750u"
grep -q 'P7580_JOB_TIMEOUT_MS == 750u' "$REPO/code/gpu31/jobs/job_test.c" \
	|| gate_fail "host job_test does not pin P7580_JOB_TIMEOUT_MS == 750u"
# (2) job_timeout_ms accessors.
grep -q 'unsigned int p7580_drv_get_job_timeout_ms(void \*priv)$' "$WS" \
	|| gate_fail "p7580_drv_get_job_timeout_ms missing in applied submit.c"
grep -q 'int p7580_drv_set_job_timeout_ms(void \*priv, unsigned int ms)$' "$WS" \
	|| gate_fail "p7580_drv_set_job_timeout_ms missing in applied submit.c"
grep -q 'if (ms < 100u || ms > 60000u)' "$WS" \
	|| gate_fail "job_timeout_ms clamp 100..60000 missing"
grep -q 'p7580_drv_job_timeout_ms = ms;' "$WS" \
	|| gate_fail "setter does not store the new budget"
grep -q 'if (p7580_drv_sched_live)' "$WS" \
	|| gate_fail "setter does not test the live sched"
grep -q 'p7580_drv_sched.timeout = msecs_to_jiffies(ms);' "$WS" \
	|| gate_fail "setter does not update the live sched->timeout"
grep -qF 'p7580_sched: job_timeout_ms=%u (new entities)' "$WS" \
	|| gate_fail "job_timeout_ms log line missing in applied submit.c"
grep -q 'unsigned int p7580_drv_get_job_timeout_ms(void \*priv);' "$WD" \
	&& grep -q 'int p7580_drv_set_job_timeout_ms(void \*priv, unsigned int ms);' "$WD" \
	|| gate_fail "drv.h lost the timeout accessor declarations"
# (3) jobs-slice forwarder, state fields, defaults.
[ "$(grep -c '^void p7580_job_set_timeout_hooks(struct p7580_job_dev \*jd,$' "$WC")" = 1 ] \
	|| gate_fail "p7580_job_set_timeout_hooks definition count != 1"
grep -q 'void p7580_job_set_timeout_hooks(struct p7580_job_dev \*jd,' "$WHH" \
	|| gate_fail "job.h lost the set_timeout_hooks declaration"
for fld in 'unsigned int hang_arm;' 'unsigned int (*get_timeout_ms)(void *priv);' \
	'int (*set_timeout_ms)(void *priv, unsigned int ms);' 'void *timeout_priv;'; do
	grep -qF "$fld" "$WHH" || gate_fail "job.h lost '$fld'"
done
WHB=$(awk '/^void p7580_job_set_timeout_hooks\(struct p7580_job_dev \*jd,/,/^}/' "$WC")
[ -n "$WHB" ] || gate_fail "p7580_job_set_timeout_hooks body not found"
printf '%s\n' "$WHB" | grep -q 'jd->get_timeout_ms = get;' || gate_fail "hook does not store the getter"
printf '%s\n' "$WHB" | grep -q 'jd->set_timeout_ms = set;' || gate_fail "hook does not store the setter"
printf '%s\n' "$WHB" | grep -q 'jd->timeout_priv = priv;' || gate_fail "hook does not store priv"
printf '%s\n' "$WHB" | grep -q 'spin_lock_irqsave(&jd->hw_lock, flags);' || gate_fail "hook is not hw_lock serialised"
grep -q 'jd->hang_arm = 0;' "$WC" || gate_fail "dev_init does not default hang_arm=0"
grep -q 'jd->get_timeout_ms = NULL;' "$WC" || gate_fail "dev_init does not clear get_timeout_ms"
grep -q 'jd->set_timeout_ms = NULL;' "$WC" || gate_fail "dev_init does not clear set_timeout_ms"
grep -q 'jd->timeout_priv = NULL;' "$WC" || gate_fail "dev_init does not clear timeout_priv"
# (4) deterministic silent hang: mask before START, budget decremented.
HIB=$(awk '/^void p7580_job_hw_submit\(struct p7580_job_dev \*jd,/,/^}/' "$WC")
printf '%s\n' "$HIB" | grep -q 'if (jd->hang_arm) {' || gate_fail "hang_inject is not armed-tested"
printf '%s\n' "$HIB" | grep -q 'jd->hang_arm--;' || gate_fail "hang_inject does not spend its budget"
printf '%s\n' "$HIB" | grep -q 'p7580_writel(jd, P7580_JOB_INT_MASK,' \
	|| gate_fail "hang_inject does not write JOB_INT_MASK"
printf '%s\n' "$HIB" | grep -q '(jd->js_present | (jd->js_present << 16)) &' \
	|| gate_fail "hang_inject mask does not cover DONE+ERR of the present slots"
printf '%s\n' "$HIB" | grep -q '~P7580_MK_JS_MASK(slot));' \
	|| gate_fail "hang_inject does not clear the slot's own bits"
printf '%s\n' "$HIB" | grep -qF 'p7580_job: hang_inject slot=%d seq=%u irq-masked' \
	|| gate_fail "hang_inject log line missing"
HI_MASK=$(printf '%s\n' "$HIB" | grep -n 'p7580_writel(jd, P7580_JOB_INT_MASK,' | cut -d: -f1 | head -1)
HI_START=$(printf '%s\n' "$HIB" | grep -n 'P7580_JS_CMD_START' | cut -d: -f1 | head -1)
[ -n "$HI_MASK" ] && [ -n "$HI_START" ] && [ "$HI_MASK" -lt "$HI_START" ] \
	|| gate_fail "hang_inject mask write is not before the START (mask=$HI_MASK start=$HI_START)"
printf '%s\n' "$HIB" | grep -E 'hang_arm|JOB_INT_MASK|hang_inject|CMD_START' | tee -a "$EVID"
# (5) knobs: hang_inject + job_timeout_ms, inert/unwired behaviour.
grep -q 'jd->hang_arm = (unsigned int)val;' "$WC" || gate_fail "hang_inject setter missing"
grep -qF 'p7580_job: hang_inject=%u (IRQ masked at the next %u dispatches)' "$WC" \
	|| gate_fail "hang_inject setter log line missing"
grep -q 'if (!jd->get_timeout_ms) {' "$WC" || gate_fail "job_timeout_ms get does not fall back when unwired"
printf '%s\n' "$(awk '/^static int p7580_job_timeout_get/,/^}/' "$WC")" | grep -q '\*val = P7580_JOB_TIMEOUT_MS;' \
	|| gate_fail "job_timeout_ms read fallback is not P7580_JOB_TIMEOUT_MS"
grep -q 'if (!jd->set_timeout_ms)' "$WC" || gate_fail "job_timeout_ms set does not refuse when unwired"
grep -q 'return jd->set_timeout_ms(jd->timeout_priv, (unsigned int)val);' "$WC" \
	|| gate_fail "job_timeout_ms set does not forward to the driver"
grep -q 'DEFINE_SIMPLE_ATTRIBUTE(p7580_job_hang_fops,' "$WC" || gate_fail "hang_inject fops missing"
grep -q 'DEFINE_SIMPLE_ATTRIBUTE(p7580_job_timeout_fops,' "$WC" || gate_fail "job_timeout_ms fops missing"
[ "$(grep -c 'debugfs_create_file("hang_inject", 0600, dir, jd,' "$WC")" = 1 ] \
	|| gate_fail "hang_inject debugfs file count != 1"
[ "$(grep -c 'debugfs_create_file("job_timeout_ms", 0600, dir, jd,' "$WC")" = 1 ] \
	|| gate_fail "job_timeout_ms debugfs file count != 1"
grep -qF 'faultdbg ev_arm=%u ev_head=%u ev_dumped=%u serialize_fs=%u hang_arm=%u' "$WC" \
	|| gate_fail "faultdbg line does not report hang_arm"
# (6) probe wiring: exactly one registration, getter/setter/priv.
[ "$(grep -c 'p7580_job_set_timeout_hooks(&drv->jd,' "$WM")" = 1 ] \
	|| gate_fail "probe does not register the timeout hooks exactly once"
grep -q 'p7580_drv_get_job_timeout_ms,' "$WM" || gate_fail "probe does not pass the getter"
grep -q 'p7580_drv_set_job_timeout_ms, drv);' "$WM" || gate_fail "probe does not pass the setter + priv"
printf '%s\n' "$(awk '/p7580_job_set_timeout_hooks\(&drv->jd,/,/drv\);/' "$WM")" | tee -a "$EVID"
# (7) driver 0001 mirror (the next ticket re-applies this file).
for pat in '^+.*P7580_JOB_TIMEOUT_MS[[:space:]]*750u' \
	'^+static unsigned int p7580_drv_job_timeout_ms = P7580_JOB_TIMEOUT_MS;' \
	'^+.*msecs_to_jiffies(p7580_drv_job_timeout_ms),' \
	'^+unsigned int p7580_drv_get_job_timeout_ms(void \*priv)$' \
	'^+int p7580_drv_set_job_timeout_ms(void \*priv, unsigned int ms)$' \
	'^+.*ms < 100u || ms > 60000u' \
	'^+.*p7580_drv_sched.timeout = msecs_to_jiffies(ms);' \
	'^+.*p7580_sched: job_timeout_ms=%u (new entities)' \
	'^+void p7580_job_set_timeout_hooks(struct p7580_job_dev \*jd,' \
	'^+.*jd->hang_arm--;' \
	'^+.*hang_inject slot=%d seq=%u irq-masked' \
	'^+.*hang_inject=%u (IRQ masked at the next %u dispatches)' \
	'^+.*faultdbg ev_arm=%u ev_head=%u ev_dumped=%u serialize_fs=%u hang_arm=%u' \
	'^+.*debugfs_create_file("hang_inject"' \
	'^+.*debugfs_create_file("job_timeout_ms"'; do
	grep -q "$pat" "$DRV1" || gate_fail "driver 0001 mirror lost '$pat'"
done
grep -q '^+.*P7580_JOB_TIMEOUT_MS[[:space:]]*5000u' "$DRV1" \
	&& gate_fail "driver 0001 mirror still carries the 5000u timeout"
# (8) compiled proof: symbols, the seeded sched_init, the mask write.
aarch64-linux-gnu-nm "$JOB_O" | grep -q ' T p7580_job_set_timeout_hooks$' \
	|| gate_fail "p7580_job_set_timeout_hooks not T in job.o"
aarch64-linux-gnu-nm "$SUBMIT_O" | grep -q ' T p7580_drv_get_job_timeout_ms$' \
	|| gate_fail "p7580_drv_get_job_timeout_ms not T in submit.o"
aarch64-linux-gnu-nm "$SUBMIT_O" | grep -q ' T p7580_drv_set_job_timeout_ms$' \
	|| gate_fail "p7580_drv_set_job_timeout_ms not T in submit.o"
aarch64-linux-gnu-nm "$JOB_O" "$SUBMIT_O" \
	| grep -E ' (p7580_job_set_timeout_hooks|p7580_drv_(get|set)_job_timeout_ms)$' | tee -a "$EVID"
for s in ' T p7580_job_set_timeout_hooks$' ' T p7580_drv_get_job_timeout_ms$' \
	' T p7580_drv_set_job_timeout_ms$'; do
	[ "$(grep -c "$s" System.map)" = 1 ] || gate_fail "System.map $s count != 1"
done
grep -E ' (p7580_job_set_timeout_hooks|p7580_drv_(get|set)_job_timeout_ms)$' System.map | tee -a "$EVID"
aarch64-linux-gnu-nm "$SUBMIT_O" | grep -q ' d p7580_drv_job_timeout_ms$' \
	|| gate_fail "p7580_drv_job_timeout_ms is not a real .data seed in submit.o"
SED=$(aarch64-linux-gnu-objdump -dr --disassemble=p7580_drv_sched_ensure "$SUBMIT_O")
[ "$(printf '%s\n' "$SED" | grep -c 'R_AARCH64_CALL26[[:space:]]*msecs_to_jiffies')" = 1 ] \
	|| gate_fail "sched_ensure must convert the runtime budget exactly once"
[ "$(printf '%s\n' "$SED" | grep -c 'R_AARCH64_CALL26[[:space:]]*drm_sched_init')" = 1 ] \
	|| gate_fail "p7580_drv_sched_ensure must call drm_sched_init exactly once"
SE_MS=$(printf '%s\n' "$SED" | grep -n 'R_AARCH64_CALL26[[:space:]]*msecs_to_jiffies' | cut -d: -f1 | head -1)
SE_INIT=$(printf '%s\n' "$SED" | grep -n 'R_AARCH64_CALL26[[:space:]]*drm_sched_init' | cut -d: -f1 | head -1)
[ -n "$SE_MS" ] && [ -n "$SE_INIT" ] && [ "$SE_MS" -lt "$SE_INIT" ] \
	|| gate_fail "sched_ensure does not convert the seed before drm_sched_init (msecs=$SE_MS init=$SE_INIT)"
printf '%s\n' "$SED" | grep -E 'msecs_to_jiffies|drm_sched_init|ldr' | head -10 | tee -a "$EVID"
TSD=$(aarch64-linux-gnu-objdump -dr --disassemble=p7580_drv_set_job_timeout_ms "$SUBMIT_O")
printf '%s\n' "$TSD" | grep -qE 'sub[[:space:]]*w[0-9]+, w[0-9]+, #0x64' \
	|| gate_fail "timeout setter lost the 100 ms lower clamp"
printf '%s\n' "$TSD" | grep -qE 'mov[[:space:]]*w[0-9]+, #0xe9fc' \
	|| gate_fail "timeout setter lost the (60000-100) ms upper clamp"
printf '%s\n' "$TSD" | grep -qE 'mov[[:space:]]*w[0-9]+, #0xffffffea' \
	|| gate_fail "timeout setter lost the -EINVAL out-of-range return"
[ "$(printf '%s\n' "$TSD" | grep -c 'R_AARCH64_CALL26[[:space:]]*msecs_to_jiffies')" = 1 ] \
	|| gate_fail "timeout setter must convert the live budget exactly once"
printf '%s\n' "$TSD" | grep -q 'bl.*<mutex_lock>' || gate_fail "timeout setter does not take the sched lock"
printf '%s\n' "$TSD" | grep -q 'bl.*<mutex_unlock>' || gate_fail "timeout setter does not drop the sched lock"
printf '%s\n' "$TSD" | grep -E '#0x64|#0xe9fc|#0xffffffea|msecs_to_jiffies|mutex' | tee -a "$EVID"
HID=$(aarch64-linux-gnu-objdump -d --disassemble=p7580_job_hw_submit "$JOB_O")
printf '%s\n' "$HID" | grep -qE 'mov[[:space:]]*w[0-9]+, #0x10001' \
	|| gate_fail "job.o hw_submit lost P7580_MK_JS_MASK (0x10001)"
printf '%s\n' "$HID" | grep -qE 'bic[[:space:]]*w[0-9]+, w[0-9]+, w[0-9]+' \
	|| gate_fail "job.o hw_submit does not clear the injected slot bits"
printf '%s\n' "$HID" | grep -qE 'mov[[:space:]]*x[0-9]+, #0x1008' \
	|| gate_fail "job.o hw_submit lost the JOB_INT_MASK (0x1008) write"
HI_MOV=$(printf '%s\n' "$HID" | grep -nE -m1 'mov[[:space:]]*x[0-9]+, #0x1008' | cut -d: -f1)
HI_D=$((HI_MOV + 1))
printf '%s\n' "$HID" | sed -n "${HI_D},$((HI_D+4))p" | grep -qE 'str[[:space:]]*w[0-9]+, \[x[0-9]+\]' \
	|| gate_fail "JOB_INT_MASK offset is not followed by the mask store"
printf '%s\n' "$HID" | grep -E '#0x10001|#0x1008|bic|str' | head -10 | tee -a "$EVID"
for s in 'p7580_job: hang_inject slot=%d seq=%u irq-masked' \
	'p7580_job: hang_inject=%u (IRQ masked at the next %u dispatches)' \
	'p7580_sched: job_timeout_ms=%u (new entities)' \
	'faultdbg ev_arm=%u ev_head=%u ev_dumped=%u serialize_fs=%u hang_arm=%u'; do
	aarch64-linux-gnu-strings "$JOB_O" "$SUBMIT_O" | grep -qF "$s" \
		|| gate_fail "string missing from a compiled object: $s"
	aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -qF "$s" \
		|| gate_fail "string missing from Image: $s"
done
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -E 'hang_inject|job_timeout_ms|hang_arm=' | tee -a "$EVID"
for s in 'p7580_job: hang_inject slot=' 'p7580_job: hang_inject=' 'p7580_sched: job_timeout_ms=' \
	'hang_arm=%u'; do
	[ "$(aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -cF "$s")" = 1 ] \
		|| gate_fail "Image '$s' count != 1"
done
# host: job_test must pass (the 750 ms pin lives there) and the driver
# tests stay green with the new timeout constant.
( cd "$REPO/code/gpu31/jobs" && make check ) > "$EXT/logs/gpu31-int49-jobtest.log" 2>&1 \
	|| gate_fail "jobs host tests failed (see $EXT/logs/gpu31-int49-jobtest.log)"
grep -q 'job_test: all checks passed' "$EXT/logs/gpu31-int49-jobtest.log" \
	|| gate_fail "job_test did not pass"
cat "$EXT/logs/gpu31-int49-jobtest.log" >> "$EVID"
grep -q 'chainsplit_test: all checks passed' "$EXT/logs/gpu31-int49-hosttest.log" \
	|| gate_fail "chainsplit_test did not pass (see log)"
grep -q 'submit_fix_test: all checks passed' "$EXT/logs/gpu31-int49-hosttest.log" \
	|| gate_fail "submit_fix_test did not pass (see log)"
grep -q 'JOB_TIMEOUT_MS.*750u' "$REPO/code/gpu31/driver/submit_fix_test.c" \
	|| gate_fail "host submit_fix_test mirror is not 750 ms"
note "== int49 watchdog gate: PASS (P7580_JOB_TIMEOUT_MS 750u with no 5000u shadow; drm_sched_init seeded from p7580_drv_job_timeout_ms; job_timeout_ms 100..60000 clamps + live sched->timeout + later entities; hang_inject masks the slot DONE+ERR before START and spends one per dispatch, default 0, dev_rearm restores; 2 knobs + faultdbg hang_arm; probe wiring, mirror, T symbols in job.o/submit.o/System.map and Image strings; job_test + driver host tests green) =="

# 6ad. gpu31-storm58 gates (3104a90). Two invariants:
#   (1) BO exclusivity has ONE publisher: SUBMIT, inside df->serial_lock,
#       for the split's last_fence (so the last writer is the newest
#       submit in serial order); run_job publishes nothing and the
#       publish_excl scope is gone. The queue-time get_excl leg stays.
#   (2) The fault capture no longer reads desc[10] out of bounds: the
#       SFBD pointer is read at the chain walk for fragment nodes
#       (word 10) and jc_age comes from a bounded 64-entry per-file
#       ledger; EVRING/TIMEOUT report them and the PAGE dump covers the
#       whole 4 KiB with true offsets.
note "== int49 gate: single serial BO publish + fault capture =="
WSS=drivers/gpu/drm/panfrost/panfrost7580_submit.c
WSJ=drivers/gpu/drm/panfrost/panfrost7580_job.c
WSH=drivers/gpu/drm/panfrost/panfrost7580_job.h
WSD=drivers/gpu/drm/panfrost/panfrost7580_drv.h
# (1) exactly one publish, inside the submit serial section, before the
# ioctl returns, guarded by last_fence; nothing in run_job.
[ "$(grep -c 'dma_resv_add_excl_fence(' "$WSS")" = 1 ] \
	|| gate_fail "submit.c must carry exactly one add_excl site, got $(grep -c 'dma_resv_add_excl_fence(' "$WSS")"
PB_LN=$(grep -n 'dma_resv_add_excl_fence(' "$WSS" | cut -d: -f1 | head -1)
ES_LN=$(grep -n '^err_split:' "$WSS" | cut -d: -f1 | head -1)
SL_LN=$(grep -n 'mutex_lock(&df->serial_lock);' "$WSS" | cut -d: -f1 | awk -v p="$PB_LN" '$1 < p' | tail -1)
SU_LN=$(grep -n 'mutex_unlock(&df->serial_lock);' "$WSS" | cut -d: -f1 | awk -v p="$PB_LN" '$1 > p' | head -1)
[ -n "$PB_LN" ] && [ -n "$ES_LN" ] && [ -n "$SL_LN" ] && [ -n "$SU_LN" ] && \
	[ "$SL_LN" -lt "$PB_LN" ] && [ "$PB_LN" -lt "$SU_LN" ] && [ "$PB_LN" -lt "$ES_LN" ] \
	|| gate_fail "BO publish is not inside the submit serial_lock before err_split (lock=$SL_LN publish=$PB_LN unlock=$SU_LN err=$ES_LN)"
GU_LN=$(grep -n 'if (last_fence) {' "$WSS" | cut -d: -f1 | awk -v p="$PB_LN" '$1 < p' | tail -1)
QU_LN=$(grep -n 'if (queued) {' "$WSS" | cut -d: -f1 | awk -v p="$PB_LN" '$1 < p' | tail -1)
[ -n "$GU_LN" ] && [ -n "$QU_LN" ] && [ "$QU_LN" -lt "$GU_LN" ] && [ "$GU_LN" -lt "$PB_LN" ] \
	|| gate_fail "publish is not queued-and-last_fence guarded (queued=$QU_LN guard=$GU_LN publish=$PB_LN)"
printf '%s\n' "$(awk '/^static int p7580_drv_run_job/,/^}/' "$WSS")" | grep -q 'dma_resv_add_excl_fence' \
	&& gate_fail "run_job still publishes BO exclusivity (the async re-publish was the storm)"
for f in "$WSS" "$WSH" "$WSD"; do
	grep -q 'publish_excl' "$f" && gate_fail "$f still carries the publish_excl scope"
done
grep -q 'publish_excl' "$DRV1" && gate_fail "driver 0001 mirror still carries publish_excl"
grep -q 'dma_resv_get_excl' "$WSS" \
	|| gate_fail "queue-time get_excl ordering leg is gone from submit.c"
grep -n 'dma_resv_get_excl\|serial_lock\|dma_resv_add_excl_fence(' "$WSS" | tee -a "$EVID"
# (2) fault capture: SFBD read at the walk, jc_age ledger, full-page dump.
grep -q 'uint32_t fbd;' "$WSH" || gate_fail "job.h lost the captured fbd pointer"
grep -q 'uint32_t jc_age;' "$WSH" || gate_fail "job.h lost the jc_age field"
grep -qF 'p7580_job: EVRING begin head=%u n=%u status=0x%08x jc=0x%llx seq=%u src=0x%llx fbd=0x%x jc_age=%u' "$WSJ" \
	|| gate_fail "EVRING begin does not report fbd/jc_age"
grep -q 'job ? job->fbd : 0u,' "$WSJ" || gate_fail "EVRING does not pass fbd"
grep -q 'job ? job->jc_age : 0u);' "$WSJ" || gate_fail "EVRING does not pass jc_age"
grep -q 'if (job->fbd)' "$WSJ" || gate_fail "fbd page dump is not gated on the captured pointer"
grep -q '(uint64_t)job->fbd, "fbd");' "$WSJ" || gate_fail "fbd page dump does not use job->fbd"
grep -q 'job->desc\[10\]' "$WSJ" && gate_fail "job.c still reads desc[10] (out of bounds)"
grep -qF 'seq=%u jc_age=%u jc=0x%llx' "$WSJ" || gate_fail "TIMEOUT line does not report jc_age"
grep -q 'job->seqno, job->jc_age, (unsigned long long)job->jc,' "$WSJ" \
	|| gate_fail "TIMEOUT does not pass jc_age"
grep -q 'job->fbd = s->fbd ? s->fbd\[pos\] : 0;' "$WSS" \
	|| gate_fail "split node does not stamp fbd"
grep -q 'job->jc_age = s->age ? s->age\[i\] : 0;' "$WSS" \
	|| gate_fail "split node does not stamp jc_age"
grep -qE '^#define P7580_BO_AGE_SLOTS[[:space:]]+64u' "$WSD" \
	|| gate_fail "P7580_BO_AGE_SLOTS 64 missing in applied drv.h"
grep -q 'uint32_t submit_no;' "$WSD" || gate_fail "per-file submit_no missing in drv.h"
grep -q '} bo_age\[P7580_BO_AGE_SLOTS\];' "$WSD" || gate_fail "bounded bo_age ledger missing in drv.h"
[ "$(grep -c '^static uint32_t p7580_drv_bo_age_touch(struct p7580_drv_file \*df,$' "$WSS")" = 1 ] \
	|| gate_fail "p7580_drv_bo_age_touch definition count != 1"
AGEB=$(awk '/^static uint32_t p7580_drv_bo_age_touch/,/^}/' "$WSS")
printf '%s\n' "$AGEB" | grep -q '(P7580_BO_AGE_SLOTS - 1u));' || gate_fail "age index is not masked to the ledger size"
printf '%s\n' "$AGEB" | grep -q 'age = seq - df->bo_age\[s\].seq;' || gate_fail "age is not the SUBMIT delta"
printf '%s\n' "$AGEB" | grep -q 'df->bo_age\[s\].seq = seq;' || gate_fail "ledger does not record the newest user"
grep -q 'df->submit_no++;' "$WSS" || gate_fail "submit_no is not bumped per SUBMIT"
grep -q 'ages\[i\] = p7580_drv_bo_age_touch(' "$WSS" || gate_fail "per-BO ages are not stamped"
grep -q 'df, to_p7580_bo(objs\[i\])->iova,' "$WSS" || gate_fail "age key is not the BO iova"
grep -q 'df->submit_no);' "$WSS" || gate_fail "age stamp does not pass the SUBMIT number"
grep -q 'uint32_t fbds\[P7580_CHAIN_MAX_JOBS\];' "$WSS" || gate_fail "per-node fbd array missing"
grep -q 'uint32_t ages\[P7580_MAX_SUBMIT_BOS\];' "$WSS" || gate_fail "per-BO age array missing"
grep -q 'fbds\[nodes\] = 0;' "$WSS" || gate_fail "fbd default 0 missing"
grep -q 'if (((hdr\[nodes\]\[4\] >> 1) & 0x7fu) == 9u)' "$WSS" \
	|| gate_fail "fragment-node test missing at the fbd capture"
grep -q '(void)p7580_drv_gpu_read(drv, walk + 40u,' "$WSS" \
	|| gate_fail "fbd is not read from descriptor word 10 (walk + 40)"
grep -q '&fbds\[nodes\],' "$WSS" || gate_fail "fbd read destination missing"
grep -q 'sctx.fbd = fbds;' "$WSS" || gate_fail "split ctx does not receive fbds"
grep -q 'sctx.age = ages;' "$WSS" || gate_fail "split ctx does not receive ages"
grep -q 'p7580_job: PAGE %s slot%02u off=%03x' "$WSS" || gate_fail "PAGE line is not the fixed off= form"
grep -q '(unsigned int)(i / 4u), (unsigned int)(i \* 64u),' "$WSS" \
	|| gate_fail "PAGE line does not use the true slot/offset math"
printf '%s\n' "$(awk '/^int p7580_drv_desc_dump/,/^}/' "$WSS")" | grep -q 'i < P7580_PAGE_DUMP_LEN / 4u / 16u' \
	|| gate_fail "desc dump no longer walks the whole page"
printf '%s\n' "$(awk '/^int p7580_drv_desc_dump/,/^}/' "$WSS")" \
	| grep -q 'const uint32_t \*q = buf + i \* 16u;' \
	|| gate_fail "desc dump line base is not 16 words"
printf '%s\n' "$(awk '/^int p7580_drv_desc_dump/,/^}/' "$WSS")" | grep -qF 'off=%03x' \
	|| gate_fail "desc dump does not print the byte offset"
grep -n 'off=%03x\|bo_age\|jc_age\|job->fbd' "$WSS" | tee -a "$EVID"
# driver 0001 mirror of every new line.
for pat in '^+.*P7580_BO_AGE_SLOTS' \
	'^+.*uint32_t submit_no;' \
	'^+.*} bo_age\[P7580_BO_AGE_SLOTS\];' \
	'^+static uint32_t p7580_drv_bo_age_touch(struct p7580_drv_file \*df,' \
	'^+.*df->bo_age\[s\].seq = seq;' \
	'^+.*job->fbd = s->fbd ? s->fbd\[pos\] : 0;' \
	'^+.*job->jc_age = s->age ? s->age\[i\] : 0;' \
	'^+.*fbds\[nodes\] = 0;' \
	'^+.*walk + 40u,' \
	'^+.*sctx.fbd = fbds;' \
	'^+.*sctx.age = ages;' \
	'^+.*off=%03x' \
	'^+.*fbd=0x%x jc_age=%u' \
	'^+.*seq=%u jc_age=%u jc=0x%llx'; do
	grep -q "$pat" "$DRV1" || gate_fail "driver 0001 mirror lost '$pat'"
done
grep -q '^+.*dma_resv_add_excl_fence(' "$DRV1" \
	|| gate_fail "driver 0001 mirror lost the SUBMIT publish"
[ "$(grep -c '^+.*dma_resv_add_excl_fence(' "$DRV1")" = 1 ] \
	|| gate_fail "driver 0001 mirror add_excl count != 1"
# host T10 pins the publish-order rule.
FST=$REPO/code/gpu31/driver/submit_fix_test.c
grep -q 'run_t10();' "$FST" || gate_fail "host submit_fix_test has no T10 (publish order)"
grep -q 'static void run_t10(void)' "$FST" || gate_fail "host T10 model missing"
grep -q 'fake_add_excl(&resv, &draws);' "$FST" || gate_fail "host T10 does not model the replace semantics"
grep -q 'CHECK(resv.excl == &fs);' "$FST" || gate_fail "host T10 does not assert the newer fence wins"
grep -q 'submit_fix_test: all checks passed' "$EXT/logs/gpu31-int49-hosttest.log" \
	|| gate_fail "host submit_fix_test did not pass (see log)"
# compiled/Image proof of the new capture strings and the one publish.
for s in 'p7580_job: EVRING begin head=%u n=%u status=0x%08x jc=0x%llx seq=%u src=0x%llx fbd=0x%x jc_age=%u' \
	'p7580_job: TIMEOUT slot=%d js_status=0x%08x (%s) stat=0x%08x rawstat=0x%08x head=0x%llx tail=0x%llx seq=%u jc_age=%u jc=0x%llx' \
	'p7580_job: PAGE %s slot%02u off=%03x' ; do
	aarch64-linux-gnu-strings "$JOB_O" "$SUBMIT_O" | grep -qF "$s" \
		|| gate_fail "string missing from a compiled object: $s"
	aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -qF "$s" \
		|| gate_fail "string missing from Image: $s"
done
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -E 'fbd=0x|jc_age=|off=%03x' | tee -a "$EVID"
note "== int49 storm58 gate: PASS (one BO publish at SUBMIT inside serial_lock, queued+last_fence guarded, before err_split; no publish_excl and no run_job publish; get_excl ordering leg intact; SFBD captured at the walk (word 10, type 9) and jc_age from the 64-entry ledger, EVRING/TIMEOUT report both, desc[10] read gone; PAGE dump covers 4 KiB with slot=i/4 off=i*64; mirror + objects + Image + host T10) =="

# 6ae. gpu31-storm58 WAIT_BO gates (98640d5). dma_resv_wait_timeout()
# returns >0 (idle, jiffies left), 0 (budget ran out) and <0 (error).
# The handler must map 0 to -ETIMEDOUT -- otherwise a 0-jiffy poll, which
# is exactly what Mesa's BO-cache fetch issues, reports a BO owned by an
# in-flight submit as ready. One new return, inside
# panfrost_ioctl_wait_bo, after the p7580_gem_wait() call; <0 still
# propagates and >0 still succeeds.
note "== int49 gate: WAIT_BO 0-jiffy result mapped to -ETIMEDOUT =="
WIC=drivers/gpu/drm/panfrost/panfrost7580_ioctl.c
WIO=drivers/gpu/drm/panfrost/panfrost7580_ioctl.o
for f in "$WIC" "$WIO"; do
	[ -f "$f" ] || gate_fail "$f missing"
done
[ "$(grep -c 'return -ETIMEDOUT;' "$WIC")" = 1 ] \
	|| gate_fail "ioctl.c must carry exactly one 'return -ETIMEDOUT;', got $(grep -c 'return -ETIMEDOUT;' "$WIC")"
WB=$(awk '/^int panfrost_ioctl_wait_bo/,/^}/' "$WIC")
[ -n "$WB" ] || gate_fail "panfrost_ioctl_wait_bo body not found in applied ioctl.c"
printf '%s\n' "$WB" | grep -q 'timeout = p7580_gem_wait(dev, file_priv, args->handle, timeout, 1);' \
	|| gate_fail "wait_bo no longer calls p7580_gem_wait"
printf '%s\n' "$WB" | grep -q 'if (timeout == 0)' \
	|| gate_fail "wait_bo lost the timeout == 0 test"
printf '%s\n' "$WB" | grep -q 'if (timeout < 0)' \
	|| gate_fail "wait_bo lost the timeout < 0 propagation test"
printf '%s\n' "$WB" | grep -q 'return (int)timeout;' \
	|| gate_fail "wait_bo no longer propagates the negative result"
printf '%s\n' "$WB" | grep -q 'return 0;' \
	|| gate_fail "wait_bo lost the >0 success return"
WB_GW=$(printf '%s\n' "$WB" | grep -n 'timeout = p7580_gem_wait(' | cut -d: -f1 | head -1)
WB_Z=$(printf '%s\n' "$WB" | grep -n 'if (timeout == 0)' | cut -d: -f1 | head -1)
WB_T=$(printf '%s\n' "$WB" | grep -n 'return -ETIMEDOUT;' | cut -d: -f1 | head -1)
WB_L=$(printf '%s\n' "$WB" | grep -n 'if (timeout < 0)' | cut -d: -f1 | head -1)
[ -n "$WB_GW" ] && [ -n "$WB_Z" ] && [ -n "$WB_T" ] && [ -n "$WB_L" ] && \
	[ "$WB_GW" -lt "$WB_Z" ] && [ "$WB_Z" -lt "$WB_T" ] && [ "$WB_T" -lt "$WB_L" ] \
	|| gate_fail "ETIMEDOUT mapping is not gem_wait($WB_GW) -> ==0($WB_Z) -> return($WB_T) -> <0($WB_L)"
printf '%s\n' "$WB" | grep -E 'gem_wait|timeout == 0|ETIMEDOUT|timeout < 0|return \(int\)timeout|return 0' | tee -a "$EVID"
# the mapping must not leak into another handler's error path: the file
# has one ETIMEDOUT and the other handlers keep their own returns.
grep -n 'return -ETIMEDOUT;' "$WIC" | tee -a "$EVID"
# driver 0001 mirror.
grep -q '^+	if (timeout == 0)' "$DRV1" \
	|| gate_fail "driver 0001 mirror lost the timeout == 0 mapping"
[ "$(grep -A1 '^+	if (timeout == 0)' "$DRV1" | grep -c '^+		return -ETIMEDOUT;')" = 1 ] \
	|| gate_fail "driver 0001 mirror mapping is not 'if (timeout == 0) -> return -ETIMEDOUT'"
grep -q '^+.*return (int)timeout;' "$DRV1" \
	|| gate_fail "driver 0001 mirror lost the negative propagation"
# compiled proof: -ETIMEDOUT (-110 = 0xffffff92) in the wait_bo handler,
# on the path that follows the p7580_gem_wait call.
WID=$(aarch64-linux-gnu-objdump -d --disassemble=panfrost_ioctl_wait_bo "$WIO")
printf '%s\n' "$WID" | grep -q 'bl.*<p7580_gem_wait>' \
	|| gate_fail "ioctl.o wait_bo does not call p7580_gem_wait"
printf '%s\n' "$WID" | grep -qE 'mov[[:space:]]*w[0-9]+, #0xffffff92' \
	|| gate_fail "ioctl.o wait_bo has no -ETIMEDOUT (-110) return"
WID_W=$(printf '%s\n' "$WID" | grep -n 'bl.*<p7580_gem_wait>' | cut -d: -f1 | head -1)
WID_T=$(printf '%s\n' "$WID" | grep -nE 'mov[[:space:]]*w[0-9]+, #0xffffff92' | cut -d: -f1 | head -1)
[ -n "$WID_W" ] && [ -n "$WID_T" ] && [ "$WID_W" -lt "$WID_T" ] \
	|| gate_fail "compiled -ETIMEDOUT is not after the gem_wait call (wait=$WID_W ret=$WID_T)"
printf '%s\n' "$WID" | grep -E 'bl.*p7580_gem_wait|#0xffffff92|cbnz|cbz' | tee -a "$EVID"
# host T11 pins the three-way mapping.
grep -q 'run_t11();' "$FST" || gate_fail "host submit_fix_test has no T11 (WAIT_BO mapping)"
grep -q 'static int model_wait_bo_ioctl(long wait_ret)' "$FST" \
	|| gate_fail "host T11 model missing"
grep -q 'CHECK(model_wait_bo_ioctl(0) == -ETIMEDOUT);' "$FST" \
	|| gate_fail "host T11 does not pin 0 -> -ETIMEDOUT"
grep -q 'CHECK(model_wait_bo_ioctl(1) == 0);' "$FST" \
	|| gate_fail "host T11 does not pin >0 -> success"
grep -q 'CHECK(model_wait_bo_ioctl(-EINTR) == -EINTR);' "$FST" \
	|| gate_fail "host T11 does not pin <0 propagation"
grep -q 'submit_fix_test: all checks passed' "$EXT/logs/gpu31-int49-hosttest.log" \
	|| gate_fail "host submit_fix_test did not pass (see log)"
note "== int49 WAIT_BO gate: PASS (exactly one return -ETIMEDOUT; in panfrost_ioctl_wait_bo, after the p7580_gem_wait call and before the <0 propagation; >0 still succeeds; mirror + ioctl.o -110 immediate + host T11) =="

# 6af. gpu31-boleak gates (8a493b5). The bo_list entry is NON-OWNING:
#   (1) map_bo links it without drm_gem_object_reference(), and neither
#       gpu_unmap nor the main.c remove walker unrefs it. The old
#       list-owned reference could never be dropped (its only unlink runs
#       from free_object at refcount zero), so every submitted BO leaked.
#   (2) walkers that need the object take kref_get_unless_zero through
#       p7580_drv_gem_try_get(), under bo_lock, so a dying object (entry
#       still linked, refcount 0) is skipped instead of resurrected.
#   (3) the remaining drm_gem_object_reference() sites are exactly the two
#       owners: the shadow's per-node refs and the job->bos copies.
note "== int49 gate: weak bo_list entry + try_get =="
BLD=drivers/gpu/drm/panfrost/panfrost7580_drv.h
BLS=drivers/gpu/drm/panfrost/panfrost7580_submit.c
BLM=drivers/gpu/drm/panfrost/panfrost7580_main.c
for f in "$BLD" "$BLS" "$BLM"; do
	[ -f "$f" ] || gate_fail "$f missing"
done
# (1) weak entry at link and unlink.
grep -q 'struct drm_gem_object \*obj;' "$BLD" \
	&& grep -q 'weak: valid while linked' "$BLD" \
	|| gate_fail "drv.h bo_list entry is not the documented weak form"
MAPB=$(awk '/^static int p7580_drv_map_bo_body/,/^}/' "$BLS")
[ -n "$MAPB" ] || gate_fail "p7580_drv_map_bo body not found"
printf '%s\n' "$MAPB" | grep -q 'drm_gem_object_reference(obj);' \
	&& gate_fail "map_bo still takes a GEM reference for the bo_list entry (the leak)"
printf '%s\n' "$MAPB" | grep -q 'e->obj = obj;' \
	|| gate_fail "map_bo does not record the object in the entry"
printf '%s\n' "$MAPB" | grep -q 'list_add_tail(&e->node, &drv->bo_list);' \
	|| gate_fail "map_bo no longer links the binding"
GUB=$(awk '/^void p7580_gem_gpu_unmap/,/^}/' "$BLS")
[ -n "$GUB" ] || gate_fail "p7580_gem_gpu_unmap body not found"
printf '%s\n' "$GUB" | grep -q 'drm_gem_object_unreference' \
	&& gate_fail "gpu_unmap still drops a bo_list reference"
printf '%s\n' "$GUB" | grep -q 'list_del(&e->node);' \
	|| gate_fail "gpu_unmap lost the entry unlink (nothing would ever free it)"
printf '%s\n' "$GUB" | grep -q 'kfree(e);' \
	|| gate_fail "gpu_unmap lost the entry free"
grep -q 'drm_gem_object_unreference' "$BLM" \
	&& gate_fail "main.c still drops a bo_list reference"
for s in 'p7580_drv_unmap_bo(drv, e->obj, pm_held);' 'list_del(&e->node);' 'kfree(e);'; do
	grep -qF "$s" "$BLM" || gate_fail "main.c remove walker lost '$s'"
done
REM_U=$(grep -n 'p7580_drv_unmap_bo(drv, e->obj, pm_held);' "$BLM" | cut -d: -f1 | head -1)
REM_D=$(grep -n 'list_del(&e->node);' "$BLM" | cut -d: -f1 | head -1)
REM_K=$(grep -n 'kfree(e);' "$BLM" | cut -d: -f1 | head -1)
[ -n "$REM_U" ] && [ -n "$REM_D" ] && [ -n "$REM_K" ] && \
	[ "$REM_U" -lt "$REM_D" ] && [ "$REM_D" -lt "$REM_K" ] \
	|| gate_fail "remove walker order is not unmap($REM_U) -> unlink($REM_D) -> free($REM_K)"
printf '%s\n' "$MAPB" | grep -E 'e->obj|list_add_tail|reference' | tee -a "$EVID"
printf '%s\n' "$GUB" | grep -E 'list_del|kfree|unreference' | tee -a "$EVID"
# (2) try_get: one definition, two uses, both under bo_lock.
[ "$(grep -c '^static int p7580_drv_gem_try_get(struct drm_gem_object \*obj)$' "$BLS")" = 1 ] \
	|| gate_fail "p7580_drv_gem_try_get definition count != 1"
TGB=$(awk '/^static int p7580_drv_gem_try_get/,/^}/' "$BLS")
printf '%s\n' "$TGB" | grep -q 'if (!obj)' || gate_fail "try_get lost the NULL guard"
printf '%s\n' "$TGB" | grep -q 'return 0;' || gate_fail "try_get lost the miss return"
printf '%s\n' "$TGB" | grep -q 'return kref_get_unless_zero(&obj->refcount);' \
	|| gate_fail "try_get is not kref_get_unless_zero"
[ "$(grep -c 'p7580_drv_gem_try_get(' "$BLS")" = 3 ] \
	|| gate_fail "try_get must appear exactly 3 times (definition + gpu_span + ptewalk), got $(grep -c 'p7580_drv_gem_try_get(' "$BLS")"
[ "$(grep -c 'kref_get_unless_zero(&obj->refcount)' "$BLS")" = 1 ] \
	|| gate_fail "kref_get_unless_zero(&obj->refcount) must be the only resurrection point"
printf '%s\n' "$MAPB" | grep -q 'p7580_drv_gem_try_get' \
	&& gate_fail "map_bo uses try_get (it already holds the caller's reference)"
GSB=$(awk '/^static int p7580_drv_gpu_span/,/^}/' "$BLS")
[ -n "$GSB" ] || gate_fail "p7580_drv_gpu_span body not found"
printf '%s\n' "$GSB" | grep -q 'mutex_lock(&drv->bo_lock);' || gate_fail "gpu_span lost bo_lock"
printf '%s\n' "$GSB" | grep -q 'if (!p7580_drv_gem_try_get(e->obj))' \
	|| gate_fail "gpu_span does not try_get the entry object"
printf '%s\n' "$GSB" | grep -q 'mutex_unlock(&drv->bo_lock);' || gate_fail "gpu_span lost the bo_lock drop"
GS_T=$(printf '%s\n' "$GSB" | grep -n 'p7580_drv_gem_try_get(e->obj)' | cut -d: -f1 | head -1)
GS_L=$(printf '%s\n' "$GSB" | grep -n 'mutex_lock(&drv->bo_lock);' | cut -d: -f1 | head -1)
GS_U=$(printf '%s\n' "$GSB" | grep -n 'mutex_unlock(&drv->bo_lock);' | cut -d: -f1 | head -1)
[ -n "$GS_T" ] && [ -n "$GS_L" ] && [ -n "$GS_U" ] && [ "$GS_L" -lt "$GS_T" ] && [ "$GS_T" -lt "$GS_U" ] \
	|| gate_fail "gpu_span try_get is not inside bo_lock (lock=$GS_L try=$GS_T unlock=$GS_U)"
printf '%s\n' "$TGB" "$GSB" | tee -a "$EVID"
# (3) the only remaining reference sites are the shadow and job->bos.
[ "$(grep -cE 'drm_gem_object_reference\([a-z]' "$BLS")" = 2 ] \
	|| gate_fail "submit.c must take GEM references at exactly 2 places (shadow nodes, job->bos), got $(grep -cE 'drm_gem_object_reference\([a-z]' "$BLS")"
grep -q 'for (i = 1; i < nodes; i++)' "$BLS" || gate_fail "shadow per-node reference loop missing"
grep -q 'drm_gem_object_reference(obj);' "$BLS" || gate_fail "shadow object reference missing"
grep -q 'drm_gem_object_reference(s->objs\[i\]);' "$BLS" || gate_fail "job->bos reference missing"
grep -n 'drm_gem_object_reference(' "$BLS" | tee -a "$EVID"
# (4) a failed bind rolls back the leaves it programmed: the invariant is
# "leaves programmed <=> bo_list entry linked", because the free-time
# unmap keys on the entry. All three map failures jump to rollback, which
# unmaps 0..i-1, publishes the clear with an invalidate, and returns the
# error -- so a freed BO can never leave PTEs naming recycled pages.
printf '%s\n' "$MAPB" | grep -q '^	int ret = 0;$' \
	|| gate_fail "map_bo lost the shared ret initialiser"
[ "$(printf '%s\n' "$MAPB" | grep -c 'goto rollback;')" = 3 ] \
	|| gate_fail "map_bo must jump to rollback from all three map failures (page-phys, mmu_map, entry alloc), got $(printf '%s\n' "$MAPB" | grep -c 'goto rollback;')"
printf '%s\n' "$MAPB" | grep -q '^rollback:$' || gate_fail "map_bo lost the rollback label"
printf '%s\n' "$MAPB" | grep -q 'return -EFAULT;' \
	&& gate_fail "map_bo still returns -EFAULT with leaves programmed"
printf '%s\n' "$MAPB" | grep -q 'return -ENOMEM;' \
	&& gate_fail "map_bo still returns -ENOMEM with leaves programmed"
RBB=$(printf '%s\n' "$MAPB" | sed -n '/^rollback:/,$p')
printf '%s\n' "$RBB" | grep -q 'while (k--)' || gate_fail "rollback does not walk 0..i-1"
printf '%s\n' "$RBB" | grep -q 'p7580_mmu_unmap(&drv->mmu, bo->iova +' \
	|| gate_fail "rollback does not unmap the programmed leaves"
printf '%s\n' "$RBB" | grep -q 'p7580_mmu_invalidate(drv->regs, P7580_DRV_AS_NR,' \
	|| gate_fail "rollback does not publish the cleared leaves"
printf '%s\n' "$RBB" | grep -q 'return ret;' || gate_fail "rollback does not return the map error"
RBB_RET=$(printf '%s\n' "$RBB" | grep -n 'return ret;' | cut -d: -f1 | tail -1)
RBB_N=$(printf '%s\n' "$RBB" | wc -l)
[ -n "$RBB_RET" ] && [ $((RBB_N - RBB_RET)) -le 1 ] \
	|| gate_fail "rollback does not end in return ret (return at $RBB_RET of $RBB_N)"
RBB_I=$(printf '%s\n' "$RBB" | grep -n 'while (k--)' | cut -d: -f1 | head -1)
RBB_U=$(printf '%s\n' "$RBB" | grep -n 'p7580_mmu_unmap(&drv->mmu, bo->iova +' | cut -d: -f1 | head -1)
RBB_P=$(printf '%s\n' "$RBB" | grep -n 'p7580_mmu_invalidate(drv->regs' | cut -d: -f1 | head -1)
[ -n "$RBB_I" ] && [ -n "$RBB_U" ] && [ -n "$RBB_P" ] && \
	[ "$RBB_I" -lt "$RBB_U" ] && [ "$RBB_U" -lt "$RBB_P" ] \
	|| gate_fail "rollback order is not walk($RBB_I) -> unmap($RBB_U) -> publish($RBB_P)"
printf '%s\n' "$RBB" | tee -a "$EVID"
# driver 0001 mirror of every new line.
for pat in '^+.*weak: valid while linked' \
	'^+static int p7580_drv_gem_try_get(struct drm_gem_object \*obj)$' \
	'^+	return kref_get_unless_zero(&obj->refcount);' \
	'^+.*if (!p7580_drv_gem_try_get(e->obj))' \
	'^+.*if (!p7580_drv_gem_try_get(le->obj))' \
	'^+.*list_del(&e->node);' \
	'^+.*gpu31-boleak: NON-OWNING entry' \
	'^+rollback:' \
	'^+.*while (k--)' \
	'^+.*p7580_mmu_unmap(&drv->mmu, bo->iova +'; do
	grep -q "$pat" "$DRV1" || gate_fail "driver 0001 mirror lost '$pat'"
done
[ "$(grep -c '^+.*goto rollback;' "$DRV1")" = 3 ] \
	|| gate_fail "driver 0001 mirror must carry the 3 rollback jumps"
# compiled: try_get is static and may be inlined; the decisive object
# evidence is that map_bo/gpu_unmap carry no kref/drm ref call and that
# the two owner sites still do.
TGM=$(aarch64-linux-gnu-nm "$SUBMIT_O" | grep -c ' [tT] p7580_drv_gem_try_get$')
note "submit.o p7580_drv_gem_try_get outlined: $TGM"
if [ "$TGM" = 1 ]; then
	aarch64-linux-gnu-nm "$SUBMIT_O" | grep ' p7580_drv_gem_try_get$' | tee -a "$EVID"
fi
grep -n 'p7580_drv_gem_try_get\|drm_gem_object_reference' "$BLS" | tee -a "$EVID"
note "== int49 boleak gate: PASS (map_bo links a weak entry with no GEM ref; gpu_unmap unlinks+frees without a put; remove walker unmaps then unlinks then frees with no put; try_get=kref_get_unless_zero is the only resurrection point and both walkers use it under bo_lock; the remaining references are exactly the shadow nodes and job->bos; mirror + objects) =="

# 6ag. gpu31-mmlock gates (int49). The 3.10 core fake-mmap-offset manager
# (dev->mm_private: bare drm_mm + bare drm_ht, no locking) is out of our
# paths entirely; the driver owns the space:
#   - p7580_dev.offset_mm (page-index window P7580_OFFSET_START/SIZE --
#     the same range the core used) + offset_hash (drm_open_hash) +
#     mmap_offset_lock; p7580_bo.offset_node/offset_hash/offset_valid;
#   - p7580_offset_alloc: search + insert + hash insert in one critical
#     section (no half-built entry visible, no double-take of a hole);
#   - p7580_offset_free: hash remove + node remove + valid=0 under the
#     same lock (what makes a concurrent .mmap miss instead of
#     resurrecting a dying object);
#   - p7580_offset_lookup: drm_ht_find_item + kref_get_unless_zero under
#     the lock; .mmap uses it and never drm_gem_mmap();
#   - MMAP_BO reads offset_node.start under the lock, -ENOENT if invalid;
#   - import fails closed when dev_private is NULL.
# gem.o is the decisive proof: no U refs to the core manager/mmap, and
# the drm_mm/drm_ht primitives the owner manager needs are all present.
note "== int49 gate: driver-owned serialised mmap-offset manager =="
MOF=drivers/gpu/drm/panfrost/panfrost7580_gem.c
MOH=drivers/gpu/drm/panfrost/panfrost7580_gem.h
MOO=drivers/gpu/drm/panfrost/panfrost7580_gem.o
MIC=drivers/gpu/drm/panfrost/panfrost7580_ioctl.c
MIO=drivers/gpu/drm/panfrost/panfrost7580_ioctl.o
for f in "$MOF" "$MOH" "$MOO" "$MIC" "$MIO"; do
	[ -f "$f" ] || gate_fail "$f missing"
done
# (1) state + window.
grep -q 'struct drm_mm offset_mm;' "$MOH" || gate_fail "gem.h lost offset_mm"
grep -q 'struct drm_open_hash offset_hash;' "$MOH" || gate_fail "gem.h lost offset_hash"
grep -q 'struct mutex mmap_offset_lock;' "$MOH" || gate_fail "gem.h lost mmap_offset_lock"
grep -q 'struct drm_mm_node offset_node;' "$MOH" || gate_fail "gem.h lost per-BO offset_node"
grep -q 'struct drm_hash_item offset_hash;' "$MOH" || gate_fail "gem.h lost per-BO offset_hash"
grep -q 'int offset_valid;' "$MOH" || gate_fail "gem.h lost offset_valid"
grep -qE '^#define P7580_OFFSET_START' "$MOF" || gate_fail "offset window start macro missing"
grep -qE '^#define P7580_OFFSET_SIZE' "$MOF" || gate_fail "offset window size macro missing"
# (2) init/fini.
grep -q 'drm_ht_create(&gd->offset_hash, 12)' "$MOF" || gate_fail "offset hash not created in dev_init"
grep -q 'drm_mm_init(&gd->offset_mm, P7580_OFFSET_START,' "$MOF" || gate_fail "offset_mm not initialised in dev_init"
grep -q 'drm_mm_takedown(&gd->offset_mm);' "$MOF" || gate_fail "offset_mm not torn down in dev_fini"
grep -q 'drm_ht_remove(&gd->offset_hash);' "$MOF" || gate_fail "offset hash not removed in dev_fini"
grep -q 'mutex_init(&gd->mmap_offset_lock);' "$MOF" || gate_fail "mmap_offset_lock not initialised"
# (3) helpers: one definition each, all manager ops inside the lock.
for fn in p7580_offset_alloc p7580_offset_free p7580_offset_lookup; do
	[ "$(grep -c "^static .*${fn}(" "$MOF")" = 1 ] || gate_fail "${fn} definition count != 1"
done
ALB=$(awk '/^static int p7580_offset_alloc/,/^}/' "$MOF")
FRB=$(awk '/^static void p7580_offset_free/,/^}/' "$MOF")
LKB=$(awk '/^static struct drm_gem_object \*p7580_offset_lookup/,/^}/' "$MOF")
[ -n "$ALB" ] && [ -n "$FRB" ] && [ -n "$LKB" ] || gate_fail "offset manager helper body missing"
for c in 'drm_mm_search_free(&gd->offset_mm,' 'drm_mm_insert_node(&gd->offset_mm,' 'drm_ht_insert_item(&gd->offset_hash,' 'bo->offset_valid = 1;'; do
	printf '%s\n' "$ALB" | grep -qF "$c" || gate_fail "alloc lost '$c'"
done
for c in 'drm_ht_remove_item(&gd->offset_hash,' 'drm_mm_remove_node(&bo->offset_node);' 'bo->offset_valid = 0;'; do
	printf '%s\n' "$FRB" | grep -qF "$c" || gate_fail "free lost '$c'"
done
for c in 'drm_ht_find_item(&gd->offset_hash,' 'drm_hash_entry(item, struct p7580_bo, offset_hash)' 'kref_get_unless_zero(&bo->base.refcount)'; do
	printf '%s\n' "$LKB" | grep -qF "$c" || gate_fail "lookup lost '$c'"
done
for b in ALB FRB LKB; do
	eval "BODY=\$$b"
	[ "$(printf '%s\n' "$BODY" | grep -c 'mutex_lock(&gd->mmap_offset_lock);')" = 1 ] \
		|| gate_fail "$b must take mmap_offset_lock exactly once"
	[ "$(printf '%s\n' "$BODY" | grep -c 'mutex_unlock(&gd->mmap_offset_lock);')" = 1 ] \
		|| gate_fail "$b must drop mmap_offset_lock exactly once"
	L1=$(printf '%s\n' "$BODY" | grep -n 'mutex_lock(&gd->mmap_offset_lock);' | cut -d: -f1 | head -1)
	L2=$(printf '%s\n' "$BODY" | grep -n 'mutex_unlock(&gd->mmap_offset_lock);' | cut -d: -f1 | head -1)
	[ -n "$L1" ] && [ -n "$L2" ] && [ "$L1" -lt "$L2" ] || gate_fail "$b lock is not before unlock"
done
printf '%s\n' "$ALB" "$FRB" "$LKB" | tee -a "$EVID"
# (4) call sites: create/import alloc, three frees, no core manager use.
[ "$(grep -c 'p7580_offset_alloc(gd, bo)' "$MOF")" = 2 ] \
	|| gate_fail "expected 2 p7580_offset_alloc sites (create + import), got $(grep -c 'p7580_offset_alloc(gd, bo)' "$MOF")"
[ "$(grep -c 'p7580_offset_free(gd, bo)' "$MOF")" = 3 ] \
	|| gate_fail "expected 3 p7580_offset_free sites (create err, import err, free_object), got $(grep -c 'p7580_offset_free(gd, bo)' "$MOF")"
grep -q 'if (bo->offset_valid) {' "$MOF" || gate_fail "free_object does not gate on offset_valid"
grep -q 'map_list' "$MOF" && gate_fail "gem.c still touches base.map_list (core offset manager)"
# (5) .mmap: driver lookup, no core mmap, lookup before the struct_mutex
# section that registers the VMA reference.
MMS=$(awk '/^int p7580_gem_mmap/,/^}/' "$MOF")
printf '%s\n' "$MMS" | grep -q 'obj = p7580_offset_lookup(gd, vma->vm_pgoff);' \
	|| gate_fail "mmap does not resolve through p7580_offset_lookup"
printf '%s\n' "$MMS" | grep -q 'return drm_mmap(filp, vma);' \
	|| gate_fail "mmap lost the legacy-offset fallback"
printf '%s\n' "$MMS" | grep -q 'drm_gem_mmap(filp' && gate_fail "mmap still calls core drm_gem_mmap()"
printf '%s\n' "$MMS" | grep -q 'kref_get_unless_zero(' && gate_fail "mmap takes the reference outside the locked lookup"
LKP=$(printf '%s\n' "$MMS" | grep -n 'p7580_offset_lookup(gd, vma->vm_pgoff)' | cut -d: -f1 | head -1)
LKM=$(printf '%s\n' "$MMS" | grep -n 'mutex_lock(&dev->struct_mutex);' | cut -d: -f1 | head -1)
[ -n "$LKP" ] && [ -n "$LKM" ] && [ "$LKP" -lt "$LKM" ] \
	|| gate_fail "mmap lookup is not before the struct_mutex VMA registration (lookup=$LKP lock=$LKM)"
printf '%s\n' "$MMS" | grep -E 'p7580_offset_lookup|drm_mmap\(|struct_mutex|drm_vm_open_locked' | tee -a "$EVID"
# (6) MMAP_BO: lock-held offset read, -ENOENT when invalid.
IOB=$(awk '/^int panfrost_ioctl_mmap_bo/,/^}/' "$MIC")
[ -n "$IOB" ] || gate_fail "panfrost_ioctl_mmap_bo body not found"
for c in 'mutex_lock(&gd->mmap_offset_lock);' 'if (bo->offset_valid)' \
	'fake_offset = (uint64_t)bo->offset_node.start << PAGE_SHIFT;' \
	'mutex_unlock(&gd->mmap_offset_lock);' 'if (!fake_offset)' 'args->offset = fake_offset;'; do
	printf '%s\n' "$IOB" | grep -qF "$c" || gate_fail "mmap_bo lost '$c'"
done
printf '%s\n' "$IOB" | grep -q 'return -ENOENT;' || gate_fail "mmap_bo lost the -ENOENT for an invalid offset"
IO_L=$(printf '%s\n' "$IOB" | grep -n 'mutex_lock(&gd->mmap_offset_lock);' | cut -d: -f1 | head -1)
IO_R=$(printf '%s\n' "$IOB" | grep -n 'bo->offset_node.start' | cut -d: -f1 | head -1)
IO_U=$(printf '%s\n' "$IOB" | grep -n 'mutex_unlock(&gd->mmap_offset_lock);' | cut -d: -f1 | head -1)
[ -n "$IO_L" ] && [ -n "$IO_R" ] && [ -n "$IO_U" ] && [ "$IO_L" -lt "$IO_R" ] && [ "$IO_R" -lt "$IO_U" ] \
	|| gate_fail "mmap_bo offset read is not inside the lock (lock=$IO_L read=$IO_R unlock=$IO_U)"
printf '%s\n' "$IOB" | tee -a "$EVID"
# (7) import fails closed without dev_private.
IMPB=$(awk '/^struct drm_gem_object \*p7580_gem_prime_import_sg_table/,/^}/' "$MOF")
[ -n "$IMPB" ] || gate_fail "import_sg_table body not found"
printf '%s\n' "$IMPB" | grep -q 'return ERR_PTR(-ENODEV);' \
	|| gate_fail "import does not fail closed without dev_private"
printf '%s\n' "$IMPB" | grep -q 'p7580_offset_alloc(gd, bo)' \
	|| gate_fail "import does not use the driver offset manager"
# (8) driver 0001 mirror.
for pat in '^+.*struct drm_mm offset_mm;' \
	'^+.*struct drm_open_hash offset_hash;' \
	'^+.*struct mutex mmap_offset_lock;' \
	'^+.*struct drm_mm_node offset_node;' \
	'^+.*struct drm_hash_item offset_hash;' \
	'^+.*int offset_valid;' \
	'^+#define P7580_OFFSET_START' \
	'^+#define P7580_OFFSET_SIZE' \
	'^+static int p7580_offset_alloc(struct p7580_dev \*gd, struct p7580_bo \*bo)$' \
	'^+static void p7580_offset_free(struct p7580_dev \*gd, struct p7580_bo \*bo)$' \
	'^+static struct drm_gem_object \*p7580_offset_lookup(struct p7580_dev \*gd,$' \
	'^+.*kref_get_unless_zero(&bo->base.refcount))' \
	'^+.*obj = p7580_offset_lookup(gd, vma->vm_pgoff);' \
	'^+.*return drm_mmap(filp, vma);' \
	'^+.*fake_offset = (uint64_t)bo->offset_node.start << PAGE_SHIFT;' \
	'^+.*return ERR_PTR(-ENODEV);'; do
	grep -q "$pat" "$DRV1" || gate_fail "driver 0001 mirror lost '$pat'"
done
# comment-stripped applied file: no statement may call the core manager
# or drm_gem_mmap() (comments *name* them, which is documentation).
NOCOMM=$(awk '{ l=$0; sub(/^[[:space:]]*/, "", l); if (l ~ /^(\*|\/\*)/) next; print }' "$MOF")
printf '%s\n' "$NOCOMM" | grep -q 'drm_gem_create_mmap_offset(' \
	&& gate_fail "applied gem.c still calls core drm_gem_create_mmap_offset()"
printf '%s\n' "$NOCOMM" | grep -q 'drm_gem_free_mmap_offset(' \
	&& gate_fail "applied gem.c still calls core drm_gem_free_mmap_offset()"
printf '%s\n' "$NOCOMM" | grep -q 'drm_gem_mmap(filp' \
	&& gate_fail "applied gem.c still calls core drm_gem_mmap()"
# (9) compiled proof: core manager/mmap absent, owner primitives present.
for sym in drm_gem_create_mmap_offset drm_gem_free_mmap_offset drm_gem_mmap; do
	aarch64-linux-gnu-nm "$MOO" | grep -q " U ${sym}\$" \
		&& gate_fail "gem.o still references the core ${sym}"
done
for sym in drm_mm_search_free_generic drm_mm_insert_node drm_mm_remove_node \
	drm_ht_insert_item drm_ht_find_item drm_ht_remove_item; do
	aarch64-linux-gnu-nm "$MOO" | grep -q " U ${sym}\$" \
		|| gate_fail "gem.o does not call ${sym} (driver-owned offset manager)"
done
aarch64-linux-gnu-nm "$MOO" | grep -q ' U drm_mmap$' \
	|| gate_fail "gem.o lost the legacy drm_mmap fallback for unknown offsets"
aarch64-linux-gnu-nm "$MOO" | grep -E 'drm_(mm|ht|open_hash|mmap)|p7580_offset' | tee -a "$EVID"
OUTL=$(aarch64-linux-gnu-nm "$MOO" | grep -cE ' [tT] p7580_offset_(alloc|free|lookup)$')
note "gem.o outlined offset helpers: $OUTL/3 (rest inlined; U-symbol gates above are the decisive proof)"
note "== int49 mmapoffset gate: PASS (driver-owned offset_mm/offset_hash/lock; alloc=search+insert+hash under one lock, free=hash+node remove, lookup=drm_ht_find_item+kref_get_unless_zero; create/import/free_object/MMAP_BO all on the owner manager, mmap_bo -ENOENT on invalid, import -ENODEV without dev_private; .mmap resolves via lookup with the core drm_mmap legacy fallback only; gem.o has zero core-manager refs and all six drm_mm/drm_ht relocs; mirror) =="

# 6ah. gpu31-fpssplit gates (5950b7e). dma_fence_wait_timeout() short-
# circuited on an already-signaled fence and returned the bare budget, so
# the 0-jiffy poll Mesa's BO-cache fetch issues returned 0 -- which the
# 0002 resv shim cannot tell apart from "budget ran out", and which
# int44's WAIT_BO correctly maps to -ETIMEDOUT. Every poll of an IDLE BO
# was answered busy; the client allocated a fresh BO set per frame.
# The fix is the mainline default_wait contract "timeout ? timeout : 1"
# in three places: default_wait's initialiser + signaled early exit,
# wait_timeout's signaled return, and the resv remainder (which an empty
# resv keeps; a waited fence set still reports 0 on timeout).
note "== int49 gate: 0-jiffy poll of a signaled fence returns >0 =="
DAF=drivers/base/dma-fence.c
DAR=drivers/base/dma-resv.c
DAFO=drivers/base/dma-fence.o
DARO=drivers/base/dma-resv.o
SDM=$REPO/code/gpu31/sched/src/dma-fence.c
SRM=$REPO/code/gpu31/sched/src/dma-resv.c
for f in "$DAF" "$DAR" "$DAFO" "$DARO"; do
	[ -f "$f" ] || gate_fail "applied $f missing"
done
# (1) 0001 applied source: initialiser, two signaled tests, both returns.
[ "$(grep -c 'signed long ret = timeout ? timeout : 1;' "$DAF")" = 1 ] \
	|| gate_fail "default_wait initialiser is not 'timeout ? timeout : 1'"
[ "$(grep -c 'test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &fence->flags)' "$DAF")" = 2 ] \
	|| gate_fail "expected the signaled test in default_wait AND wait_timeout, got $(grep -c 'test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &fence->flags)' "$DAF")"
[ "$(grep -c 'return timeout ? timeout : 1;' "$DAF")" = 1 ] \
	|| gate_fail "wait_timeout does not return a positive remainder for a signaled fence"
[ "$(grep -c 'return timeout;' "$DAF")" = 0 ] \
	|| gate_fail "dma-fence.c still returns the bare budget (the pre-fix form)"
DFW=$(awk '/^signed long dma_fence_default_wait/,/^}/' "$DAF")
[ -n "$DFW" ] || gate_fail "dma_fence_default_wait body not found"
printf '%s\n' "$DFW" | grep -q 'if (test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &fence->flags))' \
	|| gate_fail "default_wait lost the signaled early exit"
printf '%s\n' "$DFW" | grep -q 'return ret;' \
	|| gate_fail "default_wait signaled exit does not return the positive remainder"
DFT=$(printf '%s\n' "$DFW" | grep -n 'test_bit(DMA_FENCE_FLAG_SIGNALED_BIT' | cut -d: -f1 | head -1)
DFR=$(printf '%s\n' "$DFW" | grep -n 'return ret;' | cut -d: -f1 | head -1)
[ -n "$DFT" ] && [ -n "$DFR" ] && [ "$DFT" -lt "$DFR" ] \
	|| gate_fail "default_wait signaled test is not before its return (test=$DFT ret=$DFR)"
DFT2=$(awk '/^signed long dma_fence_wait_timeout/,/^}/' "$DAF" \
	| grep -n 'test_bit(DMA_FENCE_FLAG_SIGNALED_BIT' | cut -d: -f1 | head -1)
DFR2=$(awk '/^signed long dma_fence_wait_timeout/,/^}/' "$DAF" \
	| grep -n 'return timeout ? timeout : 1;' | cut -d: -f1 | head -1)
[ -n "$DFT2" ] && [ -n "$DFR2" ] && [ "$DFT2" -lt "$DFR2" ] \
	|| gate_fail "wait_timeout signaled test is not before its return"
# (2) 0002 applied source: positive remainder, kept only for an empty resv.
[ "$(grep -c 'long ret = timeout ? timeout : 1;' "$DAR")" = 1 ] \
	|| gate_fail "dma_resv_wait_timeout remainder is not 'timeout ? timeout : 1'"
grep -q 'if (excl || nshared)' "$DAR" \
	|| gate_fail "resv does not keep the positive remainder for an empty resv"
DRW=$(awk '/^long dma_resv_wait_timeout/,/^}/' "$DAR")
printf '%s\n' "$DRW" | grep -q 'ret = timeout;' \
	|| gate_fail "resv no longer reports the waited remainder"
DR_G=$(printf '%s\n' "$DRW" | grep -n 'if (excl || nshared)' | cut -d: -f1 | head -1)
DR_R=$(printf '%s\n' "$DRW" | grep -n 'ret = timeout;' | cut -d: -f1 | head -1)
[ -n "$DR_G" ] && [ -n "$DR_R" ] && [ "$DR_G" -lt "$DR_R" ] \
	|| gate_fail "resv remainder update is not inside the excl||nshared guard"
printf '%s\n' "$DFW" "$DRW" | tee -a "$EVID"
# (3) the sched-slice mirrors carry the same contract.
for pair in "$SDM:$(grep -c 'signed long ret = timeout ? timeout : 1;' "$SDM" 2>/dev/null)" \
	    "$SRM:$(grep -c 'long ret = timeout ? timeout : 1;' "$SRM" 2>/dev/null)"; do
	file=${pair%%:*}; n=${pair##*:}
	[ "$n" = 1 ] || gate_fail "sched mirror $file does not carry the fpssplit initialiser"
done
grep -q 'test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &fence->flags)' "$SDM" \
	|| gate_fail "sched mirror dma-fence.c lost the signaled test"
[ "$(grep -c 'return timeout ? timeout : 1;' "$SDM")" = 1 ] \
	|| gate_fail "sched mirror dma-fence.c lost the positive signaled return"
grep -q 'if (excl || nshared)' "$SRM" \
	|| gate_fail "sched mirror dma-resv.c lost the empty-resv remainder rule"
[ "$(grep -c '^+	signed long ret = timeout ? timeout : 1;' "$REPO/patches/drm-backport/0001-dma-fence-minimal-subset.patch")" = 1 ] \
	|| gate_fail "0001 patch body lost the initialiser"
[ "$(grep -c '^+	long ret = timeout ? timeout : 1;' "$REPO/patches/drm-backport/0002-dma-resv-shim.patch")" = 1 ] \
	|| gate_fail "0002 patch body lost the initialiser"
# (4) compiled: the ?: minimum-1 is a cmp+csinc in every one of the three
# functions (0 budget -> 1), and the signaled paths branch on the flag.
DDF=$(aarch64-linux-gnu-objdump -d --disassemble=dma_fence_wait_timeout "$DAFO")
printf '%s\n' "$DDF" | grep -qE 'cmp[[:space:]]*x2, #0x0' \
	|| gate_fail "dma_fence_wait_timeout lost the timeout==0 compare"
printf '%s\n' "$DDF" | grep -qE 'csinc[[:space:]]*x0, x2, xzr, ne' \
	|| gate_fail "dma_fence_wait_timeout does not compute timeout ? timeout : 1"
printf '%s\n' "$DDF" | grep -qE 'tbnz[[:space:]]*w[0-9]+, #0' \
	|| gate_fail "dma_fence_wait_timeout lost the signaled-flag branch"
DDB=$(aarch64-linux-gnu-objdump -d --disassemble=dma_fence_default_wait "$DAFO")
printf '%s\n' "$DDB" | grep -qE 'csinc[[:space:]]*x0, x2, xzr, ne' \
	|| gate_fail "dma_fence_default_wait does not compute timeout ? timeout : 1"
printf '%s\n' "$DDB" | grep -qE 'tbnz[[:space:]]*w[0-9]+, #0' \
	|| gate_fail "dma_fence_default_wait lost the signaled early exit"
DDR=$(aarch64-linux-gnu-objdump -d --disassemble=dma_resv_wait_timeout "$DARO")
printf '%s\n' "$DDR" | grep -qE 'cmp[[:space:]]*x3, #0x0' \
	|| gate_fail "dma_resv_wait_timeout lost the timeout==0 compare"
printf '%s\n' "$DDR" | grep -qE 'csinc[[:space:]]*x[0-9]+, x3, xzr, ne' \
	|| gate_fail "dma_resv_wait_timeout does not compute timeout ? timeout : 1"
printf '%s\n' "$DDF" | grep -E 'cmp|csinc|tbnz' | head -8 | tee -a "$EVID"
printf '%s\n' "$DDR" | grep -E 'cmp|csinc' | head -4 | tee -a "$EVID"
# (5) host: the three new checks are part of the green sched suite.
[ "$(grep -c '412 checks, 0 failures' "$EXT/logs/gpu31-int49-schedhost.log")" = 2 ] \
	|| gate_fail "sched host suite did not report 412 checks x2 (native + qemu-aarch64)"
grep -q '^gates: all PASS$' "$EXT/logs/gpu31-int49-schedhost.log" \
	|| gate_fail "sched structural gates did not pass"
note "== int49 fpssplit gate: PASS (default_wait + wait_timeout return timeout?:1 for a signaled fence, resv keeps it for an empty resv and reports the waited remainder for a pending set; no bare 'return timeout;' left; sched mirrors and both patch bodies carry it; compiled cmp+csinc in all three functions with the signaled-flag branch; 412 checks x2 on host) =="

# 6ai. gpu31-perf overlay gates (ff078f3). The overlay is a separate
# patch on top of driver 0001/0002 whose only reason to exist is
# measurement, so it must be provably inert when unarmed:
#   - drv.h: prof_arm + per-class ns/n (9 classes) + submits/sub_err/
#     dumps; p7580_prof_stamp returns 0 and p7580_prof_add is a flag
#     test when unarmed (no ktime_get, no accumulation, no print);
#   - gem_job_done_sync, drv_map_bo and drv_submit are wrapping+_body
#     pairs: the unarmed branch calls the body directly;
#   - debugfs p7580/prof_arm 0600, default 0, 1 = reset+arm, 0 = off;
#     one window per 100 SUBMITs only while armed;
#   - submit_err gets its own error-only counter (no sched_trace budget,
#     no ev_arm coupling) with n= appended to the unchanged prefix.
# The 6j/6k/6w/6u/6af/6ag retargets above point at the _body symbols.
note "== int49 gate: perf overlay present and inert when unarmed =="
PRF=drivers/gpu/drm/panfrost/panfrost7580_drv.h
PRS=drivers/gpu/drm/panfrost/panfrost7580_submit.c
PRG=drivers/gpu/drm/panfrost/panfrost7580_gem.c
PRM=drivers/gpu/drm/panfrost/panfrost7580_main.c
for f in "$PRF" "$PRS" "$PRG" "$PRM" "$SUBMIT_O" "$GEM_O" "$MAIN_O"; do
	[ -f "$f" ] || gate_fail "$f missing"
done
# (1) state + helpers.
grep -q 'int prof_arm;' "$PRF" || gate_fail "drv.h lost prof_arm"
grep -q 'u64 ns\[P7580_PROF_N\];' "$PRF" || gate_fail "drv.h lost the per-class ns array"
grep -q 'u32 n\[P7580_PROF_N\];' "$PRF" || gate_fail "drv.h lost the per-class n array"
for fld in 'u32 submits;' 'u32 sub_err;' 'u32 dumps;'; do
	grep -qF "$fld" "$PRF" || gate_fail "drv.h lost '$fld'"
done
[ "$(grep -cE '^#define P7580_PROF_[A-Z]+[[:space:]]+[0-8]$' "$PRF")" = 9 ] \
	|| gate_fail "drv.h must define 9 perf classes, got $(grep -cE '^#define P7580_PROF_[A-Z]+[[:space:]]+[0-8]$' "$PRF")"
grep -q '#define P7580_PROF_N[[:space:]]*9' "$PRF" || gate_fail "P7580_PROF_N 9 missing"
STAMP=$(awk '/^static inline u64 p7580_prof_stamp/,/^}/' "$PRF")
printf '%s\n' "$STAMP" | grep -q 'd->prof_arm' || gate_fail "prof_stamp is not armed-gated"
printf '%s\n' "$STAMP" | grep -q 'ktime_to_ns(ktime_get())' || gate_fail "prof_stamp does not timestamp when armed"
ADD=$(awk '/^static inline void p7580_prof_add/,/^}/' "$PRF")
printf '%s\n' "$ADD" | grep -q 'if (d && d->prof_arm)' || gate_fail "prof_add is not armed-gated"
printf '%s\n' "$ADD" | grep -q 'd->prof.ns\[i\] +=' || gate_fail "prof_add does not accumulate ns"
printf '%s\n' "$ADD" | grep -q 'd->prof.n\[i\]++' || gate_fail "prof_add does not count calls"
printf '%s\n' "$STAMP" "$ADD" | tee -a "$EVID"
# (2) wrapper/body split: unarmed path is a direct call, no timer.
SUBW=$(awk '/^int p7580_drv_submit\(struct p7580_drv \*drv, struct drm_device \*dev,/,/^}/' "$PRS")
printf '%s\n' "$SUBW" | grep -q 'if (!drv || !drv->prof_arm)' || gate_fail "submit wrapper is not armed-gated"
printf '%s\n' "$SUBW" | grep -q 'return p7580_drv_submit_body(drv, dev, file_priv, args);' \
	|| gate_fail "submit wrapper does not call the body directly when unarmed"
printf '%s\n' "$SUBW" | grep -q 'if (++drv->prof.submits % 100 == 0)' \
	|| gate_fail "submit wrapper lost the 100-SUBMIT window"
MAPW=$(awk '/^static int p7580_drv_map_bo\(struct p7580_drv \*drv, struct drm_gem_object \*obj\)/,/^}/' "$PRS")
printf '%s\n' "$MAPW" | grep -q 'if (!drv || !drv->prof_arm)' || gate_fail "map_bo wrapper is not armed-gated"
printf '%s\n' "$MAPW" | grep -q 'return p7580_drv_map_bo_body(drv, obj);' \
	|| gate_fail "map_bo wrapper does not call the body directly when unarmed"
DONEW=$(awk '/^void p7580_gem_job_done_sync/,/^}/' "$PRG")
printf '%s\n' "$DONEW" | grep -q 'if (!pdrv || !pdrv->prof_arm)' || gate_fail "job_done_sync wrapper is not armed-gated"
printf '%s\n' "$DONEW" | grep -q 'p7580_gem_job_done_sync_body(obj);' \
	|| gate_fail "job_done_sync wrapper does not call the body directly when unarmed"
printf '%s\n' "$SUBW" "$MAPW" "$DONEW" | tee -a "$EVID"
# (3) the submit_err diagnostic bug is fixed: own counter, no sched_trace.
ERRB=$(awk '/^static long p7580_submit_err/,/^}/' "$PRS")
printf '%s\n' "$ERRB" | grep -q 'p7580_sched_trace' \
	&& gate_fail "submit_err still consumes the sched_trace budget"
printf '%s\n' "$ERRB" | grep -q 'if (err)' || gate_fail "submit_err does not test err"
printf '%s\n' "$ERRB" | grep -q '++p7580_submit_err_n' || gate_fail "submit_err has no error-only counter"
printf '%s\n' "$ERRB" | grep -qF 'p7580_sched: submit_err stage=%s rc=%ld n=%u' \
	|| gate_fail "submit_err log lost the original prefix + n="
grep -q 'static unsigned int p7580_submit_err_n;' "$PRS" || gate_fail "submit_err counter not declared"
printf '%s\n' "$ERRB" | tee -a "$EVID"
# (4) knobs: prof_arm 0600 default 0, armed dump only.
grep -q 'drv->prof_arm = val ? 1 : 0;' "$PRS" || gate_fail "prof_arm setter missing"
grep -q 'memset(&drv->prof, 0, sizeof(drv->prof));' "$PRS" || gate_fail "prof_arm setter does not reset"
grep -q 'DEFINE_SIMPLE_ATTRIBUTE(p7580_prof_arm_fops,' "$PRS" || gate_fail "prof_arm fops missing"
[ "$(grep -c 'debugfs_create_file("prof_arm", 0600, dir, drv,' "$PRS")" = 1 ] \
	|| gate_fail "prof_arm debugfs file count != 1"
grep -q 'p7580_prof_debugfs_init(drv);' "$PRM" || gate_fail "probe does not init the prof knob"
grep -q 'if (!drv || !drv->prof_arm)' "$PRS" || gate_fail "prof_dump is not armed-gated"
grep -qF 'p7580prof: window=%u create=%llu/%u shmem=%llu/%u mapbo=%llu/%u submit=%llu/%u mmap=%llu/%u wait=%llu/%u free=%llu/%u freejob=%llu/%u jobsync=%llu/%u suberr=%u' "$PRS" \
	|| gate_fail "prof window format string missing"
# (5) classification sites (one per class that has a site).
for pat in 'P7580_PROF_CREATE' 'P7580_PROF_SHMEM' 'P7580_PROF_MAPBO' \
	'P7580_PROF_SUBMIT' 'P7580_PROF_MMAP' 'P7580_PROF_WAIT' \
	'P7580_PROF_FREE' 'P7580_PROF_FREEJOB' 'P7580_PROF_JOBSYNC'; do
	grep -q "p7580_prof_add(.*$pat" "$PRS" "$PRG" || gate_fail "no prof_add site for $pat"
done
# (6) object proof: bodies outlined, knob state/vars present, Image strings.
aarch64-linux-gnu-nm "$SUBMIT_O" | grep -q " t $SUBMIT_BODY$" || gate_fail "$SUBMIT_BODY not outlined"
aarch64-linux-gnu-nm "$SUBMIT_O" | grep -q " t $MAPBO_BODY$" || gate_fail "$MAPBO_BODY not outlined"
aarch64-linux-gnu-nm "$SUBMIT_O" | grep -q ' [bB] p7580_submit_err_n$' || gate_fail "submit_err counter not in submit.o"
aarch64-linux-gnu-nm "$SUBMIT_O" | grep -q ' [rR] p7580_prof_arm_fops$' || gate_fail "prof_arm fops not in submit.o"
aarch64-linux-gnu-nm "$MAIN_O" | grep -q ' U p7580_prof_debugfs_init$' || gate_fail "probe does not reference the prof knob"
aarch64-linux-gnu-nm "$SUBMIT_O" "$MAIN_O" | grep -E 'p7580_drv_(submit_body|map_bo_body|prof_arm_fops)|p7580_prof_debugfs_init|p7580_submit_err_n' | tee -a "$EVID"
# unarmed path: body call precedes the only ktime_get in each wrapper.
WID=$(aarch64-linux-gnu-objdump -dr --disassemble=p7580_drv_submit "$SUBMIT_O")
WB=$(printf '%s\n' "$WID" | grep -n "<$SUBMIT_BODY>" | cut -d: -f1 | head -1)
WT=$(printf '%s\n' "$WID" | grep -n '<ktime_get>' | cut -d: -f1 | head -1)
[ -n "$WB" ] && [ -n "$WT" ] && [ "$WB" -lt "$WT" ] \
	|| gate_fail "submit wrapper does not take the unarmed body path before ktime_get (body=$WB ktime=$WT)"
MID=$(aarch64-linux-gnu-objdump -dr --disassemble=p7580_drv_map_bo "$SUBMIT_O")
MB=$(printf '%s\n' "$MID" | grep -n "<$MAPBO_BODY>" | cut -d: -f1 | head -1)
MT=$(printf '%s\n' "$MID" | grep -n '<ktime_get>' | cut -d: -f1 | head -1)
[ -n "$MB" ] && [ -n "$MT" ] && [ "$MB" -lt "$MT" ] \
	|| gate_fail "map_bo wrapper does not take the unarmed body path before ktime_get"
printf '%s\n' "$WID" | grep -E 'submit_body|ktime_get|prof_arm' | head -8 | tee -a "$EVID"
for s in 'p7580prof: window=%u create=' 'p7580_sched: submit_err stage=%s rc=%ld n=%u'; do
	aarch64-linux-gnu-strings "$SUBMIT_O" | grep -qF "$s" || gate_fail "string missing from submit.o: $s"
	aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -qF "$s" || gate_fail "string missing from Image: $s"
done
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -E 'p7580prof:|submit_err stage=' | tee -a "$EVID"
note "== int49 perf gate: PASS (prof_arm + 9 class counters default 0; stamp/add are one branch when unarmed; job_done_sync/map_bo/submit wrapper->body direct on the unarmed path before any ktime_get; window every 100 SUBMITs only armed; submit_err own error-only counter with the original prefix + n=; debugfs knob wired at probe; bodies outlined and strings in submit.o/Image) =="

# 6aj. int49: the prof_arm knob is created inside the driver's EXISTING
# p7580 debugfs dir. int48 called debugfs_create_dir("p7580", NULL) a
# second time; an existing dir returns -EEXIST as an ERR_PTR, so
# IS_ERR_OR_NULL() sent the function home before debugfs_create_file()
# and the knob silently did not exist on the flashed image. The job slice
# owns the root (p7580_job_dev_init -> jd->dbg_dir), and that init runs
# earlier in probe. Every failure now logs, and success logs
# "p7580perf: prof_arm ready" so dmesg proves the knob exists.
note "== int49 gate: prof_arm created in the existing p7580 dir =="
PRS2=drivers/gpu/drm/panfrost/panfrost7580_submit.c
PRM2=drivers/gpu/drm/panfrost/panfrost7580_main.c
grep -qE '^\+.*= *debugfs_create_dir\(' "$OVL" \
	&& gate_fail "perf overlay still creates the p7580 root dir (int48 defect)"
grep -q 'dir = (struct dentry \*)drv->jd.dbg_dir;' "$PRS2" \
	|| gate_fail "prof init does not reuse drv->jd.dbg_dir"
grep -q 'IS_ERR_OR_NULL(dir)' "$PRS2" \
	&& gate_fail "prof init still error-checks a dir it no longer creates"
grep -qF 'p7580perf: prof_arm unavailable (no p7580 debugfs dir)' "$PRS2" \
	|| gate_fail "missing-dir path is not logged"
grep -qF 'p7580perf: prof_arm knob failed (%ld)' "$PRS2" \
	|| gate_fail "create-failure path is not logged"
grep -qF 'p7580perf: prof_arm ready' "$PRS2" \
	|| gate_fail "success path is not logged (dmesg cannot prove the knob)"
PRD=$(grep -n 'dir = (struct dentry \*)drv->jd.dbg_dir;' "$PRS2" | cut -d: -f1 | head -1)
PRC=$(grep -n 'debugfs_create_file("prof_arm", 0600, dir, drv,' "$PRS2" | cut -d: -f1 | head -1)
[ -n "$PRD" ] && [ -n "$PRC" ] && [ "$PRD" -lt "$PRC" ] \
	|| gate_fail "prof_arm is not created inside the reused dir (dir=$PRD create=$PRC)"
# no silent return: every failure branch logs within 4 lines of its return.
PROF=$(awk '/^void p7580_prof_debugfs_init/,/^}/' "$PRS2")
[ -n "$PROF" ] || gate_fail "p7580_prof_debugfs_init body not found"
SILENT=$(printf '%s\n' "$PROF" | awk '
	/^[[:space:]]*return;$/ { if (lastlog && NR - lastlog > 3) print NR }
	/pr_(info|err)\(/ { lastlog = NR }
	{ n = NR }
	END { }')
[ -z "$SILENT" ] || gate_fail "prof init has a silent return (lines: $SILENT)"
printf '%s\n' "$PROF" | tee -a "$EVID"
# probe ordering: the job slice must have created jd->dbg_dir first.
JDI=$(grep -n 'p7580_job_dev_init(&drv->jd' "$PRM2" | cut -d: -f1 | head -1)
PFI=$(grep -n 'p7580_prof_debugfs_init(drv);' "$PRM2" | cut -d: -f1 | head -1)
[ -n "$JDI" ] && [ -n "$PFI" ] && [ "$JDI" -lt "$PFI" ] \
	|| gate_fail "prof init runs before the job slice creates the debugfs dir (job=$JDI prof=$PFI)"
# driver 0001 remains the single owner of the p7580 root; the overlay
# must not create one (checked above).
[ "$(grep -cE '^\+.*= *debugfs_create_dir\("p7580"' "$DRV1")" = 1 ] \
	|| gate_fail "driver 0001 must own the p7580 debugfs dir exactly once"
# object/Image: all three messages, and the knob still exists.
for s in 'p7580perf: prof_arm ready' 'p7580perf: prof_arm unavailable (no p7580 debugfs dir)' \
	'p7580perf: prof_arm knob failed (%ld)'; do
	aarch64-linux-gnu-strings "$SUBMIT_O" | grep -qF "$s" || gate_fail "string missing from submit.o: $s"
	aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -qF "$s" || gate_fail "string missing from Image: $s"
done
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -E 'p7580perf: prof_arm' | tee -a "$EVID"
aarch64-linux-gnu-nm "$SUBMIT_O" | grep -q ' [rR] p7580_prof_arm_fops$' || gate_fail "prof_arm fops missing from submit.o"
note "== int49 prof_arm gate: PASS (reuses drv->jd.dbg_dir, no second debugfs_create_dir, dir assignment before create_file, every failure logged, success logged, job-dev init precedes prof init, strings in submit.o/Image) =="

note "== freshness =="
stat -c 'SCR %n birth=%w mtime=%y' "$SCR" | tee -a "$EVID"
ls "$PRISTINE/drivers/gpu/drm/drm_sched.c" 2>&1 | tee -a "$EVID"
ls "$PRISTINE/drivers/gpu/drm/panfrost/" 2>&1 | tee -a "$EVID"

# 7. pack boot.img (stock dt.img rule: dt.img is NEVER rebuilt).
# All pack I/O on the external disk: seed inputs are staged from /tmp/bimg
# (read-only) into $EXT/stage-int49; outputs land in $EXT.
STAGE=${STAGE:-$EXT/stage-int49}
mkdir -p "$STAGE"
for f in dt.img ramdisk-new.gz cmdline; do
	[ -f "$STAGE/$f" ] || cp "/tmp/bimg/$f" "$STAGE/$f"
done
[ -f "$STAGE/mkboot.py" ] || cp /tmp/mkboot.py "$STAGE/mkboot.py"
python3 "$STAGE/mkboot.py" arch/arm64/boot/Image "$STAGE/ramdisk-new.gz" \
	"$STAGE/dt.img" "$EXT/kernel-boot-int49.img" "$(cat "$STAGE/cmdline")"
ls -l "$EXT/kernel-boot-int49.img"
sha256sum "$EXT/kernel-boot-int49.img"
# Keep a copy of the built Image next to the boot.img (scratch is deleted
# after the build, so the tree Image would otherwise be lost).
cp arch/arm64/boot/Image "$EXT/Image-int49"
sha256sum "$EXT/Image-int49"

# 8. delete scratch + stage (fresh tree per ticket; evidence kept in
# $EVID + Image-int49 + logs on the external disk).
rm -rf "$SCR" "$STAGE"
note "scratch + stage deleted"
