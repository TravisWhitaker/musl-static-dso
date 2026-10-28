#include "pthread_impl.h"
#include <threads.h>
#ifdef MUSL_EMBED
#include "embed_impl.h"
#endif

static int __pthread_detach(pthread_t t)
{
#ifdef MUSL_EMBED
	/* Threads created by the host are not musl's to detach (nor to
	 * join: without this, the failed cas below would try the join and
	 * drop its EINVAL). */
	if (t->embed_kind == EMBED_FOREIGN) return EINVAL;
	/* Read before the cas: once detached, the thread may free t. */
	unsigned long handle = t->embed_host_handle;
	int host_join = t->embed_host_join;
#endif
	/* If the cas fails, detach state is either already-detached
	 * or exiting/exited, and pthread_join will trap or cleanup. */
	if (a_cas(&t->detach_state, DT_JOINABLE, DT_DETACHED) != DT_JOINABLE) {
		int cs;
		__pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cs);
		__pthread_join(t, 0);
		__pthread_setcancelstate(cs, 0);
	}
#ifdef MUSL_EMBED
	else if (host_join) __embed_host.pthread_detach(handle);
#endif
	return 0;
}

weak_alias(__pthread_detach, pthread_detach);
weak_alias(__pthread_detach, thrd_detach);
