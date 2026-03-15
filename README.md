![OpenIPC logo][logo]

# Waybeam

**FPV streamer for SigmaStar SoCs**

[![Telegram](https://openipc.org/images/telegram_button.svg)][fpv-channel]

[logo]: https://openipc.org/assets/openipc-logo-black.svg
[fpv-channel]: https://openipc.org/our-channels

---

Waybeam is a lightweight, low-latency H.265 video streamer for
[OpenIPC](https://openipc.org) cameras built around **SigmaStar** SoCs.

It uses the SigmaStar MI (Media Interface) API directly and targets:

| Target | SoC family | Build flag |
|--------|-----------|------------|
| `maruko` | infinity6c (Maruko) | `SIGMASTAR_MARUKO` |
| `star6e` | infinity6e (Star6e / Pudding) | `SIGMASTAR_PUDDING` |

Video is encoded as **H.265 / HEVC** with AVBR rate control and streamed as
RTP-framed UDP packets on port **5600** by default — ready for consumption by
[wfb-ng](https://github.com/svpcom/wfb-ng),
[PixelPilot](https://github.com/OpenIPC/PixelPilot) or any GStreamer pipeline.

---

## Quick start

### 1 — Build

```bash
# For SigmaStar infinity6c (Maruko)
./build.sh maruko

# For SigmaStar infinity6e (Star6e / Pudding)
./build.sh star6e
```

The script automatically downloads the cross-compiler toolchain and the SDK
shared libraries from the [OpenIPC firmware](https://github.com/openipc/firmware)
releases.  Internet access is required for the first build only.

### 2 — Deploy

```bash
scp -O src/waybeam root@192.168.1.10:/tmp/
```

### 3 — Run on the camera

```sh
# Stop the default streamer (if running)
killall majestic 2>/dev/null

# Start Waybeam (all arguments are optional)
/tmp/waybeam [bitrate_kbps] [host] [port] [sensor_index] [slice_rows]
```

**Example — 8 Mbps stream to ground station at 192.168.0.1:**

```sh
/tmp/waybeam 8192 192.168.0.1 5600
```

---

## Arguments

| # | Argument | Default | Description |
|---|----------|---------|-------------|
| 1 | `bitrate_kbps` | `4096` | Target bitrate in kbps (256–51200) |
| 2 | `host` | `192.168.1.10` | Destination IP address |
| 3 | `port` | `5600` | Destination UDP port |
| 4 | `sensor_index` | `0` | Sensor resolution index |
| 5 | `slice_rows` | `0` | Rows per HEVC slice (`0` = full-frame mode) |

**Environment variables:**

| Variable | Description |
|----------|-------------|
| `SENSOR` | Sensor name, used to load `/etc/sensors/<name>.bin` ISP tuning file |

---

## Low-latency slice mode

Setting `slice_rows` to a non-zero value (e.g. `4`) enables HEVC slice-split
mode.  Each slice is sent independently before the full frame is complete,
reducing end-to-end latency at the cost of slightly lower compression
efficiency.  Typical FPV use case:

```sh
SENSOR=imx415 /tmp/waybeam 8192 192.168.0.1 5600 0 4
```

---

## Receiving the stream

**GStreamer (Linux / macOS):**

```bash
gst-launch-1.0 udpsrc port=5600 ! \
    application/x-rtp,encoding-name=H265 ! \
    rtph265depay ! h265parse ! avdec_h265 ! autovideosink sync=false
```

**PixelPilot (Android):** configure the app to listen on port 5600.

**wfb-ng:** pipe the UDP stream directly into the wfb-ng transmitter.

---

## Repository layout

```
Waybeam_venc/
├── src/
│   ├── waybeam.c      Main streamer — SNR→VIF→[ISP]→VPE/SCL→VENC→UDP
│   ├── waybeam.h      Types, constants, state structure
│   └── compat.h       MI API compatibility shims (Maruko / Star6e)
├── include/
│   ├── infinity6c/    SigmaStar SDK headers for Maruko
│   └── infinity6e/    SigmaStar SDK headers for Star6e / Pudding
├── Makefile           Cross-compilation rules
├── build.sh           Automated build helper (downloads toolchain + SDK)
└── LICENSE            MIT
```

---

## Building manually

If you already have a cross-compiler and the SDK libraries:

```bash
# Maruko (infinity6c)
make -C src -B \
    CC=/path/to/arm-linux-gcc \
    DRV=/path/to/sigmastar-osdrv-infinity6c/lib \
    maruko

# Star6e (infinity6e)
make -C src -B \
    CC=/path/to/arm-linux-gcc \
    DRV=/path/to/sigmastar-osdrv-infinity6e/lib \
    star6e
```

---

## Related projects

* [OpenIPC/research](https://github.com/OpenIPC/research) — research streamer samples
* [OpenIPC/divinus](https://github.com/OpenIPC/divinus) — multi-platform streamer
* [svpcom/wfb-ng](https://github.com/svpcom/wfb-ng) — WiFi broadcast for FPV
* [OpenIPC/PixelPilot](https://github.com/OpenIPC/PixelPilot) — Android ground station

---

## License

MIT — see [LICENSE](LICENSE).
