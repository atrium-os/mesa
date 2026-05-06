/*
 * atrium_gpu_uapi.h — vendored userspace header for /dev/atrium-gpu0.
 *
 * This is a frozen snapshot of the venus-relevant subset of the
 * atrium-kmod ABI (atrium-kmod/atrium_gpu.h on the host BSD source
 * tree). Copied here so atrium-mesa builds without taking a
 * dependency on the kmod source tree.
 *
 * When the kmod's ABI evolves (e.g. V4-stretch fence_fd via kqueue),
 * sync this file by hand. Atomic with the kmod commit; deliberate
 * cost of vendoring vs cross-tree symlinks.
 */

#ifndef ATRIUM_GPU_UAPI_H_
#define ATRIUM_GPU_UAPI_H_

#include <sys/types.h>
#include <sys/ioccom.h>
#include <stdint.h>

#define ATRIUM_GPU_BO_GPU_VISIBLE     0x01
#define ATRIUM_GPU_BO_CPU_VISIBLE     0x02
#define ATRIUM_GPU_BO_COHERENT        0x04
#define ATRIUM_GPU_BO_SCANOUT         0x08

struct atrium_gpu_alloc {
	uint64_t size;
	uint32_t flags;
	uint32_t alignment;
	uint32_t handle;
	uint32_t _pad0;
	uint64_t mmap_offset;
};
#define ATRIUM_GPU_IOC_ALLOC  _IOWR('G', 1, struct atrium_gpu_alloc)
#define ATRIUM_GPU_IOC_FREE   _IOW ('G', 2, uint32_t)

#define ATRIUM_GPU_CAPSET_VENUS  4

struct atrium_gpu_capset_query {
	uint32_t capset_id;
	uint32_t capset_version;
	uint32_t actual_version;
	uint32_t data_size;
	uint64_t data_ptr;
	uint64_t _reserved[2];
};
#define ATRIUM_GPU_IOC_CAPSET_QUERY  _IOWR('G', 0x40, struct atrium_gpu_capset_query)

struct atrium_gpu_ctx_init {
	uint32_t capset_id;
	uint32_t flags;
	char     debug_name[64];
	uint32_t ctx_id_out;
	uint32_t _reserved[3];
};
#define ATRIUM_GPU_IOC_CTX_INIT  _IOWR('G', 0x41, struct atrium_gpu_ctx_init)

#define ATRIUM_GPU_BLOB_MEM_GUEST    0x0001
#define ATRIUM_GPU_BLOB_MEM_HOST3D   0x0002
#define ATRIUM_GPU_BLOB_USE_MAPPABLE 0x01

struct atrium_gpu_resource_attach {
	uint32_t bo_handle;
	uint32_t blob_mem;
	uint32_t blob_flags;
	uint32_t _pad0;
	uint64_t blob_id;
	uint32_t resource_id_out;
	uint32_t _reserved[3];
};
#define ATRIUM_GPU_IOC_RESOURCE_ATTACH \
	_IOWR('G', 0x42, struct atrium_gpu_resource_attach)

#define ATRIUM_GPU_SUBMIT_3D_SIGNAL_FENCE 0x01

struct atrium_gpu_submit_3d {
	uint64_t cmd_ptr;
	uint32_t cmd_size;
	uint32_t flags;
	uint32_t bo_count;
	uint32_t _pad0;
	uint64_t bo_handles_ptr;
	uint64_t fence_out;
	uint64_t _reserved[2];
};
#define ATRIUM_GPU_IOC_SUBMIT_3D \
	_IOWR('G', 0x43, struct atrium_gpu_submit_3d)

struct atrium_gpu_ctx_fence_wait {
	uint64_t fence;
	uint64_t timeout_ns;
	uint32_t status;
	uint32_t _pad0;
};
#define ATRIUM_GPU_IOC_CTX_FENCE_WAIT \
	_IOWR('G', 0x44, struct atrium_gpu_ctx_fence_wait)

#endif /* ATRIUM_GPU_UAPI_H_ */
