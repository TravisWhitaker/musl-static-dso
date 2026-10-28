#include <unistd.h>
#include "embed_impl.h"

/* The library's own code and musl share the host's environment array,
 * so that changes made on either side are seen by the other. The symbol
 * stays undefined in the shared object and is bound by the host dynamic
 * linker (weakly, so that a host without it can still load us). */
extern char **__embed_host_environ __asm__("environ") __attribute__((__weak__));

static char **snapshot;

char ***__embed_environ(void)
{
	__embed_ensure_init();
	return &__embed_host_environ ? &__embed_host_environ : &snapshot;
}

void __embed_environ_init(char **envp)
{
	snapshot = envp;
}
