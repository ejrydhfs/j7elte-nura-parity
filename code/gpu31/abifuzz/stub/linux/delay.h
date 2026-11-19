/* Host-only stub. NEVER in a kernel build. */
#ifndef _ABIFUZZ_LINUX_DELAY_H_
#define _ABIFUZZ_LINUX_DELAY_H_

static inline void udelay(unsigned long us)
{
	(void)us;
}

static inline void usleep_range(unsigned long min, unsigned long max)
{
	(void)min;
	(void)max;
}

#define might_sleep_if(cond)	do { (void)(cond); } while (0)

#endif /* _ABIFUZZ_LINUX_DELAY_H_ */
