/*
 * tools/divp_probe.c — Verifies the open Q1 questions in
 * documentation/DIVP_STAB_TEST_PLAN.md for the candidate DIVP-backed
 * stabilization path.
 *
 * Q1a (--probe-only):
 *   Resolve MI_DIVP_StretchBuf and the DIVP channel-lifecycle symbols
 *   from libmi_divp.so. Report which are present.
 *
 * Q1b (default):
 *   Allocate two NV12 framebufs via MI_SYS_MMA_Alloc, paint a
 *   horizontal-gradient pattern in the source, call MI_DIVP_StretchBuf
 *   with a centred crop, FlushInvCache the dest, and verify the first
 *   destination scanline matches the expected gradient slice.
 *
 *   --with-chn: create+start a DIVP channel before StretchBuf and destroy
 *               it after. Use this if the no-channel path fails — confirms
 *               the BSP needs an explicit channel.
 *
 * Build (cross):
 *   toolchain/toolchain.sigmastar-infinity6e/bin/arm-openipc-linux-gnueabihf-gcc \
 *     -std=c99 -Wall -Wextra -O2 -D_GNU_SOURCE \
 *     tools/divp_probe.c -ldl -o divp_probe
 *
 * Run on target:
 *   /tmp/divp_probe --probe-only
 *   /tmp/divp_probe
 *   /tmp/divp_probe --with-chn
 *
 * Exit codes:
 *   0  success / all expected symbols present
 *   1  required symbol missing
 *   2  MI_SYS_Init / MMA alloc failed
 *   3  MI_DIVP_StretchBuf returned non-zero
 *   4  output pixel mismatch (DIVP returned 0 but wrote wrong data)
 */

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef int32_t  MI_S32;
typedef uint32_t MI_U32;
typedef uint16_t MI_U16;
typedef uint8_t  MI_U8;
typedef uint64_t MI_U64;
typedef int      MI_BOOL;

typedef MI_U64 MI_PHY;

typedef struct {
	MI_U16 u16X;
	MI_U16 u16Y;
	MI_U16 u16Width;
	MI_U16 u16Height;
} MI_SYS_WindowRect_t;

/* Pixel format constant for NV12 (YUV420SP).  Sigmastar I6E ABI:
 * MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420 = 0x10. */
#define DIVP_PIXFMT_NV12 0x10

typedef struct {
	int ePixelFormat;
	MI_U32 u32Width;
	MI_U32 u32Height;
	MI_U32 u32Stride[3];
	MI_PHY phyAddr[3];
} MI_DIVP_DirectBuf_t;

typedef MI_S32 (*fn_sys_init_t)(void);
typedef MI_S32 (*fn_sys_init_one_t)(MI_U32);
typedef MI_S32 (*fn_sys_exit_t)(void);
typedef MI_S32 (*fn_sys_mma_alloc_t)(const char *mma, MI_U32 size, MI_PHY *phy);
typedef MI_S32 (*fn_sys_mma_alloc_named_t)(MI_U16 dev, const char *mma,
	MI_U32 size, MI_PHY *phy);
typedef MI_S32 (*fn_sys_mma_free_t)(MI_PHY phy);
typedef MI_S32 (*fn_sys_va2pa_t)(void *vir, MI_PHY *phy);
typedef MI_S32 (*fn_sys_pa2va_t)(MI_PHY phy, void **vir);
typedef MI_S32 (*fn_sys_mmap_t)(MI_PHY phy, MI_U32 size, void **vir, MI_BOOL cached);
typedef MI_S32 (*fn_sys_munmap_t)(void *vir, MI_U32 size);
typedef MI_S32 (*fn_sys_flush_t)(void *vir, MI_U32 size);

typedef MI_S32 (*fn_divp_init_dev_t)(void);
typedef MI_S32 (*fn_divp_deinit_dev_t)(void);
typedef MI_S32 (*fn_divp_create_chn_t)(MI_U32 chn, void *attr);
typedef MI_S32 (*fn_divp_start_chn_t)(MI_U32 chn);
typedef MI_S32 (*fn_divp_stop_chn_t)(MI_U32 chn);
typedef MI_S32 (*fn_divp_destroy_chn_t)(MI_U32 chn);
typedef MI_S32 (*fn_divp_stretch_buf_t)(MI_DIVP_DirectBuf_t *src,
	MI_SYS_WindowRect_t *crop, MI_DIVP_DirectBuf_t *dst);

static void *h_sys;
static void *h_divp;

static fn_sys_init_t       sys_init;
static fn_sys_init_one_t   sys_init_one;
static fn_sys_exit_t       sys_exit;
static fn_sys_mma_alloc_t  sys_mma_alloc;
static fn_sys_mma_alloc_named_t sys_mma_alloc_named;
static fn_sys_mma_free_t   sys_mma_free;
static fn_sys_mmap_t       sys_mmap;
static fn_sys_munmap_t     sys_munmap;
static fn_sys_va2pa_t      sys_va2pa;
static fn_sys_pa2va_t      sys_pa2va;
static fn_sys_flush_t      sys_flush;

static fn_divp_init_dev_t    divp_init_dev;
static fn_divp_deinit_dev_t  divp_deinit_dev;
static fn_divp_create_chn_t  divp_create_chn;
static fn_divp_start_chn_t   divp_start_chn;
static fn_divp_stop_chn_t    divp_stop_chn;
static fn_divp_destroy_chn_t divp_destroy_chn;
static fn_divp_stretch_buf_t divp_stretch_buf;

static int load_sys(void)
{
	h_sys = dlopen("libmi_sys.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!h_sys) {
		fprintf(stderr, "[divp_probe] dlopen libmi_sys.so failed: %s\n",
			dlerror());
		return -1;
	}

	sys_init            = (fn_sys_init_t)           dlsym(h_sys, "MI_SYS_Init");
	sys_init_one        = (fn_sys_init_one_t)       dlsym(h_sys, "MI_SYS_Init");
	sys_exit            = (fn_sys_exit_t)           dlsym(h_sys, "MI_SYS_Exit");
	sys_mma_alloc       = (fn_sys_mma_alloc_t)      dlsym(h_sys, "MI_SYS_MMA_Alloc");
	sys_mma_alloc_named = (fn_sys_mma_alloc_named_t)dlsym(h_sys, "MI_SYS_MMA_Alloc");
	sys_mma_free        = (fn_sys_mma_free_t)       dlsym(h_sys, "MI_SYS_MMA_Free");
	sys_mmap            = (fn_sys_mmap_t)           dlsym(h_sys, "MI_SYS_Mmap");
	sys_munmap          = (fn_sys_munmap_t)         dlsym(h_sys, "MI_SYS_Munmap");
	sys_va2pa           = (fn_sys_va2pa_t)          dlsym(h_sys, "MI_SYS_Va2Pa");
	sys_pa2va           = (fn_sys_pa2va_t)          dlsym(h_sys, "MI_SYS_Pa2Va");
	sys_flush           = (fn_sys_flush_t)          dlsym(h_sys, "MI_SYS_FlushInvCache");

	return 0;
}

static int load_divp(void)
{
	h_divp = dlopen("libmi_divp.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!h_divp)
		h_divp = dlopen("libdivp.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!h_divp) {
		fprintf(stderr, "[divp_probe] dlopen libmi_divp.so / libdivp.so "
			"failed: %s\n", dlerror());
		return -1;
	}

	divp_init_dev    = (fn_divp_init_dev_t)   dlsym(h_divp, "MI_DIVP_InitDev");
	divp_deinit_dev  = (fn_divp_deinit_dev_t) dlsym(h_divp, "MI_DIVP_DeInitDev");
	divp_create_chn  = (fn_divp_create_chn_t) dlsym(h_divp, "MI_DIVP_CreateChn");
	divp_start_chn   = (fn_divp_start_chn_t)  dlsym(h_divp, "MI_DIVP_StartChn");
	divp_stop_chn    = (fn_divp_stop_chn_t)   dlsym(h_divp, "MI_DIVP_StopChn");
	divp_destroy_chn = (fn_divp_destroy_chn_t)dlsym(h_divp, "MI_DIVP_DestroyChn");
	divp_stretch_buf = (fn_divp_stretch_buf_t)dlsym(h_divp, "MI_DIVP_StretchBuf");

	return 0;
}

#define REPORT(name, ptr) \
	printf("  %-22s %s\n", name, (ptr) ? "PRESENT" : "MISSING")

static int report_symbols(void)
{
	puts("[divp_probe] libmi_sys.so symbols:");
	REPORT("MI_SYS_Init",          sys_init);
	REPORT("MI_SYS_Exit",          sys_exit);
	REPORT("MI_SYS_MMA_Alloc",     sys_mma_alloc);
	REPORT("MI_SYS_MMA_Free",      sys_mma_free);
	REPORT("MI_SYS_Mmap",          sys_mmap);
	REPORT("MI_SYS_Munmap",        sys_munmap);
	REPORT("MI_SYS_Va2Pa",         sys_va2pa);
	REPORT("MI_SYS_Pa2Va",         sys_pa2va);
	REPORT("MI_SYS_FlushInvCache", sys_flush);

	puts("[divp_probe] libmi_divp.so symbols:");
	REPORT("MI_DIVP_InitDev",      divp_init_dev);
	REPORT("MI_DIVP_DeInitDev",    divp_deinit_dev);
	REPORT("MI_DIVP_CreateChn",    divp_create_chn);
	REPORT("MI_DIVP_StartChn",     divp_start_chn);
	REPORT("MI_DIVP_StopChn",      divp_stop_chn);
	REPORT("MI_DIVP_DestroyChn",   divp_destroy_chn);
	REPORT("MI_DIVP_StretchBuf",   divp_stretch_buf);

	if (!divp_stretch_buf) {
		puts("[divp_probe] FAIL Q1a: MI_DIVP_StretchBuf is missing — "
		     "the candidate path will not link/run on this BSP.");
		return 1;
	}
	if (!sys_mma_alloc || !sys_mma_free || !sys_flush) {
		puts("[divp_probe] FAIL Q1b: libmi_sys helpers needed for the "
		     "stretch test are missing.");
		return 1;
	}
	puts("[divp_probe] PASS Q1a: MI_DIVP_StretchBuf is resolvable.");
	return 0;
}

/* MI_SYS_Init has two known ABI shapes across versions: zero-arg and
 * single-arg (device id). Try the no-arg form first. */
static MI_S32 sys_init_compat(void)
{
	MI_S32 ret;

	if (sys_init) {
		ret = sys_init();
		if (ret == 0)
			return 0;
	}
	if (sys_init_one) {
		ret = sys_init_one(0);
		if (ret == 0)
			return 0;
	}
	return -1;
}

static MI_S32 sys_mma_alloc_compat(MI_U32 size, MI_PHY *phy)
{
	MI_S32 ret;

	if (sys_mma_alloc_named) {
		ret = sys_mma_alloc_named(0, "#nocache_divp_probe", size, phy);
		if (ret == 0 && *phy)
			return 0;
	}
	if (sys_mma_alloc) {
		ret = sys_mma_alloc("#nocache_divp_probe", size, phy);
		if (ret == 0 && *phy)
			return 0;
	}
	return -1;
}

static int paint_src_gradient(uint8_t *vir, MI_U32 stride,
	MI_U32 w, MI_U32 h)
{
	MI_U32 y, x;

	for (y = 0; y < h; y++) {
		uint8_t *row = vir + y * stride;
		for (x = 0; x < w; x++)
			row[x] = (uint8_t)x;
	}
	/* UV plane: neutral grey (NV12: chroma plane is h/2 rows of
	 * interleaved Cb/Cr at 128). */
	for (y = 0; y < h / 2; y++) {
		uint8_t *row = vir + (h + y) * stride;
		memset(row, 128, w);
	}
	return 0;
}

static int verify_dst_row(uint8_t *vir, MI_U32 stride, MI_U32 dst_w,
	MI_U32 src_crop_x, MI_U32 src_crop_w)
{
	MI_U32 x;
	int mismatches = 0;

	/* DIVP with src_crop_w == dst_w should be a pure crop; pixel x of dst
	 * row 0 should equal (src_crop_x + x) & 0xff. With stretching the
	 * mapping is bilinear and we just sanity-check monotonicity. */
	printf("[divp_probe] dst row 0 (first 16): ");
	for (x = 0; x < 16 && x < dst_w; x++)
		printf("%02x ", vir[x]);
	printf("\n");

	if (src_crop_w == dst_w) {
		for (x = 0; x < dst_w; x++) {
			uint8_t expected = (uint8_t)((src_crop_x + x) & 0xff);
			if (vir[x] != expected) {
				if (mismatches < 4)
					printf("[divp_probe] mismatch x=%u "
						"got 0x%02x want 0x%02x\n",
						x, vir[x], expected);
				mismatches++;
			}
		}
	} else {
		uint8_t prev = vir[0];
		MI_U32 wraps = 0;
		for (x = 1; x < dst_w; x++) {
			if (vir[x] < prev)
				wraps++;
			prev = vir[x];
		}
		printf("[divp_probe] stretch sanity: row-0 wraps=%u "
			"(expect approximately src_crop_w/256)\n",
			(unsigned)wraps);
	}

	(void)stride;
	return mismatches;
}

static int run_stretch_test(int with_chn)
{
	const MI_U32 src_w = 1280;
	const MI_U32 src_h = 720;
	const MI_U32 dst_w = 1024;
	const MI_U32 dst_h = 576;
	const MI_U32 src_stride = src_w; /* NV12, no padding */
	const MI_U32 dst_stride = dst_w;
	const MI_U32 src_bufsize = src_stride * src_h * 3 / 2;
	const MI_U32 dst_bufsize = dst_stride * dst_h * 3 / 2;

	MI_PHY src_phy = 0;
	MI_PHY dst_phy = 0;
	void *src_vir = NULL;
	void *dst_vir = NULL;
	MI_DIVP_DirectBuf_t src_buf;
	MI_DIVP_DirectBuf_t dst_buf;
	MI_SYS_WindowRect_t crop;
	MI_U32 crop_x;
	MI_U32 crop_y;
	MI_S32 ret;
	int chn_created = 0;
	int rc = 0;

	if (sys_init_compat() != 0) {
		fprintf(stderr, "[divp_probe] MI_SYS_Init failed\n");
		return 2;
	}

	if (with_chn) {
		if (divp_init_dev) {
			ret = divp_init_dev();
			printf("[divp_probe] MI_DIVP_InitDev -> %d\n", ret);
		}
		if (divp_create_chn) {
			uint8_t attr[256] = {0};
			ret = divp_create_chn(0, attr);
			printf("[divp_probe] MI_DIVP_CreateChn(0) -> %d\n", ret);
			if (ret == 0)
				chn_created = 1;
		}
		if (chn_created && divp_start_chn) {
			ret = divp_start_chn(0);
			printf("[divp_probe] MI_DIVP_StartChn(0) -> %d\n", ret);
		}
	}

	if (sys_mma_alloc_compat(src_bufsize, &src_phy) != 0) {
		fprintf(stderr, "[divp_probe] MMA_Alloc src failed\n");
		rc = 2;
		goto out;
	}
	if (sys_mma_alloc_compat(dst_bufsize, &dst_phy) != 0) {
		fprintf(stderr, "[divp_probe] MMA_Alloc dst failed\n");
		rc = 2;
		goto out;
	}

	if (sys_mmap) {
		ret = sys_mmap(src_phy, src_bufsize, &src_vir, 0);
		if (ret != 0 || !src_vir) {
			fprintf(stderr, "[divp_probe] mmap src failed %d\n", ret);
			rc = 2;
			goto out;
		}
		ret = sys_mmap(dst_phy, dst_bufsize, &dst_vir, 0);
		if (ret != 0 || !dst_vir) {
			fprintf(stderr, "[divp_probe] mmap dst failed %d\n", ret);
			rc = 2;
			goto out;
		}
	} else if (sys_pa2va) {
		ret = sys_pa2va(src_phy, &src_vir);
		if (ret != 0 || !src_vir) { rc = 2; goto out; }
		ret = sys_pa2va(dst_phy, &dst_vir);
		if (ret != 0 || !dst_vir) { rc = 2; goto out; }
	} else {
		fprintf(stderr, "[divp_probe] no mmap helper available\n");
		rc = 2;
		goto out;
	}

	paint_src_gradient((uint8_t *)src_vir, src_stride, src_w, src_h);
	if (sys_flush)
		sys_flush(src_vir, src_bufsize);

	memset(dst_vir, 0xaa, dst_bufsize);
	if (sys_flush)
		sys_flush(dst_vir, dst_bufsize);

	memset(&src_buf, 0, sizeof(src_buf));
	memset(&dst_buf, 0, sizeof(dst_buf));
	memset(&crop, 0, sizeof(crop));

	src_buf.ePixelFormat = DIVP_PIXFMT_NV12;
	src_buf.u32Width  = src_w;
	src_buf.u32Height = src_h;
	src_buf.u32Stride[0] = src_stride;
	src_buf.u32Stride[1] = src_stride;
	src_buf.phyAddr[0] = src_phy;
	src_buf.phyAddr[1] = src_phy + (MI_PHY)src_stride * src_h;

	dst_buf.ePixelFormat = DIVP_PIXFMT_NV12;
	dst_buf.u32Width  = dst_w;
	dst_buf.u32Height = dst_h;
	dst_buf.u32Stride[0] = dst_stride;
	dst_buf.u32Stride[1] = dst_stride;
	dst_buf.phyAddr[0] = dst_phy;
	dst_buf.phyAddr[1] = dst_phy + (MI_PHY)dst_stride * dst_h;

	/* Pure crop: take an identically-sized window from the centre. */
	crop_x = (src_w - dst_w) / 2;
	crop_y = (src_h - dst_h) / 2;
	crop.u16X = (MI_U16)crop_x;
	crop.u16Y = (MI_U16)crop_y;
	crop.u16Width  = (MI_U16)dst_w;
	crop.u16Height = (MI_U16)dst_h;

	printf("[divp_probe] StretchBuf src=%ux%u dst=%ux%u crop=%u,%u %ux%u "
		"src_phy=0x%llx dst_phy=0x%llx with_chn=%d\n",
		src_w, src_h, dst_w, dst_h,
		(unsigned)crop.u16X, (unsigned)crop.u16Y,
		(unsigned)crop.u16Width, (unsigned)crop.u16Height,
		(unsigned long long)src_phy, (unsigned long long)dst_phy,
		with_chn);

	ret = divp_stretch_buf(&src_buf, &crop, &dst_buf);
	printf("[divp_probe] MI_DIVP_StretchBuf -> %d (0x%x)\n", ret, ret);
	if (ret != 0) {
		printf("[divp_probe] FAIL Q1b: StretchBuf returned non-zero%s\n",
			with_chn ? " even with a channel created"
			         : "; retry with --with-chn");
		rc = 3;
		goto out;
	}

	if (sys_flush)
		sys_flush(dst_vir, dst_bufsize);

	if (verify_dst_row((uint8_t *)dst_vir, dst_stride, dst_w,
			   crop_x, crop.u16Width) != 0) {
		puts("[divp_probe] FAIL Q1b: destination pixels do not match "
		     "the source crop.");
		rc = 4;
		goto out;
	}

	printf("[divp_probe] PASS Q1b: StretchBuf without channel-create %s\n",
		with_chn ? "(channel was created — direct-buf path NEEDS one)"
		         : "(direct-buf path does NOT need a channel)");

out:
	if (src_vir && sys_munmap)
		sys_munmap(src_vir, src_bufsize);
	if (dst_vir && sys_munmap)
		sys_munmap(dst_vir, dst_bufsize);
	if (src_phy && sys_mma_free)
		sys_mma_free(src_phy);
	if (dst_phy && sys_mma_free)
		sys_mma_free(dst_phy);

	if (chn_created) {
		if (divp_stop_chn)    divp_stop_chn(0);
		if (divp_destroy_chn) divp_destroy_chn(0);
	}
	if (with_chn && divp_deinit_dev)
		divp_deinit_dev();
	if (sys_exit)
		sys_exit();
	return rc;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s [--probe-only] [--with-chn]\n"
		"  --probe-only   only dlsym-check symbols, do not allocate "
		"or stretch (Q1a)\n"
		"  --with-chn     create+start a DIVP channel before "
		"StretchBuf (Q1b fallback)\n",
		argv0);
}

int main(int argc, char **argv)
{
	int probe_only = 0;
	int with_chn = 0;
	int rc;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--probe-only"))
			probe_only = 1;
		else if (!strcmp(argv[i], "--with-chn"))
			with_chn = 1;
		else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage(argv[0]);
			return 0;
		} else {
			fprintf(stderr, "[divp_probe] unknown arg: %s\n", argv[i]);
			usage(argv[0]);
			return 1;
		}
	}

	if (load_sys() != 0)
		return 1;
	if (load_divp() != 0)
		return 1;

	rc = report_symbols();
	if (rc != 0 || probe_only)
		return rc;

	return run_stretch_test(with_chn);
}
