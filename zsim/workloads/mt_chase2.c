// mt_chase2.c — multi-threaded pointer-chase for CXL/DDR L-B sweep.
//
// Key design:
//   - Each OMP thread owns an independent Hamiltonian cycle through its slice
//     of memory using a coprime stride (no random shuffle needed).
//   - Initialization is O(N) sequential writes → fast, mostly L1/L2 hits.
//   - Chase is the intended DRAM workload: every step is an L3 miss (true dep).
//   - TOTAL_BYTES >> L3 ensures steady-state DRAM traffic.
//
// Build:   gcc -O2 -fopenmp -o /tmp/mt_chase2 /tmp/mt_chase2.c
// Run:     OMP_NUM_THREADS=<N> /tmp/mt_chase2

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <omp.h>

// Working set: keep large enough to spill well past the 28 MB L3.
// 256 MB total: at 4 threads →  64 MB/thread >> 28 MB/4=7 MB L3 share.
//               at 64 threads → 4 MB/thread  >> 28 MB/64=437 KB L3 share.
#define TOTAL_BYTES  (256ULL << 20)   // 256 MB total
#define CL_SIZE      64ULL            // cache line in bytes
#define PTRS_PER_CL  (CL_SIZE / sizeof(uint64_t))  // 8
#define ITERS        1000000UL        // 1 M pointer-chase steps per thread

// Any ODD number is coprime with per_thr when per_thr is a power of two.
// A large prime feels more "random" even if it isn't truly so.
#define STRIDE_ODD   6700417ULL

int main(void) {
    int nthr = 1;
#pragma omp parallel
    { if (omp_get_thread_num() == 0) nthr = omp_get_num_threads(); }

    uint64_t per_thr = (TOTAL_BYTES / CL_SIZE) / (uint64_t)nthr;
    if (per_thr < 2) per_thr = 2;

    // Allocate one cache-line-strided buffer per thread.
    uint64_t **bufs = (uint64_t **)calloc((size_t)nthr, sizeof(uint64_t *));
    if (!bufs) { fputs("calloc bufs\n", stderr); return 1; }

    for (int t = 0; t < nthr; t++) {
        bufs[t] = (uint64_t *)calloc((size_t)per_thr, CL_SIZE);
        if (!bufs[t]) { fputs("calloc buf\n", stderr); return 1; }
        // Build a single Hamiltonian cycle: slot i → slot (i+STRIDE_ODD)%per_thr.
        // STRIDE_ODD is odd → coprime with per_thr (power of 2) → visits every slot.
        for (uint64_t i = 0; i < per_thr; i++)
            bufs[t][i * PTRS_PER_CL] = (i + STRIDE_ODD) % per_thr;
    }

    volatile uint64_t sink = 0;
#pragma omp parallel reduction(+:sink)
    {
        int t = omp_get_thread_num();
        uint64_t *buf = bufs[t];
        uint64_t  pos = 0, local = 0;
        for (uint64_t i = 0; i < ITERS; i++) {
            pos   = buf[pos * PTRS_PER_CL];   // true dependency, L3 miss
            local += pos;
        }
        sink += local;
    }

    printf("sink=%lu threads=%d per_thread=%.1fMiB total=%.0fMiB\n",
           (unsigned long)sink, nthr,
           (double)per_thr * CL_SIZE / (1024.0 * 1024.0),
           (double)nthr * per_thr * CL_SIZE / (1024.0 * 1024.0));

    for (int t = 0; t < nthr; t++) free(bufs[t]);
    free(bufs);
    return 0;
}
