#include "allwinnerv4l2display.h"

#include <iostream>
#include <unistd.h>

AllwinnerV4L2Display::AllwinnerV4L2Display(int udp_port, bool h265)
    : m_port(udp_port), m_h265(h265) {}

AllwinnerV4L2Display::~AllwinnerV4L2Display() { stop(); }

void AllwinnerV4L2Display::start() {
  std::cout << "Allwinner V4L2 display placeholder -- expected to decode on port "
            << m_port << (m_h265 ? " using H265" : " using H264")
            << " and present frames on a KMS plane." << std::endl;
}

void AllwinnerV4L2Display::stop() {
  // no-op for placeholder
}
