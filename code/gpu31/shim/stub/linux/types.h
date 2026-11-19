/* Host-test stub for <linux/types.h>.
 *
 * ONLY for compiling the r7p0 compat header on the build host
 * (code/gpu31/shim/stride_test).  Never part of any kernel build;
 * the kernel uses its own linux/types.h.
 */
#ifndef SHIM_LINUX_TYPES_H
#define SHIM_LINUX_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;

#endif /* SHIM_LINUX_TYPES_H */
