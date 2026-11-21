CC      = gcc
CFLAGS  = -O2 -fopenmp
BINDIR  = /tmp

# mt_chase2: coprime-stride init (fast), 256 MB working set, 1 M iters/thread.
# Used for Xeon Gold 6248 multi-core L-B sweep (xeon_cxl_c*.cfg, xeon_ddr_c*.cfg).
mt_chase2: mt_chase2.c
	$(CC) $(CFLAGS) -o $(BINDIR)/mt_chase2 $<

# mt_chase_orig: Fisher-Yates shuffle init, 512 MB working set, 300 K iters/thread.
# Reference version; init is slower (dominates sim time at high core counts).
mt_chase_orig: mt_chase_orig.c
	$(CC) $(CFLAGS) -o $(BINDIR)/mt_chase_orig $<

all: mt_chase2 mt_chase_orig

.PHONY: all
