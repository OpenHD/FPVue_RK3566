#include "allwinnerv4l2display.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>
#include <sstream>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

namespace {
constexpr uint32_t kDefaultInputBuffers = 6;
constexpr uint32_t kDefaultOutputBuffers = 6;
constexpr uint32_t kDisplayBufferCount = 4;
constexpr uint32_t kMaxFrameSize = 4 * 1024 * 1024;

template <typename... Args>
void debug_log(const char* tag, Args&&... args) {
  std::ostringstream oss;
  oss << "[" << tag << "] ";
  (oss << ... << args);
  oss << '\n';
  auto now = std::chrono::system_clock::now();
  auto time = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  localtime_r(&time, &tm);
  char buf[64];
  strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
  std::cerr << buf << " " << oss.str();
}

#if HAVE_AW_OMX
struct ScopedOmxBufferFlag {
  OMX_BUFFERHEADERTYPE* buffer;
  OMX_U32 flags;
  explicit ScopedOmxBufferFlag(OMX_BUFFERHEADERTYPE* buf, OMX_U32 mask)
      : buffer(buf), flags(mask) {
    if (buffer)
      buffer->nFlags |= mask;
  }
  ~ScopedOmxBufferFlag() {
    if (buffer)
      buffer->nFlags &= ~flags;
  }
};
#endif

}  // namespace

AllwinnerV4L2Display::AllwinnerV4L2Display(int udp_port,
                                           bool h265,
                                           uint32_t mode_width,
                                           uint32_t mode_height,
                                           uint32_t mode_vrefresh)
    : m_port(udp_port),
      m_h265(h265),
      m_stream_width(mode_width ? mode_width : 1280),
      m_stream_height(mode_height ? mode_height : 720),
      m_stream_vrefresh(mode_vrefresh ? mode_vrefresh : 60),
      m_display_width(mode_width),
      m_display_height(mode_height),
      m_display_vrefresh(mode_vrefresh) {
  if (m_port < 0) {
    m_input_mode = InputMode::StdIn;
    m_input_fd = STDIN_FILENO;
  }
}

void AllwinnerV4L2Display::set_external_drm_fd(int fd, bool take_ownership) {
  m_drm_fd = fd;
  m_take_ownership_of_drm_fd = take_ownership;
}

void AllwinnerV4L2Display::override_input_mode(InputMode mode) {
  m_input_mode = mode;
  if (mode == InputMode::StdIn && m_input_fd < 0) {
    m_input_fd = STDIN_FILENO;
  }
}

AllwinnerV4L2Display::~AllwinnerV4L2Display() { stop(); }

bool AllwinnerV4L2Display::start() {
  debug_log("cedar", "Starting Allwinner Cedar decode pipeline. H265=", m_h265);
  if (!setup_network()) {
    std::cerr << "Failed to setup network" << std::endl;
    stop();
    return false;
  }
  if (!setup_drm()) {
    std::cerr << "Failed to setup DRM" << std::endl;
    stop();
    return false;
  }
  if (!setup_omx()) {
    std::cerr << "Failed to setup Cedar OpenMAX pipeline" << std::endl;
    stop();
    return false;
  }
  m_modeset_initialized = false;
  m_running = true;
  m_thread = std::thread(&AllwinnerV4L2Display::decode_loop, this);
  return true;
}

void AllwinnerV4L2Display::stop() {
  if (m_running) {
    debug_log("cedar", "Stopping decode loop");
    m_running = false;
    if (m_thread.joinable())
      m_thread.join();
  }
  m_modeset_initialized = false;

  if (m_sock >= 0 && m_input_mode == InputMode::UDP) {
    close(m_sock);
    m_sock = -1;
  }

#if HAVE_AW_OMX
  teardown_omx();
#endif
  teardown_drm_resources();
}

bool AllwinnerV4L2Display::setup_network() {
  if (m_input_mode == InputMode::StdIn) {
    m_sock = -1;
    if (m_input_fd < 0)
      m_input_fd = STDIN_FILENO;
    debug_log("cedar", "Reading compressed video from stdin.");
    return true;
  }

  m_sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (m_sock < 0) {
    perror("socket");
    return false;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(m_port);
  addr.sin_addr.s_addr = INADDR_ANY;
  if (bind(m_sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
    perror("bind");
    return false;
  }
  debug_log("cedar", "Listening on UDP port ", m_port, " for RTP.");
  return true;
}

bool AllwinnerV4L2Display::setup_drm() {
  if (m_drm_fd < 0) {
    if (modeset_open(&m_drm_fd, "/dev/dri/card0") < 0) {
      perror("modeset_open");
      return false;
    }
  }
  if (modeset_prepare(m_drm_fd,
                      &m_output,
                      m_display_width,
                      m_display_height,
                      m_display_vrefresh,
                      DRM_FORMAT_NV12,
                      MODESET_PLANE_TYPE_PRIMARY) < 0) {
    std::cerr << "modeset_prepare failed" << std::endl;
    return false;
  }
  m_drm_prepared = true;
  debug_log("cedar", "DRM prepared with requested mode ", m_display_width, "x",
            m_display_height, "@", m_display_vrefresh);
  return true;
}

void AllwinnerV4L2Display::teardown_drm_resources() {
  if (m_drm_fd < 0)
    return;

#if HAVE_AW_OMX
  recycle_display_buffers();
#endif

  if (m_drm_prepared) {
    if (m_output.video_request) {
      drmModeAtomicFree(m_output.video_request);
      m_output.video_request = nullptr;
    }
    modeset_cleanup(m_drm_fd, &m_output);
    m_drm_prepared = false;
  }
  if (m_take_ownership_of_drm_fd) {
    close(m_drm_fd);
  }
  m_drm_fd = -1;
}

#if !HAVE_AW_OMX

bool AllwinnerV4L2Display::setup_omx() {
  std::cerr << "This build does not include Allwinner OpenMAX IL headers." << std::endl;
  std::cerr << "Install the cedarx OpenMAX development package and rebuild." << std::endl;
  return false;
}

void AllwinnerV4L2Display::decode_loop() {
  std::cerr << "Cedar decode loop unavailable without OpenMAX support." << std::endl;
}

void AllwinnerV4L2Display::teardown_omx() {}

#else  // HAVE_AW_OMX

bool AllwinnerV4L2Display::ensure_display_buffers(uint32_t width, uint32_t height) {
  if (!m_drm_prepared)
    return false;

  if (width == 0 || height == 0)
    return false;

  bool needs_realloc = m_drm_buffers.empty() || width != m_allocated_width ||
                       height != m_allocated_height || m_display_stride == 0;

  if (!needs_realloc)
    return true;

  recycle_display_buffers();

  debug_log("cedar", "Allocating ", kDisplayBufferCount,
            " DRM NV12 buffers for ", width, "x", height);

  m_drm_buffers.resize(kDisplayBufferCount);
  uint32_t stride = width;
  uint32_t plane1_offset = stride * height;
  size_t total_height = height * 3 / 2;
  size_t size = static_cast<size_t>(stride) * total_height;

  for (auto& buf : m_drm_buffers) {
    struct drm_mode_create_dumb create{};
    create.width = stride;
    create.height = total_height;
    create.bpp = 8;
    if (ioctl(m_drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
      perror("CREATE_DUMB");
      recycle_display_buffers();
      return false;
    }

    buf.handle = create.handle;
    buf.size = static_cast<size_t>(create.pitch) * create.height;
    buf.pitches[0] = stride;
    buf.pitches[1] = stride;
    buf.offsets[0] = 0;
    buf.offsets[1] = plane1_offset;

    uint32_t handles[4] = {buf.handle, buf.handle, 0, 0};
    uint32_t pitches[4] = {buf.pitches[0], buf.pitches[1], 0, 0};
    uint32_t offsets[4] = {buf.offsets[0], buf.offsets[1], 0, 0};
    if (drmModeAddFB2(m_drm_fd,
                      width,
                      height,
                      DRM_FORMAT_NV12,
                      handles,
                      pitches,
                      offsets,
                      &buf.fb_id,
                      0) != 0) {
      perror("AddFB2");
      recycle_display_buffers();
      return false;
    }

    struct drm_mode_map_dumb map{};
    map.handle = buf.handle;
    if (ioctl(m_drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) {
      perror("MAP_DUMB");
      recycle_display_buffers();
      return false;
    }
    buf.map = mmap(nullptr, buf.size, PROT_READ | PROT_WRITE, MAP_SHARED, m_drm_fd, map.offset);
    if (buf.map == MAP_FAILED) {
      perror("mmap dumb");
      recycle_display_buffers();
      return false;
    }
  }

  m_display_stride = stride;
  m_allocated_width = width;
  m_allocated_height = height;
  m_drm_buffer_index = 0;
  return true;
}

void AllwinnerV4L2Display::recycle_display_buffers() {
  for (auto& buf : m_drm_buffers) {
    if (buf.map && buf.map != MAP_FAILED) {
      munmap(buf.map, buf.size);
      buf.map = nullptr;
    }
    if (buf.fb_id) {
      drmModeRmFB(m_drm_fd, buf.fb_id);
      buf.fb_id = 0;
    }
    if (buf.handle) {
      struct drm_mode_destroy_dumb destroy{};
      destroy.handle = buf.handle;
      ioctl(m_drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
      buf.handle = 0;
    }
  }
  m_drm_buffers.clear();
  m_display_stride = 0;
  m_allocated_width = 0;
  m_allocated_height = 0;
}

bool AllwinnerV4L2Display::wait_for_state(OMX_STATETYPE state, int timeout_ms) {
  std::unique_lock<std::mutex> lock(m_state_mutex);
  return m_state_cv.wait_for(lock,
                             std::chrono::milliseconds(timeout_ms),
                             [&] { return m_decoder_state == state; });
}

OMX_ERRORTYPE AllwinnerV4L2Display::omx_event_handler(OMX_HANDLETYPE,
                                                      OMX_PTR app_data,
                                                      OMX_EVENTTYPE event,
                                                      OMX_U32 data1,
                                                      OMX_U32 data2,
                                                      OMX_PTR) {
  auto* self = static_cast<AllwinnerV4L2Display*>(app_data);
  if (!self)
    return OMX_ErrorNone;

  debug_log("omx", "Event", static_cast<int>(event), " d1=", data1, " d2=", data2);

  if (event == OMX_EventCmdComplete && data1 == OMX_CommandStateSet) {
    std::lock_guard<std::mutex> lock(self->m_state_mutex);
    self->m_decoder_state = static_cast<OMX_STATETYPE>(data2);
    self->m_state_cv.notify_all();
  } else if (event == OMX_EventPortSettingsChanged) {
    debug_log("omx", "Port settings changed on port", data1);
    self->m_port_settings_dirty = true;
  } else if (event == OMX_EventError) {
    debug_log("omx", "Decoder error", std::hex, data1, std::dec);
  }
  return OMX_ErrorNone;
}

OMX_ERRORTYPE AllwinnerV4L2Display::omx_empty_buffer_done(OMX_HANDLETYPE,
                                                          OMX_PTR app_data,
                                                          OMX_BUFFERHEADERTYPE* buffer) {
  auto* self = static_cast<AllwinnerV4L2Display*>(app_data);
  if (!self || !buffer)
    return OMX_ErrorNone;

  {
    std::lock_guard<std::mutex> lock(self->m_input_mutex);
    self->m_available_inputs.push(buffer);
  }
  self->m_input_cv.notify_one();
  return OMX_ErrorNone;
}

OMX_ERRORTYPE AllwinnerV4L2Display::omx_fill_buffer_done(OMX_HANDLETYPE,
                                                         OMX_PTR app_data,
                                                         OMX_BUFFERHEADERTYPE* buffer) {
  auto* self = static_cast<AllwinnerV4L2Display*>(app_data);
  if (!self || !buffer)
    return OMX_ErrorNone;

  {
    std::lock_guard<std::mutex> lock(self->m_output_mutex);
    self->m_filled_outputs.push(buffer);
  }
  self->m_output_cv.notify_one();
  return OMX_ErrorNone;
}

bool AllwinnerV4L2Display::setup_omx() {
  OMX_ERRORTYPE err = OMX_Init();
  if (err != OMX_ErrorNone) {
    std::cerr << "OMX_Init failed: 0x" << std::hex << err << std::dec << std::endl;
    return false;
  }

  m_callbacks.EventHandler = omx_event_handler;
  m_callbacks.EmptyBufferDone = omx_empty_buffer_done;
  m_callbacks.FillBufferDone = omx_fill_buffer_done;

  const char* component_name = m_h265 ? "OMX.allwinner.video.decoder.hevc"
                                      : "OMX.allwinner.video.decoder.avc";
  err = OMX_GetHandle(&m_decoder, (OMX_STRING)component_name, this, &m_callbacks);
  if (err != OMX_ErrorNone) {
    std::cerr << "OMX_GetHandle(" << component_name << ") failed: 0x" << std::hex
              << err << std::dec << std::endl;
    OMX_Deinit();
    return false;
  }

  debug_log("cedar", "OMX component " , component_name , " acquired");

  auto fill_header = [](OMX_PTR header, OMX_U32 size) {
    OMX_VERSIONTYPE* ver = reinterpret_cast<OMX_VERSIONTYPE*>(
        reinterpret_cast<uint8_t*>(header) + sizeof(OMX_U32));
    *reinterpret_cast<OMX_U32*>(header) = size;
    ver->s.nVersionMajor = 1;
    ver->s.nVersionMinor = 1;
    ver->s.nRevision = 2;
    ver->s.nStep = 0;
  };

  OMX_PARAM_PORTDEFINITIONTYPE port_def{};
  fill_header(&port_def, sizeof(port_def));
  port_def.nPortIndex = 0;
  err = OMX_GetParameter(m_decoder, OMX_IndexParamPortDefinition, &port_def);
  if (err != OMX_ErrorNone) {
    std::cerr << "Failed to query input port definition: 0x" << std::hex << err
              << std::dec << std::endl;
    return false;
  }
  port_def.format.video.cMIMEType = (OMX_STRING)(m_h265 ? "video/hevc" : "video/avc");
  port_def.format.video.eCompressionFormat =
      m_h265 ? OMX_VIDEO_CodingHEVC : OMX_VIDEO_CodingAVC;
  port_def.format.video.nFrameWidth = m_stream_width;
  port_def.format.video.nFrameHeight = m_stream_height;
  port_def.nBufferCountActual = std::max(port_def.nBufferCountMin, kDefaultInputBuffers);
  err = OMX_SetParameter(m_decoder, OMX_IndexParamPortDefinition, &port_def);
  if (err != OMX_ErrorNone) {
    std::cerr << "Failed to set input port definition: 0x" << std::hex << err
              << std::dec << std::endl;
    return false;
  }

  fill_header(&port_def, sizeof(port_def));
  port_def.nPortIndex = 1;
  err = OMX_GetParameter(m_decoder, OMX_IndexParamPortDefinition, &port_def);
  if (err != OMX_ErrorNone) {
    std::cerr << "Failed to query output port definition: 0x" << std::hex << err
              << std::dec << std::endl;
    return false;
  }
  port_def.format.video.eColorFormat = OMX_COLOR_FormatYUV420Planar;
  port_def.nBufferCountActual = std::max(port_def.nBufferCountMin, kDefaultOutputBuffers);
  err = OMX_SetParameter(m_decoder, OMX_IndexParamPortDefinition, &port_def);
  if (err != OMX_ErrorNone) {
    std::cerr << "Failed to set output port definition: 0x" << std::hex << err
              << std::dec << std::endl;
    return false;
  }

  err = OMX_SendCommand(m_decoder, OMX_CommandStateSet, OMX_StateIdle, nullptr);
  if (err != OMX_ErrorNone) {
    std::cerr << "Failed to request Idle state: 0x" << std::hex << err << std::dec
              << std::endl;
    return false;
  }

  m_input_buffers.resize(port_def.nBufferCountActual, nullptr);
  m_output_buffers.resize(port_def.nBufferCountActual, nullptr);

  debug_log("cedar", "Allocating ", port_def.nBufferCountActual,
            " input buffers and ", port_def.nBufferCountActual, " output buffers");

  for (uint32_t i = 0; i < m_input_buffers.size(); ++i) {
    err = OMX_AllocateBuffer(m_decoder,
                             &m_input_buffers[i],
                             0,
                             this,
                             std::max(port_def.nBufferSize, kMaxFrameSize));
    if (err != OMX_ErrorNone) {
      std::cerr << "Failed to allocate input buffer #" << i << " : 0x" << std::hex
                << err << std::dec << std::endl;
      return false;
    }
    m_available_inputs.push(m_input_buffers[i]);
  }

  fill_header(&port_def, sizeof(port_def));
  port_def.nPortIndex = 1;
  err = OMX_GetParameter(m_decoder, OMX_IndexParamPortDefinition, &port_def);
  if (err != OMX_ErrorNone) {
    std::cerr << "Failed to re-query output port definition: 0x" << std::hex << err
              << std::dec << std::endl;
    return false;
  }

  for (uint32_t i = 0; i < m_output_buffers.size(); ++i) {
    err = OMX_AllocateBuffer(m_decoder,
                             &m_output_buffers[i],
                             1,
                             this,
                             port_def.nBufferSize);
    if (err != OMX_ErrorNone) {
      std::cerr << "Failed to allocate output buffer #" << i << " : 0x" << std::hex
                << err << std::dec << std::endl;
      return false;
    }
  }

  if (!wait_for_state(OMX_StateIdle)) {
    std::cerr << "Timeout waiting for decoder to enter Idle state" << std::endl;
    return false;
  }

  err = OMX_SendCommand(m_decoder, OMX_CommandStateSet, OMX_StateExecuting, nullptr);
  if (err != OMX_ErrorNone) {
    std::cerr << "Failed to request Executing state: 0x" << std::hex << err << std::dec
              << std::endl;
    return false;
  }

  if (!wait_for_state(OMX_StateExecuting)) {
    std::cerr << "Timeout waiting for decoder to enter Executing state" << std::endl;
    return false;
  }

  for (auto* buffer : m_output_buffers) {
    buffer->nFilledLen = 0;
    buffer->nOffset = 0;
    err = OMX_FillThisBuffer(m_decoder, buffer);
    if (err != OMX_ErrorNone) {
      std::cerr << "OMX_FillThisBuffer failed: 0x" << std::hex << err << std::dec
                << std::endl;
      return false;
    }
  }

  debug_log("cedar", "Decoder entered executing state");
  return true;
}

void AllwinnerV4L2Display::teardown_omx() {
  if (!m_decoder)
    return;

  OMX_SendCommand(m_decoder, OMX_CommandStateSet, OMX_StateIdle, nullptr);
  wait_for_state(OMX_StateIdle, 1000);
  OMX_SendCommand(m_decoder, OMX_CommandStateSet, OMX_StateLoaded, nullptr);
  wait_for_state(OMX_StateLoaded, 1000);

  for (auto* buffer : m_output_buffers) {
    if (buffer)
      OMX_FreeBuffer(m_decoder, 1, buffer);
  }
  for (auto* buffer : m_input_buffers) {
    if (buffer)
      OMX_FreeBuffer(m_decoder, 0, buffer);
  }
  m_output_buffers.clear();
  m_input_buffers.clear();

  OMX_FreeHandle(m_decoder);
  m_decoder = nullptr;
  OMX_Deinit();

  while (!m_available_inputs.empty())
    m_available_inputs.pop();
  while (!m_filled_outputs.empty())
    m_filled_outputs.pop();
  m_port_settings_dirty = false;
  m_decoder_state = OMX_StateLoaded;
}

static void copy_planar_to_nv12(uint8_t* dst,
                                uint32_t dst_stride,
                                uint32_t dst_uv_stride,
                                const uint8_t* src,
                                uint32_t width,
                                uint32_t height) {
  const uint8_t* src_y = src;
  const uint8_t* src_u = src_y + width * height;
  const uint8_t* src_v = src_u + (width / 2) * (height / 2);

  for (uint32_t row = 0; row < height; ++row) {
    memcpy(dst + row * dst_stride, src_y + row * width, width);
  }

  uint8_t* dst_uv = dst + dst_uv_stride * height;
  for (uint32_t row = 0; row < height / 2; ++row) {
    uint8_t* dst_row = dst_uv + row * dst_uv_stride;
    const uint8_t* u_row = src_u + row * (width / 2);
    const uint8_t* v_row = src_v + row * (width / 2);
    for (uint32_t col = 0; col < width / 2; ++col) {
      dst_row[2 * col] = u_row[col];
      dst_row[2 * col + 1] = v_row[col];
    }
  }
}

void AllwinnerV4L2Display::decode_loop() {
  std::vector<uint8_t> rx_buffer(4096 + 8);
  std::vector<uint8_t> nal_buffer(kMaxFrameSize);
  uint64_t frames_decoded = 0;

  debug_log("cedar", "Decode loop running.");

  while (m_running) {
    ssize_t rx = 0;
    uint8_t* nal = nullptr;
    uint32_t nal_size = 0;
    if (m_input_mode == InputMode::UDP) {
      rx = recv(m_sock, rx_buffer.data(), rx_buffer.size(), 0);
      if (rx <= 0) {
        if (rx < 0 && errno == EINTR)
          continue;
        usleep(5 * 1000);
        continue;
      }
      nal = decode_frame(rx_buffer.data(), rx, 0, nal_buffer.data(), &nal_size);
      if (!nal)
        continue;
    } else {
      rx = read(m_input_fd, nal_buffer.data(), nal_buffer.size());
      if (rx <= 0) {
        if (rx < 0 && errno == EINTR)
          continue;
        usleep(5 * 1000);
        continue;
      }
      nal = nal_buffer.data();
      nal_size = static_cast<uint32_t>(rx);
    }

    OMX_BUFFERHEADERTYPE* input_buffer = nullptr;
    {
      std::unique_lock<std::mutex> lock(m_input_mutex);
      if (!m_input_cv.wait_for(lock,
                               std::chrono::milliseconds(500),
                               [&] { return !m_available_inputs.empty() || !m_running; })) {
        debug_log("cedar", "Timed out waiting for input buffer");
        continue;
      }
      if (!m_running)
        break;
      input_buffer = m_available_inputs.front();
      m_available_inputs.pop();
    }

    if (!input_buffer)
      continue;

    if (nal_size > input_buffer->nAllocLen) {
      debug_log("cedar", "NAL too large (", nal_size, " > ", input_buffer->nAllocLen,
                ") truncating");
      nal_size = input_buffer->nAllocLen;
    }

    memcpy(input_buffer->pBuffer, nal, nal_size);
    input_buffer->nFilledLen = nal_size;
    input_buffer->nOffset = 0;
    input_buffer->nTimeStamp = frames_decoded;
    {
      ScopedOmxBufferFlag frame_flag(input_buffer, OMX_BUFFERFLAG_ENDOFFRAME);
      OMX_ERRORTYPE err = OMX_EmptyThisBuffer(m_decoder, input_buffer);
      if (err != OMX_ErrorNone) {
        std::cerr << "OMX_EmptyThisBuffer failed: 0x" << std::hex << err << std::dec
                  << std::endl;
        {
          std::lock_guard<std::mutex> lock(m_input_mutex);
          m_available_inputs.push(input_buffer);
        }
        continue;
      }
    }

    OMX_BUFFERHEADERTYPE* output_buffer = nullptr;
    {
      std::unique_lock<std::mutex> lock(m_output_mutex);
      if (!m_output_cv.wait_for(lock,
                                std::chrono::milliseconds(1000),
                                [&] { return !m_filled_outputs.empty() || !m_running; })) {
        debug_log("cedar", "No decoded frame available (timeout)");
        continue;
      }
      if (!m_running)
        break;
      output_buffer = m_filled_outputs.front();
      m_filled_outputs.pop();
    }

    if (!output_buffer)
      continue;

    if (m_port_settings_dirty) {
      OMX_PARAM_PORTDEFINITIONTYPE new_def{};
      auto fill_header = [](OMX_PTR header, OMX_U32 size) {
        OMX_VERSIONTYPE* ver = reinterpret_cast<OMX_VERSIONTYPE*>(
            reinterpret_cast<uint8_t*>(header) + sizeof(OMX_U32));
        *reinterpret_cast<OMX_U32*>(header) = size;
        ver->s.nVersionMajor = 1;
        ver->s.nVersionMinor = 1;
        ver->s.nRevision = 2;
        ver->s.nStep = 0;
      };
      fill_header(&new_def, sizeof(new_def));
      new_def.nPortIndex = 1;
      if (OMX_GetParameter(m_decoder, OMX_IndexParamPortDefinition, &new_def) ==
          OMX_ErrorNone) {
        debug_log("cedar", "Decoder resolution updated to ",
                  new_def.format.video.nFrameWidth, "x",
                  new_def.format.video.nFrameHeight);
        m_stream_width = new_def.format.video.nFrameWidth;
        m_stream_height = new_def.format.video.nFrameHeight;
        ensure_display_buffers(m_stream_width, m_stream_height);
      }
      m_port_settings_dirty = false;
    }

    if (!ensure_display_buffers(m_stream_width, m_stream_height)) {
      debug_log("cedar", "Unable to allocate display buffers yet.");
    } else if (output_buffer->nFilledLen > 0) {
      auto& drm_buf = m_drm_buffers[m_drm_buffer_index];
      uint8_t* dst = static_cast<uint8_t*>(drm_buf.map);
      copy_planar_to_nv12(dst,
                          m_display_stride,
                          m_display_stride,
                          output_buffer->pBuffer + output_buffer->nOffset,
                          m_stream_width,
                          m_stream_height);

      bool submit_via_set_fb = true;
      if (!m_modeset_initialized && m_output.video_request) {
        int ret = modeset_perform_modeset(m_drm_fd,
                                          &m_output,
                                          m_output.video_request,
                                          &m_output.video_plane,
                                          drm_buf.fb_id,
                                          static_cast<int>(m_stream_width),
                                          static_cast<int>(m_stream_height),
                                          0);
        if (ret == 0) {
          m_modeset_initialized = true;
          submit_via_set_fb = false;
        } else {
          std::cerr << "Initial modeset failed for Cedar pipeline" << std::endl;
        }
      }
      if (submit_via_set_fb) {
        extra_modeset_set_fb(m_drm_fd, &m_output, &m_output.video_plane, drm_buf.fb_id);
      }

      m_drm_buffer_index = (m_drm_buffer_index + 1) % m_drm_buffers.size();
      ++frames_decoded;
      if (frames_decoded % 30 == 0) {
        debug_log("cedar", "Decoded ", frames_decoded, " frames");
      }
    }

    output_buffer->nFilledLen = 0;
    output_buffer->nOffset = 0;
    OMX_ERRORTYPE fill_err = OMX_FillThisBuffer(m_decoder, output_buffer);
    if (fill_err != OMX_ErrorNone) {
      std::cerr << "OMX_FillThisBuffer requeue failed: 0x" << std::hex << fill_err
                << std::dec << std::endl;
    }
  }

  debug_log("cedar", "Decode loop exiting.");
}

#endif  // HAVE_AW_OMX

