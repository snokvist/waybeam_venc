#include "eis.h"
#include "eis_legacy.h"
#include "eis_gyroglide.h"

#include <stdio.h>
#include <string.h>

EisState *eis_create(const EisConfig *cfg)
{
	if (!cfg)
		return NULL;

	const char *mode = cfg->mode;
	if (!mode || mode[0] == '\0')
		mode = "legacy";

	if (strcmp(mode, "gyroglide") == 0)
		return eis_gyroglide_create(cfg);

	if (strcmp(mode, "legacy") == 0)
		return eis_legacy_create(cfg);

	fprintf(stderr, "EIS: unknown mode \"%s\", falling back to legacy\n",
		mode);
	return eis_legacy_create(cfg);
}
