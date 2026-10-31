/* Run in parallel, then join (wbx_parallel_for, system call 2001).
 *
 * miniBox runs one guest thread at a time on one host thread, and that is what
 * makes a guest a pure function of its inputs. This file adds the one exception
 * that keeps that property: a guest may hand the host a function and a count,
 * and the host runs fn(arg, 0) .. fn(arg, n-1) on a pool of host threads, and
 * returns only when every one has finished. Nothing else in the guest runs
 * meanwhile: the thread that asked is parked in this system call.
 *
 * The result is the same for any number of workers, and the same as the guest
 * running the loop itself, AS LONG AS THE TASKS FOLLOW THE RULES (documented in
 * MACHINE-SPEC, "Parallel tasks", not enforced, like the rest of section 10):
 *   - no task writes memory another task reads or writes, in the same call;
 *   - no system call, no host callback, no allocation, no thread locals;
 *   - no floating-point control word changes.
 * A task that breaks the first rule makes the machine depend on the schedule,
 * and nothing here can see it. Comparing runs with 1 and N workers can.
 *
 * What the host does to keep everything else as it was:
 *   - each worker enters the guest through the same guarded call as an export
 *     (guarded.S), with its own context: its own stack, the guest's thread
 *     pointer, and the guest's floating-point control words as they were when
 *     the call was made;
 *   - the workers' stacks are host memory outside the machine, so they are in
 *     no state and their leftovers cannot differ between runs;
 *   - a write fault on a clean page is served by the same handler as the main
 *     thread's, under the block's tracking lock, which is already safe from
 *     several threads (it was built for the history's helper threads). The
 *     handler finds the faulting thread's context through mb_active_ctx();
 *   - a task that dies (a fault, a system call) ends its worker's share; after
 *     the join the machine dies as it would have on the main thread.
 *
 * The thread that asked takes tasks too, through a context of its own with a
 * host stack, like a worker's. With no workers (MB_PARALLEL_WORKERS unset or 0,
 * or wbx_set_parallel_workers(0)) it runs every task itself, in order. Either
 * way no task ever runs on the guest's own stack, so the machine's memory is
 * the same whatever the number of workers: a guest stack the tasks had run on
 * would hold their leftovers, and only in some configurations.
 *
 * Linux only for now; Windows answers -ENOSYS, and the guest then runs the
 * loop itself (a machine whose stack differs from Linux's, below the stack
 * pointer). A guest that keeps its own %fs (fs_swap) is refused the same way:
 * the fault handler would read this file's thread locals through the guest's
 * %fs. */
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "minibox_internal.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MB_PARALLEL_MAX 64

#ifdef _WIN32

int mb_parallel_workers(void) { return 0; }
void mb_parallel_set_workers(int n) { (void)n; }
mb_context *mb_parallel_ctx_of_slot(uintptr_t slot) { (void)slot; return NULL; }
bool mb_parallel_running(void) { return false; }
mb_sword mb_parallel_for(mb_context *main, uintptr_t fn, uintptr_t arg, uintptr_t n) {
	(void)main; (void)fn; (void)arg; (void)n;
	return -ENOSYS;
}

#else

#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

/* jumps into guarded.S with r10 = context, r11 = the guest function, and the
 * task's two arguments in rdi and rsi, exactly as an export thunk does */
uintptr_t mb_parallel_enter(mb_context *c, uintptr_t fn, uintptr_t arg, uintptr_t i);
__asm__(
	".text\n"
	".p2align 4\n"
	".globl mb_parallel_enter\n"
	".hidden mb_parallel_enter\n"
	".type mb_parallel_enter, @function\n"
	"mb_parallel_enter:\n"
	"	movq %rdi, %r10\n"
	"	movq %rsi, %r11\n"
	"	movq %rdx, %rdi\n"
	"	movq %rcx, %rsi\n"
	"	jmp mb_guarded_call\n"
	".size mb_parallel_enter, .-mb_parallel_enter\n");

#define WORKER_STACK (2u << 20)

typedef struct {
	pthread_t thread;
	mb_context ctx;
	mb_range stack;          /* host memory, with a guard page at the bottom */
	volatile int failed;
} worker;

static struct {
	pthread_mutex_t lock;
	pthread_cond_t go, done_cv;
	int requested;           /* how many workers to use besides the caller */
	int started;             /* how many exist */
	worker w[MB_PARALLEL_MAX];
	worker caller;           /* the asking thread's own, for its share */
	bool caller_ready;
	/* the current call */
	volatile uint64_t generation;
	uintptr_t fn, arg;
	uint32_t n;
	volatile uint32_t next;  /* the next task index to hand out */
	volatile int finished;   /* workers done with this generation */
	volatile bool running;
} P = { .lock = PTHREAD_MUTEX_INITIALIZER, .go = PTHREAD_COND_INITIALIZER, .done_cv = PTHREAD_COND_INITIALIZER, .requested = -1 };

/* the worker this thread is (host C only: guest code never runs with a %fs this
 * file could read through, since fs_swap guests are refused) */
static __thread mb_context *t_worker_ctx;

/* A task's system call: the rules forbid it, so the task dies, and so will the
 * machine after the join. (The blob has cleared the gs slot by now, so the
 * worker is found through this thread's own variable.) */
static uintptr_t MB_SYSV worker_syscall(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4,
                                        uintptr_t a5, uintptr_t a6, uintptr_t nr, void *host) {
	(void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)host;
	mb_context *c = t_worker_ctx;
	if (c == NULL) abort();
	mb_host_guest_death(c, "a task run in parallel made system call %llu, which parallel tasks may not",
	                    (unsigned long long)nr);
}

static void *worker_main(void *arg) {
	worker *me = (worker *)arg;
	mb_prepare_worker_thread();
	t_worker_ctx = &me->ctx;
	uint64_t seen = 0;
	for (;;) {
		/* Spin a little before sleeping: a renderer asks hundreds of times a
		 * frame, and a sleep and a wake cost more than a short task. */
		int spins = 0;
		while (__atomic_load_n(&P.generation, __ATOMIC_ACQUIRE) == seen && spins < 20000) {
			__builtin_ia32_pause();
			spins++;
		}
		if (__atomic_load_n(&P.generation, __ATOMIC_ACQUIRE) == seen) {
			pthread_mutex_lock(&P.lock);
			while (P.generation == seen) pthread_cond_wait(&P.go, &P.lock);
			pthread_mutex_unlock(&P.lock);
		}
		seen = __atomic_load_n(&P.generation, __ATOMIC_ACQUIRE);
		const uint32_t n = P.n;
		for (;;) {
			const uint32_t i = __atomic_fetch_add(&P.next, 1, __ATOMIC_ACQ_REL);
			if (i >= n) break;
			mb_parallel_enter(&me->ctx, P.fn, P.arg, i);
			if (me->ctx.dead) {
				me->failed = 1;
				/* the rest of the tasks are still run by the others; the
				 * machine dies after the join either way */
				break;
			}
		}
		if (__atomic_add_fetch(&P.finished, 1, __ATOMIC_ACQ_REL) == P.started) {
			pthread_mutex_lock(&P.lock);
			pthread_cond_signal(&P.done_cv);
			pthread_mutex_unlock(&P.lock);
		}
	}
	return NULL;
}

void mb_parallel_set_workers(int n) {
	if (n < 0) n = 0;
	if (n > MB_PARALLEL_MAX) n = MB_PARALLEL_MAX;
	P.requested = n;
}

int mb_parallel_workers(void) {
	if (P.requested < 0) {
		const char *e = getenv("MB_PARALLEL_WORKERS");
		int n = e ? atoi(e) : 0;
		mb_parallel_set_workers(n);
	}
	return P.requested;
}

bool mb_parallel_running(void) { return P.running; }

/* For the fault handler: the worker context a thread's gs slot names, or NULL. */
mb_context *mb_parallel_ctx_of_slot(uintptr_t slot) {
	if (slot == 0 || !P.running) return NULL;
	if (slot == (uintptr_t)&P.caller.ctx) return &P.caller.ctx;
	for (int i = 0; i < P.started; i++)
		if (slot == (uintptr_t)&P.w[i].ctx) return &P.w[i].ctx;
	return NULL;
}

static bool map_stack(worker *w) {
	mb_range in = { 0, WORKER_STACK }, got;
	if (mb_pal_map_anon(in, MB_PROT_RW, &got) != 0) return false;
	mb_range guard = { got.start, MB_PAGESIZE };
	mb_pal_protect(guard, MB_PROT_NONE);
	w->stack = got;
	return true;
}

static void prepare(worker *w, mb_context *main) {
	mb_context *c = &w->ctx;
	/* the stack top, 16-aligned, minus the slot a return address takes */
	mb_context_init(c, mb_range_end(w->stack) - 16, 0, worker_syscall);
	c->host_ptr = main->host_ptr;
	c->thread_area = main->thread_area;
	c->fs_swap = false;
	/* the guest's control words as they are now (the system call that
	 * brought us here kept them in the main context) */
	c->guest_fcw = main->guest_fcw;
	c->guest_mxcsr = main->guest_mxcsr;
	w->failed = 0;
}

static bool start_workers(int want) {
	while (P.started < want) {
		worker *w = &P.w[P.started];
		memset(w, 0, sizeof *w);
		if (!map_stack(w)) return false;
		/* a worker that is being created must not see a generation it
		 * should not run: it starts from the current one */
		if (pthread_create(&w->thread, NULL, worker_main, w) != 0) {
			mb_pal_unmap_anon(w->stack);
			return false;
		}
		P.started++;
	}
	return true;
}

mb_sword mb_parallel_for(mb_context *main, uintptr_t fn, uintptr_t arg, uintptr_t n) {
	const int want = mb_parallel_workers();
	if (main->fs_swap) return -ENOSYS;
	if (n == 0) return 0;
	if (n > 0xffffffffu) return -EINVAL;
	if (!P.caller_ready) {
		if (!map_stack(&P.caller)) return -ENOSYS;
		P.caller_ready = true;
	}
	/* the pool is made the first time, at the size asked for then; a later
	 * change of the count takes a new process (every worker takes part in
	 * every call, so the pool cannot shrink under a running machine) */
	if (want > 0 && P.started == 0 && !start_workers(want)) return -ENOSYS;
	for (int i = 0; i < P.started; i++) prepare(&P.w[i], main);
	prepare(&P.caller, main);
	P.fn = fn;
	P.arg = arg;
	P.n = (uint32_t)n;
	__atomic_store_n(&P.next, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&P.finished, 0, __ATOMIC_RELAXED);
	P.running = true;
	if (P.started > 0) {
		pthread_mutex_lock(&P.lock);
		__atomic_add_fetch(&P.generation, 1, __ATOMIC_ACQ_REL);
		pthread_cond_broadcast(&P.go);
		pthread_mutex_unlock(&P.lock);
	}
	/* the caller's share, through its own context and stack */
	t_worker_ctx = &P.caller.ctx;
	for (;;) {
		const uint32_t i = __atomic_fetch_add(&P.next, 1, __ATOMIC_ACQ_REL);
		if (i >= P.n) break;
		mb_parallel_enter(&P.caller.ctx, fn, arg, i);
		if (P.caller.ctx.dead) {
			P.caller.failed = 1;
			break;
		}
	}
	t_worker_ctx = NULL;
	/* wait: spin first, for the same reason the workers do */
	int spins = 0;
	while (__atomic_load_n(&P.finished, __ATOMIC_ACQUIRE) != P.started && spins < 200000) {
		__builtin_ia32_pause();
		spins++;
	}
	if (__atomic_load_n(&P.finished, __ATOMIC_ACQUIRE) != P.started) {
		pthread_mutex_lock(&P.lock);
		while (__atomic_load_n(&P.finished, __ATOMIC_ACQUIRE) != P.started)
			pthread_cond_wait(&P.done_cv, &P.lock);
		pthread_mutex_unlock(&P.lock);
	}
	P.running = false;
	if (P.caller.failed) {
		char why[512];
		snprintf(why, sizeof why, "%s", mb_host_death_text(main));
		mb_host_guest_death(main, "a task run in parallel died: %s", why);
	}
	for (int i = 0; i < P.started; i++)
		if (P.w[i].failed) {
			/* the worker's death said why; the machine's death says it again,
			 * since the machine's reason replaces the worker's */
			char why[512];
			snprintf(why, sizeof why, "%s", mb_host_death_text(main));
			mb_host_guest_death(main, "a task run in parallel died: %s", why);
		}
	return 0;
}

#endif
