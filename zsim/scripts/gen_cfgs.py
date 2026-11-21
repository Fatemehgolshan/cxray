import itertools
import os

# Create output directory if it doesn't exist
os.makedirs("cfgs", exist_ok=True)

# Fixed DDR baseline
ddr_capacity = 512       # in MB
ddr_bw = 128.0           # in GB/s
ddr_lat = 10000          # in cycles

# Define DDR:CXL ratios
capacity_ratios = [1, 2, 4, 8]     # DDR:CXL capacity
bw_ratios = [1, 0.5, 0.25, 0.125]  # DDR:CXL bandwidth
lat_ratios = [2, 4]                # DDR:CXL latency

i = 0
for cap_r, bw_r, lat_r in itertools.product(capacity_ratios, bw_ratios, lat_ratios):
    cxl_capacity = int(ddr_capacity * cap_r)
    cxl_bw = ddr_bw * bw_r
    cxl_lat = int(ddr_lat * lat_r)

    with open(f"cfgs/cfg_{i}.cfg", "w") as f:
        f.write(f"""
DDR = {{
    curveAddress     = "/path/to/home/repos/memtiering/zsim/memory_curve/ddr5/";
    curveWindowSize  = {ddr_lat};
    MaxTheoreticalBW = {ddr_bw};
    controllers      = 1;
    sizeMB           = {ddr_capacity};
}};

CXL = {{
    curveAddress     = "/path/to/home/repos/memtiering/zsim/memory_curve/CXL/";
    curveWindowSize  = {cxl_lat};
    MaxTheoreticalBW = {cxl_bw};
    controllers      = 1;
    sizeMB           = {cxl_capacity};
}};
""")
    i += 1

print(f"Generated {i} config files in cfgs/")

