#b!in/bash

# H264
#gst-launch-1.0 videotestsrc ! video/x-raw, format=I420,width=1280,height=720,framerate=30/1 ! x264enc bitrate=5000 speed-preset=ultrafast tune=zerolatency key-int-max=30 ! queue ! h264parse config-interval=-1 ! \
#fdsink fd=1 | ./build/fpvue  --screen-mode 1920x1080@60

# H265
gst-launch-1.0 videotestsrc ! video/x-raw, format=I420,width=1280,height=720,framerate=30/1 ! x265enc bitrate=5000 speed-preset=ultrafast tune=zerolatency key-int-max=30 ! queue ! h265parse config-interval=-1 ! \
fdsink fd=1 | ./build/fpvue  --screen-mode 1920x1080@60 --h265

# H264 udp
#gst-launch-1.0 udpsrc port=5600 caps='application/x-rtp, payload=(int)96, clock-rate=(int)90000, media=(string)video, encoding-name=(string)H264' ! rtph264depay ! h264parse config-interval=1 ! video/x-h264,stream-format=byte-stream,alignment=au !>
#fdsink fd=1 | ./build/fpvue  --screen-mode 1920x1080@60
