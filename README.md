# FPVue_rk

WFB-ng client (Video Decoder) for Rockchip platform powered by the [Rockchip MPP library](https://github.com/rockchip-linux/mpp).
It also includes an experimental Allwinner A733 path that targets a direct
V4L2 to KMS pipeline without GStreamer.

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

A placeholder V4L2-based path can be enabled on Allwinner A733 devices:

```
fpvue --aw-display --gst-udp-port 5600
```

The implementation currently only logs that it was invoked; decoding and
display via V4L2/KMS are still TODO.

### Known issues

1. Video is cropped when the fpv feed resolution is bigger than the screen mode.
1. Crashes when video feed resolution is higher than the screen resolution.
