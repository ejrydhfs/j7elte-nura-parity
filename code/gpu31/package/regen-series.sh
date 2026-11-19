#!/bin/sh
# Regenerate code/gpu31/series/ from the authoritative patches/* dirs.
# Run from the repo root: sh code/gpu31/package/regen-series.sh
cd "$(dirname "$0")/../../.."
# Copies (no edits): drm-backport 0001-0005, drm 0001 (code candidate),
# drm-m1 fragment. Prints sha256 manifest to code/gpu31/series/SHA256SUMS.
set -eu
S=code/gpu31/series
cp patches/drm-backport/0001-dma-fence-minimal-subset.patch "$S/0001-dma-fence-minimal-subset.patch"
cp patches/drm-backport/0002-dma-resv-shim.patch             "$S/0002-dma-resv-shim.patch"
cp patches/drm-backport/0003-drm_syncobj-subset.patch       "$S/0003-drm_syncobj-subset.patch"
cp patches/drm-backport/0004-drm_sched-panfrost-job-only-shim.patch "$S/0004-drm_sched-panfrost-job-only-shim.patch"
cp patches/drm-backport/0005-build-wiring-for-shims.patch   "$S/0005-build-wiring-for-shims.patch"
cp patches/drm/0001-drm-exynos7580-decon-dsim-kms.patch     "$S/0001-drm-exynos7580-decon-dsim-kms.patch"
cp patches/drm-m1/m1-kconfig.fragment                       "$S/m1-kconfig.fragment"
( cd "$S" && sha256sum 0001-dma-fence-minimal-subset.patch 0002-dma-resv-shim.patch \
  0003-drm_syncobj-subset.patch 0004-drm_sched-panfrost-job-only-shim.patch \
  0005-build-wiring-for-shims.patch 0001-drm-exynos7580-decon-dsim-kms.patch \
  m1-kconfig.fragment > SHA256SUMS )
echo "series regenerated:"
cat "$S/SHA256SUMS"
