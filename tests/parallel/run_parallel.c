#define _GNU_SOURCE
/* Drives guest_parallel.wbx: the same machine with parallel calls off and with
 * 1, 2, 4 and 8 workers. Each count runs in a process of its own (the worker
 * pool is made once per process), started from this one:
 *
 *   run_parallel <guest.wbx>                 the test: runs every count, compares
 *   run_parallel <guest.wbx> --child N       one count; prints what it saw
 *
 * Compared: the picture's checksum after 30 frames of 40 parallel calls each,
 * a hash of the whole savestate, and that the tasks saw the guest's rounding
 * mode. Then, with 4 workers, a task that makes a system call and a task that
 * faults must each kill the machine and leave the process running. */
#include "minibox.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { FILE *f; } fr_t;
static intptr_t file_read(uintptr_t ud, uint8_t *d, uintptr_t s) { return (intptr_t)fread(d, 1, s, ((fr_t *)ud)->f); }
typedef struct { uint64_t h; uint64_t len; } hash_t;
static int32_t hash_write(uintptr_t ud, const uint8_t *d, uintptr_t n) {
	hash_t *x = (hash_t *)ud;
	for (uintptr_t i = 0; i < n; i++) { x->h ^= d[i]; x->h *= 1099511628211ull; }
	x->len += n;
	return 0;
}

typedef int (MB_GUEST_ABI *init_fn)(void);
typedef uint64_t (MB_GUEST_ABI *run0_fn)(void);
typedef uint64_t (MB_GUEST_ABI *run3_fn)(uint64_t, uint64_t, uint64_t);

static uintptr_t proc(mb_host *h, const char *n) {
	mb_return r;
	wbx_get_proc_addr(h, n, &r);
	if (r.error_message[0]) { fprintf(stderr, "proc %s: %s\n", n, r.error_message); exit(2); }
	return r.data;
}

static mb_host *load(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
	mb_memory_layout_template layout = { 64u << 20, 16u << 20, 16u << 20, 16u << 20, 64u << 20 };
	fr_t fr = { f };
	mb_return r;
	wbx_create_host(&layout, "guest_parallel.wbx", file_read, (uintptr_t)&fr, &r);
	fclose(f);
	if (r.error_message[0]) { fprintf(stderr, "create: %s\n", r.error_message); exit(2); }
	mb_host *h = (mb_host *)r.data;
	wbx_activate_host(h, &r);
	if (((init_fn)proc(h, "Init"))() != 1) { fprintf(stderr, "Init failed\n"); exit(2); }
	wbx_deactivate_host(h, &r);
	wbx_seal(h, &r);
	if (r.error_message[0]) { fprintf(stderr, "seal: %s\n", r.error_message); exit(2); }
	wbx_activate_host(h, &r);
	return h;
}

static double now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static int child(const char *path, int workers) {
	mb_return r;
	wbx_set_parallel_workers(workers, &r);
	mb_host *h = load(path);
	const uint64_t rounding = ((run0_fn)proc(h, "RunRounding"))();
	const double t0 = now_ms();
	const uint64_t pic = ((run3_fn)proc(h, "RunFrames"))(30, 40, 3);
	const double t1 = now_ms();
	hash_t st = { 1469598103934665603ull, 0 };
	wbx_save_state(h, hash_write, (uintptr_t)&st, &r);
	if (r.error_message[0]) { fprintf(stderr, "save: %s\n", r.error_message); return 2; }
	/* the heavy run, for the speed only: few calls, much work each */
	const double t2 = now_ms();
	((run3_fn)proc(h, "RunFrames"))(4, 4, 200);
	const double t3 = now_ms();
	printf("RESULT %d rounding=%llu picture=%016llx state=%016llx/%llu ms_many_calls=%.1f ms_heavy=%.1f\n",
	       workers, (unsigned long long)rounding, (unsigned long long)pic,
	       (unsigned long long)st.h, (unsigned long long)st.len, t1 - t0, t3 - t2);

	wbx_deactivate_host(h, &r);
	wbx_destroy_host(h, &r);   /* one machine at a time in a process: the death tests make their own */
	if (workers >= 2) {
		/* the rules: a task's system call and a task's fault each end the
		 * machine, not the process */
		const char *which[2] = { "RunSyscallInTask", "RunCrashInTask" };
		for (int k = 0; k < 2; k++) {
			mb_host *d = load(path);
			const uint64_t ret = ((run0_fn)proc(d, which[k]))();
			char death[600] = "";
			wbx_get_death(d, death, sizeof death, &r);
			printf("DEATH %s ret=%llu died=%d why=%s\n", which[k], (unsigned long long)ret,
			       death[0] != '\0', death);
			wbx_deactivate_host(d, &r);
			wbx_destroy_host(d, &r);
		}
	}
	return 0;
}

int main(int argc, char **argv) {
	const char *path = argc > 1 ? argv[1] : "guest_parallel.wbx";
	if (argc > 3 && strcmp(argv[2], "--child") == 0) return child(path, atoi(argv[3]));

	const int counts[] = { 0, 1, 2, 4, 8 };
	char first[200] = "";
	int fails = 0;
	for (size_t i = 0; i < sizeof counts / sizeof counts[0]; i++) {
		char cmd[4096];
		snprintf(cmd, sizeof cmd, "'%s' '%s' --child %d", argv[0], path, counts[i]);
		FILE *p = popen(cmd, "r");
		if (!p) { fprintf(stderr, "cannot run %s\n", cmd); return 1; }
		char line[1024];
		int saw_result = 0, deaths = 0;
		while (fgets(line, sizeof line, p)) {
			fputs(line, stdout);
			if (strncmp(line, "RESULT ", 7) == 0) {
				saw_result = 1;
				unsigned long long rounding = 1;
				char pic[17] = "", st[64] = "";
				int w;
				if (sscanf(line, "RESULT %d rounding=%llu picture=%16s state=%63[^ ]", &w, &rounding, pic, st) != 4) {
					printf("  FAIL: unreadable result\n"); fails++; continue;
				}
				if (rounding != 0) { printf("  FAIL: %d workers: the tasks did not see the guest's rounding mode\n", w); fails++; }
				char key[200];
				snprintf(key, sizeof key, "%s %s", pic, st);
				if (first[0] == '\0') snprintf(first, sizeof first, "%s", key);
				else if (strcmp(first, key) != 0) {
					printf("  FAIL: %d workers made a different machine (%s, expected %s)\n", w, key, first);
					fails++;
				}
			}
			if (strncmp(line, "DEATH ", 6) == 0) {
				deaths++;
				if (!strstr(line, "ret=0 ") || !strstr(line, "died=1")) {
					printf("  FAIL: a task that broke a rule did not end the machine\n");
					fails++;
				}
			}
		}
		const int status = pclose(p);
		if (status != 0 || !saw_result) { printf("  FAIL: %d workers: the run did not finish (status %d)\n", counts[i], status); fails++; }
		if (counts[i] >= 2 && deaths != 2) { printf("  FAIL: %d workers: %d of 2 rule tests ran\n", counts[i], deaths); fails++; }
	}
	printf(fails ? "parallel: %d FAILED\n" : "parallel: every worker count made the same machine\n", fails);
	return fails ? 1 : 0;
}
