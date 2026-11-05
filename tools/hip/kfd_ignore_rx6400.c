#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>

/*
 * libkfd_ignore_rx6400.so
 *
 * 1. Workaround for ROCm / libhsa-runtime64 bug on multi-GPU systems where
 *    Node 2 is a display GPU (RX 6400 4GB, gpu_id=21283) and Node 3/4 are
 *    compute GPUs (RX 9060 XT 16GB, gpu_id=59575 / 42888).
 *    libhsa-runtime64 passes the display GPU's ID for all host USERPTR
 *    allocations (hipHostMalloc / hipHostRegister). We redirect it to Node 4
 *    (42888), which has full host memory pinning capability.
 *
 * 2. When MAP_MEMORY_TO_GPU fails across multiple devices (e.g. cross-device
 *    queue/doorbell buffers), map each target device individually.
 *
 * 3. Emulate hipDeviceAttributeCanUseHostPointerForRegisteredMem = 1, which
 *    ROCm 7.x incorrectly reports as 0, enabling large host pinned tiers.
 */

struct kfd_ioctl_alloc_memory_of_gpu_args {
    uint64_t va_addr;
    uint64_t size;
    uint64_t handle;
    uint64_t mmap_offset;
    uint32_t gpu_id;
    uint32_t flags;
};

struct kfd_ioctl_map_memory_to_gpu_args {
    uint64_t handle;
    uint64_t device_ids_array_ptr;
    uint32_t n_devices;
    uint32_t n_success;
};

struct kfd_ioctl_unmap_memory_from_gpu_args {
    uint64_t handle;
    uint64_t device_ids_array_ptr;
    uint32_t n_devices;
    uint32_t n_success;
};

#define KFD_IOC_ALLOC_MEM_FLAGS_USERPTR (1 << 2)

static uint32_t display_gpu_id = 0;
static uint32_t compute_gpu_id = 0;

static void init_gpu_ids(void) {
    if (display_gpu_id && compute_gpu_id) return;
    display_gpu_id = 21283; // fallback: RX 6400 (gfx1034)
    compute_gpu_id = 42888; // fallback: RX 9060 XT #2 (gfx1200, Node 4)

    char path[128];
    for (int node = 0; node < 16; node++) {
        snprintf(path, sizeof(path), "/sys/class/kfd/kfd/topology/nodes/%d/properties", node);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char line[256];
        uint32_t gpu_id = 0;
        uint32_t gfx_ver = 0;
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "gpu_id %u", &gpu_id) == 1) {}
            if (sscanf(line, "gfx_target_version %u", &gfx_ver) == 1) {}
        }
        fclose(f);
        if (gfx_ver == 100304 && gpu_id) {
            display_gpu_id = gpu_id;
        } else if (gfx_ver == 120000 && gpu_id) {
            compute_gpu_id = gpu_id; // picks the last gfx1200 device (Node 4)
        }
    }
}

int ioctl(int fd, unsigned long request, ...) {
    static int (*real_ioctl)(int, unsigned long, void *) = NULL;
    if (!real_ioctl) {
        real_ioctl = (int (*)(int, unsigned long, void *))dlsym(RTLD_NEXT, "ioctl");
        init_gpu_ids();
    }

    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    if (_IOC_TYPE(request) == 'K') {
        int nr = _IOC_NR(request);
        if (nr == 0x16 && arg) { // ALLOC_MEMORY_OF_GPU
            struct kfd_ioctl_alloc_memory_of_gpu_args *a = (struct kfd_ioctl_alloc_memory_of_gpu_args *)arg;
            // Redirect host USERPTR allocations from display GPU to compute GPU
            if (a->gpu_id == display_gpu_id &&
                (a->flags & KFD_IOC_ALLOC_MEM_FLAGS_USERPTR) &&
                a->size > 65536) {
                a->gpu_id = compute_gpu_id;
            }
            return real_ioctl(fd, request, arg);
        } else if (nr == 0x18 && arg) { // MAP_MEMORY_TO_GPU
            struct kfd_ioctl_map_memory_to_gpu_args *m = (struct kfd_ioctl_map_memory_to_gpu_args *)arg;
            uint32_t orig_count = m->n_devices;
            uint64_t orig_ptr = m->device_ids_array_ptr;
            uint32_t *devs = (uint32_t *)orig_ptr;

            if (orig_count > 1 && devs) {
                m->n_success = 0;
                int ret = real_ioctl(fd, request, arg);
                if (ret == 0) return 0;

                // Fallback: map each target device individually
                int any_success = 0;
                for (uint32_t i = 0; i < orig_count; i++) {
                    uint32_t single_dev[1] = {devs[i]};
                    m->device_ids_array_ptr = (uint64_t)single_dev;
                    m->n_devices = 1;
                    m->n_success = 0;
                    errno = 0;
                    int r = real_ioctl(fd, request, arg);
                    if (r == 0 || errno == EBUSY) {
                        any_success = 1;
                    }
                }
                m->device_ids_array_ptr = orig_ptr;
                m->n_devices = orig_count;
                if (any_success) {
                    m->n_success = orig_count;
                    return 0;
                }
                return ret;
            }
            return real_ioctl(fd, request, arg);
        } else if (nr == 0x19 && arg) { // UNMAP_MEMORY_FROM_GPU
            struct kfd_ioctl_unmap_memory_from_gpu_args *u = (struct kfd_ioctl_unmap_memory_from_gpu_args *)arg;
            uint32_t orig_count = u->n_devices;
            uint64_t orig_ptr = u->device_ids_array_ptr;
            uint32_t *devs = (uint32_t *)orig_ptr;

            if (orig_count > 1 && devs) {
                u->n_success = 0;
                int ret = real_ioctl(fd, request, arg);
                if (ret == 0) return 0;

                for (uint32_t i = 0; i < orig_count; i++) {
                    uint32_t single_dev[1] = {devs[i]};
                    u->device_ids_array_ptr = (uint64_t)single_dev;
                    u->n_devices = 1;
                    u->n_success = 0;
                    real_ioctl(fd, request, arg);
                }
                u->device_ids_array_ptr = orig_ptr;
                u->n_devices = orig_count;
                u->n_success = orig_count;
                return 0;
            }
            return real_ioctl(fd, request, arg);
        }
    }

    return real_ioctl(fd, request, arg);
}

// Emulate hipDeviceAttributeCanUseHostPointerForRegisteredMem for ROCm/HIP
int hipDeviceGetAttribute(int *pi, int attr, int deviceId) {
    static int (*real_hipDeviceGetAttribute)(int *, int, int) = NULL;
    if (!real_hipDeviceGetAttribute) {
        real_hipDeviceGetAttribute = (int (*)(int *, int, int))dlsym(RTLD_NEXT, "hipDeviceGetAttribute");
    }
    if (attr == 4 /* hipDeviceAttributeCanUseHostPointerForRegisteredMem */) {
        if (pi) *pi = 1;
        return 0; /* hipSuccess */
    }
    if (real_hipDeviceGetAttribute) {
        return real_hipDeviceGetAttribute(pi, attr, deviceId);
    }
    return 0;
}
