# CXRay as a Sniper DRAM model

`DramPerfModelCXRay` answers every DRAM access with the CXRay library
(`cxl::CXLMemController::processRequest`) and returns that latency to Sniper. One
process-wide controller sits behind a mutex, because Sniper calls the model from several
host threads; arrivals can be out of order by up to one barrier quantum (100 ns).

Built against Sniper commit `56505e42fd98bca863fac181e769bd3c98d2bb33`
(`snipersim/snipersim` main, CHANGELOG "Version 8.1").

`stock-sniper.patch` is the complete diff against that commit: the two model sources, the
`cxray` case in `DramPerfModel::createDramPerfModel`, the CXRay link line, and the one-line
stock fix (`m_tag_directory_present`) that shared caches need.

    cd <sniper> && git checkout 56505e42fd98bca863fac181e769bd3c98d2bb33
    patch -p1 < <repo>/integrations/sniper/stock-sniper.patch

Build CXRay with Sniper's ABI, then link it and rebuild Sniper with the same compiler:

    cmake -S <repo> -B <build> -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="-D_GLIBCXX_USE_CXX11_ABI=0"
    cmake --build <build> --target cxray_lib -j4
    ln -sfn <repo>/src <sniper>/cxray/src
    ln -sfn <build>/libcxray_lib.a <sniper>/cxray/libcxray_lib.a
    cd <sniper> && make -j8

`apply.sh <sniper> <build>` does the same integration from the separate source files
(`sniper.patch`, `sniper-fixes.patch`) instead of from `stock-sniper.patch`.

Run: `perf_model/dram/type=cxray` and `perf_model/dram/cxray_config=<cxl cfg>`.
