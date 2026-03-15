/*
 * Part of the OpenIPC project — https://openipc.org
 * Targets: SigmaStar SoCs (infinity6c/Maruko, infinity6e/Star6e)
 * Contact: tech@openipc.eu
 * License: MIT
 *
 * Compatibility shims for SigmaStar MI API differences between
 * infinity6c (SIGMASTAR_MARUKO) and infinity6e (SIGMASTAR_PUDDING).
 */

#pragma once

#ifdef SIGMASTAR_MARUKO
/* infinity6c — ISP+SCL pipeline */
#include <mi_isp_ae.h>
#include <mi_isp_cus3a_api.h>
#include <mi_scl.h>

#define MDEV               0,
#define SDEV               0

/* infinity6c uses MI_VIF_DisableOutputPort instead of MI_VIF_DisableChnPort */
#define MI_VIF_DisableChnPort        MI_VIF_DisableOutputPort

/* infinity6c renamed this function */
#define MI_ISP_API_CmdLoadBinFile    MI_ISP_ApiCmdLoadBinFile

/* AE exposure limit type renamed on infinity6c */
#define MI_ISP_AE_EXPO_LIMIT_TYPE_t  MI_ISP_AE_ExpoLimitType_t

/* VENC is fed from the SCL (scaler) module on infinity6c */
#define VENC_MODULE_BIND             E_MI_SYS_BIND_TYPE_HW_RING
#define VENC_MODULE_PORT             E_MI_MODULE_ID_SCL

#else
/* infinity6e — VPE pipeline */
#include <mi_vpe.h>

#define MDEV
#define SDEV

/* VENC is fed from the VPE module on infinity6e */
#define VENC_MODULE_BIND             E_MI_SYS_BIND_TYPE_FRAME_BASE
#define VENC_MODULE_PORT             E_MI_MODULE_ID_VPE
#endif /* SIGMASTAR_MARUKO */

/* --------------------------------------------------------------------------
 * Weak symbol stubs required by the SigmaStar SDK shared libraries.
 * These are not called at runtime but the linker expects them to exist.
 * -------------------------------------------------------------------------- */
void __assert(void) {}
void __ctype_b(void) {}
void __stdin(void) {}

int __fgetc_unlocked(FILE *stream) {
	return fgetc(stream);
}

/* infinity6e SDK references finite math wrappers from its libm build. */
#ifdef SIGMASTAR_PUDDING
float __expf_finite(float a) {
	return expf(a);
}

double __log_finite(double a) {
	return log(a);
}
#endif

/* infinity6c SDK references backtrace helpers not present in uclibc. */
#ifdef SIGMASTAR_MARUKO
void backtrace(void) {}
void backtrace_symbols(void) {}
#endif

/* infinity6b0 SDK uses SYS_mmap2 on 32-bit ARM; keep for reference builds. */
#ifdef SIGMASTAR_ISPAHAN
void *mmap(void *start, size_t len, int prot, int flags, int fd, uint32_t off) {
	return (void *)syscall(SYS_mmap2, start, len, prot, flags, fd, off >> 12);
}
#endif
