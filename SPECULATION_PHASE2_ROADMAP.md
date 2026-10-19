# Speculation Phase 2 Roadmap: Codebook Retraining

## Executive Summary

Phase 2 focuses on retraining DFlash2 predecessor/successor codebooks to improve speculation acceptance rates from the current 22% baseline to a target of 32-35%. This represents a +10-13 point improvement that will increase decode throughput from 39.4 tok/s to 47-50 tok/s and overall speedup from ×2.5 to ×3.0-3.2.

**Timeline**: 3-4 weeks  
**Expected Impact**: +45-55% improvement in acceptance rate  
**Risk Level**: Medium (requires retraining infrastructure, validation of convergence)

---

## 1. Problem Statement

### Current Baseline Measurements
- **Acceptance Rate**: 22% (measured range: 18-27%)
- **Decode Throughput**: 39.4 tok/s
- **Overall Speedup**: ×2.5 vs non-speculative baseline
- **Configuration**: 4 draft tokens per speculation round

### Root Cause Analysis
The current DFlash2 codebooks (predecessor and successor) were likely trained on a different model distribution than the production target (Ternary-Bonsai-2-27B PQ2_0). This distribution mismatch causes:

1. **Suboptimal cluster assignments**: Draft token predictions don't align well with the actual next-token distribution of the target model
2. **Poor generalization**: Codebook entries fail to capture the statistical patterns of PQ2_0 quantized activations
3. **Acceptance rate ceiling**: Even with optimal runtime parameters, acceptance is constrained by codebook quality

### Why Phase 1 Was Blocked
Phase 1 (runtime parameter tuning) cannot proceed because:
- Draft generation parameters are not runtime-configurable in the current codebase
- DFlash2 uses fixed, pre-compiled codebooks that cannot be adjusted at runtime
- Any tuning would require recompilation and retraining, making iterative optimization impractical

**Conclusion**: Codebook retraining is the prerequisite for further optimization.

---

## 2. Technical Approach

### 2.1 Data Collection Strategy

**Objective**: Collect representative activation distributions from Ternary-Bonsai-2-27B PQ2_0.

**Method**:
1. **Corpus Selection**: Use a diverse evaluation corpus covering multiple domains:
   - Conversational text (50%)
   - Technical documentation (20%)
   - Code (15%)
   - Creative writing (15%)
   - Target: 10M tokens minimum

2. **Activation Capture**: Instrument the target model to collect:
   - Layer-wise activation statistics
   - Attention pattern distributions
   - Token prediction distributions at each layer
   - Focus on layers where draft model makes predictions

3. **Data Format**: Store as compressed numpy arrays or HDF5:
   - `activations_layer_{N}.h5`: activation tensors
   - `metadata.json`: corpus statistics, layer configs

**Estimated Duration**: 3-4 days

### 2.2 Codebook Training Algorithm

**Objective**: Train k-means or VQ-VAE codebooks that minimize reconstruction error on PQ2_0 activations.

**Algorithm Choice**:
- **Primary**: k-means clustering with cosine distance metric
- **Alternative**: Vector Quantization (VQ-VAE) if k-means convergence is poor
- **Rationale**: k-means is simpler, faster, and sufficient for most quantization tasks

**Training Process**:
1. **Initialization**: 
   - Use k-means++ for stable initialization
   - Codebook size: match existing DFlash2 dimensions (typically 256-1024 entries)

2. **Training Loop**:
   ```
   for epoch in 1..max_epochs:
       - Assign each activation vector to nearest codebook entry
       - Update codebook entries as cluster centroids
       - Compute reconstruction error
       - If error delta < threshold: converge
   ```

3. **Convergence Criteria**:
   - Reconstruction error improvement < 0.1% over 3 consecutive epochs
   - OR maximum 50 epochs reached

4. **Validation**:
   - Hold out 10% of activation data for validation
   - Measure reconstruction error on held-out set
   - Ensure no overfitting (train/val error gap < 5%)

**Estimated Duration**: 5-7 days (including hyperparameter tuning)

### 2.3 Integration into DFlash2

**Objective**: Replace existing codebooks in DFlash2 with newly trained versions.

**Steps**:
1. **Codebook Export**: Serialize trained codebooks to binary format compatible with DFlash2
2. **Code Modification**: Update DFlash2 initialization to load new codebooks
3. **Validation**: Ensure numerical equivalence (codebook lookup results match expected values)
4. **Build Integration**: Update build scripts to package new codebooks

**File Modifications**:
- `src/speculation/dflash2_codebook.cpp`: Codebook loading logic
- `assets/codebooks/`: New codebook binary files
- `CMakeLists.txt`: Add new codebook assets to build

**Estimated Duration**: 2-3 days

---

## 3. Implementation Phases

### Phase 2.1: Data Collection (Week 1)
**Duration**: 3-4 days

**Tasks**:
- [ ] Select and prepare evaluation corpus (10M tokens)
- [ ] Instrument Ternary-Bonsai-2-27B PQ2_0 for activation capture
- [ ] Run inference over corpus and collect activations
- [ ] Validate data quality (check distributions, identify outliers)
- [ ] Store compressed activation datasets

**Deliverables**:
- `activations_data/` directory with layer-wise activation captures
- `data_collection_report.md` with corpus statistics and validation results

**Success Criteria**:
- 10M+ tokens processed
- Activations captured for all relevant layers
- Data validation passes (no NaNs, reasonable value ranges)

---

### Phase 2.2: Codebook Training (Week 2)
**Duration**: 5-7 days

**Tasks**:
- [ ] Implement k-means codebook training script
- [ ] Run training on collected activations
- [ ] Monitor convergence and tune hyperparameters
- [ ] Validate on held-out data
- [ ] Export trained codebooks to DFlash2 format

**Deliverables**:
- `train_codebooks.py`: Training script
- `codebooks/predecessor_codebook.bin`: Trained predecessor codebook
- `codebooks/successor_codebook.bin`: Trained successor codebook
- `training_report.md`: Training curves, convergence metrics, validation results

**Success Criteria**:
- Training converges within 50 epochs
- Reconstruction error on validation set < 5% higher than training set
- Codebook entries have reasonable distribution (no dead entries)

---

### Phase 2.3: Integration and Validation (Week 3)
**Duration**: 4-5 days

**Tasks**:
- [ ] Update DFlash2 codebook loading code
- [ ] Integrate new codebooks into build system
- [ ] Rebuild speculation system
- [ ] Run smoke tests (basic inference correctness)
- [ ] Validate numerical equivalence (codebook lookups)

**Deliverables**:
- Modified `dflash2_codebook.cpp`
- Updated build scripts
- `integration_validation_report.md`

**Success Criteria**:
- Build succeeds with new codebooks
- Inference produces valid outputs (no crashes, no numerical errors)
- Codebook lookups return expected values

---

### Phase 2.4: Performance Measurement (Week 3-4)
**Duration**: 5-6 days

**Tasks**:
- [ ] Run acceptance rate benchmarks with new codebooks
- [ ] Measure decode throughput across evaluation corpus
- [ ] Compare against baseline (22%, 39.4 tok/s)
- [ ] Profile any performance regressions
- [ ] Document results

**Deliverables**:
- `PHASE2_RESULTS.md`: Comprehensive performance report
- Benchmark data: acceptance rates, throughput, speedup metrics

**Success Criteria**:
- Acceptance rate: 32-35% (target range)
- Decode throughput: 47-50 tok/s
- Overall speedup: ×3.0-3.2

---

## 4. Effort Estimate

### Timeline Breakdown

| Phase | Duration | Dependencies | Risk |
|-------|----------|--------------|------|
| 2.1: Data Collection | 3-4 days | None | Low |
| 2.2: Codebook Training | 5-7 days | Phase 2.1 | Medium |
| 2.3: Integration | 4-5 days | Phase 2.2 | Low |
| 2.4: Measurement | 5-6 days | Phase 2.3 | Low |
| **Total** | **17-22 days** | | |

### Resource Requirements
- **Compute**: 
  - Data collection: 1x GPU (A100 or equivalent) for 3-4 days
  - Training: 1x GPU for 5-7 days (k-means can run on CPU, but GPU accelerates)
  - Validation: 1x GPU for 5-6 days
- **Storage**: ~50-100 GB for activation datasets
- **Engineering Time**: 1 full-time engineer for 3-4 weeks

---

## 5. Expected Outcomes

### Primary Metrics

| Metric | Baseline | Target | Improvement |
|--------|----------|--------|-------------|
| Acceptance Rate | 22% (18-27%) | 32-35% | +10-13 points (+45-59%) |
| Decode Throughput | 39.4 tok/s | 47-50 tok/s | +7.6-10.6 tok/s (+19-27%) |
| Overall Speedup | ×2.5 | ×3.0-3.2 | +0.5-0.7× (+20-28%) |

### Secondary Benefits
1. **Foundation for Phase 3**: With properly trained codebooks, runtime parameter tuning becomes viable
2. **Model Generalization**: Better codebooks may improve acceptance across different prompt types
3. **Reusable Infrastructure**: Codebook training pipeline can be reused for future model updates

### Conservative Estimate
If codebook retraining yields lower-than-expected improvements:
- **Minimum Viable**: 28-30% acceptance rate (still a +6-8 point improvement)
- **Throughput Floor**: 44-46 tok/s
- **Speedup Floor**: ×2.8-2.9

---

## 6. Risks and Mitigations

### Risk 1: Training Divergence
**Likelihood**: Medium  
**Impact**: High  
**Description**: k-means training fails to converge or produces poor codebooks.

**Mitigation**:
- Use k-means++ initialization (proven to improve convergence)
- Implement early stopping with validation loss monitoring
- Fall back to VQ-VAE if k-means fails
- Validate on held-out data to catch overfitting

---

### Risk 2: Distribution Mismatch Persists
**Likelihood**: Low  
**Impact**: High  
**Description**: Retraining on PQ2_0 still produces suboptimal codebooks due to unexpected distribution properties.

**Mitigation**:
- Perform exploratory data analysis on activations before training
- Visualize codebook clusters to identify issues (t-SNE, PCA)
- Experiment with alternative distance metrics (cosine vs Euclidean)
- If needed, collect additional activation data from edge cases

---

### Risk 3: Integration Bugs
**Likelihood**: Low  
**Impact**: Medium  
**Description**: New codebooks cause numerical errors or performance regressions.

**Mitigation**:
- Comprehensive unit tests for codebook loading and lookup
- Numerical validation: compare codebook outputs against reference
- Smoke tests on diverse inputs before full benchmarking
- Rollback plan: keep old codebooks available for quick revert

---

### Risk 4: Insufficient Improvement
**Likelihood**: Medium  
**Impact**: Medium  
**Description**: Acceptance rate improves but falls short of 32-35% target.

**Mitigation**:
- Set conservative minimum viable target (28-30%)
- If target not met, analyze failure modes:
  - Which token types have low acceptance?
  - Which layers show poor reconstruction?
- Iterate on training (more data, different hyperparameters)
- Document findings for Phase 3 planning

---

### Risk 5: Resource Availability
**Likelihood**: Low  
**Impact**: Medium  
**Description**: GPU or engineering resources unavailable for planned timeline.

**Mitigation**:
- Pre-reserve GPU instances (A100 or equivalent)
- Parallelize where possible (data collection and training setup can overlap)
- Use spot instances or preemptible VMs to reduce cost pressure
- Have fallback CPU-based training (slower but viable)

---

## 7. Measurement and Validation Protocol

### 7.1 Baseline Establishment
Before starting Phase 2, confirm baseline metrics:
1. Run acceptance rate benchmark (1000 prompts, diverse corpus)
2. Measure decode throughput (tokens/second over 10 runs)
3. Record variance (min, max, stddev)
4. Document configuration (draft tokens, model version, hardware)

**Output**: `baseline_metrics.json`

---

### 7.2 Training Validation
During codebook training:
1. **Every epoch**: Log reconstruction error (train and validation)
2. **Every 5 epochs**: Visualize codebook clusters (t-SNE plot)
3. **At convergence**: Check for dead entries (codebook slots with <0.1% usage)

**Output**: `training_logs/` with plots and metrics

---

### 7.3 Integration Validation
After integrating new codebooks:
1. **Correctness Tests**:
   - Run 100 inference passes with known inputs
   - Compare outputs against baseline (should be identical)
   - Check for numerical stability (no NaNs, reasonable ranges)

2. **Performance Tests**:
   - Measure inference latency (single-token)
   - Ensure no regression in non-speculation performance
   - Profile memory usage

**Output**: `integration_test_results.md`

---

### 7.4 Acceptance Rate Measurement
Run comprehensive benchmarks:
1. **Test Corpus**: 1000 prompts covering:
   - Short prompts (<50 tokens): 30%
   - Medium prompts (50-200 tokens): 50%
   - Long prompts (>200 tokens): 20%

2. **Metrics to Collect**:
   - Per-prompt acceptance rate
   - Per-token-position acceptance rate
   - Acceptance rate by prompt category
   - Variance across runs (10 runs per prompt)

3. **Comparison**:
   - Side-by-side with baseline metrics
   - Statistical significance test (t-test, p < 0.05)

**Output**: `PHASE2_RESULTS.md` with full benchmark data

---

### 7.5 Throughput Measurement
Measure decode throughput:
1. **Test Setup**:
   - Run on representative hardware (target deployment config)
   - Use production-like batch sizes
   - Warm up for 10 iterations, then measure 100 iterations

2. **Metrics**:
   - Tokens/second (mean, min, max, p50, p95)
   - Speedup vs non-speculative baseline
   - Speedup vs Phase 1 baseline

3. **Profiling**:
   - Identify any bottlenecks (codebook lookup, verification)
   - Document any unexpected overhead

**Output**: `throughput_benchmark.json`

---

## 8. Success Criteria

### Must-Have (Go/No-Go)
1. **Acceptance Rate**: ≥ 28% (minimum viable improvement of +6 points)
2. **No Correctness Regressions**: Outputs match baseline on validation set
3. **No Performance Regressions**: Non-speculation latency unchanged
4. **Stable Training**: Codebook training converges within 50 epochs

### Target (Success)
1. **Acceptance Rate**: 32-35% (target range)
2. **Decode Throughput**: 47-50 tok/s
3. **Overall Speedup**: ×3.0-3.2
4. **Generalization**: Improvement consistent across prompt categories (±3% variance)

### Stretch (Exceptional)
1. **Acceptance Rate**: > 35%
2. **Decode Throughput**: > 50 tok/s
3. **Speedup**: > ×3.2
4. **Reusability**: Training pipeline documented and packaged for future models

---

## 9. Next Steps After Phase 2

### If Phase 2 Succeeds (≥32% acceptance)
**Proceed to Phase 3**: Runtime parameter tuning
- Tune draft token count (4 → 5-6?)
- Tune acceptance threshold
- Tune speculation scheduling
- Target: 35-40% acceptance

### If Phase 2 Partially Succeeds (28-31% acceptance)
**Iterate on codebook training**:
- Collect more diverse activation data
- Experiment with VQ-VAE
- Try ensemble codebooks
- Then proceed to Phase 3

### If Phase 2 Fails (<28% acceptance)
**Root cause analysis**:
- Analyze failure modes (which tokens/positions fail?)
- Consider alternative speculation strategies (multi-draft, tree-based)
- Re-evaluate DFlash2 architecture assumptions
- Document findings in `PHASE2_POSTMORTEM.md`

---

## References

- **SPECULATION_TUNING.md**: Baseline measurements, initial analysis, and projections
- **SPECULATION_TUNING_RECOMMENDATIONS.md**: Executive summary and Phase 1 vs Phase 2 decision rationale
- **PHASE1_SPECULATION_FINDINGS.md**: Detailed explanation of why Phase 1 was blocked

---

## Appendix: Key Definitions

- **Acceptance Rate**: Percentage of speculatively generated draft tokens that are accepted by the verification model
- **Decode Throughput**: Tokens generated per second during the decode phase (excludes prefill)
- **Codebook**: Lookup table mapping activation patterns to discrete codes (used in DFlash2 for draft generation)
- **Predecessor/Successor Codebooks**: DFlash2 uses two codebooks to predict next-token distributions based on previous token patterns
- **PQ2_0**: Product Quantization level 2, tier 0 (quantization scheme used by Ternary-Bonsai-2-27B)
- **k-means**: Clustering algorithm used to train vector quantization codebooks
- **Reconstruction Error**: Difference between original activations and their quantized representations (lower is better)

---

**Document Version**: 1.0  
**Last Updated**: 2026-10-19  
**Author**: Speculation Optimization Team  
**Status**: Planning - Awaiting Approval
