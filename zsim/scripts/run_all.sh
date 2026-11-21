#!/bin/bash

CFG_DIR="cfgs"
RESULT_DIR="results"
mkdir -p "$RESULT_DIR"

DDR_CAP=512
DDR_BW=128.0
DDR_LAT=10000

BFS_CFG="tests/bfs.cfg"
TEMPLATE_CFG="tests/bfs.template.cfg"

# Backup the original template
cp "$BFS_CFG" "$TEMPLATE_CFG"

shopt -s nullglob

for cfg in $CFG_DIR/*.cfg; do
    echo "Running with $cfg..."

    # Extract DDR and CXL blocks from config
    DDR_BLOCK=$(awk '/DDR = {/,/};/' "$cfg")
    CXL_BLOCK=$(awk '/CXL = {/,/};/' "$cfg")

    # Replace DDR and CXL blocks in bfs.cfg
    awk -v ddr="$DDR_BLOCK" -v cxl="$CXL_BLOCK" '
        BEGIN {in_ddr=0; in_cxl=0}
        /DDR = {/ {print ddr; in_ddr=1; next}
        /CXL = {/ {print cxl; in_cxl=1; next}
        in_ddr && /};/ {in_ddr=0; next}
        in_cxl && /};/ {in_cxl=0; next}
        !in_ddr && !in_cxl {print}
    ' "$TEMPLATE_CFG" > "$BFS_CFG"

    # Extract values for naming
    CXL_CAP=$(awk '/CXL/,/}/' "$cfg" | grep "sizeMB" | awk '{print $3}' | tr -d ';')
    CXL_BW=$(awk '/CXL/,/}/' "$cfg" | grep "MaxTheoreticalBW" | awk '{print $3}' | tr -d ';')
    CXL_LAT=$(awk '/CXL/,/}/' "$cfg" | grep "curveWindowSize" | awk '{print $3}' | tr -d ';')

    OUTFILE="dram_${DDR_CAP}_cxl_${CXL_CAP}_dram_bw_${DDR_BW}_cxl_bw_${CXL_BW}_dram_lat_${DDR_LAT}_cxl_lat_${CXL_LAT}.log"

    # Run ZSim
    ./build/opt/zsim "$BFS_CFG"

    # Save output
    cp zsim.out "$RESULT_DIR/$OUTFILE"
    echo "Saved to $RESULT_DIR/$OUTFILE"
done

# Restore original config
mv "$TEMPLATE_CFG" "$BFS_CFG"

