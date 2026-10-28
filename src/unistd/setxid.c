#include <unistd.h>
#include <signal.h>
#include "syscall.h"
#include "libc.h"

#ifdef MUSL_EMBED
#include <errno.h>
#include "embed_impl.h"

/* Credentials are per-thread at the kernel level. Every thread in the
 * process is a host thread in the embedded build, so the host libc,
 * which knows how to reach all of them, applies the change. */
int __setxid(int nr, int id, int eid, int sid)
{
	int r = -1;
	switch (nr) {
#define CASE(n, call) case SYS_##n: { \
		__typeof__(__embed_host.n) f = EMBED_HOST(n); \
		if (!f) goto nosup; \
		r = call; \
		break; }
	CASE(setuid, f(id))
	CASE(setreuid, f(id, eid))
	CASE(setresuid, f(id, eid, sid))
	CASE(setgid, f(id))
	CASE(setregid, f(id, eid))
	CASE(setresgid, f(id, eid, sid))
#undef CASE
	default:
		errno = EINVAL;
		return -1;
	}
	if (r) {
		errno = __embed_host_errno(EAGAIN);
		return -1;
	}
	return 0;
nosup:
	errno = ENOTSUP;
	return -1;
}
#else
struct ctx {
	int id, eid, sid;
	int nr, ret;
};

static void do_setxid(void *p)
{
	struct ctx *c = p;
	if (c->ret<0) return;
	int ret = __syscall(c->nr, c->id, c->eid, c->sid);
	if (ret && !c->ret) {
		/* If one thread fails to set ids after another has already
		 * succeeded, forcibly killing the process is the only safe
		 * thing to do. State is inconsistent and dangerous. Use
		 * SIGKILL because it is uncatchable. */
		__block_all_sigs(0);
		__syscall(SYS_kill, __syscall(SYS_getpid), SIGKILL);
	}
	c->ret = ret;
}

int __setxid(int nr, int id, int eid, int sid)
{
	/* ret is initially nonzero so that failure of the first thread does not
	 * trigger the safety kill above. */
	struct ctx c = { .nr = nr, .id = id, .eid = eid, .sid = sid, .ret = 1 };
	__synccall(do_setxid, &c);
	return __syscall_ret(c.ret > 0 ? -EAGAIN : c.ret);
}
#endif
