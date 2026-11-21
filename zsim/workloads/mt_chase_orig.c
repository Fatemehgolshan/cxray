// mt_chase.c  — multi-threaded pointer-chase for CXL/DDR L-B sweep.
//
// Each OMP thread owns an independent Hamiltonian cycle through
// TOTAL_MB / N_THREADS MiB of data (>= 32 MB total, >> L3).
// Self-referential: element i stores the index of the NEXT element
// to visit. Following the chain requires loading from DRAM on every
// step (true dependency, no MLP within one thread; but N threads
// together give N concurrent outstanding DRAM requests).
//
// Build: gcc -O2 -fopenmp -o /tmp/mt_chase /tmp/mt_chase.c
// Run:   OMP_NUM_THREADS=<N> /tmp/mt_chase

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <omp.h>

#define TOTAL_BYTES  (512ULL << 20)   // 512 MB total >> 28 MB L3
#define CL_SIZE      64ULL
#define ITERS        300000UL         // steps per thread (short enough to finish)

typedef uint64_t u64;

static void build_cycle(u64 *arr, u64 n, unsigned int seed) {
    // Fisher–Yates shuffle
    srand(seed);
    for (u64 i = 0; i < n; i++) arr[i] = i;
    for (u64 i = n-1; i > 0; i--) {
        u64 j = ((u64)rand() * 1103515245ULL ^ (u64)rand()) % (i+1);
        u64 t = arr[i]; arr[i] = arr[j]; arr[j] = t;
    }
    // Link: slot perm[i] holds perm[(i+1)%n]
    // Already in arr as a random permutation; re-interpret as next-pointer
    // Actually just make each arr[i] = arr[(i+1)%n] which is a random successor
    // Better: use the permutation to build a single cycle
    u64 *perm = (u64*)malloc(n * sizeof(u64));
    for (u64 i = 0; i < n; i++) perm[i] = arr[i];
    for (u64 i = 0; i < n; i++) arr[perm[i]] = perm[(i+1) % n];
    free(perm);
}

int main(void) {
    int nthr;
    #pragma omp parallel
    { if (omp_get_thread_num() == 0) nthr = omp_get_num_threads(); }

    u64 per_thr = (TOTAL_BYTES / CL_SIZE) / nthr;  // CL slots per thread
    if (per_thr == 0) per_thr = 1;

    // One buffer per thread; each CL stores a u64 next-index
    u64 **chains = (u64**)calloc(nthr, sizeof(u64*));
    for (int t = 0; t < nthr; t++) {
        chains[t] = (u64*)calloc(per_thr, CL_SIZE);   // lazy zero pages
        if (!chains[t]) { fputs("calloc\n", stderr); return 1; }
        u64 *idx_arr = (u64*)malloc(per_thr * sizeof(u64));
        build_cycle(idx_arr, per_thr, t * 1234567 + 42);
        // Write next-pointers into the CL-strided buffer
        for (u64 i = 0; i < per_thr; i++)
            chains[t][i * (CL_SIZE/sizeof(u64))] = idx_arr[i];
        free(idx_arr);
    }

    volatile u64 sink = 0;
    #pragma omp parallel reduction(+:sink)
    {
        int t = omp_get_thread_num();
        u64 *buf = chains[t];
        u64  pos = 0;
        u64  local = 0;
        for (u64 i = 0; i < ITERS; i++) {
            pos   = buf[pos * (CL_SIZE/sizeof(u64))];   // L3 miss, true dep
            local += pos;
        }
        sink += local;
    }
    printf("sink=%lu threads=%d per_thread=%.1fMiB\n",
           sink, nthr, per_thr * CL_SIZE / (1024.0*1024.0));

    for (int t = 0; t < nthr; t++) free(chains[t]);
    free(chains);
    return 0;
}
