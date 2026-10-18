#include "engine/NativeArguments.hpp"

#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace splash::engine {
namespace {

template <typename T>
bool parsePositive(std::string_view value, T &result) {
  const char *end = value.data() + value.size();
  auto parsed = std::from_chars(value.data(), end, result);
  return parsed.ec == std::errc{} && parsed.ptr == end && result != 0;
}

template <typename T>
bool parseNonNegative(std::string_view value, T &result) {
  const char *end = value.data() + value.size();
  auto parsed = std::from_chars(value.data(), end, result);
  return parsed.ec == std::errc{} && parsed.ptr == end;
}

uint64_t parseMaxMemory(std::string_view value) {
  if (value == "auto")
    return 0;
  uint64_t result = 0;
  if (!parsePositive(value, result))
    throw UsageError("MAX_MEMORY_BYTES must be auto or a positive integer");
  return result;
}

// Whether `value` is a context of 1 to the model's own tokens, which
// `result` then holds.
bool parseContext(std::string_view value,
                  const model::ModelCapabilities &capabilities, uint32_t &result) {
  return parsePositive(value, result) &&
         result <= capabilities.maximumContextTokens;
}

uint32_t parseMaxContext(std::string_view value,
                         const model::ModelCapabilities &capabilities) {
  if (value == "auto")
    return 0;
  uint32_t result = 0;
  if (!parseContext(value, capabilities, result)) {
    throw UsageError("MAX_CONTEXT must be auto or an integer in [1, " +
                     std::to_string(capabilities.maximumContextTokens) + "]");
  }
  return result;
}

uint32_t parseMaxImagePatches(std::string_view value) {
  uint32_t result = 0;
  if (!parsePositive(value, result) || result % 4 ||
      result > ops::kMaximumImagePatches)
    throw UsageError("--max-image-patches requires a positive multiple of 4 "
                     "up to " + std::to_string(ops::kMaximumImagePatches));
  return result;
}

bool parseFinite(std::string_view value, double &result) {
  const char *end = value.data() + value.size();
  auto parsed = std::from_chars(value.data(), end, result);
  return parsed.ec == std::errc{} && parsed.ptr == end && std::isfinite(result);
}

double parseIdleRelease(std::string_view value) {
  if (value == "off")
    return std::numeric_limits<double>::infinity();
  double result = 0.0;
  if (!parseFinite(value, result) || result <= 0.0)
    throw UsageError("--idle-release requires off or a positive number of seconds");
  return result;
}

double parseDecodeShare(std::string_view value) {
  double result = 0.0;
  if (!parseFinite(value, result) || result < 0.0)
    throw UsageError("--decode-share requires a nonnegative number");
  return result;
}

double parseTargetGpuUtilization(std::string_view value) {
  double result = 0.0;
  if (!parseFinite(value, result) || result <= 0.0 || result > 1.0)
    throw UsageError("--target-gpu-utilization requires a number in (0.0, 1.0]");
  return result;
}

uint32_t parseMaxBatchSize(std::string_view value) {
  uint32_t result = 0;
  if (!parsePositive(value, result) || result < 1 || result > 16)
    throw UsageError("--max-batch-size requires an integer in [1, 16]");
  return result;
}

uint32_t parseContinuousAttentionThreshold(std::string_view value) {
  uint32_t result = 0;
  if (!parsePositive(value, result))
    throw UsageError("--continuous-attention-threshold requires a positive integer");
  return result;
}

std::filesystem::path requireModelRoot(std::string_view argument) {
  std::error_code error;
  const std::filesystem::path root =
      std::filesystem::canonical(std::filesystem::path(argument), error);
  if (error || !std::filesystem::is_directory(root, error))
    throw UsageError("MODEL_DIRECTORY must name an existing directory");
  for (const char *role : {"target", "draft"}) {
    if (!std::filesystem::is_directory(root / role, error))
      throw UsageError(
          "MODEL_DIRECTORY must hold the model's target/ and draft/ directories");
  }
  return root;
}

} // namespace

NativeArguments parseNativeArguments(int argc, const char *const *argv) {
  if (argc < 5 || std::string_view(argv[1]) != "serve-native") {
    throw UsageError("expected the serve-native command");
  }
  NativeArguments result;
  // Read once the model's own context is known.
  std::optional<std::string_view> relaunchContext;
  int next = 5;
  if (next < argc && !std::string_view(argv[next]).starts_with("--")) {
    const std::string_view quota(argv[next++]);
    result.maxCacheDisk = {};
    if (quota != "0" && !parsePositive(quota, result.maxCacheDisk.bytes))
      throw UsageError("MAX_CACHE_DISK_BYTES must be a nonnegative integer");
  }
  // Options follow as --name value pairs; a missing value fails its check.
  for (; next < argc; next += 2) {
    const std::string_view option(argv[next]);
    const std::string_view value(next + 1 < argc ? argv[next + 1] : "");
    if (option == "--kv-format") {
      if (value != "int4" && value != "int8" && value != "bf16")
        throw UsageError("--kv-format requires int4, int8 or bf16");
      result.kvFormat = value == "int4"   ? kv::Format::Int4
                        : value == "int8" ? kv::Format::Int8
                                          : kv::Format::BFloat16;
    } else if (option == "--state-format") {
      if (value != "fp32" && value != "bf16")
        throw UsageError("--state-format requires fp32 or bf16");
      result.stateFormat =
          value == "fp32" ? ops::GdnStateFormat::Float32 : ops::GdnStateFormat::BFloat16;
    } else if (option == "--draft-format") {
      if (value != "int4" && value != "fp32")
        throw UsageError("--draft-format requires int4 or fp32");
      result.draftFormat =
          value == "int4" ? NativeArguments::DraftFormat::Int4 
                          : NativeArguments::DraftFormat::Float32;
    } else if (option == "--decode-share") {
      result.decodeShare = parseDecodeShare(value);
    } else if (option == "--max-image-patches") {
      result.maxImagePatches = parseMaxImagePatches(value);
    } else if (option == "--persistent-cache") {
      if (value != "on" && value != "off")
        throw UsageError("--persistent-cache requires on or off");
      result.persistentCache = value == "on";
    } else if (option == "--max-cache-disk-writes") {
      if (value != "0" && !parsePositive(value, result.maxCacheDiskWritesBytes))
        throw UsageError("--max-cache-disk-writes requires a nonnegative integer byte count");
    } else if (option == "--cache-dir") {
      if (value.empty())
        throw UsageError("--cache-dir requires a directory");
      result.cacheRoot = std::filesystem::absolute(std::filesystem::path(value));
    } else if (option == "--idle-release") {
      result.idleReleaseSeconds = parseIdleRelease(value);
    } else if (option == "--idle-sleep") {
      if (value != "prevent" && value != "allow")
        throw UsageError("--idle-sleep requires prevent or allow");
      result.preventIdleSleep = value == "prevent";
    } else if (option == "--ane") {
      if (value != "on" && value != "off")
        throw UsageError("--ane requires on or off");
      result.neuralEngine = value == "on";
    } else if (option == "--max-batch-size") {
      result.maxBatchSize = parseMaxBatchSize(value);
    } else if (option == "--dynamic-batching") {
      if (value != "on" && value != "off")
        throw UsageError("--dynamic-batching requires on or off");
      result.dynamicBatching = value == "on";
    } else if (option == "--target-gpu-utilization") {
      result.targetGpuUtilization = parseTargetGpuUtilization(value);
    } else if (option == "--continuous-attention-threshold") {
      result.continuousAttentionThreshold = parseContinuousAttentionThreshold(value);
    } else if (option == "--lazy-ane-allocation") {
      if (value != "on" && value != "off")
        throw UsageError("--lazy-ane-allocation requires on or off");
      result.lazyAneAllocation = value == "on";
    } else if (option == "--working-set") {
      uint64_t bytes = 0;
      if (!parsePositive(value, bytes))
        throw UsageError("--working-set requires a positive integer of bytes");
      result.workingSetBytes = bytes;
    } else if (option == "--kv-defragmentation") {
      if (value != "on" && value != "off")
        throw UsageError("--kv-defragmentation requires on or off");
      result.kvDefragmentation = value == "on";
    } else if (option == "--vision-text-only") {
      if (value != "on" && value != "off")
        throw UsageError("--vision-text-only requires on or off");
      result.visionTextOnly = value == "on";
    } else if (option == "--recalibrate-ane") {
      if (value != "on" && value != "off")
        throw UsageError("--recalibrate-ane requires on or off");
      result.recalibrateAne = value == "on";
    } else if (option == "--lazy-draft-loading") {
      if (value != "on" && value != "off")
        throw UsageError("--lazy-draft-loading requires on or off");
      result.lazyDraftLoading = value == "on";
    } else if (option == "--ssd-kv") {
      if (value != "on" && value != "off")
        throw UsageError("--ssd-kv requires on or off");
      result.ssdKvEnabled = value == "on";
    } else if (option == "--ssd-kv-path") {
      if (value.empty())
        throw UsageError("--ssd-kv-path requires a directory path");
      result.ssdKvPath = std::string(value);
    } else if (option == "--hot-tier-tokens") {
      if (!parsePositive(value, result.hotTierTokens))
        throw UsageError("--hot-tier-tokens requires a positive integer");
    } else if (option == "--priority-scheduling") {
      if (value != "on" && value != "off")
        throw UsageError("--priority-scheduling requires on or off");
      result.priorityScheduling = value == "on";
    } else if (option == "--preempt-threshold") {
      uint32_t threshold = 0;
      if (!parsePositive(value, threshold) || threshold == 0)
        throw UsageError("--preempt-threshold requires a positive integer");
      result.preemptThreshold = threshold;
    } else if (option == "--max-context-extended") {
      if (!parsePositive(value, result.maxContextExtended))
        throw UsageError("--max-context-extended requires a positive integer");
    } else if (option == "--prefetch-pages") {
      if (!parsePositive(value, result.prefetchPages))
        throw UsageError("--prefetch-pages requires a positive integer");
    } else if (option == "--kv-compression") {
      if (value != "on" && value != "off")
        throw UsageError("--kv-compression requires on or off");
      result.kvCompression = value == "on";
    } else if (option == "--adaptive-batching") {
      if (value != "on" && value != "off")
        throw UsageError("--adaptive-batching requires on or off");
      result.adaptiveBatching = value == "on";
    } else if (option == "--prefill-chunk-size") {
      if (!parsePositive(value, result.prefillChunkSize) || result.prefillChunkSize < 512)
        throw UsageError("--prefill-chunk-size requires a positive integer >= 512");
    } else if (option == "--fused-attention") {
      if (value != "on" && value != "off")
        throw UsageError("--fused-attention requires on or off");
      result.fusedAttention = value == "on";
    } else if (option == "--persistent-ane-calibration") {
      if (value != "on" && value != "off")
        throw UsageError("--persistent-ane-calibration requires on or off");
      result.persistentAneCalibration = value == "on";
    } else if (option == "--speculation-threshold") {
      double threshold = 0.0;
      if (!parseFinite(value, threshold) || threshold < 0.1 || threshold > 0.5)
        throw UsageError("--speculation-threshold requires a number in [0.1, 0.5]");
      result.speculationThreshold = static_cast<float>(threshold);
    } else if (option == "--kv-swap-enable") {
      if (value != "on" && value != "off")
        throw UsageError("--kv-swap-enable requires on or off");
      result.kvSwapEnable = value == "on";
    } else if (option == "--kv-swap-ssd-path") {
      if (value.empty())
        throw UsageError("--kv-swap-ssd-path requires a directory path");
      result.kvSwapSsdPath = std::filesystem::absolute(std::filesystem::path(value));
    } else if (option == "--kv-swap-threshold") {
      double threshold = 0.0;
      if (!parseFinite(value, threshold) || threshold <= 0.0 || threshold >= 1.0)
        throw UsageError("--kv-swap-threshold requires a number in (0.0, 1.0)");
      result.kvSwapThreshold = threshold;
    } else if (option == "--kv-swap-hot-tier") {
      if (!parsePositive(value, result.kvSwapHotTier))
        throw UsageError("--kv-swap-hot-tier requires a positive integer");
    } else if (option == "--kv-swap-batch-size") {
      if (!parsePositive(value, result.kvSwapBatchSize) || result.kvSwapBatchSize < 1 || result.kvSwapBatchSize > 256)
        throw UsageError("--kv-swap-batch-size requires an integer in [1, 256]");
    } else if (option == "--ane-layers") {
      if (value.empty())
        throw UsageError("--ane-layers requires comma-separated layer indices");
      // Parse comma-separated indices
      std::string indexStr;
      for (char c : value) {
        if (c == ',') {
          if (!indexStr.empty()) {
            uint32_t idx = 0;
            if (!parseNonNegative(std::string_view(indexStr), idx))
              throw UsageError("--ane-layers: invalid layer index");
            result.aneLayerIndices.push_back(idx);
            indexStr.clear();
          }
        } else if (std::isdigit(c)) {
          indexStr += c;
        } else {
          throw UsageError("--ane-layers: invalid format (expected comma-separated integers)");
        }
      }
      if (!indexStr.empty()) {
        uint32_t idx = 0;
        if (!parseNonNegative(std::string_view(indexStr), idx))
          throw UsageError("--ane-layers: invalid layer index");
        result.aneLayerIndices.push_back(idx);
      }
      // Sort and validate
      std::sort(result.aneLayerIndices.begin(), result.aneLayerIndices.end());
      // Remove duplicates
      result.aneLayerIndices.erase(
        std::unique(result.aneLayerIndices.begin(), result.aneLayerIndices.end()),
        result.aneLayerIndices.end());
    } else if (option == "--enable-weight-prefetch") {
      if (value != "on" && value != "off")
        throw UsageError("--enable-weight-prefetch requires on or off");
      result.enableWeightPrefetch = value == "on";
    } else if (option == "--prefetch-layers") {
      if (!parsePositive(value, result.prefetchLayers))
        throw UsageError("--prefetch-layers requires a positive integer");
    } else if (option == "--relaunch-context") {
      relaunchContext = value;
    } else {
      throw UsageError("unexpected argument " + std::string(option));
    }
  }
  if (result.persistentCache && !result.maxCacheDisk.automatic && !result.maxCacheDisk.bytes)
    throw UsageError("--persistent-cache requires an SSD cache, which MAX_CACHE_DISK_BYTES 0 turns off");
  if (result.maxCacheDiskWritesBytes && !result.maxCacheDisk.automatic && !result.maxCacheDisk.bytes)
    throw UsageError("--max-cache-disk-writes requires an SSD cache, which MAX_CACHE_DISK_BYTES 0 turns off");
  if (!result.cacheRoot.empty() && !result.persistentCache)
    throw UsageError("--cache-dir requires --persistent-cache on");
  if (result.ssdKvEnabled && result.ssdKvPath.empty())
    throw UsageError("--ssd-kv on requires --ssd-kv-path PATH");
  result.modelRoot = requireModelRoot(argv[2]);
  result.model = model::inspectModelRoot(result.modelRoot);
  result.maxContext = parseMaxContext(argv[3], result.model.capabilities);
  if (relaunchContext) {
    if (result.maxContext)
      throw UsageError("--relaunch-context requires MAX_CONTEXT auto");
    if (!parseContext(*relaunchContext, result.model.capabilities, result.relaunchContext))
      throw UsageError("--relaunch-context requires an integer in [1, " +
                       std::to_string(result.model.capabilities.maximumContextTokens) + "]");
  }
  result.maxMemoryBytes = parseMaxMemory(argv[4]);
  return result;
}

} // namespace splash::engine
