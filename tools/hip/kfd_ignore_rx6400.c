#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <sys/ioctl.h>

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
        real_ioctl = (int (*)(int, unsigned long, void *))dlsym(RTLD_NEXT, "ioctl");
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
