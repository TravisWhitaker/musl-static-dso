#include "pthread_impl.h"
#include "lock.h"

int pthread_kill(pthread_t t, int sig)
{
	int r;
	sigset_t set;
	/* Block not just app signals, but internal ones too, since
	 * pthread_kill is used to implement pthread_cancel, which
	 * must be async-cancel-safe. */
	__block_all_sigs(&set);
	LOCK(t->killlock);
#ifdef MUSL_EMBED
	r = t->tid ? -__syscall(SYS_tgkill, __syscall(SYS_getpid), t->tid, sig)
#else
	r = t->tid ? -__syscall(SYS_tkill, t->tid, sig)
#endif
		: (sig+0U >= _NSIG ? EINVAL : 0);
	UNLOCK(t->killlock);
	__restore_sigs(&set);
	return r;
}
