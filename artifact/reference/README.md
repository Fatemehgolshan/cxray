# Golden figures

The PNGs in this directory are the golden outputs of the current tree: exact
copies of what `./reproduce.sh` (full run) writes to `outputs/`, produced
from the same `outputs/csv/` data inside the `cxray-ae` container image.
A container rerun reproduces these files byte-for-byte (only Table VI
host-speed numbers legitimately differ per machine). On a different host
or matplotlib version, compare curves and CSV values instead of bytes.

| Golden file | Paper figure | Source CSV |
|---|---|---|
| `fig02_calibration.png` | Fig. 2 (BW vs offered load) | `fig02_calibration.csv` + `real-hardware/bandwidth-latency.csv` |
| `fig03_versions.png` | Fig. 3 (T2 across versions) | `fig03_versions.csv` |
| `fig04_bw_concurrency.png` | Fig. 4 (BW vs cores, Little's law) | `fig04_bw_concurrency.csv` |
| `fig05_mlp.png` | Fig. 5 (MLP depth sweep) | `fig05_mlp.csv` |
| `fig06_t4.png` | Fig. 6 (T4 requester sweep) | `fig06_t4.csv` |
| `fig07_loaded.png` | Fig. 7 (loaded mean/p99) | `fig07_loaded.csv` |
| `fig08_decomp.png` | Fig. 8 (latency decomposition) | `fig08_decomp.csv` |
| `fig09_credit.png` | Fig. 9 (credit-pool cliff) | `fig09_credit.csv` |
| `fig10_gim.png` | Fig. 10 (GIM device count) | `fig10_gim.csv` |
| `fig11_type2.png` | Fig. 11 (Type 2 vs Type 3) | `fig11_type2.csv` |
| `fig12_multihost.png` | Fig. 12 (multihost sharing) | `fig12_multihost.csv` |
| `table6_simspeed.png` | Table VI (paper vs reproduced speeds) | `table6_simspeed.csv` |

Methodology for each figure is documented in the top-level README
("Reproduction methodology notes") and in `reproduce.sh` stage comments.
