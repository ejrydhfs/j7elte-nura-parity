/* Host-test stubs for <linux/module.h>, <linux/platform_device.h>,
 * <linux/slab.h>: just enough for -fsyntax-only on kbase_shim.c. */
#ifndef SHIMCHECK_LINUX_MODULE_H
#define SHIMCHECK_LINUX_MODULE_H

#define MODULE_AUTHOR(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_LICENSE(x)
#define MODULE_ALIAS(x)
#define module_init(fn) int __module_init_##fn(void)
#define module_exit(fn) void __module_exit_##fn(void)
#define THIS_MODULE ((void *)0)

#ifndef __init
#define __init
#endif
#ifndef __exit
#define __exit
#endif

#include <stddef.h>
#include <sys/types.h>
#ifndef __loff_t_defined
typedef __loff_t loff_t;
#define __loff_t_defined
#endif

void *kzalloc(size_t size, int flags);
void kfree(void *p);
#define GFP_KERNEL 0
#define ENODEV 19
#define ENOMEM 12
#define EINVAL 22
#define EBUSY 16
#define PTR_ERR(p) ((long)(p))
#define IS_ERR(p) ((unsigned long)(p) > (unsigned long)-1000L)

#endif /* SHIMCHECK_LINUX_MODULE_H */
