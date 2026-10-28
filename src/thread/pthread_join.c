#define _GNU_SOURCE
#include "pthread_impl.h"
#include <sys/mman.h>
#ifdef MUSL_EMBED
#include <stdlib.h>
#include "embed_impl.h"
#endif

static void dummy1(pthread_t t)
{
}
weak_alias(dummy1, __tl_sync);

static int __pthread_timedjoin_np(pthread_t t, void **res, const struct timespec *at)
{
	int state, cs, r = 0;
	__pthread_testcancel();
#ifdef MUSL_EMBED
	/* Threads created by the host are not joinable through musl. */
	if (t->embed_kind == EMBED_FOREIGN) return EINVAL;
#endif
	__pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cs);
	if (cs == PTHREAD_CANCEL_ENABLE) __pthread_setcancelstate(cs, 0);
	while ((state = t->detach_state) && r != ETIMEDOUT && r != EINVAL) {
		if (state >= DT_DETACHED) a_crash();
		r = __timedwait_cp(&t->detach_state, state, CLOCK_REALTIME, at, 1);
	}
	__pthread_setcancelstate(cs, 0);
	if (r == ETIMEDOUT || r == EINVAL) return r;
	__tl_sync(t);
	if (res) *res = t->result;
#ifdef MUSL_EMBED
	/* The thread has marked itself exited but may still be running
	 * in this object; the host's join waits until it is gone. The
	 * thread structure heads its embed/pthread_create.c allocation. */
	if (t->embed_host_join) __embed_host.pthread_join(t->embed_host_handle, 0);
	__libc_free(t);
#else
	if (t->map_base) __munmap(t->map_base, t->map_size);
#endif
	return 0;
}

int __pthread_join(pthread_t t, void **res)
{
	return __pthread_timedjoin_np(t, res, 0);
}

static int __pthread_tryjoin_np(pthread_t t, void **res)
{
	return t->detach_state==DT_JOINABLE ? EBUSY : __pthread_join(t, res);
}

weak_alias(__pthread_tryjoin_np, pthread_tryjoin_np);
weak_alias(__pthread_timedjoin_np, pthread_timedjoin_np);
weak_alias(__pthread_join, pthread_join);
