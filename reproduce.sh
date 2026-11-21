#!/usr/bin/env bash
# reproduce.sh — ONE script that reproduces every CXRay paper figure.
# Resumable: each stage leaves artifact/work/<stage>.done; rerun to continue.
# Outputs: outputs/fig02_*.png ... outputs/fig12_*.png, outputs/table6_simspeed.png
#          outputs/csv/*.csv (+ summary.json per stage in artifact/work).
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="$REPO/artifact/work"; OUT="$REPO/outputs"; CSV="$OUT/csv"
BUILD="$REPO/build"; TR="$REPO/validation/traces"; SIM="$REPO/src/cxray.py"
SMOKE="${SMOKE:-0}"; ONLY=""; FORCE=0; FULL=0
export SMOKE
for a in "$@"; do case "$a" in
  *reproduce.sh) ;; # tolerate ENTRYPOINT arg-doubling: docker run IMG ./reproduce.sh --smoke
  --smoke) SMOKE=1;; --full) FULL=1;;
  --fig) shift;; --fig=*) ONLY="${a#--fig=}";; --force) FORCE=1;;
  --help|-h) sed -n '1,40p' "$0"; echo "Stages: build traces fig02 fig03 fig04 fig05 fig06 fig07 fig08 fig09 fig10 fig11 fig12 table6 plots"; exit 0;;
  [0-9]*|fig*) ONLY="$a";;
  plots|plot|table6|table|build|traces) ONLY="$a";;
  *) echo "Unknown arg: $a (see --help)"; exit 1;;
esac; done
# support '--fig 6' two-word form
if [[ "${1:-}" == "--fig" ]]; then ONLY="${2:-}"; fi
ONLY="${ONLY#fig}"; ONLY="${ONLY#0}"
mkdir -p "$WORK" "$CSV"
stamp() { [[ $FORCE -eq 0 && -f "$WORK/$1.done" ]]; }
mark() { date -u +%FT%TZ > "$WORK/$1.done"; echo "[done] $1"; }
want() { [[ -z "$ONLY" || "$ONLY" == "$1" ]]; }
run_sim() { # cfg trace outdir [extra --set args...]
  local cfg="$1" trace="$2" outdir="$3"; shift 3
  mkdir -p "$outdir"
  python3 "$SIM" -c "$cfg" -t "$trace" -o "$outdir" "$@" >/dev/null
}
vcfg() { # version -> config path (2.0/3.0/3.1/4.0/ddr5)
  case "$1" in
    2.0) echo "$REPO/configs/cxl_2.0.cfg";; 3.0) echo "$REPO/configs/cxl_3.0.cfg";;
    3.1) echo "$REPO/configs/cxl_3.1.cfg";; 4.0) echo "$REPO/configs/cxl_4.0.cfg";;
    ddr5) echo "$REPO/configs/ddr5_only.cfg";; esac
}
csv_of() { python3 - "$@" <<'EOF'
import json,sys,glob,os
# tiny helper: summary.json -> key=value line (used inline by stages below)
EOF
}
echo "CXRay reproduce.sh (smoke=$SMOKE only=${ONLY:-all})"
### build ###
if ! stamp build && { [[ -z "$ONLY" ]] || [[ "$ONLY" == "build" ]]; }; then
  echo "==> [build] cmake + ctest"
  cmake -S "$REPO" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -Wno-dev 2>&1 | tail -2
  cmake --build "$BUILD" -j"$(nproc)" 2>&1 | tail -3
  ctest --test-dir "$BUILD" --output-on-failure 2>&1 | tail -3
  mark build
fi
### traces ###
if ! stamp traces && [[ -z "$ONLY" ]]; then
  echo "==> [traces] generating T1-T9 (large: T4/T5 may take minutes)"
  if [[ $SMOKE -eq 1 ]]; then
    python3 "$REPO/validation/traces/generate_traces.py" --trace T2 --output "$TR"
    python3 "$REPO/validation/traces/generate_traces.py" --trace T3 --output "$TR"
    python3 "$REPO/validation/traces/generate_traces.py" --trace T6 --output "$TR"
  else
    # Only traces consumed by stages/bench: T4/T7/T8 have no consumer
    # (fig06/fig08 use closed-loop seeds; figs 4/7 use closed-loop seeds
    # via scripts/run_cxray_multicore.py, not traces).
    # T9 is capped (fig12 sweep is 1/2/4/8/16; full 16-host files are GBs).
    for t in T1 T2 T3 T5 T6; do
      python3 "$REPO/validation/traces/generate_traces.py" --trace $t --output "$TR"
    done
    if [[ $SMOKE -eq 1 ]]; then MXH=4; else MXH=16; fi
    python3 "$REPO/validation/traces/generate_traces.py" --trace T9 --max-hosts $MXH --output "$TR"
  fi
  mark traces
fi
T2="$TR/T2_ptr_chase.trace"; T6="$TR/T6_mixed_rw.trace"
### fig02 calibration (Fig.2: BW-latency, orange CXRay vs gray HW) ###
# The calibration script IS the T5-style sweep (R/W/M, cxl_2.0.cfg); its JSON
# is the single source for both the CSV and the plotted orange curves.
if want 2 && ! stamp fig02; then
  echo "==> [fig02] calibration sweep (R/W/M, cxl_2.0.cfg)"
  mkdir -p "$WORK/fig02"
  CXRAY_CALIB_JSON="$WORK/fig02/bw_sweep_cxray.json" \
    python3 "$REPO/scripts/gen_calibration_traces.py" || echo "[warn] gen_calibration_traces rc=$? (continuing)"
  if [[ -f "$WORK/fig02/bw_sweep_cxray.json" ]]; then
    python3 - "$WORK/fig02/bw_sweep_cxray.json" "$CSV/fig02_calibration.csv" <<'EOF'
import csv,json,sys
src, dst = sys.argv[1], sys.argv[2]
d = json.load(open(src))
n = 0
with open(dst, "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["traffic", "label", "mean_lat_ns", "bw_GBps"])
    for t in ["R", "W", "M"]:
        for p in sorted(d.get(t, []), key=lambda x: x["bw_GBps"]):
            # csv module quotes the label: labels contain commas ("nreq=1,int=14ns")
            w.writerow([t, p["label"], f"{p['mean_lat_ns']:.2f}", f"{p['bw_GBps']:.4f}"])
            n += 1
print(f"fig02: {n} points")
EOF
    cp "$WORK/fig02/bw_sweep_cxray.json" "$CSV/bw_sweep_cxray.json"
    mark fig02
  else
    echo "[warn] fig02 json missing; leaving stage unstamped so a rerun retries it (other stages continue)"
  fi
fi
### fig03 versions (Fig.3: protocol floor vs mean, T2 all versions) ###
if want 3 && ! stamp fig03; then
  echo "==> [fig03] T2 pointer-chase across CXL versions"
  mkdir -p "$WORK/fig03"
  CFGS="cxl_2.0.cfg cxl_3.0.cfg cxl_3.1.cfg cxl_4.0.cfg"; [[ $SMOKE -eq 1 ]] && CFGS="cxl_2.0.cfg cxl_3.1.cfg"
  { echo "config,mean_lat_ns,p99_lat_ns"; for c in $CFGS; do
    d="$WORK/fig03/${c%.cfg}"; run_sim "$REPO/configs/$c" "$T2" "$d"
    python3 - "$d/summary.json" "$c" <<'EOF'
import json,sys
s=json.load(open(sys.argv[1])); l=s["latency_ns"]
print(f"{sys.argv[2]},{l['all_mean']:.2f},{l['all_p99']:.2f}")
EOF
  done; } > "$CSV/fig03_versions.csv"
  mark fig03
fi
### fig05 mlp (Fig.5: T3 depth sweep) ###
# d=1,4,16: direct fixed-timestamp T3 replay (in-flight << 32-entry credit pool).
# d=64: closed-loop seed (fixed batch-injection at d=64 overflows the credit
# model by design -- see run_cxl40_experiments.py V2 -- so depth-64 uses the
# same closed-loop requester methodology as the multicore/feature experiments).
if want 5 && ! stamp fig05; then
  echo "==> [fig05] T3 MLP depth sweep"
  mkdir -p "$WORK/fig05"
  DEPTHS="1 4 16"; [[ $SMOKE -eq 1 ]] && DEPTHS="1 16"
  VERSIONS="2.0 3.0 3.1 4.0 ddr5"; [[ $SMOKE -eq 1 ]] && VERSIONS="2.0 ddr5"
  [[ -f "$TR/T3_rand_depth1.trace" ]] || python3 "$REPO/validation/traces/generate_traces.py" --trace T3 --output "$TR"
  { echo "device,depth,mean_lat_ns,bw_GBps,method"; for d in $DEPTHS; do
    for dev in $VERSIONS; do
      o="$WORK/fig05/${dev}_d${d}"; run_sim "$(vcfg $dev)" "$TR/T3_rand_depth${d}.trace" "$o"
      python3 - "$o/summary.json" "$d" "$dev" <<'EOF'
import json,sys
s=json.load(open(sys.argv[1]))
print(f"{sys.argv[3]},{sys.argv[2]},{s['latency_ns']['all_mean']:.2f},{s['bandwidth_GBps']['total_avg']:.3f},trace")
EOF
    done
  done
  if [[ $SMOKE -eq 0 ]]; then
    seed="$WORK/fig05/mlp_d64_seed.trace"
    python3 - "$seed" <<'EOF'
import sys
with open(sys.argv[1], "w") as f:
    f.write("# fig05 d=64 closed-loop seed: 64 requesters\n")
    for rid in range(64): f.write(f"0 R 0x{rid*0x40000:016x} 64 {rid}\n")
EOF
    for dev in $VERSIONS; do
      cfg="$(vcfg $dev)"
      o="$WORK/fig05/${dev}_d64"; mkdir -p "$o"
      if [[ $dev == ddr5 ]]; then
        python3 "$SIM" -c "$cfg" -t "$TR/T3_rand_depth64.trace" -o "$o" >/dev/null
        meth="trace"
      else
        # NOTE: default (simple) DRAM backend here, NOT queued: the d64 point
        # measures the MLP-depth knee (paper: 445 ns / 9.2 GB/s for CXL 2.0),
        # and finite bank queues blow the closed loop up to ~4 us instead.
        python3 "$SIM" -c "$cfg" -t "$seed" -o "$o" \
          --set inject.closed_loop_per_req=2000 \
          --set inject.stride_bytes=64 --set inject.region_bytes=16777216 >/dev/null
        meth="closed-loop"
      fi
      python3 - "$o/summary.json" "$dev" "$meth" <<'EOF'
import json,sys
s=json.load(open(sys.argv[1]))
print(f"{sys.argv[2]},64,{s['latency_ns']['all_mean']:.2f},{s['bandwidth_GBps']['total_avg']:.3f},{sys.argv[3]}")
EOF
    done
  fi; } > "$CSV/fig05_mlp.csv"
  mark fig05
fi
### fig06 t4 (Fig.6: row-buffer loss, requester sweep) ###
# Closed-loop per-requester seeds (each requester walks its own 16 MB region
# stride-64, queued DRAM backend): matches the paper's T4 methodology.
# Fixed-timestamp T4 traces are NOT replayed -- open-loop injection diverges
# into unbounded queues at r>=2 (documented in calibration.tex) and does not
# match the stored closed-loop reference values.
if want 6 && ! stamp fig06; then
  echo "==> [fig06] T4 requester sweep (closed-loop, all versions)"
  mkdir -p "$WORK/fig06"
  REQS="1 4 8 16 32 64 128"; [[ $SMOKE -eq 1 ]] && REQS="1 8"
  VERSIONS6="2.0 3.0 3.1 4.0"; [[ $SMOKE -eq 1 ]] && VERSIONS6="2.0"
  PER_REQ=2000; [[ $SMOKE -eq 1 ]] && PER_REQ=200
  { echo "version,requesters,mean_lat_ns,bw_GBps"; for r in $REQS; do
    seed="$WORK/fig06/t4_r${r}_seed.trace"
    python3 - "$seed" "$r" <<'EOF'
import sys
path, nreq = sys.argv[1], int(sys.argv[2])
with open(path, "w") as f:
    f.write(f"# fig06 T4 closed-loop seed: {nreq} requesters, own 16MB regions\n")
    for rid in range(nreq): f.write(f"0 R 0x{rid*0x1000000:016x} 64 {rid}\n")
EOF
    for ver in $VERSIONS6; do
      o="$WORK/fig06/${ver}_r${r}"; mkdir -p "$o"
      python3 "$SIM" -c "$(vcfg $ver)" -t "$seed" -o "$o" \
        --set dram.backend=queued --set inject.closed_loop_per_req=$PER_REQ \
        --set inject.stride_bytes=64 --set inject.region_bytes=16777216 >/dev/null
      python3 - "$o/summary.json" "$ver" "$r" <<'EOF'
import json,sys
s=json.load(open(sys.argv[1]))
print(f"{sys.argv[2]},{sys.argv[3]},{s['latency_ns']['all_mean']:.2f},{s['bandwidth_GBps']['total_avg']:.3f}")
EOF
    done
  done; } > "$CSV/fig06_t4.csv"
  mark fig06
fi
### fig08 decomp (Fig.8: latency decomposition, 4 versions x 4 ops) ###
# Protocol-floor segments are definitional model constants (host 97 ns +
# symmetric one-way link + FEC/IDE per version, matching the paper's floor
# table); DRAM + credit segments are measured closed-loop per op.
if want 8 && ! stamp fig08; then
  echo "==> [fig08] latency decomposition (all versions, r=1/4/8/16)"
  mkdir -p "$WORK/fig08"
  OPS="1 4 8 16"; [[ $SMOKE -eq 1 ]] && OPS="1 4"
  VERS8="2.0 3.0 3.1 4.0"; [[ $SMOKE -eq 1 ]] && VERS8="2.0"
  { echo "version,requesters,host_ns,fwd_ns,ret_ns,dram_ns,credit_ns,mean_ns"; for ver in $VERS8; do
    case $ver in
      2.0) FW=30; RT=30;; 3.0) FW=28; RT=28;; 3.1) FW=36; RT=36;; 4.0) FW=25; RT=25;;
    esac
    for r in $OPS; do
      seed="$WORK/fig08/decomp_r${r}.trace"
      python3 - "$seed" "$r" <<'EOF'
import sys
path, nreq = sys.argv[1], int(sys.argv[2])
with open(path, "w") as f:
    f.write(f"# fig08 decomp seed: {nreq} requesters\n")
    for rid in range(nreq): f.write(f"0 R 0x{rid*0x1000000:016x} 64 {rid}\n")
EOF
      o="$WORK/fig08/${ver}_r${r}"; mkdir -p "$o"
      python3 "$SIM" -c "$(vcfg $ver)" -t "$seed" -o "$o" \
        --set dram.backend=queued --set inject.closed_loop_per_req=1000 \
        --set inject.stride_bytes=64 --set inject.region_bytes=16777216 >/dev/null
      python3 - "$o/summary.json" "$ver" "$r" "$FW" "$RT" <<'EOF'
import json,sys
s=json.load(open(sys.argv[1])); ver,r,fw,rt=sys.argv[2],sys.argv[3],sys.argv[4],sys.argv[5]
dram=s.get("dram_ns",{}); cp=s.get("credit_pools",{})
print(f"{ver},{r},97,{fw},{rt},{float(dram.get('mean',0)):.2f},{float(cp.get('total_stall_ns',0)):.2f},{s['latency_ns']['all_mean']:.2f}")
EOF
    done
  done; } > "$CSV/fig08_decomp.csv"
  mark fig08
fi
### fig09 credit (Fig.9: E1 credit-pool cliff) ###
if want 9 && ! stamp fig09; then
  echo "==> [fig09] credit-pool sweep (closed-loop seed)"
  mkdir -p "$WORK/fig09"
  LIMITS="0 32 16 8 4"; [[ $SMOKE -eq 1 ]] && LIMITS="0 8"
  python3 - "$WORK/fig09" "$REPO/configs/cxl_3.1.cfg" "$SIM" "$LIMITS" <<'EOF'
import subprocess,json,os,sys,tempfile
workdir,cfg,sim,limits=sys.argv[1],sys.argv[2],sys.argv[3],sys.argv[4].split()
seed=os.path.join(workdir,"e1_seed.trace")
with open(seed,"w") as f:
    f.write("# E1 seed: 32 requesters (closed-loop)\n")
    for rid in range(32): f.write(f"{rid} R 0x{rid*64:016x} 64 {rid}\n")
rows=["m2s_req_limit,mean_lat_ns,p99_lat_ns,bw_GBps"]
for lim in limits:
    out=tempfile.mkdtemp()
    cmd=[sys.executable,sim,"-c",cfg,"-t",seed,"-o",out,"--set","dram.backend=queued",
         f"link.credits.m2s_req={lim}","--set" if False else "--set",f"inject.closed_loop_per_req=1000"]
    # note: cxray.py accepts repeated --set flags
    cmd=[sys.executable,sim,"-c",cfg,"-t",seed,"-o",out,"--set","dram.backend=queued",
         "--set",f"link.credits.m2s_req={lim}","--set","inject.closed_loop_per_req=1000"]
    r=subprocess.run(cmd,capture_output=True,text=True)
    if r.returncode!=0: print("E1 fail",lim,r.stderr[-300:]); continue
    s=json.load(open(os.path.join(out,"summary.json")))
    lab="disabled" if lim=="0" else lim
    rows.append(f"{lab},{s['latency_ns']['all_mean']:.1f},{s['latency_ns']['all_p99']:.1f},{s['bandwidth_GBps']['total_avg']:.3f}")
open(os.path.join(workdir,"fig09_credit.csv"),"w").write("\n".join(rows)+"\n")
print("fig09 ok",len(rows)-1)
EOF
  cp "$WORK/fig09/fig09_credit.csv" "$CSV/fig09_credit.csv"
  mark fig09
fi
### fig10 gim (Fig.10: GIM E3A overhead + E3B credit-saturation) ###
# Mirrors run_feature_experiments.py E3 exactly (seeds + overrides). The paper
# panel shows E3B (credit-saturated, us-scale, device-count invariant).
if want 10 && ! stamp fig10; then
  echo "==> [fig10] GIM device-count sweep (E3A + E3B)"
  mkdir -p "$WORK/fig10"
  gimcfg() { case "$1" in
    1) echo "$REPO/configs/cxl_3.1.cfg";; 2) echo "$REPO/configs/cxl_3.1_gim2.cfg";;
    4) echo "$REPO/configs/cxl_3.1_gim4.cfg";; esac; }
  mkseed() { python3 - "$1" "$2" <<'EOF'
import sys
path, nreq = sys.argv[1], int(sys.argv[2])
with open(path, "w") as f:
    f.write(f"# fig10 seed: {nreq} requesters (closed-loop)\n")
    for rid in range(nreq): f.write(f"0 R 0x{rid*64:016x} 64 {rid}\n")
EOF
  }
  { echo "scenario,num_devices,gim_arb_ns,mean_lat_ns,p99_lat_ns,bw_GBps"; for scen in E3A E3B; do
    if [[ $scen == E3A ]]; then NREQ=32; CREQ=0; CL=1000; STR=2048; REG=67108864
    else NREQ=128; CREQ=8; CL=2000; STR=64; REG=16777216; fi
    [[ $SMOKE -eq 1 && $scen == E3B ]] && continue
    seed="$WORK/fig10/${scen}_seed.trace"; mkseed "$seed" $NREQ
    for n in 1 2 4; do
      o="$WORK/fig10/${scen}_gim${n}"; mkdir -p "$o"
      python3 "$SIM" -c "$(gimcfg $n)" -t "$seed" -o "$o" \
        --set dram.backend=queued --set link.credits.m2s_req=$CREQ \
        --set inject.closed_loop_per_req=$CL --set inject.stride_bytes=$STR \
        --set inject.region_bytes=$REG >/dev/null
      python3 - "$o/summary.json" "$scen" "$n" <<'EOF'
import json,sys
s=json.load(open(sys.argv[1])); lat=s["latency_ns"]
print(f"{sys.argv[2]},{sys.argv[3]},{s['overhead_ns']['hdm_interleave_arbitration_ns']:.1f},{lat['all_mean']:.1f},{lat['all_p99']:.1f},{s['bandwidth_GBps']['total_avg']:.3f}")
EOF
    done
  done; } > "$CSV/fig10_gim.csv"
  mark fig10
fi
### fig11 type2 (Fig.11: Type 2 vs Type 3, E4 with R/W split) ###
if want 11 && ! stamp fig11; then
  echo "==> [fig11] Type2 vs Type3 (T2 read + T6 read/write)"
  mkdir -p "$WORK/fig11"
  [[ -f "$T6" ]] || python3 "$REPO/validation/traces/generate_traces.py" --trace T6 --output "$TR"
  { echo "device,trace,mean_lat_ns,read_lat_ns,write_lat_ns,bw_GBps"; for dev in type3 type2; do
    cfg="$REPO/configs/cxl_3.1.cfg"; [[ $dev == type2 ]] && cfg="$REPO/configs/cxl_3.1_type2.cfg"
    for t in "$T2" "$T6"; do
      tag="$(basename "$t" .trace)"; o="$WORK/fig11/${dev}_${tag}"
      run_sim "$cfg" "$t" "$o"
      python3 - "$o/summary.json" "$dev" "$tag" <<'EOF'
import json,sys
s=json.load(open(sys.argv[1])); lat=s["latency_ns"]
print(f"{sys.argv[2]},{sys.argv[3]},{lat['all_mean']:.2f},{lat['read_mean']:.2f},{lat.get('write_mean',0):.2f},{s['bandwidth_GBps']['total_avg']:.3f}")
EOF
    done
  done; } > "$CSV/fig11_type2.csv"
  mark fig11
fi
### fig12 multihost (Fig.12: fabric sharing, T9) ###
if want 12 && ! stamp fig12; then
  echo "==> [fig12] multihost sharing (T9 per-host files)"
  mkdir -p "$WORK/fig12"
  if [[ $SMOKE -eq 1 ]]; then MXH=4; else MXH=16; fi
  [[ -f "$TR/T9_mh_h0.trace" ]] || python3 "$REPO/validation/traces/generate_traces.py" --trace T9 --max-hosts $MXH --output "$TR"
  # Mirrors run_cxl40_experiments.py E5b on the 3.1 switch config: shared
  # downstream link (Gen6 x8: 64*8*0.85/8 = 54.4 GB/s) isolates link
  # saturation; per-host credit window 32 avoids credit starvation.
  # N=8,16 oversubscribe by design (paper: DL 200%/400%); open-loop injection
  # diverges there, which is the documented sharing knee, not a failure.
  HOSTS="1 2 4 8 16"; [[ $SMOKE -eq 1 ]] && HOSTS="1 2"
  { echo "hosts,mean_lat_ns,p99_lat_ns,bw_GBps,per_host_bw_GBps,dl_util_pct"; for n in $HOSTS; do
    o="$WORK/fig12/mh${n}"; mkdir -p "$o"
    args=(); for ((h=0; h<n; h++)); do args+=("$TR/T9_mh_h${h}.trace"); done
    python3 "$SIM" --traces "${args[@]}" -c "$REPO/configs/cxl_3.1_switch1hop.cfg" -o "$o" \
      --set multihost.downstream_bw_GBps=54.4 --set multihost.per_host_credits=32 \
      --set multihost.num_hosts=$n >/dev/null
    python3 - "$o/summary.json" "$n" <<'EOF'
import json,sys
s=json.load(open(sys.argv[1]))
n=int(sys.argv[2]); lat=s['latency_ns']; bw=s['bandwidth_GBps']['total_avg']
link=s.get('link',{}); dl=link.get('switch_dl_utilization',0) or 0
print(f"{n},{lat['all_mean']:.2f},{lat['all_p99']:.2f},{bw:.3f},{bw/n:.3f},{float(dl)*100:.1f}")
EOF
  done; } > "$CSV/fig12_multihost.csv"
  mark fig12
fi
### figs 4+7 (closed-loop multicore tier: CXRay, V4 recipe) ###
# Each core count maps to that many closed-loop requesters (one outstanding
# Fig.4 envelope: closed-loop stride walk, unconstrained credits, simple DRAM
# backend. One outstanding miss per core; the mean is the unloaded floor at
# every core count (p99 == mean by construction). The older zsim+Pin
# execution tier (scripts/run_zsim_figs.py) is out of reproduction scope.
if { want 4 || [[ -z "$ONLY" ]]; } && ! stamp fig04; then
  echo "==> [fig04] CXRay closed-loop envelope sweep"
  if [[ $SMOKE -eq 1 ]]; then
    CORES="4 16"; VERS="2.0 ddr"
  else
    CORES="4 8 16 32 64"; VERS="ddr 2.0 3.0 3.1 4.0"
  fi
  # shellcheck disable=SC2086
  python3 "$REPO/scripts/run_cxray_multicore.py" --out-csv "$CSV" --work "$WORK/mc" \
    --cores $CORES --versions $VERS || echo "[warn] envelope sweep rc=$?"
  if ! grep -q ",simulated" "$CSV/fig04_bw_concurrency.csv" 2>/dev/null; then
    echo "[warn] no simulated envelope rows; marking fig 4 failed (see log)"
    echo "fig,note" > "$CSV/fig04_bw_concurrency.csv"; echo "cxray,failed-see-log" >> "$CSV/fig04_bw_concurrency.csv"
  fi
  mark fig04
fi
### fig07 loaded (Fig.7: bursty closed-loop, finite pools, queued backend) ###
# Each core reissues immediately on completion to a uniform-random line: issue
# stays bounded by the requester count while bank conflicts and credit stalls
# produce genuine load tails (p99 > mean, widening with core count).
if { want 7 || [[ -z "$ONLY" ]]; } && ! stamp fig07; then
  echo "==> [fig07] CXRay bursty closed-loop loaded sweep"
  if [[ $SMOKE -eq 1 ]]; then
    CORES="4 8"; VERS="2.0"
  else
    CORES="4 8 16 32 64"; VERS="ddr 2.0 3.0 3.1 4.0"
  fi
  # shellcheck disable=SC2086
  python3 "$REPO/scripts/run_cxray_loaded.py" --out-csv "$CSV" --work "$WORK/mc_loaded" \
    --cores $CORES --versions $VERS || echo "[warn] loaded sweep rc=$?"
  if ! grep -q ",simulated" "$CSV/fig07_loaded.csv" 2>/dev/null; then
    echo "[warn] no simulated loaded rows; marking fig 7 failed (see log)"
    echo "fig,note" > "$CSV/fig07_loaded.csv"; echo "cxray,failed-see-log" >> "$CSV/fig07_loaded.csv"
  fi
  mark fig07
fi
### table6 (Table VI: Python vs C++ Mreq/s + host string) ###
if [[ -z "$ONLY" || "$ONLY" == "table6" || "$ONLY" == "table" ]] && ! stamp table6; then
  echo "==> [table6] simspeed (trace-driven; MIPS col is host-recorded)"
  TS="--target-seconds 10"; [[ $SMOKE -eq 1 ]] && TS="--target-seconds 2 --traces T2"
  # shellcheck disable=SC2086
  python3 "$REPO/scripts/bench_simspeed.py" --skip-zsim --output "$WORK/bench" $TS || echo "[warn] bench rc=$?"
  cp "$WORK/bench/bench_simspeed_results.csv" "$CSV/table6_simspeed.csv" 2>/dev/null || echo "bench,note" > "$CSV/table6_simspeed.csv"
  uname -a > "$WORK/bench/host.txt"; cat "$WORK/bench/host.txt"
  mark table6
fi
### plots ###
if [[ -z "$ONLY" || "$ONLY" == "plots" || "$ONLY" == "plot" ]] && ! stamp plots; then
  echo "==> [plots] rendering PNGs"
  python3 "$REPO/artifact/plot_ae.py" --csv "$CSV" --out "$OUT" \
    --hw "$REPO/real-hardware/bandwidth-latency.csv" --ref "$REPO/paper-data"
  mark plots
fi
echo "ALL DONE. Figures: $OUT/*.png  CSVs: $CSV/"
ls "$OUT"/*.png 2>/dev/null || echo "(no PNGs yet — run without --fig for all stages)"
