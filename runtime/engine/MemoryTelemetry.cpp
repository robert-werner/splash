#include "engine/MemoryTelemetry.hpp"
#include <sstream>
#include <iomanip>

namespace splash {

std::string MemoryTelemetry::report() const {
  std::ostringstream oss;
  
  oss << "Memory: KV " << (kvCacheUsed / 1024 / 1024) << "/" 
      << (kvCacheCapacity / 1024 / 1024) << " MB";
  
  oss << " (" << std::fixed << std::setprecision(1) 
      << (getKvUtilization() * 100) << "% used";
  
  if (fragmentation > 0.01f) {
    oss << ", " << (fragmentation * 100) << "% fragmented";
  }
  
  oss << ")";
  
  if (aneBytesAllocated > 0) {
    oss << ", ANE " << (aneBytesAllocated / 1024 / 1024) << " MB";
  }
  
  if (ssdCacheBytes > 0) {
    oss << ", SSD " << (ssdCacheBytes / 1024 / 1024 / 1024) << " GB";
  }
  
  if (peakMemoryUsed > 0) {
    oss << ", Peak " << (peakMemoryUsed / 1024 / 1024) << " MB";
  }
  
  return oss.str();
}

} // namespace splash
