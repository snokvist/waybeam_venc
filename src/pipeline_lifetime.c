#include "pipeline_lifetime.h"

#include <pthread.h>

static pthread_rwlock_t g_lock = PTHREAD_RWLOCK_INITIALIZER;

void pipeline_lifetime_rdlock(void)
{
	(void)pthread_rwlock_rdlock(&g_lock);
}

void pipeline_lifetime_rdunlock(void)
{
	(void)pthread_rwlock_unlock(&g_lock);
}

void pipeline_lifetime_wrlock(void)
{
	(void)pthread_rwlock_wrlock(&g_lock);
}

void pipeline_lifetime_wrunlock(void)
{
	(void)pthread_rwlock_unlock(&g_lock);
}
