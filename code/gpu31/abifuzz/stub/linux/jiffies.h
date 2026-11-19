/* Host-only stub. NEVER in a kernel build. */
#ifndef _ABIFUZZ_LINUX_JIFFIES_H_
#define _ABIFUZZ_LINUX_JIFFIES_H_

#define ETIMEDOUT	110

extern unsigned long jiffies;

static inline unsigned long usecs_to_jiffies(unsigned long us)
{
	return (us + 9999UL) / 10000UL;
}

static inline unsigned long msecs_to_jiffies(unsigned long ms)
{
	return (ms + 9UL) / 10UL;
}

#define time_after(a, b)	((long)(b) - (long)(a) < 0)

#endif /* _ABIFUZZ_LINUX_JIFFIES_H_ */
