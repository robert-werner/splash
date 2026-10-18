#include "engine/AneTelemetry.hpp"
#include "StderrLine.hpp"

#include <algorithm>
#include <iomanip>
#include <numeric>

namespace splash::engine {

AneTelemetry::AneTelemetry(uint32_t activeAneLayers, uint32_t totalLayers,
                           double samplingIntervalSeconds)
    : activeAneLayers_(activeAneLayers), totalLayers_(totalLayers),
      samplingInterval_(samplingIntervalSeconds) {
  samplingThread_ = std::thread([this] { samplingLoop(); });
}

AneTelemetry::~AneTelemetry() { stop(); }

void AneTelemetry::setCallback(AneTelemetryCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  callback_ = std::move(callback);
}

uint64_t AneTelemetry::recordCommandStart() {
  return nextToken_.fetch_add(1, std::memory_order_relaxed);
}

void AneTelemetry::recordCommandEnd(uint64_t /*token*/, uint32_t evaluationCount,
                                    double aneTimeMs) {
  CommandRecord record;
  record.startTime = AwakeClock::now();
  record.endTime = record.startTime;
  record.evaluationCount = evaluationCount;
  record.aneTimeMs = aneTimeMs;

  std::lock_guard<std::mutex> lock(mutex_);
  pendingRecords_.push_back(record);
}

std::optional<AneTelemetrySnapshot> AneTelemetry::currentSnapshot() const {
  std::lock_guard<std::mutex> lock(snapshotMutex_);
  return lastSnapshot_;
}

void AneTelemetry::stop() {
  if (running_.exchange(false, std::memory_order_relaxed)) {
    if (samplingThread_.joinable()) {
      samplingThread_.join();
    }
  }
}

void AneTelemetry::samplingLoop() {
  auto nextSample = AwakeClock::now() + samplingInterval_;

  while (running_.load(std::memory_order_relaxed)) {
    const auto now = AwakeClock::now();
    if (now >= nextSample) {
      AneTelemetrySnapshot snapshot = computeSnapshot(now);

      {
        std::lock_guard<std::mutex> lock(snapshotMutex_);
        lastSnapshot_ = snapshot;
      }

      AneTelemetryCallback callback;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = callback_;
      }

      if (callback) {
        callback(snapshot);
      }

      nextSample = now + samplingInterval_;
    }

    // Sleep briefly to avoid spinning; wake at least once per sampling
    // interval even if no work arrives.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

AneTelemetrySnapshot
AneTelemetry::computeSnapshot(AwakeClock::time_point now) {
  std::vector<CommandRecord> records;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    records = std::exchange(pendingRecords_, {});
  }

  AneTelemetrySnapshot snapshot;
  snapshot.timestamp = now;
  snapshot.activeAneLayers = activeAneLayers_;
  snapshot.totalLayers = totalLayers_;

  if (records.empty()) {
    return snapshot;
  }

  // Accumulate ANE time and evaluation counts from all commands completed
  // during this sampling period.
  double totalAneTimeMs = 0.0;
  uint32_t totalEvaluations = 0;

  for (const CommandRecord &record : records) {
    totalAneTimeMs += record.aneTimeMs;
    totalEvaluations += record.evaluationCount;
  }

  snapshot.aneTimeMs = totalAneTimeMs;
  snapshot.evaluationCount = totalEvaluations;

  // Compute utilization as (ANE time / sampling interval window) * 100.
  // This gives a percentage of the sampling period spent executing on ANE.
  const double windowMs =
      std::chrono::duration<double, std::milli>(samplingInterval_).count();
  snapshot.totalTimeMs = windowMs;

  if (windowMs > 0.0) {
    snapshot.utilizationPercent =
        std::min(100.0, (totalAneTimeMs / windowMs) * 100.0);
  }

  return snapshot;
}

void logAneTelemetry(const AneTelemetrySnapshot &snapshot) {
  logLine("ANE utilization: ", std::fixed, std::setprecision(1),
          snapshot.utilizationPercent, "% (", snapshot.activeAneLayers, "/",
          snapshot.totalLayers, " FFN layers active, ", snapshot.evaluationCount,
          " evaluations, ", std::setprecision(2), snapshot.aneTimeMs,
          " ms ANE time)");
}

} // namespace splash::engine
