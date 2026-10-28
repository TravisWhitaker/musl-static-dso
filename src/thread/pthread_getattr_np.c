#define _GNU_SOURCE
#include "pthread_impl.h"
#include "libc.h"
#include <sys/mman.h>
#ifdef MUSL_EMBED
#include "embed_impl.h"

/* Every thread's stack was allocated by the host, so ask the host. */
static int host_getattr(pthread_t t, pthread_attr_t *a)
{
	int (*getattr)(unsigned long, void *) = EMBED_HOST(pthread_getattr_np);
	int (*getstack)(const void *, void **, size_t *) = EMBED_HOST(pthread_attr_getstack);
	int (*getguard)(const void *, size_t *) = EMBED_HOST(pthread_attr_getguardsize);
	int (*destroy)(void *) = EMBED_HOST(pthread_attr_destroy);
	struct { char buf[256]; } hattr __attribute__((__aligned__(16)));
	void *addr;
	size_t size, guard = 0;
	int r;

	if (!getattr || !getstack || !destroy || !t->embed_host_handle)
		return ENOENT;
	if (getattr(t->embed_host_handle, &hattr)) return ENOENT;
	r = getstack(&hattr, &addr, &size);
	if (getguard) getguard(&hattr, &guard);
	destroy(&hattr);
	if (r) return ENOENT;
	a->_a_stackaddr = (uintptr_t)addr + size;
	a->_a_stacksize = size;
	a->_a_guardsize = guard;
	return 0;
}
#endif

int pthread_getattr_np(pthread_t t, pthread_attr_t *a)
{
	*a = (pthread_attr_t){0};
	a->_a_detach = t->detach_state>=DT_DETACHED;
	a->_a_guardsize = t->guard_size;
#ifdef MUSL_EMBED
	if (!host_getattr(t, a)) return 0;
	/* Without the host's answer only the main thread's stack can be
	 * found, by probing from a pointer near its top. */
	if (t->tid != __syscall(SYS_getpid) || !__embed_stack_hint)
		return ENOENT;
	{
		char *p = __embed_stack_hint;
#else
	if (t->stack) {
		a->_a_stackaddr = (uintptr_t)t->stack;
		a->_a_stacksize = t->stack_size;
	} else {
		char *p = (void *)libc.auxv;
#endif
		size_t l = PAGE_SIZE;
		p += -(uintptr_t)p & PAGE_SIZE-1;
		a->_a_stackaddr = (uintptr_t)p;
		while (mremap(p-l-PAGE_SIZE, PAGE_SIZE, 2*PAGE_SIZE, 0)==MAP_FAILED && errno==ENOMEM)
			l += PAGE_SIZE;
		a->_a_stacksize = l;
	}
	return 0;
}
