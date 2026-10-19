# Speculation Tuning: Final Recommendations

**Date:** October 19, 2026  
**Branch:** speculation  
**Current:** 22% acceptance, ×2.5 speedup, 39.4 tok/s  
**Target:** 35-40% acceptance, ×3-3.5 speedup

## Key Finding

**Draft width (8 verify rows) is near-optimal.** Changing SPLASH_TARGET_VERIFY_ROWS won't reach 35-40% acceptance target. Focus on draft model quality instead.

## Recommended Path to 35-40% Acceptance

### Phase 1: Quick Wins (1 week) — +4-6 percentage points

**1. Add draft temperature tuning**
- Location: `runtime/ops/DraftSelector.cpp`
- Add `--draft-temperature` flag (default: 1.0)
- Test range: 0.6, 0.7, 0.8, 0.9, 1.0
- Expected: 24-26% acceptance

**2. Add draft top-p filtering**
- Add `--draft-top-p` flag (default: 0.95)
- Test range: 0.8, 0.9, 0.95, 0.98
- Expected: 26-28% acceptance

### Phase 2: Model Quality (3 weeks) — +6-9 percentage points

**3. Retrain DFlash2 codebooks**
- Collect 10K outputs from target model
- Retrain predecessor/successor codebooks
- Expected: 32-35% acceptance
- **This is the main lever for reaching target**

### Phase 3: Stretch Goal (2-3 months) — +10-15 points

**4. Larger draft model or adaptive depth**
- Use bigger draft checkpoint (~2 GiB vs. current 1.18 GiB)
- Or implement runtime-adaptive verify row count
- Expected: 38-42% acceptance

## Projected Outcomes

| Phase | Acceptance | Speedup | Decode tok/s | Effort | Risk |
|-------|-----------|---------|--------------|--------|------|
| Baseline | 22% | ×2.5 | 39.4 | — | — |
| Phase 1 | 26-28% | ×2.7-2.8 | 42-44 | 1 week | Low |
| Phase 2 | 32-35% | ×3.0-3.2 | 47-50 | 4 weeks | Medium |
| Phase 3 | 38-42% | ×3.4-3.8 | 53-59 | 3 months | High |

**Recommendation:** Execute Phase 1-2 to reach 32-35% acceptance in 4 weeks.

## What NOT to Change

❌ **Don't change SPLASH_TARGET_VERIFY_ROWS** — previous narrowing experiments lost −40% throughput  
❌ **Don't optimize TTFT** — already excellent at 116 ms  
❌ **Don't pursue adaptive batching** — batching already scales linearly

## Implementation Priority

1. **Now:** Draft temperature flag (6 hours coding + testing)
2. **Week 1:** Top-p filtering + measurement suite
3. **Week 2-4:** Codebook retraining (data collection + training)
4. **Later:** Evaluate larger draft model or adaptive depth

## Success Criteria

- Acceptance rate: ≥32% (minimum), ≥35% (target), ≥38% (stretch)
- Decode speedup: ×3.0 or better
- Cycle time: Still ~75 ms (must stay bandwidth-bound)
- No regression in TTFT

See SPECULATION_TUNING.md for full analysis.
