# FPVue_rk

WFB-ng client (Video Decoder) for Rockchip platform powered by the [Rockchip MPP library](https://github.com/rockchip-linux/mpp).
It also includes an experimental GStreamer path for Allwinner A733 devices using
`gstomxvideodec` or the V4L2 stateless decoders.

Tested on RK3566 (Radxa Zero 3W) and RK3588s (Orange Pi 5).

# Compilation

Build on the Rockchip linux system directly.

## Install dependencies

- rockchip_mpp

```
git clone https://github.com/rockchip-linux/mpp.git
sudo cmake --build build --target install
```

- drm

```
sudo apt install libdrm-dev
```

## Build Instructions

Build and run application in production environment:

```
cmake -B build
sudo cmake --build build --target install
build/fpvue
```

Build and run application for debugging purposes:

```
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
build/fpvue
```

### Usage

Show command line options:
```
fpvue --help
```

### Allwinner A733 experimental mode

To use the GStreamer based decoder on an Allwinner A733 device:

```
fpvue --aw-display --gst-udp-port 5600 \
      --aw-decoder gstomxvideodec --aw-sink autovideosink
```

The `--aw-decoder` and `--aw-sink` options allow testing different hardware
decoder elements and video sinks (for example `v4l2slh264dec` or `kmssink`).

### Known issues

1. Video is cropped when the fpv feed resolution is bigger than the screen mode.
1. Crashes when video feed resolution is higher than the screen resolution.
