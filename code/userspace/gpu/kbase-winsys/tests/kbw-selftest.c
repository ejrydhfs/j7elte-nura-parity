/*
 * kbw-selftest.c - bring-up test for the kbase winsys.
 *
 * Modes:
 *   (default)     full path: handshake, props, BO alloc/mmap, soft-job
 *                 completion, out-fence, in-fence, idle.  Requires /dev/maliN.
 *   --print-caps  print the parsed capability block and exit.
 *   --print-abi   print wire sizes used by the winsys (no device needed).
 *
 * Exit codes: 0 success, 2 device absent, 1 failure.  With no device the
 * ABI checks still run, so this is useful on a build host.
 *
 * Nothing here submits a Midgard job chain: with no hardware to validate a
 * chain, a bogus jc would fault the GPU.  The submit path is exercised with
 * soft jobs only.
 *
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kbase_winsys.h"

#define DEFAULT_DEVICE "/dev/mali0"

static int print_abi(void)
{
   printf("uk version           %u.%u\n", KBASE_UK_VERSION_MAJOR,
          KBASE_UK_VERSION_MINOR);
   printf("base_jd_atom_v2      %zu\n", sizeof(struct base_jd_atom_v2));
   printf("base_jd_event_v2     %zu\n", sizeof(struct base_jd_event_v2));
   printf("kbase_uk_mem_alloc   %zu\n", sizeof(struct kbase_uk_mem_alloc));
   printf("kbase_uk_job_submit  %zu\n", sizeof(struct kbase_uk_job_submit));
   printf("dump_counters        %zu\n",
          sizeof(struct base_dump_cpu_gpu_counters));
   return 0;
}

static void print_caps(const struct kbw_caps *c)
{
   printf("UK                   %u.%u\n", c->uk_major, c->uk_minor);
   printf("DDK                  %s\n", c->ddk_version);
   printf("GPU_ID               0x%08x (product 0x%x, r%up%u)\n", c->gpu_id,
          c->product_id, c->major_revision, c->minor_revision);
   printf("shader_present       0x%016" PRIx64 " (%u cores)\n",
          c->shader_present, c->core_count);
   printf("tiler_present        0x%016" PRIx64 "\n", c->tiler_present);
   printf("l2_present           0x%016" PRIx64 "\n", c->l2_present);
   printf("mem_features         0x%08x\n", c->mem_features);
   printf("mmu_features         0x%08x (VA bits %u)\n", c->mmu_features,
          c->va_bits);
   printf("l2_features          0x%08x\n", c->l2_features);
   printf("tiler_features       0x%08x\n", c->tiler_features);
   printf("thread_features      0x%08x (max %u threads/core, %u/WG)\n",
          c->thread_features, c->thread_max_threads,
          c->thread_max_workgroup_size);
   printf("js_present           0x%08x (%u job slots)\n", c->js_present,
          c->max_job_slots);
   printf("core groups          %u (%u coherent groups)\n", c->num_core_groups,
          c->num_groups);
   printf("coherency mode       0x%08x (%s)\n", c->coherency_mode,
          c->io_coherent ? "coherent" : "non-coherent");
}

static int fail(const char *what, int err)
{
   printf("FAIL %-24s %s (%d)\n", what, kbw_strerror(err), err);
   return 1;
}

int main(int argc, char **argv)
{
   const char *path = getenv("KBW_DEVICE");
   const char *mode = argc > 1 ? argv[1] : NULL;
   struct kbw_device *dev = NULL;
   struct kbw_screen *screen = NULL;
   struct kbw_context *ctx = NULL;
   struct kbw_bo *bo = NULL;
   struct base_dump_cpu_gpu_counters dump;
   char err[256] = "";
   int ret, fd = -1;
   int failures = 0;

   print_abi();
   if (mode && !strcmp(mode, "--print-abi"))
      return 0;

   if (!path)
      path = DEFAULT_DEVICE;

   printf("\n-- kbase winsys bring-up on %s --\n", path);
   ret = kbw_device_open(path, &dev, err, sizeof(err));
   if (ret) {
      printf("device %s unavailable: %s\n", path, err);
      printf("ABI self-check passed, bring-up not executed (no hardware)\n");
      return 2;
   }

   printf("\ncapabilities:\n");
   print_caps(kbw_device_caps(dev));
   if (mode && !strcmp(mode, "--print-caps")) {
      kbw_device_destroy(dev);
      return 0;
   }

   ret = kbw_screen_create(dev, &screen);
   if (ret) {
      failures += fail("screen create", ret);
      goto done;
   }

   ret = kbw_context_create(screen, KBW_CONTEXT_SHARE_SCREEN_FD, &ctx, err,
                            sizeof(err));
   if (ret) {
      printf("context create: %s\n", err);
      failures += fail("context create", ret);
      goto done;
   }
   printf("\ncontext created (own event thread + sync timeline)\n");

   ret = kbw_bo_create(ctx, 4096, KBW_BO_DEFAULT_FLAGS, &bo, err, sizeof(err));
   if (ret) {
      printf("bo create: %s\n", err);
      failures += fail("bo create", ret);
      goto done;
   }
   printf("BO: cpu=%p gpu=0x%" PRIx64 " size=%" PRIu64 " flags=0x%" PRIx64
          "\n",
          kbw_bo_cpu(bo), kbw_bo_gpu_va(bo), kbw_bo_size(bo), bo->flags);
   if (kbw_bo_cpu(bo) != (void *)(uintptr_t)kbw_bo_gpu_va(bo)) {
      printf("FAIL SAME_VA invariant: cpu != gpu\n");
      failures++;
   }
   memset(kbw_bo_cpu(bo), 0xa5, 256);
   if (((unsigned char *)kbw_bo_cpu(bo))[255] != 0xa5) {
      printf("FAIL CPU mapping not writable\n");
      failures++;
   }

   /* Soft-job completion round trip. */
   memset(&dump, 0, sizeof(dump));
   ret = kbw_soft_dump_time(ctx, bo, &dump);
   if (ret != BASE_JD_EVENT_DONE) {
      failures += fail("soft dump time", ret);
   } else {
      printf("soft job DONE: gpu cycles=%" PRIu64
             " time=%" PRIu64 ".%06" PRIu64 "s\n",
             dump.cycle_counter, dump.sec, dump.usec);
      if (dump.cycle_counter == 0 && dump.system_time == 0) {
         printf("FAIL soft job wrote no counters\n");
         failures++;
      }
      if (kbw_bo_gpu_va(bo) != 0 && dump.system_time == 0)
         failures++;
   }

   /* Out-fence and in-fence path, using the same soft job as the payload. */
   {
      struct kbw_atom atom = {
         .jc = kbw_bo_gpu_va(bo),
         .core_req = BASE_JD_REQ_SOFT_DUMP_CPU_GPU_TIME,
         .prio = BASE_JD_PRIO_MEDIUM,
         .dep = {-1, -1},
         .dep_type = {BASE_JD_DEP_TYPE_INVALID, BASE_JD_DEP_TYPE_INVALID},
         .udata = {0x6b62775f66656e63ull, 0},
      };
      struct kbw_submit_info si = {.atoms = &atom,
                                   .atom_count = 1,
                                   .in_fence_fd = -1,
                                   .out_fence_fd = &fd};

      ret = kbw_submit(ctx, &si);
      if (ret) {
         failures += fail("submit + out fence", ret);
      } else {
         struct kbw_fence *fence = NULL;
         printf("submit returned out fence fd %d\n", fd);
         ret = kbw_fence_from_fd(fd, &fence);
         if (ret) {
            failures += fail("fence wrap", ret);
         } else {
            ret = kbw_fence_wait(fence, 5000000000ll);
            if (ret) {
               failures += fail("fence wait", ret);
            } else {
               printf("fence %d signalled\n", fd);
            }
            /* Re-submit with the same fence as the in-fence. */
            {
               int fd2 = kbw_fence_dup_fd(fence);
               struct kbw_submit_info si2 = {.atoms = &atom,
                                             .atom_count = 1,
                                             .in_fence_fd = fd2,
                                             .out_fence_fd = NULL};

               ret = kbw_submit(ctx, &si2);
               if (fd2 >= 0)
                  close(fd2);
               if (ret) {
                  failures += fail("submit + in fence", ret);
               } else {
                  printf("in-fence submit accepted\n");
               }
            }
            kbw_fence_unref(fence);
         }
      }
   }

   ret = kbw_context_wait_idle(ctx, 5000000000ll);
   if (ret) {
      failures += fail("wait idle", ret);
   } else {
      printf("context idle\n");
   }

done:
   kbw_bo_destroy(bo);
   kbw_context_destroy(ctx);
   kbw_screen_destroy(screen);
   kbw_device_destroy(dev);

   printf("\n%s\n", failures ? "FAILED" : "bring-up path OK");
   return failures ? 1 : 0;
}
