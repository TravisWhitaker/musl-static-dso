#ifndef UNISTD_H
#define UNISTD_H

#include "../../include/unistd.h"

#ifdef MUSL_EMBED
/* The environment array is the host's (embed/environ.c). */
hidden char ***__embed_environ(void);
#define __environ (*__embed_environ())
#else
extern char **__environ;
#endif

hidden int __dup3(int, int, int);
hidden int __mkostemps(char *, int, int);
hidden int __execvpe(const char *, char *const *, char *const *);
hidden off_t __lseek(int, off_t, int);

#endif
