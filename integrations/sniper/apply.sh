#!/bin/bash
# apply.sh — put the CXRay DRAM model into a Sniper checkout.  Idempotent.
#
#   bash apply.sh <sniper_root> <cxray_build_dir> [<cxray_src_dir>]
#
#   <sniper_root>      Sniper 8.x checkout (stock, or already patched by this script)
#   <cxray_build_dir>  CMake build dir of CXRay that contains libcxray_lib.a
#                      (build it with the same compiler and -D_GLIBCXX_USE_CXX11_ABI=0 as Sniper)
#   <cxray_src_dir>    CXRay src/ (default: <this repo>/src)
#
# What it does:
#   1. copies dram_perf_model_cxray.{h,cc} into <sniper_root>/common/performance_model/
#      (Sniper's Makefile picks up every .cc in common/ automatically)
#   2. applies sniper.patch (adds the "cxray" case to DramPerfModel::createDramPerfModel and the
#      CXRay include path + static library to common/Makefile.common), skipping it if it is already in
#   2b. applies sniper-fixes.patch (one-line stock-Sniper fix: uninitialised m_tag_directory_present
#      aborts every shared-LLC config); kept apart from the integration and not counted in its line count
#   3. creates <sniper_root>/cxray/{src,libcxray_lib.a} as symlinks to the CXRay sources and library
#      (the path common/Makefile.common expects via CXRAY_HOME; untracked in Sniper's git)
# Afterwards rebuild Sniper:  cd <sniper_root> && make -j8
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SNIPER=${1:?usage: apply.sh <sniper_root> <cxray_build_dir> [<cxray_src_dir>]}
CXRAY_BUILD=${2:?usage: apply.sh <sniper_root> <cxray_build_dir> [<cxray_src_dir>]}
CXRAY_SRC=${3:-$(cd "$HERE/../.." && pwd)/src}

SNIPER=$(cd "$SNIPER" && pwd)
CXRAY_BUILD=$(cd "$CXRAY_BUILD" && pwd)
CXRAY_SRC=$(cd "$CXRAY_SRC" && pwd)

[ -f "$SNIPER/common/performance_model/dram_perf_model.cc" ] || { echo "not a Sniper checkout: $SNIPER" >&2; exit 1; }
[ -f "$CXRAY_BUILD/libcxray_lib.a" ] || { echo "libcxray_lib.a not found in $CXRAY_BUILD" >&2; exit 1; }
[ -f "$CXRAY_SRC/cxl_mem.h" ] || { echo "cxl_mem.h not found in $CXRAY_SRC" >&2; exit 1; }

# 1. model sources
install -m 0644 "$HERE/dram_perf_model_cxray.h" "$HERE/dram_perf_model_cxray.cc" "$SNIPER/common/performance_model/"
echo "[apply] copied dram_perf_model_cxray.{h,cc} -> $SNIPER/common/performance_model/"

# 2. patch (skip when already applied)
if grep -q 'DramPerfModelCXRay' "$SNIPER/common/performance_model/dram_perf_model.cc" && grep -q 'CXRAY_HOME' "$SNIPER/common/Makefile.common"; then
   echo "[apply] sniper.patch already applied"
else
   (cd "$SNIPER" && patch -p1 --forward < "$HERE/sniper.patch")
   echo "[apply] sniper.patch applied"
fi

# 2b. stock-Sniper bug fix needed for any shared last-level cache (separate file, not integration code)
if grep -q 'm_tag_directory_present(false)' "$SNIPER/common/core/memory_subsystem/parametric_dram_directory_msi/memory_manager.cc"; then
   echo "[apply] sniper-fixes.patch already applied"
else
   (cd "$SNIPER" && patch -p1 --forward < "$HERE/sniper-fixes.patch")
   echo "[apply] sniper-fixes.patch applied (uninitialised m_tag_directory_present in MemoryManager)"
fi

# 3. CXRAY_HOME layout
mkdir -p "$SNIPER/cxray"
ln -sfn "$CXRAY_SRC" "$SNIPER/cxray/src"
ln -sfn "$CXRAY_BUILD/libcxray_lib.a" "$SNIPER/cxray/libcxray_lib.a"
echo "[apply] $SNIPER/cxray/src -> $CXRAY_SRC"
echo "[apply] $SNIPER/cxray/libcxray_lib.a -> $CXRAY_BUILD/libcxray_lib.a"
echo "[apply] done; rebuild with: cd $SNIPER && make -j8"
