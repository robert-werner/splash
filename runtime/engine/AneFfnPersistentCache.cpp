#include "engine/AneFfnPersistentCache.hpp"
#include "StderrLine.hpp"

#include <fstream>
#include <sstream>
#include <filesystem>
#include <unistd.h>
#include <pwd.h>

namespace splash::engine {
namespace {

std::string getHomeDirectory() {
  if (const char *home = std::getenv("HOME")) {
    return home;
  }
  struct passwd *pw = getpwuid(getuid());
  return pw ? pw->pw_dir : "/tmp";
}

std::string getCacheDirectory() {
  std::string home = getHomeDirectory();
  std::string cache_dir = home + "/.cache/splash";
  std::filesystem::create_directories(cache_dir);
  return cache_dir;
}

} // namespace

AneFfnPersistentCache::AneFfnPersistentCache()
    : cache_path_(getCacheDirectory() + "/ane_calibration.cache") {
  loadFromDisk();
}

AneFfnPersistentCache::~AneFfnPersistentCache() {
  if (dirty_) {
    saveToDisk();
  }
}

std::string AneFfnPersistentCache::makeKey(const std::string &model_key,
                                           uint32_t maxAneUnits) const {
  return model_key + ":" + std::to_string(maxAneUnits);
}

std::optional<ops::ane_ffn::Calibration>
AneFfnPersistentCache::load(const std::string &model_key, uint32_t maxAneUnits) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string key = makeKey(model_key, maxAneUnits);
  
  auto it = cache_.find(key);
  if (it != cache_.end()) {
    return it->second;
  }
  return std::nullopt;
}

void AneFfnPersistentCache::save(const std::string &model_key, uint32_t maxAneUnits,
                                 const ops::ane_ffn::Calibration &calibration) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string key = makeKey(model_key, maxAneUnits);
  cache_[key] = calibration;
  dirty_ = true;
  
  // Save immediately for reliability
  saveToDisk();
}

void AneFfnPersistentCache::remove(const std::string &model_key, uint32_t maxAneUnits) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string key = makeKey(model_key, maxAneUnits);
  cache_.erase(key);
  dirty_ = true;
}

void AneFfnPersistentCache::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  cache_.clear();
  dirty_ = true;
  saveToDisk();
}

std::string AneFfnPersistentCache::cachePath() const {
  return cache_path_;
}

void AneFfnPersistentCache::loadFromDisk() {
  std::ifstream file(cache_path_);
  if (!file.is_open()) {
    return; // No cache file exists yet
  }

  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') continue;
    
    // Format: model_key:maxAneUnits calibration_text
    size_t space_pos = line.find(' ');
    if (space_pos == std::string::npos) continue;
    
    std::string key = line.substr(0, space_pos);
    std::string cal_text = line.substr(space_pos + 1);
    
    if (auto calibration = ops::ane_ffn::Calibration::parse(cal_text)) {
      cache_[key] = *calibration;
    }
  }
}

void AneFfnPersistentCache::saveToDisk() {
  std::ofstream file(cache_path_, std::ios::trunc);
  if (!file.is_open()) {
    logWarning("Failed to save ANE calibration cache to ", cache_path_);
    return;
  }

  file << "# Splash ANE FFN Calibration Cache\n";
  file << "# Format: model_key:maxAneUnits calibration_data\n";
  
  for (const auto &[key, calibration] : cache_) {
    file << key << " " << calibration.text() << "\n";
  }
  
  dirty_ = false;
}

// Global cache instance
AneFfnPersistentCache& aneFfnPersistentCache() {
  static AneFfnPersistentCache instance;
  return instance;
}

} // namespace splash::engine
