#!/bin/sh
# gpu31 CI — one command from patch series to boot image + post-flash suite.
#
#   sh code/gpu31/ci/ci.sh [--dry-run] [--only STAGE] [--list]
#   sh code/gpu31/ci/ci.sh postflash [--dry-run]
#
# Host stages (default; no device contact, no flash):
#   S1 series-verify  sha256sum -c the generated series manifest
#   S2 scratch-sync   fresh private copy of the read-only shared tree
#   S3 series-apply   check-then-apply, backport foundation first, KMS last
#   S4 kconfig        stock config + D0 (M1) fragment, olddefconfig, assert
#   S5 build          serial Image build (TMPDIR on external disk)
#   S6 symbols        every symbols.list entry linked into vmlinux
#   S7 pack           mkboot.py + stock dt/ramdisk/cmdline, sha256, size/magic
#   S8 readiness      code/kernel/recovery/check-readiness.sh on the new image
# Device stage (separate subcommand; flash itself stays MANUAL):
#   postflash         m1-verify.sh + device-probe + harness over ssh, read-only
#
# Failure handling: `set -eu` + every stage runs through stage() — the first
# failing command aborts the run with `CI-FAIL: <stage>` (later stages never
# run after a failure). --dry-run prints the ordered commands without running
# them; --only runs a single stage (debugging, still fail-fast).
# POSIX sh only (no bashisms: no process substitution, no arrays).
# Big files stay on the external drive / device /data. The shared tree
# ($KDIR) is NEVER written — CI builds in a private $SCRATCH copy.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
# shellcheck disable=SC1091
. "$HERE/ci.conf"
ROOT=$(cd "$HERE/../../.." && pwd)
SERIES_DIR=${SERIES_DIR:-$ROOT/$SERIES_REL}
STOCK=${STOCK:-$ROOT/$STOCK_REL}
FRAG=${FRAG:-$ROOT/$FRAG_REL}
M1FRAG=${M1FRAG:-$ROOT/$M1FRAG_REL}
SYMBOLS=${SYMBOLS:-$ROOT/$SYMBOLS_REL}
MERGE_PY=$HERE/ci-merge-frag.py

DRY=0
ONLY=""
CMD="all"
for a in "$@"; do
  case "$a" in
    --dry-run) DRY=1 ;;
    --only)
      if [ -n "$ONLY" ]; then echo "ci: --only given twice" >&2; exit 2; fi
      ONLY="NEXT" ;;
    --list) CMD="list" ;;
    postflash) CMD="postflash" ;;
    *)
      if [ "$ONLY" = "NEXT" ]; then
        case "$a" in S1|S2|S3|S4|S5|S6|S7|S8) ONLY="$a" ;;
        *) echo "ci: unknown stage $a" >&2; exit 2 ;; esac
      else echo "ci: unknown arg $a" >&2; exit 2; fi ;;
  esac
done
[ "$ONLY" = "NEXT" ] && { echo "ci: --only needs a stage (S1..S8)" >&2; exit 2; }

LOGDIR=$OUT/logs
if [ "$DRY" -eq 0 ]; then mkdir -p "$LOGDIR" "$OUT/postflash" "$OUT/tmp"; fi
LOG=$LOGDIR/ci.log

log() { printf '%s\n' "$1"; if [ "$DRY" -eq 0 ]; then printf '%s\n' "$1" >>"$LOG"; fi; }
die() { log "CI-FAIL: $1"; exit 1; }
# run: print the command in both modes, execute only when live. Failure calls
# die explicitly (a bare `set -e` is NOT enough: stage() invokes "$@" behind
# `|| die`, and POSIX shells suppress errexit inside such a context).
run() { log "+ $*"; if [ "$DRY" -eq 0 ]; then eval "$*" || die "${STAGE:-ci}: $*"; fi; }
stage() {
  name=$1; shift
  STAGE=$name; export STAGE
  log "=== $name ==="
  "$@" || die "$name"
}

STAGES="S1 S2 S3 S4 S5 S6 S7 S8"

# --- S1: series integrity ------------------------------------------------
S1() {
  run "cd '$SERIES_DIR' && sha256sum -c SHA256SUMS"
}

# --- S2: private scratch tree (shared tree NEVER written) ------------------
S2() {
  log "+ cur=\$(cat '$SERIES_DIR/SHA256SUMS' | sha256sum)   # series fingerprint"
  if [ "$DRY" -eq 0 ]; then
    cur=$(cat "$SERIES_DIR/SHA256SUMS" | sha256sum)
    marker=$SCRATCH/.ci-series-sha
    if [ -f "$marker" ] && [ "$(cat "$marker")" = "$cur" ]; then
      log "scratch reuse: $SCRATCH (series unchanged)"
      return 0
    fi
  fi
  run "mkdir -p '$SCRATCH'"
  run "rsync -a --delete --exclude='.ci-series-sha' '$KDIR/' '$SCRATCH/'"
  if [ "$DRY" -eq 0 ]; then printf '%s\n' "$cur" >"$SCRATCH/.ci-series-sha"; fi
  log "scratch ready: $SCRATCH"
}

# --- S3: apply series in dependency order (check, then apply) -------------
S3() {
  for p in $SERIES_ORDER; do
    run "patch -p1 --dry-run -d '$SCRATCH' < '$SERIES_DIR/$p'"
  done
  for p in $SERIES_ORDER; do
    run "patch -p1 -N -r - -d '$SCRATCH' < '$SERIES_DIR/$p'"
  done
}

# --- S4: kconfig (D0 must equal the proven M1 symbol set) ------------------
S4() {
  run "grep -h '^CONFIG_' '$M1FRAG' | sort > '$OUT/tmp/m1.syms'"
  run "grep -h '^CONFIG_' '$FRAG' | sort > '$OUT/tmp/d0.syms'"
  run "diff '$OUT/tmp/m1.syms' '$OUT/tmp/d0.syms'"
  run "cp '$STOCK' '$SCRATCH/.config'"
  run "python3 '$MERGE_PY' '$SCRATCH/.config' '$FRAG'"
  run "make -C '$SCRATCH' ARCH=arm64 olddefconfig"
  run "grep -q '^CONFIG_DRM=y' '$SCRATCH/.config'"
  run "grep -q '^CONFIG_DRM_EXYNOS=y' '$SCRATCH/.config'"
  run "grep -q '^CONFIG_DRM_EXYNOS_G2D=y' '$SCRATCH/.config'"
  run "grep -q '^CONFIG_DRM_EXYNOS_DMABUF=y' '$SCRATCH/.config'"
  run "grep -q '^CONFIG_EXYNOS_DECON_FB=y' '$SCRATCH/.config'"
  run "grep -q '^# CONFIG_FRAMEBUFFER_CONSOLE is not set' '$SCRATCH/.config'"
}

# --- S5: serial Image build ------------------------------------------------
S5() {
  run "export TMPDIR='$OUT/tmp' TMP='$OUT/tmp' TEMP='$OUT/tmp'"
  if [ "$DRY" -eq 0 ]; then export TMPDIR="$OUT/tmp" TMP="$OUT/tmp" TEMP="$OUT/tmp"; fi
  run "make -C '$SCRATCH' ARCH=arm64 CROSS_COMPILE='$CROSS' KCFLAGS='$KCFLAGS' -j$JOBS Image > '$LOGDIR/build.log' 2>&1"
  run "ls -l '$SCRATCH/arch/arm64/boot/Image'"
}

# --- S6: required symbols linked in ----------------------------------------
S6() {
  while IFS= read -r sym; do
    case "$sym" in ''|\#*) continue ;; esac
    run "nm '$SCRATCH/vmlinux' | grep -q ' [Tt] $sym\$'"
  done <"$SYMBOLS"
  run "grep -c . '$SYMBOLS' | xargs echo checked-symbols:"
}

# --- S7: boot image + sha256 -----------------------------------------------
S7() {
  run "python3 '$MKBOOT' '$SCRATCH/arch/arm64/boot/Image' '$BIMG_RD' '$BIMG_DT' '$OUT/boot.img' \"\$(cat '$BIMG_CMD')\""
  run "head -c 8 '$OUT/boot.img' | grep -q 'ANDROID!'"
  run "test \"\$(stat -c%s '$OUT/boot.img')\" -le 33554432"
  run "sha256sum '$OUT/boot.img' | tee '$OUT/boot.img.sha256'"
  run "cp '$SCRATCH/arch/arm64/boot/Image' '$OUT/Image'"
  run "sha256sum '$OUT/Image' | tee '$OUT/Image.sha256'"
}

# --- S8: flash readiness (read-only) ----------------------------------------
S8() {
  run "sh '$ROOT/code/kernel/recovery/check-readiness.sh' '$OUT/boot.img'"
}

# --- postflash: read-only device suite (flash stays manual) -----------------
postflash() {
  pfdir=$OUT/postflash
  m1sh=$ROOT/code/kernel/m1/m1-verify.sh
  ssh_base="SSH_ASKPASS=$SSH_ASKPASS SSH_ASKPASS_REQUIRE=force timeout 100 setsid -w ssh -o StrictHostKeyChecking=no $DEVICE"
  if [ "$DRY" -eq 1 ]; then
    log "+ flock $LOCK -c \"$ssh_base 'sh -s' < $m1sh > $pfdir/m1-verify.log\"  # required gate"
    log "+ $ssh_base 'test -x /data/probe/run-device-probe.sh'                 # probe present?"
    log "+ flock $LOCK -c \"$ssh_base '/data/probe/run-device-probe.sh ...' > $pfdir/device-probe.log\"  # optional"
    log "+ $ssh_base 'test -x /data/glmark-harness/run-harness.sh'              # harness present?"
    log "+ flock $LOCK -c \"$ssh_base 'DISPLAY=:0 sh /data/glmark-harness/run-harness.sh --expect auto ...' > $pfdir/harness.log\"  # optional"
    log "POSTFLASH-PASS (dry-run)"
    return 0
  fi
  log "=== postflash: m1-verify (required) ==="
  # shellcheck disable=SC2094
  flock "$LOCK" -c "$ssh_base 'sh -s'" <"$m1sh" >"$pfdir/m1-verify.log" 2>&1 \
    || die "postflash m1-verify (see $pfdir/m1-verify.log)"
  grep -q 'M1-VERIFY-PASS' "$pfdir/m1-verify.log" \
    || die "postflash m1-verify did not PASS (see $pfdir/m1-verify.log)"
  log "--- postflash m1-verify PASS ---"
  log "=== postflash: device-probe (optional, SKIP if not deployed) ==="
  if $ssh_base 'test -x /data/probe/run-device-probe.sh' 2>/dev/null; then
    flock "$LOCK" -c "$ssh_base '/data/probe/run-device-probe.sh /data/probe/render-node-probe /data/probe'" \
      >"$pfdir/device-probe.log" 2>&1 \
      || die "postflash device-probe (see $pfdir/device-probe.log)"
    log "--- postflash device-probe done ---"
  else
    log "SKIP: /data/probe/run-device-probe.sh not deployed"
    log "  deploy: scp $ROOT/code/gpu31/probe/render-node-probe* $DEVICE:/data/probe/"
  fi
  log "=== postflash: harness (optional, SKIP if not deployed) ==="
  if $ssh_base 'test -x /data/glmark-harness/run-harness.sh' 2>/dev/null; then
    flock "$LOCK" -c "$ssh_base 'DISPLAY=:0 sh /data/glmark-harness/run-harness.sh --expect auto --out /data/glmark-harness/h-ci'" \
      >"$pfdir/harness.log" 2>&1 \
      || die "postflash harness (see $pfdir/harness.log)"
    log "--- postflash harness done ---"
  else
    log "SKIP: /data/glmark-harness/run-harness.sh not deployed"
  fi
  log "POSTFLASH-PASS"
}

if [ "$CMD" = "list" ]; then
  echo "host stages: $STAGES (in order; first failure aborts)"
  echo "device stage: postflash (m1-verify required; probe+harness optional)"
  exit 0
fi
if [ "$CMD" = "postflash" ]; then
  [ "$DRY" -eq 1 ] && log "--- dry-run: postflash ---"
  postflash
  exit 0
fi

[ "$DRY" -eq 1 ] && log "--- dry-run: host stages in order ---"
if [ -n "$ONLY" ]; then
  case " $STAGES " in *" $ONLY "*) stage "$ONLY" "$ONLY" ;; *) echo "ci: unknown stage $ONLY" >&2; exit 2 ;; esac
  exit 0
fi
for s in $STAGES; do stage "$s" "$s"; done
log "CI-PASS: boot.img $OUT/boot.img ($(cat "$OUT/boot.img.sha256" 2>/dev/null || echo 'sha pending'))"
