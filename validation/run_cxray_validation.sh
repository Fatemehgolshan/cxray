#!/bin/bash
# Run CXRay on all T1-T9 traces with CXL 2.0 and CXL 3.1 configs.
# Deposits normalized results in validation/results/cxray/

set -e
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CXRAY="$REPO_ROOT/src/cxray.py"
TRACES="$REPO_ROOT/validation/traces"
CONFIGS="$REPO_ROOT/configs"
RESULTS="$REPO_ROOT/validation/results"
PARSERS="$REPO_ROOT/validation/parsers"

mkdir -p "$RESULTS/cxray"

run_trace() {
    local trace_file="$1"
    local config_file="$2"
    local trace_id="$3"
    local config_id="$4"
    local out_dir="$RESULTS/cxray_raw/${trace_id}_${config_id}"

    echo "  Running: $trace_id + $config_id"
    python3 "$CXRAY" \
        --config "$config_file" \
        --trace "$trace_file" \
        --output "$out_dir" 2>&1 | tail -5

    python3 "$PARSERS/cxray_parser.py" \
        --input "$out_dir" \
        --trace "$trace_id" \
        --config "$config_id" \
        --output "$RESULTS/cxray/${trace_id}_${config_id}.json"
}

echo "=== CXRay Validation ==="
echo ""

echo "[CXL 2.0 Config]"
for trace in T1_seq_read T2_ptr_chase T6_mixed_rw T7_scrub_baseline T8_random_ppr; do
    run_trace "$TRACES/${trace}.trace" "$CONFIGS/cxl_2.0.cfg" "$trace" "cxl_2.0"
done
for depth in 1 2 4 8 16 32 64; do
    run_trace "$TRACES/T3_rand_depth${depth}.trace" "$CONFIGS/cxl_2.0.cfg" "T3_depth${depth}" "cxl_2.0"
done
for req in 1 2 4 8 16; do
    run_trace "$TRACES/T4_seq_req${req}.trace" "$CONFIGS/cxl_2.0.cfg" "T4_req${req}" "cxl_2.0"
done

echo ""
echo "[CXL 3.1 Config]"
for trace in T1_seq_read T2_ptr_chase T7_scrub_baseline T8_random_ppr; do
    run_trace "$TRACES/${trace}.trace" "$CONFIGS/cxl_3.1.cfg" "$trace" "cxl_3.1"
done

echo ""
echo "=== Results written to $RESULTS/cxray/ ==="
echo ""
echo "To compare vs golden:"
echo "  cd $REPO_ROOT/validation && python3 compare.py --results results/ --golden golden/paper_data.json --output reports/"
