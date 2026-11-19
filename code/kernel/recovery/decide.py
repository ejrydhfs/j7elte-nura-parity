#!/bin/sh
# decide.py — host-side simulation of the single-slot boot-count fallback.
# Models the exact initramfs + boot-guard state machine so operators can
# rehearse campaigns without touching hardware:
#
#   state  = (boot_count, cache_has_valid_copy)
#   events = boot_ok | boot_fail
#   initramfs (per boot):      boot_count += 1; if boot_count > FAIL_MAX and
#                              valid copy -> restore p10, boot_count = 0, reboot
#   boot-guard (userspace OK): boot_count = 0
#
# Usage:
#   python3 code/kernel/recovery/decide.py sim OK OK FAIL FAIL FAIL FAIL
#   python3 code/kernel/recovery/decide.py sim OK FAIL OK FAIL FAIL FAIL --fail-max 2 --no-copy -v
#   python3 code/kernel/recovery/decide.py seq 6            # prints the 2^N outcome table
# Exit 0 on success; prints the trace. No I/O, no device, no root needed.
import sys

FAIL_MAX_DEFAULT = 3


def step(state, event, fail_max):
    """One boot. state=(count, restored_total). Returns (new_state, action)."""
    count, restored = state
    count += 1  # initramfs increments on every boot
    action = "boot-flashed"
    if event == "ok":
        count = 0  # boot-guard resets the counter
        action = "boot-flashed+mark-ok"
    elif count > fail_max:
        if FAIL_MAX_COPY:
            action = "RESTORE-CACHE+rereboot"
            count = 0
            restored += 1
        else:
            action = "boot-flashed(no-valid-copy)"
    else:
        action = "boot-flashed(count=%d)" % count
    return (count, restored), action


FAIL_MAX_COPY = True


def simulate(events, fail_max=FAIL_MAX_DEFAULT, has_copy=True, verbose=False):
    global FAIL_MAX_COPY
    FAIL_MAX_COPY = has_copy
    state = (0, 0)
    for i, ev in enumerate(events):
        state, action = step(state, ev, fail_max)
        print("boot %2d  event=%-4s count=%d  -> %s" %
              (i + 1, ev, state[0], action))
    print("final: boot_count=%d restores=%d" % (state[0], state[1]))
    return state


def outcome_table(n, fail_max=FAIL_MAX_DEFAULT):
    print("seq of %d FAIL boots, fail_max=%d -> action on last boot:" %
          (n, fail_max))
    for k in range(1, n + 1):
        state = (0, 0)
        for _ in range(k):
            state, action = step(state, "fail", fail_max)
        print("  %d consecutive FAILs: count=%d last=%s" %
              (k, state[0], action))


def main(argv):
    if len(argv) < 2 or argv[1] not in ("sim", "seq"):
        print(__doc__)
        return 2
    fail_max = FAIL_MAX_DEFAULT
    has_copy = True
    rest = list(argv[2:])
    flags = [a for a in rest if a.startswith("--") or a == "-v"]
    rest = [a for a in rest if a not in flags]
    for f in flags:
        if f.startswith("--fail-max="):
            fail_max = int(f.split("=", 1)[1])
        elif f == "--no-copy":
            has_copy = False
        elif f == "--copy":
            has_copy = True
        elif f == "-v":
            pass
        else:
            print("unknown flag %s" % f)
            return 2
    if argv[1] == "sim":
        events = [e.lower() for e in rest]
        for e in events:
            if e not in ("ok", "fail"):
                print("event must be OK or FAIL, got %r" % e)
                return 2
        simulate(events, fail_max, has_copy)
    else:
        n = int(rest[0]) if rest else 6
        outcome_table(n, fail_max)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
