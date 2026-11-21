import itertools

ddr_size = 512
ddr_bw = 128
ddr_latency = 10000

capacity_ratios = [1, 2, 4, 8]
bw_ratios = [1, 0.5, 0.25, 0.125]
latency_ratios = [2, 4]

i = 0
for c, b, l in itertools.product(capacity_ratios, bw_ratios, latency_ratios):
    cap = ddr_size * c
    bw = ddr_bw * b
    lat = ddr_latency * l
    with open(f"cfgs/cfg_{i}.cfg", "w") as f:
        f.write(f"""
DDR = {{
    curveAddress     = "/path/to/home/repos/memtiering/zsim/memory_curve/ddr5/";
    curveWindowSize  = {ddr_latency};
    MaxTheoreticalBW = {ddr_bw};
    controllers      = 1;
    sizeMB           = {ddr_size};
}};

CXL = {{
    curveAddress     = "/path/to/home/repos/memtiering/zsim/memory_curve/CXL/";
    curveWindowSize  = {lat};
    MaxTheoreticalBW = {bw};
    controllers      = 1;
    sizeMB           = {cap};
}};
""")
    i += 1

