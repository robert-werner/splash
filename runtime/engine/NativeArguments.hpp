#pragma once

#include "engine/Engine.hpp"
#include "engine/RuntimeResources.hpp"
#include "model/ModelDescriptor.hpp"

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace splash::engine {

// A command line the native process cannot run.
class UsageError final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

// What the serve-native command line asks for. server/server.py
// `_native_command` writes it, and dev/tests/engine/native_command_golden.txt
// pins the lines both sides agree on.
struct NativeArguments final {
  std::filesystem::path modelRoot;
  model::ModelDescriptor model;
  // Zero for auto.
  uint32_t maxContext = 0;
  // --relaunch-context TOKENS, with MAX_CONTEXT auto: the context the
  // server's first engine served, which the server passes to every relaunch
  // (RuntimeResourcesConfig::relaunchContext); zero for none.
  uint32_t relaunchContext = 0;
  uint64_t maxMemoryBytes = 0;
  // Automatic without MAX_CACHE_DISK_BYTES, as the server's default.
  CacheDiskQuota maxCacheDisk{.automatic = true};
  // --max-cache-disk-writes BYTES: the SSD cache's write budget, the most it
  // writes a day on average; zero for none.
  uint64_t maxCacheDiskWritesBytes = 0;
  // --persistent-cache on|off: whether the cache tier keeps its files for the
  // next process, instead of temporary ones.
  bool persistentCache = false;
  // --cache-dir: where persistent caches live; empty for the default,
  // ~/Library/Caches/Splash/prefix-cache.
  std::filesystem::path cacheRoot;
  kv::Format kvFormat = kv::Format::Int8;
  ops::GdnStateFormat stateFormat = ops::GdnStateFormat::Float32;
  // --draft-format int4|fp32: draft model quantization format.
  enum class DraftFormat : uint8_t { Float32, Int4 };
  DraftFormat draftFormat = DraftFormat::Float32;
  double decodeShare = EngineConfig{}.decodeShare;
  // --max-batch-size N: maximum concurrent decode lanes (1-16).
  uint32_t maxBatchSize = EngineConfig{}.maxBatchSize;
  // --dynamic-batching on|off: enable adaptive batch size adjustment.
  bool dynamicBatching = EngineConfig{}.dynamicBatching;
  // --target-gpu-utilization N: target GPU utilization for dynamic batching (0.0-1.0).
  double targetGpuUtilization = EngineConfig{}.targetGpuUtilization;
  // --priority-scheduling on|off: enable context-aware request prioritization.
  bool priorityScheduling = EngineConfig{}.priorityScheduling;
  // --preempt-threshold N: token count where long prefills yield.
  uint32_t preemptThreshold = EngineConfig{}.preemptThreshold;
  // --continuous-attention-threshold N: token count for continuous prefill.
  uint32_t continuousAttentionThreshold = EngineConfig{}.continuousPrefillThreshold;
  uint32_t maxImagePatches = ops::kMaximumImagePatches;
  // --idle-release SECONDS|off, infinite for off.
  double idleReleaseSeconds = RuntimeResourcesConfig{}.idleReleaseSeconds;
  // --idle-sleep prevent|allow: whether the engine keeps the Mac from sleeping
  // automatically while it holds a request.
  bool preventIdleSleep = true;
  // --ane on|off: whether a dense target's prefill FFN may split with the
  // Neural Engine when that is faster (splash serve --disable-ane turns it
  // off).
  bool neuralEngine = true;
  
  // --working-set, zero when it was not passed.
  uint64_t workingSetBytes = 0;
  // Memory switches the parser read.
  bool lazyAneAllocation = false;
  bool kvDefragmentation = false;
  bool visionTextOnly = false;
  // --recalibrate-ane: forget saved ANE calibration and recalibrate.
  bool recalibrateAne = false;
  // --lazy-draft-loading: defer draft model load until first request.
  bool lazyDraftLoading = false;
  
  // --ssd-kv on|off: enable SSD-backed extended KV cache tier.
  bool ssdKvEnabled = false;
  // --ssd-kv-path PATH: path for SSD KV storage (required when --ssd-kv on).
  std::string ssdKvPath = "";
  // --hot-tier-tokens N: hot tier token capacity in RAM before demotion to SSD.
  uint32_t hotTierTokens = 200 * 1024;
  // --max-context-extended N: maximum extended context tokens (hot + cold tiers).
  // 0 = disabled (uses baseline KvPageTier ≤262K).
  uint32_t maxContextExtended = 0;
  // --prefetch-pages N: prefetch window size for cold tier sequential access.
  uint32_t prefetchPages = 16;
  
  // Week 1 optimizations
  // --kv-compression on|off: enable lossless RLE compression for KV cache (saves 10-15%).
  bool kvCompression = false;
  // --adaptive-batching on|off: enable GPU utilization-based batch size adjustment (95-98% target).
  bool adaptiveBatching = false;
  // --prefill-chunk-size N: maximum tokens per prefill chunk (default 8192).
  uint32_t prefillChunkSize = 8192;
  
  // Week 2 optimizations
  // --fused-attention on|off: enable fused QK^T+softmax+PV kernel (reduces memory traffic 30-40%).
  bool fusedAttention = false;
  // --persistent-ane-calibration on|off: persist ANE calibration across runs (saves 2-3s startup).
  bool persistentAneCalibration = true;
  // --speculation-threshold N: draft acceptance threshold for adaptive depth (0.15-0.30, default 0.25).
  float speculationThreshold = 0.25f;
  
  // Phase 2: ANE Hybrid Quantization
  // --ane-layers "0,2,4,8,10,14": comma-separated FFN layer indices for ANE (Q4_K unrotated).
  // Remaining layers use GPU (PQ2_0 rotated). Empty = disabled (pure quantization).
  std::vector<uint32_t> aneLayerIndices;
  // --ane-telemetry on|off: enable ANE utilization monitoring (default: off).
  bool aneTelemetryEnabled = false;
};

// Reads `serve-native MODEL_DIRECTORY MAX_CONTEXT|auto MAX_MEMORY_BYTES|auto
// [MAX_CACHE_DISK_BYTES] [--name value]...` and inspects the model the
// directory, its model root, holds. Throws UsageError for a command line it
// refuses.
[[nodiscard]] NativeArguments parseNativeArguments(int argc,
                                                   const char *const *argv);

} // namespace splash::engine
