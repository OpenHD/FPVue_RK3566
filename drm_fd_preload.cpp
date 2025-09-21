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
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mutex>
#include <unordered_map>
#include <vector>

#include <xf86drm.h>
#include <xf86drmMode.h>

static int (*real_open_fn)(const char *pathname, int flags, ...);
static int (*real_open64_fn)(const char *pathname, int flags, ...);
static drmModePlaneResPtr (*real_drmModeGetPlaneResources_fn)(int fd);
static void (*real_drmModeFreePlaneResources_fn)(drmModePlaneResPtr ptr);
static pthread_mutex_t fd_mutex = PTHREAD_MUTEX_INITIALIZER;
static int shared_fd = -1;

static std::mutex plane_res_mutex;
static std::unordered_map<drmModePlaneResPtr, drmModePlaneResPtr> plane_res_mapping;

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

static int ensure_real_plane_fns(void) {
    if (!real_drmModeGetPlaneResources_fn) {
        real_drmModeGetPlaneResources_fn =
            (drmModePlaneResPtr (*)(int))dlsym(RTLD_NEXT, "drmModeGetPlaneResources");
        if (!real_drmModeGetPlaneResources_fn) {
            fprintf(stderr,
                    "drm_fd_preload: failed to resolve drmModeGetPlaneResources(): %s\n",
                    dlerror());
            return -1;
        }
    }
    if (!real_drmModeFreePlaneResources_fn) {
        real_drmModeFreePlaneResources_fn =
            (void (*)(drmModePlaneResPtr))dlsym(RTLD_NEXT, "drmModeFreePlaneResources");
        if (!real_drmModeFreePlaneResources_fn) {
            fprintf(stderr,
                    "drm_fd_preload: failed to resolve drmModeFreePlaneResources(): %s\n",
                    dlerror());
            return -1;
        }
    }
    return 0;
}

static bool env_flag_enabled(const char *name) {
    const char *value = getenv(name);
    if (!value || value[0] == '\0')
        return false;
    if (strcmp(value, "0") == 0)
        return false;
    if (strcasecmp(value, "false") == 0)
        return false;
    if (strcasecmp(value, "off") == 0)
        return false;
    return true;
}

static bool get_plane_type_value(int fd, uint32_t plane_id, uint64_t *type_out) {
    drmModeObjectPropertiesPtr props =
        drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
    if (!props)
        return false;

    bool found = false;
    for (uint32_t i = 0; i < props->count_props; ++i) {
        drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[i]);
        if (!prop)
            continue;

        if (!strcmp(prop->name, "type")) {
            if (type_out)
                *type_out = props->prop_values[i];
            found = true;
            drmModeFreeProperty(prop);
            break;
        }

        drmModeFreeProperty(prop);
    }

    drmModeFreeObjectProperties(props);
    return found;
}

static drmModePlaneResPtr filter_primary_planes(int fd, drmModePlaneResPtr original) {
    if (!env_flag_enabled("FPVUE_FILTER_PRIMARY_PLANES"))
        return original;

    std::vector<uint32_t> filtered_planes;
    filtered_planes.reserve(original->count_planes);
    bool any_filtered = false;

    for (uint32_t i = 0; i < original->count_planes; ++i) {
        uint32_t plane_id = original->planes[i];
        uint64_t plane_type = 0;
        if (get_plane_type_value(fd, plane_id, &plane_type) &&
            plane_type == DRM_PLANE_TYPE_PRIMARY) {
            any_filtered = true;
            continue;
        }
        filtered_planes.push_back(plane_id);
    }

    if (!any_filtered || filtered_planes.empty())
        return original;

    drmModePlaneResPtr copy =
        static_cast<drmModePlaneResPtr>(malloc(sizeof(*copy)));
    if (!copy)
        return original;

    copy->count_planes = static_cast<uint32_t>(filtered_planes.size());
    copy->planes = static_cast<uint32_t *>(
        malloc(sizeof(uint32_t) * filtered_planes.size()));
    if (!copy->planes) {
        free(copy);
        return original;
    }

    memcpy(copy->planes, filtered_planes.data(),
           filtered_planes.size() * sizeof(uint32_t));

    {
        std::lock_guard<std::mutex> lock(plane_res_mutex);
        plane_res_mapping[copy] = original;
    }

    size_t filtered_count = static_cast<size_t>(original->count_planes) -
                            static_cast<size_t>(filtered_planes.size());
    if (filtered_count > 0) {
        fprintf(stderr,
                "drm_fd_preload: filtered %zu primary plane(s) from client enumeration.\n",
                filtered_count);
    }

    return copy;
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
    if (ensure_real_plane_fns() < 0)
        return nullptr;

    drmModePlaneResPtr res = real_drmModeGetPlaneResources_fn(fd);
    if (!res)
        return res;

    return filter_primary_planes(fd, res);
}

extern "C" void drmModeFreePlaneResources(drmModePlaneResPtr ptr) {
    if (ensure_real_plane_fns() < 0)
        return;

    drmModePlaneResPtr original = nullptr;
    {
        std::lock_guard<std::mutex> lock(plane_res_mutex);
        auto it = plane_res_mapping.find(ptr);
        if (it != plane_res_mapping.end()) {
            original = it->second;
            plane_res_mapping.erase(it);
        }
    }

    if (original) {
        free(ptr->planes);
        free(ptr);
        ptr = original;
    }

    real_drmModeFreePlaneResources_fn(ptr);
}
