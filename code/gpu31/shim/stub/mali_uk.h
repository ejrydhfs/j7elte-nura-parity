/* Host-test stub for <mali_uk.h> (t7xx/r15p0).
 *
 * ONLY for code/gpu31/shim/stride_test.  Values are the r15p0 kernel's own
 * (drivers/gpu/arm/t7xx/r15p0/mali_uk.h: UK_FUNC_ID = 512, uk_header with
 * id/ret/sizer); r7p0 uses identical definitions
 * (drivers/gpu/arm/t72x/r7p0/mali_uk.h).
 */
#ifndef SHIM_MALI_UK_H
#define SHIM_MALI_UK_H

#include <linux/types.h>

enum {
	UK_FUNC_ID = 512
};

union uk_header {
	u32 id;
	u32 ret;
	u64 sizer;
};

struct base_jd_udata {
	u64 blob[2];
};

struct base_dependency {
	u8 atom_id;
	u8 dependency_type;
};

union kbase_pointer {
	void *value;
	u32 compat_value;
	u64 sizer;
};

#endif /* SHIM_MALI_UK_H */
