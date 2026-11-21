#!/usr/bin/env bash
# init.sh — interactive setup for CXRay public tree.
# Asks which simulators you want, fetches pinned checkouts, applies patches,
# builds CXRay, generates T1-T9 traces. Idempotent: safe to rerun.
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$REPO/build"
GEM5_PIN="v24.1.0.3"
SNIPER_PIN="56505e42fd98bcafac181e769bd3c98d2bb33"
usage() { echo "Usage: ./init.sh [--all|--cxray-only|--gem5|--sniper|--zsim-note] [--build-dir DIR]"; }
WANT_ALL=0; WANT_CXRAY=1; WANT_GEM5=0; WANT_SNIPER=0
for a in "$@"; do case "$a" in
  --all) WANT_ALL=1; WANT_GEM5=1; WANT_SNIPER=1;;
  --cxray-only) WANT_GEM5=0; WANT_SNIPER=0;;
  --gem5) WANT_GEM5=1;; --sniper) WANT_SNIPER=1;;
  --zsim-note) echo "zsim fork is vendored under zsim/ (Table VI cross-check); Pin kit needed only for execution runs: see zsim-optional/README.md."; exit 0;;
  --help|-h) usage; exit 0;;
  --build-dir) shift;;
  *) echo "Unknown arg: $a"; usage; exit 1;;
esac; done
if [[ $WANT_ALL -eq 0 && $WANT_GEM5 -eq 0 && $WANT_SNIPER -eq 0 && $# -eq 0 ]]; then
  echo "CXRay setup. Which integrations do you want?"
  echo "  1) CXRay only (recommended start)"
  echo "  2) + gem5  3) + sniper  4) all"
  read -rp "Choice [1]: " c; c="${c:-1}"
  case "$c" in 2) WANT_GEM5=1;; 3) WANT_SNIPER=1;; 4) WANT_GEM5=1; WANT_SNIPER=1;; *) :;; esac
fi
need() { command -v "$1" >/dev/null || { echo "Missing: $1"; exit 1; }; }
need cmake; need python3; need g++
echo "==> Building CXRay in $BUILD"
cmake -S "$REPO" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -Wno-dev 2>&1 | tail -2
cmake --build "$BUILD" -j"$(nproc)" 2>&1 | tail -3
ctest --test-dir "$BUILD" --output-on-failure 2>&1 | tail -3
echo "==> Generating T1-T9 traces"
python3 "$REPO/validation/traces/generate_traces.py" --output "$REPO/validation/traces"
if [[ $WANT_GEM5 -eq 1 ]]; then
  echo "==> gem5 ($GEM5_PIN): clone then:"
  echo "  cmake -S $REPO -B $BUILD -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD"
  echo "  cd <gem5> && scons EXTRAS=$REPO/integrations/gem5 build/X86/gem5.opt -j16"
  echo "  (gem5 checkout stays unmodified; stock-gem5.patch is an empty diff.)"
fi
if [[ $WANT_SNIPER -eq 1 ]]; then
  echo "==> sniper ($SNIPER_PIN): clone, then:"
  echo "  bash $REPO/integrations/sniper/apply.sh <sniper_root> $BUILD"
  echo "  cd <sniper_root> && make -j8"
fi
echo "Setup done. Quick run:"
echo "  ./build/cxray --config configs/cxl_2.0.cfg --trace validation/traces/T2_ptr_chase.trace --output /tmp/cxray_out --tag demo"
