#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace splash::model {

// Hybrid quantization configuration: selective per-layer quantization to enable
// ANE acceleration on specific FFN layers while keeping others in a different
// quantization format.
//
// Motivation: PQ2_0 uses rotation, blocking ANE (takesPlaneViews requires
// !rotation). Q4_K_M without rotation enables ANE but weighs 15.3 GB, dropping
// context to ~80K. Hybrid approach: 6 FFN layers (37.5% of 16) in unrotated
// Q4_K for ANE, 10 layers in PQ2_0 for GPU, yielding ~8.85 GB total weight
// (+2.1 GB vs pure PQ2_0) with ~200K context and +25-30% decode speed.
struct HybridQuantConfig final {
  // Layer indices (0-based) that should use ANE-compatible quantization
  // (unrotated Q4_K_M). Empty means no hybrid quantization.
  std::vector<uint32_t> aneLayerIndices;

  [[nodiscard]] bool enabled() const noexcept { return !aneLayerIndices.empty(); }

  // Check if a given layer index should use ANE quantization
  [[nodiscard]] bool isAneLayer(uint32_t layerIndex) const noexcept;

  // Validate the configuration against model layer count
  void validate(uint32_t totalLayers) const;

  // Parse from CLI format: "0,2,4,8,10,14"
  [[nodiscard]] static HybridQuantConfig fromString(const std::string &indices);
};

// Select appropriate quantization format for a specific layer
enum class LayerQuantMode : uint8_t {
  Default,  // Use model's default quantization
  AneMode,  // Use ANE-compatible quantization (unrotated)
  GpuMode   // Use GPU quantization (may use rotation)
};

[[nodiscard]] LayerQuantMode selectLayerMode(const HybridQuantConfig &config, uint32_t layerIndex);

// Human-readable description of the hybrid configuration
[[nodiscard]] std::string describeHybridConfig(const HybridQuantConfig &config, uint32_t totalLayers);

} // namespace splash::model
