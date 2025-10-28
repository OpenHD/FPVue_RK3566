#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
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
#include <algorithm>
#include <stdint.h>
#include <vector>
#include <xf86drmMode.h>

static int (*real_open_fn)(const char *pathname, int flags, ...);
static int (*real_open64_fn)(const char *pathname, int flags, ...);
static drmModePlaneResPtr (*real_drmModeGetPlaneResources_fn)(int fd);
static drmModePlanePtr (*real_drmModeGetPlane_fn)(int fd, uint32_t plane_id);
static pthread_mutex_t fd_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t reserved_plane_mutex = PTHREAD_MUTEX_INITIALIZER;
static int shared_fd = -1;
static bool reserved_plane_ids_initialized = false;
static bool reserved_plane_ids_logged = false;
static std::vector<uint32_t> reserved_plane_ids;

static bool should_intercept_path(const char *pathname) {
    if (!pathname)
        return false;

    const char *device_path = getenv("FPVUE_DRM_DEVICE_PATH");
    if (device_path && device_path[0] != '\0' && strcmp(pathname, device_path) == 0)
        return true;

    const char *socket_path = getenv("FPVUE_DRM_FD_SOCKET");
    if (socket_path && socket_path[0] != '\0' && strcmp(pathname, socket_path) == 0)
        return true;

    return strcmp(pathname, "/dev/dri/card0") == 0;
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

static void parse_reserved_plane_ids_locked(void) {
    const char *env = getenv("FPVUE_RESERVED_PLANE_IDS");
    reserved_plane_ids.clear();
    if (!env)
        return;

    const char *cursor = env;
    while (*cursor) {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == ',' || *cursor == ';' || *cursor == ':')
            ++cursor;
        if (*cursor == '\0')
            break;

        char *endptr = nullptr;
        unsigned long value = strtoul(cursor, &endptr, 0);
        if (endptr == cursor)
            break;
        if (value <= 0xffffffffUL)
            reserved_plane_ids.push_back(static_cast<uint32_t>(value));
        cursor = endptr;
    }

    if (!reserved_plane_ids.empty() && !reserved_plane_ids_logged) {
        fprintf(stderr, "drm_fd_preload: reserving DRM planes");
        for (uint32_t id : reserved_plane_ids)
            fprintf(stderr, " %u", id);
        fprintf(stderr, "\n");
        reserved_plane_ids_logged = true;
    }
}

static bool reserved_planes_active(void) {
    if (pthread_mutex_lock(&reserved_plane_mutex) != 0)
        return false;
    if (!reserved_plane_ids_initialized) {
        parse_reserved_plane_ids_locked();
        reserved_plane_ids_initialized = true;
    }
    bool active = !reserved_plane_ids.empty();
    pthread_mutex_unlock(&reserved_plane_mutex);
    return active;
}

static bool plane_is_reserved(uint32_t plane_id) {
    bool reserved = false;
    if (pthread_mutex_lock(&reserved_plane_mutex) != 0)
        return false;
    if (!reserved_plane_ids_initialized) {
        parse_reserved_plane_ids_locked();
        reserved_plane_ids_initialized = true;
    }
    reserved = std::find(reserved_plane_ids.begin(), reserved_plane_ids.end(), plane_id) != reserved_plane_ids.end();
    pthread_mutex_unlock(&reserved_plane_mutex);
    return reserved;
}

static int ensure_real_drm_symbols(void) {
    if (!real_drmModeGetPlaneResources_fn) {
        real_drmModeGetPlaneResources_fn =
            reinterpret_cast<drmModePlaneResPtr (*)(int)>(dlsym(RTLD_NEXT, "drmModeGetPlaneResources"));
        if (!real_drmModeGetPlaneResources_fn) {
            fprintf(stderr, "drm_fd_preload: failed to resolve drmModeGetPlaneResources(): %s\n", dlerror());
            return -1;
        }
    }
    if (!real_drmModeGetPlane_fn) {
        real_drmModeGetPlane_fn =
            reinterpret_cast<drmModePlanePtr (*)(int, uint32_t)>(dlsym(RTLD_NEXT, "drmModeGetPlane"));
        if (!real_drmModeGetPlane_fn) {
            fprintf(stderr, "drm_fd_preload: failed to resolve drmModeGetPlane(): %s\n", dlerror());
            return -1;
        }
    }
    return 0;
}

extern "C" int open(const char *pathname, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }

    if (should_intercept_path(pathname)) {
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

    if (should_intercept_path(pathname)) {
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

extern "C" drmModePlaneResPtr drmModeGetPlaneResources(int fd) {
    if (ensure_real_drm_symbols() < 0) {
        errno = ENOSYS;
        return nullptr;
    }
    drmModePlaneResPtr res = real_drmModeGetPlaneResources_fn(fd);
    if (!res)
        return res;
    if (!reserved_planes_active())
        return res;

    uint32_t original_count = res->count_planes;
    uint32_t write_index = 0;
    for (uint32_t read_index = 0; read_index < original_count; ++read_index) {
        uint32_t plane_id = res->planes[read_index];
        if (plane_is_reserved(plane_id))
            continue;
        res->planes[write_index++] = plane_id;
    }
    res->count_planes = write_index;
    return res;
}

extern "C" drmModePlanePtr drmModeGetPlane(int fd, uint32_t plane_id) {
    if (ensure_real_drm_symbols() < 0) {
        errno = ENOSYS;
        return nullptr;
    }
    drmModePlanePtr plane = real_drmModeGetPlane_fn(fd, plane_id);
    if (plane && reserved_planes_active() && plane_is_reserved(plane_id)) {
        plane->possible_crtcs = 0;
        plane->count_formats = 0;
    }
    return plane;
}
