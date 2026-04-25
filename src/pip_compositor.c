/* Picture-in-Picture compositor (Star6E grayscale I8 path).
 *
 * Architecture validated by tools/pip_probe.c + src/pip_probe_inproc.c
 * (probes 1–4) — see include/pip_compositor.h for the design notes.
 * Per-frame steady-state cost on 192.168.1.13: ~0.5 ms HW + ~0% CPU. */

#include "pip_compositor.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* star6e.h is force-included via -include ssc338q_compat.h, providing
 * MI_S32, MI_U*, MI_BOOL, MI_SYS_ChnPort_t (= i6_sys_bind),
 * MI_VPE_PortAttr_t (= i6_vpe_port), I6_PIXFMT_*, MI_VPE_* macros. */

/* ── Vendored SDK types (minimal subset) ───────────────────────────── */

typedef unsigned long long MI_PHY;
typedef MI_S32             MI_SYS_BUF_HANDLE;

#define MI_SUCCESS 0
#define DIVP_PIXFMT_YUV420SP 11

typedef struct {
	uint16_t u16X, u16Y, u16Width, u16Height;
} PipWindowRect;

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
} PipFrameDataSubset;

typedef struct {
	MI_U64           u64Pts;
	MI_U64           u64SidebandMsg;
	int              eBufType;
	MI_BOOL          bEndOfStream;
	MI_BOOL          bUsrBuf;
	MI_U32           u32SequenceNumber;
	MI_BOOL          bDrop;
	union {
		PipFrameDataSubset stFrameData;
		uint8_t _pad[768];
	};
} PipBufInfo;

#pragma pack(push, 4)
typedef struct {
	int    ePixelFormat;
	MI_U32 u32Width;
	MI_U32 u32Height;
	MI_U32 u32Stride[3];
	MI_PHY phyAddr[3];
} PipDirectBuf;
#pragma pack(pop)

/* MI_RGN types — same layout as in src/debug_osd.c */
typedef enum {
	PIP_RGN_PIXFMT_ARGB1555 = 0,
	PIP_RGN_PIXFMT_ARGB4444,
	PIP_RGN_PIXFMT_I2,
	PIP_RGN_PIXFMT_I4,
	PIP_RGN_PIXFMT_I8,
	PIP_RGN_PIXFMT_RGB565,
	PIP_RGN_PIXFMT_ARGB888,
} PipRgnPixfmt;

typedef struct {
	int          type;       /* I6_RGN_TYPE_OSD = 0 */
	PipRgnPixfmt pixFmt;
	struct { unsigned int width, height; } size;
} PipRgnCnf;

typedef struct {
	int           invColOn;
	int           lowThanThresh;
	unsigned int  lumThresh;
	unsigned short divWidth, divHeight;
} PipRgnInv;

typedef struct {
	unsigned int  layer;
	int           constAlphaOn;
	union {
		unsigned char bgFgAlpha[2];
		unsigned char constAlpha[2];
	};
	PipRgnInv invert;
} PipRgnOsd;

typedef struct {
	int show;
	struct { unsigned int x, y; } point;
	union {
		uint8_t _pad_for_cov[64];
		PipRgnOsd osd;
	};
} PipRgnChn;

typedef struct {
	uint64_t      phyAddr;
	unsigned long virtAddr;
	struct { uint32_t u32Width, u32Height; } stSize;
	uint32_t      u32Stride;
	int           ePixelFmt;
} PipRgnCanvas;

typedef struct {
	unsigned char alpha, red, green, blue;
} PipRgnPaletteEntry;

typedef struct {
	PipRgnPaletteEntry element[256];
} PipRgnPalette;

#define PIP_RGN_HANDLE 1   /* debug_osd uses 0 */

/* ── Compositor state ──────────────────────────────────────────────── */

struct PipCompositor {
	void *sys_lib;
	void *divp_lib;
	void *rgn_lib;

	/* dlsym handles. */
	MI_S32 (*p_chn_get)(MI_SYS_ChnPort_t *, PipBufInfo *,
		MI_SYS_BUF_HANDLE *);
	MI_S32 (*p_chn_put)(MI_SYS_BUF_HANDLE);
	MI_S32 (*p_mma_alloc)(MI_U8 *, MI_U32, MI_PHY *);
	MI_S32 (*p_mma_free)(MI_PHY);
	MI_S32 (*p_sys_set_chn_depth)(MI_SYS_ChnPort_t *, MI_U32, MI_U32);
	MI_S32 (*p_sys_flush_inv_cache)(void *vaddr, MI_U32 size);
	MI_S32 (*p_divp_stretch)(PipDirectBuf *, PipWindowRect *,
		PipDirectBuf *);
	MI_S32 (*p_rgn_init)(PipRgnPalette *);
	MI_S32 (*p_rgn_create)(int, PipRgnCnf *);
	MI_S32 (*p_rgn_destroy)(int);
	MI_S32 (*p_rgn_attach)(int, MI_SYS_ChnPort_t *, PipRgnChn *);
	MI_S32 (*p_rgn_detach)(int, MI_SYS_ChnPort_t *);
	MI_S32 (*p_rgn_get_canvas)(int, PipRgnCanvas *);
	MI_S32 (*p_rgn_update_canvas)(int);
	MI_S32 (*p_rgn_set_display_attr)(int, PipRgnChn *);

	/* Configured rects (mirror config). */
	pthread_mutex_t cfg_mutex;
	uint16_t        zoom_x, zoom_y, zoom_w, zoom_h;
	uint16_t        pos_x, pos_y, pos_w, pos_h;
	uint8_t         refresh_every;
	bool            blit_enabled;

	/* Pre-allocated buffers. */
	MI_PHY    canvas_phy;       /* RGN I8 canvas (Y dst) */
	uint32_t  canvas_stride;
	uint32_t  canvas_height;    /* allocated H, may differ from pos_h */
	MI_PHY    uv_scratch_phy;   /* small UV discard buffer */
	uint32_t  uv_scratch_size;

	/* Thread state. */
	pthread_t        thread;
	volatile bool    thread_running;
	volatile bool    thread_started;
};

/* ── dlopen helpers ───────────────────────────────────────────────── */

static int pip_load_syms(PipCompositor *c)
{
	c->sys_lib  = dlopen("libmi_sys.so",  RTLD_LAZY);
	c->divp_lib = dlopen("libmi_divp.so", RTLD_LAZY);
	c->rgn_lib  = dlopen("libmi_rgn.so",  RTLD_LAZY);
	if (!c->sys_lib || !c->divp_lib || !c->rgn_lib) {
		fprintf(stderr, "[pip] dlopen failed\n");
		return -1;
	}
#define PIP_SYM(field, lib, name) do { \
		c->field = (void*)dlsym(c->lib, name); \
		if (!c->field) { \
			fprintf(stderr, "[pip] missing symbol %s\n", name); \
			return -1; \
		} \
	} while (0)

	PIP_SYM(p_chn_get,           sys_lib,  "MI_SYS_ChnOutputPortGetBuf");
	PIP_SYM(p_chn_put,           sys_lib,  "MI_SYS_ChnOutputPortPutBuf");
	PIP_SYM(p_mma_alloc,         sys_lib,  "MI_SYS_MMA_Alloc");
	PIP_SYM(p_mma_free,          sys_lib,  "MI_SYS_MMA_Free");
	PIP_SYM(p_sys_set_chn_depth, sys_lib,  "MI_SYS_SetChnOutputPortDepth");
	PIP_SYM(p_sys_flush_inv_cache, sys_lib, "MI_SYS_FlushInvCache");
	PIP_SYM(p_divp_stretch,      divp_lib, "MI_DIVP_StretchBuf");
	PIP_SYM(p_rgn_init,          rgn_lib,  "MI_RGN_Init");
	PIP_SYM(p_rgn_create,        rgn_lib,  "MI_RGN_Create");
	PIP_SYM(p_rgn_destroy,       rgn_lib,  "MI_RGN_Destroy");
	PIP_SYM(p_rgn_attach,        rgn_lib,  "MI_RGN_AttachToChn");
	PIP_SYM(p_rgn_detach,        rgn_lib,  "MI_RGN_DetachFromChn");
	PIP_SYM(p_rgn_get_canvas,    rgn_lib,  "MI_RGN_GetCanvasInfo");
	PIP_SYM(p_rgn_update_canvas, rgn_lib,  "MI_RGN_UpdateCanvas");
	PIP_SYM(p_rgn_set_display_attr, rgn_lib, "MI_RGN_SetDisplayAttr");
#undef PIP_SYM
	return 0;
}

static void pip_unload_syms(PipCompositor *c)
{
	if (c->sys_lib)  { dlclose(c->sys_lib);  c->sys_lib  = NULL; }
	if (c->divp_lib) { dlclose(c->divp_lib); c->divp_lib = NULL; }
	if (c->rgn_lib)  { dlclose(c->rgn_lib);  c->rgn_lib  = NULL; }
}

/* Install a 256-entry grayscale palette: index i → RGB(i,i,i), alpha=255.
 * Index 0 is fully transparent so Y=0 (pure black) shows through; this
 * matches user expectation (black should not occlude the underlying frame
 * because true 0 is rare in real video). */
static void pip_palette_grayscale(PipRgnPalette *pal)
{
	memset(pal, 0, sizeof(*pal));
	for (int i = 1; i < 256; i++) {
		pal->element[i].alpha = 255;
		pal->element[i].red   = (unsigned char)i;
		pal->element[i].green = (unsigned char)i;
		pal->element[i].blue  = (unsigned char)i;
	}
}

/* ── Lifecycle ────────────────────────────────────────────────────── */

PipCompositor *pip_compositor_create(const VencConfig *vcfg)
{
	if (!vcfg || !vcfg->pip.enabled) return NULL;
	if (strcmp(vcfg->pip.format, "grayscale") != 0) {
		fprintf(stderr, "[pip] unsupported pip.format=\"%s\" "
			"(only \"grayscale\" implemented in v1)\n",
			vcfg->pip.format);
		return NULL;
	}
	if (!vcfg->pip.zoom.w || !vcfg->pip.zoom.h ||
	    !vcfg->pip.position.w || !vcfg->pip.position.h) {
		fprintf(stderr, "[pip] zoom or position rect has zero "
			"dimensions, refusing to start\n");
		return NULL;
	}

	PipCompositor *c = calloc(1, sizeof(*c));
	if (!c) return NULL;

	pthread_mutex_init(&c->cfg_mutex, NULL);
	c->zoom_x = vcfg->pip.zoom.x;
	c->zoom_y = vcfg->pip.zoom.y;
	c->zoom_w = vcfg->pip.zoom.w;
	c->zoom_h = vcfg->pip.zoom.h;
	c->pos_x  = vcfg->pip.position.x;
	c->pos_y  = vcfg->pip.position.y;
	c->pos_w  = vcfg->pip.position.w;
	c->pos_h  = vcfg->pip.position.h;
	c->refresh_every = vcfg->pip.refresh_every ? vcfg->pip.refresh_every : 1;
	c->blit_enabled = true;

	if (pip_load_syms(c) != 0) goto fail;

	/* VPE port-1 outputs the FULL encode frame (matches port-0's output
	 * dims).  Zoom crop is applied later inside DIVP_StretchBuf — that
	 * way the user's zoom coordinates stay in encode-output space
	 * (1920×1080) rather than VPE-input space (sensor 2400×1350) which
	 * SetPortCrop expects.  DIVP does crop+scale in HW at ~0.4 ms.
	 *
	 * Cost: a 1920×1080 YUV420SP port-1 buffer × queue depth = ~3 MB
	 * per buffer.  We use depth 2 to keep MMA usage modest. */
	MI_VPE_PortAttr_t port = {0};
	port.output.width  = vcfg->video0.width;
	port.output.height = vcfg->video0.height;
	port.mirror = 0;
	port.flip = 0;
	port.pixFmt = I6_PIXFMT_YUV420SP;
	port.compress = I6_COMPR_NONE;
	if (MI_VPE_SetPortMode(0, 1, &port) != 0) {
		fprintf(stderr, "[pip] MI_VPE_SetPortMode(0,1) failed\n");
		goto fail;
	}

	(void)c->zoom_x; (void)c->zoom_y; /* used in thread for DIVP crop */

	MI_SYS_ChnPort_t p1 = {
		.module = (i6_sys_mod)E_MI_MODULE_ID_VPE,
		.device = 0, .channel = 0, .port = 1
	};
	/* Depth 2 is enough for the compositor (one in flight + one queued)
	 * and keeps MMA usage at ~6 MB for the full-frame port-1 buffers. */
	c->p_sys_set_chn_depth(&p1, 2, 2);

	if (MI_VPE_EnablePort(0, 1) != 0) {
		fprintf(stderr, "[pip] MI_VPE_EnablePort(0,1) failed\n");
		goto fail;
	}

	/* DON'T re-init RGN — that would tear down debug_osd's region
	 * (handle 0) and flood the kernel log with "Handle not found"
	 * errors on every subsequent debug_osd UpdateCanvas, costing ~30%
	 * fps.  We rely on debug_osd's palette covering indices 0-15;
	 * indices 16-255 default to fully-transparent in that palette,
	 * which means most Y values from real video render as transparent.
	 *
	 * To fix: when both debug_osd and pip are enabled, debug_osd's
	 * palette init needs to include a grayscale ramp at 16-255 (named
	 * colours at 1-8 stay intact for I4 OSD).  See debug_osd_draw.c
	 * g_palette[].  Pip writes Y values 0-255; only 1-8 collide with
	 * named colours (rare in real images), 16-255 render as grayscale,
	 * 0,9-15 render transparent. */
	(void)c->p_rgn_init;  /* not called intentionally */

	/* Create I8 region at pip.position dims. */
	PipRgnCnf cnf = {
		.type = 0,                /* OSD */
		.pixFmt = PIP_RGN_PIXFMT_I8,
		.size = { .width = c->pos_w, .height = c->pos_h }
	};
	if (c->p_rgn_create(PIP_RGN_HANDLE, &cnf) != 0) {
		fprintf(stderr, "[pip] MI_RGN_Create(I8 %ux%u) failed\n",
			c->pos_w, c->pos_h);
		MI_VPE_DisablePort(0, 1);
		goto fail;
	}

	/* Attach to VPE port-0 channel at pip.position.{x,y}. */
	MI_SYS_ChnPort_t rgn_bind = {
		.module = 0,              /* RGN's own MID enum: VPE = 0 */
		.device = 0, .channel = 0, .port = 0
	};
	PipRgnChn chn = {0};
	chn.show = 1;
	chn.point.x = c->pos_x;
	chn.point.y = c->pos_y;
	chn.osd.layer = 1;            /* above debug_osd's layer 0 */
	chn.osd.constAlphaOn = 0;     /* per-pixel alpha from palette */
	if (c->p_rgn_attach(PIP_RGN_HANDLE, &rgn_bind, &chn) != 0) {
		fprintf(stderr, "[pip] MI_RGN_AttachToChn failed\n");
		c->p_rgn_destroy(PIP_RGN_HANDLE);
		MI_VPE_DisablePort(0, 1);
		goto fail;
	}

	PipRgnCanvas canvas = {0};
	if (c->p_rgn_get_canvas(PIP_RGN_HANDLE, &canvas) != 0) {
		fprintf(stderr, "[pip] MI_RGN_GetCanvasInfo failed\n");
		c->p_rgn_detach(PIP_RGN_HANDLE, &rgn_bind);
		c->p_rgn_destroy(PIP_RGN_HANDLE);
		MI_VPE_DisablePort(0, 1);
		goto fail;
	}
	c->canvas_phy    = canvas.phyAddr;
	c->canvas_stride = canvas.u32Stride;
	c->canvas_height = canvas.stSize.u32Height;

	/* Initialise the canvas via virtAddr, then UpdateCanvas — this is
	 * the SDK-blessed sequence (same as debug_osd does at create) and
	 * puts the RGN double-buffer state machine into a known "front
	 * buffer valid" state.  Skipping it leaves "front buf state 0"
	 * and every subsequent UpdateCanvas fails. */
	if (canvas.virtAddr) {
		uint8_t *p = (uint8_t *)(uintptr_t)canvas.virtAddr;
		for (uint32_t y = 0; y < canvas.stSize.u32Height; y++)
			memset(p + y * canvas.u32Stride, 0, canvas.u32Stride);
		c->p_rgn_update_canvas(PIP_RGN_HANDLE);
	}

	/* UV scratch — DIVP writes UV plane here, we discard it.  Sized at
	 * canvas_stride * canvas_height / 2 (the YUV420SP UV plane size). */
	c->uv_scratch_size = c->canvas_stride * c->canvas_height / 2;
	if (c->p_mma_alloc((MI_U8*)"mma_heap_name0", c->uv_scratch_size,
			&c->uv_scratch_phy) != 0) {
		fprintf(stderr, "[pip] UV scratch alloc failed\n");
		c->p_rgn_detach(PIP_RGN_HANDLE, &rgn_bind);
		c->p_rgn_destroy(PIP_RGN_HANDLE);
		MI_VPE_DisablePort(0, 1);
		goto fail;
	}

	fprintf(stderr,
		"[pip] grayscale compositor ready: zoom=%u,%u %ux%u "
		"pos=%u,%u %ux%u canvas_phy=0x%llx stride=%u\n",
		c->zoom_x, c->zoom_y, c->zoom_w, c->zoom_h,
		c->pos_x, c->pos_y, c->pos_w, c->pos_h,
		(unsigned long long)c->canvas_phy, c->canvas_stride);
	return c;

fail:
	pip_unload_syms(c);
	pthread_mutex_destroy(&c->cfg_mutex);
	free(c);
	return NULL;
}

void pip_compositor_destroy(PipCompositor *c)
{
	if (!c) return;
	pip_compositor_stop(c);

	MI_SYS_ChnPort_t rgn_bind = {
		.module = 0, .device = 0, .channel = 0, .port = 0
	};
	if (c->p_rgn_detach)  c->p_rgn_detach(PIP_RGN_HANDLE, &rgn_bind);
	if (c->p_rgn_destroy) c->p_rgn_destroy(PIP_RGN_HANDLE);

	MI_VPE_DisablePort(0, 1);

	if (c->p_mma_free && c->uv_scratch_phy) c->p_mma_free(c->uv_scratch_phy);

	pip_unload_syms(c);
	pthread_mutex_destroy(&c->cfg_mutex);
	free(c);
}

/* ── Compositor thread ────────────────────────────────────────────── */

static void *pip_thread_fn(void *arg)
{
	PipCompositor *c = arg;
	MI_SYS_ChnPort_t p1 = {
		.module = (i6_sys_mod)E_MI_MODULE_ID_VPE,
		.device = 0, .channel = 0, .port = 1
	};

	/* Pre-warm DIVP — first call after a long idle takes ~2s on this
	 * BSP.  Doing it before the steady-state loop shifts that latency
	 * out of the user-observable per-frame window. */
	{
		PipBufInfo info = {0};
		MI_SYS_BUF_HANDLE handle = 0;
		int wait = 0;
		while (c->thread_running && wait < 50) {
			if (c->p_chn_get(&p1, &info, &handle) == MI_SUCCESS) {
				PipDirectBuf src = {
					.ePixelFormat =
						info.stFrameData.ePixelFormat,
					.u32Width = info.stFrameData.u16Width,
					.u32Height = info.stFrameData.u16Height,
					.u32Stride = {
						info.stFrameData.u32Stride[0],
						info.stFrameData.u32Stride[1], 0 },
					.phyAddr = {
						info.stFrameData.phyAddr[0],
						info.stFrameData.phyAddr[1], 0 }
				};
				PipDirectBuf dst = {
					.ePixelFormat = DIVP_PIXFMT_YUV420SP,
					.u32Width  = c->pos_w,
					.u32Height = c->pos_h,
					.u32Stride = { c->canvas_stride,
						c->canvas_stride, 0 },
					.phyAddr = { c->canvas_phy,
						c->uv_scratch_phy, 0 }
				};
				PipWindowRect crop = {
					.u16X = c->zoom_x, .u16Y = c->zoom_y,
					.u16Width = c->zoom_w,
					.u16Height = c->zoom_h
				};
				c->p_divp_stretch(&src, &crop, &dst);
				c->p_rgn_update_canvas(PIP_RGN_HANDLE);
				c->p_chn_put(handle);
				break;
			}
			usleep(20000);
			wait++;
		}
	}

	uint32_t frame_counter = 0;
	while (c->thread_running) {
		uint8_t refresh;
		bool blit_enabled;
		pthread_mutex_lock(&c->cfg_mutex);
		refresh = c->refresh_every ? c->refresh_every : 1;
		blit_enabled = c->blit_enabled;
		pthread_mutex_unlock(&c->cfg_mutex);

		if (!blit_enabled) {
			usleep(10000);
			continue;
		}

		PipBufInfo info = {0};
		MI_SYS_BUF_HANDLE handle = 0;
		MI_S32 r = c->p_chn_get(&p1, &info, &handle);
		if (r != MI_SUCCESS) {
			usleep(2000);
			continue;
		}

		frame_counter++;
		if ((frame_counter % refresh) != 0) {
			c->p_chn_put(handle);
			continue;
		}

		/* Re-acquire canvas every frame — RGN canvas is double-
		 * buffered, virtAddr/phyAddr can change after UpdateCanvas.
		 * Same pattern debug_osd uses in begin_frame. */
		PipRgnCanvas canvas = {0};
		if (c->p_rgn_get_canvas(PIP_RGN_HANDLE, &canvas) !=
		    MI_SUCCESS || !canvas.phyAddr) {
			c->p_chn_put(handle);
			usleep(2000);
			continue;
		}

		uint16_t zx, zy, zw, zh;
		pthread_mutex_lock(&c->cfg_mutex);
		zx = c->zoom_x; zy = c->zoom_y;
		zw = c->zoom_w; zh = c->zoom_h;
		pthread_mutex_unlock(&c->cfg_mutex);

		/* Log port-1 actual dims once on first frame so we can verify
		 * the buffer layout matches our crop assumption. */
		static int dim_logged = 0;
		if (!dim_logged) {
			fprintf(stderr, "[pip] port-1 frame: w=%u h=%u "
				"stride[0]=%u stride[1]=%u "
				"phy[0]=0x%llx phy[1]=0x%llx pix_fmt=%d\n",
				(unsigned)info.stFrameData.u16Width,
				(unsigned)info.stFrameData.u16Height,
				(unsigned)info.stFrameData.u32Stride[0],
				(unsigned)info.stFrameData.u32Stride[1],
				(unsigned long long)info.stFrameData.phyAddr[0],
				(unsigned long long)info.stFrameData.phyAddr[1],
				info.stFrameData.ePixelFormat);
			fflush(stderr);
			dim_logged = 1;
		}

		PipDirectBuf src = {
			.ePixelFormat = info.stFrameData.ePixelFormat,
			.u32Width  = info.stFrameData.u16Width,
			.u32Height = info.stFrameData.u16Height,
			.u32Stride = { info.stFrameData.u32Stride[0],
				info.stFrameData.u32Stride[1], 0 },
			.phyAddr = { info.stFrameData.phyAddr[0],
				info.stFrameData.phyAddr[1], 0 }
		};
		PipDirectBuf dst = {
			.ePixelFormat = DIVP_PIXFMT_YUV420SP,
			.u32Width  = c->pos_w,
			.u32Height = c->pos_h,
			.u32Stride = { canvas.u32Stride, canvas.u32Stride, 0 },
			.phyAddr = { canvas.phyAddr, c->uv_scratch_phy, 0 }
		};
		/* DIVP crop = the user's zoom rect, in port-1 output space
		 * (= encode-output space since port-1 outputs full frame). */
		PipWindowRect crop = {
			.u16X = zx, .u16Y = zy, .u16Width = zw, .u16Height = zh
		};
		if (c->p_divp_stretch(&src, &crop, &dst) == MI_SUCCESS) {
			/* DIVP writes via DMA so the SDK's userspace dirty-
			 * page tracking doesn't see the change.  Touch one
			 * byte via virtAddr (kernel marks the page dirty),
			 * then FlushInvCache to sync caches both directions
			 * so UpdateCanvas's commit sees the DMA-written data
			 * AND the kernel-side state machine accepts the
			 * front-buffer as ready. */
			if (canvas.virtAddr) {
				volatile uint8_t *p =
					(volatile uint8_t *)
					(uintptr_t)canvas.virtAddr;
				p[0] = p[0];   /* RMW touch — marks dirty */
				c->p_sys_flush_inv_cache(
					(void*)(uintptr_t)canvas.virtAddr,
					canvas.u32Stride *
					canvas.stSize.u32Height);
			}
			c->p_rgn_update_canvas(PIP_RGN_HANDLE);
		}
		c->p_chn_put(handle);
	}
	return NULL;
}

int pip_compositor_start(PipCompositor *c)
{
	if (!c || c->thread_started) return 0;
	c->thread_running = true;
	if (pthread_create(&c->thread, NULL, pip_thread_fn, c) != 0) {
		c->thread_running = false;
		fprintf(stderr, "[pip] pthread_create failed: %s\n",
			strerror(errno));
		return -1;
	}
	c->thread_started = true;
	fprintf(stderr, "[pip] compositor thread started\n");
	return 0;
}

void pip_compositor_stop(PipCompositor *c)
{
	if (!c || !c->thread_started) return;
	c->thread_running = false;
	pthread_join(c->thread, NULL);
	c->thread_started = false;
	fprintf(stderr, "[pip] compositor thread stopped\n");
}

/* ── Live updates ─────────────────────────────────────────────────── */

int pip_compositor_apply_zoom(PipCompositor *c, uint16_t x, uint16_t y,
	uint16_t w, uint16_t h)
{
	if (!c) return -1;
	/* The compositor thread reads the zoom rect each iteration under
	 * cfg_mutex and passes it as the DIVP crop — no SDK call needed. */
	pthread_mutex_lock(&c->cfg_mutex);
	c->zoom_x = x; c->zoom_y = y; c->zoom_w = w; c->zoom_h = h;
	pthread_mutex_unlock(&c->cfg_mutex);
	return 0;
}

int pip_compositor_apply_position(PipCompositor *c, uint16_t x, uint16_t y)
{
	if (!c) return -1;
	pthread_mutex_lock(&c->cfg_mutex);
	c->pos_x = x; c->pos_y = y;
	pthread_mutex_unlock(&c->cfg_mutex);
	MI_SYS_ChnPort_t rgn_bind = {
		.module = 0, .device = 0, .channel = 0, .port = 0
	};
	PipRgnChn chn = {0};
	chn.show = 1;
	chn.point.x = x;
	chn.point.y = y;
	chn.osd.layer = 1;
	chn.osd.constAlphaOn = 0;
	(void)rgn_bind;  /* SetDisplayAttr signature varies; HOSD set only */
	return c->p_rgn_set_display_attr(PIP_RGN_HANDLE, &chn);
}

int pip_compositor_apply_enabled(PipCompositor *c, bool enabled)
{
	if (!c) return -1;
	pthread_mutex_lock(&c->cfg_mutex);
	c->blit_enabled = enabled;
	pthread_mutex_unlock(&c->cfg_mutex);
	return 0;
}
