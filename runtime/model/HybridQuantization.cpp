#include "HybridQuantization.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace splash::model {

bool HybridQuantConfig::isAneLayer(uint32_t layerIndex) const noexcept {
  return std::find(aneLayerIndices.begin(), aneLayerIndices.end(), layerIndex) != aneLayerIndices.end();
}

void HybridQuantConfig::validate(uint32_t totalLayers) const {
  if (aneLayerIndices.empty()) return;

  for (uint32_t idx : aneLayerIndices) {
    if (idx >= totalLayers) {
      throw std::invalid_argument("hybrid quantization: layer index " + std::to_string(idx) +
                                  " exceeds total layers " + std::to_string(totalLayers));
    }
  }

  // Ensure indices are unique
  std::vector<uint32_t> sorted = aneLayerIndices;
  std::sort(sorted.begin(), sorted.end());
  auto it = std::unique(sorted.begin(), sorted.end());
  if (it != sorted.end()) {
    throw std::invalid_argument("hybrid quantization: duplicate layer indices in ANE layer list");
  }
}

HybridQuantConfig HybridQuantConfig::fromString(const std::string &indices) {
  HybridQuantConfig config;
  if (indices.empty()) return config;

  std::istringstream stream(indices);
  std::string token;

  while (std::getline(stream, token, ',')) {
    // Trim whitespace
    token.erase(0, token.find_first_not_of(" \t"));
    token.erase(token.find_last_not_of(" \t") + 1);

    if (token.empty()) continue;

    try {
      size_t pos;
      int value = std::stoi(token, &pos);
      if (pos != token.size() || value < 0) {
        throw std::invalid_argument("invalid layer index: " + token);
      }
      config.aneLayerIndices.push_back(static_cast<uint32_t>(value));
    } catch (const std::exception &) {
      throw std::invalid_argument("hybrid quantization: failed to parse layer index '" + token + "'");
    }
  }

  // Sort for efficient lookup
  std::sort(config.aneLayerIndices.begin(), config.aneLayerIndices.end());

  return config;
}

LayerQuantMode selectLayerMode(const HybridQuantConfig &config, uint32_t layerIndex) {
  if (!config.enabled()) return LayerQuantMode::Default;
  return config.isAneLayer(layerIndex) ? LayerQuantMode::AneMode : LayerQuantMode::GpuMode;
}

std::string describeHybridConfig(const HybridQuantConfig &config, uint32_t totalLayers) {
  if (!config.enabled() || totalLayers == 0) return "disabled";

  std::ostringstream desc;
  desc << config.aneLayerIndices.size() << " of " << totalLayers << " FFN layers on ANE (";

  for (size_t i = 0; i < config.aneLayerIndices.size(); ++i) {
    if (i > 0) desc << ",";
    desc << config.aneLayerIndices[i];
  }

  desc << "), " << (totalLayers - config.aneLayerIndices.size()) << " on GPU";
  return desc.str();
}

} // namespace splash::model
