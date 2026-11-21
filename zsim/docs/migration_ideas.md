# Novel Page Migration Methods for DDR-CXL Tiered Memory

## Background & Root Cause Analysis

### Why ROB-Stall Methods Underperform FreqBased

All existing ROB-stall-based methods (robStallBased, robPressureWeighted, criticalityWeighted)
share a fundamental **observability bias**: ROB stall cycles are only generated for pages
currently in CXL (slow memory). Once a hot page is promoted to DDR, it stops causing ROB
stalls, making its per-interval score drop to zero.

This creates a perverse ranking in the migration sort:

| Page | Location | accessCount | robStallCycles | Sort Score |
|------|----------|-------------|----------------|------------|
| A    | DDR      | 50,000      | 0              | **0**      |
| B    | CXL      | 30          | 1,500          | **1,500**  |

Page B (30 accesses, trivial stall) is ranked above Page A (50k accesses, genuinely hot).
When B is promoted, A is demoted—making performance worse. This is why robStallBased often
**loses to freqBased**: it unknowingly demotes hot DDR pages to make room for barely-accessed
CXL pages that happened to cause one stall event.

FreqBased avoids this entirely because it tracks `accessCount` for all pages regardless of
their current location.

### CXL-Specific Properties That Matter

1. **Large but moderate latency differential**: DDR≈200 cycles, CXL≈1000-1150 cycles (~5-6×).
   This ratio is large enough that stall signals matter, but small enough that frequency
   still provides a strong signal—unlike DDR-NVM where NVM is 50-100× slower.

2. **Shared CXL link with congestion**: Unlike NVM channels (dedicated, fixed latency),
   the CXL link is a shared PCIe/CXL bus. Under load, observed latency can far exceed
   the baseline, inflating stall measurements with queuing noise.

3. **Migration bandwidth competes with application bandwidth**: Each page migration (4 KB)
   consumes CXL bandwidth needed for application accesses. Excessive or oscillating
   migrations waste this shared resource.

---

## Individual Methods (7 new)

### Method: fsc — Frequency-Stall Composite

**Sort key**: `score = accessCount + α × robStallCycles`

**Core idea**: Directly fixes the observability bias by giving DDR pages a non-zero score
via their access count. CXL pages get extra weight from their stall signal.

**Alpha calibration**: Set `α = 1 / cxlBaselineLatency` (default: 0.001). This makes
one full-CXL-latency stall cycle equivalent to one additional effective access. A CXL page
with `x` accesses at stall probability `p` scores approximately `x + x·p·1000·α = x(1 + p)`,
only slightly higher than a DDR page with the same access count—correct behavior.

**CXL-specific**: The `α` scaling depends on the CXL latency. With DDR-NVM (latency 20,000+
cycles), stalls would completely dominate and collapse to pure stall-based. The moderate
CXL ratio makes both signals contribute meaningfully.

**Expected benefit**: Should match or exceed freqBased while also giving urgency boost to
stall-heavy CXL pages. Likely the most impactful single improvement.

---

### Method: sys — Stall Yield Score

**Sort key**: `score = robStallCycles × (1 + robStallCycles / accessCount)`

**Core idea**: Identifies pages that are disproportionately on the critical path. A page
with 50 accesses and 45,000 stall cycles (900 cycles/access stall rate) is far more
critical than one with 10,000 accesses and 100,000 stall cycles (10 cycles/access).

The `(1 + yield)` multiplier gives extra weight to pages with high stall intensity
(stall cycles per access) while still considering the absolute stall count.

**DDR pages**: Still score 0 (observability bias not fixed). Best used in combination
with FSC (see `sys+cema`).

**CXL-specific**: The stall yield = robStallCycles/accessCount estimates the average
"CXL congestion cost per access" for that page. Pages with yield ≈ CXL_latency had
nearly every access block the ROB head—they are maximally critical.

---

### Method: sse — Shadow Stall Estimation

**Sort key**:
- CXL page: `score = robStallCycles` (observed)
- DDR page: `score = accessCount × globalStallYield × cxlBaselineLatency` (estimated)

where `globalStallYield = sum(robStallCycles for CXL pages) / sum(accessCount for CXL pages)`
computed fresh each interval.

**Core idea**: Directly addresses the observability gap. For DDR pages, we estimate what
their stall score *would be* if they were moved to CXL. This makes DDR and CXL pages
compete on equal footing.

**Interpretation**: `globalStallYield` is the fraction of CXL accesses that cause ROB head
stalls (typically 5–15%). The shadow stall for a DDR page = accessCount × P(stall per access)
× CXL_latency ≈ what it would contribute if demoted.

**CXL-specific**: Requires `cxlBaselineLatency` as a parameter. The formula is physically
meaningful only with a specific, known slow-tier latency.

---

### Method: spfd — Stall-Promote Frequency-Demote

**Promotion**: Sort CXL pages by `robStallCycles` descending (same as robStallBased)
**Demotion victim**: Choose the DDR page with the **lowest access count** (not lowest stall score)

**Core idea**: Decouples promotion and demotion decisions. Current methods use the same
sort key for both: the demotion victim is the DDR page with the worst stall score—which
is zero for all DDR pages, making victim selection arbitrary. SPFD explicitly picks the
DDR page we'd miss least (lowest access count) when making room for a high-stall CXL page.

**Implementation**: SPFD builds the `canDemote` list as ALL DDR pages sorted by access
count ascending, so `pop_back()` gives the lowest-access DDR page as the eviction victim.

**CXL-specific**: The large latency gap means evicting a high-frequency DDR page is very
costly (it will cause many future stalls). SPFD minimizes this cost by always picking the
lowest-frequency DDR page as the victim.

---

### Method: cema — Cross-Interval Exponential Moving Average

**Sort key**: Sort by `ema_stall` (EMA of robStallCycles across intervals)

**EMA update** (at each interval boundary, before reset):
```
ema_stall_new  = (1 - λ) × ema_stall_old  + λ × current_interval_robStallCycles
ema_access_new = (1 - λ) × ema_access_old + λ × current_interval_accessCount
```
Default λ = 0.3 (30% weight on current interval, 70% on history).

**Core idea**: Fixes "interval amnesia." A page that was consistently causing stalls for
10 intervals but happened to be quiet in the current interval (e.g., it temporarily fit
in L3 cache) loses all its score at the interval boundary with hard reset. EMA retains
the history.

**Also prevents thrash**: A page promoted 2 intervals ago (now in DDR, stall=0) retains
a decaying `ema_stall` from when it was in CXL. This prevents it from being immediately
demoted just because its current-interval score dropped to 0.

**CXL-specific**: Each migration consumes 4 KB of CXL bandwidth. Reducing thrash
(unnecessary promote-then-demote cycles) directly frees CXL bandwidth for application
accesses. This tradeoff is unique to CXL's shared link.

---

### Method: clcas — CXL Link Congestion Amplified Score

**Sort key**: `score = sum(stall_cycles² / cxlBaselineLatency)` per page
         = `congestionWeightedStall` (tracked per-event as accesses occur)

**Per-event update** (in `registerRobHeadLoadStall`):
```
congestionWeightedStall += stall_cycles × (stall_cycles / cxlBaselineLatency)
```

**Core idea**: When CXL is congested, observed stall cycles far exceed the baseline
(e.g., 3000 cycles instead of 1000). This "congestion excess" should count more:
promoting a page that was accessed during peak congestion eliminates not just the
page's own stall but also its contribution to the shared CXL bottleneck.

The quadratic weighting `stall_cycles²` gives a square-law boost to congestion events
vs. normal events (1000² = 1M for normal, 3000² = 9M for 3× congestion—9× weight).

**CXL-specific**: This idea is *uniquely* applicable to CXL. NVM has a dedicated,
fixed-latency channel with no queuing. CXL's PCIe-based shared link is the specific
source of latency variance. This metric would provide zero discriminative signal for
DDR-NVM tiering.

---

### Method: sbs — Stability-Boosted Score

**Sort key**: `score = base_stall_score × (1 + γ × min(consecutiveIntervals, maxStable))`

where `consecutiveIntervals` = number of consecutive intervals the page has remained
in its current memory tier, and γ = 0.05, maxStable = 10 (configurable).

**Core idea**: Pages that have been stably placed in DDR for multiple intervals are
likely genuinely hot and should resist demotion. Similarly, newly-promoted pages
(consecutiveIntervals = 0) get no bonus until they prove their stability.

**Stability update** (at each interval boundary, after migration):
```
if page stayed in same location: consecutiveIntervals++
if page changed location:        consecutiveIntervals = 0
```

**Effect**: A DDR page stable for 10 intervals gets 1.5× effective score multiplier,
making it much harder to displace. A newly promoted CXL page starts with no bonus.
This prevents thrashing where marginally hot pages oscillate between tiers.

**CXL-specific**: CXL link bandwidth is shared. Each migration (4 KB) consumes bandwidth
that competes with application accesses. Stability-based hysteresis directly reduces
migration count and the associated CXL bandwidth waste.

---

## Combined Methods (6 new)

### Method: fsc+cema — Frequency-Stall Composite with EMA

**Sort key**: `score = ema_access × α + ema_stall` (EMA of composite FSC score)

Combines FSC's observability fix with CEMA's temporal persistence. The EMA is applied
to *both* access count and stall cycles, then the composite score is computed from EMA values.

**Best for**: Workloads with consistent hotness patterns across intervals and some phase
sensitivity.

---

### Method: fsc+sbs — Frequency-Stall Composite with Stability Bonus

**Sort key**: `score = (accessCount × α + robStallCycles) × stabilityMultiplier`

Combines FSC's bias correction with SBS's hysteresis. The stability bonus amplifies the
score of pages that have been correctly placed for multiple intervals, reducing churn.

**Best for**: Workloads where the hot working set is stable (reduces unnecessary migrations).

---

### Method: fsc+cema+sbs — Full Combination A (recommended)

**Sort key**: `score = (ema_access × α + ema_stall) × stabilityMultiplier`

Stacks all three complementary improvements:
1. **FSC**: Corrects observability bias (DDR pages get non-zero score)
2. **CEMA**: Prevents interval amnesia (history survives interval resets)
3. **SBS**: Reduces migration thrash (stable pages are protected)

This is the primary recommended combination for improving over freqBased.

---

### Method: sse+spfd — Shadow Stall with Asymmetric Demotion

**Scoring**: SSE unified scoring (DDR pages get shadow stall estimate)
**Demotion**: SPFD asymmetric (evict lowest-access DDR page)

Makes DDR and CXL pages compete on a fair, unified scale (SSE), then ensures the
demotion victim is the DDR page we'd miss least (SPFD). Particularly strong when
there are large access count disparities among DDR pages.

---

### Method: sse+spfd+clcas — Full Combination B (CXL-specific combination)

**Scoring**:
- CXL pages: `congestionWeightedStall` (CLCAS)
- DDR pages: `accessCount × globalStallYield × cxlBaselineLatency` (SSE)
**Demotion**: `min(accessCount)` DDR page (SPFD)

The most CXL-specific combination. Uses observed CXL congestion to identify the most
critical CXL pages (CLCAS), estimates DDR page value fairly (SSE), and selects the
best demotion victim (SPFD). Zero benefit over SSE+SPFD for non-congestion workloads
(Simple memory model), but valuable for WeaveMessMemCtrl with realistic CXL timing.

---

### Method: sys+cema — Stall Yield with Persistence

**Sort key**: `score = ema_stall × (1 + ema_stall / max(1, ema_access))`

Applies the stall yield formula to EMA values instead of per-interval values. A page
that *consistently* stalls with high intensity (across many intervals) gets highest
priority—filtering out one-time spikes while rewarding persistent critical-path pages.

---

## Implementation Notes

### Parameters

| Parameter | Default | Used By | Meaning |
|-----------|---------|---------|---------|
| `alpha`   | 0.001   | FSC, fsc+* | Weight for access count in composite score |
| `lambda`  | 0.3     | CEMA, fsc+cema, fsc+cema+sbs, sys+cema | EMA decay rate |
| `gamma`   | 0.05    | SBS, fsc+sbs, fsc+cema+sbs | Stability bonus per interval |
| `maxStabilityIntervals` | 10 | SBS, fsc+sbs, fsc+cema+sbs | Cap on stability bonus |
| `cxlBaselineLatency` | 1000.0 | CLCAS, SSE | Baseline CXL latency in cycles |

### Config File Parameters

```
sys.mem.migrationPolicy = "fsc"         # or sys, sse, spfd, cema, clcas, sbs,
                                         #    fsc+cema, fsc+sbs, fsc+cema+sbs,
                                         #    sse+spfd, sse+spfd+clcas, sys+cema
sys.mem.fscAlpha = 0.001                 # Alpha for FSC methods
sys.mem.emaLambda = 0.3                  # Lambda for EMA methods
sys.mem.stabilityGamma = 0.05           # Gamma for SBS methods
sys.mem.cxlBaselineLatency = 1000.0     # Baseline CXL latency for CLCAS/SSE
```

### Thread Safety

All methods use the same `pageTrackerMutex` pattern as existing migrators.
The `registerRobHeadLoadStall` and `access` methods both acquire this lock.

### Stats Reported

All HybridPageMigrator instances report:
- `numPageMigrations`, `numSwappedIn`, `numSwappedOut`, `numHotPagesStayedIn`
- `pageTrackerAccesses`, `pageTrackerHits`
- `avgEmaStall` — average EMA stall score (for EMA methods)
- `avgStabilityBonus` — average stability multiplier (for SBS methods)
- `avgShadowStallYield` — globalStallYield value (for SSE methods)
