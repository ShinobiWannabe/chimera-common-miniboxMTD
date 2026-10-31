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

#endif
