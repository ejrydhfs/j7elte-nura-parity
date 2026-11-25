# Legacy lock_all owner death: patch 0133 (int83)

Patch: `patches/drm-kms/0133-kms-lock-all-owner-death.patch`
Base: M0 series through 0130 (owner/depth `drm_modeset_lock_all()` from 0128,
int82 fb sync from 0130, int82 trace from 0129).

## Problem

0128 lets only the recorded owner task release the legacy lock_all:

```
drm_modeset_unlock_all: enter by X (owner=modetest depth=1 ctx=...)
drm_modeset_unlock_all: modetest is not the owner (modetest), not unlocking
```

`config->lock_all_owner` is a bare `struct task_struct *` and
`mode_config.mutex` + the legacy `crtc->mutex`es stay locked until that exact
task calls `drm_modeset_unlock_all()`.  Kill (or oops) the owning client
between the two calls and nothing ever releases them: every later DRM ioctl,
including a fresh `open("/dev/dri/card0")` + `drm_mode_getresources`, blocks in
`mutex_lock()` forever.  One dead client wedges the whole KMS device.

## Change

`drivers/gpu/drm/drm_crtc.c` (pre-image lines are in the 0130 tree, post-image
lines are after the patch):

| post | pre | what |
|---|---|---|
| 42-51 | - (inserted before old 42) | `static DEFINE_MUTEX(drm_lock_all_abandon_mutex)` — serialises the rescue; two rescuers would otherwise unlock the same mutex twice |
| 53-63 | - | `drm_lock_all_owner_gone()`: `!pid_alive(owner) || owner->state == TASK_DEAD`.  `pid_alive()` alone is not enough — it stays true up to and including the zombie state (its own kerneldoc says "at most zombie state"); `do_exit()` sets `TASK_DEAD` before its final `schedule()`, after which the task never runs again |
| 65-105 | - | `drm_lock_all_force_release(dev, owner, why)`: `pr_err` owner/pid/state, then depth/ctx/why/caller, `dump_stack()`, clear `lock_all_owner`/`lock_all_depth`/`acquire_ctx`, `drm_modeset_drop_locks()` + `drm_modeset_acquire_fini()` + `kfree(ctx)`, unlock the legacy crtc mutexes, unlock `mode_config.mutex`, `put_task_struct(owner)` |
| 107-139 | - | exported `drm_modeset_lock_all_force_unlock(dev, why)`: under the abandon mutex, no-op when no owner, no-op (pr_debug) when the owner is alive and not current, else force release |
| 182-190 | before old 83 | `drm_modeset_lock_all()`: if the recorded owner is not current and is gone, force-release **before** `mutex_lock(&config->mutex)`, i.e. before another task can block on a dead owner |
| 207-213 | after old 96 | `get_task_struct(current)` when installing the owner, so the recorded pointer stays valid to inspect after the task dies |
| 250-280 | before old 135 | `drm_modeset_unlock_all()` repeated `config->lock_all_owner != current` check and the whole depth-0 clear+release under the abandon mutex; `put_task_struct()` added.  Nesting (`--depth`) semantics unchanged |

`drivers/gpu/drm/drm_fops.c` — `drm_release()`, right after
`mutex_lock(&drm_global_mutex)` (post 444-452):

```c
	if (drm_core_check_feature(dev, DRIVER_MODESET) &&
	    dev->mode_config.lock_all_owner)
		drm_modeset_lock_all_force_unlock(dev, "client file release");
```

Covers "the client that owns the locks is closing its fd" (the owner is
`current` there, because the fd's `release` runs in the exiting task) and "the
recorded owner is already dead".  It is a no-op for every other release, so
the normal path is untouched.

`include/drm/drm_crtc.h` — `lock_all_owner` comment now says refcounted
(post 998), and the new prototype next to `drm_modeset_unlock_all()`
(post 1182-1183) so `drm_fops.c` gets a declaration instead of an implicit one.

Owner invariant used throughout: `lock_all_owner` is set only after
`mode_config.mutex` is taken and cleared before it is released, so
`owner != NULL && owner != current` implies that owner still holds the locks.
The rescue path therefore releases exactly what `drm_modeset_unlock_all()`
would have.

## Why the extra mutex, and why it cannot deadlock

Normal release: holds the modeset locks, then takes `drm_lock_all_abandon_mutex`.
Rescue: takes `drm_lock_all_abandon_mutex`, then only *releases*
`mode_config.mutex` (never waits for it), so the order cannot be inverted.
Its purpose is the read-decide-act race: without it, a rescuer could read
`owner = T`, T could finish its own `unlock_all()` and die, and the rescuer
would then see `T` as dead, unlock a mutex that a *new* owner already holds,
and `put_task_struct()` a reference T already dropped.  With the clear under
the abandon mutex, a rescuer either sees a live, running owner (and backs off)
or sees the field cleared.

## Verification

No kernel build (left to the integrator).  Apply check against a tree rebuilt
from the series, `m0work` + 0106, 0110, 0121, 0126, 0128, 0129 plus the pristine
3.10 `drm_fops.c` (`drm_crtc.c` byte-identical to the int82 image source):

```
$ cd <series-tree> && git apply --check --verbose patches/drm-kms/0133-kms-lock-all-owner-death.patch
Checking patch drivers/gpu/drm/drm_crtc.c...
Checking patch drivers/gpu/drm/drm_fops.c...
Checking patch include/drm/drm_crtc.h...
$ echo $?
0
```

Touched files are disjoint from 0132 (`exynos_drm7580_crtc.c`) and from the
driver patches 0104-0130, so the patch is position independent of them.

## Apply order

1. `0104`..`0130` as today (0130 last);
2. `0133` (this patch);
3. `0132` (manual page-flip state) in any order relative to 0133 — different
   files.

**Do not apply `0131-kms-lock-abandon-dead-owner.patch` as well.** That
untracked draft implements the same int83 fix with a different helper name and
also inserts `get_task_struct()` at the same spot, so the two conflict by
construction.  If the draft is kept instead, 0133 must be dropped; they are
alternatives, not a stack.

## Tradeoffs / residual risks

* Cross-task `mutex_unlock()` is what makes the rescue possible and is outside
  the mutex API contract.  It is safe here only because the build has
  `CONFIG_MUTEX_SPIN_ON_OWNER=y` with `CONFIG_DEBUG_MUTEXES`,
  `CONFIG_DEBUG_LOCK_ALLOC` and `CONFIG_PROVE_LOCKING` all off
  (`build/final-config`), i.e. `mutex_unlock()` just clears the owner field and
  wakes a waiter.  Enabling any mutex/lockdep debugging turns the rescue into a
  warning or a panic — revisit this patch if that ever happens (the real fix is
  to stop using a system-wide owner-tracked lock, see below).
* Two windows are not detectable by design: a task that dies between
  `mutex_lock(&config->mutex)` and `lock_all_owner = current` (nothing recorded
  yet), and one that dies between clearing the owner and the last
  `mutex_unlock()` in the tail.  Both require death inside a short critical
  section, both are left as-is rather than adding an "in progress" flag.
* A task killed while blocked in an uninterruptible state keeps
  `state != TASK_DEAD`, so it is still treated as a live owner.  Deliberate: it
  may still return and release.
* `owner->state` is read without barriers; a stale `TASK_RUNNING` only delays
  the rescue to the next `lock_all()`/`drm_release()` caller.
* Cost on the healthy path: 2 refcount atomics in `lock_all`/`unlock_all` and
  one extra mutex round-trip at depth 0.  Nested calls are unchanged.
* This is recovery, not prevention: a client can still hold the global
  lock_all across a syscall and be killed in the middle of a legacy path.  The
  M1 direction (per-object w/w modesets, no global owner-tracked lock_all)
  removes the failure mode; 0133 only keeps the device usable until then.
* The rescue runs from `drm_release()` while `drm_global_mutex` is held; it
  takes only its private mutex and drops locks, so no new ordering with
  `drm_global_mutex` is introduced.

## Expected dmesg on a rescue

```
drm_modeset_lock_all_force_release: owner modetest/1234 state=64
drm_modeset_lock_all_force_release: depth=1 ctx=ffffffc0... (client file release) by kmscube/1300
drm_modeset_lock_all_force_release: forcing unlock of the legacy modeset locks
<backtrace>
drm_modeset_lock_all_force_release: released by kmscube
```

followed by the caller's own `drm_modeset_lock_all: acquired by ...`.
