/*
 * trace_replay_gem5.c — Trace replay binary for gem5-CXL SE mode.
 *
 * gem5-CXL maps the CXL device at 0x200000000. We mmap to that exact address
 * with MAP_FIXED so all memory accesses go through the CXL controller/device.
 * Local DDR3 is at 0x80000000 (used for the program stack/heap).
 *
 * Usage: ./trace_replay_gem5 <trace_file>
 *
 * In gem5:
 *   build/X86/gem5.opt configs/deprecated/example/se_two_hierachy.py \
 *     --cxl \
 *     -c trace_replay_gem5 \
 *     --options="T2_ptr_chase.trace"
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <errno.h>

/* CXL device address as configured in gem5-CXL se_two_hierachy.py */
#define CXL_BASE_ADDR  0x200000000ULL
#define CXL_SIZE       (1024ULL * 1024 * 1024)  /* 1 GB CXL device */
#define CACHE_LINE     64
#define MAX_ADDRS      (1 << 20)

static volatile char *cxl_mem = NULL;

static inline uint64_t do_read(uint64_t addr) {
    uint64_t offset = addr & (CXL_SIZE - CACHE_LINE);
    offset &= ~(CACHE_LINE - 1);
    volatile uint64_t *p = (volatile uint64_t *)(cxl_mem + offset);
    return *p;
}

static inline void do_write(uint64_t addr, uint64_t val) {
    uint64_t offset = addr & (CXL_SIZE - CACHE_LINE);
    offset &= ~(CACHE_LINE - 1);
    volatile uint64_t *p = (volatile uint64_t *)(cxl_mem + offset);
    *p = val;
}

/* Simple cache eviction: sequential stride through CXL memory */
static void evict_cache(void) {
    static volatile uint64_t sink = 0;
    const size_t EVICT_SIZE = 32 * 1024 * 1024;
    for (size_t i = 0; i < EVICT_SIZE && i < CXL_SIZE; i += 64) {
        sink += cxl_mem[i];
    }
    (void)sink;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <trace_file>\n", argv[0]);
        return 1;
    }

    /* Map to the CXL address range in gem5 */
    cxl_mem = (char *)mmap((void *)CXL_BASE_ADDR, CXL_SIZE,
                           PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                           -1, 0);
    if (cxl_mem == MAP_FAILED) {
        /* Fallback: let OS choose — still exercises memory subsystem */
        cxl_mem = (char *)mmap(NULL, CXL_SIZE,
                               PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (cxl_mem == MAP_FAILED) {
            perror("mmap");
            return 1;
        }
        fprintf(stderr, "Note: using OS-assigned address (not fixed CXL range)\n");
    }

    /* Touch pages */
    for (size_t i = 0; i < CXL_SIZE; i += 4096) {
        cxl_mem[i] = 0;
    }

    /* Load trace */
    FILE *f = fopen(argv[1], "r");
    if (!f) { perror(argv[1]); return 1; }

    uint64_t *addrs = malloc(MAX_ADDRS * sizeof(uint64_t));
    int *ops = malloc(MAX_ADDRS * sizeof(int));
    if (!addrs || !ops) { perror("malloc"); return 1; }

    int n = 0;
    char line[256];
    while (fgets(line, sizeof(line), f) && n < MAX_ADDRS) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char op_str[16];
        uint64_t addr;
        if (sscanf(line, "%*s %15s %lx", op_str, &addr) == 2) {
            addrs[n] = addr;
            ops[n] = (op_str[0] == 'W' || op_str[0] == 'w') ? 1 : 0;
            n++;
        }
    }
    fclose(f);

    if (n == 0) {
        fprintf(stderr, "No trace entries\n");
        return 1;
    }

    /* Replay */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    evict_cache();
    uint64_t sum = 0;
    for (int i = 0; i < n; i++) {
        if (ops[i] == 0) {
            sum += do_read(addrs[i]);
        } else {
            do_write(addrs[i], (uint64_t)i);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    long elapsed_ns = (t1.tv_sec - t0.tv_sec) * 1000000000L +
                      (t1.tv_nsec - t0.tv_nsec);

    /* Output for parser */
    fprintf(stderr, "prevent_opt=%lu\n", sum);
    printf("gem5_cxl results:\n");
    printf("  trace_entries:   %d\n", n);
    printf("  elapsed_ns:      %ld\n", elapsed_ns);
    printf("  avg_latency_ns:  %.1f\n", (double)elapsed_ns / n);

    munmap((void *)cxl_mem, CXL_SIZE);
    free(addrs);
    free(ops);
    return 0;
}
