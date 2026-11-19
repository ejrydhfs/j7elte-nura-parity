/* gpu-job-load.c — sustained kbase SOFT_JOB load for power measurement.
 *
 * Keeps the Mali-T720 powered + DVFS governor fed for a fixed window so
 * utilization / time_in_state / vdd_g3d / battery current can be sampled
 * under load. Read-only w.r.t. policy: never touches clock/lock/dvfs nodes.
 *
 * Build (host, aarch64 static for the musl/Nura rootfs):
 *   aarch64-linux-gnu-gcc -O2 -Wall -std=c11 -D_POSIX_C_SOURCE=200809L \
 *     -static -o gpu-job-load-arm64 gpu-job-load.c kbase.c
 * where kbase.{c,h}, kbase_uapi.h come from code/userspace/gpu/kbase/.
 *
 * Usage (device, as root — /dev/mali0 is root-only):
 *   /usr/local/bin/rootshell sh -c '/home/user/gpu-job-load 30 300'
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "kbase.h"

int main(int argc, char **argv)
{
	int seconds = argc > 1 ? atoi(argv[1]) : 20;
	int per_batch = argc > 2 ? atoi(argv[2]) : 50;
	struct kbase_dev dev;
	struct kbase_bo bo;
	char err[256];
	time_t t0 = time(NULL);
	long jobs = 0, fails = 0;
	int ret = kbase_open_dev(&dev, "/dev/mali0", err, sizeof(err));

	if (ret) {
		printf("open failed: %s\n", err);
		return 1;
	}
	printf("handshake UK %u.%u DDK %s ctx %d\n",
	       dev.uk_major, dev.uk_minor, dev.ddk_version, dev.ctx_id);
	ret = kbase_alloc(&dev, 1,
			  BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
				  BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
				  BASE_MEM_SAME_VA,
			  &bo, err, sizeof(err));
	if (ret) {
		printf("alloc failed: %s\n", err);
		kbase_close_dev(&dev);
		return 1;
	}
	printf("alloc gpu_va=0x%llx\n", (unsigned long long)bo.gpu_va);
	while (time(NULL) - t0 < seconds) {
		int i;

		for (i = 0; i < per_batch; i++) {
			struct base_dump_cpu_gpu_counters dump;
			int r;

			memset(&dump, 0, sizeof(dump));
			r = kbase_soft_dump_time(&dev, &bo, &dump);
			if (r < 0)
				fails++;
			else
				jobs++;
		}
	}
	printf("jobs=%ld fails=%ld elapsed=%lds\n",
	       jobs, (long)(time(NULL) - t0), fails);
	kbase_free(&dev, &bo);
	kbase_close_dev(&dev);
	printf("RESULT: %s\n", fails == 0 ? "OK" : "FAILURES");
	return fails == 0 ? 0 : 1;
}
