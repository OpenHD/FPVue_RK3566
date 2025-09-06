#include "gstallwinnerdisplay.h"
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
static void initGstreamerOrThrow() {
  GError *error = nullptr;
  if (!gst_init_check(nullptr, nullptr, &error)) {
    g_error_free(error);
    throw std::runtime_error("GStreamer initialization failed");
  }
}

static std::string gst_create_rtp_caps(bool h265) {
  std::stringstream ss;
  if (h265) {
    ss << "caps=\"application/x-rtp, media=(string)video, "
          "encoding-name=(string)H265\"";
  } else {
    ss << "caps=\"application/x-rtp, media=(string)video, "
          "encoding-name=(string)H264, payload=(int)96\"";
  }
  return ss.str();
}

static std::string create_rtp_depay(bool h265) {
  return h265 ? "rtph265depay ! " : "rtph264depay ! ";
}

static std::string create_parse(bool h265) {
  return h265 ? "h265parse config-interval=-1 ! "
              : "h264parse config-interval=-1 ! ";
}
} // namespace

GstAllwinnerDisplay::GstAllwinnerDisplay(int udp_port, bool h265,
                                         const std::string &decoder,
                                         const std::string &sink)
    : m_port(udp_port), m_h265(h265), m_decoder(decoder), m_sink(sink) {
  initGstreamerOrThrow();
}

GstAllwinnerDisplay::~GstAllwinnerDisplay() { stop(); }

std::string GstAllwinnerDisplay::construct_pipeline() const {
  std::stringstream ss;
  ss << "udpsrc port=" << m_port << " " << gst_create_rtp_caps(m_h265) << " ! ";
  ss << create_rtp_depay(m_h265);
  ss << create_parse(m_h265);
  ss << m_decoder << " ! " << m_sink;
  return ss.str();
}

void GstAllwinnerDisplay::start() {
  std::string pipe = construct_pipeline();
  std::cout << "GSTREAMER PIPE=[" << pipe << "]" << std::endl;
  GError *error = nullptr;
  m_pipeline = gst_parse_launch(pipe.c_str(), &error);
  if (error) {
    std::cerr << "gst_parse_launch error: " << error->message << std::endl;
    g_error_free(error);
    m_pipeline = nullptr;
    return;
  }
  gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
}

void GstAllwinnerDisplay::stop() {
  if (m_pipeline) {
    gst_element_send_event(m_pipeline, gst_event_new_eos());
    gst_element_set_state(m_pipeline, GST_STATE_NULL);
    gst_object_unref(m_pipeline);
    m_pipeline = nullptr;
  }
}
