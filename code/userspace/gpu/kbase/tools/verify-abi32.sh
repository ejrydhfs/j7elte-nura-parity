#!/bin/sh
# verify-abi32.sh - prove the r15p0 UK 10.6 wire layout under the 32-bit ARM
# AAPCS (i.e. what a CONFIG_COMPAT / KCTX_COMPAT client sends).
#
# This matters for the ARM Linux DDK assessment: an armhf r15p0 userspace must
# produce exactly the sizes the 64-bit kernel's copy_from_user()/stride checks
# expect.  Needs clang with the armv7a target (no sysroot: freestanding).
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
KERNEL_DIR=${KERNEL_DIR:-/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/j7elte-session/android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07}
R15="$KERNEL_DIR/drivers/gpu/arm/t7xx/r15p0"
CLANG=${CLANG:-clang}

[ -f "$R15/mali_kbase_uku.h" ] || { echo "r15p0 headers not found under $R15" >&2; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/stub/linux"
cat > "$TMP/stub/linux/version.h" <<'EOF'
#ifndef _STUB_LINUX_VERSION_H
#define _STUB_LINUX_VERSION_H
#define KERNEL_VERSION(a, b, c) (((a) << 16) + ((b) << 8) + (c))
#define LINUX_VERSION_CODE KERNEL_VERSION(3, 10, 108)
#endif
EOF

cat > "$TMP/layout32.c" <<'EOF'
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef int s32;
typedef unsigned int uintptr_t;
typedef unsigned int size_t;
#define __user
#define CDBG_ASSERT(x) ((void)0)
#define PAGE_SHIFT 12
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define PAGE_MASK (~(PAGE_SIZE - 1))
#include "mali_uk.h"
#include "mali_base_kernel.h"
#include "mali_kbase_gpuprops_types.h"
#include "mali_kbase_platform_uku.h"
#include "mali_kbase_uku.h"

_Static_assert(sizeof(void *) == 4, "target is not ILP32");
_Static_assert(sizeof(union kbase_pointer) == 8, "kbase_pointer must stay 8 bytes");
_Static_assert(sizeof(struct uku_version_check_args) == 16, "version check");
_Static_assert(sizeof(struct kbase_uk_mem_alloc) == 56, "mem_alloc");
_Static_assert(sizeof(struct kbase_uk_job_submit) == 40, "job_submit");
_Static_assert(sizeof(struct base_jd_atom_v2) == 64, "atom");
_Static_assert(sizeof(struct base_jd_event_v2) == 24, "event");
_Static_assert(__builtin_offsetof(struct base_jd_atom_v2, core_req) == 44, "core_req");
_Static_assert(__builtin_offsetof(struct base_jd_atom_v2, surfacep) == 56, "surfacep");
_Static_assert(__builtin_offsetof(struct kbase_uk_job_submit, surfacep) == 32, "submit surfacep");
EOF

"$CLANG" --target=armv7a-linux-gnueabihf -ffreestanding -nostdlibinc \
	-I"$TMP/stub" -I"$R15" -c "$TMP/layout32.c" -o "$TMP/layout32.o"

echo "32-bit ARM (AAPCS) r15p0 layout: OK"
echo "  atom=64 job_submit=40 mem_alloc=56 event=24 core_req@44 surfacep@56"
echo "  => an armhf r15p0 DDK client and a 64-bit kernel agree on the wire format"
