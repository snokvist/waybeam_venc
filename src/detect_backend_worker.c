/*
 * detect_backend_worker.c — "worker" detector backend.
 *
 * Adapts the prebuilt libipu_yolo_worker.so (a stateless singleton with a
 * frozen 4-function ABI) to the DetectBackend interface.  The worker owns the
 * IPU device, the YOLOv8 decode, and its own MI_RGN overlay.
 *
 * The worker's frame/detection structs are laid out identically to DetectFrame
 * / DetectBox (32 / 24 bytes), so the host arrays pass straight through with a
 * cast — no per-element translation.  See detect_plugin.h and the worker ABI
 * notes there.
 *
 * libipu_yolo_worker.so is not linked against the MI libs it uses by name, and
 * the venc binary is not linked against libmi_ipu/libmi_rgn either, so we
 * dlopen the worker's shared-library dependencies with RTLD_GLOBAL before
 * loading the worker itself (mirrors the star6e_ipu.c loader).
 */

#include "detect_plugin.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

/* Worker ABI (recovered, byte-accurate).  Frame/det match DetectFrame/DetectBox. */
typedef int  (*worker_init_fn)(const char *model_path);
typedef int  (*worker_process_fn)(const DetectFrame *frame, DetectBox *out,
	int max, int *count);
typedef void (*worker_set_display_fn)(int width, int height);
typedef void (*worker_deinit_fn)(void);

static void                 *g_worker_handle;
static void                 *g_dep_handles[5];
static worker_init_fn         g_w_init;
static worker_process_fn      g_w_process;
static worker_set_display_fn  g_w_set_display;
static worker_deinit_fn       g_w_deinit;
static int                    g_inited;

static const char *const g_visdrone_classes[10] = {
	"pedestrian", "people", "bicycle", "car", "van",
	"truck", "tricycle", "awning-tricycle", "bus", "motor"
};

/* libipu_yolo_worker.so's byname deps (not auto-resolved).  Best-effort: the
 * host process may already have some loaded via the other MI libs. */
static void worker_load_deps(void)
{
	static const char *const deps[] = {
		"libcam_os_wrapper.so", "libcam_fs_wrapper.so",
		"libmi_sys.so", "libmi_ipu.so", "libmi_rgn.so"
	};
	size_t i;

	for (i = 0; i < sizeof(deps) / sizeof(deps[0]); i++) {
		if (!g_dep_handles[i])
			g_dep_handles[i] = dlopen(deps[i],
				RTLD_LAZY | RTLD_GLOBAL);
	}
}

static void worker_unload(void)
{
	size_t i;

	if (g_worker_handle) {
		dlclose(g_worker_handle);
		g_worker_handle = NULL;
	}
	for (i = 0; i < sizeof(g_dep_handles) / sizeof(g_dep_handles[0]); i++) {
		if (g_dep_handles[i]) {
			dlclose(g_dep_handles[i]);
			g_dep_handles[i] = NULL;
		}
	}
	g_w_init = NULL;
	g_w_process = NULL;
	g_w_set_display = NULL;
	g_w_deinit = NULL;
}

static int worker_backend_init(const DetectBackendConfig *cfg)
{
	const char *lib = (cfg && cfg->worker_lib && cfg->worker_lib[0])
		? cfg->worker_lib : "/root/libipu_yolo_worker.so";
	const char *model = (cfg && cfg->model_path && cfg->model_path[0])
		? cfg->model_path : NULL;   /* NULL -> worker's built-in default */
	int rc;

	if (g_inited)
		return 0;

	worker_load_deps();

	g_worker_handle = dlopen(lib, RTLD_NOW | RTLD_GLOBAL);
	if (!g_worker_handle) {
		fprintf(stderr, "[ipu-yolo] dlopen %s failed: %s\n", lib,
			dlerror());
		return -1;
	}

	g_w_init = (worker_init_fn)dlsym(g_worker_handle,
		"ipu_yolo_worker_init");
	g_w_process = (worker_process_fn)dlsym(g_worker_handle,
		"ipu_yolo_worker_process");
	g_w_set_display = (worker_set_display_fn)dlsym(g_worker_handle,
		"ipu_yolo_worker_set_display_size");
	g_w_deinit = (worker_deinit_fn)dlsym(g_worker_handle,
		"ipu_yolo_worker_deinit");

	if (!g_w_init || !g_w_process || !g_w_set_display || !g_w_deinit) {
		fprintf(stderr, "[ipu-yolo] worker missing symbols "
			"(init=%p process=%p deinit=%p set_display=%p)\n",
			(void *)g_w_init, (void *)g_w_process,
			(void *)g_w_deinit, (void *)g_w_set_display);
		worker_unload();
		return -1;
	}

	rc = g_w_init(model);
	if (rc != 0) {
		fprintf(stderr, "[ipu-yolo] worker init failed (%d) model=%s\n",
			rc, model ? model : "(default)");
		worker_unload();
		return -1;
	}

	if (cfg && cfg->display_w > 0 && cfg->display_h > 0)
		g_w_set_display(cfg->display_w, cfg->display_h);

	g_inited = 1;
	return 0;
}

static int worker_backend_process(const DetectFrame *frame, DetectBox *out,
	int max, int *count)
{
	if (!g_inited || !g_w_process)
		return -1;
	return g_w_process(frame, out, max, count);
}

static void worker_backend_set_display(int width, int height)
{
	if (g_inited && g_w_set_display)
		g_w_set_display(width, height);
}

static void worker_backend_deinit(void)
{
	if (g_inited && g_w_deinit)
		g_w_deinit();
	worker_unload();
	g_inited = 0;
}

static int worker_backend_describe(const char *const **class_names)
{
	if (class_names)
		*class_names = g_visdrone_classes;
	return 10;
}

static const DetectBackend g_worker_backend = {
	.name        = "worker",
	.abi         = DETECT_PLUGIN_ABI,
	.init        = worker_backend_init,
	.process     = worker_backend_process,
	.set_display = worker_backend_set_display,
	.deinit      = worker_backend_deinit,
	.describe    = worker_backend_describe,
};

const DetectBackend *detect_backend_worker(void)
{
	return &g_worker_backend;
}

const DetectBackend *detect_backend_find(const char *name)
{
	if (!name || !name[0] || strcmp(name, "worker") == 0)
		return detect_backend_worker();
	/* Phase B: else if (strcmp(name,"yolov8")==0) return detect_backend_yolov8(); */
	return NULL;
}
