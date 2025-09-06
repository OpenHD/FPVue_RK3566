#ifndef FPVUE_ALLWINNERV4L2DISPLAY_H
#define FPVUE_ALLWINNERV4L2DISPLAY_H

#include <string>

/**
 * Experimental display path for Allwinner A733 devices.
 * This class is intended to receive RTP frames, decode them
 * using the V4L2 hardware decoder and display the result on a
 * KMS plane. Implementation is currently a placeholder and
 * performs no decoding yet.
 */
class AllwinnerV4L2Display {
public:
  AllwinnerV4L2Display(int udp_port, bool h265);
  ~AllwinnerV4L2Display();

  void start();
  void stop();

private:
  int m_port;
  bool m_h265;
};

#endif // FPVUE_ALLWINNERV4L2DISPLAY_H
