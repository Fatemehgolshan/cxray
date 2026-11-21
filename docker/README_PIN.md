# Execution-driven tier (zsim + Pin) — outside the artifact's scope

Figs. 2–12 and the CXRay columns of Table VI need no Pin: they come from the
CXRay trace/closed-loop tiers, which is all `./reproduce.sh` (and this
image) runs.

The vendored `zsim/` fork and the gem5/Sniper integrations stay in the tree
for users who want execution-driven cross-checks
(`scripts/run_zsim_figs.py`, `scripts/bench_simspeed.py` without
`--skip-zsim`). Those paths need Intel Pin 2.14 (kit 71313, Linux
`gcc.4.4.7` build), which cannot be redistributed (Intel EULA) and cannot be
fetched non-interactively, so it is deliberately NOT part of the image.
To try it manually: accept the EULA at
https://www.intel.com/content/www/us/en/developer/articles/tool/pin-a-binary-instrumentation-tool-downloads.html,
unpack the kit, and point `PINPATH` at it before building `zsim/` with
`scons` (`LIBCONFIGPATH=/usr HDF5PATH=<serial-hdf5>`).
