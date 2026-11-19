/* Host-only stub. NEVER in a kernel build. */
#ifndef _ABIFUZZ_LINUX_IO_H_
#define _ABIFUZZ_LINUX_IO_H_

#include <stdint.h>

#define __iomem

static inline uint32_t readl(const volatile void __iomem *addr)
{
	(void)addr;
	return 0;
}

static inline void writel(uint32_t v, volatile void __iomem *addr)
{
	(void)v;
	(void)addr;
}

static inline void cpu_relax(void)
{
}

#endif /* _ABIFUZZ_LINUX_IO_H_ */
