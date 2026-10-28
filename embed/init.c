#include <elf.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include "pthread_impl.h"
#include "stdio_impl.h"
#include "libc.h"
#include "atomic.h"
#include "syscall.h"
#include "embed_impl.h"

/* Process bootstrap for the embedded build. The host's startup code and
 * dynamic linker have done everything __libc_start_main would; what is
 * left is to collect auxv and put musl into its multithreaded state.
 *
 * This file is compiled without stack protector because the constructor
 * (re)seeds the canary that protected frames compare against. */

#define AUX_CNT 38
#define AUXV_MAX 128

void *__embed_stack_hint;

static size_t proc_auxv[2*AUXV_MAX + 2];
static volatile int init_state, init_tid;

static void dummy(void) {}
weak_alias(dummy, __funcs_on_exit);
weak_alias(dummy, __stdio_exit);
weak_alias(dummy, __membarrier_init);

static void dummy1(void *p) {}
weak_alias(dummy1, __init_ssp);

static FILE *volatile dummy_file = 0;
weak_alias(dummy_file, __stdin_used);
weak_alias(dummy_file, __stdout_used);
weak_alias(dummy_file, __stderr_used);

/* The linker defines these only for executables. In a shared object the
 * host's dynamic linker runs the arrays (DT_INIT_ARRAY, DT_FINI_ARRAY),
 * so, as ldso/dynlink.c does for libc.so, they mark empty ranges: code
 * written for static linking that refers to them (exit.c, GHC's static
 * RTS) links, and finds nothing to run. */
hidden void (*const __init_array_start)(void)=0, (*const __fini_array_start)(void)=0;

extern hidden void (*const __init_array_end)(void), (*const __fini_array_end)(void);

weak_alias(__init_array_start, __init_array_end);
weak_alias(__fini_array_start, __fini_array_end);

static size_t *read_proc_auxv(void)
{
	size_t i, n = 0, max = sizeof proc_auxv - 2*sizeof(size_t);
	int found = 0;
	int fd = __syscall(SYS_openat, AT_FDCWD, "/proc/self/auxv",
		O_RDONLY|O_CLOEXEC);
	if (fd < 0) return 0;
	while (n < max) {
		long r = __syscall(SYS_read, fd, (char *)proc_auxv + n, max - n);
		if (r == -EINTR) continue;
		if (r <= 0) break;
		n += r;
	}
	__syscall(SYS_close, fd);
	n /= 2*sizeof(size_t);
	if (!n) return 0;
	proc_auxv[2*n] = proc_auxv[2*n+1] = 0;
	/* A kernel never omits these; a vector without them is not the
	 * process's (Rosetta reports all zeros when the dynamic linker is
	 * run as a command), and the constructor arguments are used.  */
	for (i = 0; proc_auxv[i]; i += 2) {
		if (proc_auxv[i] == AT_PAGESZ) found |= 1;
		if (proc_auxv[i] == AT_PHDR) found |= 2;
	}
	return found == 3 ? proc_auxv : 0;
}

/* Whether the page containing p is mapped. mincore reports ENOMEM for
 * unmapped pages instead of faulting; the page size is not known yet,
 * so the possible sizes are tried until the alignment is accepted. */
static int mapped(const void *p)
{
	static const size_t sizes[] = { 4096, 16384, 65536 };
	unsigned char vec;
	for (int i = 0; i < sizeof sizes / sizeof *sizes; i++) {
		long r = __syscall(SYS_mincore, (uintptr_t)p & -sizes[i], 1, &vec);
		if (r != -EINVAL) return !r;
	}
	return 0;
}

/* The page size without AT_PAGESZ. mincore checks the alignment of the
 * address before anything else, so the page size is the smallest power
 * of two s for which it accepts an address that is a multiple of s but
 * not of 2s. Zero if mincore is unusable. */
static size_t probe_page_size(void)
{
	static const size_t sizes[] = { 4096, 16384, 65536 };
	unsigned char vec;
	for (int i = 0; i < sizeof sizes / sizeof *sizes; i++) {
		uintptr_t a = ((uintptr_t)&init_state & -(2*sizes[i])) + sizes[i];
		long r = __syscall(SYS_mincore, a, 1, &vec);
		if (r == -EINVAL) continue;
		return r == 0 || r == -ENOMEM ? sizes[i] : 0;
	}
	return 0;
}

/* glibc and bionic pass (argc, argv, envp) to init_array functions;
 * musl's dynamic linker and others pass nothing, leaving garbage in the
 * argument registers. Accept the arguments only if they describe a
 * plausible initial process stack, and return the auxv that follows. */
static size_t *auxv_from_args(int argc, char **argv, char **envp)
{
	uintptr_t page = -1;
	int found = 0;
	size_t i, *auxv;
	char **p;

#define CHECK(q) do { \
	if (((uintptr_t)(q) & -4096) != page) { \
		if (!mapped(q)) return 0; \
		page = (uintptr_t)(q) & -4096; \
	} } while (0)

	if (argc <= 0 || argc >= 1<<20) return 0;
	if ((uintptr_t)argv % sizeof *argv) return 0;
	if (envp != argv + argc + 1) return 0;
	for (i = 0; i <= argc; i++) CHECK(argv + i);
	if (argv[argc]) return 0;
	for (p = envp; ; p++) {
		CHECK(p);
		if (!*p) break;
		if (p - envp > 1<<20) return 0;
	}
	/* unsetenv done in place leaves more null slots before auxv. */
	do {
		if (++p - envp > 1<<20) return 0;
		CHECK(p);
	} while (!*p);
	auxv = (size_t *)p;
	for (i = 0; ; i += 2) {
		CHECK(auxv + i);
		CHECK(auxv + i + 1);
		if (!auxv[i]) break;
		if (auxv[i] == AT_PAGESZ) found |= 1;
		if (auxv[i] == AT_PHDR) found |= 2;
		if (i > 2*AUXV_MAX) return 0;
	}
	if (found != 3) return 0;
	if (argv[0] && !mapped(argv[0])) return 0;
	return auxv;
#undef CHECK
}

static void init_file_lock(FILE *f)
{
	if (f && f->lock<0) f->lock = 0;
}

/* Must not use __pthread_self(), errno, malloc or stdio output. */
static void do_init(int argc, char **argv, char **envp)
{
	size_t i, *auxv, *arg_auxv, aux[AUX_CNT] = { 0 };
	char *pn;

	arg_auxv = argv ? auxv_from_args(argc, argv, envp) : 0;
	auxv = read_proc_auxv();
	if (!auxv) auxv = arg_auxv;
	if (!auxv) {
		/* Page sizes of 4k, 16k and 64k all exist on aarch64, and
		 * musl sizes every mapping with PAGE_SIZE, so without
		 * AT_PAGESZ the kernel is asked. */
		size_t ps = probe_page_size();
		if (ps) libc.page_size = ps;
#ifdef __aarch64__
		else {
			static const char msg[] =
				"musl-embed: cannot determine the page size\n";
			__syscall(SYS_write, 2, msg, sizeof msg - 1);
			a_crash();
		}
#endif
		auxv = __embed_null_auxv;
	}
	if (arg_auxv) __embed_stack_hint = argv;

	libc.auxv = auxv;
	for (i=0; auxv[i]; i+=2) if (auxv[i]<AUX_CNT) aux[auxv[i]] = auxv[i+1];
	__hwcap = aux[AT_HWCAP];
	if (aux[AT_SYSINFO]) __sysinfo = aux[AT_SYSINFO];
	if (aux[AT_PAGESZ]) libc.page_size = aux[AT_PAGESZ];
	/* The host has already sanitised fds 0-2 for secure programs. */
	libc.secure = aux[AT_SECURE] || aux[AT_UID]!=aux[AT_EUID]
		|| aux[AT_GID]!=aux[AT_EGID];

	pn = arg_auxv && argv[0] ? argv[0] : (char *)aux[AT_EXECFN];
	if (!pn) pn = "";
	__progname = __progname_full = pn;
	for (i=0; pn[i]; i++) if (pn[i]=='/') __progname = pn+i+1;

	__embed_environ_init(arg_auxv ? envp : 0);

	/* The host creates threads without telling musl, so musl must
	 * always take its locks. */
	libc.threaded = 1;
	libc.need_locks = 1;
	libc.threads_minus_1 = 0;
	for (FILE *f=*__ofl_lock(); f; f=f->next)
		init_file_lock(f);
	__ofl_unlock();
	init_file_lock(__stdin_used);
	init_file_lock(__stdout_used);
	init_file_lock(__stderr_used);

	__membarrier_init();
	__embed_host_init();
	__embed_fork_init();
	libc.can_do_threads = !!__embed_host.pthread_create;
}

static void run_init(int argc, char **argv, char **envp)
{
	int s, tid = __syscall(SYS_gettid);
	while ((s = a_cas(&init_state, 0, 1))) {
		if (s == 2) return;
		/* Re-entered from the initialising thread: carry on. */
		if (init_tid == tid) return;
		__wait(&init_state, 0, 1, 1);
	}
	init_tid = tid;
	do_init(argc, argv, envp);
	a_store(&init_state, 2);
	__wake(&init_state, -1, 1);
}

void __embed_ensure_init(void)
{
	if (init_state != 2) run_init(0, 0, 0);
}

static void seed_ssp(void)
{
	size_t i, buf[2];
	void *entropy = 0;

	/* Prefer fresh entropy: glibc derives its own canary from the same
	 * AT_RANDOM bytes, and leaking one should not leak the other. */
	if (__syscall(SYS_getrandom, buf, sizeof buf, 1 /* GRND_NONBLOCK */)
	    == sizeof buf)
		entropy = buf;
	for (i=0; !entropy && libc.auxv[i]; i+=2)
		if (libc.auxv[i]==AT_RANDOM) entropy = (void *)libc.auxv[i+1];
	__init_ssp(entropy);
}

/* Priority 101 runs before every unprioritised constructor of the shared
 * object, including the library's own and crtbegin's. Anything that runs
 * even earlier gets lazy initialisation through __embed_ensure_init. */
__attribute__((__constructor__(101)))
static void embed_ctor(int argc, char **argv, char **envp)
{
	run_init(argc, argv, envp);
	/* Only here: no frame protected by this object's canary is live
	 * while the host dynamic linker runs constructors. */
	seed_ssp();
}

volatile int __embed_exiting;

void __embed_exit_hooks(void)
{
	/* Neither function can run twice: both leave locks held for good,
	 * so the fork handlers must not try to take them afterwards. */
	if (a_swap(&__embed_exiting, 1)) return;
	__funcs_on_exit();
	__stdio_exit();
}

/* Runs after the object's other destructors (fini_array runs in reverse
 * and prioritised entries come first). First the fork handlers are
 * dropped: on glibc < 2.36 that waits for forks in progress, whose
 * handlers would wait for the locks the exit hooks leave held. Then the
 * atexit handlers and C++ static destructors registered with musl run,
 * and musl's stdio is flushed. */
__attribute__((__destructor__(101)))
static void embed_dtor(void)
{
	__embed_fork_fini();
	__embed_exit_hooks();
}
