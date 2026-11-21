import h5py
import numpy as np
import sys
import os

PAGE_SIZE = 4096  # bytes

# Usage: python read_mem_stats.py /path/to/zsim.h5
if len(sys.argv) > 1:
    H5_FILE = sys.argv[1]
else:
    H5_FILE = "zsim.h5"

if not os.path.exists(H5_FILE):
    raise FileNotFoundError(f"zsim.h5 not found at: {H5_FILE}")

with h5py.File(H5_FILE, "r") as f:
    stats = f["stats"]["root"]
    mem = stats["mem"]

    keys = [
        "accesses",
        "ddrAccesses",
        "cxlAccesses",
        "uniquePages",
        "fastMemPages",
        "slowMemPages",
    ]

    print(f"=== Reading memory stats from: {H5_FILE} ===\n")
    print("=== Memory stats (final / max) ===")
    for k in keys:
        data = mem[k][:]
        final = int(data[-1])
        maxv = int(np.max(data))
        print(f"{k:15s}: final = {final:8d}, max = {maxv:8d}")

    # Footprint calculation
    unique_pages = mem["uniquePages"][:]
    footprint_mb = (unique_pages * PAGE_SIZE) / (1024 * 1024)

    print("\n=== Footprint ===")
    print(f"Final footprint (MB): {footprint_mb[-1]:.2f}")
    print(f"Max footprint   (MB): {footprint_mb.max():.2f}")

