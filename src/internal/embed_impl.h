#ifndef EMBED_IMPL_H
#define EMBED_IMPL_H

#include <features.h>
#include <stddef.h>

/* Support for MUSL_EMBED, a build of musl that is linked statically into
 * a shared library which is then loaded into a process whose executable
 * uses a different libc (the "host"). See embed/README.md. */

struct dl_phdr_info;

/* Host libc functions that musl calls. None of them is referenced at
 * link time; hostsym.c finds them at run time by walking the host
 * dynamic linker's link map. Any of them may be missing (null). */
#define EMBED_HOST_SYMS(X) \
	X(int, pthread_create, (void *, const void *, void *(*)(void *), void *)) \
	X(int, pthread_detach, (unsigned long)) \
	X(int, pthread_join, (unsigned long, void **)) \
	X(void, pthread_exit, (void *)) \
	X(unsigned long, pthread_self, (void)) \
	X(int, pthread_attr_init, (void *)) \
	X(int, pthread_attr_destroy, (void *)) \
	X(int, pthread_attr_setstacksize, (void *, size_t)) \
	X(int, pthread_attr_setguardsize, (void *, size_t)) \
	X(int, pthread_attr_setstack, (void *, void *, size_t)) \
	X(int, pthread_attr_setdetachstate, (void *, int)) \
	X(int, pthread_getattr_np, (unsigned long, void *)) \
	X(int, pthread_attr_getstack, (const void *, void **, size_t *)) \
	X(int, pthread_attr_getguardsize, (const void *, size_t *)) \
	X(int, __register_atfork, (void (*)(void), void (*)(void), void (*)(void), void *)) \
	X(int, pthread_atfork, (void (*)(void), void (*)(void), void (*)(void))) \
	X(void, __cxa_finalize, (void *)) \
	X(int, fork, (void)) \
	X(int, _Fork, (void)) \
	X(void, exit, (int)) \
	X(int *, __errno_location, (void)) \
	X(void *, dlopen, (const char *, int)) \
	X(void *, dlsym, (void *, const char *)) \
	X(int, dlclose, (void *)) \
	X(char *, dlerror, (void)) \
	X(int, dladdr, (const void *, void *)) \
	X(int, dlinfo, (void *, int, void *)) \
	X(int, dl_iterate_phdr, (int (*)(struct dl_phdr_info *, size_t, void *), void *)) \
	X(int, setuid, (unsigned)) \
	X(int, setreuid, (unsigned, unsigned)) \
	X(int, setresuid, (unsigned, unsigned, unsigned)) \
	X(int, setgid, (unsigned)) \
	X(int, setregid, (unsigned, unsigned)) \
	X(int, setresgid, (unsigned, unsigned, unsigned)) \
	X(int, setgroups, (size_t, const unsigned *)) \
	X(int, setrlimit, (int, const void *)) \
	X(int, timer_create, (int, void *, void **)) \
	X(int, timer_settime, (void *, int, const void *, void *)) \
	X(int, timer_gettime, (void *, void *)) \
	X(int, timer_delete, (void *)) \
	X(int, timer_getoverrun, (void *))

struct __embed_host {
#define EMBED_HOST_FIELD(r, n, a) r (*n) a;
	EMBED_HOST_SYMS(EMBED_HOST_FIELD)
#undef EMBED_HOST_FIELD
};

extern hidden struct __embed_host __embed_host;

/* Resolves the slot by name if it was not found at init time (e.g. a
 * library that provides it was dlopened later) and returns it. */
hidden void *__embed_host_resolve(void *volatile *, const char *);
#define EMBED_HOST(n) ((__typeof__(__embed_host.n)) \
	(__embed_host.n ? (void *)__embed_host.n : \
	 __embed_host_resolve((void *volatile *)&__embed_host.n, #n)))

/* Host errno after a failed host call, or fallback if unknown. */
hidden int __embed_host_errno(int);

hidden void __embed_host_init(void);
hidden void __embed_ensure_init(void);
hidden void __embed_fork_init(void);
hidden void __embed_fork_fini(void);
hidden void __embed_post_fork_child(void);
hidden void __embed_exit_hooks(void);
hidden char ***__embed_environ(void);
hidden void __embed_environ_init(char **);

extern hidden size_t __embed_null_auxv[2];
/* Set at init when all host timer_* functions exist; never changes. */
extern hidden int __embed_host_timers;
/* An address near the top of the main thread's stack, or null. */
extern hidden void *__embed_stack_hint;
/* Set once musl's exit hooks have started. */
extern hidden volatile int __embed_exiting;

#endif
