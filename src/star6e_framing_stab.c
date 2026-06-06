#include "star6e_framing_stab.h"
#include "star6e_framing_host.h"
#include "star6e_pipeline.h"
#include "imu_ring.h"

#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* ── Image stabilization framing module — "stab" preset (Star6E) ──────────
 *
 * HW-crop DIS using MI_IVE_Shift_Detector.  Registered as a FramingModule
 * (see star6e_framing.h); the pipeline selects it when video0.framing="stab".
 *
 *   VIF → VPE → port0 (SCL-cropped to the encode dim) → bound zero-copy to
 *               VENC ch0.
 *   VIF → VPE → port1 (256x256 centre tap) → detector thread:
 *       1. drain the port1 tap frame
 *       2. run MI_IVE_Shift_Detector on a centre Y patch vs the previous frame
 *       3. accumulate dx/dy → off_x/off_y, clipped to half the dead border
 *       4. reprogram port0's MI_VPE_SetPortCrop to the shifted window — the
 *          bind feeds VENC the stabilized crop with no per-frame copy.
 *
 * If the BSP rejects the port1 tap, port0 stays bound at a static centre crop
 * (no detector).  Local types and dlsym-resolved symbols mirror the working
 * standalone star.c sample; pulling SigmaStar mi_sys.h / mi_ive.h here would
 * collide with waybeam's own MI compatibility layer in include/star6e.h. */

typedef MI_U64 STAB_MI_PHY;

typedef struct {
	MI_U16 u16X;
	MI_U16 u16Y;
	MI_U16 u16Width;
	MI_U16 u16Height;
} StabSysWindowRect_t;

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
	STAB_MI_PHY phyAddr[3];
	MI_U32 u32Stride[3];
	MI_U32 u32BufSize;
	MI_U16 u16RingBufStartLine;
	MI_U16 u16RingBufRealTotalHeight;
	struct {
		int eType;
		union {
			MI_U32 u32GlobalGradient;
		} uIspInfo;
	} stFrameIspInfo;
	StabSysWindowRect_t stContentCropWindow;
} StabSysFrameData_t;

typedef struct {
	void *pVirAddr;
	STAB_MI_PHY phyAddr;
	MI_U32 u32BufSize;
	MI_U32 u32ContentSize;
	MI_BOOL bEndOfFrame;
	MI_U64 u64SeqNum;
} StabSysRawData_t;

typedef struct {
	void *pVirAddr;
	STAB_MI_PHY phyAddr;
	MI_U32 u32Size;
	MI_U32 u32ExtraData;
	MI_U32 eDataFromModule;
} StabSysMetaData_t;

typedef struct {
	MI_U64 u64Pts;
	MI_U64 u64SidebandMsg;
	int eBufType;
	MI_BOOL bEndOfStream;
	MI_BOOL bUsrBuf;
	MI_U32 u32SequenceNumber;
	MI_BOOL bDrop;
	union {
		StabSysFrameData_t stFrameData;
		StabSysRawData_t stRawData;
		StabSysMetaData_t stMetaData;
		MI_U8 reserved_union[512];
	};
	MI_U8 u8CusFlag;
} StabSysBufInfo_t;

typedef struct {
	MI_U16 u16BufHAlignment;
	MI_U16 u16BufVAlignment;
	MI_U16 u16BufChromaAlignment;
	MI_BOOL bClearPadding;
} StabSysFrameBufExtraConfig_t;

typedef struct {
	MI_U16 u16Width;
	MI_U16 u16Height;
	int eFrameScanMode;
	int eFormat;
	StabSysFrameBufExtraConfig_t stFrameBufExtraConf;
	int eCompressMode;
} StabSysBufFrameConfig_t;

typedef struct {
	int eBufType;
	MI_U32 u32Flags;
	MI_U64 u64TargetPts;
	union {
		StabSysBufFrameConfig_t stFrameCfg;
		struct { MI_U32 u32Size; } stRawCfg;
		struct { MI_U32 u32Size; } stMetaCfg;
	};
	MI_U8 u8CusFlag;
} StabSysBufConf_t;

typedef MI_S32 StabSysBufHandle_t;

#define STAB_E_BUFDATA_FRAME              1
#define STAB_E_FRAME_SCAN_MODE_PROGRESSIVE 0
#define STAB_E_PIXEL_FRAME_I8             9

typedef int StabIveImageType_e;
#define STAB_E_IVE_IMAGE_TYPE_U8C1 0x0
#define STAB_E_IVE_IMAGE_TYPE_S8C1 0x1

typedef struct {
	StabIveImageType_e eType;
	STAB_MI_PHY aphyPhyAddr[3];
	MI_U8 *apu8VirAddr[3];
	MI_U16 azu16Stride[3];
	MI_U16 u16Width;
	MI_U16 u16Height;
	MI_U16 u16Reserved;
} StabIveImage_t;

#define STAB_E_IVE_SHIFT_DETECT_MODE_SINGLE 0x00

typedef struct {
	int enMode;
	MI_U8 pyramid_level;
	MI_U8 search_range;
	MI_U16 u16Left;
	MI_U16 u16Top;
	MI_U16 u16Width;
	MI_U16 u16Height;
} StabIveShiftDetectCtrl_t;

typedef MI_S32 (*stab_sys_get_fd_fn_t)(MI_SYS_ChnPort_t *port, MI_S32 *fd);
typedef MI_S32 (*stab_sys_close_fd_fn_t)(MI_S32 fd);
typedef MI_S32 (*stab_sys_out_get_buf_fn_t)(MI_SYS_ChnPort_t *port,
	StabSysBufInfo_t *buf, StabSysBufHandle_t *handle);
typedef MI_S32 (*stab_sys_out_put_buf_fn_t)(StabSysBufHandle_t handle);
typedef MI_S32 (*stab_sys_flush_inv_cache_fn_t)(void *vir, MI_U32 size);
typedef MI_S32 (*stab_sys_va2pa_fn_t)(void *vir, STAB_MI_PHY *phy);

typedef int StabIveHandle_t;
typedef MI_S32 (*stab_ive_create_fn_t)(StabIveHandle_t handle);
typedef MI_S32 (*stab_ive_destroy_fn_t)(StabIveHandle_t handle);
typedef MI_S32 (*stab_ive_shift_fn_t)(StabIveHandle_t handle,
	StabIveImage_t *src1, StabIveImage_t *src2,
	StabIveImage_t *dst_x, StabIveImage_t *dst_y,
	StabIveShiftDetectCtrl_t *ctrl, MI_BOOL instant);

/* Shift_Detector geometry.  On Star6E there is no IVE kernel module, so
 * MI_IVE_Shift_Detector runs as a userspace CPU fallback — it is the
 * dominant per-frame stab cost (~19ms on the A7), independent of the crop
 * resolution.  These are the FULL 384/256/3 values: a larger correlation
 * box and a 3-level pyramid give noticeably smoother motion estimates than
 * the cheapened 256/128/2 config (which was tried for fps but produced
 * visibly jittery/shaky stabilization — the estimates are noisier and the
 * offset is applied raw every frame).  Smoothness was chosen over the fps
 * the cheaper detector bought.  margin = (crop-box)/2 = 64px and
 * SEARCH_RANGE = 96 are unchanged. */
#define STAB_SHIFT_CROP_W   384
#define STAB_SHIFT_CROP_H   384
#define STAB_BOX_SIZE       256
#define STAB_PYRAMID        3
#define STAB_SEARCH_RANGE   96
#define STAB_SHIFT_SIGN_X   (-1)
#define STAB_SHIFT_SIGN_Y   (-1)
/* Run the (CPU-bound) detector every Nth drained frame.  N=1 (detect +
 * correct every frame) is required for smooth stabilization: sampling
 * motion below the frame rate aliases real jitter (Nyquist), so the
 * accumulator mis-corrects and the image visibly fights/shakes.  Keep
 * this at 1 unless a higher resolution makes the per-frame detector cost
 * exceed the frame budget — at 1152x864 the cheapened detector (256 crop,
 * 128 box, 2-level pyramid) fits 60fps every frame.  Bump only as a last
 * resort; it trades stabilization quality for fps. */
#define STAB_DETECT_EVERY   1
/* Return-to-center policy (see the recenter block in the stab thread).
 * Unconditional decay fights live stabilization, so gate it:
 *  - MOTION_THRESH: |inter-frame shift| (px) above which the camera counts
 *    as actively moving — re-arms the stillness timer.
 *  - STILL_FRAMES: consecutive sub-threshold frames before the offset
 *    decays fully back to center (the "cooldown").
 *  - EDGE_PCT: while still moving, only give margin back on an axis once
 *    its offset passes this % of the dead-border, so corrections in the
 *    central zone are never eroded (no fight); during sustained motion the
 *    offset settles near the edge instead of being pinned/saturated. */
#define STAB_MOTION_THRESH         1
/* "Lock the scene stiffer" tuning: hold the stabilized crop longer before
 * leaking back to center.  STILL_FRAMES is the post-motion cooldown (frames
 * of stillness before the settled-recenter starts) — longer = the view stays
 * locked after a disturbance instead of creeping back.  EDGE_PCT is how much
 * of the ±border the offset may use during sustained motion before margin is
 * given back — higher = sticks harder (closer to saturation) before leaking.
 * The leak RATE itself is the per-preset recenter_speed (venc_config.c). */
#define STAB_RECENTER_STILL_FRAMES 60
#define STAB_RECENTER_EDGE_PCT     88
/* Final EMA low-pass on the applied crop offset (per frame, DETECT_EVERY=1).
 * applied += ALPHA * (target - applied).  Lower = smoother but more lag.
 * 0.30 ≈ 3-frame time constant (~33ms @90fps): kills the per-frame judder
 * that the raw offset magnifies at the geometry extremes (low/high) while
 * the lag stays imperceptible for the "locked scene" feel. */
#define STAB_OUTPUT_SMOOTH_ALPHA   0.30

static stab_sys_get_fd_fn_t g_stab_sys_get_fd;
static stab_sys_close_fd_fn_t g_stab_sys_close_fd;
static stab_sys_out_get_buf_fn_t g_stab_sys_out_get_buf;
static stab_sys_out_put_buf_fn_t g_stab_sys_out_put_buf;
static stab_sys_flush_inv_cache_fn_t g_stab_sys_flush_inv_cache;
static stab_sys_va2pa_fn_t g_stab_sys_va2pa;

static stab_ive_create_fn_t g_stab_ive_create;
static stab_ive_destroy_fn_t g_stab_ive_destroy;
static stab_ive_shift_fn_t g_stab_ive_shift;
static StabIveHandle_t g_stab_ive_handle;
static int g_stab_ive_created;
static void *g_stab_ive_lib;

static pthread_t g_stab_thread;
static volatile int g_stab_running;
/* Stabilization data path (single HW-crop mode): VPE port0 hardware-crops the
 * stab window straight to VENC (zero-copy bind); a tiny port1 256x256 tap feeds
 * the detector, which updates port0's SetPortCrop rect per detect.  No
 * per-frame BufBlitPa; port0 tears down via the standard bound path.
 *   g_stab_tap_active=1 when the port1 detector tap came up and the detector
 *   thread runs.  If the BSP rejects the tap, port0 still binds to VENC
 *   (static centre crop, no shake compensation) and the detector is skipped.
 *   g_stab_pause/parked are a quiesce handshake so teardown can disable port1
 *   while the detector is guaranteed not inside an MI_SYS call. */
static volatile int g_stab_tap_active;
static volatile int g_stab_pause;
static volatile int g_stab_parked;
static pthread_mutex_t g_stab_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_stab_src_w;     /* image (port0-output) domain — detector + */
static uint32_t g_stab_src_h;     /* accumulator + recenter all use this dim   */
static uint32_t g_stab_enc_w;
static uint32_t g_stab_enc_h;
/* VPE channel input dim (precrop / VIF→VPE window).  MI_VPE_SetPortCrop is an
 * INPUT-domain crop, so HW mode scales the image-domain stab window into this
 * domain.  Equals g_stab_src_* when the sensor isn't aspect-cropped (ratio 1).
 * Only HW-crop mode uses these. */
static uint32_t g_stab_pre_w;
static uint32_t g_stab_pre_h;
static uint32_t g_stab_crop_percent;
static uint32_t g_stab_recenter_period;   /* frames between 1-pixel leak; 0=off */
/* Advanced "stab" feel knobs, set from VencConfig in star6e_stab_configure()
 * (MUT_RESTART, so fixed for the lifetime of a stab run).  Initialized to the
 * STAB_* compile-time defaults as a fallback if configure is bypassed. */
static double g_stab_smooth_alpha = STAB_OUTPUT_SMOOTH_ALPHA;
static int g_stab_still_frames_max = STAB_RECENTER_STILL_FRAMES;
static int g_stab_edge_pct = STAB_RECENTER_EDGE_PCT;
static int g_stab_motion_thresh = STAB_MOTION_THRESH;
static volatile int g_stab_off_x;
static volatile int g_stab_off_y;
/* User-controlled pan center as parts-per-thousand of (src_w, src_h).
 * 500/500 = exact center.  Updated live via star6e_stab_set_pan() so
 * the existing zoomX/zoomY HTTP controls steer the stabilized framing
 * without a pipeline restart. */
static volatile int g_stab_pan_x_mil = 500;
static volatile int g_stab_pan_y_mil = 500;
static MI_SYS_ChnPort_t g_stab_vpe_port;

/* apply_ae_crop is defined after the detector block but called by set_pan. */
static void star6e_stab_apply_ae_crop(void);

static int star6e_stab_load_sys_extra_symbols(void)
{
	void *h;

	if (g_stab_sys_out_get_buf && g_stab_sys_out_put_buf &&
	    g_stab_sys_flush_inv_cache && g_stab_sys_va2pa)
		return 0;

	h = dlopen("libmi_sys.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!h)
		return -1;

	g_stab_sys_get_fd = (stab_sys_get_fd_fn_t)dlsym(h, "MI_SYS_GetFd");
	g_stab_sys_close_fd = (stab_sys_close_fd_fn_t)dlsym(h, "MI_SYS_CloseFd");
	g_stab_sys_out_get_buf = (stab_sys_out_get_buf_fn_t)dlsym(h,
		"MI_SYS_ChnOutputPortGetBuf");
	g_stab_sys_out_put_buf = (stab_sys_out_put_buf_fn_t)dlsym(h,
		"MI_SYS_ChnOutputPortPutBuf");
	g_stab_sys_flush_inv_cache = (stab_sys_flush_inv_cache_fn_t)dlsym(h,
		"MI_SYS_FlushInvCache");
	g_stab_sys_va2pa = (stab_sys_va2pa_fn_t)dlsym(h, "MI_SYS_Va2Pa");

	return (g_stab_sys_out_get_buf && g_stab_sys_out_put_buf &&
		g_stab_sys_flush_inv_cache && g_stab_sys_va2pa) ? 0 : -1;
}

static uint64_t star6e_stab_pts_us(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

/* Compute encoded dims preserving the configured image_w:image_h aspect.
 * Integer math only — no floats per CONVENTIONS.md §1.  Width is 8-aligned
 * down (VENC + SCL); height is 2-aligned down (NV12). */
static void star6e_stab_compute_crop_dims(uint32_t src_w, uint32_t src_h,
	uint32_t pct, uint32_t *out_w, uint32_t *out_h)
{
	uint32_t w;
	uint32_t h;

	w = (src_w * pct) / 100u;
	w &= ~7u;
	if (w == 0) w = src_w & ~7u;
	h = (uint32_t)(((uint64_t)w * src_h) / src_w);
	h &= ~1u;

	if (h > src_h) {
		h = ((src_h * pct) / 100u) & ~1u;
		if (h == 0) h = src_h & ~1u;
		w = (uint32_t)(((uint64_t)h * src_w) / src_h);
		w &= ~7u;
	}

	if (w < 64 || h < 64 || w > src_w || h > src_h) {
		w = src_w & ~7u;
		h = src_h & ~1u;
	}

	*out_w = w;
	*out_h = h;
}

static int star6e_stab_pan_clamp_mil(double v)
{
	int mil;

	if (!isfinite(v) || v <= 0.0) return 0;
	if (v >= 1.0) return 1000;
	mil = (int)(v * 1000.0 + 0.5);
	if (mil < 0) mil = 0;
	if (mil > 1000) mil = 1000;
	return mil;
}

static void star6e_stab_configure(uint32_t src_w, uint32_t src_h,
	uint32_t crop_pct, uint32_t recenter_speed, uint32_t venc_fps,
	double pan_x, double pan_y, uint32_t smooth_pct, uint32_t still_frames,
	uint32_t edge_pct, uint32_t motion_thresh)
{
	g_stab_src_w = src_w & ~1u;
	g_stab_src_h = src_h & ~1u;
	g_stab_crop_percent = crop_pct;
	star6e_stab_compute_crop_dims(g_stab_src_w, g_stab_src_h,
		crop_pct, &g_stab_enc_w, &g_stab_enc_h);

	/* Advanced feel knobs.  Each accepts a sentinel/out-of-range value and
	 * falls back to the compile-time default, so a hand-edited config (which
	 * bypasses the HTTP validator) can never freeze or destabilize the loop:
	 *  - smooth_pct 0 or <5/>100  → default EMA alpha (smoother = lower).
	 *  - still_frames clamped to <=600 (0 = recenter as soon as settled).
	 *  - edge_pct 0 or <50/>100   → default edge-stick.
	 *  - motion_thresh clamped to <=16 (0 = any motion re-arms stillness). */
	g_stab_smooth_alpha = (smooth_pct >= 5 && smooth_pct <= 100) ?
		(double)smooth_pct / 100.0 : STAB_OUTPUT_SMOOTH_ALPHA;
	g_stab_still_frames_max = (still_frames <= 600) ?
		(int)still_frames : 600;
	g_stab_edge_pct = (edge_pct >= 50 && edge_pct <= 100) ?
		(int)edge_pct : STAB_RECENTER_EDGE_PCT;
	g_stab_motion_thresh = (motion_thresh <= 16) ? (int)motion_thresh : 16;

	/* recenter_speed is "frames between 1-pixel leak":
	 * 0 = no leak (stick to current patch),
	 * lower = faster recenter (user feedback: "Lower the second number
	 * faster crop recenter, 0 is no recenter"). venc_fps is passed for
	 * future "pixels/sec" remapping; currently the value IS the period
	 * in frames so the knob stays direct + deterministic. */
	(void)venc_fps;
	g_stab_recenter_period = recenter_speed;
	g_stab_pan_x_mil = star6e_stab_pan_clamp_mil(pan_x);
	g_stab_pan_y_mil = star6e_stab_pan_clamp_mil(pan_y);

	pthread_mutex_lock(&g_stab_lock);
	g_stab_off_x = 0;
	g_stab_off_y = 0;
	pthread_mutex_unlock(&g_stab_lock);
}

/* Live pan update — called from the LIVE_GROUP_ZOOM apply path so that
 * the existing zoomX/zoomY HTTP controls steer the stabilized framing
 * without a pipeline restart. */
static void star6e_stab_set_pan(double pan_x, double pan_y)
{
	g_stab_pan_x_mil = star6e_stab_pan_clamp_mil(pan_x);
	g_stab_pan_y_mil = star6e_stab_pan_clamp_mil(pan_y);
	/* Keep the AE meter on the stabilized crop as it pans, mirroring the
	 * zoom path's AE tracking — the only intended runtime difference
	 * between the two modes is the pan ramp. */
	star6e_stab_apply_ae_crop();
}

static int star6e_stab_max_off_x(void)
{
	return (int)((g_stab_src_w - g_stab_enc_w) / 2u);
}

static int star6e_stab_max_off_y(void)
{
	return (int)((g_stab_src_h - g_stab_enc_h) / 2u);
}

/* Map an image-domain length to the VPE-input (precrop) domain.  When the
 * sensor isn't aspect-cropped pre==src and this is the identity. */
static int star6e_stab_img_to_pre_x(int v)
{
	return (int)((int64_t)v * (int64_t)g_stab_pre_w / (int64_t)g_stab_src_w);
}
static int star6e_stab_img_to_pre_y(int v)
{
	return (int)((int64_t)v * (int64_t)g_stab_pre_h / (int64_t)g_stab_src_h);
}

/* HW-crop output: program VPE port0's SetPortCrop to the stab window
 * (enc_w x enc_h positioned at pan-center + accumulated shake offset).  The
 * window is computed in image domain (src_x/src_y math drives the
 * accumulator/feel) and then scaled into the
 * VPE INPUT (precrop) domain, because MI_VPE_SetPortCrop crops the channel
 * input — the SCL then scales that window down to the encoded port output,
 * the same downscale ratio the non-stab full-frame path uses.  x/y/w/h
 * aligned to 2 for NV12 chroma; the SDK accepts 2-px granularity (zoom path
 * precedent), preserving fine stabilization steps. */
static void star6e_stab_apply_port_crop(int acc_x, int acc_y)
{
	int pan_x = g_stab_pan_x_mil;
	int pan_y = g_stab_pan_y_mil;
	int center_x = (int)((g_stab_src_w * (uint32_t)pan_x) / 1000u);
	int center_y = (int)((g_stab_src_h * (uint32_t)pan_y) / 1000u);
	int src_x = center_x - (int)g_stab_enc_w / 2 + acc_x;
	int src_y = center_y - (int)g_stab_enc_h / 2 + acc_y;
	int max_x = (int)(g_stab_src_w - g_stab_enc_w);
	int max_y = (int)(g_stab_src_h - g_stab_enc_h);
	int rx, ry, rw, rh, rmax_x, rmax_y;
	i6_common_rect rect;
	MI_S32 ret;

	if (src_x < 0) src_x = 0;
	if (src_x > max_x) src_x = max_x;
	if (src_y < 0) src_y = 0;
	if (src_y > max_y) src_y = max_y;

	/* Scale image window → precrop (input) domain. */
	rx = star6e_stab_img_to_pre_x(src_x) & ~1;
	ry = star6e_stab_img_to_pre_y(src_y) & ~1;
	rw = star6e_stab_img_to_pre_x((int)g_stab_enc_w) & ~1;
	rh = star6e_stab_img_to_pre_y((int)g_stab_enc_h) & ~1;
	if (rw < 2) rw = 2;
	if (rh < 2) rh = 2;
	rmax_x = (int)g_stab_pre_w - rw;
	rmax_y = (int)g_stab_pre_h - rh;
	if (rx < 0) rx = 0;
	if (rx > rmax_x) rx = rmax_x;
	if (ry < 0) ry = 0;
	if (ry > rmax_y) ry = rmax_y;

	rect.x = (unsigned short)rx;
	rect.y = (unsigned short)ry;
	rect.width = (unsigned short)rw;
	rect.height = (unsigned short)rh;
	ret = MI_VPE_SetPortCrop(0, 0, &rect);
	if (ret != 0) {
		static int warned;
		if (!warned) {
			warned = 1;
			fprintf(stderr, "[waybeam] stab HW SetPortCrop(0,0) "
				"x=%d y=%d %dx%d (pre %ux%u) failed %d\n", rx, ry,
				rw, rh, g_stab_pre_w, g_stab_pre_h, (int)ret);
		}
	}
}


static int star6e_stab_make_center_y_crop(StabIveImage_t *image,
	const StabSysBufInfo_t *buf, int crop_w, int crop_h)
{
	int src_w;
	int src_h;
	int stride;
	int crop_x;
	int crop_y;

	if (!image || !buf || buf->eBufType != STAB_E_BUFDATA_FRAME ||
	    !buf->stFrameData.pVirAddr[0] || !buf->stFrameData.phyAddr[0])
		return -1;

	src_w = (int)buf->stFrameData.u16Width;
	src_h = (int)buf->stFrameData.u16Height;
	stride = (int)buf->stFrameData.u32Stride[0];
	if (crop_w > src_w) crop_w = src_w;
	if (crop_h > src_h) crop_h = src_h;
	crop_w &= ~15;
	crop_h &= ~1;
	crop_x = ((src_w - crop_w) / 2) & ~15;
	crop_y = ((src_h - crop_h) / 2) & ~1;
	if (crop_x < 0) crop_x = 0;
	if (crop_y < 0) crop_y = 0;

	memset(image, 0, sizeof(*image));
	image->eType = STAB_E_IVE_IMAGE_TYPE_U8C1;
	image->u16Width = (MI_U16)crop_w;
	image->u16Height = (MI_U16)crop_h;
	image->apu8VirAddr[0] = (MI_U8 *)buf->stFrameData.pVirAddr[0] +
		crop_y * stride + crop_x;
	image->aphyPhyAddr[0] = buf->stFrameData.phyAddr[0] +
		(STAB_MI_PHY)(crop_y * stride + crop_x);
	image->azu16Stride[0] = (MI_U16)stride;
	return 0;
}

static int star6e_stab_alloc_ive_image(StabIveImage_t *image,
	MI_U16 width, MI_U16 height, StabIveImageType_e type)
{
	MI_U32 align = 64;
	MI_U32 size;
	STAB_MI_PHY phy = 0;
	MI_S32 ret;

	memset(image, 0, sizeof(*image));
	image->eType = type;
	image->u16Width = width;
	image->u16Height = height;
	image->azu16Stride[0] = (width + (align - 1)) & ~(align - 1);
	size = image->azu16Stride[0] * height;
	if (posix_memalign((void **)&image->apu8VirAddr[0], align, size) != 0)
		return -1;
	memset(image->apu8VirAddr[0], 0, size);

	/* IVE needs a valid physical address even for 1×1 S8C1 result images;
	 * leaving aphyPhyAddr[0]=0 makes Shift_Detector return zero dx/dy. */
	ret = g_stab_sys_va2pa(image->apu8VirAddr[0], &phy);
	if (ret != 0 || !phy)
		phy = (STAB_MI_PHY)(uintptr_t)image->apu8VirAddr[0];
	image->aphyPhyAddr[0] = phy;
	return 0;
}


/* Read the gyro samples captured during the frame interval [t0, t1] from the
 * shared IMU ring (CLOCK_MONOTONIC domain).  Returns the sample count and,
 * when non-zero, the mean angular rate (rad/s) on each axis.  This is the
 * frame-aligned data a gyro estimator integrates.  Returns 0 with zeroed
 * means when the IMU is disabled or no samples fell in the window — the
 * stabilizer then runs optical-only. */
static uint32_t star6e_stab_gyro_window(struct timespec t0, struct timespec t1,
	float *mean_gx, float *mean_gy, float *mean_gz)
{
	ImuRingSample s[64];
	uint32_t n, i;
	double sx = 0.0, sy = 0.0, sz = 0.0;

	*mean_gx = 0.0f;
	*mean_gy = 0.0f;
	*mean_gz = 0.0f;
	{
		ImuRing *ring = star6e_pipeline_imu_ring();
		if (!ring)
			return 0;
		n = imu_ring_read_range(ring, t0, t1, s,
			(uint32_t)(sizeof(s) / sizeof(s[0])));
	}
	for (i = 0; i < n; i++) {
		sx += s[i].gyro_x;
		sy += s[i].gyro_y;
		sz += s[i].gyro_z;
	}
	if (n > 0) {
		*mean_gx = (float)(sx / n);
		*mean_gy = (float)(sy / n);
		*mean_gz = (float)(sz / n);
	}
	return n;
}

/* Per-frame motion estimate.  On success returns 0 and fills out_dx/out_dy
 * with the measured inter-frame shift (signed pixels, source-frame domain;
 * pan/recenter applied by the caller); non-zero on detector failure.
 *
 * IMU-gyro fusion seam: today the estimate is purely optical
 * (MI_IVE_Shift_Detector).  To add gyro-assisted stabilization, read the
 * frame-aligned angular rates via star6e_stab_gyro_window(prev_ts, curr_ts,
 * ...), integrate yaw/pitch over the interval into a pixel shift using the
 * lens focal length (focal_px = (src_w/2) / tan(hfov/2)), and fuse it with
 * the optical shift here (e.g. a complementary filter: gyro for fast jitter,
 * optical to cancel gyro drift).  Nothing else in the pipeline changes. */
static int star6e_stab_estimate_shift(StabIveImage_t *prev_img,
	StabIveImage_t *curr_img, StabIveImage_t *dx, StabIveImage_t *dy,
	int *out_dx, int *out_dy)
{
	int img_w = (int)curr_img->u16Width;
	int img_h = (int)curr_img->u16Height;
	int box = STAB_BOX_SIZE;
	int left, top;
	MI_S32 ret;

	if (box > img_w) box = img_w;
	if (box > img_h) box = img_h;
	left = ((img_w - box) / 2) & ~1;
	top  = ((img_h - box) / 2) & ~1;

	{
		StabIveShiftDetectCtrl_t ctrl = {
			.enMode = STAB_E_IVE_SHIFT_DETECT_MODE_SINGLE,
			.pyramid_level = STAB_PYRAMID,
			.search_range = STAB_SEARCH_RANGE,
			.u16Left = (MI_U16)left,
			.u16Top = (MI_U16)top,
			.u16Width = (MI_U16)box,
			.u16Height = (MI_U16)box,
		};
		ret = g_stab_ive_shift(g_stab_ive_handle,
			prev_img, curr_img, dx, dy, &ctrl, true);
	}
	if (ret != 0)
		return ret;

	g_stab_sys_flush_inv_cache(dx->apu8VirAddr[0], dx->azu16Stride[0]);
	g_stab_sys_flush_inv_cache(dy->apu8VirAddr[0], dy->azu16Stride[0]);
	*out_dx = STAB_SHIFT_SIGN_X * (int)((int8_t *)dx->apu8VirAddr[0])[0];
	*out_dy = STAB_SHIFT_SIGN_Y * (int)((int8_t *)dy->apu8VirAddr[0])[0];
	return 0;
}

static void *star6e_stab_thread_main(void *arg)
{
	MI_S32 ret;
	MI_S32 fd = -1;
	StabSysBufHandle_t prev_handle = 0;
	StabIveImage_t prev_img;
	int have_prev = 0;
	/* Shake-correction offset accumulator.  Held in float (facc_*) so the
	 * return-to-center decays exact-proportionally and both axes converge
	 * along the true diagonal; acc_* are the rounded ints handed to the crop
	 * and OSD each detect. */
	double facc_x = 0.0;
	double facc_y = 0.0;
	/* Final low-pass on the APPLIED offset.  The accumulator/recenter math
	 * can carry per-frame jitter (detector noise, 2-px crop quantization);
	 * applied raw it shows as judder, and the geometry magnifies it
	 * differently per preset (low: small motion vs quantization; high: large
	 * upscale) so only the mid preset looked smooth.  EMA-smoothing the
	 * offset before the crop equalises that — see STAB_OUTPUT_SMOOTH_ALPHA. */
	double smooth_x = 0.0;
	double smooth_y = 0.0;
	int acc_x = 0;
	int acc_y = 0;
	int dbg_frame = 0;
	int loop_n = 0;            /* drained frames since have_prev; gates detect */
	int still_frames = g_stab_still_frames_max;  /* recenter cooldown; start settled */
	StabIveImage_t dx;
	StabIveImage_t dy;
	struct timespec prev_ts;
	struct timespec curr_ts;

	(void)arg;
	memset(&prev_ts, 0, sizeof(prev_ts));
	memset(&prev_img, 0, sizeof(prev_img));
	memset(&dx, 0, sizeof(dx));
	memset(&dy, 0, sizeof(dy));

	if (star6e_stab_alloc_ive_image(&dx, 1, 1,
	    STAB_E_IVE_IMAGE_TYPE_S8C1) != 0 ||
	    star6e_stab_alloc_ive_image(&dy, 1, 1,
	    STAB_E_IVE_IMAGE_TYPE_S8C1) != 0) {
		fprintf(stderr, "[waybeam] ERROR: stab IVE result alloc failed — "
			"thread exiting, VENC ch0 will not receive frames\n");
		goto out;
	}

	if (g_stab_sys_get_fd)
		ret = g_stab_sys_get_fd(&g_stab_vpe_port, &fd);
	else
		ret = -1;
	if (ret != 0)
		fd = -1;

	while (g_stab_running) {
		StabSysBufInfo_t curr_buf;
		StabSysBufHandle_t curr_handle = 0;
		StabIveImage_t curr_img;
		int meas_dx = 0;
		int meas_dy = 0;
		float gyro_x = 0.0f;
		float gyro_y = 0.0f;
		float gyro_z = 0.0f;
		uint32_t gyro_n;

		memset(&curr_buf, 0, sizeof(curr_buf));

		/* Quiesce handshake: when teardown raises g_stab_pause, park here
		 * — not inside any MI_SYS GetBuf/PutBuf — so star6e_stab_stop can
		 * safely DisablePort(0,1) without racing the drain on the port. */
		if (g_stab_pause) {
			g_stab_parked = 1;
			usleep(2000);
			continue;
		}
		g_stab_parked = 0;

		if (fd >= 0) {
			fd_set rfds;
			struct timeval tv;

			FD_ZERO(&rfds);
			FD_SET(fd, &rfds);
			tv.tv_sec = 0;
			tv.tv_usec = 50000;
			ret = select(fd + 1, &rfds, NULL, NULL, &tv);
			if (ret <= 0 || !FD_ISSET(fd, &rfds))
				continue;
		}

		ret = g_stab_sys_out_get_buf(&g_stab_vpe_port,
			&curr_buf, &curr_handle);
		if (ret != 0) {
			if (fd < 0)
				usleep(1000);
			continue;
		}

		if (curr_buf.eBufType != STAB_E_BUFDATA_FRAME ||
		    !curr_buf.stFrameData.phyAddr[0] ||
		    !curr_buf.stFrameData.pVirAddr[0]) {
			g_stab_sys_out_put_buf(curr_handle);
			continue;
		}

		if (star6e_stab_make_center_y_crop(&curr_img, &curr_buf,
		    STAB_SHIFT_CROP_W, STAB_SHIFT_CROP_H) != 0) {
			g_stab_sys_out_put_buf(curr_handle);
			continue;
		}

		clock_gettime(CLOCK_MONOTONIC, &curr_ts);

		if (!have_prev) {
			/* VENC is hardware-fed from port0; the detector tap (port1)
			 * only seeds the reference frame here. */
			prev_handle = curr_handle;
			prev_img = curr_img;
			prev_ts = curr_ts;
			have_prev = 1;
			continue;
		}

		/* Estimate motion only every STAB_DETECT_EVERY-th frame — the
		 * detector is the CPU bottleneck.  prev_img/prev_handle are held
		 * across skipped frames, so each detect spans the full interval
		 * since the last detect (no motion lost).  The crop blit + VENC
		 * send below run on every frame regardless, keeping output at
		 * sensor fps. */
		loop_n++;
		if ((loop_n % STAB_DETECT_EVERY) == 0) {
			/* Gyro samples for this interval (frame-aligned, optical-only
			 * today — see star6e_stab_estimate_shift for the fusion seam). */
			gyro_n = star6e_stab_gyro_window(prev_ts, curr_ts,
				&gyro_x, &gyro_y, &gyro_z);

			ret = star6e_stab_estimate_shift(&prev_img, &curr_img, &dx, &dy,
				&meas_dx, &meas_dy);

			if (ret == 0) {
				int max_x;
				int max_y;

				facc_x += meas_dx;
				facc_y += meas_dy;
				max_x = star6e_stab_max_off_x();
				max_y = star6e_stab_max_off_y();
				if (facc_x < -max_x) facc_x = -max_x;
				if (facc_x >  max_x) facc_x =  max_x;
				if (facc_y < -max_y) facc_y = -max_y;
				if (facc_y >  max_y) facc_y =  max_y;

				/* Gated return-to-center.  Recenter when the camera has
				 * settled (still for STILL_FRAMES), or while moving once the
				 * offset has pushed past EDGE_PCT of the dead-border on
				 * either axis (reclaim margin near saturation) — leaving the
				 * central zone untouched so it doesn't fight live
				 * stabilization.  When recentering, decay the (acc_x, acc_y)
				 * VECTOR's magnitude by (tau-1)/tau with its direction held
				 * constant, so both axes shrink proportionally and reach
				 * center together along a straight diagonal.  (Per-axis decay
				 * truncates the shorter axis to zero first, leaving a visible
				 * axis-aligned tail.)  nmag<1 snaps both axes to 0 together. */
				if (g_stab_recenter_period > 0) {
					uint32_t tau = g_stab_recenter_period;
					int settled;
					int edge_x = (max_x * g_stab_edge_pct) / 100;
					int edge_y = (max_y * g_stab_edge_pct) / 100;
					int do_recenter;
					if (tau < 2) tau = 2;

					if (abs(meas_dx) > g_stab_motion_thresh ||
					    abs(meas_dy) > g_stab_motion_thresh)
						still_frames = 0;
					else if (still_frames < g_stab_still_frames_max)
						still_frames++;
					settled = (still_frames >= g_stab_still_frames_max);

					do_recenter = settled ||
						facc_x > edge_x || facc_x < -edge_x ||
						facc_y > edge_y || facc_y < -edge_y;
					if (do_recenter) {
						/* Scale both axes by the same factor → direction
						 * held, straight diagonal to center.  Float math
						 * keeps the proportion exact at sub-pixel
						 * magnitudes; snap both to 0 together once the
						 * whole vector is under half a pixel. */
						double scale = (double)(tau - 1) / (double)tau;
						facc_x *= scale;
						facc_y *= scale;
						if (fabs(facc_x) < 0.5 && fabs(facc_y) < 0.5) {
							facc_x = 0.0;
							facc_y = 0.0;
						}
					}
				}

				/* Low-pass the offset before it reaches the crop so
				 * per-frame jitter doesn't judder the output. */
				smooth_x += g_stab_smooth_alpha * (facc_x - smooth_x);
				smooth_y += g_stab_smooth_alpha * (facc_y - smooth_y);
				acc_x = (int)lround(smooth_x);
				acc_y = (int)lround(smooth_y);
				pthread_mutex_lock(&g_stab_lock);
				g_stab_off_x = acc_x;
				g_stab_off_y = acc_y;
				pthread_mutex_unlock(&g_stab_lock);
				dbg_frame++;
				if ((dbg_frame % 120) == 0)
					fprintf(stderr, "[waybeam] stab tick %d: meas=(%d,%d) "
						"acc=(%d,%d) max=(%d,%d) pan=(%d,%d) still=%d "
						"gyro_n=%u gyro=(%.3f,%.3f,%.3f)\n",
						dbg_frame, meas_dx, meas_dy,
						acc_x, acc_y, max_x, max_y,
						g_stab_pan_x_mil, g_stab_pan_y_mil, still_frames,
						gyro_n, gyro_x, gyro_y, gyro_z);
			} else {
				if ((dbg_frame++ % 120) == 0)
					fprintf(stderr, "[waybeam] stab Shift_Detector "
						"ret=0x%x\n", (unsigned)ret);
			}

			/* Emit the new offset by reprogramming port0's hardware crop
			 * (VENC is fed by the bind), then rotate prev to this frame. */
			star6e_stab_apply_port_crop(acc_x, acc_y);
			if (prev_handle)
				g_stab_sys_out_put_buf(prev_handle);
			prev_handle = curr_handle;
			prev_img = curr_img;
			prev_ts = curr_ts;
		} else {
			/* Skipped-detect frame: nothing to emit (port0 holds the last
			 * crop, VENC is hardware-fed).  Keep prev as the last detected
			 * frame so the next detect spans the interval. */
			g_stab_sys_out_put_buf(curr_handle);
		}
	}

	if (prev_handle)
		g_stab_sys_out_put_buf(prev_handle);
	if (fd >= 0 && g_stab_sys_close_fd)
		g_stab_sys_close_fd(fd);
out:
	free(dx.apu8VirAddr[0]);
	free(dy.apu8VirAddr[0]);
	return NULL;
}


/* Set up VPE output ports for stabilization (single HW-crop path).
 *
 * port0 outputs the encoded dim and is hardware-bound to VENC ch0; the
 * detector reads a tiny port1 256x256 centre tap and reprograms port0's
 * SetPortCrop per detect (no software blit; port0 tears down via the standard
 * bound-port path, state->bound_vpe_venc=1).  Sets g_stab_tap_active=1,
 * g_stab_vpe_port=port1.
 *
 * If this BSP rejects the port1 tap, port0 is still bound to VENC and held at
 * a static centre crop (g_stab_tap_active=0, no detector, no shake
 * compensation) — the stream stays up at the configured framing.  Returns 0 on
 * success, <0 only on a hard port0/bind failure the caller must abort on. */
static int star6e_stab_setup_ports(Star6ePipelineState *state,
	uint32_t bind_src_fps, uint32_t bind_dst_fps)
{
	MI_VPE_PortAttr_t port = {0};
	MI_SYS_ChnPort_t vpe0 = {
		.module = I6_SYS_MOD_VPE, .device = 0, .channel = 0, .port = 0 };
	MI_SYS_ChnPort_t vpe1 = {
		.module = I6_SYS_MOD_VPE, .device = 0, .channel = 0, .port = 1 };
	i6_common_rect rect;
	int port1_enabled = 0;
	int tap_ok;
	MI_S32 ret;

	g_stab_tap_active = 0;
	state->bound_vpe_venc = 0;

	/* VPE channel input (precrop) dim — the SetPortCrop coordinate domain.
	 * Set before any star6e_stab_apply_port_crop / img_to_pre call below. */
	g_stab_pre_w = state->active_precrop.w ? state->active_precrop.w
		: g_stab_src_w;
	g_stab_pre_h = state->active_precrop.h ? state->active_precrop.h
		: g_stab_src_h;

	/* port0: encoded-dim output (the SCL will crop the src into this). */
	port.output.width = g_stab_enc_w;
	port.output.height = g_stab_enc_h;
	port.pixFmt = I6_PIXFMT_YUV420SP;
	port.compress = I6_COMPR_NONE;
	ret = MI_VPE_SetPortMode(0, 0, &port);
	if (ret != 0) {
		fprintf(stderr, "[waybeam] ERROR: stab port0 SetPortMode %ux%u "
			"failed %d\n", g_stab_enc_w, g_stab_enc_h, (int)ret);
		return ret;   /* even the legacy path needs a working port0 */
	}
	ret = MI_VPE_EnablePort(0, 0);
	if (ret != 0) {
		fprintf(stderr, "[waybeam] ERROR: stab port0 EnablePort failed "
			"%d\n", (int)ret);
		return ret;
	}

	/* port1: 256x256 centre detector tap — the BSP-dependent step. */
	memset(&port, 0, sizeof(port));
	port.output.width = STAB_SHIFT_CROP_W;
	port.output.height = STAB_SHIFT_CROP_H;
	port.pixFmt = I6_PIXFMT_YUV420SP;
	port.compress = I6_COMPR_NONE;
	ret = MI_VPE_SetPortMode(0, 1, &port);
	if (ret == 0) {
		ret = MI_VPE_EnablePort(0, 1);
		if (ret == 0)
			port1_enabled = 1;
	}
	if (ret == 0) {
		/* Detector tap: crop a centre window of the precrop input that maps
		 * to a 256x256 IMAGE-domain patch, scaled to the 256x256 port1
		 * output.  The detector then measures motion in image pixels —
		 * identical to the legacy centre crop — so the accumulator and
		 * clamps stay mode-agnostic (image domain). */
		int dw = star6e_stab_img_to_pre_x(STAB_SHIFT_CROP_W) & ~1;
		int dh = star6e_stab_img_to_pre_y(STAB_SHIFT_CROP_H) & ~1;
		int dcx, dcy;
		if (dw < 2) dw = 2;
		if (dh < 2) dh = 2;
		if (dw > (int)g_stab_pre_w) dw = (int)g_stab_pre_w & ~1;
		if (dh > (int)g_stab_pre_h) dh = (int)g_stab_pre_h & ~1;
		dcx = (((int)g_stab_pre_w - dw) / 2) & ~1;
		dcy = (((int)g_stab_pre_h - dh) / 2) & ~1;
		if (dcx < 0) dcx = 0;
		if (dcy < 0) dcy = 0;
		rect.x = (unsigned short)dcx;
		rect.y = (unsigned short)dcy;
		rect.width = (unsigned short)dw;
		rect.height = (unsigned short)dh;
		ret = MI_VPE_SetPortCrop(0, 1, &rect);
	}
	tap_ok = (ret == 0 && port1_enabled);

	/* Bind port0 -> VENC zero-copy regardless of the tap: the stabilized (or,
	 * if the tap is unavailable, static centre) crop is fed by this bind. */
	ret = MI_SYS_BindChnPort2(&state->vpe_port, &state->venc_port,
		bind_src_fps, bind_dst_fps, I6_SYS_LINK_FRAMEBASE, 0);
	if (ret != 0) {
		fprintf(stderr, "[waybeam] ERROR: stab port0->VENC bind failed "
			"%d\n", (int)ret);
		if (port1_enabled)
			MI_VPE_DisablePort(0, 1);
		return ret;
	}
	MI_SYS_SetChnOutputPortDepth(&state->venc_port, 1, 3);
	state->bound_vpe_venc = 1;
	star6e_stab_apply_port_crop(0, 0);   /* centre the initial crop window */

	if (tap_ok) {
		MI_SYS_SetChnOutputPortDepth(&vpe1, 2, 4);
		g_stab_vpe_port = vpe1;          /* detector drains port1 */
		g_stab_tap_active = 1;
		fprintf(stderr, "[waybeam] stab: HW-crop mode (port0->VENC bind, "
			"port1 %dx%d detector tap)\n",
			STAB_SHIFT_CROP_W, STAB_SHIFT_CROP_H);
	} else {
		/* No detector tap on this BSP: keep the bound static centre crop,
		 * no shake compensation (never the legacy manual drain). */
		if (port1_enabled)
			MI_VPE_DisablePort(0, 1);
		g_stab_vpe_port = vpe0;
		g_stab_tap_active = 0;
		fprintf(stderr, "[waybeam] WARNING: stab port1 tap unavailable "
			"(%d); static centre crop, no stabilization\n", (int)ret);
	}
	return 0;
}

static int star6e_stab_start(void)
{
	MI_S32 ret;

	g_stab_pause = 0;
	g_stab_parked = 0;

	/* Degrade path: port0 is bound at a static centre crop, no port1 tap —
	 * run no detector thread. */
	if (!g_stab_tap_active) {
		fprintf(stderr, "[waybeam] stab: detector disabled "
			"(static centre crop, no shake compensation)\n");
		return 0;
	}

	if (star6e_stab_load_sys_extra_symbols() != 0) {
		fprintf(stderr, "[waybeam] ERROR: stab cannot resolve required "
			"MI_SYS symbols\n");
		return -1;
	}

	g_stab_ive_lib = dlopen("libmi_ive.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!g_stab_ive_lib)
		g_stab_ive_lib = dlopen("libive.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!g_stab_ive_lib) {
		fprintf(stderr, "[waybeam] ERROR: stab cannot dlopen "
			"libmi_ive.so / libive.so\n");
		return -1;
	}

	g_stab_ive_create = (stab_ive_create_fn_t)dlsym(g_stab_ive_lib,
		"MI_IVE_Create");
	g_stab_ive_destroy = (stab_ive_destroy_fn_t)dlsym(g_stab_ive_lib,
		"MI_IVE_Destroy");
	g_stab_ive_shift = (stab_ive_shift_fn_t)dlsym(g_stab_ive_lib,
		"MI_IVE_Shift_Detector");
	if (!g_stab_ive_create || !g_stab_ive_destroy || !g_stab_ive_shift) {
		fprintf(stderr, "[waybeam] ERROR: stab missing IVE symbols\n");
		dlclose(g_stab_ive_lib);
		g_stab_ive_lib = NULL;
		return -1;
	}

	g_stab_ive_handle = 0;
	ret = g_stab_ive_create(g_stab_ive_handle);
	if (ret != 0) {
		fprintf(stderr, "[waybeam] ERROR: MI_IVE_Create failed %d\n",
			(int)ret);
		dlclose(g_stab_ive_lib);
		g_stab_ive_lib = NULL;
		return ret;
	}
	g_stab_ive_created = 1;

	g_stab_running = 1;
	if (pthread_create(&g_stab_thread, NULL,
	    star6e_stab_thread_main, NULL) != 0) {
		g_stab_running = 0;
		g_stab_ive_destroy(g_stab_ive_handle);
		g_stab_ive_created = 0;
		dlclose(g_stab_ive_lib);
		g_stab_ive_lib = NULL;
		fprintf(stderr, "[waybeam] ERROR: stab thread spawn failed\n");
		return -1;
	}

	fprintf(stderr, "[waybeam] stab: src=%ux%u out=%ux%u crop=%u%% "
		"recenter=%u (0=stick) smooth=%.2f still=%d edge=%d%% thresh=%d\n",
		g_stab_src_w, g_stab_src_h,
		g_stab_enc_w, g_stab_enc_h, g_stab_crop_percent,
		g_stab_recenter_period, g_stab_smooth_alpha, g_stab_still_frames_max,
		g_stab_edge_pct, g_stab_motion_thresh);
	return 0;
}

static void star6e_stab_stop(void)
{
	if (g_stab_running) {
		/* Stop the detector thread and JOIN it before touching port1.
		 * In HW detect mode the thread keeps one port1 buffer checked out
		 * across iterations (prev_handle, the IVE reference frame); on loop
		 * exit it returns that buffer while the port is still enabled (safe)
		 * and can start no further IVE read.  Disabling port1 with the
		 * thread still alive — even "parked" — could free the ring under an
		 * in-flight IVE read: _MI_SYS_MMU_Callback Status=0x2 ClientId=0x15
		 * IsWrite=0 stormed the MMU into a hardware-watchdog reset.  The old
		 * 100ms park-spin was not a real barrier under the every-frame
		 * "high" detector load (it survived a few respawn cycles, then lost
		 * the race and reset the board).  pthread_join IS the barrier; only
		 * then disable the tap.  port0 stays bound feeding VENC throughout,
		 * so the VPE channel never backs up and there is no heavy manual
		 * drain to wedge [vpe0_P0_MAIN]. */
		g_stab_running = 0;
		pthread_join(g_stab_thread, NULL);
		memset(&g_stab_thread, 0, sizeof(g_stab_thread));
		g_stab_pause = 0;
		g_stab_parked = 0;
	}
	/* Disable the tap whether or not the detector thread ran (the start-fail
	 * path sets up the tap but never spawns the thread).  Always after the
	 * join above, never before — see the MMU-safety note. */
	if (g_stab_tap_active) {
		MI_VPE_DisablePort(0, 1);
		g_stab_tap_active = 0;
	}
	if (g_stab_ive_created) {
		g_stab_ive_destroy(g_stab_ive_handle);
		g_stab_ive_created = 0;
	}
	if (g_stab_ive_lib) {
		dlclose(g_stab_ive_lib);
		g_stab_ive_lib = NULL;
	}
}

static int star6e_stab_enabled(const VencConfig *vcfg)
{
	return vcfg && vcfg->video0.stab_crop_pct >= 50 &&
		vcfg->video0.stab_crop_pct <= 100;
}

/* Stabilization path: constrain the AE meter to the stabilized crop window.
 * The crop is taken from the VPE *output* (g_stab_src), not from precrop, so
 * the meter fraction is g_stab_enc / g_stab_src (= the crop %) regardless of
 * any sensor→stream downscale — unlike the zoom path's image_width/precrop.
 * Centre follows the live pan; the small per-frame stabilization offset is
 * intentionally not tracked (a few px; the meter window need not chase it). */
static void star6e_stab_apply_ae_crop(void)
{
	Star6eAeCropRect r;
	double fw, fh, x, y, rx, ry;

	if (g_stab_src_w == 0 || g_stab_src_h == 0)
		return;
	fw = (double)g_stab_enc_w / (double)g_stab_src_w;
	fh = (double)g_stab_enc_h / (double)g_stab_src_h;
	if (!isfinite(fw) || !isfinite(fh) ||
	    fw <= 0.0 || fw >= 1.0 || fh <= 0.0 || fh >= 1.0)
		return;  /* no crop → leave AE full-frame */

	x = (double)g_stab_pan_x_mil / 1000.0;
	y = (double)g_stab_pan_y_mil / 1000.0;
	rx = x - fw * 0.5;
	ry = y - fh * 0.5;
	if (rx < 0.0) rx = 0.0;
	if (ry < 0.0) ry = 0.0;
	if (rx + fw > 1.0) rx = 1.0 - fw;
	if (ry + fh > 1.0) ry = 1.0 - fh;

	memset(&r, 0, sizeof(r));
	r.crop_x = (uint16_t)(rx * 1023.0 + 0.5);
	r.crop_y = (uint16_t)(ry * 1023.0 + 0.5);
	r.crop_w = (uint16_t)(fw * 1023.0 + 0.5);
	r.crop_h = (uint16_t)(fh * 1023.0 + 0.5);
	if (r.crop_w == 0) r.crop_w = 1;
	if (r.crop_h == 0) r.crop_h = 1;
	if (r.crop_x + r.crop_w > 1023)
		r.crop_x = (uint16_t)(1023 - r.crop_w);
	if (r.crop_y + r.crop_h > 1023)
		r.crop_y = (uint16_t)(1023 - r.crop_h);

	star6e_emit_ae_crop(&r);
}

static int star6e_stab_active(void)
{
	return g_stab_running;
}

/* Compute stabilization geometry from vcfg and the source dims; clamp the
 * source to <=1920x1080 (above it the VPE+VENC path cannot sustain 60/90/120
 * fps with stab on) and return the encode dims the pipeline sizes VENC to.
 * Stabilization is always CENTERED (0.5/0.5) so the accumulator has symmetric
 * headroom — zoomX/zoomY are the zoom-mode pan and do not apply here. */
static int star6e_stab_prepare(const VencConfig *vcfg, uint32_t src_w,
	uint32_t src_h, uint32_t *enc_w, uint32_t *enc_h)
{
	if (src_w > 1920 || src_h > 1080) {
		uint64_t rw = (uint64_t)1920 * 1000 / src_w;
		uint64_t rh = (uint64_t)1080 * 1000 / src_h;
		uint64_t r = rw < rh ? rw : rh;
		uint32_t nw = (uint32_t)((uint64_t)src_w * r / 1000) & ~1u;
		uint32_t nh = (uint32_t)((uint64_t)src_h * r / 1000) & ~1u;
		if (nw < 2) nw = 2;
		if (nh < 2) nh = 2;
		fprintf(stderr, "[waybeam] WARNING: video0.framing '%s': source "
			"%ux%u exceeds 1920x1080; clamping to %ux%u to preserve fps\n",
			vcfg->video0.framing, src_w, src_h, nw, nh);
		src_w = nw;
		src_h = nh;
	}
	star6e_stab_configure(src_w, src_h,
		vcfg->video0.stab_crop_pct,
		vcfg->video0.stab_recenter_speed,
		vcfg->video0.fps,
		0.5, 0.5,
		vcfg->video0.stab_smooth_pct,
		vcfg->video0.stab_still_frames,
		vcfg->video0.stab_edge_pct,
		vcfg->video0.stab_motion_thresh);
	*enc_w = g_stab_enc_w;
	*enc_h = g_stab_enc_h;
	return 0;
}

const FramingModule star6e_framing_stab = {
	.preset_name   = "stab",
	.enabled       = star6e_stab_enabled,
	.prepare       = star6e_stab_prepare,
	.setup_ports   = star6e_stab_setup_ports,
	.start         = star6e_stab_start,
	.stop          = star6e_stab_stop,
	.apply_ae_crop = star6e_stab_apply_ae_crop,
	.set_pan       = star6e_stab_set_pan,
	.active        = star6e_stab_active,
	.set_live      = NULL,
};
