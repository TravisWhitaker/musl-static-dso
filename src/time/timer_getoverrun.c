#include <time.h>
#include <limits.h>
#include "pthread_impl.h"
#ifdef MUSL_EMBED
#include "embed_impl.h"
#endif

int timer_getoverrun(timer_t t)
{
#ifdef MUSL_EMBED
	/* See timer_create.c. Without host timers, all are kernel ids. */
	if (__embed_host_timers) {
		int r = __embed_host.timer_getoverrun(t);
		if (r < 0) errno = __embed_host_errno(EINVAL);
		return r;
	}
#else
	if ((intptr_t)t < 0) {
		pthread_t td = (void *)((uintptr_t)t << 1);
		t = (void *)(uintptr_t)(td->timer_id & INT_MAX);
	}
#endif
	return syscall(SYS_timer_getoverrun, t);
}
