/* tools/pip_probe.c
 *
 * Star6E PiP architecture probe. Tests MI_DIVP_StretchBuf (hardware
 * stretch/blit) as a candidate for compositing a zoom-region into the
 * encoded frame. Standalone — does not need a live pipeline.
 *
 * Build (host, against repo toolchain):
 *   cc -static -ldl -lrt -o pip_probe tools/pip_probe.c   # local sanity
 *   toolchain/toolchain.sigmastar-infinity6e/bin/arm-openipc-linux-gnueabihf-gcc \
 *     -O2 -ldl -lrt tools/pip_probe.c -o pip_probe-arm
 *
 * Run (on device):
 *   scp pip_probe-arm root@192.168.1.13:/tmp/
 *   ssh root@192.168.1.13 "/tmp/pip_probe-arm"
 *
 * Output: single-line JSON with success flag, timing, dst-changed verify.
 *
 * What this proves (or disproves):
 *   - MI_DIVP_StretchBuf can take two MMA-allocated YUV420SP buffers and
 *     copy a cropped/stretched region from src to dst, with no DIVP
 *     channel created. If yes, DIVP is a viable PiP compositor: the
 *     compositor thread would call StretchBuf(VPE port-1 buf, zoom rect,
 *     scratch buf) then memcpy/StretchBuf scratch into VPE port-0 buf.
 *   - Per-call latency in us. If << 1 frame interval (16.6ms @ 60fps),
 *     PiP is essentially free CPU-wise (HW path).
 *
 * Limitations:
 *   - Standalone. Doesn't validate VPE port-1 → port-0 in-place compositing
 *     (that needs a live pipeline).
 *   - Uses synthetic test pattern, not real sensor frames.
 *   - Does not test VPE port-zoom (MI_VPE_StartPortZoom) — that requires
 *     a VPE channel.
 *   - Does not test RGN canvas memcpy — that requires VPE channel + RGN
 *     attach (already proven by debug_osd.c).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

/* ── Vendored MI_SYS / MI_DIVP types (subset) ─────────────────────────── */

typedef int                MI_S32;
typedef unsigned int       MI_U32;
typedef unsigned short     MI_U16;
typedef unsigned char      MI_U8;
typedef unsigned long long MI_U64;
typedef unsigned long long MI_PHY;
typedef int                MI_BOOL;

#define MI_SUCCESS 0

/* From mi_sys_datatype.h enum order:
 *  0  YUV422_YUYV
 *  1  ARGB8888
 *  2  ABGR8888
 *  3  BGRA8888
 *  4  RGB565
 *  5  ARGB1555
 *  6  ARGB4444
 *  7  I2
 *  8  I4
 *  9  I8
 *  10 YUV_SEMIPLANAR_422
 *  11 YUV_SEMIPLANAR_420
 * DIVP supports YUV420SP and ARGB8888 only per mi_divp_datatype.h. */
#define E_MI_SYS_PIXEL_FRAME_YUV422_YUYV          0
#define E_MI_SYS_PIXEL_FRAME_ARGB8888             1
#define E_MI_SYS_PIXEL_FRAME_ARGB4444             6
#define E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420   11

typedef struct {
	MI_U16 u16X, u16Y;
	MI_U16 u16Width, u16Height;
} MI_SYS_WindowRect_t;

#pragma pack(push, 4)
typedef struct {
	int      ePixelFormat;   /* MI_SYS_PixelFormat_e */
	MI_U32   u32Width;
	MI_U32   u32Height;
	MI_U32   u32Stride[3];
	MI_PHY   phyAddr[3];
} MI_DIVP_DirectBuf_t;
#pragma pack(pop)

/* ── dlsym handles ────────────────────────────────────────────────────── */

static MI_S32 (*p_sys_init)(void);
static MI_S32 (*p_sys_exit)(void);
static MI_S32 (*p_mma_alloc)(MI_U8 *name, MI_U32 size, MI_PHY *phy);
static MI_S32 (*p_mma_free)(MI_PHY phy);
static MI_S32 (*p_sys_mmap)(MI_U64 phy, MI_U32 size, void **vaddr, MI_BOOL bcache);
static MI_S32 (*p_sys_munmap)(void *vaddr, MI_U32 size);
static MI_S32 (*p_divp_stretch)(MI_DIVP_DirectBuf_t *src,
	MI_SYS_WindowRect_t *crop, MI_DIVP_DirectBuf_t *dst);

static int load_syms(void)
{
	void *sys_lib = dlopen("/usr/lib/libmi_sys.so", RTLD_LAZY);
	void *divp_lib = dlopen("/usr/lib/libmi_divp.so", RTLD_LAZY);
	if (!sys_lib || !divp_lib) {
		fprintf(stderr, "dlopen failed: %s\n", dlerror());
		return -1;
	}
	p_sys_init     = (void*)dlsym(sys_lib, "MI_SYS_Init");
	p_sys_exit     = (void*)dlsym(sys_lib, "MI_SYS_Exit");
	p_mma_alloc    = (void*)dlsym(sys_lib, "MI_SYS_MMA_Alloc");
	p_mma_free     = (void*)dlsym(sys_lib, "MI_SYS_MMA_Free");
	p_sys_mmap     = (void*)dlsym(sys_lib, "MI_SYS_Mmap");
	p_sys_munmap   = (void*)dlsym(sys_lib, "MI_SYS_Munmap");
	p_divp_stretch = (void*)dlsym(divp_lib, "MI_DIVP_StretchBuf");
	if (!p_sys_init || !p_mma_alloc || !p_mma_free || !p_sys_mmap ||
	    !p_sys_munmap || !p_divp_stretch) {
		fprintf(stderr, "dlsym failed (%s)\n", dlerror());
		return -1;
	}
	return 0;
}

/* ── /proc/self/stat CPU sampler ──────────────────────────────────────── */

static unsigned long long read_self_jiffies(void)
{
	FILE *f = fopen("/proc/self/stat", "r");
	if (!f) return 0;
	unsigned long long utime = 0, stime = 0;
	int dummy_i; char dummy_s[256]; char dummy_c;
	fscanf(f, "%d %s %c %d %d %d %d %d %u %lu %lu %lu %lu %llu %llu",
	       &dummy_i, dummy_s, &dummy_c, &dummy_i, &dummy_i, &dummy_i,
	       &dummy_i, &dummy_i, (unsigned*)&dummy_i,
	       (unsigned long*)&dummy_i, (unsigned long*)&dummy_i,
	       (unsigned long*)&dummy_i, (unsigned long*)&dummy_i,
	       &utime, &stime);
	fclose(f);
	return utime + stime;
}

/* ── Probe ────────────────────────────────────────────────────────────── */

#define ALIGN_UP(x, n)   (((x) + ((n)-1)) & ~((n)-1))
#define ITERATIONS 200

/* Compute MMA-buffer size + plane stride for the given pixfmt + dims. */
static MI_U32 buf_size_for(int pixfmt, MI_U32 w, MI_U32 h, MI_U32 *stride_out)
{
	MI_U32 stride = 0, size = 0;
	switch (pixfmt) {
	case E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420:
		stride = ALIGN_UP(w, 16);
		size   = stride * h * 3 / 2;
		break;
	case E_MI_SYS_PIXEL_FRAME_ARGB8888:
		stride = ALIGN_UP(w, 16) * 4;
		size   = stride * h;
		break;
	default:
		break;
	}
	if (stride_out) *stride_out = stride;
	return size;
}

/* Fill src buffer with a known top-half-white / bottom-half-black pattern. */
static void fill_src_pattern(int pixfmt, void *va, MI_U32 stride,
	MI_U32 w, MI_U32 h)
{
	(void)w;
	if (pixfmt == E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420) {
		memset(va, 0xFF, stride * (h / 2));                  /* Y top */
		memset((char*)va + stride * (h / 2), 0x00,
		       stride * (h / 2));                             /* Y bot */
		memset((char*)va + stride * h, 0x80,
		       stride * h / 2);                                /* UV neutral */
	} else if (pixfmt == E_MI_SYS_PIXEL_FRAME_ARGB8888) {
		MI_U32 row;
		for (row = 0; row < h / 2; row++)
			memset((char*)va + row * stride, 0xFF, stride);  /* white */
		for (row = h / 2; row < h; row++) {
			MI_U32 col;
			unsigned char *p = (unsigned char*)va + row * stride;
			for (col = 0; col < stride; col += 4) {
				p[col + 0] = 0xFF;  /* alpha */
				p[col + 1] = 0x00;  /* R */
				p[col + 2] = 0x00;  /* G */
				p[col + 3] = 0x00;  /* B */
			}
		}
	}
}

/* Sample top + bottom of dst; return brightness for verification. */
static void sample_dst(int pixfmt, void *va, MI_U32 stride, MI_U32 w, MI_U32 h,
	int *top_out, int *bot_out)
{
	if (pixfmt == E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420) {
		*top_out = ((unsigned char*)va)[stride * 1 + w / 2];
		*bot_out = ((unsigned char*)va)[stride * (h - 2) + w / 2];
	} else if (pixfmt == E_MI_SYS_PIXEL_FRAME_ARGB8888) {
		/* Sample R+G+B average at one pixel — should be ~255 (top) or
		 * ~0 (bottom). Byte order may be A,R,G,B or B,G,R,A; we
		 * average the three non-alpha bytes either way. */
		unsigned char *top = (unsigned char*)va + stride * 1 + (w / 2) * 4;
		unsigned char *bot = (unsigned char*)va + stride * (h - 2) +
		                     (w / 2) * 4;
		*top_out = (top[0] + top[1] + top[2] + top[3]) / 4;
		*bot_out = (bot[0] + bot[1] + bot[2] + bot[3]) / 4;
	}
}

static int pixfmt_threshold_ok(int pixfmt, int top, int bot)
{
	(void)pixfmt;
	return (top > 180 && bot < 80);
}

static const char *pixfmt_name(int pixfmt)
{
	switch (pixfmt) {
	case E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420: return "yuv420sp";
	case E_MI_SYS_PIXEL_FRAME_ARGB8888:           return "argb8888";
	default:                                       return "unknown";
	}
}

/* Run a single benchmark case. Returns 0 on full success, 1 otherwise. */
static int run_case(const char *label,
	int src_pixfmt, MI_U32 src_w, MI_U32 src_h,
	int dst_pixfmt, MI_U32 dst_w, MI_U32 dst_h,
	MI_U32 crop_x, MI_U32 crop_y, MI_U32 crop_w, MI_U32 crop_h)
{
	MI_U32 src_stride = 0, dst_stride = 0;
	MI_U32 src_size = buf_size_for(src_pixfmt, src_w, src_h, &src_stride);
	MI_U32 dst_size = buf_size_for(dst_pixfmt, dst_w, dst_h, &dst_stride);

	MI_PHY src_phy = 0, dst_phy = 0;
	MI_S32 ret = p_mma_alloc((MI_U8*)"mma_heap_name0", src_size, &src_phy);
	if (ret != MI_SUCCESS) {
		printf("{\"case\":\"%s\",\"ok\":false,\"err\":\"src MMA_Alloc\","
		       "\"code\":\"0x%x\",\"size\":%u}\n",
		       label, ret, src_size);
		return 1;
	}
	ret = p_mma_alloc((MI_U8*)"mma_heap_name0", dst_size, &dst_phy);
	if (ret != MI_SUCCESS) {
		printf("{\"case\":\"%s\",\"ok\":false,\"err\":\"dst MMA_Alloc\","
		       "\"code\":\"0x%x\"}\n", label, ret);
		p_mma_free(src_phy);
		return 1;
	}

	void *src_va = NULL, *dst_va = NULL;
	if (p_sys_mmap(src_phy, src_size, &src_va, 0) == MI_SUCCESS && src_va) {
		fill_src_pattern(src_pixfmt, src_va, src_stride, src_w, src_h);
		p_sys_munmap(src_va, src_size);
	}
	if (p_sys_mmap(dst_phy, dst_size, &dst_va, 0) == MI_SUCCESS && dst_va) {
		memset(dst_va, 0x55, dst_size);  /* sentinel */
		p_sys_munmap(dst_va, dst_size);
	}

	MI_DIVP_DirectBuf_t src_buf = {
		.ePixelFormat = src_pixfmt,
		.u32Width = src_w, .u32Height = src_h,
		.u32Stride = { src_stride, src_stride, 0 },
		.phyAddr = { src_phy,
			src_pixfmt == E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420
				? src_phy + src_stride * src_h : 0,
			0 }
	};
	MI_DIVP_DirectBuf_t dst_buf = {
		.ePixelFormat = dst_pixfmt,
		.u32Width = dst_w, .u32Height = dst_h,
		.u32Stride = { dst_stride, dst_stride, 0 },
		.phyAddr = { dst_phy,
			dst_pixfmt == E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420
				? dst_phy + dst_stride * dst_h : 0,
			0 }
	};
	MI_SYS_WindowRect_t crop = {
		.u16X = crop_x, .u16Y = crop_y,
		.u16Width = crop_w, .u16Height = crop_h
	};

	/* Warm-up. */
	ret = p_divp_stretch(&src_buf, &crop, &dst_buf);
	if (ret != MI_SUCCESS) {
		printf("{\"case\":\"%s\",\"ok\":false,"
		       "\"err\":\"DIVP_StretchBuf warmup\",\"code\":\"0x%x\","
		       "\"src\":\"%s %ux%u\",\"dst\":\"%s %ux%u\"}\n",
		       label, ret,
		       pixfmt_name(src_pixfmt), src_w, src_h,
		       pixfmt_name(dst_pixfmt), dst_w, dst_h);
		p_mma_free(src_phy); p_mma_free(dst_phy);
		return 1;
	}

	struct timespec t0, t1;
	unsigned long long jif0 = read_self_jiffies();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	int success = 0; MI_S32 last_err = 0;
	for (int i = 0; i < ITERATIONS; i++) {
		ret = p_divp_stretch(&src_buf, &crop, &dst_buf);
		if (ret == MI_SUCCESS) success++;
		else last_err = ret;
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	unsigned long long jif1 = read_self_jiffies();

	long long ns = (t1.tv_sec - t0.tv_sec) * 1000000000LL +
	               (t1.tv_nsec - t0.tv_nsec);
	long long us_per = ns / ITERATIONS / 1000;
	long long total_ms = ns / 1000000;
	unsigned long long jif_used = jif1 - jif0;

	int dst_ok = 0; int top = -1, bot = -1;
	if (p_sys_mmap(dst_phy, dst_size, &dst_va, 0) == MI_SUCCESS && dst_va) {
		sample_dst(dst_pixfmt, dst_va, dst_stride, dst_w, dst_h,
			&top, &bot);
		dst_ok = pixfmt_threshold_ok(dst_pixfmt, top, bot);
		p_sys_munmap(dst_va, dst_size);
	}

	printf("{\"case\":\"%s\",\"ok\":%s,\"calls\":%d,\"success\":%d,"
	       "\"avg_us\":%lld,\"total_ms\":%lld,\"self_jiffies\":%llu,"
	       "\"src\":\"%s %ux%u\",\"dst\":\"%s %ux%u\","
	       "\"crop\":\"%u,%u %ux%u\","
	       "\"dst_top\":%d,\"dst_bot\":%d,\"dst_pattern_ok\":%s,"
	       "\"last_err\":\"0x%x\"}\n",
	       label,
	       (success == ITERATIONS && dst_ok) ? "true" : "false",
	       ITERATIONS, success, us_per, total_ms, jif_used,
	       pixfmt_name(src_pixfmt), src_w, src_h,
	       pixfmt_name(dst_pixfmt), dst_w, dst_h,
	       crop_x, crop_y, crop_w, crop_h,
	       top, bot, dst_ok ? "true" : "false", last_err);

	p_mma_free(src_phy);
	p_mma_free(dst_phy);
	return (success == ITERATIONS && dst_ok) ? 0 : 1;
}

int main(void)
{
	if (load_syms() != 0) {
		printf("{\"ok\":false,\"err\":\"dlsym\"}\n");
		return 1;
	}

	MI_S32 ret = p_sys_init();
	if (ret != MI_SUCCESS) {
		printf("{\"ok\":false,\"err\":\"MI_SYS_Init\",\"code\":\"0x%x\"}\n", ret);
		return 1;
	}

	int fails = 0;

	/* Case 1: YUV420SP → YUV420SP, 1920x1080 → 480x270 with 960x540 crop.
	 * Baseline — proves DIVP can crop+scale at no host CPU cost. */
	fails += run_case("yuv2yuv",
		E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420, 1920, 1080,
		E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420, 480, 270,
		480, 270, 960, 540);

	/* Case 2: YUV420SP → ARGB8888, same dims.  Tests HW colour-space
	 * conversion — proves "color" PiP path is also free. */
	fails += run_case("yuv2argb",
		E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420, 1920, 1080,
		E_MI_SYS_PIXEL_FRAME_ARGB8888,           480, 270,
		480, 270, 960, 540);

	/* Case 3: ARGB8888 → ARGB8888, 1:1.  Tests pure ARGB blit (no
	 * conversion, no scale) — useful if we composite layered ARGB
	 * via DIVP rather than RGN. */
	fails += run_case("argb2argb",
		E_MI_SYS_PIXEL_FRAME_ARGB8888, 480, 270,
		E_MI_SYS_PIXEL_FRAME_ARGB8888, 480, 270,
		0, 0, 480, 270);

	p_sys_exit();
	return fails ? 1 : 0;
}
