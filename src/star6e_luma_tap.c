/*
 * star6e_luma_tap.c — read-only NV12 luma tap on VPE port1 (Star6E).
 *
 * Supplies overlay-free frames to a vision consumer.  MI_RGN composites per
 * scaler output port, and every overlay producer in the system (debug_osd here,
 * osd_render in waybeam-hub) attaches to port0 — which the H.265 encoder, the
 * MJPEG snapshot channel and the recorder all share 1:N.  port1 is a separate
 * scaler output and carries no overlay.
 *
 * Two rules shape this file, both learned from the retired /api/v1/snapshot.pgm
 * (PR #205):
 *
 *   1. The port is programmed and enabled ONCE per pipeline run and disabled
 *      only at teardown.  snapshot.pgm cycled Enable/Disable per HTTP request;
 *      DisablePort races an in-flight mhal buffer, jams the VPE input FIFO
 *      (`EnsureInputPortFifoEmpty ... no response in 1000ms!`) and ends in a
 *      kernel panic or a hard hang — about twice per 560 stressed captures.
 *      The race is kernel-side; not cycling the port is the only fix.
 *
 *   2. The reader thread drains EVERY frame at line rate and copies the luma
 *      plane out only when a grab is pending.  An enabled-but-undrained port is
 *      dangerous on this BSP (the port2 probe stalled port0 with no consumer),
 *      and a slow consumer must never sit between GetBuf and PutBuf.
 *
 * Teardown order is load-bearing: park the reader outside the GetBuf/PutBuf
 * window, join it, reset the output depth, THEN DisablePort.  Leaving the depth
 * registered makes the kernel SCL keep queueing port1 output tasks for a
 * consumer that no longer exists, and a successor process inherits a stale
 * queue whose fence never completes.
 */

#include "star6e_luma_tap.h"
#include "star6e.h"
#include "star6e_vpe_ports.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#define LT_OWNER      "qr"
#define LT_PORT       1
#define LT_MAX_DIM    4096u
/* Refuse a tap whose luma plane alone would dominate a 64-128 MB target. */
#define LT_MAX_PIXELS (8u * 1024u * 1024u)

/* ---- local MI_SYS output-buffer mirror.  Layout copied from the proven
 * definition in star6e_ipu_yolo.c; only the fields we read are named. ------ */
typedef MI_U64 LtPhy;

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
	LtPhy phyAddr[3];
	MI_U32 u32Stride[3];
	MI_U32 u32BufSize;
	MI_U16 u16RingBufStartLine;
	MI_U16 u16RingBufRealTotalHeight;
	struct {
		int eType;
		union { MI_U32 u32GlobalGradient; } uIspInfo;
	} stFrameIspInfo;
	MI_U8 reserved_rect[16];
} LtFrameData_t;

typedef struct {
	MI_U64 u64Pts;
	MI_U64 u64SidebandMsg;
	int eBufType;
	MI_BOOL bEndOfStream;
	MI_BOOL bUsrBuf;
	MI_U32 u32SequenceNumber;
	MI_BOOL bDrop;
	union {
		LtFrameData_t stFrameData;
		MI_U8 reserved_union[512];
	};
	MI_U8 u8CusFlag;
} LtBufInfo_t;

typedef MI_S32 LtBufHandle_t;

#define LT_E_BUFDATA_FRAME 1

typedef MI_S32 (*lt_get_fd_fn)(MI_SYS_ChnPort_t *port, MI_S32 *fd);
typedef MI_S32 (*lt_close_fd_fn)(MI_S32 fd);
typedef MI_S32 (*lt_get_buf_fn)(MI_SYS_ChnPort_t *port, LtBufInfo_t *buf,
	LtBufHandle_t *handle);
typedef MI_S32 (*lt_put_buf_fn)(LtBufHandle_t handle);
typedef MI_S32 (*lt_mmap_fn)(MI_U64 phy, MI_U32 size, void **vir, MI_U8 cached);
typedef MI_S32 (*lt_munmap_fn)(void *vir, MI_U32 size);

/* One pipeline per process, so module state is static rather than heap-hung. */
static struct {
	int              running;        /* tap started (port owned)            */
	int              port_enabled;
	int              claimed;
	MI_SYS_ChnPort_t port;           /* {VPE,0,0,1}                         */
	uint32_t         w, h;           /* tap geometry actually programmed    */

	pthread_t             reader;
	int                   reader_started;
	volatile sig_atomic_t reader_run;
	volatile int          pause;
	volatile int          parked;

	/* Grab handshake.  `lock` guards grab_pending/latch_valid and publishes
	 * the latch; `api_lock` serializes concurrent HTTP callers so only one
	 * grab is ever outstanding. */
	pthread_mutex_t lock;
	pthread_cond_t  cond;
	pthread_mutex_t api_lock;
	int             grab_pending;
	int             latch_valid;
	uint8_t        *latch;           /* w*h, tightly packed, stride removed */

	lt_get_fd_fn   get_fd;
	lt_close_fd_fn close_fd;
	lt_get_buf_fn  get_buf;
	lt_put_buf_fn  put_buf;
	lt_mmap_fn     mmap_fn;
	lt_munmap_fn   munmap_fn;
} g = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.cond = PTHREAD_COND_INITIALIZER,
	.api_lock = PTHREAD_MUTEX_INITIALIZER,
};

static int lt_load_sys_symbols(void)
{
	/* libmi_sys stays resident for the whole run; open once and cache, so a
	 * SIGHUP reinit cycle does not bump the refcount unboundedly. */
	static void *sys_h;

	if (!sys_h) {
		sys_h = dlopen("libmi_sys.so", RTLD_LAZY | RTLD_GLOBAL);
		if (!sys_h)
			return -1;
	}
	g.get_fd = (lt_get_fd_fn)dlsym(sys_h, "MI_SYS_GetFd");
	g.close_fd = (lt_close_fd_fn)dlsym(sys_h, "MI_SYS_CloseFd");
	g.get_buf = (lt_get_buf_fn)dlsym(sys_h, "MI_SYS_ChnOutputPortGetBuf");
	g.put_buf = (lt_put_buf_fn)dlsym(sys_h, "MI_SYS_ChnOutputPortPutBuf");
	g.mmap_fn = (lt_mmap_fn)dlsym(sys_h, "MI_SYS_Mmap");
	g.munmap_fn = (lt_munmap_fn)dlsym(sys_h, "MI_SYS_Munmap");
	if (!g.get_buf || !g.put_buf || !g.mmap_fn || !g.munmap_fn)
		return -1;
	return 0;
}

/* Copy the luma plane of one frame into the latch, removing stride.  Runs on
 * the reader thread between GetBuf and PutBuf, so it is kept to a mapped
 * row-wise copy and nothing else. */
static int lt_latch_frame(const LtFrameData_t *fr)
{
	uint32_t stride = fr->u32Stride[0] ? fr->u32Stride[0] : fr->u16Width;
	uint32_t rows = fr->u16Height;
	uint32_t cols = fr->u16Width;
	MI_U64 phy = fr->phyAddr[0];
	void *vir = NULL;

	/* The programmed geometry is what the latch was sized for; a frame that
	 * disagrees is copied only up to the smaller of the two. */
	if (rows > g.h) rows = g.h;
	if (cols > g.w) cols = g.w;
	if (!phy || !rows || !cols)
		return -EIO;

	/* Map non-cached (flag 0) so reads see the latest DMA without an
	 * explicit invalidate.  The source is never written. */
	if (g.mmap_fn(phy, stride * fr->u16Height, &vir, 0) != 0 || !vir)
		return -EIO;

	{
		const uint8_t *src = (const uint8_t *)vir;
		for (uint32_t row = 0; row < rows; ++row)
			memcpy(g.latch + (size_t)row * g.w,
			       src + (size_t)row * stride, cols);
	}
	g.munmap_fn(vir, stride * fr->u16Height);
	return 0;
}

static void *lt_reader_main(void *arg)
{
	MI_S32 fd = -1;

	(void)arg;
	if (g.get_fd && g.get_fd(&g.port, &fd) != 0)
		fd = -1;

	while (g.reader_run) {
		LtBufInfo_t buf;
		LtBufHandle_t handle = 0;
		int want;

		/* Quiesce handshake: park outside GetBuf/PutBuf so stop() can
		 * DisablePort without racing the drain. */
		if (g.pause) {
			g.parked = 1;
			usleep(2000);
			continue;
		}
		g.parked = 0;

		if (fd >= 0) {
			fd_set rfds;
			struct timeval tv;
			FD_ZERO(&rfds);
			FD_SET(fd, &rfds);
			tv.tv_sec = 0;
			tv.tv_usec = 50000;
			if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0 ||
			    !FD_ISSET(fd, &rfds))
				continue;
		}

		memset(&buf, 0, sizeof(buf));
		if (g.get_buf(&g.port, &buf, &handle) != 0) {
			if (fd < 0)
				usleep(1000);
			continue;
		}

		if (buf.eBufType != LT_E_BUFDATA_FRAME ||
		    !buf.stFrameData.phyAddr[0]) {
			g.put_buf(handle);
			continue;
		}

		pthread_mutex_lock(&g.lock);
		want = g.grab_pending;
		pthread_mutex_unlock(&g.lock);

		if (want) {
			int rc = lt_latch_frame(&buf.stFrameData);
			pthread_mutex_lock(&g.lock);
			g.grab_pending = 0;
			g.latch_valid = (rc == 0);
			pthread_cond_broadcast(&g.cond);
			pthread_mutex_unlock(&g.lock);
		}

		/* Unconditional: every frame is returned immediately, whether or
		 * not it was copied.  This is what keeps the port drained. */
		g.put_buf(handle);
	}

	if (fd >= 0 && g.close_fd)
		g.close_fd(fd);
	return NULL;
}

/* Release the port: depth reset THEN disable, in that order.  Every path that
 * enabled the port comes through here so the two calls cannot drift apart. */
static void lt_port_teardown(void)
{
	MI_S32 dret, rret;

	if (!g.port_enabled)
		return;
	rret = MI_SYS_SetChnOutputPortDepth(&g.port, 0, 0);
	dret = MI_VPE_DisablePort(0, LT_PORT);
	if (rret != 0 || dret != 0)
		fprintf(stderr, "[luma-tap] port1 teardown: depth_reset=%d "
			"disable=%d\n", (int)rret, (int)dret);
	g.port_enabled = 0;
}

int star6e_luma_tap_start(const VencConfig *cfg, uint32_t main_w,
	uint32_t main_h)
{
	MI_VPE_PortAttr_t port;
	MI_S32 ret;
	uint32_t w, h;

	if (!cfg || !cfg->qr.tap_enabled)
		return 0;
	if (g.running)
		return 0;

	w = cfg->qr.tap_width ? cfg->qr.tap_width : main_w;
	h = cfg->qr.tap_height ? cfg->qr.tap_height : main_h;
	/* Even dimensions: the tap is NV12 and a 4:2:0 chroma plane needs them,
	 * even though only luma is read. */
	w &= ~1u;
	h &= ~1u;
	if (w == 0 || h == 0 || w > LT_MAX_DIM || h > LT_MAX_DIM ||
	    (uint64_t)w * h > LT_MAX_PIXELS) {
		fprintf(stderr, "[luma-tap] refusing geometry %ux%u\n",
			cfg->qr.tap_width, cfg->qr.tap_height);
		return -1;
	}

	/* Lowest-priority claimant: stab and detect have already had their turn
	 * by the time the pipeline calls us, and losing is non-fatal. */
	if (star6e_vpe_port1_claim(LT_OWNER) != 0) {
		fprintf(stderr, "[luma-tap] skipped — VPE port1 held by '%s'\n",
			star6e_vpe_port1_owner());
		return -1;
	}
	g.claimed = 1;

	if (lt_load_sys_symbols() != 0) {
		fprintf(stderr, "[luma-tap] MI_SYS symbols unavailable\n");
		goto fail;
	}

	g.latch = malloc((size_t)w * h);
	if (!g.latch) {
		fprintf(stderr, "[luma-tap] latch alloc %ux%u failed\n", w, h);
		goto fail;
	}

	g.port = (MI_SYS_ChnPort_t){
		.module = I6_SYS_MOD_VPE, .device = 0, .channel = 0,
		.port = LT_PORT };

	/* Full-frame scale to the tap geometry.  MI_VPE_SetPortCrop is
	 * deliberately never called: it is sticky on i6e and a rect left behind
	 * poisons a later detect run on the same port. */
	memset(&port, 0, sizeof(port));
	port.output.width = (unsigned short)w;
	port.output.height = (unsigned short)h;
	port.pixFmt = I6_PIXFMT_YUV420SP;
	port.compress = I6_COMPR_NONE;
	ret = MI_VPE_SetPortMode(0, LT_PORT, &port);
	if (ret == 0) {
		ret = MI_VPE_EnablePort(0, LT_PORT);
		if (ret == 0)
			g.port_enabled = 1;
	}
	if (!g.port_enabled) {
		fprintf(stderr, "[luma-tap] VPE port1 %ux%u unavailable (%d)\n",
			w, h, (int)ret);
		goto fail;
	}
	/* Unbound output port needs a user frame queue or GetBuf sees 0 frames. */
	MI_SYS_SetChnOutputPortDepth(&g.port, 2, 4);

	g.w = w;
	g.h = h;
	g.grab_pending = 0;
	g.latch_valid = 0;
	g.pause = 0;
	g.parked = 0;
	g.reader_run = 1;
	if (pthread_create(&g.reader, NULL, lt_reader_main, NULL) != 0) {
		fprintf(stderr, "[luma-tap] reader thread spawn failed\n");
		g.reader_run = 0;
		goto fail;
	}
	g.reader_started = 1;
	g.running = 1;
	fprintf(stderr, "[luma-tap] VPE port1 tap up at %ux%u\n", w, h);
	return 0;

fail:
	lt_port_teardown();
	free(g.latch);
	g.latch = NULL;
	if (g.claimed) {
		star6e_vpe_port1_release(LT_OWNER);
		g.claimed = 0;
	}
	return -1;
}

void star6e_luma_tap_stop(void)
{
	/* Refuse new grabs before anything is dismantled; in-flight ones are
	 * released by the broadcast below and re-check g.running. */
	g.running = 0;

	if (g.reader_started) {
		int spins;

		/* Park the reader outside its GetBuf/PutBuf window before the
		 * port goes away.  select() bounds a cycle at 50 ms, so this
		 * settles well inside the budget; the join is what actually
		 * guarantees it, the park just makes it prompt. */
		g.pause = 1;
		for (spins = 0; spins < 100 && !g.parked; ++spins)
			usleep(2000);
		g.reader_run = 0;
		/* Release anyone blocked in grab() so the join cannot stall. */
		pthread_mutex_lock(&g.lock);
		g.grab_pending = 0;
		g.latch_valid = 0;
		pthread_cond_broadcast(&g.cond);
		pthread_mutex_unlock(&g.lock);
		pthread_join(g.reader, NULL);
		g.reader_started = 0;
	}

	lt_port_teardown();

	/* Under the lock: a grab that woke on the broadcast may still be inside
	 * its memcpy out of the latch. */
	pthread_mutex_lock(&g.lock);
	free(g.latch);
	g.latch = NULL;
	g.latch_valid = 0;
	pthread_mutex_unlock(&g.lock);

	if (g.claimed) {
		star6e_vpe_port1_release(LT_OWNER);
		g.claimed = 0;
	}
	g.w = g.h = 0;
}

int star6e_luma_tap_running(void)
{
	return g.running;
}

int star6e_luma_tap_grab_pgm(uint8_t **out_buf, size_t *out_len,
	uint32_t timeout_ms)
{
	struct timespec deadline;
	char hdr[32];
	int hlen;
	size_t total;
	uint8_t *out;
	int rc = 0;

	if (!out_buf || !out_len)
		return -EINVAL;
	*out_buf = NULL;
	*out_len = 0;
	if (!g.running)
		return -ENODEV;
	if (timeout_ms == 0)
		timeout_ms = 1000;

	/* One outstanding grab at a time: the latch is single-buffered. */
	pthread_mutex_lock(&g.api_lock);

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += (time_t)(timeout_ms / 1000u);
	deadline.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec += 1;
		deadline.tv_nsec -= 1000000000L;
	}

	pthread_mutex_lock(&g.lock);
	g.latch_valid = 0;
	g.grab_pending = 1;
	while (!g.latch_valid && g.running) {
		if (pthread_cond_timedwait(&g.cond, &g.lock, &deadline) != 0)
			break;
	}
	if (!g.latch_valid)
		rc = g.running ? -ETIMEDOUT : -ENODEV;
	if (rc != 0)
		g.grab_pending = 0;
	pthread_mutex_unlock(&g.lock);

	if (rc == 0) {
		hlen = snprintf(hdr, sizeof(hdr), "P5\n%u %u\n255\n", g.w, g.h);
		total = (size_t)hlen + (size_t)g.w * g.h;
		out = malloc(total);
		if (!out) {
			rc = -ENOMEM;
		} else {
			memcpy(out, hdr, (size_t)hlen);
			pthread_mutex_lock(&g.lock);
			if (g.latch && g.latch_valid) {
				memcpy(out + hlen, g.latch,
				       (size_t)g.w * g.h);
			} else {
				rc = -ENODEV;   /* torn down under us */
			}
			pthread_mutex_unlock(&g.lock);
			if (rc == 0) {
				*out_buf = out;
				*out_len = total;
			} else {
				free(out);
			}
		}
	}

	pthread_mutex_unlock(&g.api_lock);
	return rc;
}

void star6e_luma_tap_free(uint8_t *buf)
{
	free(buf);
}
