#!/bin/sh
#
# musl-embed-check-so libfoo.so
#
# Checks a shared library linked with libc-embed.a (see musl-embed-link)
# for properties it needs to load into a process that uses another libc,
# including old ones: no dependencies, no imports beyond what the ELF TLS
# ABI requires, nothing of musl exported, nothing that needs a recent
# dynamic linker, and no use of the host's thread pointer layout.
#
# Environment:
#   READELF, OBJDUMP   tools to use [readelf, objdump]
#   MUSL_EMBED_LIBC    libc-embed.a; if set, no symbol it defines may be
#                      exported
#

so=$1
READELF=${READELF:-readelf}
OBJDUMP=${OBJDUMP:-objdump}
status=0

fail () {
	printf 'musl-embed-check-so: %s: %s\n' "$so" "$*" >&2
	status=1
}

test -f "$so" || { echo "usage: $0 LIBRARY" >&2 ; exit 2 ; }

hdr=$($READELF -hW "$so") || exit 1
case $hdr in
*AArch64*) arch=aarch64 ;;
*X86-64*) arch=x86_64 ;;
*) fail "unsupported machine" ; exit 1 ;;
esac

dyn=$($READELF -dW "$so")
dynsyms=$($READELF --dyn-syms -W "$so")
relocs=$($READELF -rW "$so")
phdrs=$($READELF -lW "$so")
sections=$($READELF -SW "$so")

printf '%s\n' "$dyn" | grep '(NEEDED)' >/dev/null && fail "has DT_NEEDED entries"
# Harmless without DT_NEEDED, but a build path in the binary.
printf '%s\n' "$dyn" | grep -E '\((RPATH|RUNPATH)\)' >/dev/null &&
	printf 'musl-embed-check-so: %s: warning: has DT_RPATH/DT_RUNPATH, which it does not use\n' "$so" >&2
printf '%s\n' "$dyn" | grep '(TEXTREL)' >/dev/null && fail "has text relocations (non-PIC code)"
printf '%s\n' "$dyn" | grep 'STATIC_TLS' >/dev/null &&
	fail "uses initial-exec TLS (DF_STATIC_TLS); dlopen may fail"
printf '%s\n' "$dyn" | grep '(RELR' >/dev/null && fail "uses DT_RELR (needs glibc 2.36)"
printf '%s\n' "$dyn" | grep '(GNU_HASH)' >/dev/null || fail "no DT_GNU_HASH"
printf '%s\n' "$dyn" | grep '(HASH)' >/dev/null || fail "no DT_HASH (needed before glibc 2.5)"

# Undefined dynamic symbols bind to the host. Allowed: the TLS ABI entry
# point, the environment, and the weak references of crtbeginS.o (and
# libstdc++'s to libitm), which are meant for the host.
bad=$(printf '%s\n' "$dynsyms" | awk -v arch=$arch '
	$7 == "UND" && $8 != "" {
		name = $8; sub(/@.*/, "", name); bind = $5
		if (name == "__tls_get_addr" && bind == "GLOBAL" && arch == "x86_64") next
		if (name == "environ") next
		if (bind == "WEAK" && name ~ /^(__cxa_finalize|_ITM_[A-Za-z0-9_]*|_ZGTt[A-Za-z0-9_]*|__gmon_start__|__register_frame_info|__deregister_frame_info)$/) next
		print name " (" bind ")"
	}')
test -n "$bad" && fail "imports symbols that would bind to the host libc:" $bad

if test -n "$MUSL_EMBED_LIBC" ; then
	exported=$(printf '%s\n' "$dynsyms" | awk '$7 != "UND" && $7 != "Ndx" && ($5 == "GLOBAL" || $5 == "WEAK") { print $8 }' | sed 's/@.*//' | sort -u)
	libc=$($READELF -sW "$MUSL_EMBED_LIBC" | awk '$7 != "UND" && $7 != "Ndx" && ($5 == "GLOBAL" || $5 == "WEAK") { print $8 }' | sort -u)
	leaked=$(printf '%s\n%s\n' "$exported" "$libc" | sort | uniq -d | grep -v '^$')
	test -n "$leaked" && fail "exports libc symbols:" $leaked
fi

ntls=$(printf '%s\n' "$phdrs" | grep -c '^ *TLS ')
test "$ntls" = 1 || fail "expected one PT_TLS segment, found $ntls"

printf '%s\n' "$relocs" | grep '_COPY ' >/dev/null && fail "has copy relocations"
printf '%s\n' "$relocs" | grep '_IRELATIVE' >/dev/null && fail "has IRELATIVE relocations"
printf '%s\n' "$relocs" | grep -E 'R_X86_64_TPOFF|R_AARCH64_TLS_TPREL' >/dev/null &&
	fail "uses initial-exec TLS relocations; dlopen may fail"
test $arch = x86_64 && printf '%s\n' "$relocs" | grep 'TLSDESC' >/dev/null &&
	fail "uses TLS descriptors (-mtls-dialect=gnu2) on x86_64"
printf '%s\n' "$sections" | grep '\.relr' >/dev/null && fail "has a .relr.dyn section"

# The first init_array entry must be musl's initialisation, so that it
# runs before the library's own constructors.
ia=$(printf '%s\n' "$sections" | awk '$2 == ".init_array" { print $4 }')
if test -n "$ia" ; then
	first=$(printf '%s\n' "$relocs" | awk -v off="$ia" '
		{ o = $1; sub(/^0+/, "", o); t = off; sub(/^0+/, "", t) }
		o == t && $3 ~ /RELATIVE/ { print $NF; exit }')
	ctor=$($READELF -sW "$so" | awk '$8 == "embed_ctor" { print $2; exit }')
	if test -z "$ctor" ; then
		echo "musl-embed-check-so: $so: note: no symbol table; init_array order not checked" >&2
	elif test -z "$first" ; then
		fail "cannot determine the first init_array entry"
	else
		first=$(printf '%s' "$first" | sed 's/^0*//')
		ctor=$(printf '%s' "$ctor" | sed 's/^0*//')
		test "$first" = "$ctor" || fail "musl's constructor is not first in init_array"
	fi
else
	fail "no .init_array (musl's constructor is missing)"
fi

# Direct thread-pointer reads (__builtin_thread_pointer, inline asm,
# local-exec TLS) use the absolute form, `mov %fs:0x0,%rax`; the only
# one allowed is the stack protector's %fs:0x28. Register-relative forms
# (`%fs:(%rax)`) belong to initial-exec and TLS-descriptor accesses,
# rejected above by their relocations, and are also what objdump prints
# for data in .text: GHC's info tables precede their entry code and have
# no symbol, so a 0x64 byte in them becomes an fs prefix on whatever
# follows. The moffs encoding (movabs) is never used for the thread
# pointer by compilers or gas; it too only comes from such mis-decoding.
if test $arch = x86_64 && command -v "$OBJDUMP" >/dev/null ; then
	fs=$($OBJDUMP -d "$so" | grep -E '%fs:-?0x[0-9a-f]+([^(0-9a-f]|$)' |
		grep -vE '%fs:0x28([^(0-9a-f]|$)|movabs')
	test -n "$fs" && fail "accesses the thread pointer directly:
$fs"
fi

test $status = 0 && printf 'musl-embed-check-so: %s: ok\n' "$so"
exit $status
