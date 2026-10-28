#include <unistd.h>

#ifndef MUSL_EMBED
extern char **__environ;
#endif

int execv(const char *path, char *const argv[])
{
	return execve(path, argv, __environ);
}
