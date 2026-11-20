#ifndef _SCHED_EMU_W_printk_H
#define _SCHED_EMU_W_printk_H
#include "kernel-emu.h"
#include <stdio.h>
/* Host stand-in for KERN_INFO pr_info: trace probes print to stdout. */
#ifndef pr_info
#define pr_info(fmt, ...) printf(fmt, ##__VA_ARGS__)
#endif
#endif
