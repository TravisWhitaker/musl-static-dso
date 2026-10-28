#include <time.h>
#include <limits.h>
#include "pthread_impl.h"
#ifdef MUSL_EMBED
#include "embed_impl.h"
#endif

int timer_delete(timer_t t)
{
#ifdef MUSL_EMBED
	/* See timer_create.c. Without host timers, all are kernel ids. */
	if (__embed_host_timers) {
		if (__embed_host.timer_delete(t)) {
			errno = __embed_host_errno(EINVAL);
			return -1;
		}
		return 0;
	}
#else
	if ((intptr_t)t < 0) {
		pthread_t td = (void *)((uintptr_t)t << 1);
		a_store(&td->timer_id, td->timer_id | INT_MIN);
		__syscall(SYS_tkill, td->tid, SIGTIMER);
		return 0;
	}
#endif
	return __syscall(SYS_timer_delete, t);
}
