/* star6e_jpeg.c — Star6E (Infinity6E) MJPEG snapshot backend.
 *
 * Creates one dedicated VENC channel (default ch7) bound to the same
 * VPE output port the main H.265 channel taps.  Channel stays
 * idle (StartRecvPic off) between requests; on each capture we flip
 * StartRecvPic on, poll MI_VENC_Query for ready packs, drain one frame
 * via MI_VENC_GetStream, copy the bytes, ReleaseStream, then turn
 * StartRecvPic back off.  All MI_VENC calls go through the dlopen
 * dispatch macros set up in include/star6e.h.
 *
 * Channel-id 7 is well clear of ch0 (main) and ch1 (dual/recorder).
 * The bind survives across captures, so steady-state cost is one VENC
 * channel slot — no encoder CPU when StartRecvPic is off.
 */

#include "venc_jpeg.h"
#include "star6e.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_PACKS_PER_JPEG 8   /* MJPEG can split a frame into APP/VDO/PIC packs */

static MI_SYS_ChnPort_t g_vpe_port;
static int g_have_vpe_port = 0;
static MI_VENC_CHN g_chn = -1;
static int g_bound = 0;
static int g_chn_created = 0;
static uint32_t g_quality = 80;

void venc_jpeg_set_source(const void *vpe_port_opaque)
{
	if (!vpe_port_opaque) {
		g_have_vpe_port = 0;
		return;
	}
	g_vpe_port = *(const MI_SYS_ChnPort_t *)vpe_port_opaque;
	g_have_vpe_port = 1;
}

int venc_jpeg_backend_init(const VencJpegConfig *cfg)
{
	if (!cfg)
		return -EINVAL;
	if (!g_have_vpe_port) {
		fprintf(stderr, "[jpeg-star6e] no VPE source registered; "
			"call venc_jpeg_set_source() before init\n");
		return -ENODEV;
	}
	if (cfg->width == 0 || cfg->height == 0) {
		fprintf(stderr, "[jpeg-star6e] width/height must be non-zero "
			"(got %ux%u)\n", cfg->width, cfg->height);
		return -EINVAL;
	}

	uint32_t w = cfg->width, h = cfg->height;
	uint32_t q = cfg->quality ? cfg->quality : 80;
	if (q > 99) q = 99;
	if (q < 1) q = 1;
	g_quality = q;
	g_chn = (MI_VENC_CHN)cfg->channel;

	MI_VENC_ChnAttr_t attr = {0};
	attr.attrib.codec = I6_VENC_CODEC_MJPG;
	attr.attrib.mjpg.maxWidth = w;
	attr.attrib.mjpg.maxHeight = h;
	attr.attrib.mjpg.bufSize = w * h * 3 / 2;
	attr.attrib.mjpg.byFrame = 1;
	attr.attrib.mjpg.width = w;
	attr.attrib.mjpg.height = h;

	attr.rate.mode = I6_VENC_RATEMODE_MJPGQP;
	attr.rate.mjpgQp.fpsNum = 5;   /* low — we'll only ever pull on demand */
	attr.rate.mjpgQp.fpsDen = 1;
	attr.rate.mjpgQp.quality = q;

	MI_S32 ret = MI_VENC_CreateChn(g_chn, &attr);
	if (ret != 0) {
		fprintf(stderr, "[jpeg-star6e] MI_VENC_CreateChn(%d) failed %d\n",
			(int)g_chn, ret);
		return -EIO;
	}
	g_chn_created = 1;

	MI_U32 venc_dev = 0;
	if (MI_VENC_GetChnDevid(g_chn, &venc_dev) != 0) {
		fprintf(stderr, "[jpeg-star6e] MI_VENC_GetChnDevid(%d) failed\n", (int)g_chn);
		MI_VENC_DestroyChn(g_chn);
		g_chn_created = 0;
		return -EIO;
	}
	MI_SYS_ChnPort_t jpeg_port = {
		.module  = I6_SYS_MOD_VENC,
		.device  = venc_dev,
		.channel = (unsigned)g_chn,
		.port    = 0,
	};

	/* Bind VPE output → JPEG VENC input.  1:N from VPE is supported
	 * (the dual-stream path uses the same pattern at the main channel's
	 * source port).  FRAMEBASE link mode + low destination fps — the
	 * SDK uses dstFps to throttle which frames make it into the JPEG
	 * channel; 5 fps matches the rate-control attr above. */
	ret = MI_SYS_BindChnPort2(&g_vpe_port, &jpeg_port, 30, 5,
		I6_SYS_LINK_FRAMEBASE, 0);
	if (ret != 0) {
		fprintf(stderr, "[jpeg-star6e] BindChnPort2 VPE→JPEG-VENC failed %d\n", ret);
		MI_VENC_DestroyChn(g_chn);
		g_chn_created = 0;
		return -EIO;
	}
	g_bound = 1;

	fprintf(stderr, "[jpeg-star6e] init OK: chn=%d %ux%u q=%u\n",
		(int)g_chn, w, h, q);
	return 0;
}

static int64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int venc_jpeg_backend_capture(uint8_t **out_buf, size_t *out_len,
	uint32_t timeout_ms)
{
	if (!out_buf || !out_len)
		return -EINVAL;
	*out_buf = NULL;
	*out_len = 0;
	if (!g_chn_created || !g_bound)
		return -ENODEV;
	if (timeout_ms == 0)
		timeout_ms = 1500;

	MI_S32 ret = MI_VENC_StartRecvPic(g_chn);
	if (ret != 0) {
		fprintf(stderr, "[jpeg-star6e] StartRecvPic failed %d\n", ret);
		return -EIO;
	}

	int rc = 0;
	int64_t deadline = now_ms() + (int64_t)timeout_ms;
	MI_VENC_Stream_t stream = {0};
	MI_VENC_Pack_t packs[MAX_PACKS_PER_JPEG] = {0};

	/* Wait for at least one pending pack.  Query is cheap and returns
	 * immediately; sleep 5 ms between polls so we don't burn CPU. */
	MI_VENC_Stat_t stat = {0};
	for (;;) {
		if (MI_VENC_Query(g_chn, &stat) == 0 && stat.curPacks > 0)
			break;
		if (now_ms() >= deadline) {
			rc = -ETIMEDOUT;
			goto stop;
		}
		usleep(5000);
	}

	uint32_t n = stat.curPacks;
	if (n > MAX_PACKS_PER_JPEG) n = MAX_PACKS_PER_JPEG;
	stream.count = n;
	stream.packet = packs;

	ret = MI_VENC_GetStream(g_chn, &stream, 200);
	if (ret != 0) {
		fprintf(stderr, "[jpeg-star6e] GetStream failed %d\n", ret);
		rc = -EIO;
		goto stop;
	}
	if (stream.count == 0) {
		fprintf(stderr, "[jpeg-star6e] GetStream returned 0 packs\n");
		MI_VENC_ReleaseStream(g_chn, &stream);
		rc = -EIO;
		goto stop;
	}

	/* Concatenate packs into a single JPEG blob. */
	size_t total = 0;
	for (uint32_t i = 0; i < stream.count; ++i)
		total += stream.packet[i].length;
	if (total == 0) {
		MI_VENC_ReleaseStream(g_chn, &stream);
		rc = -EIO;
		goto stop;
	}

	uint8_t *copy = malloc(total);
	if (!copy) {
		MI_VENC_ReleaseStream(g_chn, &stream);
		rc = -ENOMEM;
		goto stop;
	}

	size_t off = 0;
	for (uint32_t i = 0; i < stream.count; ++i) {
		memcpy(copy + off, stream.packet[i].data, stream.packet[i].length);
		off += stream.packet[i].length;
	}
	MI_VENC_ReleaseStream(g_chn, &stream);

	*out_buf = copy;
	*out_len = total;

stop:
	(void)MI_VENC_StopRecvPic(g_chn);
	return rc;
}

/* ── Grayscale (P5 PGM) capture ──────────────────────────────────────────
 *
 * The MJPEG path feeds NV12 frames straight into a hardware VENC channel, so
 * the host never sees raw pixels.  To hand a consumer (e.g. a boot-time QR
 * scan) plain grayscale, we attach our OWN user-frame reader to the same VPE
 * output port the JPEG channel taps — port0 is I6_COMPR_NONE YUV420SP
 * (star6e_pipeline.c), so its Y plane is a CPU-readable luma image at the
 * main stream resolution.  Same source as the JPEG; second consumer.
 *
 * The MI_SYS buffer ABI and the GetBuf/PutBuf idiom mirror the proven
 * detector tap in star6e_ipu_yolo.c; the port0 user-depth registration
 * mirrors the stab tap in star6e_framing_stab.c.  A user output depth is
 * registered only for the duration of one capture and released again, so the
 * running pipeline is left exactly as it was between snapshots.  Every step
 * is best-effort: any failure returns an error and the endpoint serves it —
 * nothing else in the pipeline is touched. */

typedef struct {
	int eTileMode;
	int ePixelFormat;
	int eCompressMode;
	int eFrameScanMode;
	int eFieldType;
	int ePhylayoutType;
	MI_U16 u16Width;
	MI_U16 u16Height;
	void *pVirAddr[3];
	MI_U64 phyAddr[3];
	MI_U32 u32Stride[3];
	MI_U32 u32BufSize;
	MI_U16 u16RingBufStartLine;
	MI_U16 u16RingBufRealTotalHeight;
	struct {
		int eType;
		union { MI_U32 u32GlobalGradient; } uIspInfo;
	} stFrameIspInfo;
	MI_U8 reserved_rect[16];
} GrayFrameData_t;

typedef struct {
	MI_U64 u64Pts;
	MI_U64 u64SidebandMsg;
	int eBufType;
	MI_BOOL bEndOfStream;
	MI_BOOL bUsrBuf;
	MI_U32 u32SequenceNumber;
	MI_BOOL bDrop;
	union {
		GrayFrameData_t stFrameData;
		MI_U8 reserved_union[512];
	};
	MI_U8 u8CusFlag;
} GrayBufInfo_t;

#define GRAY_E_BUFDATA_FRAME 1
#define GRAY_MAX_DIM         8192u   /* sanity clamp on frame geometry */

typedef MI_S32 (*gray_get_buf_fn)(MI_SYS_ChnPort_t *port, GrayBufInfo_t *buf,
	MI_S32 *handle);
typedef MI_S32 (*gray_put_buf_fn)(MI_S32 handle);
typedef MI_S32 (*gray_mmap_fn)(MI_U64 phy, MI_U32 size, void **vir, MI_U8 cache);
typedef MI_S32 (*gray_munmap_fn)(void *vir, MI_U32 size);

static gray_get_buf_fn g_gray_get_buf;
static gray_put_buf_fn g_gray_put_buf;
static gray_mmap_fn    g_gray_mmap;
static gray_munmap_fn  g_gray_munmap;

static int gray_load_syms(void)
{
	/* libmi_sys stays resident for the whole run; open once and cache. */
	static void *sys_h;
	if (g_gray_get_buf && g_gray_put_buf && g_gray_mmap)
		return 0;
	if (!sys_h) {
		sys_h = dlopen("libmi_sys.so", RTLD_LAZY | RTLD_GLOBAL);
		if (!sys_h)
			return -1;
	}
	g_gray_get_buf = (gray_get_buf_fn)dlsym(sys_h,
		"MI_SYS_ChnOutputPortGetBuf");
	g_gray_put_buf = (gray_put_buf_fn)dlsym(sys_h,
		"MI_SYS_ChnOutputPortPutBuf");
	g_gray_mmap = (gray_mmap_fn)dlsym(sys_h, "MI_SYS_Mmap");
	g_gray_munmap = (gray_munmap_fn)dlsym(sys_h, "MI_SYS_Munmap");
	if (!g_gray_get_buf || !g_gray_put_buf || !g_gray_mmap)
		return -1;
	return 0;
}

/* Pack the Y plane of one NV12 frame into a freshly malloc'd P5 PGM blob
 * (header + tightly-packed rows, stride removed).  Returns 0 on success. */
static int gray_frame_to_pgm(const GrayFrameData_t *fr, uint8_t **out_buf,
	size_t *out_len)
{
	uint32_t w = fr->u16Width, h = fr->u16Height;
	uint32_t stride = fr->u32Stride[0] ? fr->u32Stride[0] : w;
	MI_U64 phy = fr->phyAddr[0];
	void *vir = NULL;

	if (w == 0 || h == 0 || w > GRAY_MAX_DIM || h > GRAY_MAX_DIM || !phy)
		return -EIO;

	/* Map the Y plane non-cached (flag 0) so reads see the latest DMA
	 * without a cache invalidate; the source stays untouched. */
	if (g_gray_mmap(phy, stride * h, &vir, 0) != 0 || !vir)
		return -EIO;

	char hdr[32];
	int hlen = snprintf(hdr, sizeof(hdr), "P5\n%u %u\n255\n", w, h);
	size_t total = (size_t)hlen + (size_t)w * h;
	uint8_t *out = malloc(total);
	if (!out) {
		g_gray_munmap(vir, stride * h);
		return -ENOMEM;
	}

	memcpy(out, hdr, (size_t)hlen);
	const uint8_t *src = (const uint8_t *)vir;
	uint8_t *dst = out + hlen;
	for (uint32_t row = 0; row < h; ++row)
		memcpy(dst + (size_t)row * w, src + (size_t)row * stride, w);

	g_gray_munmap(vir, stride * h);

	*out_buf = out;
	*out_len = total;
	return 0;
}

int venc_jpeg_backend_capture_gray(uint8_t **out_buf, size_t *out_len,
	uint32_t timeout_ms)
{
	if (!out_buf || !out_len)
		return -EINVAL;
	*out_buf = NULL;
	*out_len = 0;
	if (!g_have_vpe_port)
		return -ENODEV;
	if (gray_load_syms() != 0)
		return -EIO;
	if (timeout_ms == 0)
		timeout_ms = 1500;

	/* Register a shallow user-frame queue on the (already bound) VPE port so
	 * GetBuf sees frames; without it an unregistered user consumer reads 0. */
	if (MI_SYS_SetChnOutputPortDepth(&g_vpe_port, 2, 4) != 0) {
		fprintf(stderr, "[jpeg-star6e] gray: SetChnOutputPortDepth failed\n");
		return -EIO;
	}

	int rc = -ETIMEDOUT;
	int64_t deadline = now_ms() + (int64_t)timeout_ms;
	for (;;) {
		GrayBufInfo_t buf;
		MI_S32 handle = 0;

		memset(&buf, 0, sizeof(buf));
		if (g_gray_get_buf(&g_vpe_port, &buf, &handle) != 0) {
			if (now_ms() >= deadline)
				break;
			usleep(2000);
			continue;
		}
		if (buf.eBufType != GRAY_E_BUFDATA_FRAME ||
		    !buf.stFrameData.phyAddr[0]) {
			g_gray_put_buf(handle);
			if (now_ms() >= deadline)
				break;
			usleep(2000);
			continue;
		}
		rc = gray_frame_to_pgm(&buf.stFrameData, out_buf, out_len);
		g_gray_put_buf(handle);
		break;
	}

	(void)MI_SYS_SetChnOutputPortDepth(&g_vpe_port, 0, 0);
	return rc;
}

int venc_jpeg_backend_set_quality(uint32_t q)
{
	if (!g_chn_created)
		return -ENODEV;
	if (q == 0) q = 1;
	if (q > 99) q = 99;

	MI_VENC_ChnAttr_t attr = {0};
	MI_S32 gret = MI_VENC_GetChnAttr(g_chn, &attr);
	if (gret != 0) {
		fprintf(stderr,
			"[jpeg-star6e] GetChnAttr(%d) failed %d during live "
			"quality update\n", (int)g_chn, gret);
		return -EIO;
	}
	attr.rate.mjpgQp.quality = q;
	MI_S32 sret = MI_VENC_SetChnAttr(g_chn, &attr);
	if (sret != 0) {
		fprintf(stderr,
			"[jpeg-star6e] SetChnAttr(q=%u) failed %d\n", q, sret);
		return -EIO;
	}
	g_quality = q;
	return 0;
}

void venc_jpeg_backend_shutdown(void)
{
	if (g_bound) {
		MI_U32 venc_dev = 0;
		(void)MI_VENC_GetChnDevid(g_chn, &venc_dev);
		MI_SYS_ChnPort_t jpeg_port = {
			.module  = I6_SYS_MOD_VENC,
			.device  = venc_dev,
			.channel = (unsigned)g_chn,
			.port    = 0,
		};
		(void)MI_SYS_UnBindChnPort(&g_vpe_port, &jpeg_port);
		g_bound = 0;
	}
	if (g_chn_created) {
		(void)MI_VENC_DestroyChn(g_chn);
		g_chn_created = 0;
	}
	g_chn = -1;
}
