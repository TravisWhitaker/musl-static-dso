#include <signal.h>
#include <errno.h>
#include "syscall.h"

int pthread_sigmask(int how, const sigset_t *restrict set, sigset_t *restrict old)
{
	int ret;
	if (set && (unsigned)how - SIG_BLOCK > 2U) return EINVAL;
	ret = -__syscall(SYS_rt_sigprocmask, how, set, old, _NSIG/8);
	if (!ret && old) {
#ifdef MUSL_EMBED
		/* Only 32 and 33 are implementation-internal in the host
		 * libc (glibc); 34 is its SIGRTMIN, an application signal
		 * whose state in the caller's mask must be preserved. */
		if (sizeof old->__bits[0] == 8) {
			old->__bits[0] &= ~0x180000000ULL;
		} else {
			old->__bits[0] &= ~0x80000000UL;
			old->__bits[1] &= ~0x1UL;
		}
#else
		if (sizeof old->__bits[0] == 8) {
			old->__bits[0] &= ~0x380000000ULL;
		} else {
			old->__bits[0] &= ~0x80000000UL;
			old->__bits[1] &= ~0x3UL;
		}
#endif
	}
	return ret;
}
