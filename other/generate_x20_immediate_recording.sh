#bin/bash

gst-launch-1.0 -v udpsrc port=5600 caps = "application/x-rtp, media=(string)video, encoding-name=(string)H264, payload=(int)96" ! rtph264depay ! filesink location=x20_immediate_recording.h264