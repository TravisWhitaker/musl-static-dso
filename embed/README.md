# libc-embed.a: musl for shared libraries in foreign-libc processes

`libc-embed.a` is a build of musl meant to be linked statically into a
shared library (`libfoo.so`) that is then loaded by a program using some
other libc, typically glibc of any age, through `DT_NEEDED` or `dlopen`.
The library keeps its own musl; the program keeps its own libc. Both work
on every thread.

It is built from the same tree as the ordinary `libc.a`/`libc.so` (with
`-DMUSL_EMBED`, as `.eo` objects), next to them, by default on x86_64 and
aarch64 (`configure --disable-embed` turns it off). Headers and ABI are
upstream musl's, so code compiled against this tree's headers, or any musl
toolchain's, can be linked with it.

## How it works

- **No thread pointer.** musl's per-thread state (`struct pthread`) lives
  in an ordinary `__thread` variable of the shared object, reached through
  the host dynamic linker (`__tls_get_addr` on x86_64, TLS descriptors on
  aarch64). The host allocates it for every thread, including threads
  created before the library was loaded, and frees it at thread exit. musl
  never reads or writes `%fs`/`tpidr_el0` contents, the kernel robust list
  or `clear_child_tid`, which belong to the host.
- **Threads come from the host.** musl's `pthread_create` calls the host's,
  so every thread has a proper host TCB: the library's threads can run
  client callbacks and client signal handlers. musl keeps its own thread
  structure and join/detach protocol for them, and joins or detaches the
  host thread along with it.
- **Host functions are found at run time.** The shared object imports
  nothing but `__tls_get_addr` (x86_64 only) and `environ`. Host functions
  musl needs (`pthread_create`, `fork`, `dlopen`, `exit`, `setuid`,
  `timer_create`, ...) are looked up in the host dynamic linker's link map
  (found through the executable's `DT_DEBUG`, or the dynamic linker's
  `_r_debug`). If one is missing, only the feature that needs it degrades.
- **Process-wide state stays the host's.** The environment is the host's
  `environ`; the program break is left to the host's malloc (mallocng uses
  only `mmap`); musl's `exit` is the host's, and musl's atexit handlers and
  stdio flush run from the library's destructor, after its other
  destructors; `fork` runs musl's lock protocol as a host atfork handler;
  `dlopen` and friends are the host's; credential changes go through the
  host, which applies them to all threads (as does `setrlimit` on kernels
  without `prlimit64`); implementation-internal signals (32 and 33 on
  glibc) are never touched.

## Linking a library

```sh
CC=x86_64-unknown-linux-musl-gcc musl-embed-link -o libfoo.so \
    --exports exports.map foo.o libbar.a [-lsupc++]
```

`musl-embed-link` (installed in `$prefix/bin`) runs the compiler driver
with the required flags, adds `libc-embed.a` and the compiler's `libgcc.a`
(and `libgcc_eh.a` where that is separate), and then checks the result
with `musl-embed-check-so`. It takes what `cc -shared` takes, including
response files (`@file`), so a build system can use it as its linker for
the library. The version script should list the library's public API and
make everything else local:

```
{ global: foo_*; local: *; };
```

Everything that goes into the library must be compiled with `-fPIC`, and on
x86_64 with `-mtls-dialect=gnu` (the default dialect; not `gnu2`). Do not
use `-ftls-model=initial-exec` for code that may be dlopened.
Dependencies go in as PIC static archives: `musl-embed-link` links with
`-Bstatic`, so `-lfoo` takes `libfoo.a` and never `libfoo.so` (C++ code
adds `-lstdc++`).

With a nixpkgs cross stdenv (`pkgsCross.musl64.stdenv.mkDerivation`), set
`NIX_NO_SELF_RPATH = true`; otherwise stdenv adds a `RUNPATH` naming the
output's `lib` directory (inert without `DT_NEEDED`, but a store path in
the binary).

Called without `-o`, `musl-embed-link` passes its arguments to the
compiler, so it can stand in for `cc` where a build tool probes its
linker. A GHC foreign library (cabal `foreign-library`, with
`options: standalone` so that the Haskell libraries and the RTS are linked
in statically) is linked by GHC with `-pgml musl-embed-link`, and with
`-Wl,--version-script=...` in the package's `ld-options`; see
`tests/haskell/` in the repository this fork lives in.

On the client side nothing special is needed: `cc main.c -L. -lfoo`, or
`dlopen("libfoo.so", RTLD_NOW)`. With glibc older than 2.34, link the
client with `-lpthread -ldl` (and `-lrt` for POSIX timers), or the library
cannot create threads or load objects. If an old linker complains that
`__tls_get_addr` is undefined, add the dynamic linker to the link line.

## Limitations

- `pthread_cancel` returns `ENOTSUP`; there is no signal for it.
- `pthread_join`/`pthread_detach` work only on threads musl created.
- Robust mutexes are robust in userspace only (owned threads that exit
  release them; a thread killed while holding one does not).
- TSD destructors do not run when a host-created thread exits; deleted
  keys are never reused (`PTHREAD_KEYS_MAX` keys per process lifetime).
- Library threads must not end through the host's `pthread_exit` or be
  cancelled by the host; they must return or call musl's `pthread_exit`.
  On a host thread, musl's `pthread_exit` hands over to the host's, which
  in glibc needs `libgcc_s.so.1` to be loadable (it aborts otherwise).
- `timer_create(SIGEV_THREAD)` needs the host's POSIX timers; then all
  timers are the host's.
- `pthread_getattr_np` asks the host; without the host's, only the main
  thread's stack can be found.
- The first musl call on a thread allocates its TLS block. In a dlopened
  library the host may `malloc` for that, which is not async-signal-safe:
  a library signal handler that is the first musl use on a thread can
  deadlock if the signal interrupted the host's malloc.
- No `thread_local` destructors for the library's C++ objects on
  host-created threads.
- Unloading the library while its threads run is undefined, as for any
  library. Once they have been joined it is fine (a detached thread may
  still be in the library's code for a moment after it has signalled that
  it is done): the destructor drops musl's fork handlers, runs musl's
  atexit handlers and flushes musl's stdio.
- Do not unload the library while another thread may fork: glibc 2.36 and
  later run atfork handlers without holding a lock, so `dlclose` can unmap
  musl's handler while it runs.
- Statically linked programs, and dynamic linkers without ELF TLS for
  shared objects (Android before 10), cannot load the library.
- Sharing `errno`, locale, `FILE *`, TSD keys, `malloc`ed memory or
  `pthread_t` values between the two libcs is not supported.

## Files

- `embed/self.c`: the TLS variable, foreign-thread TCB initialisation.
- `embed/init.c`: constructor (auxv, multithreaded state), destructor,
  empty init/fini array bounds (as `ldso/dynlink.c` has for `libc.so`).
- `embed/hostsym.c`: host symbol lookup.
- `embed/environ.c`, `embed/pthread_create.c`, `embed/fork.c`,
  `embed/dl.c`, `embed/synccall.c`: replacements for upstream files.
- `src/internal/embed_impl.h`: internal interface; other changes are under
  `#ifdef MUSL_EMBED` in upstream files.
- `tools/musl-embed-check.sh`: run on `libc-embed.a` after every build;
  fails if anything touches the thread pointer, the robust list or
  `clear_child_tid`.
- `tools/musl-embed-link.sh`, `tools/musl-embed-check-so.sh`: installed as
  `musl-embed-link` and `musl-embed-check-so`.
