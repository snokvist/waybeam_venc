/* In-process PiP architecture probe.
 *
 * Runs once at startup when pip.enabled=true.  Uses the already-running
 * sensor + VIF + VPE channel (no separate bring-up); enables VPE port-1
 * with the configured zoom crop, drains one frame via
 * MI_SYS_ChnOutputPortGetBuf, runs MI_DIVP_StretchBuf with that live
 * phys addr as source into a scratch MMA buffer, and reports JSON to
 * stderr.  Also tries GetBuf on port-0 (REALTIME-bound to VENC) to
 * confirm whether direct in-place compositing into the bound buffer is
 * possible.
 *
 * Output line tagged "probe":"pip_inproc". */

#include "venc_config.h"
/* star6e.h is force-included via -include ssc338q_compat.h, providing
 * MI_S32, MI_U*, MI_BOOL, MI_SYS_ChnPort_t (= i6_sys_bind),
 * MI_VPE_PortAttr_t (= i6_vpe_port), and the MI_VPE_* call wrappers. */

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ── Vendored types not exposed by star6e.h ────────────────────────── */

typedef unsigned long long MI_PHY;
typedef MI_S32             MI_SYS_BUF_HANDLE;

#define MI_SUCCESS 0

/* SDK MI_SYS_WindowRect_t — same layout as i6_common_rect (4× u16). */
typedef struct {
	uint16_t u16X, u16Y, u16Width, u16Height;
} MI_SYS_WindowRect_t;

/* Subset of MI_SYS_FrameData_t — only the fields we read. */
typedef struct {
	int      eTileMode;
	int      ePixelFormat;
	int      eCompressMode;
	int      eFrameScanMode;
	int      eFieldType;
	int      ePhylayoutType;
	uint16_t u16Width;
	uint16_t u16Height;
	void    *pVirAddr[3];
	MI_PHY   phyAddr[3];
	MI_U32   u32Stride[3];
	MI_U32   u32BufSize;
	uint8_t  _trailing[256];
} MI_SYS_FrameData_subset_t;

typedef struct {
	MI_U64           u64Pts;
	MI_U64           u64SidebandMsg;
	int              eBufType;
	MI_BOOL          bEndOfStream;
	MI_BOOL          bUsrBuf;
	MI_U32           u32SequenceNumber;
	MI_BOOL          bDrop;
	union {
		MI_SYS_FrameData_subset_t stFrameData;
		uint8_t _pad[768];
	};
} MI_SYS_BufInfo_t;

#pragma pack(push, 4)
typedef struct {
	int    ePixelFormat;
	MI_U32 u32Width;
	MI_U32 u32Height;
	MI_U32 u32Stride[3];
	MI_PHY phyAddr[3];
} MI_DIVP_DirectBuf_t;
#pragma pack(pop)

/* DIVP enum index: YUV420SP = 11, ARGB8888 = 1.  Same numeric value as
 * I6_PIXFMT_YUV420SP because both enums are SDK-shared. */
#define DIVP_PIXFMT_YUV420SP 11
#define DIVP_PIXFMT_ARGB8888 1

/* MI_RGN types — duplicated from src/debug_osd.c (SDK header is not on
 * the include path).  Layouts match the SigmaStar vendor SDK. */
typedef enum {
	I6_RGN_PIXFMT_ARGB1555_p = 0,
	I6_RGN_PIXFMT_ARGB4444_p,
	I6_RGN_PIXFMT_I2_p,
	I6_RGN_PIXFMT_I4_p,
	I6_RGN_PIXFMT_I8_p,
	I6_RGN_PIXFMT_RGB565_p,
	I6_RGN_PIXFMT_ARGB888_p,
} probe_rgn_pixfmt;

typedef struct { unsigned int width, height; } probe_rgn_size;

typedef struct {
	int           type;       /* I6_RGN_TYPE_OSD = 0 */
	probe_rgn_pixfmt pixFmt;
	probe_rgn_size   size;
} probe_rgn_cnf;

typedef struct {
	int           invColOn;
	int           lowThanThresh;
	unsigned int  lumThresh;
	unsigned short divWidth, divHeight;
} probe_rgn_inv;

typedef struct {
	unsigned int  layer;
	int           constAlphaOn;
	union {
		unsigned char bgFgAlpha[2];
		unsigned char constAlpha[2];
	};
	probe_rgn_inv invert;
} probe_rgn_osd;

typedef struct {
	int show;
	struct { unsigned int x, y; } point;
	union {
		uint8_t _pad_for_cov[64];
		probe_rgn_osd osd;
	};
} probe_rgn_chn;

/* CanvasInfo ABI — matches MI_RGN_CanvasInfo_t (verified against
 * debug_osd: sizeof=32, stride at offset 20). */
typedef struct {
	uint64_t      phyAddr;
	unsigned long virtAddr;
	struct { uint32_t u32Width, u32Height; } stSize;
	uint32_t      u32Stride;
	int           ePixelFmt;
} probe_rgn_canvas;

/* Use a separate handle from debug_osd (which uses 0). */
#define PROBE_RGN_HANDLE 1

/* ── dlsym handles for symbols not in star6e.h ──────────────────────── */

static MI_S32 (*p_chn_get)(MI_SYS_ChnPort_t *port, MI_SYS_BufInfo_t *buf,
	MI_SYS_BUF_HANDLE *handle);
static MI_S32 (*p_chn_put)(MI_SYS_BUF_HANDLE handle);
static MI_S32 (*p_mma_alloc)(MI_U8 *name, MI_U32 size, MI_PHY *phy);
static MI_S32 (*p_mma_free)(MI_PHY phy);
static MI_S32 (*p_sys_mmap)(MI_U64 phy, MI_U32 size, void **vaddr, MI_BOOL bcache);
static MI_S32 (*p_sys_munmap)(void *vaddr, MI_U32 size);
static MI_S32 (*p_sys_set_chn_depth)(MI_SYS_ChnPort_t *port,
	MI_U32 user_depth, MI_U32 buf_queue_depth);
static MI_S32 (*p_divp_stretch)(MI_DIVP_DirectBuf_t *src,
	MI_SYS_WindowRect_t *crop, MI_DIVP_DirectBuf_t *dst);

/* MI_RGN entry points (subset — phase 2 reuses debug OSD's already-init
 * state if present, else does its own init).  Cast pal arg to void to
 * avoid pulling the palette type. */
static MI_S32 (*p_rgn_init)(void *pal);
static MI_S32 (*p_rgn_create)(int handle, probe_rgn_cnf *cnf);
static MI_S32 (*p_rgn_destroy)(int handle);
static MI_S32 (*p_rgn_attach)(int handle, MI_SYS_ChnPort_t *chn,
	probe_rgn_chn *cfg);
static MI_S32 (*p_rgn_detach)(int handle, MI_SYS_ChnPort_t *chn);
static MI_S32 (*p_rgn_get_canvas)(int handle, probe_rgn_canvas *out);
static MI_S32 (*p_rgn_update_canvas)(int handle);

static int load_extra_syms(void)
{
	void *sys = dlopen("libmi_sys.so", RTLD_LAZY);
	void *divp = dlopen("libmi_divp.so", RTLD_LAZY);
	void *rgn = dlopen("libmi_rgn.so", RTLD_LAZY);
	if (!sys || !divp || !rgn) return -1;
	p_chn_get      = (void*)dlsym(sys,  "MI_SYS_ChnOutputPortGetBuf");
	p_chn_put      = (void*)dlsym(sys,  "MI_SYS_ChnOutputPortPutBuf");
	p_mma_alloc    = (void*)dlsym(sys,  "MI_SYS_MMA_Alloc");
	p_mma_free     = (void*)dlsym(sys,  "MI_SYS_MMA_Free");
	p_sys_mmap     = (void*)dlsym(sys,  "MI_SYS_Mmap");
	p_sys_munmap   = (void*)dlsym(sys,  "MI_SYS_Munmap");
	p_sys_set_chn_depth = (void*)dlsym(sys,
		"MI_SYS_SetChnOutputPortDepth");
	p_divp_stretch = (void*)dlsym(divp, "MI_DIVP_StretchBuf");
	p_rgn_init     = (void*)dlsym(rgn,  "MI_RGN_Init");
	p_rgn_create   = (void*)dlsym(rgn,  "MI_RGN_Create");
	p_rgn_destroy  = (void*)dlsym(rgn,  "MI_RGN_Destroy");
	p_rgn_attach   = (void*)dlsym(rgn,  "MI_RGN_AttachToChn");
	p_rgn_detach   = (void*)dlsym(rgn,  "MI_RGN_DetachFromChn");
	p_rgn_get_canvas    = (void*)dlsym(rgn, "MI_RGN_GetCanvasInfo");
	p_rgn_update_canvas = (void*)dlsym(rgn, "MI_RGN_UpdateCanvas");
	return (p_chn_get && p_chn_put && p_mma_alloc && p_mma_free &&
	        p_sys_mmap && p_sys_munmap && p_divp_stretch &&
	        p_rgn_create && p_rgn_destroy && p_rgn_attach &&
	        p_rgn_detach && p_rgn_get_canvas &&
	        p_rgn_update_canvas) ? 0 : -1;
}

#define ALIGN_UP(x, n) (((x) + ((n)-1)) & ~((n)-1))

/* ── Probe entrypoint ─────────────────────────────────────────────── */

void pip_probe_inproc_run(const VencConfig *vcfg)
{
	if (!vcfg || !vcfg->pip.enabled) return;
	if (!vcfg->pip.zoom.w || !vcfg->pip.zoom.h ||
	    !vcfg->pip.position.w || !vcfg->pip.position.h) {
		fprintf(stderr, "{\"probe\":\"pip_inproc\","
			"\"err\":\"zoom or position rect has zero dim\"}\n");
		fflush(stderr);
		return;
	}
	if (load_extra_syms() != 0) {
		fprintf(stderr, "{\"probe\":\"pip_inproc\","
			"\"err\":\"dlsym failed\"}\n");
		fflush(stderr);
		return;
	}

	const MI_U32 dst_w = vcfg->pip.position.w;
	const MI_U32 dst_h = vcfg->pip.position.h;
	const MI_U32 dst_stride = ALIGN_UP(dst_w, 16);
	const MI_U32 dst_size = dst_stride * dst_h * 3 / 2;

	/* Use the real MI_VPE_PortAttr_t layout — i6_vpe_port from
	 * include/sigmastar_types.h, brought in via star6e.h. */
	MI_VPE_PortAttr_t port = {0};
	port.output.width  = (uint16_t)dst_w;
	port.output.height = (uint16_t)dst_h;
	port.mirror = 0;
	port.flip = 0;
	port.pixFmt = I6_PIXFMT_YUV420SP;
	port.compress = I6_COMPR_NONE;
	MI_S32 set_mode_ret = MI_VPE_SetPortMode(0, 1, &port);
	if (set_mode_ret != 0) {
		fprintf(stderr, "{\"probe\":\"pip_inproc\","
			"\"err\":\"VPE_SetPortMode(0,1)\","
			"\"code\":\"0x%x\"}\n",
			(unsigned)set_mode_ret);
		fflush(stderr);
		return;
	}

	/* SetPortCrop is wrapped over an MI_SYS_WindowRect_t* in star6e.h. */
	MI_SYS_WindowRect_t crop = {
		.u16X = vcfg->pip.zoom.x, .u16Y = vcfg->pip.zoom.y,
		.u16Width = vcfg->pip.zoom.w, .u16Height = vcfg->pip.zoom.h
	};
	MI_S32 crop_ret = MI_VPE_SetPortCrop(0, 1, &crop);

	MI_SYS_ChnPort_t p1 = {
		.module = (i6_sys_mod)E_MI_MODULE_ID_VPE,
		.device = 0, .channel = 0, .port = 1
	};
	/* u32UserFrameDepth = how many frames userspace may hold at once;
	 * u32BufQueueDepth  = kernel-side buffer queue depth.  Without the
	 * user-side depth set > 0, ChnOutputPortGetBuf returns 0xa009200d
	 * "no resources" because there's nowhere to hand the frame to. */
	if (p_sys_set_chn_depth) p_sys_set_chn_depth(&p1, 4, 4);

	MI_S32 enable_ret = MI_VPE_EnablePort(0, 1);
	if (enable_ret != 0) {
		fprintf(stderr, "{\"probe\":\"pip_inproc\","
			"\"err\":\"VPE_EnablePort(0,1)\","
			"\"code\":\"0x%x\",\"crop_ret\":\"0x%x\"}\n",
			(unsigned)enable_ret, (unsigned)crop_ret);
		fflush(stderr);
		return;
	}

	usleep(80000);  /* let port-1 produce a frame */

	MI_PHY scratch_phy = 0;
	MI_S32 alloc_ret = p_mma_alloc((MI_U8*)"mma_heap_name0", dst_size,
		&scratch_phy);
	if (alloc_ret != 0) {
		MI_VPE_DisablePort(0, 1);
		fprintf(stderr, "{\"probe\":\"pip_inproc\","
			"\"err\":\"MMA_Alloc\",\"code\":\"0x%x\"}\n",
			(unsigned)alloc_ret);
		fflush(stderr);
		return;
	}
	void *scratch_va = NULL;
	if (p_sys_mmap(scratch_phy, dst_size, &scratch_va, 0) == MI_SUCCESS &&
	    scratch_va) {
		memset(scratch_va, 0x55, dst_size);
		p_sys_munmap(scratch_va, dst_size);
	}

	MI_SYS_BufInfo_t info_1;
	memset(&info_1, 0, sizeof(info_1));
	MI_SYS_BUF_HANDLE handle_1 = 0;
	struct timespec t_g0, t_g1;
	clock_gettime(CLOCK_MONOTONIC, &t_g0);
	MI_S32 get1_ret = p_chn_get(&p1, &info_1, &handle_1);
	clock_gettime(CLOCK_MONOTONIC, &t_g1);
	long get1_us = (t_g1.tv_sec - t_g0.tv_sec) * 1000000L +
	               (t_g1.tv_nsec - t_g0.tv_nsec) / 1000L;

	int divp_ret = -999; long divp_warmup_us = -1;
	long divp_min_us = -1, divp_max_us = -1, divp_avg_us = -1;
	int divp_steady_iters = 0;
	int top_y = -1; int bot_y = -1;
	if (get1_ret == MI_SUCCESS) {
		MI_DIVP_DirectBuf_t src = {
			.ePixelFormat = info_1.stFrameData.ePixelFormat,
			.u32Width  = info_1.stFrameData.u16Width,
			.u32Height = info_1.stFrameData.u16Height,
			.u32Stride = { info_1.stFrameData.u32Stride[0],
				info_1.stFrameData.u32Stride[1], 0 },
			.phyAddr = { info_1.stFrameData.phyAddr[0],
				info_1.stFrameData.phyAddr[1], 0 }
		};
		MI_DIVP_DirectBuf_t dst = {
			.ePixelFormat = DIVP_PIXFMT_YUV420SP,
			.u32Width  = dst_w,
			.u32Height = dst_h,
			.u32Stride = { dst_stride, dst_stride, 0 },
			.phyAddr = { scratch_phy,
				scratch_phy + dst_stride * dst_h, 0 }
		};
		MI_SYS_WindowRect_t src_crop = {
			.u16X = 0, .u16Y = 0,
			.u16Width = src.u32Width, .u16Height = src.u32Height
		};

		/* Warmup call — first invocation may include lazy DIVP init
		 * or contend with the live pipeline coming up. */
		struct timespec t_d0, t_d1;
		clock_gettime(CLOCK_MONOTONIC, &t_d0);
		divp_ret = p_divp_stretch(&src, &src_crop, &dst);
		clock_gettime(CLOCK_MONOTONIC, &t_d1);
		divp_warmup_us = (t_d1.tv_sec - t_d0.tv_sec) * 1000000L +
		                 (t_d1.tv_nsec - t_d0.tv_nsec) / 1000L;

		/* Steady-state: loop N calls against the same source buffer.
		 * Same buffer is OK — we still hold its handle and the kernel
		 * keeps it alive until we PutBuf. */
		if (divp_ret == MI_SUCCESS) {
			const int N = 30;
			long total_us = 0;
			for (int i = 0; i < N; i++) {
				clock_gettime(CLOCK_MONOTONIC, &t_d0);
				int r = p_divp_stretch(&src, &src_crop, &dst);
				clock_gettime(CLOCK_MONOTONIC, &t_d1);
				if (r != MI_SUCCESS) { divp_ret = r; break; }
				long us = (t_d1.tv_sec - t_d0.tv_sec) * 1000000L +
					(t_d1.tv_nsec - t_d0.tv_nsec) / 1000L;
				if (divp_min_us < 0 || us < divp_min_us)
					divp_min_us = us;
				if (us > divp_max_us) divp_max_us = us;
				total_us += us;
				divp_steady_iters++;
			}
			if (divp_steady_iters > 0)
				divp_avg_us = total_us / divp_steady_iters;
		}

		p_chn_put(handle_1);

		if (divp_ret == MI_SUCCESS &&
		    p_sys_mmap(scratch_phy, dst_size, &scratch_va, 0) ==
		        MI_SUCCESS && scratch_va) {
			top_y = ((unsigned char*)scratch_va)
				[dst_stride + dst_w / 2];
			bot_y = ((unsigned char*)scratch_va)
				[dst_stride * (dst_h - 2) + dst_w / 2];
			p_sys_munmap(scratch_va, dst_size);
		}
	}

	/* Try GetBuf on port-0 (REALTIME-bound to VENC). */
	MI_SYS_ChnPort_t p0 = {
		.module = (i6_sys_mod)E_MI_MODULE_ID_VPE,
		.device = 0, .channel = 0, .port = 0
	};
	MI_SYS_BufInfo_t info_0;
	memset(&info_0, 0, sizeof(info_0));
	MI_SYS_BUF_HANDLE handle_0 = 0;
	MI_S32 get0_ret = p_chn_get(&p0, &info_0, &handle_0);
	if (get0_ret == MI_SUCCESS) p_chn_put(handle_0);

	/* ── Phase 2: DIVP → RGN ARGB888 canvas ──────────────────────────
	 *
	 * Skip the scratch buffer entirely.  Create an RGN region with
	 * ARGB888 pixfmt at pip.position.{w,h}, attached to VPE channel 0
	 * at pip.position.{x,y}.  GetCanvasInfo gives us the phys addr;
	 * DIVP can write YUV→ARGB8888 directly into it; UpdateCanvas
	 * commits.  If this works, the production compositor needs only:
	 *   GetBuf port-1 + DIVP + UpdateCanvas — zero userspace memcpy.
	 *
	 * Region is left attached after the probe so the user can visually
	 * confirm a single-frame PiP overlay on the live stream until the
	 * next venc restart. */
	int rgn_create_ret = -1, rgn_attach_ret = -1, rgn_canvas_ret = -1;
	int rgn_divp_ret = -999, rgn_update_ret = -1;
	int rgn_argb888_create_ret = -1;  /* first attempt before fallback */
	long rgn_divp_us = -1;
	uint64_t rgn_canvas_phy = 0;
	uint32_t rgn_canvas_stride = 0;
	int rgn_pix_fmt = -1;

	/* GetBuf port-1 again (warmup loop already returned the handle). */
	MI_SYS_BufInfo_t info_p2;
	memset(&info_p2, 0, sizeof(info_p2));
	MI_SYS_BUF_HANDLE handle_p2 = 0;
	int p2_get1_ret = p_chn_get(&p1, &info_p2, &handle_p2);

	if (p2_get1_ret == MI_SUCCESS && p_rgn_create) {
		/* Region create.  debug_osd already called MI_RGN_Init when
		 * debug.showOsd=true; we don't re-Init to avoid clobbering
		 * the palette it set up.  If debug OSD is disabled, RGN is
		 * uninitialized and Create will likely fail — caller can
		 * enable debug.showOsd to provoke initialization. */
		/* Try ARGB888 first (direct DIVP target).  If the kernel
		 * rejects ("Check osd attr error" in dmesg), retry with
		 * ARGB4444 — that's the format debug OSD's I4 region uses
		 * (well, I4, but ARGB4444 is the documented OSD canvas
		 * format).  The fallback path needs CPU ARGB8888→ARGB4444
		 * downconvert. */
		probe_rgn_cnf cnf = {
			.type = 0,                 /* I6_RGN_TYPE_OSD */
			.pixFmt = I6_RGN_PIXFMT_ARGB888_p,
			.size = { .width = dst_w, .height = dst_h }
		};
		rgn_argb888_create_ret = p_rgn_create(PROBE_RGN_HANDLE, &cnf);
		rgn_create_ret = rgn_argb888_create_ret;
		if (rgn_create_ret != 0) {
			cnf.pixFmt = I6_RGN_PIXFMT_ARGB4444_p;
			rgn_create_ret = p_rgn_create(PROBE_RGN_HANDLE, &cnf);
		}

		if (rgn_create_ret == 0) {
			/* MI_RGN uses its own module enum — VPE = 0.  Build
			 * the bind manually as debug_osd does. */
			MI_SYS_ChnPort_t rgn_vpe_bind = {
				.module = 0, .device = 0,
				.channel = 0, .port = 0
			};
			probe_rgn_chn chn = {0};
			chn.show = 1;
			chn.point.x = vcfg->pip.position.x;
			chn.point.y = vcfg->pip.position.y;
			chn.osd.layer = 1;          /* above debug OSD's layer 0 */
			chn.osd.constAlphaOn = 0;   /* pixel alpha */
			rgn_attach_ret = p_rgn_attach(PROBE_RGN_HANDLE,
				&rgn_vpe_bind, &chn);

			if (rgn_attach_ret == 0) {
				probe_rgn_canvas canvas;
				memset(&canvas, 0, sizeof(canvas));
				rgn_canvas_ret = p_rgn_get_canvas(
					PROBE_RGN_HANDLE, &canvas);
				rgn_canvas_phy = canvas.phyAddr;
				rgn_canvas_stride = canvas.u32Stride;
				rgn_pix_fmt = canvas.ePixelFmt;

				if (rgn_canvas_ret == 0 && canvas.phyAddr) {
					MI_DIVP_DirectBuf_t src = {
						.ePixelFormat =
							info_p2.stFrameData.ePixelFormat,
						.u32Width = info_p2.stFrameData.u16Width,
						.u32Height = info_p2.stFrameData.u16Height,
						.u32Stride = {
							info_p2.stFrameData.u32Stride[0],
							info_p2.stFrameData.u32Stride[1], 0 },
						.phyAddr = {
							info_p2.stFrameData.phyAddr[0],
							info_p2.stFrameData.phyAddr[1], 0 }
					};
					MI_DIVP_DirectBuf_t dst = {
						.ePixelFormat = DIVP_PIXFMT_ARGB8888,
						.u32Width = dst_w,
						.u32Height = dst_h,
						.u32Stride = { canvas.u32Stride, 0, 0 },
						.phyAddr = { canvas.phyAddr, 0, 0 }
					};
					MI_SYS_WindowRect_t crop_rgn = {
						.u16X = 0, .u16Y = 0,
						.u16Width = src.u32Width,
						.u16Height = src.u32Height
					};
					struct timespec t0, t1;
					clock_gettime(CLOCK_MONOTONIC, &t0);
					rgn_divp_ret = p_divp_stretch(
						&src, &crop_rgn, &dst);
					clock_gettime(CLOCK_MONOTONIC, &t1);
					rgn_divp_us = (t1.tv_sec - t0.tv_sec) *
						1000000L +
						(t1.tv_nsec - t0.tv_nsec) / 1000L;

					if (rgn_divp_ret == 0)
						rgn_update_ret = p_rgn_update_canvas(
							PROBE_RGN_HANDLE);
				}
			}
		}
	}

	if (p2_get1_ret == MI_SUCCESS) p_chn_put(handle_p2);

	/* Don't destroy the RGN region — leave it attached so the user can
	 * visually confirm a single-frame PiP overlay on the live stream.
	 * Destroyed on next venc restart automatically. */

	fprintf(stderr, "{\"probe\":\"pip_inproc\","
		"\"port1_set_mode\":\"0x%x\","
		"\"port1_set_crop\":\"0x%x\","
		"\"port1_enable\":\"0x%x\","
		"\"port1_get_us\":%ld,"
		"\"port1_get_ret\":\"0x%x\","
		"\"port1_src_w\":%u,\"port1_src_h\":%u,"
		"\"port1_src_phy\":\"0x%llx\","
		"\"port1_src_stride\":%u,"
		"\"divp_ret\":\"0x%x\","
		"\"divp_warmup_us\":%ld,\"divp_steady_n\":%d,"
		"\"divp_min_us\":%ld,\"divp_avg_us\":%ld,\"divp_max_us\":%ld,"
		"\"scratch_top_y\":%d,\"scratch_bot_y\":%d,"
		"\"port0_get_ret\":\"0x%x\","
		"\"rgn_argb888_create_ret\":\"0x%x\","
		"\"rgn_create_ret\":\"0x%x\","
		"\"rgn_attach_ret\":\"0x%x\","
		"\"rgn_canvas_ret\":\"0x%x\","
		"\"rgn_canvas_phy\":\"0x%llx\","
		"\"rgn_canvas_stride\":%u,"
		"\"rgn_canvas_pix_fmt\":%d,"
		"\"rgn_divp_ret\":\"0x%x\","
		"\"rgn_divp_us\":%ld,"
		"\"rgn_update_ret\":\"0x%x\""
		"}\n",
		(unsigned)set_mode_ret, (unsigned)crop_ret,
		(unsigned)enable_ret,
		get1_us, (unsigned)get1_ret,
		(unsigned)info_1.stFrameData.u16Width,
		(unsigned)info_1.stFrameData.u16Height,
		(unsigned long long)info_1.stFrameData.phyAddr[0],
		(unsigned)info_1.stFrameData.u32Stride[0],
		(unsigned)divp_ret,
		divp_warmup_us, divp_steady_iters,
		divp_min_us, divp_avg_us, divp_max_us,
		top_y, bot_y,
		(unsigned)get0_ret,
		(unsigned)rgn_argb888_create_ret,
		(unsigned)rgn_create_ret, (unsigned)rgn_attach_ret,
		(unsigned)rgn_canvas_ret,
		(unsigned long long)rgn_canvas_phy,
		(unsigned)rgn_canvas_stride, rgn_pix_fmt,
		(unsigned)rgn_divp_ret, rgn_divp_us,
		(unsigned)rgn_update_ret);
	fflush(stderr);

	MI_VPE_DisablePort(0, 1);
	p_mma_free(scratch_phy);
}
