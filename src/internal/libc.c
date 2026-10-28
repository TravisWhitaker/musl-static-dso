#include "libc.h"

#ifdef MUSL_EMBED
#include "embed_impl.h"

/* The embedded build has no process entry point of its own; the host
 * starts threads, so musl always behaves as multithreaded. Valid
 * values are provided statically so that nothing that might run
 * before embed/init.c dereferences a null auxv. */
size_t __embed_null_auxv[2];
struct __libc __libc = {
	.threaded = 1,
	.need_locks = 1,
	.auxv = __embed_null_auxv,
	.page_size = 4096,
};
#else
struct __libc __libc;
#endif

size_t __hwcap;
char *__progname=0, *__progname_full=0;

weak_alias(__progname, program_invocation_short_name);
weak_alias(__progname_full, program_invocation_name);
