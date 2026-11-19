/* Host-only kernel-env stubs for abifuzz. NEVER in a kernel build. */
#ifndef _ABIFUZZ_LINUX_TYPES_H_
#define _ABIFUZZ_LINUX_TYPES_H_

#include <stdint.h>

typedef uint32_t __u32;
typedef int32_t __s32;
typedef uint64_t __u64;
typedef int64_t __s64;

typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef int64_t s64;

#endif /* _ABIFUZZ_LINUX_TYPES_H_ */
