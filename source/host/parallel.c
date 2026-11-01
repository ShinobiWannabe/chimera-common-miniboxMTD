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
 * Linux and Windows alike: the threads, locks and the way into the guest are
 * the platform's (below), the rest is the same code, so the machine is the
 * same on both. A guest that keeps its own %fs (fs_swap) is refused with
 * -ENOSYS, and runs the loop itself: on Linux the fault handler would read
 * this file's thread locals through the guest's %fs. */
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
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef HANDLE mb_pthread;
typedef SRWLOCK mb_lock;
typedef CONDITION_VARIABLE mb_cond;
#define LOCK_STATIC SRWLOCK_INIT
#define COND_STATIC CONDITION_VARIABLE_INIT
#define LOCK_INIT(l) InitializeSRWLock(l)
#define COND_INIT(c) InitializeConditionVariable(c)
#define LOCK(l) AcquireSRWLockExclusive(l)
#define UNLOCK(l) ReleaseSRWLockExclusive(l)
#define WAIT(c, l) SleepConditionVariableSRW((c), (l), INFINITE, 0)
#define SIGNAL(c) WakeConditionVariable(c)
static long host_cores(void) { return (long)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS); }
static DWORD WINAPI worker_entry(LPVOID arg);
/* the worker's own host stack only holds worker_main's frame: tasks run on
 * the stacks mapped below */
static bool thread_start(mb_pthread *t, void *arg) {
	*t = CreateThread(NULL, 256 << 10, worker_entry, arg, 0, NULL);
	return *t != NULL;
}
#else
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
typedef pthread_t mb_pthread;
typedef pthread_mutex_t mb_lock;
typedef pthread_cond_t mb_cond;
#define LOCK_STATIC PTHREAD_MUTEX_INITIALIZER
#define COND_STATIC PTHREAD_COND_INITIALIZER
#define LOCK_INIT(l) pthread_mutex_init((l), NULL)
#define COND_INIT(c) pthread_cond_init((c), NULL)
#define LOCK(l) pthread_mutex_lock(l)
#define UNLOCK(l) pthread_mutex_unlock(l)
#define WAIT(c, l) pthread_cond_wait((c), (l))
#define SIGNAL(c) pthread_cond_signal(c)
static long host_cores(void) { return sysconf(_SC_NPROCESSORS_ONLN); }
static void *worker_entry(void *arg);
static bool thread_start(mb_pthread *t, void *arg) {
	return pthread_create(t, NULL, worker_entry, arg) == 0;
}
#endif

/* jumps into guarded.S with r10 = context, r11 = the guest function, and the
 * task's two arguments in rdi and rsi, exactly as an export thunk does */
uintptr_t mb_parallel_enter(mb_context *c, uintptr_t fn, uintptr_t arg, uintptr_t i);
#ifdef _WIN32
/* win64 brings the arguments in rcx, rdx, r8 and r9, and wants rdi and rsi
 * kept: they are saved here, before they carry the task's arguments (the
 * guarded call's own saves would see the arguments) */
__asm__(
	".text\n"
	".p2align 4\n"
	".globl mb_parallel_enter\n"
	".def mb_parallel_enter; .scl 2; .type 32; .endef\n"
	"mb_parallel_enter:\n"
	".seh_proc mb_parallel_enter\n"
	"	pushq %rdi\n"
	".seh_pushreg %rdi\n"
	"	pushq %rsi\n"
	".seh_pushreg %rsi\n"
	"	subq $8, %rsp\n"
	".seh_stackalloc 8\n"
	".seh_endprologue\n"
	"	movq %rcx, %r10\n"
	"	movq %rdx, %r11\n"
	"	movq %r8, %rdi\n"
	"	movq %r9, %rsi\n"
	"	call mb_guarded_call\n"
	"	addq $8, %rsp\n"
	"	popq %rsi\n"
	"	popq %rdi\n"
	"	ret\n"
	".seh_endproc\n");
#else
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
#endif

#define WORKER_STACK (2u << 20)

typedef struct {
	mb_pthread thread;
	mb_context ctx;
	mb_range stack;          /* host memory, with a guard page at the bottom */
	volatile int failed;
	/* where this worker sleeps: a call wakes only the workers it needs, so
	 * the rest of a large pool costs nothing */
	mb_lock lock;
	mb_cond go;
} worker;

static struct {
	mb_lock lock;
	mb_cond done_cv;
	int requested;           /* how many workers to use besides the caller */
	int started;             /* how many exist */
	worker w[MB_PARALLEL_MAX];
	worker caller;           /* the asking thread's own, for its share */
	bool caller_ready;
	/* the current call */
	volatile uint64_t generation;  /* (call number << 8) | workers taking part */
	uintptr_t fn, arg;
	uint32_t n;
	volatile uint32_t next;  /* the next task index to hand out */
	volatile int finished;   /* workers done with this generation */
	volatile bool running;
} P = { .lock = LOCK_STATIC, .done_cv = COND_STATIC, .requested = -1 };

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
	const int idx = (int)(me - P.w);
	uint64_t seen = 0;
	bool idle = false;       /* sat out the last call: sleep at once */
	for (;;) {
		/* Spin a little before sleeping: a renderer asks hundreds of times a
		 * frame, and a sleep and a wake cost more than a short task. A worker
		 * that was not needed last time goes straight to sleep, so that a
		 * pool larger than the work does not burn the cores the work needs. */
		int spins = idle ? 20000 : 0;
		while (__atomic_load_n(&P.generation, __ATOMIC_ACQUIRE) == seen && spins < 20000) {
			__builtin_ia32_pause();
			spins++;
		}
		if (__atomic_load_n(&P.generation, __ATOMIC_ACQUIRE) == seen) {
			LOCK(&me->lock);
			while (__atomic_load_n(&P.generation, __ATOMIC_ACQUIRE) == seen) WAIT(&me->go, &me->lock);
			UNLOCK(&me->lock);
		}
		/* the generation names the workers it needs, in one word: a worker
		 * that sat out an earlier call may wake late, and must not mistake
		 * the next call's count for its own */
		const uint64_t g = __atomic_load_n(&P.generation, __ATOMIC_ACQUIRE);
		const int active = (int)(g & 0xff);
		seen = g;
		idle = idx >= active;
		if (idle) continue;  /* not counted: the caller waits for the active ones only */
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
		if (__atomic_add_fetch(&P.finished, 1, __ATOMIC_ACQ_REL) == active) {
			LOCK(&P.lock);
			SIGNAL(&P.done_cv);
			UNLOCK(&P.lock);
		}
	}
	return NULL;
}

#ifdef _WIN32
static DWORD WINAPI worker_entry(LPVOID arg) { worker_main(arg); return 0; }
#else
static void *worker_entry(void *arg) { return worker_main(arg); }
#endif

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
		LOCK_INIT(&w->lock);
		COND_INIT(&w->go);
		/* a worker that is being created must not see a generation it
		 * should not run: it starts from the current one */
		if (!thread_start(&w->thread, w)) {
			mb_pal_unmap_anon(w->stack);
			return false;
		}
		P.started++;
	}
	return true;
}

mb_sword mb_parallel_for(mb_context *main, uintptr_t fn, uintptr_t arg, uintptr_t n) {
	int want = mb_parallel_workers();
	/* a worker more than the host has cores only waits for its turn while
	 * the others spin waiting for it (which thread runs a task never
	 * matters, so this is the same machine) */
	const long cores = host_cores();
	if (cores > 0 && want > cores - 1) want = (int)(cores - 1);
	if (main->fs_swap) return -ENOSYS;
	if (n == 0) return 0;
	if (n > 0xffffffffu) return -EINVAL;
	if (!P.caller_ready) {
		if (!map_stack(&P.caller)) return -ENOSYS;
		P.caller_ready = true;
	}
	/* the pool is made the first time, at the size asked for then; a later
	 * change of the count takes a new process */
	if (want > 0 && P.started == 0 && !start_workers(want)) return -ENOSYS;
	/* no more workers than there are tasks besides the caller's: the rest
	 * stay asleep (which thread runs a task never matters, so this is the
	 * same machine) */
	int active = P.started;
	if ((uintptr_t)active > n - 1) active = (int)(n - 1);
	for (int i = 0; i < active; i++) prepare(&P.w[i], main);
	prepare(&P.caller, main);
	P.fn = fn;
	P.arg = arg;
	P.n = (uint32_t)n;
	__atomic_store_n(&P.next, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&P.finished, 0, __ATOMIC_RELAXED);
	P.running = true;
	if (active > 0) {
		const uint64_t g = __atomic_load_n(&P.generation, __ATOMIC_RELAXED);
		__atomic_store_n(&P.generation, ((g >> 8) + 1) << 8 | (uint64_t)active, __ATOMIC_RELEASE);
		/* wake the ones taking part; the others may wake later on their own,
		 * see they are not needed, and sleep again */
		for (int i = 0; i < active; i++) {
			LOCK(&P.w[i].lock);
			SIGNAL(&P.w[i].go);
			UNLOCK(&P.w[i].lock);
		}
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
	while (__atomic_load_n(&P.finished, __ATOMIC_ACQUIRE) != active && spins < 200000) {
		__builtin_ia32_pause();
		spins++;
	}
	if (__atomic_load_n(&P.finished, __ATOMIC_ACQUIRE) != active) {
		LOCK(&P.lock);
		while (__atomic_load_n(&P.finished, __ATOMIC_ACQUIRE) != active)
			WAIT(&P.done_cv, &P.lock);
		UNLOCK(&P.lock);
	}
	P.running = false;
	if (P.caller.failed) {
		char why[512];
		snprintf(why, sizeof why, "%s", mb_host_death_text(main));
		mb_host_guest_death(main, "a task run in parallel died: %s", why);
	}
	for (int i = 0; i < active; i++)
		if (P.w[i].failed) {
			/* the worker's death said why; the machine's death says it again,
			 * since the machine's reason replaces the worker's */
			char why[512];
			snprintf(why, sizeof why, "%s", mb_host_death_text(main));
			mb_host_guest_death(main, "a task run in parallel died: %s", why);
		}
	return 0;
}
