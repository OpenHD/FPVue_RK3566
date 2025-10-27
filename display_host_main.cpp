#include "display_host.h"
#include <ctype.h>
#include <fstream>
#include <iterator>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sstream>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern "C" {
#include "drm.h"
}
#include <xf86drmMode.h>

#ifndef DEFAULT_PRELOAD_PATH
#define DEFAULT_PRELOAD_PATH ""
#endif

struct PlaneAssignment {
    uint32_t primary_plane_id = 0;
    uint32_t overlay_plane_id = 0;
    uint32_t connector_id = 0;
    std::string connector_name;
};

static const char *connector_type_to_string(uint32_t type) {
    switch (type) {
    case DRM_MODE_CONNECTOR_VGA:
        return "VGA";
    case DRM_MODE_CONNECTOR_DVII:
        return "DVI-I";
    case DRM_MODE_CONNECTOR_DVID:
        return "DVI-D";
    case DRM_MODE_CONNECTOR_DVIA:
        return "DVI-A";
    case DRM_MODE_CONNECTOR_Composite:
        return "Composite";
    case DRM_MODE_CONNECTOR_SVIDEO:
        return "SVIDEO";
    case DRM_MODE_CONNECTOR_LVDS:
        return "LVDS";
    case DRM_MODE_CONNECTOR_Component:
        return "Component";
    case DRM_MODE_CONNECTOR_9PinDIN:
        return "DIN";
    case DRM_MODE_CONNECTOR_DisplayPort:
        return "DP";
    case DRM_MODE_CONNECTOR_HDMIA:
        return "HDMI-A";
    case DRM_MODE_CONNECTOR_HDMIB:
        return "HDMI-B";
    case DRM_MODE_CONNECTOR_TV:
        return "TV";
    case DRM_MODE_CONNECTOR_eDP:
        return "eDP";
    case DRM_MODE_CONNECTOR_VIRTUAL:
        return "Virtual";
    case DRM_MODE_CONNECTOR_DSI:
        return "DSI";
    case DRM_MODE_CONNECTOR_DPI:
        return "DPI";
#ifdef DRM_MODE_CONNECTOR_WRITEBACK
    case DRM_MODE_CONNECTOR_WRITEBACK:
        return "Writeback";
#endif
#ifdef DRM_MODE_CONNECTOR_SPI
    case DRM_MODE_CONNECTOR_SPI:
        return "SPI";
#endif
#ifdef DRM_MODE_CONNECTOR_USB
    case DRM_MODE_CONNECTOR_USB:
        return "USB";
#endif
    default:
        return "Unknown";
    }
}

static std::string build_connector_name(uint32_t type, uint32_t type_id) {
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%s-%u", connector_type_to_string(type), type_id);
    return std::string(buffer);
}

static bool determine_plane_assignment(const char *drm_node, PlaneAssignment &assignment) {
    assignment = PlaneAssignment{};
    int fd;
    if (modeset_open(&fd, drm_node) < 0) {
        fprintf(stderr, "Failed to open DRM node %s when determining plane assignment.\n", drm_node);
        return false;
    }

    struct modeset_output *out = static_cast<struct modeset_output *>(calloc(1, sizeof(*out)));
    if (!out) {
        fprintf(stderr, "Failed to allocate modeset output structure.\n");
        close(fd);
        return false;
    }

    if (modeset_prepare(fd, out, 0, 0, 0, DRM_FORMAT_ARGB8888, MODESET_PLANE_TYPE_PRIMARY) != 0) {
        fprintf(stderr, "Unable to locate a primary plane on %s.\n", drm_node);
        free(out);
        close(fd);
        return false;
    }

    assignment.primary_plane_id = out->video_plane.id;
    assignment.connector_id = out->connector.id;

    struct drm_object overlay_plane{};
    if (modeset_find_plane(fd, out, &overlay_plane, DRM_FORMAT_ARGB8888, MODESET_PLANE_TYPE_OVERLAY) == 0) {
        assignment.overlay_plane_id = overlay_plane.id;
    } else {
        fprintf(stderr, "Warning: no overlay plane supporting ARGB8888 detected on %s.\n", drm_node);
    }

    drmModeConnectorPtr connector = drmModeGetConnector(fd, assignment.connector_id);
    if (connector) {
        assignment.connector_name = build_connector_name(connector->connector_type, connector->connector_type_id);
        drmModeFreeConnector(connector);
    }

    modeset_cleanup(fd, out);
    close(fd);
    return assignment.primary_plane_id != 0;
}

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

static bool ensure_debug_sample_video(const char *path) {
    if (access(path, R_OK) == 0)
        return true;

    printf("Debug sample video %s missing, downloading...\n", path);
    pid_t download_pid = fork();
    if (download_pid < 0) {
        perror("fork curl");
        return false;
    }
    if (download_pid == 0) {
        execlp("curl",
               "curl",
               "-L",
               "-o",
               path,
               "https://commondatastorage.googleapis.com/gtv-videos-bucket/sample/BigBuckBunny.mp4",
               (char *)NULL);
        perror("execlp curl");
        _exit(127);
    }

    int status = 0;
    if (waitpid(download_pid, &status, 0) < 0) {
        perror("waitpid curl");
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        int exit_code = -1;
        if (WIFEXITED(status))
            exit_code = WEXITSTATUS(status);
        else if (WIFSIGNALED(status))
            exit_code = 128 + WTERMSIG(status);
        fprintf(stderr, "curl failed with status %d while downloading %s\n", exit_code, path);
        return false;
    }

    if (access(path, R_OK) != 0) {
        fprintf(stderr, "Downloaded sample video %s is not accessible\n", path);
        return false;
    }

    return true;
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

static std::string build_sample_video_pipeline_command(const char *debug_sample_path) {
    std::ostringstream pipeline_builder;
    pipeline_builder << "run_decoder() {\n"
                     << "    decoder=\"$1\";\n"
                     << "    echo \"Attempting debug decode with $decoder\" >&2;\n"
                     << "    gst-launch-1.0 -q filesrc location=" << debug_sample_path
                     << " ! qtdemux name=demux demux.video_0 ! h264parse config-interval=1"
                     << " ! video/x-h264,stream-format=byte-stream,alignment=au ! queue ! \"$decoder\""
                     << " ! videoconvert ! video/x-raw,format=NV12,width=1280,height=720 ! queue ! fdsink fd=1 sync=false;\n"
                     << "}\n"
                     << "(run_decoder omxh264dec || run_decoder mppvideodec || run_decoder avdec_h264)";

    std::string pipeline_command = "(";
    pipeline_command += pipeline_builder.str();
    pipeline_command += ") | fpvue --screen-mode 1280x720@60 --stdin-nv12";
    return pipeline_command;
}

enum class DebugMode {
    None,
    ColorCycle,
    DualVideo,
};

int main(int argc, char **argv) {
    const char *drm_node = "/dev/dri/card0";
    const char *socket_path = "/tmp/drm-master";
    int clients = 2;
    uint16_t width = 0, height = 0;
    DebugMode debug_mode = DebugMode::None;

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
        } else if (strcmp(argv[i], "-debug") == 0 || strcmp(argv[i], "--debug") == 0) {
            debug_mode = DebugMode::ColorCycle;
        } else if (strcmp(argv[i], "-debug2") == 0 || strcmp(argv[i], "--debug2") == 0) {
            debug_mode = DebugMode::DualVideo;
        } else {
            fprintf(stderr,
                    "Usage: %s [720p|1080p] [--socket path] [--drm node] [--clients n] [-debug|-debug2]\n",
                    argv[0]);
            return 1;
        }
    }

    if (clients < 2)
        clients = 2;

    PlaneAssignment plane_assignment;
    bool have_plane_assignment = determine_plane_assignment(drm_node, plane_assignment);
    if (have_plane_assignment) {
        const char *connector = plane_assignment.connector_name.empty() ? "connector" : plane_assignment.connector_name.c_str();
        printf("Detected primary plane %u and overlay plane %u on %s.\n",
               plane_assignment.primary_plane_id,
               plane_assignment.overlay_plane_id,
               connector);
    } else {
        fprintf(stderr, "Warning: failed to determine plane assignment; clients may contend for the same plane.\n");
    }

    const char *debug_sample_path = "/tmp/bbb_720p.mp4";
    bool debug_sample_available = true;
    bool debug_mode_enabled = debug_mode != DebugMode::None;
    if (debug_mode_enabled)
        debug_sample_available = ensure_debug_sample_video(debug_sample_path);

    if (debug_mode == DebugMode::DualVideo && !debug_sample_available) {
        fprintf(stderr,
                "Falling back to debug color cycle overlay; sample video unavailable for dual video mode.\n");
        debug_mode = DebugMode::ColorCycle;
    }

    pid_t primary_pid = fork();
    bool primary_is_sample_player = debug_mode != DebugMode::None && debug_sample_available;
    if (primary_pid == 0) {
        sleep(1);
        configure_shared_drm_environment(socket_path, drm_node);

        if (primary_is_sample_player) {
            if (have_plane_assignment && plane_assignment.overlay_plane_id != 0) {
                char reserved_planes[32];
                snprintf(reserved_planes, sizeof(reserved_planes), "%u", plane_assignment.overlay_plane_id);
                setenv("FPVUE_RESERVED_PLANE_IDS", reserved_planes, 1);
            }
            if (have_plane_assignment && plane_assignment.primary_plane_id != 0 &&
                debug_mode == DebugMode::DualVideo) {
                char primary_plane[32];
                snprintf(primary_plane, sizeof(primary_plane), "%u", plane_assignment.primary_plane_id);
                setenv("FPVUE_STDIN_NV12_PLANE_ID", primary_plane, 1);
            }

            // Attempt to use platform-specific hardware decoders first and fall back to a
            // software decoder if negotiation keeps failing.
            std::string pipeline_command = build_sample_video_pipeline_command(debug_sample_path);
            fprintf(stderr, "Launching debug sample video pipeline command:\n%s\n", pipeline_command.c_str());

            execlp("sh", "sh", "-c", pipeline_command.c_str(), (char *)NULL);
            perror("execlp sample video pipeline");
            return 1;
        }

        setenv("FPVUE_COLOR_CYCLE_ZPOS", "0", 1);
        execlp("fpvue", "fpvue", "--color-cycle", NULL);
        perror("execlp fpvue");
        return 1;
    }

    if (primary_pid < 0) {
        perror("fork primary client");
        return 1;
    }

    if (debug_mode != DebugMode::None && !debug_sample_available) {
        fprintf(stderr, "Falling back to color cycle primary client; sample video unavailable.\n");
        primary_is_sample_player = false;
    }

    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    bool capture_qopenhd_logs = false;
    pid_t overlay_pid = -1;
    bool overlay_is_qopenhd = false;
    bool overlay_is_sample_player = false;

    if (!debug_mode_enabled) {
        capture_qopenhd_logs = pipe(stdout_pipe) == 0 && pipe(stderr_pipe) == 0;
        if (!capture_qopenhd_logs) {
            if (stdout_pipe[0] >= 0) {
                close(stdout_pipe[0]);
                close(stdout_pipe[1]);
            }
            if (stderr_pipe[0] >= 0) {
                close(stderr_pipe[0]);
                close(stderr_pipe[1]);
            }
        }

        pid_t qopenhd_pid = fork();
        if (qopenhd_pid == 0) {
            sleep(2);
            configure_shared_drm_environment(socket_path, drm_node);
            if (have_plane_assignment && plane_assignment.primary_plane_id != 0) {
                char reserved_planes[32];
                snprintf(reserved_planes, sizeof(reserved_planes), "%u", plane_assignment.primary_plane_id);
                setenv("FPVUE_RESERVED_PLANE_IDS", reserved_planes, 1);
                if (plane_assignment.overlay_plane_id != 0) {
                    char overlay_plane[32];
                    snprintf(overlay_plane, sizeof(overlay_plane), "%u", plane_assignment.overlay_plane_id);
                    setenv("FPVUE_OVERLAY_PLANE_ID", overlay_plane, 1);
                }
            }
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

            if (capture_qopenhd_logs) {
                close(stdout_pipe[0]);
                close(stderr_pipe[0]);
                dup2(stdout_pipe[1], STDOUT_FILENO);
                dup2(stderr_pipe[1], STDERR_FILENO);
                close(stdout_pipe[1]);
                close(stderr_pipe[1]);
            }

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
            perror("execlp qopenhd");
            return 1;
        }

        overlay_pid = qopenhd_pid;
        overlay_is_qopenhd = true;

        if (capture_qopenhd_logs) {
            close(stdout_pipe[1]);
            close(stderr_pipe[1]);
            pipe_output_to_stream(stdout_pipe[0], stdout, "[QOpenHD] ");
            pipe_output_to_stream(stderr_pipe[0], stderr, "[QOpenHD] ");
        }
    } else if (debug_mode == DebugMode::ColorCycle) {
        pid_t debug_pid = fork();
        if (debug_pid == 0) {
            sleep(2);
            configure_shared_drm_environment(socket_path, drm_node);
            setenv("FPVUE_COLOR_CYCLE_ZPOS", "1", 1);
            if (have_plane_assignment && plane_assignment.primary_plane_id != 0) {
                char reserved_planes[32];
                snprintf(reserved_planes, sizeof(reserved_planes), "%u", plane_assignment.primary_plane_id);
                setenv("FPVUE_RESERVED_PLANE_IDS", reserved_planes, 1);
            }
            if (have_plane_assignment && plane_assignment.overlay_plane_id != 0) {
                char overlay_plane[32];
                snprintf(overlay_plane, sizeof(overlay_plane), "%u", plane_assignment.overlay_plane_id);
                setenv("FPVUE_COLOR_CYCLE_PLANE_ID", overlay_plane, 1);
            }
            setenv("FPVUE_COLOR_CYCLE_PLANE_TYPE", "overlay", 1);

            char overlay_width[16];
            char overlay_height[16];
            char overlay_x[16];
            char overlay_y[16];
            snprintf(overlay_width, sizeof(overlay_width), "%d", 640);
            snprintf(overlay_height, sizeof(overlay_height), "%d", 360);
            snprintf(overlay_x, sizeof(overlay_x), "%d", 100);
            snprintf(overlay_y, sizeof(overlay_y), "%d", 100);
            setenv("FPVUE_COLOR_CYCLE_BUFFER_WIDTH", overlay_width, 0);
            setenv("FPVUE_COLOR_CYCLE_BUFFER_HEIGHT", overlay_height, 0);
            setenv("FPVUE_COLOR_CYCLE_CRTC_WIDTH", overlay_width, 0);
            setenv("FPVUE_COLOR_CYCLE_CRTC_HEIGHT", overlay_height, 0);
            setenv("FPVUE_COLOR_CYCLE_CRTC_X", overlay_x, 0);
            setenv("FPVUE_COLOR_CYCLE_CRTC_Y", overlay_y, 0);

            execlp("fpvue", "fpvue", "--color-cycle", NULL);
            perror("execlp fpvue debug overlay");
            return 1;
        }
        overlay_pid = debug_pid;
    } else if (debug_mode == DebugMode::DualVideo) {
        pid_t debug_pid = fork();
        if (debug_pid == 0) {
            sleep(3);
            configure_shared_drm_environment(socket_path, drm_node);
            setenv("FPVUE_STDIN_NV12_PLANE_TYPE", "overlay", 1);
            setenv("FPVUE_STDIN_NV12_ZPOS", "1", 1);
            if (have_plane_assignment && plane_assignment.primary_plane_id != 0) {
                char reserved_planes[32];
                snprintf(reserved_planes, sizeof(reserved_planes), "%u", plane_assignment.primary_plane_id);
                setenv("FPVUE_RESERVED_PLANE_IDS", reserved_planes, 1);
            }
            if (have_plane_assignment && plane_assignment.overlay_plane_id != 0) {
                char overlay_plane[32];
                snprintf(overlay_plane, sizeof(overlay_plane), "%u", plane_assignment.overlay_plane_id);
                setenv("FPVUE_STDIN_NV12_PLANE_ID", overlay_plane, 1);
            }

            char overlay_width[16];
            char overlay_height[16];
            char overlay_x[16];
            char overlay_y[16];
            snprintf(overlay_width, sizeof(overlay_width), "%d", 640);
            snprintf(overlay_height, sizeof(overlay_height), "%d", 360);
            snprintf(overlay_x, sizeof(overlay_x), "%d", 100);
            snprintf(overlay_y, sizeof(overlay_y), "%d", 100);
            setenv("FPVUE_STDIN_NV12_CRTC_WIDTH", overlay_width, 1);
            setenv("FPVUE_STDIN_NV12_CRTC_HEIGHT", overlay_height, 1);
            setenv("FPVUE_STDIN_NV12_CRTC_X", overlay_x, 1);
            setenv("FPVUE_STDIN_NV12_CRTC_Y", overlay_y, 1);

            std::string pipeline_command = build_sample_video_pipeline_command(debug_sample_path);
            fprintf(stderr, "Launching debug dual video overlay pipeline command:\n%s\n", pipeline_command.c_str());

            execlp("sh", "sh", "-c", pipeline_command.c_str(), (char *)NULL);
            perror("execlp dual video overlay pipeline");
            return 1;
        }
        overlay_pid = debug_pid;
        overlay_is_sample_player = true;
    }

    printf("Starting display host with DRM node %s, socket %s, expecting %d clients", drm_node, socket_path, clients);
    if (width > 0 && height > 0) {
        printf(", forcing mode %ux%u", width, height);
    }
    printf("\n");
    if (primary_is_sample_player) {
        printf("Launched debug sample video pipeline as PID %d.\n", primary_pid);
    } else {
        printf("Launched fpvue color cycle client as PID %d.\n", primary_pid);
    }
    if (have_plane_assignment) {
        printf("Reserved primary plane %u for fpvue.\n", plane_assignment.primary_plane_id);
    }
    if (overlay_pid > 0) {
        if (overlay_is_qopenhd) {
            if (have_plane_assignment) {
                if (plane_assignment.overlay_plane_id != 0) {
                    printf("Launched QOpenHD client as PID %d with overlay plane %u available.\n", overlay_pid,
                           plane_assignment.overlay_plane_id);
                } else {
                    printf("Launched QOpenHD client as PID %d with overlay plane discovery unavailable.\n", overlay_pid);
                }
            } else {
                printf("Launched QOpenHD client as PID %d using overlay plane.\n", overlay_pid);
            }
        } else if (overlay_is_sample_player) {
            if (have_plane_assignment && plane_assignment.overlay_plane_id != 0) {
                printf("Launched debug dual video overlay as PID %d on plane %u.\n",
                       overlay_pid,
                       plane_assignment.overlay_plane_id);
            } else {
                printf("Launched debug dual video overlay as PID %d without detected overlay plane.\n", overlay_pid);
            }
        } else {
            if (have_plane_assignment && plane_assignment.overlay_plane_id != 0) {
                printf("Launched debug color cycle overlay as PID %d on plane %u.\n", overlay_pid,
                       plane_assignment.overlay_plane_id);
            } else {
                printf("Launched debug color cycle overlay as PID %d without detected overlay plane.\n", overlay_pid);
            }
        }
    } else if (debug_mode == DebugMode::ColorCycle) {
        printf("Debug overlay color cycle launch failed; see logs for details.\n");
    } else if (debug_mode == DebugMode::DualVideo) {
        printf("Debug dual video overlay launch failed; see logs for details.\n");
    }

    if (overlay_pid > 0) {
        std::thread([overlay_pid, overlay_is_qopenhd, overlay_is_sample_player]() {
            int status = 0;
            pid_t result = waitpid(overlay_pid, &status, 0);
            if (result > 0) {
                if (WIFEXITED(status)) {
                    if (overlay_is_qopenhd) {
                        fprintf(stderr, "QOpenHD exited with status %d.\n", WEXITSTATUS(status));
                    } else if (overlay_is_sample_player) {
                        fprintf(stderr, "Debug dual video overlay exited with status %d.\n", WEXITSTATUS(status));
                    } else {
                        fprintf(stderr, "Debug color cycle overlay exited with status %d.\n", WEXITSTATUS(status));
                    }
                } else if (WIFSIGNALED(status)) {
                    if (overlay_is_qopenhd) {
                        fprintf(stderr, "QOpenHD terminated by signal %d.\n", WTERMSIG(status));
                    } else if (overlay_is_sample_player) {
                        fprintf(stderr, "Debug dual video overlay terminated by signal %d.\n", WTERMSIG(status));
                    } else {
                        fprintf(stderr, "Debug color cycle overlay terminated by signal %d.\n", WTERMSIG(status));
                    }
                }
            }
        }).detach();
    }

    if (overlay_is_qopenhd) {
        printf("Qt applications launched by the host automatically share DRM master access via %s.\n", socket_path);
    } else if (debug_mode == DebugMode::DualVideo) {
        printf("Display helper debug2 mode launched dual sample video clients for DRM testing.\n");
    } else if (debug_mode == DebugMode::ColorCycle) {
        printf("Display helper debug mode launched dual color cycle clients for DRM testing.\n");
    }

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
