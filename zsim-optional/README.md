# zsim execution-driven tier (Table VI speed cross-check)

The zsim fork itself is vendored in this repo under `../zsim/` (GPLv2, see
`../zsim/LICENSE`). Its CXLMemory model is self-contained — no CXRay headers
needed — and the cross-check sweep uses `../zsim/tests/xeon_*.cfg` plus
`../zsim/workloads/mt_chase2.c`.

Figs. 4/7 are produced by `../scripts/run_cxray_multicore.py` (CXRay
closed-loop tier, which reproduces the paper); this zsim tier is the
independent execution-driven cross-check for the Table VI speed context.

What is NOT vendored is Intel Pin 2.14 (EULA + no direct download). Provide it:

1. Download `pin-2.14-71313-gcc.4.4.7-linux.tar.gz` from Intel's Pin page
   (accept the EULA; kit 71313, Linux gcc.4.4.7 build). Do not unpack it.
2. Native: unpack anywhere, then in `zsim/` run
   `PINPATH=<pin-dir> LIBCONFIGPATH=/usr HDF5PATH=<hdf5-root> scons -j8`
   (needs scons, libconfig++-dev, hdf5 dev libs, g++).
   Then: `python3 scripts/run_zsim_figs.py --out-csv outputs/csv --work artifact/work/zsim`.
3. Docker: copy the tarball into `../docker/` (gitignored) and build with
   `--build-arg WITH_ZSIM=1` (see `../docker/README_PIN.md`).

`scripts/run_zsim_figs.py` also accepts `--cores`, `--versions`, `--skip-build`
and `--dump-fields <zsim.h5>` (field discovery for the DDR p99 path).
