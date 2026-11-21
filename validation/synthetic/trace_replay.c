/*
 * trace_replay.c — Synthetic workload that replays CXRay address traces.
 *
 * CXLMemSim wraps a running binary and intercepts memory accesses via PEBS.
 * This program maps a large memory region to CXL address space and then
 * replays the exact read/write sequences from a trace file, causing real
 * hardware cache misses that CXLMemSim can detect and measure.
 *
 * Usage: ./trace_replay <trace_file> [--repeat N]
 *   trace_file: CXRay .trace format: "tick op 0xaddr size req_id"
 *
 * Memory layout: mmap a 16GB region. Addresses from trace are masked to fit
 * within this region so the accesses stay in the mapped area.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <errno.h>

#define MEM_SIZE  (4ULL * 1024 * 1024 * 1024)   /* 4 GB mapped region */
#define CACHE_LINE 64
#define MAX_ADDRS  (1 << 20)   /* up to 1M trace entries before wrapping */

static volatile char *mem_base = NULL;

/* Force a cache miss on a read */
static inline uint64_t do_read(uint64_t addr) {
    uint64_t masked = addr & (MEM_SIZE - CACHE_LINE);
    masked = masked & ~(CACHE_LINE - 1);  /* align to cache line */
    volatile uint64_t *p = (volatile uint64_t *)(mem_base + masked);
    return *p;
}

/* Force a cache miss on a write */
static inline void do_write(uint64_t addr, uint64_t val) {
    uint64_t masked = addr & (MEM_SIZE - CACHE_LINE);
    masked = masked & ~(CACHE_LINE - 1);
    volatile uint64_t *p = (volatile uint64_t *)(mem_base + masked);
    *p = val;
}

/* Evict cache by reading a large stride region */
static void evict_cache(void) {
    static uint64_t sum = 0;
    const size_t EVICT_SIZE = 32 * 1024 * 1024;  /* 32MB stride to flush L3 */
    for (size_t i = 0; i < EVICT_SIZE; i += 64) {
        sum += mem_base[i];
    }
    (void)sum;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <trace_file> [repeat_count]\n", argv[0]);
        return 1;
    }
    const char *trace_file = argv[1];
    int repeats = (argc >= 3) ? atoi(argv[2]) : 1;

    /* Map the large region. In a real CXL setup this would be CXL memory.
     * For CXLMemSim it intercepts these accesses via PEBS and injects latency. */
    mem_base = (char *)mmap(NULL, MEM_SIZE,
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (mem_base == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    /* Touch pages to fault them in (avoid page-fault noise in measurements) */
    fprintf(stderr, "Faulting in %zu GB...\n", MEM_SIZE >> 30);
    for (size_t i = 0; i < MEM_SIZE; i += 4096) {
        mem_base[i] = (char)i;
    }

    /* Load trace */
    FILE *f = fopen(trace_file, "r");
    if (!f) {
        perror(trace_file);
        return 1;
    }

    uint64_t *addrs = malloc(MAX_ADDRS * sizeof(uint64_t));
    int *ops = malloc(MAX_ADDRS * sizeof(int));  /* 0=read 1=write */
    if (!addrs || !ops) { perror("malloc"); return 1; }

    int n = 0;
    char line[256];
    while (fgets(line, sizeof(line), f) && n < MAX_ADDRS) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char op_str[16];
        uint64_t addr;
        /* Format: tick op 0xaddr size req_id */
        if (sscanf(line, "%*s %15s %lx", op_str, &addr) == 2) {
            addrs[n] = addr;
            ops[n] = (op_str[0] == 'W' || op_str[0] == 'w') ? 1 : 0;
            n++;
        }
    }
    fclose(f);
    fprintf(stderr, "Loaded %d trace entries\n", n);

    if (n == 0) {
        fprintf(stderr, "No trace entries found\n");
        return 1;
    }

    /* Replay: evict cache between iterations so accesses go to "CXL memory" */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t sum = 0;

    for (int r = 0; r < repeats; r++) {
        evict_cache();
        for (int i = 0; i < n; i++) {
            if (ops[i] == 0) {
                sum += do_read(addrs[i]);
            } else {
                do_write(addrs[i], (uint64_t)i);
            }
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    long elapsed_ns = (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
    long total_accesses = (long)n * repeats;
    double avg_ns = (double)elapsed_ns / total_accesses;

    fprintf(stderr, "Sum=%lu (prevent opt)\n", sum);
    printf("trace_replay results:\n");
    printf("  trace_file:      %s\n", trace_file);
    printf("  accesses:        %ld\n", total_accesses);
    printf("  elapsed_ns:      %ld\n", elapsed_ns);
    printf("  avg_latency_ns:  %.1f\n", avg_ns);
    printf("  throughput_Mops: %.2f\n", total_accesses / (elapsed_ns / 1e9) / 1e6);

    munmap((void *)mem_base, MEM_SIZE);
    free(addrs);
    free(ops);
    return 0;
}
