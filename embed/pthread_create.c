#define _GNU_SOURCE
#include "pthread_impl.h"
#include "stdio_impl.h"
#include "libc.h"
#include "lock.h"
#include "embed_impl.h"
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>

/* Threads are created by the host's pthread_create, so that every thread
 * in the process has a proper host TCB and both libcs work on it: client
 * callbacks and signal handlers are safe on threads the library creates.
 * musl keeps its own struct pthread for these threads on the heap (found
 * through the same TLS variable as on host-created threads) and its own
 * join/detach protocol; joining or detaching a thread through musl also
 * joins or detaches it at the host level. */

static void dummy_0()
{
}
weak_alias(dummy_0, __pthread_tsd_run_dtors);
weak_alias(dummy_0, __do_orphaned_stdio_locks);
weak_alias(dummy_0, __dl_thread_cleanup);

static int tl_lock_count;
static int tl_lock_waiters;

void __tl_lock(void)
{
	int tid = __pthread_self()->tid;
	int val = __thread_list_lock;
	if (val == tid) {
		tl_lock_count++;
		return;
	}
	while ((val = a_cas(&__thread_list_lock, 0, tid)))
		__wait(&__thread_list_lock, &tl_lock_waiters, val, 0);
}

void __tl_unlock(void)
{
	if (tl_lock_count) {
		tl_lock_count--;
		return;
	}
	a_store(&__thread_list_lock, 0);
	if (tl_lock_waiters) __wake(&__thread_list_lock, 1, 0);
}

void __tl_sync(pthread_t td)
{
	a_barrier();
	int val = __thread_list_lock;
	if (!val) return;
	__wait(&__thread_list_lock, &tl_lock_waiters, val, 0);
	if (tl_lock_waiters) __wake(&__thread_list_lock, 1, 0);
}

_Noreturn void __pthread_exit(void *result)
{
	pthread_t self = __pthread_self();
	sigset_t set;

	self->canceldisable = 1;
	self->cancelasync = 0;
	self->result = result;

	while (self->cancelbuf) {
		void (*f)(void *) = self->cancelbuf->__f;
		void *x = self->cancelbuf->__x;
		self->cancelbuf = self->cancelbuf->__next;
		f(x);
	}

	__pthread_tsd_run_dtors();

	if (self->embed_kind == EMBED_FOREIGN) {
		/* The host created this thread; only the host can end it. */
		void (*host_exit)(void *) = EMBED_HOST(pthread_exit);
		if (host_exit) host_exit(result);
		a_crash();
	}

	__block_app_sigs(&set);

	/* This atomic potentially competes with a concurrent pthread_detach
	 * call; the loser is responsible for freeing thread resources. */
	int state = a_cas(&self->detach_state, DT_JOINABLE, DT_EXITING);

	/* Access to target the exiting thread with syscalls that use
	 * its kernel tid is controlled by killlock. For detached threads,
	 * any use past this point would have undefined behavior, but for
	 * joinable threads it's a valid usage that must be handled.
	 * Signals must be blocked since pthread_kill must be AS-safe. */
	LOCK(self->killlock);

	/* The thread list lock must be AS-safe, and thus depends on
	 * application signals being blocked above. */
	__tl_lock();

	self->tid = 0;
	UNLOCK(self->killlock);

	/* Process the robust list in userspace; the kernel does not know
	 * about it in the embedded build. */
	__vm_lock();
	volatile void *volatile *rp;
	while ((rp=self->robust_list.head) && rp != &self->robust_list.head) {
		pthread_mutex_t *m = (void *)((char *)rp
			- offsetof(pthread_mutex_t, _m_next));
		int waiters = m->_m_waiters;
		int priv = (m->_m_type & 128) ^ 128;
		self->robust_list.pending = rp;
		self->robust_list.head = *rp;
		int cont = a_swap(&m->_m_lock, 0x40000000);
		self->robust_list.pending = 0;
		if (cont < 0 || waiters)
			__wake(&m->_m_lock, 1, priv);
	}
	__vm_unlock();

	__do_orphaned_stdio_locks();
	__dl_thread_cleanup();

	/* Unlink from the list of owned threads. Upstream leaves the thread
	 * list locked until the kernel clears it at SYS_exit; here the host
	 * ends the thread, so the lock is released explicitly. */
	--libc.threads_minus_1;
	self->next->prev = self->prev;
	self->prev->next = self->next;
	self->prev = self->next = self;
	__tl_unlock();

	/* Return to the start trampoline, which finishes the exit protocol
	 * and then returns to the host's thread start function. Returning,
	 * rather than calling the host's pthread_exit, avoids the host's
	 * unwinder (glibc's needs libgcc_s.so.1, which may be absent). */
	self->embed_exit_state = state;
	longjmp(*(jmp_buf *)self->embed_jb, 1);
}

void __do_cleanup_push(struct __ptcb *cb)
{
	struct pthread *self = __pthread_self();
	cb->__next = self->cancelbuf;
	self->cancelbuf = cb;
}

void __do_cleanup_pop(struct __ptcb *cb)
{
	__pthread_self()->cancelbuf = cb->__next;
}

struct start_args {
	void *(*start_func)(void *);
	void *start_arg;
	int c11;
	volatile int control;
	volatile int tid_ready;
	unsigned long sig_mask[_NSIG/8/sizeof(long)];
};

/* One allocation per thread, freed by the joiner or, for a detached
 * thread, by the thread itself. Joiners read the thread structure after
 * the thread's TLS is gone, so it cannot live there. */
struct thread_alloc {
	struct pthread td;
	void *tsd[PTHREAD_KEYS_MAX];
	struct start_args args;
};

static void *start(void *p)
{
	struct thread_alloc *a = p;
	struct pthread *td = &a->td;
	struct start_args *args = &a->args;
	jmp_buf jb;

	__embed_tls.self = td;
	td->tid = __syscall(SYS_gettid);
	td->embed_jb = &jb;
	a_store(&args->tid_ready, 1);
	__wake(&args->tid_ready, 1, 1);

	/* Wait for the creator to finish with the allocation, which this
	 * thread may free once it runs (upstream's creator held the thread
	 * list lock across clone for that; see __pthread_create). */
	if (a_cas(&args->control, 1, 2)==1)
		__wait(&args->control, 0, 2, 1);
	if (args->control) {
		/* The creator failed to apply the requested scheduling
		 * and fails pthread_create; it frees everything once we
		 * stop touching it. */
		__embed_tls.self = 0;
		a_store(&args->control, 0);
		__wake(&args->control, 1, 1);
		return 0;
	}
	__syscall(SYS_rt_sigprocmask, SIG_SETMASK, &args->sig_mask, 0, _NSIG/8);

	if (!setjmp(jb)) {
		void *r = args->c11
			? (void *)(uintptr_t)((int (*)(void *))args->start_func)(args->start_arg)
			: args->start_func(args->start_arg);
		__pthread_exit(r);
	}

	/* Back from __pthread_exit, which ran cleanup handlers and TSD
	 * destructors, resolved the race with pthread_detach, cleared tid,
	 * processed the robust list and unlinked the thread. */
	int state = td->embed_exit_state;
	__embed_tls.self = 0;
	if (state == DT_DETACHED) {
		__libc_free(a);
	} else {
		/* Wake any joiner. It also joins the host thread before freeing
		 * the allocation, so neither that nor this object's code can go
		 * away while this thread is still running here. */
		a_store(&td->detach_state, DT_EXITED);
		__wake(&td->detach_state, 1, 1);
	}
	return 0;
}

int __pthread_create(pthread_t *restrict res, const pthread_attr_t *restrict attrp, void *(*entry)(void *), void *restrict arg)
{
	int ret, c11 = (attrp == __ATTRP_C11_THREAD), host_join;
	int (*host_create)(void *, const void *, void *(*)(void *), void *);
	int (*h_init)(void *), (*h_destroy)(void *), (*h_detachstate)(void *, int);
	int (*h_stack)(void *, void *, size_t);
	int (*h_stacksize)(void *, size_t), (*h_guardsize)(void *, size_t);
	int (*h_join)(unsigned long, void **), (*h_detach)(unsigned long);
	struct thread_alloc *a;
	struct pthread *td;
	pthread_attr_t attr = { 0 };
	sigset_t set;
	unsigned long handle = 0;
	/* Never pass a musl pthread_attr_t to the host: the layouts differ
	 * (glibc's is 56 bytes on 64-bit, bionic's 64). */
	struct { char buf[256]; } hattr __attribute__((__aligned__(16)));
	void *hattrp = 0;

	if (!(host_create = EMBED_HOST(pthread_create))) return ENOSYS;
	__pthread_self();
	if (attrp && !c11) attr = *attrp;

	/* The host thread stays joinable while the musl thread is: joining
	 * it is the only way to know that the thread has left this object's
	 * code, after which the caller may unload the object. Without the
	 * host functions for that, it is detached from the start. */
	h_join = EMBED_HOST(pthread_join);
	h_detach = EMBED_HOST(pthread_detach);
	host_join = h_join && h_detach;

	if (!(a = __libc_calloc(1, sizeof *a))) return EAGAIN;
	td = &a->td;
	td->self = td;
	td->tsd = a->tsd;
	td->locale = &libc.global_locale;
	td->detach_state = attr._a_detach ? DT_DETACHED : DT_JOINABLE;
	td->robust_list.head = &td->robust_list.head;
	td->embed_kind = EMBED_OWNED;
	td->embed_host_join = host_join;
	a->args.start_func = entry;
	a->args.start_arg = arg;
	a->args.c11 = c11;
	a->args.control = 1;

	/* Without an attribute object the host's default stack size applies
	 * (8 MiB on glibc), which is more than musl's 128 KiB. */
	h_init = EMBED_HOST(pthread_attr_init);
	h_destroy = EMBED_HOST(pthread_attr_destroy);
	if (h_init && h_destroy && !h_init(&hattr)) {
		hattrp = &hattr;
		if (!host_join && (h_detachstate = EMBED_HOST(pthread_attr_setdetachstate)))
			h_detachstate(hattrp, PTHREAD_CREATE_DETACHED);
		if (attrp && !c11 && attr._a_stackaddr) {
			if ((h_stack = EMBED_HOST(pthread_attr_setstack)))
				h_stack(hattrp, (void *)(attr._a_stackaddr - attr._a_stacksize),
					attr._a_stacksize);
		} else if (attrp && !c11) {
			/* A size below the host's minimum is rejected and the
			 * host default kept. */
			if ((h_stacksize = EMBED_HOST(pthread_attr_setstacksize)))
				h_stacksize(hattrp, attr._a_stacksize);
			if ((h_guardsize = EMBED_HOST(pthread_attr_setguardsize)))
				h_guardsize(hattrp, attr._a_guardsize);
		}
	}

	/* Application signals must be blocked before the thread list lock
	 * can be taken, to ensure that the lock is AS-safe. The new thread
	 * starts with them blocked and restores the caller's mask. Unlike
	 * upstream, SIGCANCEL is not unblocked: it belongs to the host. */
	__block_app_sigs(&set);
	memcpy(&a->args.sig_mask, &set, sizeof a->args.sig_mask);

	/* The thread is listed before it exists, so that it can unlink
	 * itself whenever it exits. Unlike upstream, no lock is held while
	 * the thread is created: the host's pthread_create may take the host
	 * dynamic linker's lock (glibc < 2.35 does, for TLS), which a dlopen
	 * holds while it runs constructors, and a constructor may create a
	 * thread or fork through musl. */
	__tl_lock();
	libc.threads_minus_1++;
	td->next = __embed_thread_list.next;
	td->prev = &__embed_thread_list;
	td->next->prev = td;
	td->prev->next = td;
	__tl_unlock();

	ret = host_create(&handle, hattrp, start, a);
	if (hattrp) h_destroy(hattrp);
	if (ret) {
		/* All host failures translate to EAGAIN, as clone failures
		 * do upstream. */
		ret = EAGAIN;
		goto fail;
	}
	if (!hattrp && !host_join && h_detach) h_detach(handle);
	td->embed_host_handle = handle;

	/* Others may use td->tid as soon as pthread_create returns. The
	 * thread then waits on control, so it cannot have exited (and freed
	 * a) while a is used here. */
	while (!a->args.tid_ready)
		__wait(&a->args.tid_ready, 0, 0, 1);
	if (attr._a_sched)
		ret = -__syscall(SYS_sched_setscheduler,
			td->tid, attr._a_policy, &attr._a_prio);
	if (a_swap(&a->args.control, ret ? 3 : 0)==2)
		__wake(&a->args.control, 1, 1);
	if (ret) {
		while (a->args.control == 3)
			__wait(&a->args.control, 0, 3, 1);
		if (host_join) h_join(handle, 0);
		goto fail;
	}
	if (host_join && attr._a_detach) h_detach(handle);

	__restore_sigs(&set);
	*res = td;
	return 0;

fail:
	__tl_lock();
	td->next->prev = td->prev;
	td->prev->next = td->next;
	libc.threads_minus_1--;
	__tl_unlock();
	__restore_sigs(&set);
	__libc_free(a);
	return ret;
}

weak_alias(__pthread_exit, pthread_exit);
weak_alias(__pthread_create, pthread_create);
