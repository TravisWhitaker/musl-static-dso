#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <elf.h>
#include "pthread_impl.h"
#include "dynlink.h"
#include "libc.h"
#include "embed_impl.h"

/* Dynamic loading for the embedded build is the host's. The constants
 * (RTLD_*), Dl_info and struct dl_phdr_info are the same in musl and
 * glibc. Handles returned by dlopen are host handles. Forwarding
 * dl_iterate_phdr is what lets the unwinder in libgcc_eh find this
 * object's PT_GNU_EH_FRAME, so C++ exceptions and backtraces work. */

hidden char *__embed_dlerror_local(void);

void *dlopen(const char *file, int mode)
{
	void *(*f)(const char *, int) = EMBED_HOST(dlopen);
	if (f) return f(file, mode);
	__dl_seterr("Dynamic loading not supported");
	return 0;
}

/* The host computes RTLD_NEXT from the return address, which is in this
 * object: "the next object after this one", as expected. */
void *dlsym(void *restrict p, const char *restrict s)
{
	void *(*f)(void *, const char *) = EMBED_HOST(dlsym);
	if (f) return f(p, s);
	__dl_seterr("Symbol not found: %s", s);
	return 0;
}

int dlclose(void *p)
{
	int (*f)(void *) = EMBED_HOST(dlclose);
	if (f) return f(p);
	return __dl_invalid_handle(p);
}

char *dlerror(void)
{
	char *(*f)(void);
	if (__pthread_self()->dlerror_flag) return __embed_dlerror_local();
	f = EMBED_HOST(dlerror);
	return f ? f() : 0;
}

int dladdr(const void *addr, Dl_info *info)
{
	int (*f)(const void *, void *) = EMBED_HOST(dladdr);
	return f ? f(addr, info) : 0;
}

/* Other requests have different numbers and result layouts in glibc. */
int dlinfo(void *dso, int req, void *res)
{
	int (*f)(void *, int, void *) = EMBED_HOST(dlinfo);
	if (f && req == RTLD_DI_LINKMAP) return f(dso, req, res);
	if (!f) {
		__dl_invalid_handle(dso);
		return -1;
	}
	__dl_seterr("Unsupported request %d", req);
	return -1;
}

extern hidden const ElfW(Ehdr) __ehdr_start __attribute__((__weak__));
extern hidden const ElfW(Dyn) _DYNAMIC[];

/* Without the host's: report the executable, from auxv, and this object,
 * from its own headers, which is what the unwinder needs. TLS data of
 * either cannot be determined. */
static int fallback_dl_iterate_phdr(int(*callback)(struct dl_phdr_info *info, size_t size, void *data), void *data)
{
	const unsigned char *p;
	const ElfW(Phdr) *phdr;
	struct dl_phdr_info info = { 0 };
	size_t i, n, aux[AUX_CNT] = { 0 };
	int r;

	for (i=0; libc.auxv[i]; i+=2)
		if (libc.auxv[i]<AUX_CNT) aux[libc.auxv[i]] = libc.auxv[i+1];
	if (aux[AT_PHDR]) {
		for (p=(void *)aux[AT_PHDR],n=aux[AT_PHNUM]; n; n--,p+=aux[AT_PHENT]) {
			phdr = (void *)p;
			if (phdr->p_type == PT_PHDR)
				info.dlpi_addr = aux[AT_PHDR] - phdr->p_vaddr;
		}
		info.dlpi_name = "";
		info.dlpi_phdr = (void *)aux[AT_PHDR];
		info.dlpi_phnum = aux[AT_PHNUM];
		if ((r = callback(&info, sizeof info, data))) return r;
	}
	if (!&__ehdr_start) return 0;
	info = (struct dl_phdr_info){ 0 };
	info.dlpi_phdr = (void *)((char *)&__ehdr_start + __ehdr_start.e_phoff);
	info.dlpi_phnum = __ehdr_start.e_phnum;
	for (i=0; i<info.dlpi_phnum; i++)
		if (info.dlpi_phdr[i].p_type == PT_DYNAMIC)
			info.dlpi_addr = (size_t)_DYNAMIC - info.dlpi_phdr[i].p_vaddr;
	info.dlpi_name = "";
	return callback(&info, sizeof info, data);
}

int dl_iterate_phdr(int(*callback)(struct dl_phdr_info *info, size_t size, void *data), void *data)
{
	int (*f)(int (*)(struct dl_phdr_info *, size_t, void *), void *) = EMBED_HOST(dl_iterate_phdr);
	if (f) return f(callback, data);
	return fallback_dl_iterate_phdr(callback, data);
}
