#ifndef VENC_WEBUI_H
#define VENC_WEBUI_H

/* Register the web dashboard route at "/".
 * Must be called after venc_httpd_start(). */
int venc_webui_register(void);

#endif /* VENC_WEBUI_H */
