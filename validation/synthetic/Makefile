CC      = gcc
CFLAGS  = -O2 -std=c11 -Wall
OUTDIR  = build

all: $(OUTDIR)/trace_replay

$(OUTDIR)/trace_replay: trace_replay.c | $(OUTDIR)
	$(CC) $(CFLAGS) -o $@ $<

$(OUTDIR):
	mkdir -p $(OUTDIR)

clean:
	rm -f $(OUTDIR)/trace_replay

.PHONY: all clean
