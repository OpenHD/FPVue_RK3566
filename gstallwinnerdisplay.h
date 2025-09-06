#ifndef FPVUE_GSTALLWINNERDISPLAY_H
#define FPVUE_GSTALLWINNERDISPLAY_H

#include <gst/gst.h>
#include <string>

/**
 * Simple GStreamer based RTP receiver that decodes and displays video
 * using Allwinner hardware decoders. The decoder element and video sink
 * are configurable so different display modes can be tested.
 */
class GstAllwinnerDisplay {
public:
  GstAllwinnerDisplay(int udp_port, bool h265, const std::string &decoder,
                      const std::string &sink);
  ~GstAllwinnerDisplay();

  void start();
  void stop();

private:
  std::string construct_pipeline() const;

  int m_port;
  bool m_h265;
  std::string m_decoder;
  std::string m_sink;
  GstElement *m_pipeline{nullptr};
};

#endif // FPVUE_GSTALLWINNERDISPLAY_H
