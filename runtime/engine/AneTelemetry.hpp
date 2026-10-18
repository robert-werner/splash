#pragma once

#include "AwakeClock.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace splash::engine {

// ANE utilization statistics for a sampling period.
struct AneTelemetrySnapshot {
  // ANE utilization percentage (0.0-100.0).
  double utilizationPercent = 0.0;
  // Number of ANE-accelerated FFN layers active in this period.
  uint32_t activeAneLayers = 0;
  // Total FFN layers in the model.
  uint32_t totalLayers = 0;
  // Number of inferences/evaluations processed during this period.
  uint32_t evaluationCount = 0;
  // Total ANE execution time in milliseconds during this period.
  double aneTimeMs = 0.0;
  // Total command execution time in milliseconds during this period.
  double totalTimeMs = 0.0;
  // Timestamp when this snapshot was captured.
  AwakeClock::time_point timestamp;
};

// Callback invoked when a new telemetry snapshot is available.
// Called from the telemetry background thread approximately every second.
using AneTelemetryCallback = std::function<void(const AneTelemetrySnapshot &)>;

// Records ANE execution timing information to compute utilization.
class AneTelemetry final {
public:
  // `activeAneLayers`: count of FFN layers configured to run on ANE
  // (aneLayerIndices size).
  // `totalLayers`: total FFN layer count in the model.
  // `samplingIntervalSeconds`: how often to compute and report utilization
  // (default: 1.0s).
  AneTelemetry(uint32_t activeAneLayers, uint32_t totalLayers,
               double samplingIntervalSeconds = 1.0);

  AneTelemetry(const AneTelemetry &) = delete;
  AneTelemetry &operator=(const AneTelemetry &) = delete;
  ~AneTelemetry();

  // Register a callback to receive telemetry snapshots. Only one callback
  // is supported; calling again replaces the previous callback.
  void setCallback(AneTelemetryCallback callback);

  // Record the start of a command that includes ANE evaluations.
  // Returns a token to pass to recordCommandEnd.
  [[nodiscard]] uint64_t recordCommandStart();

  // Record the completion of a command with ANE evaluations.
  // `token`: value returned by recordCommandStart.
  // `evaluationCount`: number of ANE FFN evaluations in this command.
  // `aneTimeMs`: measured ANE execution time in milliseconds (from
  // ane::Handoff timing).
  void recordCommandEnd(uint64_t token, uint32_t evaluationCount,
                        double aneTimeMs);

  // Retrieve the most recent telemetry snapshot without waiting.
  // Returns nullopt if no data has been collected yet.
  [[nodiscard]] std::optional<AneTelemetrySnapshot> currentSnapshot() const;

  // Stop the background sampling thread (idempotent).
  void stop();

private:
  struct CommandRecord {
    AwakeClock::time_point startTime;
    AwakeClock::time_point endTime;
    uint32_t evaluationCount;
    double aneTimeMs;
  };

  void samplingLoop();
  AneTelemetrySnapshot computeSnapshot(AwakeClock::time_point now);

  uint32_t activeAneLayers_;
  uint32_t totalLayers_;
  std::chrono::duration<double> samplingInterval_;

  mutable std::mutex mutex_;
  std::vector<CommandRecord> pendingRecords_;
  std::vector<CommandRecord> processedRecords_;
  std::atomic<uint64_t> nextToken_{1};

  std::thread samplingThread_;
  std::atomic<bool> running_{true};

  mutable std::mutex snapshotMutex_;
  std::optional<AneTelemetrySnapshot> lastSnapshot_;

  AneTelemetryCallback callback_;
};

} // namespace splash::engine
