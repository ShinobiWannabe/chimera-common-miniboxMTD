/* A guest that uses wbx_parallel_for the way a renderer would: many calls a
 * frame, each splitting work into disjoint slices of memory a savestate
 * carries. The runner (run_parallel.c) runs it with the feature off and with
 * 1, 2, 4 and 8 workers, and every machine must come out byte for byte the
 * same. It also checks the rules the host keeps: the guest's floating-point
 * control words reach the tasks, and a task that breaks a rule kills the
 * machine instead of the process. */
#include <emulibc.h>
#include <waterbox_parallel.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define W 640
#define H 480
#define SLICES 60   /* 8 lines each */

static uint32_t g_pic[H][W];           /* the "framebuffer": savestated */
static uint32_t g_frame;
static double g_rounding[SLICES];      /* written by tasks, one slot each */

typedef struct { uint32_t frame; uint32_t work; } job;

/* every pixel depends on what it was, the frame, and a little floating point:
 * a task that wrote outside its lines, or ran with the wrong rounding, would
 * change the picture */
static void draw(void *arg, uint32_t slice) {
	const job *j = (const job *)arg;
	const uint32_t y0 = slice * (H / SLICES), y1 = y0 + H / SLICES;
	for (uint32_t y = y0; y < y1; y++)
		for (uint32_t x = 0; x < W; x++) {
			uint32_t v = g_pic[y][x];
			for (uint32_t k = 0; k < j->work; k++) {
				v ^= v << 13; v ^= v >> 17; v ^= v << 5;
				v += j->frame * 2654435761u + x * 40503u + y;
			}
			double d = (double)v / 3.0;
			g_pic[y][x] = v ^ (uint32_t)(int64_t)(d * 1.0000001);
		}
}

static uint64_t checksum(void) {
	uint64_t h = 1469598103934665603ull;
	const uint8_t *p = (const uint8_t *)g_pic;
	for (size_t i = 0; i < sizeof g_pic; i++) { h ^= p[i]; h *= 1099511628211ull; }
	return h;
}

ECL_EXPORT int Init(void) {
	for (uint32_t y = 0; y < H; y++)
		for (uint32_t x = 0; x < W; x++) g_pic[y][x] = y * W + x;
	return 1;
}

/* frames x calls_per_frame parallel calls; returns the picture's checksum */
ECL_EXPORT uint64_t RunFrames(uint32_t frames, uint32_t calls, uint32_t work) {
	for (uint32_t f = 0; f < frames; f++, g_frame++)
		for (uint32_t c = 0; c < calls; c++) {
			job j = { g_frame * 1000 + c, work };
			wbx_parallel_for(draw, &j, SLICES);
		}
	return checksum();
}

/* The same frames with wbx_parallel_begin and wbx_parallel_join: while the
 * tasks draw, the guest's own thread works on memory of its own, and now and
 * then makes a system call, which joins the call early. The picture must be
 * RunFrames' picture, and the whole machine the same for every worker count. */
static uint32_t g_side[4096];

ECL_EXPORT uint64_t RunFramesAsync(uint32_t frames, uint32_t calls, uint32_t work) {
	static job j;   /* read by the tasks after begin returns: not on this stack */
	for (uint32_t f = 0; f < frames; f++, g_frame++)
		for (uint32_t c = 0; c < calls; c++) {
			j.frame = g_frame * 1000 + c;
			j.work = work;
			wbx_parallel_begin(draw, &j, SLICES);
			for (uint32_t k = 0; k < 4096; k++)
				g_side[k] = g_side[k] * 1103515245u + k + c;
			if (c % 7 == 3) (void)getpid();
			wbx_parallel_join();
		}
	uint64_t h = checksum();
	for (uint32_t k = 0; k < 4096; k++) h = (h ^ g_side[k]) * 1099511628211ull;
	return h;
}

ECL_EXPORT uint64_t PictureOnly(void) { return checksum(); }

/* 1.0/3.0 rounded to nearest and up differ in the last bit: each task records what it
 * computes under the rounding the guest set before the call */
static void rounding(void *arg, uint32_t i) {
	volatile double one = 1.0, three = 3.0 + (double)(i & 0);
	(void)arg;
	g_rounding[i] = one / three;
}

ECL_EXPORT uint64_t RunRounding(void) {
	uint32_t old, down;
	__asm__ volatile("stmxcsr %0" : "=m"(old));
	down = (old & ~0x6000u) | 0x4000u;   /* round toward plus infinity */
	__asm__ volatile("ldmxcsr %0" : : "m"(down));
	wbx_parallel_for(rounding, NULL, SLICES);
	__asm__ volatile("ldmxcsr %0" : : "m"(old));
	volatile double one = 1.0, three = 3.0;
	const double nearest = one / three;
	uint64_t differ = 0, same_as_first = 0;
	for (int i = 0; i < SLICES; i++) {
		if (g_rounding[i] != nearest) differ++;
		if (g_rounding[i] == g_rounding[0]) same_as_first++;
	}
	/* every task rounded up: all equal, all different from nearest */
	return differ == SLICES && same_as_first == SLICES ? 0 : 1 + differ;
}

/* a task making a system call (write) breaks the rules: the machine dies */
static void talk(void *arg, uint32_t i) {
	(void)arg;
	if (i == 7) write(2, "a task talks\n", 13);
}
ECL_EXPORT uint64_t RunSyscallInTask(void) {
	wbx_parallel_for(talk, NULL, SLICES);
	return 42;
}

/* a task faulting: the machine dies */
static void crash(void *arg, uint32_t i) {
	(void)arg;
	if (i == 3) { volatile uintptr_t bad = 16; *(volatile int *)bad = 1; }
}
ECL_EXPORT uint64_t RunCrashInTask(void) {
	wbx_parallel_for(crash, NULL, SLICES);
	return 42;
}
