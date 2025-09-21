#define _GNU_SOURCE
#include "display_host.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int (*real_open_fn)(const char *pathname, int flags, ...);
static int (*real_open64_fn)(const char *pathname, int flags, ...);
static pthread_mutex_t fd_mutex = PTHREAD_MUTEX_INITIALIZER;
static int shared_fd = -1;

static const char *target_device_path(void) {
    const char *path = getenv("FPVUE_DRM_DEVICE_PATH");
    if (path && path[0] != '\0')
        return path;
    return "/dev/dri/card0";
}

static int ensure_real_open(void) {
    if (!real_open_fn) {
        real_open_fn = (int (*)(const char *, int, ...))dlsym(RTLD_NEXT, "open");
        if (!real_open_fn) {
            fprintf(stderr, "drm_fd_preload: failed to resolve real open(): %s\n", dlerror());
            return -1;
        }
    }
    if (!real_open64_fn) {
        real_open64_fn = (int (*)(const char *, int, ...))dlsym(RTLD_NEXT, "open64");
        if (!real_open64_fn) {
            real_open64_fn = real_open_fn;
        }
    }
    return 0;
}

static int get_shared_fd(void) {
    const char *socket_path = getenv("FPVUE_DRM_FD_SOCKET");
    if (!socket_path)
        return -1;

    if (pthread_mutex_lock(&fd_mutex) != 0)
        return -1;

    if (shared_fd < 0) {
        shared_fd = receive_fd_from_socket(socket_path);
        if (shared_fd < 0) {
            fprintf(stderr, "drm_fd_preload: failed to receive DRM FD from %s\n", socket_path);
        }
    }

    int fd = -1;
    if (shared_fd >= 0)
        fd = dup(shared_fd);

    pthread_mutex_unlock(&fd_mutex);
    return fd;
}

extern "C" int open(const char *pathname, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }

    if (pathname && strcmp(pathname, target_device_path()) == 0) {
        int fd = get_shared_fd();
        if (fd >= 0)
            return fd;
    }

    if (ensure_real_open() < 0)
        errno = ENOSYS;

    if (!real_open_fn)
        return -1;

    if (flags & O_CREAT)
        return real_open_fn(pathname, flags, mode);
    return real_open_fn(pathname, flags);
}

extern "C" int open64(const char *pathname, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }

    if (pathname && strcmp(pathname, target_device_path()) == 0) {
        int fd = get_shared_fd();
        if (fd >= 0)
            return fd;
    }

    if (ensure_real_open() < 0)
        errno = ENOSYS;

    if (!real_open64_fn)
        return -1;

    if (flags & O_CREAT)
        return real_open64_fn(pathname, flags, mode);
    return real_open64_fn(pathname, flags);
}
