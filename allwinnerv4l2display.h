#ifndef FPVUE_ALLWINNERV4L2DISPLAY_H
#define FPVUE_ALLWINNERV4L2DISPLAY_H

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "rtp.h"
#include "drm.h"
}

#ifndef HAVE_AW_OMX
#define HAVE_AW_OMX 0
#endif

#if HAVE_AW_OMX
extern "C" {
#include <OMX_Component.h>
#include <OMX_Core.h>
}
#endif

/**
 * Experimental display path for Allwinner A733 devices.
 * This class receives RTP frames on a UDP port, decodes them using
 * the Allwinner Cedar OpenMAX IL decoder and presents the frames on a KMS
 * plane driven by the sunxi-drm driver.
 */
class AllwinnerV4L2Display {
public:
  enum class InputMode {
    UDP,
    StdIn
  };

  AllwinnerV4L2Display(int udp_port,
                       bool h265,
                       uint32_t mode_width = 1280,
                       uint32_t mode_height = 720,
                       uint32_t mode_vrefresh = 60);
  void set_external_drm_fd(int fd, bool take_ownership);
  void override_input_mode(InputMode mode);
  ~AllwinnerV4L2Display();

  // Start the decoding/ display thread. Returns true on success.
  bool start();
  void stop();

private:
  bool setup_network();
  bool setup_drm();
  bool setup_omx();
  void decode_loop();
  void teardown_omx();
  void teardown_drm_resources();

#if HAVE_AW_OMX
  bool wait_for_state(OMX_STATETYPE state, int timeout_ms = 2000);
  bool ensure_display_buffers(uint32_t width, uint32_t height);
  void recycle_display_buffers();
  static OMX_ERRORTYPE omx_event_handler(OMX_HANDLETYPE, OMX_PTR, OMX_EVENTTYPE,
                                         OMX_U32, OMX_U32, OMX_PTR);
  static OMX_ERRORTYPE omx_empty_buffer_done(OMX_HANDLETYPE, OMX_PTR,
                                             OMX_BUFFERHEADERTYPE*);
  static OMX_ERRORTYPE omx_fill_buffer_done(OMX_HANDLETYPE, OMX_PTR,
                                            OMX_BUFFERHEADERTYPE*);
#endif

  int m_port;
  bool m_h265;

  std::atomic<bool> m_running{false};
  std::thread m_thread;

  int m_sock{-1};
  int m_input_fd{-1};
  int m_drm_fd{-1};
  bool m_take_ownership_of_drm_fd{true};
  InputMode m_input_mode{InputMode::UDP};
  bool m_drm_prepared{false};

  uint32_t m_stream_width{1280};
  uint32_t m_stream_height{720};
  uint32_t m_stream_vrefresh{60};
  uint32_t m_display_width{0};
  uint32_t m_display_height{0};
  uint32_t m_display_vrefresh{0};
  bool m_modeset_initialized{false};

  struct modeset_output m_output{};

#if HAVE_AW_OMX
  OMX_HANDLETYPE m_decoder{nullptr};
  OMX_CALLBACKTYPE m_callbacks{};
  OMX_STATETYPE m_decoder_state{OMX_StateLoaded};
  std::vector<OMX_BUFFERHEADERTYPE*> m_input_buffers;
  std::vector<OMX_BUFFERHEADERTYPE*> m_output_buffers;
  std::queue<OMX_BUFFERHEADERTYPE*> m_available_inputs;
  std::queue<OMX_BUFFERHEADERTYPE*> m_filled_outputs;
  std::mutex m_input_mutex;
  std::condition_variable m_input_cv;
  std::mutex m_output_mutex;
  std::condition_variable m_output_cv;
  std::mutex m_state_mutex;
  std::condition_variable m_state_cv;
  bool m_port_settings_dirty{false};

  struct DrmBuffer {
    uint32_t fb_id{0};
    uint32_t handle{0};
    void* map{nullptr};
    size_t size{0};
    uint32_t pitches[2]{};
    uint32_t offsets[2]{};
  };
  std::vector<DrmBuffer> m_drm_buffers;
  uint32_t m_display_stride{0};
  uint32_t m_allocated_width{0};
  uint32_t m_allocated_height{0};
  uint32_t m_drm_buffer_index{0};
#endif
};

#endif // FPVUE_ALLWINNERV4L2DISPLAY_H
