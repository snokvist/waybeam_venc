/* maruko_framing_stab.c — video0.framing="stab" for the Maruko (i6c) backend.
 *
 * The parallel of src/star6e_framing_stab.c's HW-crop "stab" path, ported to
 * the SigmaStar Infinity6C SCL pipeline.  Structurally SIMPLER than Star6E: the
 * SCL output-port crop and the detector tap both live in the SAME SCL-input
 * (ISP-output) domain, so there is no VPE precrop domain conversion — the
 * detector measures shake in SCL-input pixels and the emit applies it in the
 * same pixels.
 *
 * Data path (per frame, mirrors the Phase-1 bench_1d prototype, now with the
 * shared Kalman and real geometry):
 *   SCL port 2 (fixed 1:1 384x384 centre tap) --GetBuf--> MI_IVE_Shift_Detector
 *     --> framing_kalman_step --> shift the SCL port-0 crop window (SetPortConfig)
 *   port 0 stays bound to VENC via the ring the whole time (zero-copy).
 *
 * Geometry: src = the AR-precrop base surface (ctx->scl_crop_{x,y,w,h}); the
 * stab window is `pct`% of src (oversample margin = (src-win)/2 per axis),
 * scaled by the SCL to the fixed encode size (ctx->cfg.image_{width,height}).
 * The window's AR equals src's AR (keep_aspect precrop guarantees src AR ==
 * encode AR), so the SCL always does a uniform crop-then-scale — the only
 * stall-free shape on I6C.
 *
 * Teardown ordering is load-bearing (R6): pthread_join the detector BEFORE
 * disabling the tap port, else an in-flight IVE read races the freed ring and
 * storms _MI_SYS_MMU_Callback into a hardware-watchdog reset.  Never SIGKILL.
 */
#include "maruko_framing.h"
#include "maruko_framing_host.h"
#include "maruko_mi.h"          /* g_mi_scl */
#include "maruko_bindings.h"    /* i6c_scl_port */
#include "framing_kalman.h"
#include "venc_config.h"

#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#if HAVE_FRAMING_STAB

/* ── detector geometry (mirrors STAB_SHIFT_* in star6e_framing_stab.c) ─────── */
#define STAB_TAP_W        384
#define STAB_TAP_H        384
#define STAB_BOX          256
#define STAB_PYRAMID      3
#define STAB_SEARCH_RANGE 96
#define STAB_TAP_PORT     2
#define STAB_DEFAULT_PCT  90   /* fallback framing window when cfg is out of range */

/* ── MI_SYS buffer ABI (i6c; mirrors star6e_framing_stab.c:40-103) ─────────── */
typedef struct { MI_U16 x, y, w, h; } StabRect_t;
typedef struct {
	int eTileMode, ePixelFormat, eCompressMode, eFrameScanMode;
	int eFieldType, ePhylayoutType;
	MI_U16 u16Width, u16Height;
	void *pVirAddr[3];
	MI_U64 phyAddr[3];
	MI_U32 u32Stride[3];
	MI_U32 u32BufSize;
	MI_U16 u16RingStart, u16RingTotal;
	struct { int eType; union { MI_U32 g; } u; } stIsp;
	StabRect_t stCrop;
} StabFrame_t;
typedef struct {
	MI_U64 u64Pts, u64Sideband;
	int eBufType;
	MI_U8 bEos, bUsrBuf;
	MI_U32 u32Seq;
	MI_U8 bDrop;
	union { StabFrame_t stFrameData; MI_U8 pad[512]; };
	MI_U8 u8CusFlag;
} StabBufInfo_t;

/* ── IVE types (device-proven; from tools/ive_i6c_shift.c) ─────────────────── */
#define E_IVE_IMAGE_TYPE_U8C1 0x0
#define E_IVE_IMAGE_TYPE_S8C1 0x1
#define E_IVE_SHIFT_MODE_SINGLE 0x00
typedef struct {
	int eType;
	MI_U64 aphyPhyAddr[3];
	MI_U8 *apu8VirAddr[3];
	MI_U16 azu16Stride[3];
	MI_U16 u16Width, u16Height, u16Reserved;
} IveImage_t;
typedef struct {
	int enMode;
	MI_U8 pyramid_level, search_range;
	MI_U16 u16Left, u16Top, u16Width, u16Height;
} IveShiftCtrl_t;

/* ── vendor fn pointers ────────────────────────────────────────────────────── */
typedef MI_S32 (*fn_getfd)(MI_SYS_ChnPort_t *, MI_S32 *);
typedef MI_S32 (*fn_closefd)(MI_S32);
typedef MI_S32 (*fn_getbuf)(MI_SYS_ChnPort_t *, StabBufInfo_t *, MI_S32 *);
typedef MI_S32 (*fn_putbuf)(MI_S32);
typedef MI_S32 (*fn_setdepth)(MI_U16, MI_SYS_ChnPort_t *, MI_U32, MI_U32);
typedef MI_S32 (*fn_flush)(void *, MI_U32);
typedef MI_S32 (*fn_mma_alloc)(MI_U16, MI_U8 *, MI_U32, MI_U64 *);
typedef MI_S32 (*fn_mma_free)(MI_U16, MI_U64);
typedef MI_S32 (*fn_mmap)(MI_U64, MI_U32, void **, MI_U8);
typedef MI_S32 (*fn_munmap)(void *, MI_U32);
typedef MI_S32 (*fn_ive_create)(int);
typedef MI_S32 (*fn_ive_destroy)(int);
typedef MI_S32 (*fn_ive_shift)(int, IveImage_t *, IveImage_t *,
	IveImage_t *, IveImage_t *, IveShiftCtrl_t *, MI_U8);

static fn_getfd     SysGetFd;
static fn_closefd   SysCloseFd;
static fn_getbuf    SysGetBuf;
static fn_putbuf    SysPutBuf;
static fn_setdepth  SysSetDepth;
static fn_flush     SysFlush;
static fn_mma_alloc MmaAlloc;
static fn_mma_free  MmaFree;
static fn_mmap      SysMmap;
static fn_munmap    SysMunmap;
static fn_ive_create  IveCreate;
static fn_ive_destroy IveDestroy;
static fn_ive_shift   IveShift;
static void *g_sys_lib;
static void *g_ive_lib;

/* ── module state (mirrors the star6e stab globals; MUT_RESTART-scoped) ─────── */
static MarukoBackendContext *g_ctx;   /* borrowed; cleared on stop */
static FramingKalman g_kalman = {
	0.0, 0.0, 0.0, 0.0, 1.0, 1.0,
	FRAMING_KALMAN_Q_DEFAULT, FRAMING_KALMAN_R_DEFAULT
};
static uint32_t g_src_w, g_src_h;     /* AR-precrop base surface (SCL-input px) */
static uint32_t g_win_w, g_win_h;     /* stab framing window (< src, oversample) */
static uint32_t g_scl_crop_x, g_scl_crop_y;  /* base window offset in SCL input */
static uint32_t g_crop_pct;
static uint32_t g_recenter_period;
static double g_cfg_q = FRAMING_KALMAN_Q_DEFAULT;
static double g_cfg_r = FRAMING_KALMAN_R_DEFAULT;
static volatile int g_pan_x_mil = 500;
static volatile int g_pan_y_mil = 500;
static volatile int g_paused;

static pthread_t g_thread;
static volatile int g_running;
static volatile int g_tap_active;
static int g_ive_created;
static int g_ive_handle;

/* ── small helpers ─────────────────────────────────────────────────────────── */
static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
static int pan_clamp_mil(double v)
{
	int mil;
	if (!isfinite(v) || v <= 0.0) return 0;
	if (v >= 1.0) return 1000;
	mil = (int)(v * 1000.0 + 0.5);
	if (mil < 0) mil = 0;
	if (mil > 1000) mil = 1000;
	return mil;
}
static uint32_t stab_pct(const VencConfig *vcfg)
{
	uint32_t p = vcfg->video0.stab_crop_pct;
	if (p < 60 || p > 99) p = STAB_DEFAULT_PCT;
	return p;
}

/* ── emit: shift the SCL port-0 crop window (parallel of
 * star6e_stab_apply_port_crop, but the SCL crop is already SCL-input domain so
 * no precrop scaling is needed). acc is in SCL-input pixels. ───────────────── */
static void maruko_stab_apply_crop(int acc_x, int acc_y)
{
	MarukoBackendContext *ctx = g_ctx;
	int src_w, src_h, win_w, win_h, center_x, center_y, off_x, off_y, mx, my;
	i6c_scl_port p;

	if (!ctx) return;
	src_w = (int)g_src_w; src_h = (int)g_src_h;
	win_w = (int)g_win_w; win_h = (int)g_win_h;
	if (win_w <= 0 || win_h <= 0 || win_w > src_w || win_h > src_h) return;

	center_x = (int)(((int64_t)src_w * g_pan_x_mil) / 1000);
	center_y = (int)(((int64_t)src_h * g_pan_y_mil) / 1000);
	off_x = center_x - win_w / 2 + acc_x;
	off_y = center_y - win_h / 2 + acc_y;
	mx = src_w - win_w; my = src_h - win_h;
	if (off_x < 0) off_x = 0; if (off_x > mx) off_x = mx;
	if (off_y < 0) off_y = 0; if (off_y > my) off_y = my;
	off_x &= ~1; off_y &= ~1;   /* keep the 2-px-even assumption (1c open) */

	memset(&p, 0, sizeof(p));
	p.crop.x = (unsigned short)((int)g_scl_crop_x + off_x);
	p.crop.y = (unsigned short)((int)g_scl_crop_y + off_y);
	p.crop.width = (unsigned short)win_w;
	p.crop.height = (unsigned short)win_h;
	p.output.width = (unsigned short)ctx->cfg.image_width;
	p.output.height = (unsigned short)ctx->cfg.image_height;
	p.pixFmt = I6_PIXFMT_YUV420SP;
	p.compress = (i6_common_compr)6;   /* IFC — required for the VENC ring */
	(void)g_mi_scl.fnSetPortConfig(0, 0, 0, &p);
}

/* ── vtable: enabled / prepare ─────────────────────────────────────────────── */
static int maruko_stab_enabled(const VencConfig *vcfg)
{
	return strcmp(vcfg->video0.framing, "stab") == 0;
}

/* On Maruko the encode size is fixed (video0.size); stab does NOT resize VENC —
 * it crops a `pct` window of the SCL base and the SCL scales it up to the
 * encode size.  So prepare() leaves enc dims unchanged and just latches the
 * tuning; the real geometry is computed in setup_ports once scl_crop is known. */
static int maruko_stab_prepare(const VencConfig *vcfg, uint32_t src_w,
	uint32_t src_h, uint32_t *enc_w, uint32_t *enc_h)
{
	(void)src_w; (void)src_h;
	g_crop_pct = stab_pct(vcfg);
	g_recenter_period = vcfg->video0.stab_recenter_speed;
	g_cfg_q = vcfg->video0.stab_kalman_q;
	g_cfg_r = vcfg->video0.stab_kalman_r;
	g_pan_x_mil = 500;
	g_pan_y_mil = 500;
	g_paused = 0;
	/* enc dims unchanged: the SCL scales the crop window up to the configured
	 * encode size, so VENC stays at video0.width x video0.height. */
	if (enc_w) *enc_w = vcfg->video0.width;
	if (enc_h) *enc_h = vcfg->video0.height;
	return 0;
}

/* ── symbol loading (libmi_sys already mapped by the main process) ─────────── */
static int load_syms(void)
{
	if (SysGetBuf && IveShift) return 0;
	g_sys_lib = dlopen("libmi_sys.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!g_sys_lib) {
		fprintf(stderr, "[maruko-stab] dlopen libmi_sys: %s\n", dlerror());
		return -1;
	}
	SysGetFd   = (fn_getfd)   dlsym(g_sys_lib, "MI_SYS_GetFd");
	SysCloseFd = (fn_closefd) dlsym(g_sys_lib, "MI_SYS_CloseFd");
	SysGetBuf  = (fn_getbuf)  dlsym(g_sys_lib, "MI_SYS_ChnOutputPortGetBuf");
	SysPutBuf  = (fn_putbuf)  dlsym(g_sys_lib, "MI_SYS_ChnOutputPortPutBuf");
	SysSetDepth = (fn_setdepth) dlsym(g_sys_lib, "MI_SYS_SetChnOutputPortDepth");
	SysFlush   = (fn_flush)   dlsym(g_sys_lib, "MI_SYS_FlushInvCache");
	MmaAlloc   = (fn_mma_alloc) dlsym(g_sys_lib, "MI_SYS_MMA_Alloc");
	MmaFree    = (fn_mma_free)  dlsym(g_sys_lib, "MI_SYS_MMA_Free");
	SysMmap    = (fn_mmap)    dlsym(g_sys_lib, "MI_SYS_Mmap");
	SysMunmap  = (fn_munmap)  dlsym(g_sys_lib, "MI_SYS_Munmap");
	if (!SysGetBuf || !SysPutBuf || !SysFlush || !SysSetDepth ||
	    !MmaAlloc || !SysMmap) {
		fprintf(stderr, "[maruko-stab] missing MI_SYS symbols\n");
		return -1;
	}
	g_ive_lib = dlopen("libmi_ive.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!g_ive_lib) {
		fprintf(stderr, "[maruko-stab] dlopen libmi_ive: %s\n", dlerror());
		return -1;
	}
	IveCreate  = (fn_ive_create)  dlsym(g_ive_lib, "MI_IVE_Create");
	IveDestroy = (fn_ive_destroy) dlsym(g_ive_lib, "MI_IVE_Destroy");
	IveShift   = (fn_ive_shift)   dlsym(g_ive_lib, "MI_IVE_Shift_Detector");
	if (!IveCreate || !IveDestroy || !IveShift) {
		fprintf(stderr, "[maruko-stab] missing MI_IVE symbols\n");
		return -1;
	}
	return 0;
}

static int ive_alloc(IveImage_t *img, MI_U16 w, MI_U16 h, int type)
{
	MI_U32 stride = (w + 63u) & ~63u;
	MI_U32 size = stride * h;
	MI_U64 phy = 0; void *vir = NULL;
	memset(img, 0, sizeof(*img));
	if (MmaAlloc(0, NULL, size, &phy) != 0 || !phy) {
		if (MmaAlloc(0, (MI_U8 *)"mma_heap_name0", size, &phy) != 0 || !phy)
			return -1;
	}
	if (SysMmap(phy, size, &vir, 0) != 0 || !vir) { MmaFree(0, phy); return -1; }
	memset(vir, 0, size);
	img->eType = type; img->u16Width = w; img->u16Height = h;
	img->azu16Stride[0] = (MI_U16)stride;
	img->aphyPhyAddr[0] = phy; img->apu8VirAddr[0] = (MI_U8 *)vir;
	return 0;
}
static void ive_free(IveImage_t *img)
{
	MI_U32 size = (MI_U32)img->azu16Stride[0] * img->u16Height;
	if (img->apu8VirAddr[0] && SysMunmap) SysMunmap(img->apu8VirAddr[0], size);
	if (img->aphyPhyAddr[0] && MmaFree) MmaFree(0, img->aphyPhyAddr[0]);
	memset(img, 0, sizeof(*img));
}

/* ── the detector tap: fixed 1:1 384x384 window at the centre of the base
 * surface (compress=0 so the Y plane is CPU-readable).  1:1 means the shift the
 * detector measures is already in SCL-input pixels — no scaling to the emit. ─ */
static int stab_tap_enable(void)
{
	i6c_scl_port p;
	MI_SYS_ChnPort_t bind;
	int cx, cy;
	MI_S32 ret;

	cx = ((int)g_src_w - STAB_TAP_W) / 2;
	cy = ((int)g_src_h - STAB_TAP_H) / 2;
	if (cx < 0) cx = 0;
	if (cy < 0) cy = 0;
	cx &= ~1; cy &= ~1;

	memset(&p, 0, sizeof(p));
	p.crop.x = (unsigned short)((int)g_scl_crop_x + cx);
	p.crop.y = (unsigned short)((int)g_scl_crop_y + cy);
	p.crop.width = STAB_TAP_W;
	p.crop.height = STAB_TAP_H;
	p.output.width = STAB_TAP_W;
	p.output.height = STAB_TAP_H;
	p.pixFmt = I6_PIXFMT_YUV420SP;
	p.compress = (i6_common_compr)0;   /* RAW — CPU reads the Y plane */
	ret = g_mi_scl.fnSetPortConfig(0, 0, STAB_TAP_PORT, &p);
	if (ret != 0) {
		fprintf(stderr, "[maruko-stab] tap SetPortConfig -> %d\n", (int)ret);
		return -1;
	}
	ret = g_mi_scl.fnEnablePort(0, 0, STAB_TAP_PORT);
	if (ret != 0) {
		fprintf(stderr, "[maruko-stab] tap EnablePort -> %d\n", (int)ret);
		return -1;
	}
	/* Unbound out-port needs a user-frame queue or GetBuf sees 0 frames
	 * (Phase-1 1a finding). */
	memset(&bind, 0, sizeof(bind));
	bind.module = I6_SYS_MOD_SCL; bind.port = STAB_TAP_PORT;
	ret = SysSetDepth(0, &bind, 2, 4);
	if (ret != 0)
		fprintf(stderr, "[maruko-stab] tap SetChnOutputPortDepth -> %d\n",
			(int)ret);
	return 0;
}
static void stab_tap_disable(void)
{
	(void)g_mi_scl.fnDisablePort(0, 0, STAB_TAP_PORT);
}

/* ── vtable: setup_ports — compute geometry (scl_crop now known), enable tap ─ */
static int maruko_stab_setup_ports(MarukoBackendContext *ctx,
	uint32_t src_fps, uint32_t dst_fps)
{
	(void)src_fps; (void)dst_fps;
	if (!ctx) return -1;
	/* Resolve MI_SYS/IVE symbols before the tap setup — stab_tap_enable needs
	 * SysSetDepth, and the detector thread needs the rest. Idempotent. */
	if (load_syms() != 0)
		return -1;
	g_ctx = ctx;
	g_scl_crop_x = ctx->scl_crop_x;
	g_scl_crop_y = ctx->scl_crop_y;
	g_src_w = ctx->scl_crop_w;
	g_src_h = ctx->scl_crop_h;
	if (g_src_w == 0 || g_src_h == 0) {
		fprintf(stderr, "[maruko-stab] no scl_crop base — stab off\n");
		g_ctx = NULL;
		return -1;
	}
	if (g_src_w < STAB_TAP_W + 2 || g_src_h < STAB_TAP_H + 2) {
		fprintf(stderr, "[maruko-stab] base %ux%u too small for %dx%d tap\n",
			g_src_w, g_src_h, STAB_TAP_W, STAB_TAP_H);
		g_ctx = NULL;
		return -1;
	}
	/* Stab framing window: pct of the base, 2-aligned, AR == base AR (== encode
	 * AR under keep_aspect), so the SCL always crop-then-uniform-scales. */
	g_win_w = ((g_src_w * g_crop_pct) / 100u) & ~1u;
	g_win_h = ((g_src_h * g_crop_pct) / 100u) & ~1u;
	if (g_win_w >= g_src_w) g_win_w = (g_src_w - 2u) & ~1u;
	if (g_win_h >= g_src_h) g_win_h = (g_src_h - 2u) & ~1u;

	/* R2: stab is the sole writer of the SCL port-0 crop — stop the pan ramp. */
	maruko_framing_pan_ramp_stop();

	if (stab_tap_enable() != 0) {
		g_tap_active = 0;
		g_ctx = NULL;
		return -1;
	}
	g_tap_active = 1;
	/* Centre the initial stab window (acc=0). */
	framing_kalman_reset(&g_kalman, g_cfg_q, g_cfg_r);
	maruko_stab_apply_crop(0, 0);
	fprintf(stderr, "[maruko-stab] base %ux%u @(%u,%u) win %ux%u (pct=%u) "
		"tap %dx%d\n", g_src_w, g_src_h, g_scl_crop_x, g_scl_crop_y,
		g_win_w, g_win_h, g_crop_pct, STAB_TAP_W, STAB_TAP_H);
	return 0;
}

/* ── detector thread ───────────────────────────────────────────────────────── */
static IveImage_t g_a, g_b, g_dx, g_dy;

static void *maruko_stab_thread_main(void *arg)
{
	MI_SYS_ChnPort_t bind;
	MI_S32 fd = -1;
	int have_prev = 0, cur = 0, dbg = 0;
	int max_x = (int)(g_src_w - g_win_w) / 2;
	int max_y = (int)(g_src_h - g_win_h) / 2;
	int left = ((STAB_TAP_W - STAB_BOX) / 2) & ~1;
	int top  = ((STAB_TAP_H - STAB_BOX) / 2) & ~1;
	IveShiftCtrl_t ctrl = {
		.enMode = E_IVE_SHIFT_MODE_SINGLE,
		.pyramid_level = STAB_PYRAMID, .search_range = STAB_SEARCH_RANGE,
		.u16Left = (MI_U16)left, .u16Top = (MI_U16)top,
		.u16Width = STAB_BOX, .u16Height = STAB_BOX,
	};
	(void)arg;

	memset(&bind, 0, sizeof(bind));
	bind.module = I6_SYS_MOD_SCL; bind.port = STAB_TAP_PORT;
	if (SysGetFd) (void)SysGetFd(&bind, &fd);

	while (g_running) {
		StabBufInfo_t buf;
		MI_S32 h = -1;
		IveImage_t *prev = (cur & 1) ? &g_b : &g_a;
		IveImage_t *curr = (cur & 1) ? &g_a : &g_b;
		MI_U8 *y; MI_U32 stride;

		if (fd >= 0) {
			fd_set rf; struct timeval tv = {0, 100000};
			FD_ZERO(&rf); FD_SET(fd, &rf);
			if (select(fd + 1, &rf, NULL, NULL, &tv) <= 0) continue;
		}
		memset(&buf, 0, sizeof(buf));
		if (SysGetBuf(&bind, &buf, &h) != 0) {
			if (fd < 0) usleep(1000);
			continue;
		}
		y = (MI_U8 *)buf.stFrameData.pVirAddr[0];
		stride = buf.stFrameData.u32Stride[0];
		if (y && stride >= STAB_TAP_W) {
			for (int r = 0; r < STAB_TAP_H; r++)
				memcpy(curr->apu8VirAddr[0] + (size_t)r * curr->azu16Stride[0],
				       y + (size_t)r * stride, STAB_TAP_W);
			SysFlush(curr->apu8VirAddr[0],
				(MI_U32)curr->azu16Stride[0] * STAB_TAP_H);
		}
		SysPutBuf(h);

		if (!have_prev) { have_prev = 1; cur++; continue; }

		{
			MI_S32 r = IveShift(g_ive_handle, prev, curr, &g_dx, &g_dy, &ctrl, 1);
			if (r == 0) {
				int acc_x = 0, acc_y = 0;
				double meas_dx, meas_dy;
				uint32_t tau;
				SysFlush(g_dx.apu8VirAddr[0], g_dx.azu16Stride[0]);
				SysFlush(g_dy.apu8VirAddr[0], g_dy.azu16Stride[0]);
				/* STAB_SHIFT_SIGN_{X,Y} = -1 (star6e_framing_stab.c). */
				meas_dx = -(double)((signed char *)g_dx.apu8VirAddr[0])[0];
				meas_dy = -(double)((signed char *)g_dy.apu8VirAddr[0])[0];
				tau = g_recenter_period ? g_recenter_period : 30;
				framing_kalman_step(&g_kalman, meas_dx, meas_dy,
					g_paused, tau, max_x, max_y, &acc_x, &acc_y);
				maruko_stab_apply_crop(acc_x, acc_y);
				if ((++dbg % 120) == 0)
					fprintf(stderr, "[maruko-stab] meas=(%.0f,%.0f) "
						"acc=(%d,%d) max=(%d,%d) pan=(%d,%d) "
						"q=%.4f r=%.2f paused=%d\n",
						meas_dx, meas_dy, acc_x, acc_y, max_x, max_y,
						g_pan_x_mil, g_pan_y_mil, g_kalman.q,
						g_kalman.r, g_paused);
			}
		}
		cur++;
	}
	if (fd >= 0 && SysCloseFd) SysCloseFd(fd);
	return NULL;
}

/* ── vtable: start / stop ──────────────────────────────────────────────────── */
static int maruko_stab_start(void)
{
	if (!g_tap_active) {
		fprintf(stderr, "[maruko-stab] no tap — detector disabled\n");
		return 0;
	}
	if (load_syms() != 0)
		return -1;
	g_ive_handle = 0;
	if (IveCreate(g_ive_handle) != 0) {
		fprintf(stderr, "[maruko-stab] MI_IVE_Create failed\n");
		return -1;
	}
	g_ive_created = 1;
	if (ive_alloc(&g_a, STAB_TAP_W, STAB_TAP_H, E_IVE_IMAGE_TYPE_U8C1) ||
	    ive_alloc(&g_b, STAB_TAP_W, STAB_TAP_H, E_IVE_IMAGE_TYPE_U8C1) ||
	    ive_alloc(&g_dx, 1, 1, E_IVE_IMAGE_TYPE_S8C1) ||
	    ive_alloc(&g_dy, 1, 1, E_IVE_IMAGE_TYPE_S8C1)) {
		fprintf(stderr, "[maruko-stab] IVE image alloc failed\n");
		ive_free(&g_a); ive_free(&g_b); ive_free(&g_dx); ive_free(&g_dy);
		IveDestroy(g_ive_handle); g_ive_created = 0;
		return -1;
	}
	g_running = 1;
	if (pthread_create(&g_thread, NULL, maruko_stab_thread_main, NULL) != 0) {
		g_running = 0;
		ive_free(&g_a); ive_free(&g_b); ive_free(&g_dx); ive_free(&g_dy);
		IveDestroy(g_ive_handle); g_ive_created = 0;
		fprintf(stderr, "[maruko-stab] thread spawn failed\n");
		return -1;
	}
	fprintf(stderr, "[maruko-stab] detector running (win %ux%u, max_off "
		"%d/%d)\n", g_win_w, g_win_h, (int)(g_src_w - g_win_w) / 2,
		(int)(g_src_h - g_win_h) / 2);
	return 0;
}

static void maruko_stab_stop(void)
{
	/* R6: JOIN the detector BEFORE disabling the tap port.  The thread holds
	 * no port buffer across iterations (PutBuf every frame), but joining first
	 * still guarantees no in-flight IVE read races the DisablePort — the same
	 * _MI_SYS_MMU_Callback storm that resets Star6E.  Never SIGKILL. */
	if (g_running) {
		g_running = 0;
		pthread_join(g_thread, NULL);
		memset(&g_thread, 0, sizeof(g_thread));
	}
	if (g_tap_active) {
		stab_tap_disable();
		g_tap_active = 0;
	}
	ive_free(&g_a); ive_free(&g_b); ive_free(&g_dx); ive_free(&g_dy);
	if (g_ive_created) {
		IveDestroy(g_ive_handle);
		g_ive_created = 0;
	}
	g_ctx = NULL;
}

/* ── vtable: apply_ae_crop / set_pan / active / set_live ───────────────────── */
static void maruko_stab_apply_ae_crop(void)
{
	if (!g_ctx || g_src_w == 0 || g_src_h == 0) return;
	/* pct = window/base fraction; keep the AE meter on the stab crop. */
	maruko_framing_apply_ae_crop(g_ctx,
		(double)g_win_w / (double)g_src_w,
		(double)g_pan_x_mil / 1000.0,
		(double)g_pan_y_mil / 1000.0);
}
static void maruko_stab_set_pan(double x, double y)
{
	g_pan_x_mil = pan_clamp_mil(x);
	g_pan_y_mil = pan_clamp_mil(y);
	maruko_stab_apply_ae_crop();
}
static int maruko_stab_active(void)
{
	return g_running;
}
static int maruko_stab_set_live(const char *key, const char *val)
{
	if (!key) return -1;
	if (strcmp(key, "pause") == 0 || strcmp(key, "pauseStab") == 0 ||
	    strcmp(key, "video0.pause_stab") == 0) {
		g_paused = val && (val[0] == '1' || val[0] == 't' || val[0] == 'T' ||
			val[0] == 'y' || val[0] == 'Y');
		return 0;
	}
	return -1;
}

const MarukoFramingModule maruko_framing_stab = {
	.preset_name   = "stab",
	.enabled       = maruko_stab_enabled,
	.prepare       = maruko_stab_prepare,
	.setup_ports   = maruko_stab_setup_ports,
	.start         = maruko_stab_start,
	.stop          = maruko_stab_stop,
	.apply_ae_crop = maruko_stab_apply_ae_crop,
	.set_pan       = maruko_stab_set_pan,
	.active        = maruko_stab_active,
	.set_live      = maruko_stab_set_live,
};

#endif /* HAVE_FRAMING_STAB */
