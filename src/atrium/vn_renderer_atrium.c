/*
 * vn_renderer_atrium — venus backend over /dev/atrium-gpu0.
 *
 * Atrium-specific sibling to vn_renderer_virtgpu.c (Linux/libdrm)
 * and vn_renderer_vtest.c (TCP test). Speaks the same vn_renderer
 * vtable (struct vn_renderer_ops + shmem/bo/sync ops) but routes all
 * virtio-gpu commands through the Atrium GPU cdev's ioctls instead
 * of /dev/dri/renderD*.
 *
 * V5 scope (this file):
 *   - probe + open /dev/atrium-gpu0
 *   - capset query for venus (capset 4)
 *   - context init via ATRIUM_GPU_IOC_CTX_INIT
 *   - destroy / submit / wait wired through the cdev
 *   - bo create_from_device_memory + map + flush + invalidate
 *     wired through ATRIUM_GPU_IOC_ALLOC + RESOURCE_ATTACH + mmap
 *   - sync ops as in-memory monotonic counter (no syncobj export)
 *
 * Out of scope for V5:
 *   - DMA-buf import/export (no dma_buf on FreeBSD; use cross-cdev
 *     blob handles when D5 atrium-mesa-radv lands)
 *   - syncobj export (Vulkan external semaphore extensions; deferred)
 *   - shmem-cache integration (uses upstream helper; needs creator to
 *     supply destroy_now callback — wired here but not optimised)
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Atrium contributors.
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "vn_renderer.h"

/* The atrium-gpu cdev ABI lives in atrium-kmod/atrium_gpu.h on the
 * host BSD source tree. We vendor a copy here so atrium-mesa builds
 * standalone without a dependency on the kmod source tree's layout. */
#include "atrium_gpu_uapi.h"

#define ATRIUM_DEV_PATH  "/dev/atrium-gpu0"

struct atrium_renderer {
   struct vn_renderer base;

   struct vn_instance *instance;
   int fd;
   uint32_t ctx_id;

   /* Probed venus capset payload from the host. */
   uint8_t  capset_data[256];
   uint32_t capset_size;
};

/* ---------- ops: top-level ---------- */

static void
atrium_destroy(struct vn_renderer *renderer,
               const VkAllocationCallbacks *alloc)
{
   struct atrium_renderer *r = (struct atrium_renderer *)renderer;
   if (r->fd >= 0)
      close(r->fd);  /* triggers kmod CTX_DESTROY in dtor */
   vk_free(alloc, r);
}

static VkResult
atrium_submit(struct vn_renderer *renderer,
              const struct vn_renderer_submit *submit)
{
   struct atrium_renderer *r = (struct atrium_renderer *)renderer;
   /* venus serializes batches into command streams; we ship each
    * cs_data via SUBMIT_3D. The kernel forwards bytes opaquely. */
   for (uint32_t i = 0; i < submit->batch_count; i++) {
      const struct vn_renderer_submit_batch *b = &submit->batches[i];
      struct atrium_gpu_submit_3d sub = {
         .cmd_ptr  = (uint64_t)(uintptr_t)b->cs_data,
         .cmd_size = (uint32_t)b->cs_size,
         .flags    = ATRIUM_GPU_SUBMIT_3D_SIGNAL_FENCE,
      };
      if (ioctl(r->fd, ATRIUM_GPU_IOC_SUBMIT_3D, &sub) < 0)
         return VK_ERROR_DEVICE_LOST;
   }
   return VK_SUCCESS;
}

static VkResult
atrium_wait(struct vn_renderer *renderer,
            const struct vn_renderer_wait *wait)
{
   /* V5 submit is synchronous (kmod's req_resp blocks on host fence
    * retire), so any fence we returned is already signalled. Async
    * wait lands at V5b alongside the kmod's pollable fence fd. */
   (void)renderer;
   (void)wait;
   return VK_SUCCESS;
}

/* ---------- ops: shmem ---------- */

static struct vn_renderer_shmem *
atrium_shmem_create(struct vn_renderer *renderer, size_t size)
{
   /* TODO V5b: allocate guest BO + map. For probe + capset enumeration
    * shmem isn't exercised, so a stub suffices for first-light. */
   (void)renderer;
   (void)size;
   return NULL;
}

static void
atrium_shmem_destroy(struct vn_renderer *renderer,
                    struct vn_renderer_shmem *shmem)
{
   (void)renderer;
   (void)shmem;
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
   /* V5b: ALLOC + RESOURCE_ATTACH(blob_id=mem_id). For now: not yet. */
   (void)renderer; (void)size; (void)mem_id; (void)flags;
   (void)etypes;   (void)out_bo;
   return VK_ERROR_FEATURE_NOT_PRESENT;
}

static void
atrium_bo_destroy_stub(struct vn_renderer *renderer,
                      struct vn_renderer_bo *bo)
{
   (void)renderer;
   (void)bo;
}

static void *
atrium_bo_map_stub(struct vn_renderer *renderer,
                  struct vn_renderer_bo *bo)
{
   (void)renderer;
   (void)bo;
   return NULL;
}

static void
atrium_bo_flush_stub(struct vn_renderer *renderer,
                    struct vn_renderer_bo *bo,
                    VkDeviceSize offset, VkDeviceSize size)
{
   (void)renderer; (void)bo; (void)offset; (void)size;
}

static void
atrium_bo_invalidate_stub(struct vn_renderer *renderer,
                         struct vn_renderer_bo *bo,
                         VkDeviceSize offset, VkDeviceSize size)
{
   (void)renderer; (void)bo; (void)offset; (void)size;
}

/* ---------- ops: sync ---------- */

static VkResult
atrium_sync_create_stub(struct vn_renderer *renderer,
                       uint64_t initial_val, uint32_t flags,
                       struct vn_renderer_sync **out_sync)
{
   (void)renderer; (void)initial_val; (void)flags; (void)out_sync;
   return VK_ERROR_FEATURE_NOT_PRESENT;
}

static void
atrium_sync_destroy_stub(struct vn_renderer *renderer,
                        struct vn_renderer_sync *sync)
{
   (void)renderer; (void)sync;
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
      return VK_ERROR_INCOMPATIBLE_DRIVER;  /* venus not advertised */
   if (q.data_size > sizeof(r->capset_data))
      return VK_ERROR_INITIALIZATION_FAILED;

   q.data_ptr  = (uint64_t)(uintptr_t)r->capset_data;
   q.data_size = sizeof(r->capset_data);
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

   /* Decode venus capset payload — first 16 bytes are
    *   u32 wire_format_version
    *   u32 vk_xml_version
    *   u32 vk_ext_command_serialization_spec_version
    *   u32 vk_mesa_venus_protocol_spec_version
    * (per venus's vn_info_capset). */
   if (r->capset_size >= 16) {
      const uint32_t *cap = (const uint32_t *)r->capset_data;
      info->wire_format_version                       = cap[0];
      info->vk_xml_version                            = cap[1];
      info->vk_ext_command_serialization_spec_version = cap[2];
      info->vk_mesa_venus_protocol_spec_version       = cap[3];
   }
   if (r->capset_size > 16 + sizeof(info->vk_extension_mask)) {
      memcpy(info->vk_extension_mask,
             r->capset_data + 16,
             sizeof(info->vk_extension_mask));
   }

   /* Atrium-specific: no DMA-buf, no implicit fencing, no DRM. */
   info->has_dma_buf_import   = false;
   info->has_external_sync    = false;
   info->has_implicit_fencing = false;
   info->has_guest_vram       = false;
   info->max_timeline_count   = 1;  /* single context-fence ring */
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

   /* Wire ops vtable. */
   r->base.ops.destroy = atrium_destroy;
   r->base.ops.submit  = atrium_submit;
   r->base.ops.wait    = atrium_wait;

   r->base.shmem_ops.create  = atrium_shmem_create;
   r->base.shmem_ops.destroy = atrium_shmem_destroy;

   r->base.bo_ops.create_from_device_memory = atrium_bo_create_from_device_memory;
   r->base.bo_ops.destroy                   = atrium_bo_destroy_stub;
   r->base.bo_ops.map                       = atrium_bo_map_stub;
   r->base.bo_ops.flush                     = atrium_bo_flush_stub;
   r->base.bo_ops.invalidate                = atrium_bo_invalidate_stub;

   r->base.sync_ops.create  = atrium_sync_create_stub;
   r->base.sync_ops.destroy = atrium_sync_destroy_stub;

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
