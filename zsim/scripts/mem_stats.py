import sys
import pathlib

file_path = sys.argv[1] if len(sys.argv) > 1 else "zsim.out"
if not pathlib.Path(file_path).is_file():
    sys.exit(f"File not found: {file_path}")

metrics = {}
with open(file_path) as f:
    for line in f:
        if ":" not in line:
            continue
        key, rest = line.split(":", 1)
        key = key.strip()
        value_str = rest.strip().split()[0]
        if key in {
            "accesses",
            "ddrAccesses",
            "cxlAccesses",
            "promotions",
            "demotions",
        }:
            try:
                metrics[key] = int(value_str)
            except ValueError:
                pass

ddr = metrics.get("ddrAccesses")
cxl = metrics.get("cxlAccesses")
if ddr is not None and cxl is not None:
    total = ddr + cxl
    hit_rate = ddr / total if total else 0
    print(f"DRAM hit rate: {hit_rate:.3f} ({ddr}/{total})")
    print(f"CXL fraction: {cxl / total:.3f} ({cxl}/{total})")
else:
    print("DRAM hit rate: N/A (missing counters)")

promotions = metrics.get("promotions", 0)
demotions = metrics.get("demotions", 0)
if promotions or demotions:
    print(f"Promotions: {promotions}")
    print(f"Demotions: {demotions}")
    total_mig = promotions + demotions
    accesses = metrics.get("accesses")
    if accesses:
        mig_rate = total_mig / accesses
        print(f"Migration rate: {mig_rate:.6f} ({total_mig}/{accesses})")
