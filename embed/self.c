#include "pthread_impl.h"
#include "embed_impl.h"

/* musl's per-thread state, in the TLS of the containing shared object.
 * The host dynamic linker allocates and zero-fills a block for every
 * thread, including threads created before this object was loaded, and
 * frees it when the thread exits. */
__thread struct __embed_tls __embed_tls;

volatile int __thread_list_lock;

/* Threads created by musl's pthread_create are linked into this ring.
 * Threads created by the host never are: their TCB lives in TLS that
 * goes away without musl noticing. */
struct pthread __embed_thread_list = {
	.self = &__embed_thread_list,
	.prev = &__embed_thread_list,
	.next = &__embed_thread_list,
};

struct pthread *__embed_self_slow(struct __embed_tls *t)
{
	struct pthread *td = &t->tcb;
	unsigned long (*host_self)(void);

	/* First use of musl on a thread musl did not create. Must not call
	 * anything that uses __pthread_self(). */
	__embed_ensure_init();
	td->self = td;
	td->tid = __syscall(SYS_gettid);
	td->detach_state = DT_DETACHED;
	td->locale = &libc.global_locale;
	td->robust_list.head = &td->robust_list.head;
	td->tsd = t->tsd;
	td->next = td->prev = td;
	td->embed_kind = EMBED_FOREIGN;
	if ((host_self = __embed_host.pthread_self))
		td->embed_host_handle = host_self();
	t->self = td;
	return td;
}
