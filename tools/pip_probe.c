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

/* From mi_sys_datatype.h: YUV420SP is enum index 11. */
#define E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420 11

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

#define SRC_W 1920
#define SRC_H 1080
#define DST_W 480
#define DST_H 270
#define ITERATIONS 200

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

	const MI_U32 src_stride = ALIGN_UP(SRC_W, 16);
	const MI_U32 src_size   = src_stride * SRC_H * 3 / 2;
	const MI_U32 dst_stride = ALIGN_UP(DST_W, 16);
	const MI_U32 dst_size   = dst_stride * DST_H * 3 / 2;

	MI_PHY src_phy = 0, dst_phy = 0;
	ret = p_mma_alloc((MI_U8*)"mma_heap_name0", src_size, &src_phy);
	if (ret != MI_SUCCESS) {
		printf("{\"ok\":false,\"err\":\"src MMA_Alloc\",\"code\":\"0x%x\","
		       "\"size\":%u}\n", ret, src_size);
		p_sys_exit(); return 1;
	}
	ret = p_mma_alloc((MI_U8*)"mma_heap_name0", dst_size, &dst_phy);
	if (ret != MI_SUCCESS) {
		printf("{\"ok\":false,\"err\":\"dst MMA_Alloc\",\"code\":\"0x%x\"}\n", ret);
		p_mma_free(src_phy); p_sys_exit(); return 1;
	}

	/* Fill source: top half Y=255 (white), bottom half Y=0 (black),
	 * UV neutral (128). DIVP should produce the same banding in dst. */
	void *src_va = NULL;
	if (p_sys_mmap(src_phy, src_size, &src_va, 0) == MI_SUCCESS && src_va) {
		memset(src_va, 0xFF, src_stride * (SRC_H / 2));        /* Y top */
		memset((char*)src_va + src_stride * (SRC_H / 2), 0x00,
		       src_stride * (SRC_H / 2));                       /* Y bot */
		memset((char*)src_va + src_stride * SRC_H, 0x80,
		       src_stride * SRC_H / 2);                          /* UV neutral */
		p_sys_munmap(src_va, src_size);
	}

	/* Pre-zero dst so we can detect change. */
	void *dst_va = NULL;
	if (p_sys_mmap(dst_phy, dst_size, &dst_va, 0) == MI_SUCCESS && dst_va) {
		memset(dst_va, 0x55, dst_size); /* sentinel */
		p_sys_munmap(dst_va, dst_size);
	}

	MI_DIVP_DirectBuf_t src_buf = {
		.ePixelFormat = E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420,
		.u32Width = SRC_W, .u32Height = SRC_H,
		.u32Stride = { src_stride, src_stride, 0 },
		.phyAddr = { src_phy, src_phy + src_stride * SRC_H, 0 }
	};
	MI_DIVP_DirectBuf_t dst_buf = {
		.ePixelFormat = E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420,
		.u32Width = DST_W, .u32Height = DST_H,
		.u32Stride = { dst_stride, dst_stride, 0 },
		.phyAddr = { dst_phy, dst_phy + dst_stride * DST_H, 0 }
	};
	/* Crop: take the centre 960x540 region of src, scaled into 480x270 dst. */
	MI_SYS_WindowRect_t crop = {
		.u16X = 480, .u16Y = 270,
		.u16Width = 960, .u16Height = 540
	};

	/* Warm-up — first call may include lazy init in the kernel driver. */
	ret = p_divp_stretch(&src_buf, &crop, &dst_buf);
	if (ret != MI_SUCCESS) {
		printf("{\"ok\":false,\"err\":\"DIVP_StretchBuf warmup\","
		       "\"code\":\"0x%x\"}\n", ret);
		p_mma_free(src_phy); p_mma_free(dst_phy); p_sys_exit();
		return 1;
	}

	/* Timed loop. */
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

	/* Verify dst was actually written: sample top row Y plane and
	 * bottom row Y plane — should be ~255 and ~0 respectively. */
	int dst_ok = 0; int top_y = -1, bot_y = -1;
	if (p_sys_mmap(dst_phy, dst_size, &dst_va, 0) == MI_SUCCESS && dst_va) {
		top_y = ((unsigned char*)dst_va)[dst_stride * 1 + DST_W / 2];
		bot_y = ((unsigned char*)dst_va)[dst_stride * (DST_H - 2) + DST_W / 2];
		dst_ok = (top_y > 200 && bot_y < 50);
		p_sys_munmap(dst_va, dst_size);
	}

	printf("{\"ok\":%s,\"calls\":%d,\"success\":%d,"
	       "\"avg_us\":%lld,\"total_ms\":%lld,\"self_jiffies\":%llu,"
	       "\"src\":\"%ux%u\",\"dst\":\"%ux%u\",\"crop\":\"%u,%u %ux%u\","
	       "\"dst_top_y\":%d,\"dst_bot_y\":%d,\"dst_pattern_ok\":%s,"
	       "\"last_err\":\"0x%x\"}\n",
	       (success == ITERATIONS && dst_ok) ? "true" : "false",
	       ITERATIONS, success, us_per, total_ms, jif_used,
	       SRC_W, SRC_H, DST_W, DST_H,
	       crop.u16X, crop.u16Y, crop.u16Width, crop.u16Height,
	       top_y, bot_y, dst_ok ? "true" : "false", last_err);

	p_mma_free(src_phy);
	p_mma_free(dst_phy);
	p_sys_exit();
	return (success == ITERATIONS && dst_ok) ? 0 : 1;
}
