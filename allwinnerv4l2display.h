#ifndef FPVUE_ALLWINNERV4L2DISPLAY_H
#define FPVUE_ALLWINNERV4L2DISPLAY_H

#include <atomic>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "rtp.h"
#include "drm.h"
}

/**
 * Experimental display path for Allwinner A733 devices.
 * This class receives RTP frames on a UDP port, decodes them using
 * the V4L2 stateless decoder and presents the frames on a KMS plane
 * driven by the sunxi-drm driver.
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
  bool setup_v4l2();
  void decode_loop();
  int open_candidate_v4l2_device(const char* path);

  int m_port;
  bool m_h265;

  std::atomic<bool> m_running{false};
  std::thread m_thread;

  int m_sock{-1};
  int m_input_fd{-1};
  int m_v4l2_fd{-1};
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

  std::string m_v4l2_device_path;
  std::string m_v4l2_driver_name;

  std::vector<void*> m_output_buffers;
  std::vector<void*> m_capture_buffers;
  std::vector<int> m_capture_fbs;
};

#endif // FPVUE_ALLWINNERV4L2DISPLAY_H
