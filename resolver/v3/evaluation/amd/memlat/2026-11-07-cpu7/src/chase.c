#define _GNU_SOURCE
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#define LINE 64
#define REPS 5

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t next(void) {
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return rng;
}

static double now(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

static long anon_huge_kb(void* base) {
	FILE* f = fopen("/proc/self/smaps", "r");
	char line[512];
	int in = 0;
	long kb = -1;
	char want[32];
	snprintf(want, sizeof want, "%lx-", (unsigned long)base);
	while (fgets(line, sizeof line, f)) {
		if (!strncmp(line, want, strlen(want)))
			in = 1;
		else if (in && !strncmp(line, "AnonHugePages:", 14)) {
			kb = atol(line + 14);
			break;
		}
	}
	fclose(f);
	return kb;
}

int main(int argc, char** argv) {
	if (argc != 4) {
		fprintf(stderr, "usage: %s <cpu> <min bytes> <max bytes>\n", argv[0]);
		return 2;
	}
	int cpu = atoi(argv[1]);
	size_t minb = strtoull(argv[2], 0, 10), maxb = strtoull(argv[3], 0, 10);
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof set, &set)) {
		perror("affinity");
		return 1;
	}
	if (prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0)) {
		perror("thp");
		return 1;
	}
	char* buf = mmap(0, maxb, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (buf == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	madvise(buf, maxb, MADV_NOHUGEPAGE);
	memset(buf, 1, maxb);
	long huge = anon_huge_kb(buf);

	size_t nsizes = 0, sizes[256];
	for (double s = minb; s <= maxb * 1.0001; s *= 1.189207115) {
		size_t b = ((size_t)s / LINE) * LINE;
		if (!nsizes || b != sizes[nsizes - 1])
			sizes[nsizes++] = b;
	}
	static double res[256][REPS];
	static size_t acc[256];
	size_t* order = malloc((maxb / LINE) * sizeof(size_t));
	volatile uintptr_t sink = 0;
	for (size_t k = 0; k < nsizes; ++k) {
		size_t n = sizes[k] / LINE;
		for (size_t i = 0; i < n; ++i)
			order[i] = i;
		for (size_t i = n - 1; i > 0; --i) {
			size_t j = next() % i;
			size_t t = order[i];
			order[i] = order[j];
			order[j] = t;
		}
		for (size_t i = 0; i < n; ++i)
			*(char**)(buf + order[i] * LINE) = buf + order[(i + 1) % n] * LINE;
		size_t steps = n * 4 > (1u << 24) ? n * 4 : (1u << 24);
		acc[k] = steps;
		char* p = buf;
		for (size_t i = 0; i < 2 * n + (1u << 20); ++i)
			p = *(char**)p;
		for (int r = 0; r < REPS; ++r) {
			double t0 = now();
			for (size_t i = 0; i < steps; ++i)
				p = *(char**)p;
			double t1 = now();
			res[k][r] = (t1 - t0) * 1e9 / steps;
		}
		sink += (uintptr_t)p;
	}
	printf("# cpu=%d running=%d line=%d reps=%d anon_huge_kb_after_touch=%ld sink=%lu\n", cpu, sched_getcpu(), LINE, REPS,
	       huge, (unsigned long)sink);
	printf("bytes,accesses_per_rep,rep1_ns,rep2_ns,rep3_ns,rep4_ns,rep5_ns,median_ns\n");
	for (size_t k = 0; k < nsizes; ++k) {
		double v[REPS];
		memcpy(v, res[k], sizeof v);
		for (int a = 0; a < REPS; ++a)
			for (int b = a + 1; b < REPS; ++b)
				if (v[b] < v[a]) {
					double t = v[a];
					v[a] = v[b];
					v[b] = t;
				}
		printf("%zu,%zu", sizes[k], acc[k]);
		for (int r = 0; r < REPS; ++r)
			printf(",%.3f", res[k][r]);
		printf(",%.3f\n", v[REPS / 2]);
	}
	printf("# anon_huge_kb_at_end=%ld\n", anon_huge_kb(buf));
	return 0;
}
