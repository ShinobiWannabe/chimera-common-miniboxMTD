/* waterbox_parallel.h - miniBox guest kit: run in parallel, then join.
 *
 *     wbx_parallel_for(fn, arg, n);
 *
 * calls fn(arg, i) once for every i from 0 to n-1 and returns when all of them
 * have returned. The host runs them (MACHINE-SPEC, "Parallel tasks"), on its
 * worker threads and the calling one; a host without the call answers no and
 * the loop runs right here, in order. The
 * machine comes out the same either way, PROVIDED the tasks keep the rules:
 *
 *   - a task writes only memory no other task of the same call reads or writes;
 *   - a task makes no system call: no malloc, no printf, no file, no lock,
 *     no thread, nothing from libc that might reach the host;
 *   - a task uses no thread locals (errno included) and calls no host callback;
 *   - a task does not change the floating-point control words.
 *
 * Breaking one of these is not caught when the loop runs here, and makes the
 * machine depend on the host's schedule when it does not. Run the core with 1
 * worker and with several and compare: that is the test.
 *
 * C and C++, header-only. */
#ifndef WATERBOX_PARALLEL_H
#define WATERBOX_PARALLEL_H

#include <errno.h>
#include <stdint.h>
#include <unistd.h>

#define WBX_NR_PARALLEL_FOR 2001

typedef void (*wbx_parallel_task)(void *arg, uint32_t index);

static inline void wbx_parallel_for(wbx_parallel_task fn, void *arg, uint32_t n) {
	if (n == 0) return;
	/* errno is part of the machine: a refusal (the feature off) must leave
	 * it as it found it, or "off" and "on" are two different machines */
	const int saved = errno;
	/* even one task goes to the host: it runs tasks on stacks of its own,
	 * so whether a task ran leaves nothing on the guest's stack */
	const long r = syscall(WBX_NR_PARALLEL_FOR, fn, arg, (unsigned long)n);
	errno = saved;
	if (r == 0) return;
	for (uint32_t i = 0; i < n; i++) fn(arg, i);
}


/* Begin, then join later:
 *
 *     wbx_parallel_begin(fn, arg, n);
 *     ... the guest goes on ...
 *     wbx_parallel_join();
 *
 * runs the same tasks as wbx_parallel_for, with the same rules, but returns
 * at once: the host's workers run them while the guest's own thread goes on,
 * and wbx_parallel_join returns when all have finished (the calling thread
 * takes any left over). One begun call at a time; a second begin, a
 * wbx_parallel_for, or ANY other system call joins the one running first.
 * Between the two the guest must not read or write memory the tasks write,
 * nor write memory they read: that is the guest's to keep, and the same test
 * finds a mistake (1 worker against several). With no workers every task runs
 * inside begin; a host without the calls answers no and the loop runs right
 * here, in order. */
#define WBX_NR_PARALLEL_BEGIN 2002
#define WBX_NR_PARALLEL_JOIN 2003

static inline void wbx_parallel_begin(wbx_parallel_task fn, void *arg, uint32_t n) {
	if (n == 0) return;
	const int saved = errno;
	const long r = syscall(WBX_NR_PARALLEL_BEGIN, fn, arg, (unsigned long)n);
	errno = saved;
	if (r == 0) return;
	for (uint32_t i = 0; i < n; i++) fn(arg, i);
}

static inline void wbx_parallel_join(void) {
	const int saved = errno;
	(void)syscall(WBX_NR_PARALLEL_JOIN);
	errno = saved;
}

#endif
