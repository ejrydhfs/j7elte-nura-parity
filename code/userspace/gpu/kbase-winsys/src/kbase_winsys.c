/*
 * kbase_winsys.c - kbase UK 10.6 winsys implementation.
 *
 * See kbase_winsys.h for the contract and the kernel reference points.
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "kbase.h"
#include "kbase_winsys.h"

/* mali_base_kernel.h: not part of the minimal client header. */
#ifndef BASE_MEM_GROW_ON_GPF
#define BASE_MEM_GROW_ON_GPF (1u << 9)
#endif

/* ------------------------------------------------------------------ */
/* UK 10.6 calls that the minimal client library does not expose. */

/* mali_kbase_uku.h: KBASE_FUNC_STREAM_CREATE = UK_FUNC_ID + 24.  With
 * CONFIG_SYNC=y (Nura config:3173) it returns an Android sync timeline fd;
 * fences are created on it with STREAM_IOC_FENCE_INSERT. */
#define KBASE_FUNC_STREAM_CREATE (UK_FUNC_ID + 24)

struct kbw_uk_stream_create {
   union kbase_uk_header header;
   char name[32];
   int32_t fd;
   uint32_t padding;
};

_Static_assert(sizeof(struct kbw_uk_stream_create) == 48,
               "kbase_uk_stream_create wire size");

/* mali_base_kernel.h: the fence descriptor a soft fence trigger/wait atom
 * points at with atom->jc (a CPU pointer, not a GPU VA).  The kernel writes
 * the new fence fd back into basep.fd during kbase_prepare_soft_job(). */
struct kbw_base_fence {
   struct {
      int fd;
      int stream_fd;
   } basep;
};

_Static_assert(sizeof(struct kbw_base_fence) == 8, "base_fence wire size");

/* Linux 3.10 Android sync UAPI (include/linux/sync.h).  kbase fences are
 * sync_fence fds: poll() reports POLLIN once signaled.  We only need poll,
 * so no sync_pt ioctls are used. */

#define KBW_PAGE_SIZE 4096u
#define KBW_ALIGN(x, a) (((x) + (a) - 1) & ~((uint64_t)(a) - 1))

/* ------------------------------------------------------------------ */
/* GPU props wire layout.  Values measured from the kernel's own headers by
 * tools/kbw-props-layout.c (re-run it if the kernel headers change). */

struct kbw_core_props {
   uint32_t product_id;      /* 0 */
   uint16_t version_status;  /* 4 */
   uint16_t minor_revision;  /* 6 */
   uint16_t major_revision;  /* 8 */
   uint16_t padding;         /* 10 */
   uint32_t gpu_speed_mhz;   /* 12 */
   uint32_t gpu_freq_khz_max;
   uint32_t gpu_freq_khz_min;
   uint32_t log2_program_counter_size;
   uint32_t texture_features[3];
   uint64_t gpu_available_memory_size;
};

struct kbw_l2_props {
   uint8_t log2_line_size;
   uint8_t log2_cache_size;
   uint8_t num_l2_slices;
   uint8_t padding[5];
};

struct kbw_tiler_props {
   uint32_t bin_size_bytes;
   uint32_t max_active_levels;
};

struct kbw_thread_props {
   uint32_t max_threads;
   uint32_t max_workgroup_size;
   uint32_t max_barrier_size;
   uint16_t max_registers;
   uint8_t max_task_queue;
   uint8_t max_thread_group_split;
   uint8_t impl_tech;
   uint8_t padding[7];
};

struct kbw_raw_props {
   uint64_t shader_present;
   uint64_t tiler_present;
   uint64_t l2_present;
   uint64_t stack_present;
   uint32_t l2_features;
   uint32_t suspend_size;
   uint32_t mem_features;
   uint32_t mmu_features;
   uint32_t as_present;
   uint32_t js_present;
   uint32_t js_features[16]; /* GPU_MAX_JOB_SLOTS */
   uint32_t tiler_features;
   uint32_t texture_features[3];
   uint32_t gpu_id;
   uint32_t thread_max_threads;
   uint32_t thread_max_workgroup_size;
   uint32_t thread_max_barrier_size;
   uint32_t thread_features;
   uint32_t coherency_mode;
};

struct kbw_core_group {
   uint64_t core_mask;
   uint16_t num_cores;
   uint16_t padding[3];
};

struct kbw_coherency_info {
   uint32_t num_groups;
   uint32_t num_core_groups;
   uint32_t coherency;
   uint32_t padding;
   struct kbw_core_group group[16]; /* BASE_MAX_COHERENT_GROUPS */
};

struct kbw_gpu_props {
   struct kbw_core_props core_props; /* 0 */
   struct kbw_l2_props l2_props;     /* 48 */
   uint64_t unused_1;                /* 56 */
   struct kbw_tiler_props tiler_props;   /* 64 */
   struct kbw_thread_props thread_props; /* 72 */
   struct kbw_raw_props raw_props;       /* 96 */
   struct kbw_coherency_info coherency_info; /* 256 */
};

_Static_assert(sizeof(struct kbw_core_props) == 48, "core props size");
_Static_assert(sizeof(struct kbw_raw_props) == 160, "raw props size");
_Static_assert(sizeof(struct kbw_coherency_info) == 272, "coherency size");
_Static_assert(sizeof(struct kbw_gpu_props) == 528, "gpu props size");
_Static_assert(__builtin_offsetof(struct kbw_gpu_props, l2_props) == 48, "");
_Static_assert(__builtin_offsetof(struct kbw_gpu_props, tiler_props) == 64, "");
_Static_assert(__builtin_offsetof(struct kbw_gpu_props, thread_props) == 72, "");
_Static_assert(__builtin_offsetof(struct kbw_gpu_props, raw_props) == 96, "");
_Static_assert(__builtin_offsetof(struct kbw_gpu_props, coherency_info) == 256,
               "");
_Static_assert(__builtin_offsetof(struct kbw_raw_props, mem_features) == 40, "");
_Static_assert(__builtin_offsetof(struct kbw_raw_props, mmu_features) == 44, "");
_Static_assert(__builtin_offsetof(struct kbw_raw_props, js_present) == 52, "");
_Static_assert(__builtin_offsetof(struct kbw_raw_props, js_features) == 56, "");
_Static_assert(__builtin_offsetof(struct kbw_raw_props, tiler_features) ==
                  120,
               "");
_Static_assert(__builtin_offsetof(struct kbw_raw_props, gpu_id) == 136, "");
_Static_assert(__builtin_offsetof(struct kbw_raw_props, thread_max_threads) ==
                  140,
               "");

/* ------------------------------------------------------------------ */
/* Small helpers. */

static void kbw_set_err(char *err, size_t errlen, const char *fmt, ...)
{
   va_list ap;

   if (!err || !errlen)
      return;
   va_start(ap, fmt);
   vsnprintf(err, errlen, fmt, ap);
   va_end(ap);
}

static int kbw_uk_call(int fd, void *args, size_t size)
{
   if (size > KBASE_UK_CALL_MAX_SIZE)
      return -E2BIG;
   if (ioctl(fd, KBASE_UK_IOCTL(size), args) != 0)
      return -errno;
   return 0;
}

static int64_t kbw_now_ns(void)
{
   struct timespec ts;

   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

static void kbw_deadline_to_ts(int64_t deadline_ns, struct timespec *ts)
{
   ts->tv_sec = deadline_ns / 1000000000ll;
   ts->tv_nsec = deadline_ns % 1000000000ll;
}

struct kbw_device {
   struct kbase_dev kdev;
   struct kbw_caps caps;
   char path[64];
   bool owns_fd;
};

struct kbw_screen {
   struct kbw_device *dev;
};

struct kbw_context {
   struct kbw_screen *screen;
   struct kbase_dev dev; /* copy of the device or a fresh kbase context */
   bool owns_fd;
   int tl_fd;

   pthread_t ev_thread;
   bool ev_thread_valid;
   int stop_pipe[2];

   pthread_mutex_t lock;
   pthread_cond_t cond;
   bool atom_busy[KBW_MAX_ATOMS];
   unsigned last_event[KBW_MAX_ATOMS];

   int64_t slot_timeout_ns;
};

/* ------------------------------------------------------------------ */
/* Device. */

static unsigned kbw_popcount64(uint64_t v)
{
   return (unsigned)__builtin_popcountll(v);
}

static unsigned kbw_fls64(uint64_t v)
{
   return v ? (unsigned)(64 - __builtin_clzll(v)) : 0;
}

/* Handshake, flags, tracking page, version, context id.  Mirrors
 * kbase_open_dev() but works on an fd the caller already owns. */
static int kbw_dev_init_fd(struct kbase_dev *dev, int fd, bool owns_fd,
                           char *err, size_t errlen)
{
   struct uku_version_check_args ver;
   struct kbase_uk_set_flags flags;
   struct kbase_uk_get_ddk_version version;
   struct kbase_uk_context_id ctx;
   int ret;

   memset(dev, 0, sizeof(*dev));
   dev->fd = owns_fd ? fd : -1;
   dev->ctx_id = -1;
   dev->mtp = MAP_FAILED;

   memset(&ver, 0, sizeof(ver));
   ver.header.id = UKP_FUNC_ID_CHECK_VERSION;
   ver.major = KBASE_UK_VERSION_MAJOR;
   ver.minor = KBASE_UK_VERSION_MINOR;
   ret = kbw_uk_call(fd, &ver, sizeof(ver));
   if (ret) {
      kbw_set_err(err, errlen, "VERSION_CHECK: %s", strerror(-ret));
      return ret;
   }
   if (ver.header.ret != 0) {
      kbw_set_err(err, errlen, "VERSION_CHECK rejected: ret=%u",
                  ver.header.ret);
      return -EPROTO;
   }
   if (!(ver.major == 10 && ver.minor >= 3) && !(ver.major > 10)) {
      kbw_set_err(err, errlen, "kernel speaks UK %u.%u, need >= 10.3",
                  ver.major, ver.minor);
      return -EPROTO;
   }
   dev->uk_major = ver.major;
   dev->uk_minor = ver.minor;

   memset(&flags, 0, sizeof(flags));
   flags.header.id = KBASE_FUNC_SET_FLAGS;
   ret = kbw_uk_call(fd, &flags, sizeof(flags));
   if (ret) {
      kbw_set_err(err, errlen, "SET_FLAGS: %s", strerror(-ret));
      return ret;
   }
   if (flags.header.ret != 0) {
      kbw_set_err(err, errlen, "SET_FLAGS rejected: ret=%u", flags.header.ret);
      return -EPROTO;
   }

   dev->mtp = mmap(NULL, KBW_PAGE_SIZE, PROT_NONE, MAP_SHARED, fd,
                   (off_t)BASE_MEM_MAP_TRACKING_HANDLE);
   if (dev->mtp == MAP_FAILED) {
      kbw_set_err(err, errlen, "mmap(tracking page): %s", strerror(errno));
      return -errno;
   }

   memset(&version, 0, sizeof(version));
   version.header.id = KBASE_FUNC_GET_VERSION;
   ret = kbw_uk_call(fd, &version, sizeof(version));
   if (ret) {
      kbw_set_err(err, errlen, "GET_VERSION: %s", strerror(-ret));
      return ret;
   }
   if (version.header.ret != 0) {
      kbw_set_err(err, errlen, "GET_VERSION rejected: ret=%u",
                  version.header.ret);
      return -EPROTO;
   }
   version.version_buffer[KBASE_GET_VERSION_BUFFER_SIZE - 1] = '\0';
   memcpy(dev->ddk_version, version.version_buffer,
          KBASE_GET_VERSION_BUFFER_SIZE);
   dev->ddk_version[KBASE_GET_VERSION_BUFFER_SIZE] = '\0';

   memset(&ctx, 0, sizeof(ctx));
   ctx.header.id = KBASE_FUNC_GET_CONTEXT_ID;
   ret = kbw_uk_call(fd, &ctx, sizeof(ctx));
   if (ret == 0 && ctx.header.ret == 0)
      dev->ctx_id = ctx.id;

   return 0;
}

static int kbw_parse_props(struct kbw_caps *caps, const void *blob)
{
   const struct kbw_gpu_props *p = blob;

   memset(caps, 0, sizeof(*caps));
   caps->gpu_id = p->raw_props.gpu_id;
   caps->product_id = p->core_props.product_id;
   caps->major_revision = p->core_props.major_revision;
   caps->minor_revision = p->core_props.minor_revision;
   caps->version_status = p->core_props.version_status;

   caps->shader_present = p->raw_props.shader_present;
   caps->tiler_present = p->raw_props.tiler_present;
   caps->l2_present = p->raw_props.l2_present;
   caps->stack_present = p->raw_props.stack_present;
   caps->as_present = p->raw_props.as_present;
   caps->js_present = p->raw_props.js_present;
   memcpy(caps->js_features, p->raw_props.js_features,
          sizeof(caps->js_features));

   caps->l2_features = p->raw_props.l2_features;
   caps->mem_features = p->raw_props.mem_features;
   caps->mmu_features = p->raw_props.mmu_features;
   caps->tiler_features = p->raw_props.tiler_features;
   memcpy(caps->texture_features, p->raw_props.texture_features,
          sizeof(caps->texture_features));
   caps->thread_features = p->raw_props.thread_features;
   caps->thread_max_threads = p->raw_props.thread_max_threads;
   caps->thread_max_workgroup_size = p->raw_props.thread_max_workgroup_size;
   caps->thread_max_barrier_size = p->raw_props.thread_max_barrier_size;
   caps->coherency_mode = p->raw_props.coherency_mode;

   caps->num_groups = p->coherency_info.num_groups;
   caps->num_core_groups = p->coherency_info.num_core_groups;

   caps->va_bits = caps->mmu_features & 0xff;
   caps->core_count = kbw_popcount64(caps->shader_present);
   caps->core_id_range = kbw_fls64(caps->shader_present);
   caps->max_job_slots = kbw_popcount64(caps->js_present);
   /* mali_base_kernel.h: the selected coherency mode, 0 = not coherent. */
   caps->io_coherent = caps->coherency_mode != 0;

   return 0;
}

static void kbw_dev_fini(struct kbase_dev *dev, bool owns_fd)
{
   if (dev->mtp && dev->mtp != MAP_FAILED) {
      munmap(dev->mtp, KBW_PAGE_SIZE);
      dev->mtp = NULL;
   }
   if (owns_fd && dev->fd >= 0)
      close(dev->fd);
   dev->fd = -1;
}

static int kbw_device_alloc_fd(int fd, bool owns_fd, const char *path,
                               struct kbw_device **out, char *err,
                               size_t errlen)
{
   struct kbw_device *dev;
   int ret;

   dev = calloc(1, sizeof(*dev));
   if (!dev)
      return -ENOMEM;

   ret = kbw_dev_init_fd(&dev->kdev, fd, owns_fd, err, errlen);
   if (ret)
      goto fail;

   {
      struct kbw_gpu_props props;
      ret = kbase_get_props(&dev->kdev, &props, err, errlen);
      if (ret)
         goto fail;
      kbw_parse_props(&dev->caps, &props);
   }

   dev->caps.uk_major = dev->kdev.uk_major;
   dev->caps.uk_minor = dev->kdev.uk_minor;
   memcpy(dev->caps.ddk_version, dev->kdev.ddk_version,
          sizeof(dev->caps.ddk_version));
   dev->owns_fd = owns_fd;
   if (path)
      snprintf(dev->path, sizeof(dev->path), "%s", path);

   *out = dev;
   return 0;

fail:
   kbw_dev_fini(&dev->kdev, owns_fd);
   free(dev);
   return ret;
}

int kbw_device_open(const char *path, struct kbw_device **out, char *err,
                    size_t errlen)
{
   int fd;

   if (!path)
      path = "/dev/mali0";
   errno = 0;
   fd = open(path, O_RDWR | O_CLOEXEC);
   if (fd < 0) {
      kbw_set_err(err, errlen, "open(%s): %s", path, strerror(errno));
      return -errno;
   }

   return kbw_device_alloc_fd(fd, true, path, out, err, errlen);
}

int kbw_device_adopt_fd(int fd, struct kbw_device **out, char *err,
                        size_t errlen)
{
   if (fd < 0)
      return -EBADF;
   return kbw_device_alloc_fd(fd, true, NULL, out, err, errlen);
}

void kbw_device_destroy(struct kbw_device *dev)
{
   if (!dev)
      return;
   kbw_dev_fini(&dev->kdev, dev->owns_fd);
   free(dev);
}

const struct kbw_caps *kbw_device_caps(const struct kbw_device *dev)
{
   return &dev->caps;
}

int kbw_device_fd(const struct kbw_device *dev)
{
   return dev->kdev.fd;
}

/* ------------------------------------------------------------------ */
/* Screen. */

int kbw_screen_create(struct kbw_device *dev, struct kbw_screen **out)
{
   struct kbw_screen *screen;

   if (!dev)
      return -EINVAL;
   screen = calloc(1, sizeof(*screen));
   if (!screen)
      return -ENOMEM;
   screen->dev = dev;
   *out = screen;
   return 0;
}

void kbw_screen_destroy(struct kbw_screen *screen)
{
   free(screen);
}

struct kbw_device *kbw_screen_device(struct kbw_screen *screen)
{
   return screen->dev;
}

/* ------------------------------------------------------------------ */
/* Context. */

static void *kbw_event_thread(void *arg)
{
   struct kbw_context *ctx = arg;
   struct pollfd pfd[2];

   pfd[0].fd = ctx->dev.fd;
   pfd[0].events = POLLIN;
   pfd[1].fd = ctx->stop_pipe[0];
   pfd[1].events = POLLIN;

   for (;;) {
      int r = poll(pfd, 2, -1);

      if (r < 0) {
         if (errno == EINTR)
            continue;
         break;
      }
      if (pfd[1].revents & POLLIN)
         break;
      if (!(pfd[0].revents & (POLLIN | POLLHUP | POLLERR)))
         continue;

      /* poll() is level triggered: one event per wakeup, then back to
       * poll() while events remain queued. */
      {
         struct base_jd_event_v2 ev;
         ssize_t n = read(ctx->dev.fd, &ev, sizeof(ev));

         if (n == 0)
            goto out;
         if (n < 0) {
            if (errno == EINTR)
               continue;
            /* EPIPE: kbase reported BASE_JD_EVENT_DRV_TERMINATED. */
            goto out;
         }
         if (n != (ssize_t)sizeof(ev))
            goto out;

         {
            pthread_mutex_lock(&ctx->lock);
            ctx->last_event[ev.atom_number] = ev.event_code;
            ctx->atom_busy[ev.atom_number] = false;
            pthread_cond_broadcast(&ctx->cond);
            pthread_mutex_unlock(&ctx->lock);
         }
      }
   }

out:
   return NULL;
}

static int kbw_stream_create(struct kbw_context *ctx, char *err, size_t errlen)
{
   struct kbw_uk_stream_create sc;
   int ret;

   memset(&sc, 0, sizeof(sc));
   sc.header.id = KBASE_FUNC_STREAM_CREATE;
   snprintf(sc.name, sizeof(sc.name), "kbw-%d", (int)getpid());
   sc.fd = -1;

   ret = kbw_uk_call(ctx->dev.fd, &sc, sizeof(sc));
   if (ret) {
      kbw_set_err(err, errlen, "STREAM_CREATE: %s", strerror(-ret));
      return ret;
   }
   if (sc.header.ret != 0 || sc.fd < 0) {
      kbw_set_err(err, errlen, "STREAM_CREATE rejected: ret=%u",
                  sc.header.ret);
      return -EPROTO;
   }
   ctx->tl_fd = sc.fd;
   return 0;
}

int kbw_context_create(struct kbw_screen *screen, uint32_t flags,
                       struct kbw_context **out, char *err, size_t errlen)
{
   struct kbw_context *ctx;
   int ret;

   if (!screen)
      return -EINVAL;

   ctx = calloc(1, sizeof(*ctx));
   if (!ctx)
      return -ENOMEM;

   ctx->screen = screen;
   ctx->tl_fd = -1;
   ctx->stop_pipe[0] = ctx->stop_pipe[1] = -1;
   ctx->slot_timeout_ns = 5000000000ll; /* 5 s */

   if (flags == KBW_CONTEXT_NEW_FD) {
      int fd = open(screen->dev->path[0] ? screen->dev->path : "/dev/mali0",
                    O_RDWR | O_CLOEXEC);
      if (fd < 0) {
         ret = -errno;
         kbw_set_err(err, errlen, "open(%s): %s",
                     screen->dev->path[0] ? screen->dev->path : "/dev/mali0",
                     strerror(errno));
         free(ctx);
         return ret;
      }
      ret = kbw_dev_init_fd(&ctx->dev, fd, true, err, errlen);
      if (ret) {
         close(fd);
         free(ctx);
         return ret;
      }
      ctx->owns_fd = true;
   } else {
      ctx->dev = screen->dev->kdev;
      ctx->owns_fd = false;
   }

   pthread_mutex_init(&ctx->lock, NULL);
   pthread_condattr_t attr;
   pthread_condattr_init(&attr);
   pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
   pthread_cond_init(&ctx->cond, &attr);
   pthread_condattr_destroy(&attr);

   if (pipe2(ctx->stop_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
      ret = -errno;
      goto fail;
   }

   ret = kbw_stream_create(ctx, err, errlen);
   if (ret)
      goto fail;

   if (pthread_create(&ctx->ev_thread, NULL, kbw_event_thread, ctx) != 0) {
      ret = -errno;
      goto fail;
   }
   ctx->ev_thread_valid = true;

   *out = ctx;
   return 0;

fail:
   if (ctx->tl_fd >= 0)
      close(ctx->tl_fd);
   if (ctx->stop_pipe[0] >= 0) {
      close(ctx->stop_pipe[0]);
      close(ctx->stop_pipe[1]);
   }
   if (ctx->owns_fd)
      kbw_dev_fini(&ctx->dev, true);
   free(ctx);
   return ret;
}

void kbw_context_destroy(struct kbw_context *ctx)
{
   if (!ctx)
      return;

   if (ctx->ev_thread_valid) {
      char c = 1;
      ssize_t ignored = write(ctx->stop_pipe[1], &c, 1);
      (void)ignored;
      pthread_join(ctx->ev_thread, NULL);
   }
   if (ctx->stop_pipe[0] >= 0) {
      close(ctx->stop_pipe[0]);
      close(ctx->stop_pipe[1]);
   }
   if (ctx->tl_fd >= 0)
      close(ctx->tl_fd);

   pthread_cond_destroy(&ctx->cond);
   pthread_mutex_destroy(&ctx->lock);

   if (ctx->owns_fd)
      kbw_dev_fini(&ctx->dev, true);

   free(ctx);
}

int kbw_context_wait_idle(struct kbw_context *ctx, int64_t timeout_ns)
{
   int64_t deadline = timeout_ns < 0 ? -1 : kbw_now_ns() + timeout_ns;
   int ret = 0;

   pthread_mutex_lock(&ctx->lock);
   for (unsigned i = 1; i < KBW_MAX_ATOMS; i++) {
      while (ctx->atom_busy[i]) {
         if (deadline < 0) {
            pthread_cond_wait(&ctx->cond, &ctx->lock);
         } else {
            struct timespec ts;
            kbw_deadline_to_ts(deadline, &ts);
            if (pthread_cond_timedwait(&ctx->cond, &ctx->lock, &ts) ==
                ETIMEDOUT) {
               ret = -ETIME;
               goto out;
            }
         }
      }
   }
out:
   pthread_mutex_unlock(&ctx->lock);
   return ret;
}

/* ------------------------------------------------------------------ */
/* Atom slots. */

static int kbw_alloc_slots_locked(struct kbw_context *ctx, unsigned count,
                                  uint8_t *slots, int64_t deadline)
{
   unsigned found = 0;
   int ret = 0;

   while (found < count) {
      unsigned i;

      for (i = 1; i < KBW_MAX_ATOMS && found < count; i++) {
         if (!ctx->atom_busy[i]) {
            ctx->atom_busy[i] = true;
            slots[found++] = (uint8_t)i;
         }
      }
      if (found == count)
         break;

      /* Not enough free slots: undo and wait for completions. */
      for (unsigned k = 0; k < found; k++)
         ctx->atom_busy[slots[k]] = false;
      found = 0;

      if (deadline < 0) {
         pthread_cond_wait(&ctx->cond, &ctx->lock);
      } else {
         struct timespec ts;
         kbw_deadline_to_ts(deadline, &ts);
         if (pthread_cond_timedwait(&ctx->cond, &ctx->lock, &ts) ==
             ETIMEDOUT) {
            ret = -ETIME;
            break;
         }
      }
   }

   return ret;
}

static void kbw_release_slot(struct kbw_context *ctx, uint8_t slot)
{
   pthread_mutex_lock(&ctx->lock);
   ctx->atom_busy[slot] = false;
   pthread_cond_broadcast(&ctx->cond);
   pthread_mutex_unlock(&ctx->lock);
}

/* ------------------------------------------------------------------ */
/* Buffer objects. */

/* MEM_ALLOC + SAME_VA cookie mmap.  Growable allocations (for example a
 * tiler heap) commit a single page initially; kbase would otherwise back the
 * whole VA range at allocation time. */
static int kbw_mem_alloc(struct kbw_context *ctx, uint64_t pages,
                         uint64_t flags, struct kbase_bo *bo, char *err,
                         size_t errlen)
{
   struct kbase_uk_mem_alloc alloc;
   void *cpu;
   int ret;

   memset(bo, 0, sizeof(*bo));
   memset(&alloc, 0, sizeof(alloc));
   alloc.header.id = KBASE_FUNC_MEM_ALLOC;
   alloc.va_pages = pages;
   alloc.commit_pages = (flags & BASE_MEM_GROW_ON_GPF) ? 1 : pages;
   alloc.extent = 0;
   alloc.flags = flags;

   ret = kbw_uk_call(ctx->dev.fd, &alloc, sizeof(alloc));
   if (ret) {
      kbw_set_err(err, errlen, "MEM_ALLOC ioctl: %s", strerror(-ret));
      return ret;
   }
   if (alloc.header.ret != 0) {
      kbw_set_err(err, errlen, "MEM_ALLOC rejected: ret=%u", alloc.header.ret);
      return -EPROTO;
   }

   cpu = mmap(NULL, pages * KBW_PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
              ctx->dev.fd, (off_t)alloc.gpu_va);
   if (cpu == MAP_FAILED) {
      kbw_set_err(err, errlen, "mmap(cookie 0x%llx): %s",
                  (unsigned long long)alloc.gpu_va, strerror(errno));
      return -errno;
   }

   bo->cookie = alloc.gpu_va;
   bo->va_pages = pages;
   bo->flags = alloc.flags;
   bo->cpu = cpu;
   bo->gpu_va = (uint64_t)(uintptr_t)cpu;
   return 0;
}

static int kbw_mem_free(struct kbw_context *ctx, struct kbase_bo *bo)
{
   struct kbase_uk_mem_free mem;
   int ret;

   if (!bo->cpu)
      return 0;

   if (bo->flags & BASE_MEM_SAME_VA) {
      if (munmap(bo->cpu, bo->va_pages * KBW_PAGE_SIZE) != 0)
         return -errno;
   } else {
      memset(&mem, 0, sizeof(mem));
      mem.header.id = KBASE_FUNC_MEM_FREE;
      mem.gpu_addr = bo->cookie;
      ret = kbw_uk_call(ctx->dev.fd, &mem, sizeof(mem));
      if (ret == 0 && mem.header.ret != 0)
         ret = -EPROTO;
      if (ret)
         return ret;
   }
   memset(bo, 0, sizeof(*bo));
   return 0;
}

int kbw_bo_create(struct kbw_context *ctx, uint64_t size, uint64_t flags,
                  struct kbw_bo **out, char *err, size_t errlen)
{
   struct kbw_bo *bo;
   uint64_t pages;
   int ret;

   if (!ctx || !size)
      return -EINVAL;
   if (flags == 0)
      flags = KBW_BO_DEFAULT_FLAGS;

   pages = KBW_ALIGN(size, KBW_PAGE_SIZE) / KBW_PAGE_SIZE;
   if (!pages)
      pages = 1;

   bo = calloc(1, sizeof(*bo));
   if (!bo)
      return -ENOMEM;

   ret = kbw_mem_alloc(ctx, pages, flags, &bo->base, err, errlen);
   if (ret) {
      free(bo);
      return ret;
   }

   bo->ctx = ctx;
   bo->size = pages * KBW_PAGE_SIZE;
   bo->flags = bo->base.flags;
   *out = bo;
   return 0;
}

void kbw_bo_destroy(struct kbw_bo *bo)
{
   if (!bo)
      return;
   kbw_mem_free(bo->ctx, &bo->base);
   free(bo);
}

void *kbw_bo_cpu(const struct kbw_bo *bo)
{
   return bo ? bo->base.cpu : NULL;
}

uint64_t kbw_bo_gpu_va(const struct kbw_bo *bo)
{
   return bo ? bo->base.gpu_va : 0;
}

uint64_t kbw_bo_size(const struct kbw_bo *bo)
{
   return bo ? bo->size : 0;
}

int kbw_bo_fd(const struct kbw_bo *bo)
{
   return bo ? bo->ctx->dev.fd : -1;
}

/* ------------------------------------------------------------------ */
/* Fences. */

int kbw_fence_from_fd(int fd, struct kbw_fence **out)
{
   struct kbw_fence *fence;

   if (fd < 0)
      return -EBADF;
   fence = calloc(1, sizeof(*fence));
   if (!fence)
      return -ENOMEM;
   fence->fd = fd;
   fence->refcount = 1;
   *out = fence;
   return 0;
}

struct kbw_fence *kbw_fence_ref(struct kbw_fence *fence)
{
   if (fence)
      __atomic_add_fetch(&fence->refcount, 1, __ATOMIC_RELAXED);
   return fence;
}

void kbw_fence_unref(struct kbw_fence *fence)
{
   if (!fence)
      return;
   if (__atomic_sub_fetch(&fence->refcount, 1, __ATOMIC_ACQ_REL) != 0)
      return;
   if (fence->fd >= 0)
      close(fence->fd);
   free(fence);
}

static int kbw_poll_fd(int fd, int64_t timeout_ns)
{
   struct pollfd pfd = {.fd = fd, .events = POLLIN};

   for (;;) {
      int r = poll(&pfd, 1, timeout_ns < 0 ? -1 : (int)(timeout_ns / 1000000));

      if (r < 0) {
         if (errno == EINTR)
            continue;
         return -errno;
      }
      if (r == 0)
         return -ETIME;
      if (pfd.revents & (POLLIN | POLLHUP | POLLERR))
         return 0;
   }
}

int kbw_fence_wait(struct kbw_fence *fence, int64_t timeout_ns)
{
   int ret;

   if (!fence)
      return -EINVAL;
   if (fence->signaled)
      return 0;
   if (fence->fd < 0)
      return -EINVAL;

   ret = kbw_poll_fd(fence->fd, timeout_ns);
   if (ret == 0)
      fence->signaled = true;
   return ret;
}

bool kbw_fence_is_signaled(struct kbw_fence *fence)
{
   if (!fence)
      return true;
   if (fence->signaled)
      return true;
   if (fence->fd >= 0 && kbw_poll_fd(fence->fd, 0) == 0)
      fence->signaled = true;
   return fence->signaled;
}

int kbw_fence_dup_fd(struct kbw_fence *fence)
{
   if (!fence || fence->fd < 0)
      return -EINVAL;
   return fcntl(fence->fd, F_DUPFD_CLOEXEC, 0);
}

/* ------------------------------------------------------------------ */
/* Submission.
 *
 * Wire order inside one KBASE_FUNC_JOB_SUBMIT:
 *   [fence-wait soft atom] [hardware atoms...] [fence-trigger soft atom]
 * kbase resolves dependencies against the persistent 256-atom table, and a
 * dependency on a slot that is UNUSED/JOB_INVALID fails the dependent atom
 * (mali_kbase_jd.c:1100-1160).  The wait atom therefore precedes the jobs,
 * and the trigger atom follows them. */

struct kbw_submit_frame {
   struct base_jd_atom_v2 atoms[KBW_MAX_ATOMS];
   uint8_t wait_slot;
   uint8_t trigger_slot;
   uint8_t hw_slots[KBW_MAX_ATOMS];
   struct kbw_base_fence wait_fence;
   struct kbw_base_fence trig_fence;
   uint8_t slot[KBW_MAX_ATOMS];
   unsigned n_atoms;
};

int kbw_submit(struct kbw_context *ctx, const struct kbw_submit_info *info)
{
   struct kbw_submit_frame *f;
   unsigned alloc_count, idx = 0;
   int64_t deadline;
   int ret;

   if (!ctx || !info || !info->atoms || info->atom_count == 0)
      return -EINVAL;
   if (info->atom_count > KBW_MAX_ATOMS - 3)
      return -E2BIG;

   f = calloc(1, sizeof(*f));
   if (!f)
      return -ENOMEM;
   f->wait_slot = 0;
   f->trigger_slot = 0;
   f->wait_fence.basep.fd = -1;
   f->wait_fence.basep.stream_fd = -1;
   f->trig_fence.basep.fd = -1;
   f->trig_fence.basep.stream_fd = ctx->tl_fd;

   for (unsigned i = 0; i < info->atom_count; i++) {
      const struct kbw_atom *a = &info->atoms[i];

      if (!a->jc)
         goto einval;
      if (!a->core_req)
         goto einval;
      for (unsigned d = 0; d < 2; d++) {
         if (a->dep[d] < 0)
            continue;
         if ((unsigned)a->dep[d] >= info->atom_count ||
             (unsigned)a->dep[d] == i)
            goto einval;
         if (a->dep_type[d] != BASE_JD_DEP_TYPE_DATA &&
             a->dep_type[d] != BASE_JD_DEP_TYPE_ORDER)
            goto einval;
      }
   }

   if (info->in_fence_fd >= 0)
      f->wait_fence.basep.fd = info->in_fence_fd;

   alloc_count = info->atom_count + (info->in_fence_fd >= 0 ? 1 : 0) + 1;
   deadline = ctx->slot_timeout_ns < 0 ? -1
                                       : kbw_now_ns() + ctx->slot_timeout_ns;

   pthread_mutex_lock(&ctx->lock);
   ret = kbw_alloc_slots_locked(ctx, alloc_count, f->slot, deadline);
   pthread_mutex_unlock(&ctx->lock);
   if (ret)
      goto out;

   if (info->in_fence_fd >= 0)
      f->wait_slot = f->slot[idx++];
   for (unsigned i = 0; i < info->atom_count; i++)
      f->hw_slots[i] = f->slot[idx++];
   f->trigger_slot = f->slot[idx++];

   /* Fence-wait soft job, if any. */
   if (f->wait_slot) {
      struct base_jd_atom_v2 *a = &f->atoms[f->n_atoms++];
      a->jc = (uint64_t)(uintptr_t)&f->wait_fence;
      a->core_req = BASE_JD_REQ_SOFT_FENCE_WAIT;
      a->atom_number = f->wait_slot;
      a->prio = BASE_JD_PRIO_MEDIUM;
      a->pre_dep[0].atom_id = 0;
      a->pre_dep[1].atom_id = 0;
   }

   for (unsigned i = 0; i < info->atom_count; i++) {
      const struct kbw_atom *src = &info->atoms[i];
      struct base_jd_atom_v2 *a = &f->atoms[f->n_atoms++];
      unsigned dep_slot = 0;

      a->jc = src->jc;
      a->udata.blob[0] = src->udata[0];
      a->udata.blob[1] = src->udata[1];
      a->core_req = src->core_req;
      a->atom_number = f->hw_slots[i];
      a->prio = src->prio;
      a->device_nr = src->device_nr;

      for (unsigned d = 0; d < 2; d++) {
         if (src->dep[d] >= 0) {
            a->pre_dep[d].atom_id = f->hw_slots[(unsigned)src->dep[d]];
            a->pre_dep[d].dependency_type = src->dep_type[d];
         } else {
            a->pre_dep[d].atom_id = 0;
            a->pre_dep[d].dependency_type = BASE_JD_DEP_TYPE_INVALID;
         }
      }

      /* Serialize on the in-fence when there is a free dependency slot.
       * Callers that fill both slots are responsible for their own chain. */
      if (f->wait_slot) {
         dep_slot = (a->pre_dep[0].atom_id == 0) ? 0 : 1;
         if (a->pre_dep[dep_slot].atom_id == 0) {
            a->pre_dep[dep_slot].atom_id = f->wait_slot;
            a->pre_dep[dep_slot].dependency_type = BASE_JD_DEP_TYPE_ORDER;
         }
      }
   }

   /* Fence-trigger soft job: signals the out-fence fd when the last
    * hardware atom completes. */
   {
      struct base_jd_atom_v2 *a = &f->atoms[f->n_atoms++];

      a->jc = (uint64_t)(uintptr_t)&f->trig_fence;
      a->core_req = BASE_JD_REQ_SOFT_FENCE_TRIGGER;
      a->atom_number = f->trigger_slot;
      a->prio = BASE_JD_PRIO_MEDIUM;
      a->pre_dep[0].atom_id = f->hw_slots[info->atom_count - 1];
      a->pre_dep[0].dependency_type = BASE_JD_DEP_TYPE_ORDER;
      a->pre_dep[1].atom_id = 0;
   }

   ret = kbase_submit_atoms(&ctx->dev, f->atoms, f->n_atoms);
   if (ret) {
      /* kbase_jd_submit() only fails before consuming atoms (size/stride
       * validation), so every slot can be returned. */
      goto out_release_failed_submit;
   }

   if (info->out_fence_fd) {
      if (f->trig_fence.basep.fd >= 0) {
         *info->out_fence_fd = f->trig_fence.basep.fd;
      } else {
         *info->out_fence_fd = -1;
         ret = -EPROTO;
      }
   } else if (f->trig_fence.basep.fd >= 0) {
      close(f->trig_fence.basep.fd);
   }

   free(f);
   return ret;

out_release_failed_submit:
   pthread_mutex_lock(&ctx->lock);
   for (unsigned i = 0; i < alloc_count; i++)
      ctx->atom_busy[f->slot[i]] = false;
   pthread_cond_broadcast(&ctx->cond);
   pthread_mutex_unlock(&ctx->lock);
out:
   return ret;
einval:
   free(f);
   return -EINVAL;
}

/* ------------------------------------------------------------------ */
/* Soft job round trip. */

int kbw_soft_dump_time(struct kbw_context *ctx, struct kbw_bo *bo,
                       struct base_dump_cpu_gpu_counters *dump)
{
   struct base_jd_atom_v2 atom;
   uint8_t slot;
   int64_t deadline;
   int ret;
   unsigned code = 0;

   if (!ctx || !bo || !dump)
      return -EINVAL;
   if ((kbw_bo_gpu_va(bo) & 63) != 0)
      return -EINVAL;
   if (!(bo->flags & BASE_MEM_PROT_GPU_WR))
      return -EINVAL;

   deadline = ctx->slot_timeout_ns < 0 ? -1
                                       : kbw_now_ns() + ctx->slot_timeout_ns;

   pthread_mutex_lock(&ctx->lock);
   ret = kbw_alloc_slots_locked(ctx, 1, &slot, deadline);
   pthread_mutex_unlock(&ctx->lock);
   if (ret)
      return ret;

   memset(&atom, 0, sizeof(atom));
   atom.jc = kbw_bo_gpu_va(bo);
   atom.udata.blob[0] = 0x6b62775f64756d70ull; /* "kbw_dump" */
   atom.udata.blob[1] = kbw_bo_gpu_va(bo);
   atom.core_req = BASE_JD_REQ_SOFT_DUMP_CPU_GPU_TIME;
   atom.compat_core_req =
      (uint16_t)(BASE_JD_REQ_SOFT_DUMP_CPU_GPU_TIME & 0x7fff);
   atom.atom_number = slot;
   atom.prio = BASE_JD_PRIO_MEDIUM;
   atom.pre_dep[0].dependency_type = BASE_JD_DEP_TYPE_INVALID;
   atom.pre_dep[1].dependency_type = BASE_JD_DEP_TYPE_INVALID;

   ret = kbase_submit_atoms(&ctx->dev, &atom, 1);
   if (ret) {
      kbw_release_slot(ctx, slot);
      return ret;
   }

   pthread_mutex_lock(&ctx->lock);
   while (ctx->atom_busy[slot]) {
      if (deadline < 0) {
         pthread_cond_wait(&ctx->cond, &ctx->lock);
      } else {
         struct timespec ts;
         kbw_deadline_to_ts(deadline, &ts);
         if (pthread_cond_timedwait(&ctx->cond, &ctx->lock, &ts) ==
             ETIMEDOUT) {
            pthread_mutex_unlock(&ctx->lock);
            return -ETIME;
         }
      }
   }
   code = ctx->last_event[slot];
   pthread_mutex_unlock(&ctx->lock);

   if (code != BASE_JD_EVENT_DONE)
      return (int)code;

   memcpy(dump, bo->base.cpu, sizeof(*dump));
   return (int)code;
}

/* ------------------------------------------------------------------ */

const char *kbw_strerror(int err)
{
   switch (err) {
   case 0:
      return "ok";
   case -ETIME:
      return "timed out";
   case -EPROTO:
      return "protocol error";
   case -EPIPE:
      return "device terminated";
   case -E2BIG:
      return "argument buffer too large";
   case -ENOMEM:
      return "out of memory";
   case -EINVAL:
      return "invalid argument";
   default:
      if (err < 0)
         return strerror(-err);
      return kbase_event_name((unsigned)err);
   }
}
