#include <signal.h>
#include <stdint.h>
#include "syscall.h"
#include "pthread_impl.h"

int raise(int sig)
{
	sigset_t set;
	__block_app_sigs(&set);
#ifdef MUSL_EMBED
	/* The cached tid can be stale after a fork done by the host. */
	int ret = syscall(SYS_tgkill, __syscall(SYS_getpid),
		__syscall(SYS_gettid), sig);
#else
	int ret = syscall(SYS_tkill, __pthread_self()->tid, sig);
#endif
	__restore_sigs(&set);
	return ret;
}
