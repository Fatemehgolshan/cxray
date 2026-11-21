# Artifact evaluation (HPCA 2027)

Run sheet, result checklist, and submission pointers for the CXRay artifact.
The appendix draft ships with the paper (not in this repo; fill
`TODO-REPO`, `TODO-DOI` there before HotCRP upload).

## What the evaluator runs

```bash
docker build -f docker/Dockerfile -t cxray-ae .
docker run --rm -v "$PWD/outputs:/repo/outputs" cxray-ae | tee eval.log
docker run --rm cxray-ae ./reproduce.sh --smoke   # fast path (~6 min)
```

Verified image: `cxray-ae:latest`,
image ID `sha256:9e2c7dd35b6808e38344917f9a8ce009bef6d7e220b708dfacee16b5d9368a69`
(1.13 GB; registry digest assigned on push).
Behind a proxy add `--build-arg http_proxy=$http_proxy --build-arg
https_proxy=$https_proxy` to the build.
The run needs no Pin, zsim, license server, or cluster; internet is assumed
for apt only.

## What the evaluator checks

- `outputs/*.png` (12 figures + Table VI) against `artifact/reference/`
  golden PNGs: byte-identical on a container rerun; only Table VI host
  speeds differ (recorded on our host, re-measured on yours).
- `outputs/csv/` values against the paper's macros (tolerances per figure
  in `validation/VALIDATION_PLAN.md`): Fig. 7 matches all 75 loaded values
  exactly; Figs. 4/9 round exactly to their macros; Figs. 3, 5, 6, 8, 10,
  11 match floors/overheads within 1 ns.
- `reproduce.sh` resume: killing and rerunning continues from stamps in
  `artifact/work/` (each stage prints `[done] <stage>`; the run ends with
  `ALL DONE`). Fig. 10 is a placeholder under `--smoke` only (E3B skipped).

## Measured stage times

Per-stage wall times from a timestamped full container run on a 32-core
Xeon E5-2670 / 62 GB host (02:26–02:59 UTC):

| Stage | Time |
|---|---|
| traces (T1–T9 generation) | 3.9 min |
| fig02 calibration sweep | 2.2 min |
| fig11 Type2 vs Type3 | 1.7 min |
| fig12 multihost (dominant) | 14.8 min |
| fig04 envelope sweep | 1.9 min |
| fig07 loaded sweep (25 configs) | 2.4 min |
| table6 sim-speed benchmark | 3.9 min |
| build / fig03 / fig05 / fig06 / fig08 / fig09 / fig10 / plots | <1 min each |
| **full run, end to end** | **~33 min** |
| `--smoke` subset | ~6 min |
| image build (cold, apt + CMake + ctest) | ~5 min |

## Claims (badges)

- Reproduced (standalone CXRay): Figs. 2–12 (all CXRay curves) + Table VI
  CXRay columns. Target: Results Reproduced; Reusable (documented tiers,
  configs, and tests for repurposing).
- Recorded reference, shipped not re-measured: Fig. 2 CZ120 hardware curve,
  Table VI zsim/Xerxes columns (machine-specific).
- Out of scope: Tables VII–VIII, GAPBS/ML runs, gem5/Sniper execution paths.
- Archival: Zenodo TODO-DOI (exact submitted tree; new version if AE review
  requires changes).

## Pointers (do not copy)

- Repro entry: `reproduce.sh`, `README.md` (Stage 0).
- Image recipe: `docker/Dockerfile`, `docker/README_PIN.md`.
- Golden outputs: `artifact/reference/`.
- Paper + appendix draft: ship with the publication, not in this repo.
