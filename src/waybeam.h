/*
 * Part of the OpenIPC project — https://openipc.org
 * Targets: SigmaStar SoCs (infinity6c/Maruko, infinity6e/Star6e)
 * Contact: tech@openipc.eu
 * License: MIT
 */

#pragma once

#include <errno.h>
#include <endian.h>
#include <fcntl.h>
#include <poll.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <linux/limits.h>
#include <sys/syscall.h>

#include <mi_isp.h>
#include <mi_sensor.h>
#include <mi_sys.h>
#include <mi_venc.h>
#include <mi_vif.h>
#include "compat.h"

/* Default streaming parameters */
#define WB_SENSOR_PAD    0
#define WB_PIXEL_FORMAT  E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420
#define WB_MTU_SIZE      1400
#define WB_DEFAULT_BRATE 4096   /* kbps */
#define WB_DEFAULT_PORT  5600
#define WB_DEFAULT_HOST  "192.168.1.10"

/* RTP header (packed, no padding) */
#pragma pack(push, 1)
typedef struct {
	uint8_t  version;
	uint8_t  payload_type;
	uint16_t sequence;
	uint32_t timestamp;
	uint32_t ssrc_id;
} rtp_header_t;
#pragma pack(pop)

/* Runtime state shared across the program */
typedef struct {
	/* Network */
	struct sockaddr_in address;
	int    socket_fd;
	int    mtu_size;
	uint16_t sequence;

	/* SigmaStar SDK pixel format for the current sensor */
	MI_SYS_PixelFormat_e format;

	/* Sensor / stream parameters */
	int  sensor_index;
	int  width;
	int  height;
	int  fps;
	int  bitrate;   /* kbps */
	int  slice;     /* rows per slice, 0 = full frame */
	bool running;
} wb_state_t;
