# CXRay inside gem5

gem5 version: v24.1.0.3 (commit b9da2bfe1e21, also tagged v24.1).

The gem5 checkout is not modified. `stock-gem5.patch` is the empty output of
`git diff` in that tree, so `EXTRAS=` leaves gem5 untouched. Applying it is a
no-op:

    git -C <gem5> apply integrations/gem5/stock-gem5.patch

Build libcxray and gem5 with the same compiler:

    cmake -S <repo> -B build-cxray -DCMAKE_BUILD_TYPE=Release
    cmake --build build-cxray --target cxray_lib
    cd <gem5>
    scons EXTRAS=<repo>/integrations/gem5 build/X86/gem5.opt -j16

The SConscript links `libcxray_lib.a` from `<repo>/rebuttal/gem5/build-cxray`
(override with `CXRAY_BUILD`). Select the object with `--mem-type=CXRayMem`.
Stock `se.py` cannot set its parameters, so they come from the environment:
`CXRAY_CONFIG` (a `.cfg` with `output_dir` and `run_tag`) and
`CXRAY_ATOMIC_LATENCY_NS` (default 100).

Timing mode calls `cxl::CXLMemController::processRequest` and sends the
response after the reported latency. Atomic mode, used for fast-forward, uses
the fixed latency and does not call CXRay. SE mode only; the whole address
range is CXL memory.
