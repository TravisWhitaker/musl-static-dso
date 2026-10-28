#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include "libc.h"
#include "lock.h"
#include "pthread_impl.h"
#include "fork_impl.h"
#include "aio_impl.h"
#include "embed_impl.h"

/* fork for the embedded build. musl's internal locks must be taken
 * around every fork in the process, including those the host makes for
 * the client, or musl state in the child could be left locked forever.
 * So the same lock choreography as upstream fork() is registered as a
 * host atfork handler, and musl's fork() simply calls the host's. */

static volatile int *const dummy_lockptr = 0;

weak_alias(dummy_lockptr, __at_quick_exit_lockptr);
weak_alias(dummy_lockptr, __atexit_lockptr);
weak_alias(dummy_lockptr, __gettext_lockptr);
weak_alias(dummy_lockptr, __locale_lockptr);
weak_alias(dummy_lockptr, __random_lockptr);
weak_alias(dummy_lockptr, __sem_open_lockptr);
weak_alias(dummy_lockptr, __stdio_ofl_lockptr);
weak_alias(dummy_lockptr, __syslog_lockptr);
weak_alias(dummy_lockptr, __timezone_lockptr);
weak_alias(dummy_lockptr, __bump_lockptr);

weak_alias(dummy_lockptr, __vmlock_lockptr);

static volatile int *const *const atfork_locks[] = {
	&__at_quick_exit_lockptr,
	&__atexit_lockptr,
	&__gettext_lockptr,
	&__locale_lockptr,
	&__random_lockptr,
	&__sem_open_lockptr,
	&__stdio_ofl_lockptr,
	&__syslog_lockptr,
	&__timezone_lockptr,
	&__bump_lockptr,
};

static void dummy(int x) { }
weak_alias(dummy, __fork_handler);
weak_alias(dummy, __malloc_atfork);
weak_alias(dummy, __aio_atfork);
weak_alias(dummy, __pthread_key_atfork);
weak_alias(dummy, __ldso_atfork);

static void dummy_0(void) { }
weak_alias(dummy_0, __tl_lock);
weak_alias(dummy_0, __tl_unlock);

/* crtbeginS.o's; lets the host unregister our handlers at dlclose. */
extern void *__dso_handle __attribute__((__weak__, __visibility__("hidden")));

static int host_atfork, host_atfork_dso;

static void prepare(void)
{
	__fork_handler(-1);
	__block_app_sigs(&__pthread_self()->embed_fork_mask);
	__ldso_atfork(-1);
	__pthread_key_atfork(-1);
	__aio_atfork(-1);
	__inhibit_ptc();
	for (int i=0; i<sizeof atfork_locks/sizeof *atfork_locks; i++)
		if (*atfork_locks[i]) LOCK(*atfork_locks[i]);
	__malloc_atfork(-1);
	__tl_lock();
}

static void parent(void)
{
	__tl_unlock();
	__malloc_atfork(0);
	for (int i=0; i<sizeof atfork_locks/sizeof *atfork_locks; i++)
		if (*atfork_locks[i]) UNLOCK(*atfork_locks[i]);
	__release_ptc();
	__aio_atfork(0);
	__pthread_key_atfork(0);
	__ldso_atfork(0);
	__restore_sigs(&__pthread_self()->embed_fork_mask);
	__fork_handler(0);
}

/* Threads other than the caller do not exist in the child. Only valid
 * while the thread list lock was held across the fork. */
static void mark_others_gone(void)
{
	pthread_t self = __pthread_self(), td;
	for (td=__embed_thread_list.next; td!=&__embed_thread_list; td=td->next)
		if (td != self) td->tid = -1;
}

static void child(void)
{
	if (__vmlock_lockptr) {
		__vmlock_lockptr[0] = 0;
		__vmlock_lockptr[1] = 0;
	}
	__tl_unlock();
	__malloc_atfork(1);
	for (int i=0; i<sizeof atfork_locks/sizeof *atfork_locks; i++)
		if (*atfork_locks[i]) **atfork_locks[i] = 0;
	__release_ptc();
	__pthread_key_atfork(1);
	__ldso_atfork(1);
	__restore_sigs(&__pthread_self()->embed_fork_mask);
	__fork_handler(1);
}

/* As host atfork handlers, __abort_lock is also taken around the fork,
 * as upstream _Fork does. Once musl's exit hooks have run, their locks
 * are held for good and the handlers do nothing (where the host cannot
 * unregister them, or a fork raced with that). */
static void host_prepare(void)
{
	pthread_t self = __pthread_self();
	if ((self->embed_fork_skip = __embed_exiting)) return;
	prepare();
	LOCK(__abort_lock);
}

static void host_parent(void)
{
	if (__pthread_self()->embed_fork_skip) return;
	UNLOCK(__abort_lock);
	parent();
}

static void host_child(void)
{
	if (__pthread_self()->embed_fork_skip) return;
	mark_others_gone();
	__post_Fork(0);
	child();
}

void __embed_fork_init(void)
{
	int (*reg)(void (*)(void), void (*)(void), void (*)(void), void *);
	int (*atfork)(void (*)(void), void (*)(void), void (*)(void));

	if (!__embed_host.fork) return;
	/* glibc's pthread_atfork is in libc_nonshared.a; its libc.so.6 only
	 * has __register_atfork, which also records the object so that the
	 * handlers are dropped when it is unloaded. */
	if ((reg = __embed_host.__register_atfork)) {
		host_atfork = !reg(host_prepare, host_parent, host_child,
			&__dso_handle ? __dso_handle : 0);
		host_atfork_dso = host_atfork && &__dso_handle && __dso_handle;
	} else if ((atfork = __embed_host.pthread_atfork))
		host_atfork = !atfork(host_prepare, host_parent, host_child);
}

/* Called when the object is unloaded (or the process exits): drops the
 * handlers registered above, which would otherwise point into unmapped
 * code at the next fork. crtbeginS.o's weak reference to __cxa_finalize
 * binds to musl's own (a no-op) whenever musl's atexit is linked, so the
 * host's is called here; a second call does nothing. With a null
 * argument it would run every atexit handler, so never pass one. */
void __embed_fork_fini(void)
{
	void (*fin)(void *) = __embed_host.__cxa_finalize;
	if (host_atfork_dso && fin) fin(__dso_handle);
}

void __embed_post_fork_child(void)
{
	pthread_t self = __pthread_self();
	self->tid = __syscall(SYS_gettid);
	self->robust_list.off = 0;
	self->robust_list.pending = 0;
	__embed_thread_list.next = __embed_thread_list.prev = &__embed_thread_list;
	if (self->embed_kind == EMBED_OWNED) {
		self->next = self->prev = &__embed_thread_list;
		__embed_thread_list.next = __embed_thread_list.prev = self;
		libc.threads_minus_1 = 1;
	} else {
		self->next = self->prev = self;
		libc.threads_minus_1 = 0;
	}
	__thread_list_lock = 0;
}

/* Also used by clone() in src/linux/clone.c. */
void __post_Fork(int ret)
{
	if (!ret) __embed_post_fork_child();
	UNLOCK(__abort_lock);
	if (!ret) __aio_atfork(1);
}

static pid_t do_Fork(int mark)
{
	int (*host_Fork)(void) = EMBED_HOST(_Fork);
	pid_t ret;
	sigset_t set;
	__block_all_sigs(&set);
	LOCK(__abort_lock);
	if (host_Fork) {
		/* Lets the host libc fix up its own state in the child. */
		ret = host_Fork();
		if (ret < 0) ret = -__embed_host_errno(EAGAIN);
	} else {
		/* The host libc's cached state (e.g. thread ids) will be
		 * stale in the child. */
#ifdef SYS_fork
		ret = __syscall(SYS_fork);
#else
		ret = __syscall(SYS_clone, SIGCHLD, 0);
#endif
	}
	if (!ret && mark) mark_others_gone();
	__post_Fork(ret);
	__restore_sigs(&set);
	return __syscall_ret(ret);
}

pid_t _Fork(void)
{
	return do_Fork(0);
}

pid_t fork(void)
{
	if (host_atfork) {
		/* Our handlers run inside the host's fork. */
		pid_t ret = __embed_host.fork();
		if (ret < 0) errno = __embed_host_errno(EAGAIN);
		return ret;
	}
	prepare();
	pid_t ret = do_Fork(1);
	int errno_save = errno;
	if (ret) parent();
	else child();
	if (ret<0) errno = errno_save;
	return ret;
}
