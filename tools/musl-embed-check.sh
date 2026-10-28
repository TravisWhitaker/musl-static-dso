#!/bin/sh
#
# musl-embed-check.sh ARCH libc-embed.a [TOOL_PREFIX]
#
# Checks that the embedded libc never touches state that belongs to the
# host libc: the thread pointer, the kernel's per-thread robust list and
# clear_child_tid address, and that it reaches its own per-thread data
# only through the host dynamic linker's TLS support. Run after every
# build of libc-embed.a.
#

arch=$1
lib=$2
pfx=$3
# An explicit prefix wins over NM/OBJDUMP from the environment, which may
# name tools for another target.
if test -n "$pfx" ; then
	NM=${pfx}nm
	OBJDUMP=${pfx}objdump
else
	NM=${NM:-nm}
	OBJDUMP=${OBJDUMP:-objdump}
fi
status=0

fail () {
	printf 'musl-embed-check: %s: %s\n' "$lib" "$*" >&2
	status=1
}

test -f "$lib" || { fail "no such file"; exit 1; }

syms=$($NM -A "$lib") || { fail "$NM failed"; exit 1; }

case $arch in
x86_64)
	printf '%s\n' "$syms" | grep ' U __tls_get_addr$' >/dev/null ||
		fail "no reference to __tls_get_addr (TLS model is not dynamic)"
	;;
aarch64)
	! printf '%s\n' "$syms" | grep ' __tls_get_addr$' >/dev/null ||
		fail "__tls_get_addr is referenced (expected TLS descriptors only)"
	;;
*)
	fail "unsupported architecture $arch"
	;;
esac

for s in __tls_get_addr __init_tp __copy_tls __set_thread_area __unmapself \
	__init_tls __reset_tls __libc_start_main __environ environ; do
	printf '%s\n' "$syms" | grep " [ABCDGRSTVWbdgrst] $s\$" >/dev/null &&
		fail "defines $s"
done

dis=$($OBJDUMP -dr "$lib") || { fail "$OBJDUMP failed"; exit 1; }

# One line per problem found in the disassembly; empty if none.
problems=$(printf '%s\n' "$dis" | awk -v arch="$arch" '
function report(what) { printf "%s: %s\n", fn, what }
function hex(s,   i, c, v) {
	v = 0; sub(/^0x/, "", s)
	for (i = 1; i <= length(s); i++) {
		c = index("0123456789abcdef", substr(s, i, 1)) - 1
		if (c < 0) break
		v = v*16 + c
	}
	return v
}
function endfn() {
	if (arch == "aarch64" && has_mrs && !has_tlsdesc)
		report("reads tpidr_el0 outside a TLS descriptor sequence")
	has_mrs = has_tlsdesc = 0; nr = -1
}
/^[0-9a-f]+ <.*>:$/ { endfn(); fn = $2; gsub(/[<>:]/, "", fn); next }
/^In archive/ || /file format/ { endfn(); obj = $0; next }
arch == "x86_64" {
	t = $0; gsub(/%fs:0x28/, "", t)
	if (t ~ /%fs:/)
		report("thread pointer access: " $0)
	if ($0 ~ /mov +\$0x[0-9a-f]+,%(e|r)ax/) {
		s = $0; sub(/.*mov +\$/, "", s); sub(/,.*/, "", s); nr = hex(s)
	} else if ($0 ~ /,%(e|r)ax$/ || $0 ~ /%(e|r)ax,%(e|r)ax/) nr = -1
	if ($0 ~ /\tsyscall/) {
		# (musl never calls the public arch_prctl() wrapper itself.)
		if (nr == 158 && fn != "arch_prctl") report("arch_prctl syscall")
		if (nr == 218) report("set_tid_address syscall")
		if (nr == 273) report("set_robust_list syscall")
		if (nr == 200) report("tkill syscall")
	}
}
arch == "aarch64" {
	if ($0 ~ /msr[ \t]+tpidr_el0/) report("writes tpidr_el0")
	if ($0 ~ /mrs[ \t]+[xw][0-9]+, *tpidr_el0/) has_mrs = 1
	if ($0 ~ /R_AARCH64_TLSDESC_CALL/) has_tlsdesc = 1
	if ($0 ~ /mov[ \t]+[xw]8, #0x[0-9a-f]+/) {
		s = $0; sub(/.*mov[ \t]+[xw]8, #/, "", s); sub(/[^0-9a-fx].*/, "", s); nr = hex(s)
	} else if ($0 ~ /\t[a-z]+[ \t]+[xw]8,/) nr = -1
	if ($0 ~ /\tsvc[ \t]/) {
		if (nr == 96) report("set_tid_address syscall")
		if (nr == 99) report("set_robust_list syscall")
		if (nr == 130) report("tkill syscall")
	}
}
END { endfn() }')

if test -n "$problems" ; then
	printf '%s\n' "$problems" | while IFS= read -r line ; do
		printf 'musl-embed-check: %s: %s\n' "$lib" "$line" >&2
	done
	status=1
fi

test $status = 0 && printf 'musl-embed-check: %s: ok\n' "$lib"
exit $status
