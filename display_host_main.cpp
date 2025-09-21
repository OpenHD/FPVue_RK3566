#include "display_host.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef DEFAULT_PRELOAD_PATH
#define DEFAULT_PRELOAD_PATH ""
#endif

static int build_preload_path(char *buffer, size_t size) {
    char exe_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len < 0)
        return -1;
    exe_path[len] = '\0';

    char *slash = strrchr(exe_path, '/');
    if (!slash)
        return -1;
    *slash = '\0';

    const char *filename = "libdrm_fd_preload.so";
    if (snprintf(buffer, size, "%s/%s", exe_path, filename) < (int)size &&
        access(buffer, F_OK) == 0) {
        return 0;
    }

    char parent[PATH_MAX];
    strncpy(parent, exe_path, sizeof(parent));
    parent[sizeof(parent) - 1] = '\0';
    slash = strrchr(parent, '/');
    if (slash) {
        *slash = '\0';
        if (snprintf(buffer, size, "%s/lib/%s", parent, filename) < (int)size &&
            access(buffer, F_OK) == 0) {
            return 0;
        }
    }

    if (DEFAULT_PRELOAD_PATH[0] != '\0' &&
        snprintf(buffer, size, "%s", DEFAULT_PRELOAD_PATH) < (int)size &&
        access(buffer, F_OK) == 0) {
        return 0;
    }

    return -1;
}

int main(int argc, char **argv) {
    const char *drm_node = "/dev/dri/card0";
    const char *socket_path = "/tmp/drm-master";
    int clients = 2;
    uint16_t width = 0, height = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "720p") == 0) {
            width = 1280;
            height = 720;
        } else if (strcmp(argv[i], "1080p") == 0) {
            width = 1920;
            height = 1080;
        } else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (strcmp(argv[i], "--drm") == 0 && i + 1 < argc) {
            drm_node = argv[++i];
        } else if (strcmp(argv[i], "--clients") == 0 && i + 1 < argc) {
            clients = atoi(argv[++i]);
        } else {
            fprintf(stderr, "Usage: %s [720p|1080p] [--socket path] [--drm node] [--clients n]\n", argv[0]);
            return 1;
        }
    }

    if (clients < 2)
        clients = 2;

    pid_t pid = fork();
    if (pid == 0) {
        sleep(1);
        setenv("FPVUE_DRM_FD_SOCKET", socket_path, 1);
        setenv("FPVUE_DRM_DEVICE_PATH", drm_node, 1);
        setenv("FPVUE_COLOR_CYCLE_ZPOS", "0", 1);
        execlp("fpvue", "fpvue", "--color-cycle", NULL);
        perror("execlp fpvue");
        return 1;
    }

    pid_t kmscube_pid = fork();
    if (kmscube_pid == 0) {
        sleep(2);
        setenv("FPVUE_DRM_FD_SOCKET", socket_path, 1);
        setenv("FPVUE_DRM_DEVICE_PATH", drm_node, 1);
        char preload[PATH_MAX];
        if (build_preload_path(preload, sizeof(preload)) == 0) {
            setenv("LD_PRELOAD", preload, 1);
        } else {
            fprintf(stderr, "Failed to locate libdrm_fd_preload.so; kmscube may not receive DRM FD\n");
        }
        execlp("kmscube", "kmscube", "--atomic", NULL);
        perror("execlp kmscube");
        return 1;
    }

    printf("Starting display host with DRM node %s, socket %s, expecting %d clients", drm_node, socket_path, clients);
    if (width > 0 && height > 0) {
        printf(", forcing mode %ux%u", width, height);
    }
    printf("\n");
    printf("Launched fpvue color cycle client as PID %d.\n", pid);
    printf("Launched kmscube client as PID %d using overlay plane.\n", kmscube_pid);
    printf("To run a Qt5 application against this host, set FPVUE_DRM_FD_SOCKET=%s and export QT_QPA_PLATFORM=eglfs before launching your Qt app.\n", socket_path);

    int fd = start_display_host(drm_node, socket_path, clients, width, height);
    if (fd < 0) {
        fprintf(stderr, "Failed to start display host\n");
        return 1;
    }
    printf("Display host running. DRM FD %d shared on %s\n", fd, socket_path);
    while (1) {
        sleep(60);
    }
    return 0;
}
