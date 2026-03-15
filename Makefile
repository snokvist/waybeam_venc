# Waybeam — FPV streamer for SigmaStar SoCs
# Targets: infinity6c (Maruko), infinity6e (Star6e/Pudding)
#
# Usage (called from build.sh):
#   make -C src -B CC=<cross-gcc> DRV=<sdk-lib-dir> maruko
#   make -C src -B CC=<cross-gcc> DRV=<sdk-lib-dir> star6e

SRC  = waybeam.c
LIBS = -lmi_isp -lmi_sensor -lmi_sys -lmi_venc -lmi_vif -lcus3a -lispalgo -lcam_os_wrapper

.PHONY: maruko star6e

maruko:
	$(CC) $(SRC) \
		-D SIGMASTAR_MARUKO \
		-I ../include/infinity6c \
		-L $(DRV) $(LIBS) \
		-lmi_common -lmi_scl \
		-Os -s -o waybeam

star6e:
	$(CC) $(SRC) \
		-D SIGMASTAR_PUDDING \
		-I ../include/infinity6e \
		-L $(DRV) $(LIBS) \
		-lmi_vpe -lm \
		-Os -s -o waybeam
