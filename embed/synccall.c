#include "pthread_impl.h"

/* Upstream __synccall runs func on every thread by signalling them with
 * an implementation-internal signal. In the embedded build those signal
 * numbers belong to the host, and the host owns most threads anyway, so
 * operations that need all threads are forwarded to the host instead
 * (see setxid.c, setgroups.c). This remains for completeness. */
void __synccall(void (*func)(void *), void *ctx)
{
	func(ctx);
}
