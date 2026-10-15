#pragma once

#include <cstddef>
#include <string>

namespace splash {

// Real-time memory usage telemetry for production monitoring
struct MemoryTelemetry {
  size_t kvCacheUsed = 0;
  size_t kvCacheCapacity = 0;
  float fragmentation = 0.0f;
  size_t aneBytesAllocated = 0;
  size_t ssdCacheBytes = 0;
  size_t peakMemoryUsed = 0;
  
  float getKvUtilization() const {
    return kvCacheCapacity > 0 
      ? static_cast<float>(kvCacheUsed) / kvCacheCapacity 
      : 0.0f;
  }
  
  std::string report() const;
};

} // namespace splash
