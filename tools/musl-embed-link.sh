#!/bin/sh
#
# musl-embed-link -o libfoo.so [--exports exports.map] [linker inputs...]
#
# Links a shared library that carries its own copy of musl, linked from
# libc-embed.a, so that it can be loaded by programs that use any other
# libc (see embed/README.md in the musl source). Inputs are objects,
# archives and compiler-driver options as for "cc -shared"; -lc and
# musl's other empty libraries (-lpthread, -lm, -lrt, -ldl, ...) are
# dropped, since libc-embed.a provides all of them. Other -lfoo options
# take libfoo.a only, never libfoo.so. Called without -o, it runs the
# compiler with the arguments as given, so that it can stand in for "cc"
# as a linker that build tools probe.
#
# --exports names a version script listing the library's public API.
# Everything not listed should be local ("local: *;"), so that nothing
# but that API is visible to the host. musl's own symbols are local in
# any case.
#
# Environment:
#   CC                 musl-targeting compiler driver [@CC@]
#   MUSL_EMBED_LIBDIR  directory containing libc-embed.a [@LIBDIR@]
#   MUSL_EMBED_CHECK   set to 0 to skip musl-embed-check-so afterwards
#

cc=${CC:-@CC@}
libdir=${MUSL_EMBED_LIBDIR:-@LIBDIR@}
out=
exports=

# Prints the arguments in response file $1, one per line, splitting as
# GCC does: on white space, with quotes and backslashes as in the shell.
response () {
	awk 'BEGIN { RS = "\001" } {
		n = length($0); arg = ""; have = 0; q = ""; bs = 0
		for (i = 1; i <= n; i++) {
			c = substr($0, i, 1)
			if (bs) { arg = arg c; bs = 0; have = 1 }
			else if (c == "\\") bs = 1
			else if (q != "") { if (c == q) q = ""; else arg = arg c }
			else if (c == "\"" || c == "\047") { q = c; have = 1 }
			else if (c ~ /[ \t\r\n\f\v]/) { if (have) print arg; arg = ""; have = 0 }
			else { arg = arg c; have = 1 }
		}
		if (have) print arg
	}' "$1"
}

# Expand response files (GHC passes its whole link line in one), so that
# the options handled below are seen. Nested ones are not expanded.
n=$#
nl='
'
while test $n -gt 0 ; do
	a=$1 ; shift ; n=$((n-1))
	case $a in
	@?*)
		if test -f "${a#@}" ; then
			list=$(response "${a#@}") || exit
			set -f ; IFS=$nl
			set -- "$@" $list
			unset IFS ; set +f
			continue
		fi ;;
	esac
	set -- "$@" "$a"
done

# Without -o this is not a link but a question for the compiler, such as
# GHC's -Wl,--version probe of its linker (ghc -pgml musl-embed-link).
link=
for a ; do
	case $a in
	-o|-o?*) link=1 ; break ;;
	esac
done
if test -z "$link" && test $# -gt 0 ; then
	exec "$cc" "$@"
fi

# -lfoo becomes -l:libfoo.a, which is what it means here (see -Bstatic
# below), and which keeps nixpkgs' ld wrapper from adding an rpath for
# each directory that also holds a libfoo.so.
n=$#
while test $n -gt 0 ; do
	a=$1 ; shift ; n=$((n-1))
	case $a in
	-l) a=-l$1 ; shift ; n=$((n-1)) ;;
	esac
	case $a in
	-o) out=$1 ; shift ; n=$((n-1)) ; continue ;;
	-o*) out=${a#-o} ; continue ;;
	--exports) exports=$1 ; shift ; n=$((n-1)) ; continue ;;
	--exports=*) exports=${a#*=} ; continue ;;
	-lc|-lpthread|-lm|-lrt|-ldl|-lutil|-lxnet|-lresolv|-lcrypt) continue ;;
	-l:*) ;;
	-l?*) a=-l:lib${a#-l}.a ;;
	-shared|-static|-static-pie|-pie) continue ;;
	esac
	set -- "$@" "$a"
done

test -n "$out" || { echo "usage: $0 -o OUTPUT [--exports MAP] INPUTS..." >&2 ; exit 2 ; }
test -f "$libdir/libc-embed.a" || { echo "$0: $libdir/libc-embed.a not found" >&2 ; exit 1 ; }

# -nodefaultlibs, not -nostdlib: crtbeginS.o provides __dso_handle and
#   runs the library's C++ constructors.
# -Bstatic: the library must not need other shared objects, so libraries
#   named in other ways (-Wl,-lfoo) are static ones too.
# -Bsymbolic: references inside the library bind locally.
# --hash-style=both: DT_HASH for dynamic linkers older than glibc 2.5.
# -z nopack-relative-relocs: DT_RELR needs glibc 2.36.
# -u __embed_ensure_init: pulls in musl's initialisation.
# The compiler's runtime is libgcc.a, plus libgcc_eh.a where the unwinder
# is separate from it (a toolchain built without libgcc_s keeps it in
# libgcc.a and has no libgcc_eh.a).
libcc=-lgcc
case $("$cc" -print-file-name=libgcc_eh.a) in /*) libcc="$libcc -lgcc_eh" ;; esac
link () {
	"$cc" -shared -nodefaultlibs \
		${exports:+"-Wl,--version-script=$exports"} \
		-Wl,-Bstatic -Wl,-Bsymbolic -Wl,--hash-style=both \
		-Wl,-z,noexecstack -Wl,-z,relro -Wl,-z,now \
		-Wl,-z,nopack-relative-relocs \
		-Wl,--exclude-libs,libc-embed.a:libgcc.a:libgcc_eh.a \
		-Wl,-u,__embed_ensure_init \
		-o "$out" "$@" \
		-Wl,--start-group "$libdir/libc-embed.a" $libcc -Wl,--end-group
}

link "$@" || exit

undefined () {
	${READELF:-readelf} --dyn-syms -W "$out" |
	awk -v b="$1" '$5 == b && $7 == "UND" && $8 != "" { sub(/@.*/, "", $8); print $8 }'
}

# The linker does not take archive members just to satisfy weak
# references, so a weak reference to something an input archive defines
# (libsupc++ has one to __cxa_pure_virtual) would be left for the host to
# bind. Link again, asking for those symbols explicitly. crtbeginS.o's
# weak references are meant for the host and stay, as do libstdc++'s to
# libitm (transactional memory clones, only called from -fgnu-tm code).
weak=$(undefined WEAK |
	grep -vxE '__cxa_finalize|_ITM_[A-Za-z0-9_]*|_ZGTt[A-Za-z0-9_]*|__gmon_start__|__register_frame_info|__deregister_frame_info|environ')
if test -n "$weak" ; then
	link $(printf -- '-Wl,-u,%s ' $weak) "$@" || exit
	# -u makes the reference strong; a symbol nothing defines must stay
	# weak, for the check below to report it as it is.
	left=$(undefined GLOBAL | grep -xF "$weak")
	if test -n "$left" ; then
		weak=$(printf '%s\n' $weak | grep -vxF "$left")
		link ${weak:+$(printf -- '-Wl,-u,%s ' $weak)} "$@" || exit
	fi
fi

test "$MUSL_EMBED_CHECK" = 0 && exit 0
# The compiler's own objdump, for the disassembly checks; OBJDUMP in the
# environment may be for the build machine.
case $cc in
*-gcc|*-cc|*-clang) pfx=${cc%-*}- ;;
*) pfx= ;;
esac
if test -n "$pfx" && command -v "${pfx}objdump" >/dev/null ; then
	OBJDUMP=${pfx}objdump
	export OBJDUMP
fi
check=$(dirname "$0")/musl-embed-check-so
test -x "$check" || check=musl-embed-check-so
MUSL_EMBED_LIBC=${MUSL_EMBED_LIBC:-$libdir/libc-embed.a} exec "$check" "$out"
