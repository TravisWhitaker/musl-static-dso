#define _GNU_SOURCE
#include <elf.h>
#include <link.h>
#include <string.h>
#include <stdint.h>
#include "libc.h"
#include "atomic.h"
#include "embed_impl.h"

/* Finds host libc functions at run time, so that the shared object has
 * no DT_NEEDED entries and no undefined symbols beyond what the ELF TLS
 * ABI requires, and so that musl can keep defining the same names
 * (pthread_create, fork, dlopen...) for the library's own use.
 *
 * The host dynamic linker's list of loaded objects is reached through
 * the r_debug structure that it publishes for debuggers. Only the first
 * five fields of struct link_map are used; they are the same in every
 * dynamic linker. Lookup follows list order (executable first, then
 * libraries in load order), approximating the global scope.
 *
 * Nothing here may allocate, use __pthread_self() or be a cancellation
 * point: it runs before the calling thread's TCB exists. */

struct __embed_host __embed_host;
int __embed_host_timers;

extern hidden const ElfW(Dyn) _DYNAMIC[];

static struct r_debug *rdebug;

struct obj {
	uintptr_t base;
	const char *strings;
	const ElfW(Sym) *syms;
	const uint32_t *hashtab;
	const uint32_t *ghashtab;
	const ElfW(Half) *versym;
};

static uint32_t sysv_hash(const char *s0)
{
	const unsigned char *s = (void *)s0;
	uint_fast32_t h = 0;
	while (*s) {
		h = 16*h + *s++;
		h ^= h>>24 & 0xf0;
	}
	return h & 0xfffffff;
}

static uint32_t gnu_hash(const char *s0)
{
	const unsigned char *s = (void *)s0;
	uint_fast32_t h = 5381;
	for (; *s; s++)
		h += h*32 + *s;
	return h;
}

static uintptr_t distance(uintptr_t a, uintptr_t b)
{
	return a > b ? a - b : b - a;
}

static int decode(struct obj *o, uintptr_t base, const ElfW(Dyn) *dyn)
{
	uintptr_t d = (uintptr_t)dyn;
	memset(o, 0, sizeof *o);
	o->base = base;
	for (; dyn->d_tag != DT_NULL; dyn++) {
		/* glibc relocates these pointers in place, except in objects
		 * whose dynamic section is read-only (such as the vDSO);
		 * musl's and bionic's dynamic linkers leave the link-time
		 * addresses. The tables are in the same object as the dynamic
		 * section, so whichever reading lands nearer to it is right.
		 * (Comparing with the base fails for objects linked at high
		 * addresses: the x86_64 vDSO before Linux 3.16, prelinked
		 * libraries.) */
		uintptr_t p = dyn->d_un.d_ptr;
		if (distance(p + base, d) < distance(p, d)) p += base;
		switch (dyn->d_tag) {
		case DT_STRTAB: o->strings = (void *)p; break;
		case DT_SYMTAB: o->syms = (void *)p; break;
		case DT_HASH: o->hashtab = (void *)p; break;
		case DT_GNU_HASH: o->ghashtab = (void *)p; break;
		case DT_VERSYM: o->versym = (void *)p; break;
		}
	}
	return o->strings && o->syms && (o->hashtab || o->ghashtab);
}

#define OK_TYPES (1<<STT_NOTYPE | 1<<STT_OBJECT | 1<<STT_FUNC | 1<<STT_GNU_IFUNC)
#define OK_BINDS (1<<STB_GLOBAL | 1<<STB_WEAK | 1<<STB_GNU_UNIQUE)

static int match(const struct obj *o, uint32_t i, const char *name)
{
	const ElfW(Sym) *s = o->syms + i;
	if (s->st_shndx == SHN_UNDEF) return 0;
	if (!(1<<(s->st_info&0xf) & OK_TYPES)) return 0;
	if (!(1<<(s->st_info>>4) & OK_BINDS)) return 0;
	/* Skip non-default versions, e.g. pthread_create@GLIBC_2.2.5 next
	 * to pthread_create@@GLIBC_2.34. */
	if (o->versym && (o->versym[i] & 0x8000)) return 0;
	return !strcmp(name, o->strings + s->st_name);
}

static const ElfW(Sym) *lookup(const struct obj *o, const char *name)
{
	uint32_t i;
	if (o->ghashtab) {
		const uint32_t *ht = o->ghashtab;
		uint32_t h1 = gnu_hash(name), nbuckets = ht[0];
		const uint32_t *buckets = ht + 4 + ht[2]*(sizeof(size_t)/4);
		const uint32_t *hashval;
		/* An object that exports nothing may have no buckets (lld). */
		if (!nbuckets || !(i = buckets[h1 % nbuckets])) return 0;
		hashval = buckets + nbuckets + (i - ht[1]);
		for (h1 |= 1; ; i++) {
			uint32_t h2 = *hashval++;
			if ((h2|1) == h1 && match(o, i, name)) return o->syms + i;
			if (h2 & 1) return 0;
		}
	}
	const uint32_t *ht = o->hashtab;
	if (!ht[0]) return 0;
	for (i = ht[2 + sysv_hash(name) % ht[0]]; i; i = ht[2 + ht[0] + i])
		if (match(o, i, name)) return o->syms + i;
	return 0;
}

static void *address(const struct obj *o, const ElfW(Sym) *s)
{
	void *p = (void *)(o->base + s->st_value);
	if ((s->st_info&0xf) == STT_GNU_IFUNC)
		p = ((void *(*)(unsigned long, void *))p)(__hwcap, 0);
	return p;
}

/* Looks name up in the object whose ELF header is at ehdr. */
static void *lookup_ehdr(uintptr_t ehdr, const char *name)
{
	const ElfW(Ehdr) *eh = (void *)ehdr;
	const ElfW(Phdr) *ph;
	const ElfW(Sym) *s;
	struct obj o;
	uintptr_t base = 0, dyn = 0;
	int i, have_base = 0;

	if (memcmp(eh->e_ident, ELFMAG, SELFMAG)) return 0;
	ph = (void *)(ehdr + eh->e_phoff);
	for (i = 0; i < eh->e_phnum; i++, ph = (void *)((char *)ph + eh->e_phentsize)) {
		if (ph->p_type == PT_LOAD && !have_base) {
			base = ehdr - (ph->p_vaddr & -(uintptr_t)ph->p_align);
			have_base = 1;
		}
		if (ph->p_type == PT_DYNAMIC) dyn = ph->p_vaddr;
	}
	if (!have_base || !dyn || !decode(&o, base, (void *)(base + dyn)))
		return 0;
	return (s = lookup(&o, name)) ? address(&o, s) : 0;
}

static struct r_debug *find_r_debug(void)
{
	size_t i, *auxv = libc.auxv;
	uintptr_t phdr = 0, phnum = 0, phent = 0, interp = 0, bias = 0, dyn = 0;
	uintptr_t load = -1;
	int have_bias = 0;
	const ElfW(Ehdr) *eh;
	const ElfW(Phdr) *ph;
	const ElfW(Dyn) *d;
	struct r_debug *r;

	for (i = 0; auxv[i]; i += 2) switch (auxv[i]) {
	case AT_PHDR: phdr = auxv[i+1]; break;
	case AT_PHNUM: phnum = auxv[i+1]; break;
	case AT_PHENT: phent = auxv[i+1]; break;
	case AT_BASE: interp = auxv[i+1]; break;
	}
	if (!phdr || !phent) return 0;

	/* The executable's DT_DEBUG entry, filled in by the dynamic linker
	 * for debuggers. The PT_PHDR trick for the load bias is the one in
	 * __init_tls.c. */
	for (i = 0, ph = (void *)phdr; i < phnum; i++, ph = (void *)((char *)ph + phent)) {
		if (ph->p_type == PT_PHDR) bias = phdr - ph->p_vaddr, have_bias = 1;
		if (ph->p_type == PT_DYNAMIC) dyn = ph->p_vaddr;
		if (ph->p_type == PT_LOAD && load == -1)
			load = ph->p_vaddr & -(uintptr_t)ph->p_align;
	}
	/* Without PT_PHDR: a non-PIE executable (bias 0), a PIE linked
	 * without one, or the dynamic linker itself, run as a command (then
	 * the kernel's auxv describes it, not the program). In the last two
	 * cases the ELF header precedes the program headers. */
	eh = (void *)(phdr - sizeof *eh);
	if (!have_bias && !memcmp(eh->e_ident, ELFMAG, SELFMAG)
	    && eh->e_phoff == sizeof *eh && eh->e_type == ET_DYN) {
		if ((r = lookup_ehdr((uintptr_t)eh, "_r_debug"))) return r;
		if (load != -1) bias = (uintptr_t)eh - load;
	}
	if (dyn) for (d = (void *)(bias + dyn); d->d_tag != DT_NULL; d++)
		if (d->d_tag == DT_DEBUG && d->d_un.d_ptr)
			return (void *)d->d_un.d_ptr;

	/* No DT_DEBUG: ask the dynamic linker for its _r_debug. */
	if (interp && (r = lookup_ehdr(interp, "_r_debug"))) return r;
	return 0;
}

static void *host_lookup(const char *name)
{
	const struct link_map *lm;
	const ElfW(Sym) *s;
	struct obj o;

	if (!rdebug) return 0;
	for (lm = rdebug->r_map; lm; lm = lm->l_next) {
		if (!lm->l_ld || lm->l_ld == _DYNAMIC) continue;
		if (!decode(&o, lm->l_addr, lm->l_ld)) continue;
		if ((s = lookup(&o, name))) return address(&o, s);
	}
	return 0;
}

void __embed_host_init(void)
{
	rdebug = find_r_debug();
#define RESOLVE(r, n, a) __embed_host.n = (__typeof__(__embed_host.n))host_lookup(#n);
	EMBED_HOST_SYMS(RESOLVE)
#undef RESOLVE
	/* timer_t values must have one meaning for the life of the process,
	 * so this is decided once (see timer_create.c). */
	__embed_host_timers = __embed_host.timer_create && __embed_host.timer_settime
		&& __embed_host.timer_gettime && __embed_host.timer_delete
		&& __embed_host.timer_getoverrun;
}

/* For functions that were missing at init, e.g. in libpthread.so.0 on
 * glibc < 2.34 when that was dlopened later. The walk is read-only; the
 * only hazard is a concurrent dlclose unmapping an object being read. */
void *__embed_host_resolve(void *volatile *slot, const char *name)
{
	void *p;
	__embed_ensure_init();
	if ((p = *slot)) return p;
	if ((p = host_lookup(name))) a_cas_p(slot, 0, p);
	return p;
}

int __embed_host_errno(int fallback)
{
	int *(*f)(void) = __embed_host.__errno_location;
	return f ? *f() : fallback;
}
