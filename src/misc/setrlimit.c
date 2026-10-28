#include <sys/resource.h>
#include <errno.h>
#include "syscall.h"
#include "libc.h"
#ifdef MUSL_EMBED
#include "embed_impl.h"
#endif

#define MIN(a, b) ((a)<(b) ? (a) : (b))
#define FIX(x) do{ if ((x)>=SYSCALL_RLIM_INFINITY) (x)=RLIM_INFINITY; }while(0)

struct ctx {
	unsigned long lim[2];
	int res;
	int err;
};

#if defined(SYS_setrlimit) && !defined(MUSL_EMBED)
static void do_setrlimit(void *p)
{
	struct ctx *c = p;
	if (c->err>0) return;
	c->err = -__syscall(SYS_setrlimit, c->res, c->lim);
}
#endif

int setrlimit(int resource, const struct rlimit *rlim)
{
	struct rlimit tmp;
	if (SYSCALL_RLIM_INFINITY != RLIM_INFINITY) {
		tmp = *rlim;
		FIX(tmp.rlim_cur);
		FIX(tmp.rlim_max);
		rlim = &tmp;
	}
	int ret = __syscall(SYS_prlimit64, 0, resource, rlim, 0);
#ifdef MUSL_EMBED
	/* Kernels before 2.6.36 lack prlimit64. musl cannot reach every
	 * thread for its fallback below, but the host can; struct rlimit
	 * and RLIM_INFINITY are the same in glibc. */
	if (ret == -ENOSYS) {
		int (*f)(int, const void *) = EMBED_HOST(setrlimit);
		if (f) {
			if (!f(resource, rlim)) return 0;
			errno = __embed_host_errno(EINVAL);
			return -1;
		}
	}
#endif
#if defined(SYS_setrlimit) && !defined(MUSL_EMBED)
	if (ret != -ENOSYS) return __syscall_ret(ret);

	struct ctx c = {
		.lim[0] = MIN(rlim->rlim_cur, MIN(-1UL, SYSCALL_RLIM_INFINITY)),
		.lim[1] = MIN(rlim->rlim_max, MIN(-1UL, SYSCALL_RLIM_INFINITY)),
		.res = resource, .err = -1
	};
	__synccall(do_setrlimit, &c);
	if (c.err) {
		if (c.err>0) errno = c.err;
		return -1;
	}
	return 0;
#else
	return __syscall_ret(ret);
#endif
}
