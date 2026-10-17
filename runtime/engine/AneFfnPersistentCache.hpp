#pragma once

#include "ops/AneFfnCalibration.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <mutex>

namespace splash::engine {

// Persistent ANE calibration cache that survives across engine restarts.
// Saves calibration data to disk to avoid 2-3s startup penalty on every launch.
// Cache is keyed by model identifier and max ANE units.
class AneFfnPersistentCache final {
public:
  AneFfnPersistentCache();
  ~AneFfnPersistentCache();

  // Load calibration from persistent cache for the given model and max ANE units.
  // Returns nullopt if no cached calibration exists or if cache is invalid.
  [[nodiscard]] std::optional<ops::ane_ffn::Calibration> 
  load(const std::string &model_key, uint32_t maxAneUnits);

  // Save calibration to persistent cache for future engine starts.
  void save(const std::string &model_key, uint32_t maxAneUnits,
            const ops::ane_ffn::Calibration &calibration);

  // Remove calibration from cache (used when recalibrating).
  void remove(const std::string &model_key, uint32_t maxAneUnits);

  // Clear all cached calibrations.
  void clear();

  // Get the cache file path for diagnostics.
  [[nodiscard]] std::string cachePath() const;

private:
  std::string makeKey(const std::string &model_key, uint32_t maxAneUnits) const;
  void loadFromDisk();
  void saveToDisk();

  std::string cache_path_;
  std::unordered_map<std::string, ops::ane_ffn::Calibration> cache_;
  std::mutex mutex_;
  bool dirty_ = false;
};

// Global persistent cache instance (initialized in AneFfnPersistentCache.cpp).
[[nodiscard]] AneFfnPersistentCache& aneFfnPersistentCache();

} // namespace splash::engine
