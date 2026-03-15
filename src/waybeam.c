/*
 * Waybeam — FPV streamer for SigmaStar SoCs
 * Part of the OpenIPC project — https://openipc.org
 *
 * Targets: SigmaStar infinity6c (Maruko) and infinity6e (Star6e/Pudding)
 * Contact: tech@openipc.eu
 * License: MIT
 *
 * Usage:
 *   waybeam [bitrate_kbps] [host] [port] [sensor_index] [slice_rows]
 *
 * Defaults:
 *   bitrate      = 4096 kbps
 *   host         = 192.168.1.10
 *   port         = 5600
 *   sensor_index = 0
 *   slice_rows   = 0  (full-frame mode; set > 0 for low-latency slice mode)
 *
 * The stream is H.265 (HEVC) over UDP with RTP-style framing.
 * Each NAL unit that exceeds the MTU is split using RFC 7798 FU packets.
 */

#include "waybeam.h"
#include <stdbool.h>
#include <signal.h>
#include <alloca.h>

/* --------------------------------------------------------------------------
 * Module-level state
 * -------------------------------------------------------------------------- */
static wb_state_t wb;

/* --------------------------------------------------------------------------
 * Sensor (SNR) initialisation
 * -------------------------------------------------------------------------- */
static int mi_snr_init(void) {
	if (MI_SNR_SetPlaneMode(WB_SENSOR_PAD, false)) {
		fprintf(stderr, "waybeam: MI_SNR_SetPlaneMode failed\n");
		return 1;
	}

	MI_SNR_Res_t res = {0};
	if (MI_SNR_GetRes(WB_SENSOR_PAD, wb.sensor_index, &res)) {
		fprintf(stderr, "waybeam: MI_SNR_GetRes failed\n");
		return 1;
	}

	wb.fps = res.u32MaxFps;

	if (MI_SNR_SetRes(WB_SENSOR_PAD, wb.sensor_index)) {
		fprintf(stderr, "waybeam: MI_SNR_SetRes failed\n");
		return 1;
	}

	if (MI_SNR_Enable(WB_SENSOR_PAD)) {
		fprintf(stderr, "waybeam: MI_SNR_Enable failed\n");
		return 1;
	}

	return 0;
}

static int mi_snr_deinit(void) {
	return MI_SNR_Disable(WB_SENSOR_PAD);
}

/* --------------------------------------------------------------------------
 * VIF (Video Input Front-end) initialisation
 * -------------------------------------------------------------------------- */
#ifdef SIGMASTAR_MARUKO
static int mi_vif_init(void) {
	MI_VIF_GroupAttr_t attr = {0};
	attr.eHDRType            = E_MI_VIF_HDR_TYPE_OFF;
	attr.eClkEdge            = E_MI_VIF_CLK_EDGE_DOUBLE;
	attr.eIntfMode           = E_MI_VIF_MODE_MIPI;
	attr.eWorkMode           = E_MI_VIF_WORK_MODE_1MULTIPLEX;
	attr.u32GroupStitchMask  = E_MI_VIF_GROUPMASK_ID1;

	if (MI_VIF_CreateDevGroup(0, &attr)) {
		fprintf(stderr, "waybeam: MI_VIF_CreateDevGroup failed\n");
		return 1;
	}

	MI_SNR_PlaneInfo_t plane = {0};
	if (MI_SNR_GetPlaneInfo(WB_SENSOR_PAD, 0, &plane)) {
		fprintf(stderr, "waybeam: MI_SNR_GetPlaneInfo failed\n");
		return 1;
	}

	wb.format = (MI_SYS_PixelFormat_e)RGB_BAYER_PIXEL(
		plane.ePixPrecision, plane.eBayerId);

	MI_VIF_DevAttr_t dev = {0};
	dev.stInputRect.u16X      = plane.stCapRect.u16X;
	dev.stInputRect.u16Y      = plane.stCapRect.u16Y;
	dev.stInputRect.u16Width  = plane.stCapRect.u16Width;
	dev.stInputRect.u16Height = plane.stCapRect.u16Height;
	dev.eInputPixel           = wb.format;

	if (MI_VIF_SetDevAttr(0, &dev)) {
		fprintf(stderr, "waybeam: MI_VIF_SetDevAttr failed\n");
		return 1;
	}

	if (MI_VIF_EnableDev(0)) {
		fprintf(stderr, "waybeam: MI_VIF_EnableDev failed\n");
		return 1;
	}

	MI_VIF_OutputPortAttr_t port = {0};
	port.stCapRect.u16Width  = plane.stCapRect.u16Width;
	port.stCapRect.u16Height = plane.stCapRect.u16Height;
	port.stDestSize.u16Width  = plane.stCapRect.u16Width;
	port.stDestSize.u16Height = plane.stCapRect.u16Height;
	port.ePixFormat           = wb.format;

	wb.width  = plane.stCapRect.u16Width;
	wb.height = plane.stCapRect.u16Height;

	if (MI_VIF_SetOutputPortAttr(0, 0, &port)) {
		fprintf(stderr, "waybeam: MI_VIF_SetOutputPortAttr failed\n");
		return 1;
	}

	if (MI_VIF_EnableOutputPort(0, 0)) {
		fprintf(stderr, "waybeam: MI_VIF_EnableOutputPort failed\n");
		return 1;
	}

	return 0;
}

#else /* infinity6e / PUDDING */
static int mi_vif_init(void) {
	MI_SNR_PADInfo_t info = {0};
	if (MI_SNR_GetPadInfo(WB_SENSOR_PAD, &info)) {
		fprintf(stderr, "waybeam: MI_SNR_GetPadInfo failed\n");
		return 1;
	}

	MI_VIF_DevAttr_t dev = {0};
	dev.eIntfMode = info.eIntfMode;
	dev.eHDRType  = E_MI_VIF_HDR_TYPE_OFF;
	dev.eClkEdge  = E_MI_VIF_CLK_EDGE_DOUBLE;
	dev.eBitOrder = E_MI_VIF_BITORDER_NORMAL;
	dev.eWorkMode = E_MI_VIF_WORK_MODE_RGB_REALTIME;

	if (dev.eIntfMode == E_MI_VIF_MODE_MIPI)
		dev.eDataSeq = info.unIntfAttr.stMipiAttr.eDataYUVOrder;
	else
		dev.eDataSeq = E_MI_VIF_INPUT_DATA_YUYV;

	if (MI_VIF_SetDevAttr(0, &dev)) {
		fprintf(stderr, "waybeam: MI_VIF_SetDevAttr failed\n");
		return 1;
	}

	if (MI_VIF_EnableDev(0)) {
		fprintf(stderr, "waybeam: MI_VIF_EnableDev failed\n");
		return 1;
	}

	MI_SNR_PlaneInfo_t plane = {0};
	if (MI_SNR_GetPlaneInfo(WB_SENSOR_PAD, 0, &plane)) {
		fprintf(stderr, "waybeam: MI_SNR_GetPlaneInfo failed\n");
		return 1;
	}

	wb.format = (MI_SYS_PixelFormat_e)RGB_BAYER_PIXEL(
		plane.ePixPrecision, plane.eBayerId);

	MI_VIF_ChnPortAttr_t port = {0};
	port.stCapRect.u16Width   = plane.stCapRect.u16Width;
	port.stCapRect.u16Height  = plane.stCapRect.u16Height;
	port.stDestSize.u16Width  = plane.stCapRect.u16Width;
	port.stDestSize.u16Height = plane.stCapRect.u16Height;
	port.ePixFormat           = wb.format;

	wb.width  = plane.stCapRect.u16Width;
	wb.height = plane.stCapRect.u16Height;

	if (MI_VIF_SetChnPortAttr(0, 0, &port)) {
		fprintf(stderr, "waybeam: MI_VIF_SetChnPortAttr failed\n");
		return 1;
	}

	if (MI_VIF_EnableChnPort(0, 0)) {
		fprintf(stderr, "waybeam: MI_VIF_EnableChnPort failed\n");
		return 1;
	}

	return 0;
}
#endif /* SIGMASTAR_MARUKO */

static int mi_vif_deinit(void) {
	if (MI_VIF_DisableChnPort(0, 0))
		return 1;

	if (MI_VIF_DisableDev(0))
		return 1;

#ifdef SIGMASTAR_MARUKO
	if (MI_VIF_DestroyDevGroup(0))
		return 1;
#endif

	return 0;
}

/* --------------------------------------------------------------------------
 * ISP initialisation (infinity6c only — infinity6e uses VPE)
 * -------------------------------------------------------------------------- */
#ifdef SIGMASTAR_MARUKO
static int mi_isp_init(void) {
	MI_ISP_DevAttr_t dev = {0};
	dev.u32DevStitchMask = E_MI_ISP_DEVICEMASK_ID0;

	if (MI_ISP_CreateDevice(0, &dev)) {
		fprintf(stderr, "waybeam: MI_ISP_CreateDevice failed\n");
		return 1;
	}

	MI_ISP_ChannelAttr_t attr = {0};
	attr.u32SensorBindId = E_MI_ISP_SENSOR0;

	if (MI_ISP_CreateChannel(0, 0, &attr)) {
		fprintf(stderr, "waybeam: MI_ISP_CreateChannel failed\n");
		return 1;
	}

	MI_ISP_ChnParam_t param = {0};
	param.eHDRType = E_MI_ISP_HDR_TYPE_OFF;
	param.eRot     = E_MI_SYS_ROTATE_NONE;

	if (MI_ISP_SetChnParam(0, 0, &param)) {
		fprintf(stderr, "waybeam: MI_ISP_SetChnParam failed\n");
		return 1;
	}

	if (MI_ISP_StartChannel(0, 0)) {
		fprintf(stderr, "waybeam: MI_ISP_StartChannel failed\n");
		return 1;
	}

	MI_SYS_ChnPort_t input  = {0};
	input.eModId             = E_MI_MODULE_ID_VIF;

	MI_SYS_ChnPort_t output = {0};
	output.eModId            = E_MI_MODULE_ID_ISP;

	if (MI_SYS_BindChnPort2(0, &input, &output, wb.fps,
			wb.fps, E_MI_SYS_BIND_TYPE_REALTIME, 0)) {
		fprintf(stderr, "waybeam: MI_SYS_BindChnPort2 (VIF→ISP) failed\n");
		return 1;
	}

	MI_ISP_OutPortParam_t port = {0};
	port.ePixelFormat = WB_PIXEL_FORMAT;

	if (MI_ISP_SetOutputPortParam(0, 0, 0, &port)) {
		fprintf(stderr, "waybeam: MI_ISP_SetOutputPortParam failed\n");
		return 1;
	}

	if (MI_ISP_EnableOutputPort(0, 0, 0)) {
		fprintf(stderr, "waybeam: MI_ISP_EnableOutputPort failed\n");
		return 1;
	}

	return 0;
}

static int mi_isp_deinit(void) {
	MI_SYS_ChnPort_t input  = {0};
	input.eModId             = E_MI_MODULE_ID_VIF;

	MI_SYS_ChnPort_t output = {0};
	output.eModId            = E_MI_MODULE_ID_ISP;

	if (MI_SYS_UnBindChnPort(0, &input, &output))
		return 1;

	if (MI_ISP_DisableOutputPort(0, 0, 0))
		return 1;

	if (MI_ISP_StopChannel(0, 0))
		return 1;

	if (MI_ISP_DestroyChannel(0, 0))
		return 1;

	if (MI_ISP_DestoryDevice(0))
		return 1;

	return 0;
}
#endif /* SIGMASTAR_MARUKO */

/* --------------------------------------------------------------------------
 * VPE / SCL (video pre-processing / scaler) initialisation
 * -------------------------------------------------------------------------- */
#ifdef SIGMASTAR_MARUKO
/* infinity6c: ISP → SCL → VENC */
static int mi_vpe_init(void) {
	MI_SYS_GlobalPrivPoolConfig_t pool = {0};
	pool.bCreate        = true;
	pool.eConfigType    = E_MI_SYS_PER_DEV_PRIVATE_RING_POOL;
	pool.uConfig.stpreDevPrivRingPoolConfig.eModule    = E_MI_MODULE_ID_SCL;
	pool.uConfig.stpreDevPrivRingPoolConfig.u16MaxWidth  = wb.width;
	pool.uConfig.stpreDevPrivRingPoolConfig.u16MaxHeight = wb.height;
	pool.uConfig.stpreDevPrivRingPoolConfig.u16RingLine  = wb.height / 4;

	if (MI_SYS_ConfigPrivateMMAPool(0, &pool)) {
		fprintf(stderr, "waybeam: MI_SYS_ConfigPrivateMMAPool (SCL) failed\n");
		return 1;
	}

	MI_SCL_DevAttr_t dev = {0};
	dev.u32NeedUseHWOutPortMask = E_MI_SCL_HWSCL0 | E_MI_SCL_HWSCL1;

	if (MI_SCL_CreateDevice(0, &dev)) {
		fprintf(stderr, "waybeam: MI_SCL_CreateDevice failed\n");
		return 1;
	}

	MI_SCL_ChannelAttr_t attr = {0};
	if (MI_SCL_CreateChannel(0, 0, &attr)) {
		fprintf(stderr, "waybeam: MI_SCL_CreateChannel failed\n");
		return 1;
	}

	MI_SCL_ChnParam_t param = {0};
	param.eRot = E_MI_SYS_ROTATE_NONE;

	if (MI_SCL_SetChnParam(0, 0, &param)) {
		fprintf(stderr, "waybeam: MI_SCL_SetChnParam failed\n");
		return 1;
	}

	if (MI_SCL_StartChannel(0, 0)) {
		fprintf(stderr, "waybeam: MI_SCL_StartChannel failed\n");
		return 1;
	}

	MI_SYS_ChnPort_t input  = {0};
	input.eModId             = E_MI_MODULE_ID_ISP;

	MI_SYS_ChnPort_t output = {0};
	output.eModId            = E_MI_MODULE_ID_SCL;

	if (MI_SYS_BindChnPort2(MDEV &input, &output, wb.fps,
			wb.fps, E_MI_SYS_BIND_TYPE_REALTIME, 0)) {
		fprintf(stderr, "waybeam: MI_SYS_BindChnPort2 (ISP→SCL) failed\n");
		return 1;
	}

	MI_SCL_OutPortParam_t port = {0};
	port.stSCLOutputSize.u16Width  = wb.width;
	port.stSCLOutputSize.u16Height = wb.height;
	port.ePixelFormat              = WB_PIXEL_FORMAT;
	port.eCompressMode             = E_MI_SYS_COMPRESS_MODE_IFC;

	if (MI_SCL_SetOutputPortParam(0, 0, 0, &port)) {
		fprintf(stderr, "waybeam: MI_SCL_SetOutputPortParam failed\n");
		return 1;
	}

	if (MI_SCL_EnableOutputPort(0, 0, 0)) {
		fprintf(stderr, "waybeam: MI_SCL_EnableOutputPort failed\n");
		return 1;
	}

	return 0;
}

static int mi_vpe_deinit(void) {
	MI_SYS_ChnPort_t input  = {0};
	input.eModId             = E_MI_MODULE_ID_ISP;

	MI_SYS_ChnPort_t output = {0};
	output.eModId            = E_MI_MODULE_ID_SCL;

	if (MI_SYS_UnBindChnPort(MDEV &input, &output))
		return 1;

	if (MI_SCL_DisableOutputPort(0, 0, 0))
		return 1;

	if (MI_SCL_StopChannel(0, 0))
		return 1;

	if (MI_SCL_DestroyChannel(0, 0))
		return 1;

	if (MI_SCL_DestroyDevice(0))
		return 1;

	return 0;
}

#else /* infinity6e: VIF → VPE → VENC */
static int mi_vpe_init(void) {
	MI_VPE_ChannelAttr_t attr = {0};
	attr.eSensorBindId = E_MI_VPE_SENSOR0;
	attr.eRunningMode  = E_MI_VPE_RUN_REALTIME_MODE;
	attr.eHDRType      = E_MI_VPE_HDR_TYPE_OFF;
	attr.ePixFmt       = wb.format;
	attr.u16MaxW       = wb.width;
	attr.u16MaxH       = wb.height;

	if (MI_VPE_CreateChannel(0, &attr)) {
		fprintf(stderr, "waybeam: MI_VPE_CreateChannel failed\n");
		return 1;
	}

	if (MI_VPE_StartChannel(0)) {
		fprintf(stderr, "waybeam: MI_VPE_StartChannel failed\n");
		return 1;
	}

	MI_SYS_ChnPort_t input  = {0};
	input.eModId             = E_MI_MODULE_ID_VIF;

	MI_SYS_ChnPort_t output = {0};
	output.eModId            = E_MI_MODULE_ID_VPE;

	if (MI_SYS_BindChnPort2(MDEV &input, &output, wb.fps,
			wb.fps, E_MI_SYS_BIND_TYPE_REALTIME, 0)) {
		fprintf(stderr, "waybeam: MI_SYS_BindChnPort2 (VIF→VPE) failed\n");
		return 1;
	}

	MI_VPE_PortMode_t port = {0};
	port.ePixelFormat = WB_PIXEL_FORMAT;
	port.u16Width     = wb.width;
	port.u16Height    = wb.height;

	if (MI_VPE_SetPortMode(0, 0, &port)) {
		fprintf(stderr, "waybeam: MI_VPE_SetPortMode failed\n");
		return 1;
	}

	if (MI_VPE_EnablePort(0, 0)) {
		fprintf(stderr, "waybeam: MI_VPE_EnablePort failed\n");
		return 1;
	}

	return 0;
}

static int mi_vpe_deinit(void) {
	if (MI_VPE_DisablePort(0, 0))
		return 1;

	MI_SYS_ChnPort_t input  = {0};
	input.eModId             = E_MI_MODULE_ID_VIF;

	MI_SYS_ChnPort_t output = {0};
	output.eModId            = E_MI_MODULE_ID_VPE;

	if (MI_SYS_UnBindChnPort(MDEV &input, &output))
		return 1;

	if (MI_VPE_StopChannel(0))
		return 1;

	if (MI_VPE_DestroyChannel(0))
		return 1;

	return 0;
}
#endif /* SIGMASTAR_MARUKO */

/* --------------------------------------------------------------------------
 * VENC (H.265 video encoder) initialisation
 * -------------------------------------------------------------------------- */
static int mi_venc_config(void) {
	char file[PATH_MAX];
	const char *sensor = getenv("SENSOR");

	if (sensor) {
		snprintf(file, sizeof(file), "/etc/sensors/%s.bin", sensor);
		if (access(file, F_OK) == 0) {
			printf("waybeam: loading ISP API file: %s\n", file);
			if (MI_ISP_API_CmdLoadBinFile(MDEV 0, file, 1234)) {
				fprintf(stderr, "waybeam: MI_ISP_API_CmdLoadBinFile failed\n");
				return 1;
			}
		}
	}

	MI_ISP_AE_EXPO_LIMIT_TYPE_t exposure;
	if (MI_ISP_AE_GetExposureLimit(MDEV 0, &exposure))
		return 1;

	/* Cap shutter to one frame period to prevent motion blur in FPV. */
	exposure.u32MaxShutterUS = (1000 * 1000) / wb.fps;
	if (MI_ISP_AE_SetExposureLimit(MDEV 0, &exposure))
		return 1;

	return 0;
}

static int mi_venc_init(void) {
	MI_VENC_ChnAttr_t attr = {0};
	attr.stVeAttr.eType                           = E_MI_VENC_MODTYPE_H265E;
	attr.stVeAttr.stAttrH265e.bByFrame            = !wb.slice;
	attr.stVeAttr.stAttrH265e.u32PicWidth         = wb.width;
	attr.stVeAttr.stAttrH265e.u32PicHeight        = wb.height;
	attr.stVeAttr.stAttrH265e.u32MaxPicWidth      = wb.width;
	attr.stVeAttr.stAttrH265e.u32MaxPicHeight     = wb.height;

	attr.stRcAttr.eRcMode                                   = E_MI_VENC_RC_MODE_H265AVBR;
	attr.stRcAttr.stAttrH265Avbr.u32Gop                     = wb.fps;
	attr.stRcAttr.stAttrH265Avbr.u32MaxBitRate              = wb.bitrate * 1024;
	attr.stRcAttr.stAttrH265Avbr.u32SrcFrmRateNum           = wb.fps;
	attr.stRcAttr.stAttrH265Avbr.u32SrcFrmRateDen           = 1;
	attr.stRcAttr.stAttrH265Avbr.u32MaxQp                   = 42;
	attr.stRcAttr.stAttrH265Avbr.u32MinQp                   = 12;

	if (MI_VENC_CreateChn(MDEV 0, &attr)) {
		fprintf(stderr, "waybeam: MI_VENC_CreateChn failed\n");
		return 1;
	}

	MI_VENC_RcParam_t param;
	if (MI_VENC_GetRcParam(MDEV 0, &param)) {
		fprintf(stderr, "waybeam: MI_VENC_GetRcParam failed\n");
		return 1;
	}

	/* Prefer lower I-frame QP relative to P-frames for better FPV detail. */
	param.stParamH265Avbr.s32IPQPDelta = -4;
	if (MI_VENC_SetRcParam(MDEV 0, &param)) {
		fprintf(stderr, "waybeam: MI_VENC_SetRcParam failed\n");
		return 1;
	}

#ifdef SIGMASTAR_MARUKO
	/* Ring-buffer DMA mode on infinity6c reduces latency significantly. */
	MI_SYS_GlobalPrivPoolConfig_t pool = {0};
	pool.bCreate        = true;
	pool.eConfigType    = E_MI_SYS_PER_DEV_PRIVATE_RING_POOL;
	pool.uConfig.stpreDevPrivRingPoolConfig.eModule    = E_MI_MODULE_ID_VENC;
	pool.uConfig.stpreDevPrivRingPoolConfig.u16MaxWidth  = wb.width;
	pool.uConfig.stpreDevPrivRingPoolConfig.u16MaxHeight = wb.height;
	pool.uConfig.stpreDevPrivRingPoolConfig.u16RingLine  = wb.height;

	if (MI_SYS_ConfigPrivateMMAPool(0, &pool)) {
		fprintf(stderr, "waybeam: MI_SYS_ConfigPrivateMMAPool (VENC) failed\n");
		return 1;
	}

	MI_VENC_InputSourceConfig_t config = {0};
	config.eInputSrcBufferMode = E_MI_VENC_INPUT_MODE_RING_UNIFIED_DMA;

	if (MI_VENC_SetInputSourceConfig(0, 0, &config)) {
		fprintf(stderr, "waybeam: MI_VENC_SetInputSourceConfig failed\n");
		return 1;
	}
#endif /* SIGMASTAR_MARUKO */

	if (wb.slice) {
		MI_VENC_ParamH265SliceSplit_t split;
		split.bSplitEnable    = true;
		split.u32SliceRowCount = wb.slice;

		if (MI_VENC_SetH265SliceSplit(MDEV 0, &split)) {
			fprintf(stderr, "waybeam: MI_VENC_SetH265SliceSplit failed\n");
			return 1;
		}
	}

	if (MI_VENC_StartRecvPic(MDEV 0)) {
		fprintf(stderr, "waybeam: MI_VENC_StartRecvPic failed\n");
		return 1;
	}

	MI_SYS_ChnPort_t input  = {0};
	input.eModId             = VENC_MODULE_PORT;

	MI_SYS_ChnPort_t output = {0};
	output.eModId            = E_MI_MODULE_ID_VENC;

	if (MI_SYS_BindChnPort2(MDEV &input, &output,
			wb.fps, wb.fps, VENC_MODULE_BIND, 0)) {
		fprintf(stderr, "waybeam: MI_SYS_BindChnPort2 (→VENC) failed\n");
		return 1;
	}

	return 0;
}

static int mi_venc_deinit(void) {
	MI_SYS_ChnPort_t input  = {0};
	input.eModId             = VENC_MODULE_PORT;

	MI_SYS_ChnPort_t output = {0};
	output.eModId            = E_MI_MODULE_ID_VENC;

	if (MI_SYS_UnBindChnPort(MDEV &input, &output))
		return 1;

	if (MI_VENC_StopRecvPic(MDEV 0))
		return 1;

	return MI_VENC_DestroyChn(MDEV 0);
}

/* --------------------------------------------------------------------------
 * RTP / FU-A framing and UDP send
 * -------------------------------------------------------------------------- */

/*
 * Send one RTP packet carrying `size` bytes of NAL payload.
 * The RTP header is prepended via scatter-gather to avoid a copy.
 */
static void send_rtp_packet(const uint8_t *data, uint32_t size) {
	rtp_header_t rtp;
	rtp.version      = 0x80;
	rtp.sequence     = htobe16(wb.sequence++);
	rtp.payload_type = 0x60;   /* 96 — dynamic payload type for H.265 */
	rtp.timestamp    = 0;
	rtp.ssrc_id      = 0xDEADBEEF;

	struct iovec iov[2] = {0};
	iov[0].iov_base = &rtp;
	iov[0].iov_len  = sizeof(rtp);
	iov[1].iov_base = (void *)data;
	iov[1].iov_len  = size;

	struct msghdr msg = {0};
	msg.msg_iovlen  = 2;
	msg.msg_iov     = iov;
	msg.msg_name    = &wb.address;
	msg.msg_namelen = sizeof(struct sockaddr_in);

	if (sendmsg(wb.socket_fd, &msg, 0) < 0) {
		fprintf(stderr, "waybeam: sendmsg failed: %s (packet %u bytes)\n",
			strerror(errno), size);
	}
}

/*
 * Fragment one NAL unit into MTU-sized RTP packets.
 *
 * The raw buffer starts with a 4-byte Annex-B start code (0x00 00 00 01)
 * which is stripped before sending.  When the NAL is larger than MTU the
 * RFC 7798 Fragmentation Unit (FU; HEVC type 49) is used.  For smaller
 * NALs the data is sent as a single-NAL-unit packet.
 *
 * AVC (H.264) FU-A is also handled for streams that may use it.
 */
static void fragment_nal(const uint8_t *data, int size) {
	/* Strip the 4-byte Annex-B prefix. */
	const int prefix = 4;
	data += prefix;
	size -= prefix;

	if (size <= 0)
		return;

	if (size <= wb.mtu_size) {
		/* Fits in a single packet — send as-is. */
		send_rtp_packet(data, size);
		return;
	}

	/* Determine NAL type for fragmentation header construction. */
	uint8_t nal_type_avc  = data[0] & 0x1F;
	uint8_t nal_type_hevc = (data[0] >> 1) & 0x3F;
	uint8_t nal_bits_avc  = data[0] & 0xE0;
	uint8_t nal_bits_hevc = data[0] & 0x81;

	/* tx_buffer holds the FU header (2–3 bytes) + chunk of NAL payload. */
	uint8_t tx_buffer[4096];
	int     hdr_len   = 2;   /* default for AVC FU-A */
	bool    start_bit = true;

	while (size > 0) {
		int chunk = (size > wb.mtu_size) ? wb.mtu_size : size;

		if (nal_type_avc == 1 || nal_type_avc == 5) {
			/* H.264 FU-A */
			tx_buffer[0] = nal_bits_avc | 28;   /* FU indicator */
			tx_buffer[1] = nal_type_avc;          /* FU header */
			hdr_len = 2;

			if (start_bit) {
				/* Skip the NAL header byte in the payload. */
				data++;
				size--;
				chunk = (size > wb.mtu_size) ? wb.mtu_size : size;
				tx_buffer[1] = 0x80 | nal_type_avc;   /* S-bit set */
				start_bit = false;
			}

			if (chunk == size)
				tx_buffer[1] |= 0x40;   /* E-bit — last fragment */

		} else if (nal_type_hevc == 1 || nal_type_hevc == 19) {
			/* H.265 FU (RFC 7798 §4.4.3) */
			tx_buffer[0] = nal_bits_hevc | (49 << 1);   /* PayloadHdr[0] */
			tx_buffer[1] = 1;                             /* PayloadHdr[1] — layer id=0, tid=1 */
			tx_buffer[2] = nal_type_hevc;                 /* FU header */
			hdr_len = 3;

			if (start_bit) {
				/* Skip the 2-byte HEVC NAL header. */
				data += 2;
				size -= 2;
				chunk = (size > wb.mtu_size) ? wb.mtu_size : size;
				tx_buffer[2] = 0x80 | nal_type_hevc;   /* S-bit set */
				start_bit = false;
			}

			if (chunk == size)
				tx_buffer[2] |= 0x40;   /* E-bit */
		} else {
			/* Unknown NAL type — send as single-NAL-unit packet and bail. */
			send_rtp_packet(data, size);
			return;
		}

		memcpy(tx_buffer + hdr_len, data, chunk);
		send_rtp_packet(tx_buffer, chunk + hdr_len);

		data += chunk;
		size -= chunk;
	}
}

/* --------------------------------------------------------------------------
 * Encoded stream reader
 * -------------------------------------------------------------------------- */
static int read_frame(void) {
	struct pollfd desc;
	desc.fd     = MI_VENC_GetFd(MDEV 0);
	desc.events = POLLIN;

	if (poll(&desc, 1, 1000) < 0) {
		fprintf(stderr, "waybeam: poll() failed: %s\n", strerror(errno));
		return 1;
	}

	MI_VENC_ChnStat_t stat = {0};
	if (MI_VENC_Query(MDEV 0, &stat)) {
		fprintf(stderr, "waybeam: MI_VENC_Query failed\n");
		return 1;
	}

	if (stat.u32CurPacks == 0)
		return 0;   /* no data ready yet */

	MI_VENC_Stream_t stream = {0};
	stream.u32PackCount = stat.u32CurPacks;
	stream.pstPack      = alloca(sizeof(MI_VENC_Pack_t) * stat.u32CurPacks);

	if (MI_VENC_GetStream(MDEV 0, &stream, 0)) {
		fprintf(stderr, "waybeam: MI_VENC_GetStream failed\n");
		return 1;
	}

	for (MI_U32 i = 0; i < stream.u32PackCount; i++) {
		MI_VENC_Pack_t *pkt = &stream.pstPack[i];

		if (pkt->u32DataNum) {
			/* Multiple data segments in one pack (slice mode). */
			for (MI_U32 j = 0; j < pkt->u32DataNum; j++) {
				uint8_t *ptr = pkt->pu8Addr + pkt->asackInfo[j].u32PackOffset;
				int      len = pkt->asackInfo[j].u32PackLength;
				fragment_nal(ptr, len);
			}
		} else {
			fragment_nal(pkt->pu8Addr, pkt->u32Len);
		}
	}

	if (MI_VENC_ReleaseStream(MDEV 0, &stream)) {
		fprintf(stderr, "waybeam: MI_VENC_ReleaseStream failed\n");
		return 1;
	}

	return 0;
}

/* --------------------------------------------------------------------------
 * Signal handler
 * -------------------------------------------------------------------------- */
static void handle_signal(int sig) {
	(void)sig;
	wb.running = false;
}

/* --------------------------------------------------------------------------
 * Print usage
 * -------------------------------------------------------------------------- */
static void usage(const char *prog) {
	fprintf(stderr,
		"Usage: %s [bitrate_kbps] [host] [port] [sensor_index] [slice_rows]\n"
		"\n"
		"  bitrate_kbps  Target bitrate in kbps      (default: %d)\n"
		"  host          Destination IP address       (default: %s)\n"
		"  port          Destination UDP port         (default: %d)\n"
		"  sensor_index  Sensor resolution index      (default: 0)\n"
		"  slice_rows    Rows per slice (0=full frame) (default: 0)\n"
		"\n"
		"Environment:\n"
		"  SENSOR        Sensor name for /etc/sensors/<name>.bin ISP tuning file\n",
		prog,
		WB_DEFAULT_BRATE,
		WB_DEFAULT_HOST,
		WB_DEFAULT_PORT);
}

/* --------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------- */
int main(int argc, char **argv) {
	if (argc == 2 && (strcmp(argv[1], "-h") == 0
			       || strcmp(argv[1], "--help") == 0)) {
		usage(argv[0]);
		return 0;
	}

	/* Defaults */
	wb.bitrate      = WB_DEFAULT_BRATE;
	wb.mtu_size     = WB_MTU_SIZE;
	wb.sensor_index = 0;
	wb.slice        = 0;

	char dst_addr[64];
	strncpy(dst_addr, WB_DEFAULT_HOST, sizeof(dst_addr) - 1);
	dst_addr[sizeof(dst_addr) - 1] = '\0';
	int dst_port = WB_DEFAULT_PORT;

	if (argc > 1) wb.bitrate      = atoi(argv[1]);
	if (argc > 2) strncpy(dst_addr, argv[2], sizeof(dst_addr) - 1);
	if (argc > 3) dst_port        = atoi(argv[3]);
	if (argc > 4) wb.sensor_index = atoi(argv[4]);
	if (argc > 5) wb.slice        = atoi(argv[5]);

	/* Clamp bitrate to a sane range */
	if (wb.bitrate < 256)   wb.bitrate = 256;
	if (wb.bitrate > 51200) wb.bitrate = 51200;

	/* Initialise the SigmaStar MI system */
	if (MI_SYS_Init(SDEV)) {
		fprintf(stderr, "waybeam: MI_SYS_Init failed\n");
		return 1;
	}

	if (mi_snr_init()) {
		fprintf(stderr, "waybeam: sensor init failed\n");
		return 1;
	}

	if (mi_vif_init()) {
		fprintf(stderr, "waybeam: VIF init failed\n");
		return 1;
	}

#ifdef SIGMASTAR_MARUKO
	if (mi_isp_init()) {
		fprintf(stderr, "waybeam: ISP init failed\n");
		return 1;
	}
#endif

	if (mi_vpe_init()) {
		fprintf(stderr, "waybeam: VPE/SCL init failed\n");
		return 1;
	}

	if (mi_venc_init()) {
		fprintf(stderr, "waybeam: VENC init failed\n");
		return 1;
	}

	/* Give the ISP a moment to converge after init. */
	sleep(1);

	if (mi_venc_config()) {
		fprintf(stderr, "waybeam: VENC config failed\n");
		return 1;
	}

	/* Set up UDP socket */
	wb.address.sin_family      = AF_INET;
	wb.address.sin_port        = htons(dst_port);
	wb.address.sin_addr.s_addr = inet_addr(dst_addr);
	wb.socket_fd               = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (wb.socket_fd < 0) {
		fprintf(stderr, "waybeam: socket() failed: %s\n", strerror(errno));
		return 1;
	}

	signal(SIGINT,  handle_signal);
	signal(SIGTERM, handle_signal);

	printf("waybeam: sensor=%d bitrate=%dkbps slice=%d | %dx%d@%dfps | %s:%d\n",
		wb.sensor_index, wb.bitrate, wb.slice,
		wb.width, wb.height, wb.fps,
		dst_addr, dst_port);

	/* Main streaming loop */
	wb.running = true;
	while (wb.running) {
		if (read_frame())
			break;
	}

	printf("waybeam: shutting down\n");

	close(wb.socket_fd);

	if (mi_venc_deinit())
		fprintf(stderr, "waybeam: VENC deinit failed\n");

	if (mi_vpe_deinit())
		fprintf(stderr, "waybeam: VPE/SCL deinit failed\n");

#ifdef SIGMASTAR_MARUKO
	if (mi_isp_deinit())
		fprintf(stderr, "waybeam: ISP deinit failed\n");
#endif

	if (mi_vif_deinit())
		fprintf(stderr, "waybeam: VIF deinit failed\n");

	if (mi_snr_deinit())
		fprintf(stderr, "waybeam: SNR deinit failed\n");

	if (MI_SYS_Exit(SDEV))
		fprintf(stderr, "waybeam: MI_SYS_Exit failed\n");

	return 0;
}
