# gpu31 CI — one command from patch series to boot image + post-flash suite

Date: 2026-11-19. Files (this slice owns all of `code/gpu31/ci/`):

- `ci.sh` — the one command. Host stages S1–S8 (no device contact, no
  flash) + `postflash` device stage (read-only ssh; flash stays MANUAL).
- `ci.conf` — all paths/defaults (KDIR, SCRATCH, OUT, JOBS, DEVICE, LOCK).
- `ci-merge-frag.py` — fragment merger (same semantics as
  `code/gpu31/config/check-matrix.sh`).
- `symbols.list` — 7 required link symbols asserted in vmlinux.

## Usage

```sh
sh code/gpu31/ci/ci.sh --list                 # stage inventory
sh code/gpu31/ci/ci.sh --dry-run              # print all 46 host commands, run none
sh code/gpu31/ci/ci.sh --only S4 --dry-run    # single stage, still fail-fast
sh code/gpu31/ci/ci.sh                        # full host pipeline S1..S8
sh code/gpu31/ci/ci.sh postflash --dry-run    # print the 5 device commands
sh code/gpu31/ci/ci.sh postflash              # AFTER a manual flash
```

## Stages (in order; first failure aborts, later stages never run)

| Stage | What | Fails when |
|---|---|---|
| S1 series-verify | `sha256sum -c` the generated series manifest | any patch drifted from `patches/*` |
| S2 scratch-sync | `rsync` the read-only shared tree to private `$SCRATCH` (skipped if the series fingerprint is unchanged) | shared tree unmounted |
| S3 series-apply | `patch --dry-run` every patch, then `patch -N` apply; backport foundation (fence→resv→syncobj→sched→wiring) before KMS code | any hunk rejects |
| S4 kconfig | D0 fragment must be symbol-identical to the proven M1 fragment; merge onto stock, `olddefconfig`, assert DRM/EXYNOS/G2D/DMABUF on + `EXYNOS_DECON_FB=y` kept + `FRAMEBUFFER_CONSOLE` off | D0 drifted from M1; a symbol failed to close |
| S5 build | serial `Image` build (`-j1`, TMPDIR on the external disk — `-jN` died in `kernel/irq` on a Broken-pipe artifact) | any compile error; log in `$OUT/logs/build.log` |
| S6 symbols | every `symbols.list` entry present as `T/t` in `vmlinux` (`dma_fence_add_callback`, `dma_fence_context_alloc`, `drm_sched_init`, `drm_sched_job_alloc`, `drm_sched_job_submit`, `drm_syncobj_create`, `exynos_drm_platform_probe`) | backport object dropped from the link |
| S7 pack | `mkboot.py` + stock ramdisk/dt/cmdline → `$OUT/boot.img` (+`.sha256`) and `$OUT/Image` (+`.sha256`); assert `ANDROID!` magic and ≤ 32 MiB | mis-assembled image |
| S8 readiness | `code/kernel/recovery/check-readiness.sh $OUT/boot.img` (known-good match, size, magic) | readiness NOT READY |
| postflash | `m1-verify.sh` (REQUIRED gate, `M1-VERIFY-PASS` asserted) + device-probe + harness (both optional: SKIP with a deploy hint when not installed on the device); every ssh hold under `flock $LOCK` ≤ 115 s | any required check FAILs |

Rules enforced by construction: the shared tree is never written (all work
in `$SCRATCH`); the DT patch (`patches/drm/0002-*`) is never in
`SERIES_ORDER` (stock `dt.img` is always reused); the display-swap config
fragment is never merged (D0 is the boot candidate); no `dd`/reboot anywhere
(flash stays a manual `RECOVERY.md` step between S8 and postflash).

## Dry-run (2026-11-19, `sh code/gpu31/ci/ci.sh --dry-run`)

Full-host dry-run: 46 commands printed, 0 executed, `CI-FAIL` count 0,
exit 0 (transcript: all 8 stage headers `=== S1 === … === S8 ===` in order,
each closed by `--- Sn OK ---`, ending in `CI-PASS: boot.img …/boot.img`).
Ordering proof: S1 (manifest) → S2 (rsync) → S3 (dry-run × 6 then apply ×
6, backport first, KMS last) → S4 (diff → cp → merge → olddefconfig →
6 asserts) → S5 (Image) → S6 (7 `nm` asserts) → S7 (pack + magic + size +
2 sha256) → S8 (readiness).
Failure handling proof: live `--only S1` passes (RC=0, 7 manifest lines OK);
live `--only S6` with no built tree fails `CI-FAIL: S6: nm … vmlinux …`
(RC=1) — failure aborts the stage via `die`, later commands in that stage
never run. Postflash dry-run prints 5 commands (m1-verify gate + 2
presence probes + 2 flocked runs), exit 0.

## Outputs

Host run: `$OUT/boot.img` + `boot.img.sha256`, `$OUT/Image` +
`Image.sha256`, `$OUT/logs/build.log`, `$OUT/logs/ci.log`.
Postflash run: `$OUT/postflash/m1-verify.log` (must contain
`M1-VERIFY-PASS`), `device-probe.log`, `harness.log` (when deployed).
