#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define MAXSIZES 256
#define CALLS_PER_SIZE 10
#define SMAPS_MAX (1 << 20)

struct walk {
	uint64_t visited;
	uint64_t max_off;
	uint64_t hash;
	int closed;
	int distinct;
};

static struct walk first[MAXSIZES], last[MAXSIZES];
static char* buf;
static size_t bytes;
static size_t wanted;
static uint64_t* bits;
static uint64_t calls;
static char smaps[SMAPS_MAX];
static size_t smaps_len;
static int (*real_gettime)(clockid_t, struct timespec*);
static void* (*real_mmap)(void*, size_t, int, int, int, off_t);

static void walk_chain(struct walk* w) {
	memset(bits, 0, bytes / 64 / 8 + 8);
	char* p = buf;
	w->visited = 0;
	w->max_off = 0;
	w->hash = 0xcbf29ce484222325ull;
	w->closed = 0;
	w->distinct = 1;
	for (;;) {
		uint64_t off = (uint64_t)(p - buf);
		if (off >= bytes || off % 64) {
			w->distinct = 0;
			return;
		}
		uint64_t line = off / 64;
		if ((bits[line / 64] >> (line % 64)) & 1) {
			w->distinct = 0;
			return;
		}
		bits[line / 64] |= 1ull << (line % 64);
		if (off > w->max_off)
			w->max_off = off;
		w->hash = (w->hash ^ off) * 0x100000001b3ull;
		w->visited++;
		p = *(char**)p;
		if (p == buf) {
			w->closed = 1;
			return;
		}
	}
}

static void capture_smaps(void) {
	int fd = open("/proc/self/smaps", O_RDONLY);
	if (fd < 0)
		return;
	ssize_t got;
	while (smaps_len < SMAPS_MAX && (got = read(fd, smaps + smaps_len, SMAPS_MAX - smaps_len)) > 0)
		smaps_len += (size_t)got;
	close(fd);
}

void* mmap(void* addr, size_t len, int prot, int flags, int fd, off_t off) {
	if (!real_mmap)
		real_mmap = dlsym(RTLD_NEXT, "mmap");
	void* r = real_mmap(addr, len, prot, flags, fd, off);
	if (!wanted) {
		const char* e = getenv("CHAINWALK_BYTES");
		wanted = e ? strtoull(e, 0, 10) : (size_t)-1;
	}
	if (!buf && len == wanted && (flags & MAP_ANONYMOUS) && r != MAP_FAILED) {
		buf = r;
		bytes = len;
		bits = real_mmap(0, bytes / 64 / 8 + 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	}
	return r;
}

int clock_gettime(clockid_t c, struct timespec* t) {
	if (!real_gettime)
		real_gettime = dlsym(RTLD_NEXT, "clock_gettime");
	if (buf && c == CLOCK_MONOTONIC) {
		uint64_t i = calls++;
		uint64_t k = i / CALLS_PER_SIZE;
		uint64_t r = i % CALLS_PER_SIZE;
		if (k < MAXSIZES && r == 0) {
			walk_chain(&first[k]);
			if (first[k].visited * 64 + 64 >= bytes)
				capture_smaps();
		}
		if (k < MAXSIZES && r == CALLS_PER_SIZE - 1)
			walk_chain(&last[k]);
	}
	return real_gettime(c, t);
}

__attribute__((destructor)) static void report(void) {
	const char* out = getenv("CHAINWALK_OUT");
	const char* sm = getenv("CHAINWALK_SMAPS");
	if (out) {
		FILE* f = fopen(out, "w");
		if (f) {
			fprintf(f, "# buf=%lu bytes=%zu monotonic_calls=%lu\n", (unsigned long)buf, bytes, (unsigned long)calls);
			fprintf(f, "size_index,visited,visited_bytes,max_off,closed,distinct,hash_first,hash_last,visited_last,closed_last,distinct_last\n");
			for (uint64_t k = 0; k < calls / CALLS_PER_SIZE && k < MAXSIZES; ++k)
				fprintf(f, "%lu,%lu,%lu,%lu,%d,%d,%016lx,%016lx,%lu,%d,%d\n", (unsigned long)k, (unsigned long)first[k].visited,
				        (unsigned long)(first[k].visited * 64), (unsigned long)first[k].max_off, first[k].closed,
				        first[k].distinct, (unsigned long)first[k].hash, (unsigned long)last[k].hash,
				        (unsigned long)last[k].visited, last[k].closed, last[k].distinct);
			fclose(f);
		}
	}
	if (sm && smaps_len) {
		FILE* f = fopen(sm, "w");
		if (f) {
			fwrite(smaps, 1, smaps_len, f);
			fclose(f);
		}
	}
}
