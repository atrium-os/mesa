/*
 * vn_renderer_atrium — venus backend over /dev/atrium-gpu0.
 *
 * Atrium-specific sibling to vn_renderer_virtgpu.c (Linux/libdrm)
 * and vn_renderer_vtest.c (TCP test). Implements the same vn_renderer
 * vtable, routing all virtio-gpu commands through the Atrium GPU
 * cdev's ioctls instead of /dev/dri/renderD*.
 *
 * What's implemented (V5b, MVP for vulkaninfo):
 *   - probe + open + capset query + ctx_init   (V5)
 *   - submit / wait                            (V5)
 *   - bo: create_from_device_memory, destroy, map, flush, invalidate
 *   - shmem: create / destroy
 *   - sync: in-memory monotonic timeline counter
 *
 * Not implemented (returns VK_ERROR_FEATURE_NOT_PRESENT):
 *   - bo create_from_dma_buf, export_dma_buf, export_sync_file
 *     (no DMA-buf on FreeBSD; D5 atrium-mesa-radv uses cross-cdev
 *     blob handles when needed)
 *   - sync create_from_syncobj, export_syncobj
 *     (Vulkan external semaphore extensions; deferred)
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Atrium contributors.
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "vn_renderer.h"
#include "atrium_gpu_uapi.h"

#define ATRIUM_DEV_PATH  "/dev/atrium-gpu0"

/* ---------- types ---------- */

struct atrium_renderer {
   struct vn_renderer base;

   struct vn_instance *instance;
   int fd;
   uint32_t ctx_id;

   /* Probed venus capset payload from the host. */
   uint8_t  capset_data[256];
   uint32_t capset_size;
};

struct atrium_shmem {
   struct vn_renderer_shmem base;
   uint32_t bo_handle;        /* atrium-gpu BO */
};

struct atrium_bo {
   struct vn_renderer_bo base;
   uint32_t bo_handle;        /* atrium-gpu BO */
   size_t   size;
};

/* In-memory timeline counter. venus uses syncs as VkSemaphore-shaped
 * timelines. Submits are synchronous in V5 (the kmod's req_resp
 * blocks until the host fence retires), so any sync we need to
 * signal we signal immediately on submit completion. */
struct atrium_sync {
   struct vn_renderer_sync base;
   pthread_mutex_t lock;
   uint64_t value;
};

/* ---------- helpers ---------- */

static struct atrium_renderer *
to_renderer(struct vn_renderer *r)
{
   return (struct atrium_renderer *)r;
}

/* Allocate + register a guest BO via the cdev. Returns the BO handle
 * and, optionally, an mmap'd CPU pointer + the size rounded to page
 * alignment. */
static VkResult
atrium_alloc_bo(struct atrium_renderer *r, size_t size, uint32_t flags,
                uint32_t *out_handle, size_t *out_size, void **out_map)
{
   struct atrium_gpu_alloc alloc = {
      .size = size, .flags = flags,
   };
   if (ioctl(r->fd, ATRIUM_GPU_IOC_ALLOC, &alloc) < 0)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   *out_handle = alloc.handle;
   *out_size   = alloc.size;

   if (out_map) {
      void *p = mmap(NULL, alloc.size, PROT_READ | PROT_WRITE,
                     MAP_SHARED, r->fd, alloc.mmap_offset);
      if (p == MAP_FAILED) {
         /* TODO: free the BO on the cdev side once IOC_FREE is wired */
         return VK_ERROR_MEMORY_MAP_FAILED;
      }
      *out_map = p;
   }
   return VK_SUCCESS;
}

/* ---------- ops: top-level ---------- */

static void
atrium_destroy(struct vn_renderer *renderer,
               const VkAllocationCallbacks *alloc)
{
   struct atrium_renderer *r = to_renderer(renderer);
   if (r->fd >= 0)
      close(r->fd);  /* triggers kmod CTX_DESTROY in dtor */
   vk_free(alloc, r);
}

static VkResult
atrium_submit(struct vn_renderer *renderer,
              const struct vn_renderer_submit *submit)
{
   struct atrium_renderer *r = to_renderer(renderer);

   for (uint32_t i = 0; i < submit->batch_count; i++) {
      const struct vn_renderer_submit_batch *b = &submit->batches[i];

      if (b->cs_size > 0) {
         struct atrium_gpu_submit_3d sub = {
            .cmd_ptr  = (uint64_t)(uintptr_t)b->cs_data,
            .cmd_size = (uint32_t)b->cs_size,
            .flags    = ATRIUM_GPU_SUBMIT_3D_SIGNAL_FENCE,
         };
         fprintf(stderr, "[atrium-vn] SUBMIT_3D ring=%u sz=%u\n",
                 b->ring_idx, (unsigned)b->cs_size);
         if (ioctl(r->fd, ATRIUM_GPU_IOC_SUBMIT_3D, &sub) < 0) {
            fprintf(stderr, "[atrium-vn] SUBMIT_3D failed: %s\n",
                    strerror(errno));
            return VK_ERROR_DEVICE_LOST;
         }
      }

      /* Submit is synchronous (host fence retires before SUBMIT_3D
       * returns), so any syncs the caller wants signaled are
       * signaled now. */
      for (uint32_t s = 0; s < b->sync_count; s++) {
         struct atrium_sync *sync = (struct atrium_sync *)b->syncs[s];
         pthread_mutex_lock(&sync->lock);
         if (b->sync_values[s] > sync->value)
            sync->value = b->sync_values[s];
         pthread_mutex_unlock(&sync->lock);
      }
   }
   return VK_SUCCESS;
}

static VkResult
atrium_wait(struct vn_renderer *renderer,
            const struct vn_renderer_wait *wait)
{
   /* Synchronous submit means every sync the caller could be waiting
    * on is already signaled. Just check we have actually-signaled
    * values for each requested sync (caller bug otherwise). */
   (void)renderer;
   for (uint32_t i = 0; i < wait->sync_count; i++) {
      struct atrium_sync *sync = (struct atrium_sync *)wait->syncs[i];
      pthread_mutex_lock(&sync->lock);
      uint64_t cur = sync->value;
      pthread_mutex_unlock(&sync->lock);
      if (cur < wait->sync_values[i]) {
         /* If a guest waits on an unsignaled sync, that's a bug
          * in the venus protocol layer above us under our sync
          * model. Return VK_TIMEOUT (matches vtest behavior). */
         if (wait->wait_any)
            continue;
         return VK_TIMEOUT;
      }
   }
   return VK_SUCCESS;
}

/* ---------- ops: shmem ---------- */

static struct vn_renderer_shmem *
atrium_shmem_create(struct vn_renderer *renderer, size_t size)
{
   struct atrium_renderer *r = to_renderer(renderer);
   struct atrium_shmem *shmem = calloc(1, sizeof(*shmem));
   if (!shmem) return NULL;

   uint32_t handle;
   size_t   real_size;
   void    *map_ptr;
   VkResult result = atrium_alloc_bo(r, size,
       ATRIUM_GPU_BO_GPU_VISIBLE | ATRIUM_GPU_BO_CPU_VISIBLE
       | ATRIUM_GPU_BO_COHERENT,
       &handle, &real_size, &map_ptr);
   if (result != VK_SUCCESS) {
      free(shmem);
      return NULL;
   }

   /* Expose to the host as a guest-backed blob resource. Without
    * RESOURCE_ATTACH the host renderer has no idea this shmem region
    * exists, and the venus ring buffer the frontend writes into is
    * invisible to the worker — the ring's alive-seqno never advances
    * and the frontend hangs in vn_ring_wait_alive. */
   struct atrium_gpu_resource_attach ra = {
      .bo_handle  = handle,
      .blob_mem   = ATRIUM_GPU_BLOB_MEM_GUEST,
      .blob_flags = ATRIUM_GPU_BLOB_USE_MAPPABLE,
      .blob_id    = 0,  /* shmem isn't a venus VkDeviceMemory */
   };
   if (ioctl(r->fd, ATRIUM_GPU_IOC_RESOURCE_ATTACH, &ra) < 0) {
      munmap(map_ptr, real_size);
      uint32_t h = handle;
      ioctl(r->fd, ATRIUM_GPU_IOC_FREE, &h);
      free(shmem);
      return NULL;
   }

   shmem->bo_handle           = handle;
   shmem->base.refcount       = VN_REFCOUNT_INIT(1);
   shmem->base.res_id         = ra.resource_id_out;
   shmem->base.mmap_size      = real_size;
   shmem->base.mmap_ptr       = map_ptr;
   shmem->base.cache_timestamp = 0;
   list_inithead(&shmem->base.cache_head);
   return &shmem->base;
}

static void
atrium_shmem_destroy(struct vn_renderer *renderer,
                     struct vn_renderer_shmem *base)
{
   struct atrium_renderer *r = to_renderer(renderer);
   struct atrium_shmem *shmem = (struct atrium_shmem *)base;
   if (base->mmap_ptr)
      munmap(base->mmap_ptr, base->mmap_size);
   uint32_t h = shmem->bo_handle;
   ioctl(r->fd, ATRIUM_GPU_IOC_FREE, &h);
   free(shmem);
}

/* ---------- ops: bo ---------- */

static VkResult
atrium_bo_create_from_device_memory(struct vn_renderer *renderer,
                                    VkDeviceSize size,
                                    vn_object_id mem_id,
                                    VkMemoryPropertyFlags flags,
                                    VkExternalMemoryHandleTypeFlags etypes,
                                    struct vn_renderer_bo **out_bo)
{
   struct atrium_renderer *r = to_renderer(renderer);
   struct atrium_bo *bo = calloc(1, sizeof(*bo));
   if (!bo) return VK_ERROR_OUT_OF_HOST_MEMORY;

   uint32_t handle;
   size_t   real_size;
   uint32_t bo_flags = ATRIUM_GPU_BO_GPU_VISIBLE;
   if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
      bo_flags |= ATRIUM_GPU_BO_CPU_VISIBLE | ATRIUM_GPU_BO_COHERENT;

   VkResult result = atrium_alloc_bo(r, size, bo_flags,
       &handle, &real_size, NULL);
   if (result != VK_SUCCESS) {
      free(bo);
      return result;
   }

   /* Attach as a venus host-allocated blob backed by guest pages
    * (BLOB_MEM_GUEST). venus's mem_id becomes the blob_id, so the
    * host renderer can correlate the resource with the
    * VkDeviceMemory it allocated. */
   struct atrium_gpu_resource_attach ra = {
      .bo_handle  = handle,
      .blob_mem   = ATRIUM_GPU_BLOB_MEM_GUEST,
      .blob_flags = ATRIUM_GPU_BLOB_USE_MAPPABLE,
      .blob_id    = mem_id,
   };
   if (ioctl(r->fd, ATRIUM_GPU_IOC_RESOURCE_ATTACH, &ra) < 0) {
      uint32_t h = handle;
      ioctl(r->fd, ATRIUM_GPU_IOC_FREE, &h);
      free(bo);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }

   bo->bo_handle      = handle;
   bo->size           = real_size;
   bo->base.refcount  = VN_REFCOUNT_INIT(1);
   bo->base.res_id    = ra.resource_id_out;
   bo->base.mmap_size = real_size;
   bo->base.mmap_ptr  = NULL;  /* lazy via map() */
   *out_bo = &bo->base;
   (void)etypes;
   return VK_SUCCESS;
}

static VkResult
atrium_bo_create_from_dma_buf(struct vn_renderer *renderer,
                              VkDeviceSize size, int fd,
                              VkMemoryPropertyFlags flags,
                              struct vn_renderer_bo **out_bo)
{
   /* No DMA-buf on FreeBSD. */
   (void)renderer; (void)size; (void)fd; (void)flags; (void)out_bo;
   return VK_ERROR_FEATURE_NOT_PRESENT;
}

static bool
atrium_bo_destroy(struct vn_renderer *renderer, struct vn_renderer_bo *base)
{
   struct atrium_renderer *r = to_renderer(renderer);
   struct atrium_bo *bo = (struct atrium_bo *)base;
   if (base->mmap_ptr)
      munmap(base->mmap_ptr, base->mmap_size);
   uint32_t h = bo->bo_handle;
   ioctl(r->fd, ATRIUM_GPU_IOC_FREE, &h);
   free(bo);
   return true;
}

static int
atrium_bo_export_dma_buf(struct vn_renderer *renderer,
                         struct vn_renderer_bo *bo)
{
   (void)renderer; (void)bo;
   return -1;
}

static int
atrium_bo_export_sync_file(struct vn_renderer *renderer,
                           struct vn_renderer_bo *bo)
{
   (void)renderer; (void)bo;
   return -1;
}

static void *
atrium_bo_map(struct vn_renderer *renderer,
              struct vn_renderer_bo *base, void *placed_addr)
{
   struct atrium_renderer *r = to_renderer(renderer);
   struct atrium_bo *bo = (struct atrium_bo *)base;

   if (base->mmap_ptr)
      return base->mmap_ptr;

   /* placed_addr is a hint from the caller; honor it via MAP_FIXED
    * if non-NULL so venus can place mappings deterministically. */
   void *p = mmap(placed_addr, bo->size, PROT_READ | PROT_WRITE,
                  MAP_SHARED | (placed_addr ? MAP_FIXED : 0),
                  r->fd, ((uint64_t)bo->bo_handle) * 0x10000ULL);
   if (p == MAP_FAILED)
      return NULL;
   base->mmap_ptr = p;
   return p;
}

static void
atrium_bo_flush(struct vn_renderer *renderer, struct vn_renderer_bo *bo,
                VkDeviceSize offset, VkDeviceSize size)
{
   /* COHERENT BOs need no explicit flush. atrium-gpu IOC_SYNC is a
    * no-op in v0.1; revisit when we add non-coherent paths. */
   (void)renderer; (void)bo; (void)offset; (void)size;
}

static void
atrium_bo_invalidate(struct vn_renderer *renderer, struct vn_renderer_bo *bo,
                     VkDeviceSize offset, VkDeviceSize size)
{
   (void)renderer; (void)bo; (void)offset; (void)size;
}

/* ---------- ops: sync (in-memory monotonic) ---------- */

static VkResult
atrium_sync_create(struct vn_renderer *renderer, uint64_t initial_val,
                   uint32_t flags, struct vn_renderer_sync **out_sync)
{
   (void)renderer; (void)flags;
   struct atrium_sync *sync = calloc(1, sizeof(*sync));
   if (!sync) return VK_ERROR_OUT_OF_HOST_MEMORY;
   pthread_mutex_init(&sync->lock, NULL);
   sync->value = initial_val;
   sync->base.sync_id = (uintptr_t)sync & 0xffffffff;
   *out_sync = &sync->base;
   return VK_SUCCESS;
}

static VkResult
atrium_sync_create_from_syncobj(struct vn_renderer *renderer, int fd,
                                bool sync_file,
                                struct vn_renderer_sync **out_sync)
{
   (void)renderer; (void)fd; (void)sync_file; (void)out_sync;
   return VK_ERROR_FEATURE_NOT_PRESENT;
}

static void
atrium_sync_destroy(struct vn_renderer *renderer,
                    struct vn_renderer_sync *base)
{
   (void)renderer;
   struct atrium_sync *sync = (struct atrium_sync *)base;
   pthread_mutex_destroy(&sync->lock);
   free(sync);
}

static int
atrium_sync_export_syncobj(struct vn_renderer *renderer,
                           struct vn_renderer_sync *sync, bool sync_file)
{
   (void)renderer; (void)sync; (void)sync_file;
   return -1;
}

static VkResult
atrium_sync_reset(struct vn_renderer *renderer,
                  struct vn_renderer_sync *base, uint64_t initial_val)
{
   (void)renderer;
   struct atrium_sync *sync = (struct atrium_sync *)base;
   pthread_mutex_lock(&sync->lock);
   sync->value = initial_val;
   pthread_mutex_unlock(&sync->lock);
   return VK_SUCCESS;
}

static VkResult
atrium_sync_read(struct vn_renderer *renderer,
                 struct vn_renderer_sync *base, uint64_t *val)
{
   (void)renderer;
   struct atrium_sync *sync = (struct atrium_sync *)base;
   pthread_mutex_lock(&sync->lock);
   *val = sync->value;
   pthread_mutex_unlock(&sync->lock);
   return VK_SUCCESS;
}

static VkResult
atrium_sync_write(struct vn_renderer *renderer,
                  struct vn_renderer_sync *base, uint64_t val)
{
   (void)renderer;
   struct atrium_sync *sync = (struct atrium_sync *)base;
   pthread_mutex_lock(&sync->lock);
   if (val > sync->value) sync->value = val;
   pthread_mutex_unlock(&sync->lock);
   return VK_SUCCESS;
}

/* ---------- backend init ---------- */

static VkResult
atrium_open(struct atrium_renderer *r)
{
   r->fd = open(ATRIUM_DEV_PATH, O_RDWR | O_CLOEXEC);
   if (r->fd < 0)
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   return VK_SUCCESS;
}

static VkResult
atrium_init_capset(struct atrium_renderer *r)
{
   struct atrium_gpu_capset_query q = {
      .capset_id = ATRIUM_GPU_CAPSET_VENUS,
   };
   if (ioctl(r->fd, ATRIUM_GPU_IOC_CAPSET_QUERY, &q) < 0)
      return VK_ERROR_INITIALIZATION_FAILED;
   if (q.data_size == 0)
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   if (q.data_size > sizeof(r->capset_data))
      return VK_ERROR_INITIALIZATION_FAILED;

   q.data_ptr  = (uint64_t)(uintptr_t)r->capset_data;
   if (ioctl(r->fd, ATRIUM_GPU_IOC_CAPSET_QUERY, &q) < 0)
      return VK_ERROR_INITIALIZATION_FAILED;
   r->capset_size = q.data_size;
   return VK_SUCCESS;
}

static VkResult
atrium_init_context(struct atrium_renderer *r)
{
   struct atrium_gpu_ctx_init ci = {
      .capset_id  = ATRIUM_GPU_CAPSET_VENUS,
      .debug_name = "atrium-mesa-venus",
   };
   if (ioctl(r->fd, ATRIUM_GPU_IOC_CTX_INIT, &ci) < 0)
      return VK_ERROR_INITIALIZATION_FAILED;
   r->ctx_id = ci.ctx_id_out;
   return VK_SUCCESS;
}

static void
atrium_init_renderer_info(struct atrium_renderer *r)
{
   struct vn_renderer_info *info = &r->base.info;
   memset(info, 0, sizeof(*info));

   /* venus capset payload header: 16 bytes of u32 versions, then a
    * vk_extension_mask (struct vn_info_capset). */
   if (r->capset_size >= 16) {
      const uint32_t *cap = (const uint32_t *)r->capset_data;
      info->wire_format_version                       = cap[0];
      info->vk_xml_version                            = cap[1];
      info->vk_ext_command_serialization_spec_version = cap[2];
      info->vk_mesa_venus_protocol_spec_version       = cap[3];
   }
   if (r->capset_size >= 16 + sizeof(info->vk_extension_mask)) {
      memcpy(info->vk_extension_mask, r->capset_data + 16,
             sizeof(info->vk_extension_mask));
   }

   /* Atrium-specific: no DMA-buf, no implicit fencing, no DRM. */
   info->has_dma_buf_import   = false;
   info->has_external_sync    = false;
   info->has_implicit_fencing = false;
   info->has_guest_vram       = false;
   info->max_timeline_count   = 1;
}

static VkResult
atrium_init(struct atrium_renderer *r)
{
   VkResult result = atrium_open(r);
   if (result != VK_SUCCESS) return result;
   result = atrium_init_capset(r);
   if (result != VK_SUCCESS) return result;
   result = atrium_init_context(r);
   if (result != VK_SUCCESS) return result;
   atrium_init_renderer_info(r);

   r->base.ops.destroy = atrium_destroy;
   r->base.ops.submit  = atrium_submit;
   r->base.ops.wait    = atrium_wait;

   r->base.shmem_ops.create  = atrium_shmem_create;
   r->base.shmem_ops.destroy = atrium_shmem_destroy;

   r->base.bo_ops.create_from_device_memory = atrium_bo_create_from_device_memory;
   r->base.bo_ops.create_from_dma_buf       = atrium_bo_create_from_dma_buf;
   r->base.bo_ops.destroy                   = atrium_bo_destroy;
   r->base.bo_ops.export_dma_buf            = atrium_bo_export_dma_buf;
   r->base.bo_ops.export_sync_file          = atrium_bo_export_sync_file;
   r->base.bo_ops.map                       = atrium_bo_map;
   r->base.bo_ops.flush                     = atrium_bo_flush;
   r->base.bo_ops.invalidate                = atrium_bo_invalidate;

   r->base.sync_ops.create               = atrium_sync_create;
   r->base.sync_ops.create_from_syncobj  = atrium_sync_create_from_syncobj;
   r->base.sync_ops.destroy              = atrium_sync_destroy;
   r->base.sync_ops.export_syncobj       = atrium_sync_export_syncobj;
   r->base.sync_ops.reset                = atrium_sync_reset;
   r->base.sync_ops.read                 = atrium_sync_read;
   r->base.sync_ops.write                = atrium_sync_write;

   return VK_SUCCESS;
}

VkResult
vn_renderer_create_atrium(struct vn_instance *instance,
                          const VkAllocationCallbacks *alloc,
                          struct vn_renderer **renderer)
{
   /* Probe: avoid even allocating the renderer state if the cdev
    * isn't present. Lets vn_renderer_create() fall through cleanly
    * to vtest on systems without an atrium-gpu kmod loaded. */
   struct stat st;
   if (stat(ATRIUM_DEV_PATH, &st) < 0)
      return VK_ERROR_INCOMPATIBLE_DRIVER;

   struct atrium_renderer *r =
      vk_zalloc(alloc, sizeof(*r), VN_DEFAULT_ALIGN,
                VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!r)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   r->instance = instance;
   r->fd       = -1;

   VkResult result = atrium_init(r);
   if (result != VK_SUCCESS) {
      atrium_destroy(&r->base, alloc);
      return result;
   }

   *renderer = &r->base;
   return VK_SUCCESS;
}
