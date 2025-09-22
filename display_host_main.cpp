#include "display_host.h"
#include <ctype.h>
#include <cstdarg>
#include <fstream>
#include <iterator>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <errno.h>

#ifndef DEFAULT_PRELOAD_PATH
#define DEFAULT_PRELOAD_PATH ""
#endif

static bool tokenize_preload_contains(const char *preload, const char *library) {
    if (!preload || !library || library[0] == '\0')
        return false;

    size_t library_len = strlen(library);
    const char *cursor = preload;
    while (cursor && *cursor) {
        const char *next = strchr(cursor, ':');
        size_t token_len = next ? static_cast<size_t>(next - cursor) : strlen(cursor);
        if (token_len == library_len && strncmp(cursor, library, token_len) == 0)
            return true;
        if (!next)
            break;
        cursor = next + 1;
    }

    return false;
}

static bool locate_executable_path(const char *name, char *buffer, size_t size) {
    if (!name || name[0] == '\0' || !buffer || size == 0)
        return false;

    if (strchr(name, '/')) {
        if (access(name, X_OK) == 0) {
            if (snprintf(buffer, size, "%s", name) < static_cast<int>(size))
                return true;
        }
        return false;
    }

    const char *path_env = getenv("PATH");
    if (!path_env)
        return false;

    char path_copy[4096];
    strncpy(path_copy, path_env, sizeof(path_copy));
    path_copy[sizeof(path_copy) - 1] = '\0';

    char *saveptr = nullptr;
    for (char *token = strtok_r(path_copy, ":", &saveptr); token; token = strtok_r(nullptr, ":", &saveptr)) {
        if (snprintf(buffer, size, "%s/%s", token, name) >= static_cast<int>(size))
            continue;
        if (access(buffer, X_OK) == 0)
            return true;
    }

    return false;
}

static bool extract_linked_library(const char *executable_path, const char *library_prefix, char *buffer, size_t size) {
    if (!executable_path || !library_prefix || !buffer || size == 0)
        return false;

    std::ifstream file(executable_path, std::ios::binary);
    if (!file)
        return false;

    std::vector<char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (data.empty())
        return false;

    const std::string prefix(library_prefix);
    for (size_t i = 0; i + prefix.size() <= data.size(); ++i) {
        if (memcmp(&data[i], prefix.data(), prefix.size()) == 0) {
            size_t end = i + prefix.size();
            while (end < data.size()) {
                char ch = data[end];
                if (ch == '\0' || (!isalnum(static_cast<unsigned char>(ch)) && ch != '.' && ch != '_' && ch != '-'))
                    break;
                ++end;
            }

            size_t length = end - i;
            if (length >= size)
                length = size - 1;
            memcpy(buffer, &data[i], length);
            buffer[length] = '\0';
            return true;
        }
    }

    return false;
}

static bool locate_library_in_ldconfig(const char *library_prefix, char *buffer, size_t size) {
    if (!library_prefix || !buffer || size == 0)
        return false;

    FILE *pipe = popen("ldconfig -p", "r");
    if (!pipe)
        return false;

    bool found = false;
    const size_t prefix_len = strlen(library_prefix);
    char line[512];
    while (fgets(line, sizeof(line), pipe)) {
        char *cursor = line;
        while (*cursor && isspace(static_cast<unsigned char>(*cursor)))
            ++cursor;

        if (strncmp(cursor, library_prefix, prefix_len) != 0)
            continue;

        char *arrow = strstr(cursor, "=>");
        if (!arrow)
            continue;
        arrow += 2;
        while (*arrow && isspace(static_cast<unsigned char>(*arrow)))
            ++arrow;
        if (!*arrow)
            continue;

        char *newline = strchr(arrow, '\n');
        if (newline)
            *newline = '\0';

        if (snprintf(buffer, size, "%s", arrow) >= static_cast<int>(size))
            continue;

        found = true;
        break;
    }

    pclose(pipe);
    return found;
}

static constexpr const char kDisplayHostLogPrefix[] = "display_host: ";

static void log_with_prefix(FILE *stream, const char *fmt, va_list args) {
    fputs(kDisplayHostLogPrefix, stream);
    vfprintf(stream, fmt, args);
    fflush(stream);
}

static void log_info(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_with_prefix(stdout, fmt, args);
    va_end(args);
}

static void log_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_with_prefix(stderr, fmt, args);
    va_end(args);
}

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

static void configure_shared_drm_environment(const char *socket_path, const char *drm_node) {
    setenv("FPVUE_DRM_FD_SOCKET", socket_path, 1);
    setenv("FPVUE_DRM_DEVICE_PATH", drm_node, 1);
}

static void ensure_preload_in_environment(const char *target_executable) {
    char preload[PATH_MAX];
    if (build_preload_path(preload, sizeof(preload)) != 0) {
        fprintf(stderr, "Failed to locate libdrm_fd_preload.so; secondary clients may not receive DRM FD\n");
        return;
    }

    char asan_library[PATH_MAX];
    bool add_asan_preload = false;
    if (target_executable && target_executable[0] != '\0') {
        char executable_path[PATH_MAX];
        if (locate_executable_path(target_executable, executable_path, sizeof(executable_path))) {
            if (extract_linked_library(executable_path, "libasan.so", asan_library, sizeof(asan_library))) {
                if (!strchr(asan_library, '/')) {
                    char resolved[PATH_MAX];
                    if (locate_library_in_ldconfig(asan_library, resolved, sizeof(resolved))) {
                        strncpy(asan_library, resolved, sizeof(asan_library));
                        asan_library[sizeof(asan_library) - 1] = '\0';
                    }
                }
                add_asan_preload = true;
            }
        }
    }

    if (!add_asan_preload && target_executable && strcmp(target_executable, "QOpenHD") == 0) {
        if (locate_library_in_ldconfig("libasan.so", asan_library, sizeof(asan_library))) {
            add_asan_preload = true;
        }
    }

    const char *existing_preload = getenv("LD_PRELOAD");
    std::string combined_preload = existing_preload ? existing_preload : "";

    if (add_asan_preload && !tokenize_preload_contains(combined_preload.c_str(), asan_library)) {
        if (!combined_preload.empty())
            combined_preload = std::string(asan_library) + ":" + combined_preload;
        else
            combined_preload = asan_library;
        fprintf(stderr, "Preloading %s before launching %s to satisfy AddressSanitizer.\n", asan_library,
                target_executable ? target_executable : "client");
    }

    if (!tokenize_preload_contains(combined_preload.c_str(), preload)) {
        if (!combined_preload.empty())
            combined_preload += ":";
        combined_preload += preload;
    }

    if (!combined_preload.empty()) {
        setenv("LD_PRELOAD", combined_preload.c_str(), 1);
        if (target_executable && target_executable[0] != '\0') {
            fprintf(stderr, "LD_PRELOAD for %s set to %s\n", target_executable, combined_preload.c_str());
        }
    }
}

static void pipe_output_to_stream(int fd, FILE *stream, const char *prefix) {
    std::thread([fd, stream, prefix]() {
        char buffer[512];
        bool at_line_start = true;
        while (1) {
            ssize_t bytes = read(fd, buffer, sizeof(buffer));
            if (bytes <= 0)
                break;

            size_t offset = 0;
            while (offset < static_cast<size_t>(bytes)) {
                if (at_line_start && prefix) {
                    fputs(prefix, stream);
                }

                char *newline = static_cast<char *>(memchr(buffer + offset, '\n', bytes - offset));
                size_t chunk = newline ? static_cast<size_t>(newline - (buffer + offset) + 1)
                                       : static_cast<size_t>(bytes - offset);
                fwrite(buffer + offset, 1, chunk, stream);
                fflush(stream);
                at_line_start = newline != nullptr;
                offset += chunk;
            }
        }
        close(fd);
    }).detach();
}

int main(int argc, char **argv) {
    int stop_sddm_status = system("sudo systemctl stop sddm");
    if (stop_sddm_status == -1) {
        log_error("Failed to execute 'sudo systemctl stop sddm': %s\n", strerror(errno));
    } else if (WIFEXITED(stop_sddm_status)) {
        if (WEXITSTATUS(stop_sddm_status) == 0) {
            log_info("Successfully stopped sddm service.\n");
        } else {
            log_error("Stopping sddm service exited with status %d.\n", WEXITSTATUS(stop_sddm_status));
        }
    } else if (WIFSIGNALED(stop_sddm_status)) {
        log_error("Stopping sddm service terminated by signal %d.\n", WTERMSIG(stop_sddm_status));
    } else {
        log_error("Stopping sddm service returned unexpected status.\n");
    }

    setenv("QT_QPA_EGLFS_KMS_DEBUG", "1", 1);
    setenv("QT_QPA_EGLFS_DEBUG", "1", 1);

    const char *drm_node = "/dev/dri/card0";
    const char *socket_path = "/tmp/drm-master";
    int clients = 2;
    uint16_t width = 0, height = 0;
    std::vector<int> forced_planes;

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
            char *end = nullptr;
            long value = strtol(argv[i], &end, 10);
            if (argv[i][0] != '\0' && end && *end == '\0') {
                forced_planes.push_back(static_cast<int>(value));
            } else {
                log_error("Usage: %s [720p|1080p] [--socket path] [--drm node] [--clients n] [plane0 [plane1 ...]]\n", argv[0]);
                return 1;
            }
        }
    }

    if (clients < 2)
        clients = 2;

    std::string qopenhd_plane = forced_planes.size() > 0 ? std::to_string(forced_planes[0]) : std::string("3");
    std::string fpvue_plane = forced_planes.size() > 1 ? std::to_string(forced_planes[1]) : std::string("0");

    int qopenhd_stdout_pipe[2] = {-1, -1};
    int qopenhd_stderr_pipe[2] = {-1, -1};
    bool capture_qopenhd_logs = pipe(qopenhd_stdout_pipe) == 0 && pipe(qopenhd_stderr_pipe) == 0;
    if (!capture_qopenhd_logs) {
        if (qopenhd_stdout_pipe[0] >= 0) {
            close(qopenhd_stdout_pipe[0]);
            close(qopenhd_stdout_pipe[1]);
        }
        if (qopenhd_stderr_pipe[0] >= 0) {
            close(qopenhd_stderr_pipe[0]);
            close(qopenhd_stderr_pipe[1]);
        }
    }

    pid_t qopenhd_pid = fork();
    if (qopenhd_pid == 0) {
        sleep(2);
        if (capture_qopenhd_logs) {
            close(qopenhd_stdout_pipe[0]);
            close(qopenhd_stderr_pipe[0]);
            dup2(qopenhd_stdout_pipe[1], STDOUT_FILENO);
            dup2(qopenhd_stderr_pipe[1], STDERR_FILENO);
            close(qopenhd_stdout_pipe[1]);
            close(qopenhd_stderr_pipe[1]);
        }
        configure_shared_drm_environment(socket_path, drm_node);
        setenv("QT_QPA_EGLFS_KMS_PLANE_INDEX", qopenhd_plane.c_str(), 1);
        const char *current_platform = getenv("QT_QPA_PLATFORM");
        if (!current_platform || strcmp(current_platform, "eglfs") != 0)
            setenv("QT_QPA_PLATFORM", "eglfs", 1);

        const char *kms_config = getenv("QT_QPA_EGLFS_KMS_CONFIG");
        if (!kms_config || kms_config[0] == '\0') {
            const char *default_kms_config = "/root/kms.json";
            if (access(default_kms_config, R_OK) == 0) {
                setenv("QT_QPA_EGLFS_KMS_CONFIG", default_kms_config, 1);
            } else {
                fprintf(stderr,
                        "Warning: default KMS config %s not accessible; QOpenHD may not bind to the expected plane\n",
                        default_kms_config);
            }
        }

        const char *kms_atomic = getenv("QT_QPA_EGLFS_KMS_ATOMIC");
        if (!kms_atomic || kms_atomic[0] == '\0')
            setenv("QT_QPA_EGLFS_KMS_ATOMIC", "1", 1);

        setenv("QT_LOGGING_TO_CONSOLE", "1", 1);
        ensure_preload_in_environment("QOpenHD");
        const char *ld_preload = getenv("LD_PRELOAD");
        const char *qt_platform_value = getenv("QT_QPA_PLATFORM");
        const char *kms_value = getenv("QT_QPA_EGLFS_KMS_CONFIG");
        const char *kms_atomic_value = getenv("QT_QPA_EGLFS_KMS_ATOMIC");
        const char *drm_socket = getenv("FPVUE_DRM_FD_SOCKET");
        const char *drm_device = getenv("FPVUE_DRM_DEVICE_PATH");
        fprintf(stderr,
                "Launching QOpenHD command: QOpenHD --platform=eglfs\n"
                "  QT_QPA_PLATFORM=%s\n"
                "  QT_QPA_EGLFS_KMS_CONFIG=%s\n"
                "  QT_QPA_EGLFS_KMS_ATOMIC=%s\n"
                "  LD_PRELOAD=%s\n"
                "  FPVUE_DRM_FD_SOCKET=%s\n"
                "  FPVUE_DRM_DEVICE_PATH=%s\n",
                qt_platform_value ? qt_platform_value : "(unset)",
                kms_value ? kms_value : "(unset)",
                kms_atomic_value ? kms_atomic_value : "(unset)",
                ld_preload ? ld_preload : "(unset)",
                drm_socket ? drm_socket : "(unset)",
                drm_device ? drm_device : "(unset)");
        fflush(stderr);
        execlp("QOpenHD", "QOpenHD", "--platform=eglfs", NULL);
        fprintf(stderr, "Failed to launch QOpenHD: %s\n", strerror(errno));
        return 1;
    }

    if (capture_qopenhd_logs) {
        close(qopenhd_stdout_pipe[1]);
        close(qopenhd_stderr_pipe[1]);
        pipe_output_to_stream(qopenhd_stdout_pipe[0], stdout, "qopenhd: ");
        pipe_output_to_stream(qopenhd_stderr_pipe[0], stderr, "qopenhd: ");
    }

    log_info("Delaying fpvue color cycle launch by 60 seconds to allow QOpenHD to initialize first.\n");

    int fpvue_stdout_pipe[2] = {-1, -1};
    int fpvue_stderr_pipe[2] = {-1, -1};
    bool capture_fpvue_logs = pipe(fpvue_stdout_pipe) == 0 && pipe(fpvue_stderr_pipe) == 0;
    if (!capture_fpvue_logs) {
        if (fpvue_stdout_pipe[0] >= 0) {
            close(fpvue_stdout_pipe[0]);
            close(fpvue_stdout_pipe[1]);
        }
        if (fpvue_stderr_pipe[0] >= 0) {
            close(fpvue_stderr_pipe[0]);
            close(fpvue_stderr_pipe[1]);
        }
    }

    pid_t fpvue_pid = fork();
    if (fpvue_pid == 0) {
        sleep(60);
        if (capture_fpvue_logs) {
            close(fpvue_stdout_pipe[0]);
            close(fpvue_stderr_pipe[0]);
            dup2(fpvue_stdout_pipe[1], STDOUT_FILENO);
            dup2(fpvue_stderr_pipe[1], STDERR_FILENO);
            close(fpvue_stdout_pipe[1]);
            close(fpvue_stderr_pipe[1]);
        }
        configure_shared_drm_environment(socket_path, drm_node);
        setenv("FPVUE_COLOR_CYCLE_ZPOS", "0", 1);
        setenv("FPVUE_FORCED_PLANE_ID", fpvue_plane.c_str(), 1);
        execlp("fpvue", "fpvue", "--color-cycle", NULL);
        fprintf(stderr, "Failed to launch fpvue color cycle: %s\n", strerror(errno));
        return 1;
    }

    if (capture_fpvue_logs) {
        close(fpvue_stdout_pipe[1]);
        close(fpvue_stderr_pipe[1]);
        pipe_output_to_stream(fpvue_stdout_pipe[0], stdout, "fpvue: ");
        pipe_output_to_stream(fpvue_stderr_pipe[0], stderr, "fpvue: ");
    }

    setenv("FPVUE_LOG_PREFIX", kDisplayHostLogPrefix, 1);

    if (width > 0 && height > 0) {
        log_info("Starting display host with DRM node %s, socket %s, expecting %d clients and forcing mode %ux%u.\n",
                 drm_node, socket_path, clients, width, height);
    } else {
        log_info("Starting display host with DRM node %s, socket %s, expecting %d clients.\n", drm_node, socket_path,
                 clients);
    }
    log_info("Configured QOpenHD plane %s and launched client as PID %d.\n", qopenhd_plane.c_str(), qopenhd_pid);
    log_info("Configured fpvue plane %s and launched color cycle client as PID %d.\n", fpvue_plane.c_str(), fpvue_pid);
    std::thread([qopenhd_pid]() {
        int status = 0;
        pid_t result = waitpid(qopenhd_pid, &status, 0);
        if (result > 0) {
            if (WIFEXITED(status)) {
                log_error("QOpenHD exited with status %d.\n", WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                log_error("QOpenHD terminated by signal %d.\n", WTERMSIG(status));
            }
        }
    }).detach();
    log_info("Qt applications launched by the host automatically share DRM master access via %s.\n", socket_path);

    int fd = start_display_host(drm_node, socket_path, clients, width, height);
    if (fd < 0) {
        log_error("Failed to start display host\n");
        return 1;
    }
    log_info("Display host running. DRM FD %d shared on %s\n", fd, socket_path);
    while (1) {
        sleep(60);
    }
    return 0;
}
