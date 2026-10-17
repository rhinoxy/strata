#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <sys/ioctl.h>

/*
 * Workaround for ROCm / libhsa-runtime64 bug on multi-GPU systems where
 * Node 2 is a small-VRAM display GPU (e.g., Radeon RX 6400 4GB, gpu_id=21283)
 * with Full BAR (Above 4G MMIO) enabled, and Node 3/4 are compute GPUs
 * (e.g., Radeon RX 9060 XT 16GB, gpu_id=59575 / 42888).
 *
 * Even when HIP_VISIBLE_DEVICES=1,2 or ROCR_VISIBLE_DEVICES=1,2 is set,
 * libhsa-runtime64's internal default_gpuid_ remains bound to the first GPU
 * node (RX 6400) and passes its gpu_id to AMDKFD_IOC_ALLOC_MEMORY_OF_GPU
 * for all host USERPTR allocations (hipHostMalloc / hipHostRegister).
 * Under Full BAR, the RX 6400 driver rejects pinning more than ~4-5 GiB of
 * system memory with "amdgpu 0000:05:00.0: failed to pin userptr".
 *
 * Redirecting USERPTR allocations (> 64 KiB) from RX 6400 (gpu_id=21283) to
 * RX 9060 XT #1 (gpu_id=59575) allows full host memory pinning across all
 * compute GPUs without touching the display GPU's TTM/DMA validator.
 */

struct kfd_ioctl_alloc_memory_of_gpu_args {
    uint64_t va_addr;
    uint64_t size;
    uint64_t handle;
    uint64_t mmap_offset;
    uint32_t gpu_id;
    uint32_t flags;
};

#define KFD_IOC_ALLOC_MEM_FLAGS_USERPTR (1 << 2)

static uint32_t read_gpu_id(const char *path, uint32_t fallback) {
    FILE *f = fopen(path, "r");
    if (!f) return fallback;
    unsigned int val = 0;
    if (fscanf(f, "%u", &val) != 1 || val == 0) {
        fclose(f);
        return fallback;
    }
    fclose(f);
    return (uint32_t)val;
}

int ioctl(int fd, unsigned long request, ...) {
    static int (*real_ioctl)(int, unsigned long, void *) = NULL;
    static uint32_t display_gpu_id = 0;
    static uint32_t compute_gpu_id = 0;

    if (!real_ioctl) {
        real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    }
    if (!display_gpu_id) {
        display_gpu_id = read_gpu_id("/sys/class/kfd/kfd/topology/nodes/2/gpu_id", 21283);
        compute_gpu_id = read_gpu_id("/sys/class/kfd/kfd/topology/nodes/3/gpu_id", 59575);
    }

    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    if (_IOC_TYPE(request) == 'K' && _IOC_NR(request) == 0x16 && arg != NULL) {
        struct kfd_ioctl_alloc_memory_of_gpu_args *a =
            (struct kfd_ioctl_alloc_memory_of_gpu_args *)arg;
        if (a->gpu_id == display_gpu_id &&
            (a->flags & KFD_IOC_ALLOC_MEM_FLAGS_USERPTR) &&
            a->size > 65536) {
            a->gpu_id = compute_gpu_id;
        }
    }

    return real_ioctl(fd, request, arg);
}
