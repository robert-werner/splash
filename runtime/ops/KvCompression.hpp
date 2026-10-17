#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace splash::kv {

// Lossless RLE compression for KV cache pages with repeated patterns.
// Targets 10-15% memory savings on typical workloads by detecting runs
// of identical bytes in quantized KV data. Operates on page granularity
// to maintain zero-copy semantics for uncompressed pages.
//
// Compression format (little-endian):
//   [magic: 4B] [version: 4B] [run_count: 4B] [run_table] [data]
//   run_table: array of (offset: 4B, length: 2B, value: 1B)
//   data: non-run bytes, verbatim
//
// Compression threshold: 12% savings minimum to avoid overhead.
class KvCompression final {
public:
  static constexpr uint32_t kMagic = 0x534B5643; // 'SKVC'
  static constexpr uint32_t kVersion = 1;
  static constexpr size_t kMinRunLength = 8; // Minimum run to encode
  static constexpr double kCompressionThreshold = 0.12; // 12% savings minimum

  struct CompressionStats {
    size_t originalBytes = 0;
    size_t compressedBytes = 0;
    uint32_t runCount = 0;
    double compressionRatio = 1.0;
    bool compressed = false;
  };

  // Compress a page buffer using RLE. Returns compressed data only if
  // compression achieves at least kCompressionThreshold savings;
  // otherwise returns empty vector (caller uses original uncompressed).
  // Thread-safe, no internal state.
  [[nodiscard]] static std::vector<std::byte>
  compress(std::span<const std::byte> source, CompressionStats &stats) noexcept;

  // Decompress RLE-encoded data into destination buffer.
  // Returns false on format error or destination too small.
  // Thread-safe, no internal state.
  [[nodiscard]] static bool
  decompress(std::span<const std::byte> compressed,
             std::span<std::byte> destination) noexcept;

  // Validate compressed buffer format without decompression.
  [[nodiscard]] static bool
  validate(std::span<const std::byte> compressed) noexcept;

  // Estimate compressed size without performing compression.
  // Conservative upper bound for allocation planning.
  [[nodiscard]] static size_t
  estimateCompressedSize(size_t originalSize) noexcept {
    // Worst case: no runs, header + run_count + original data
    return 12 + originalSize;
  }

private:
  struct Run {
    uint32_t offset;
    uint16_t length;
    std::byte value;
  };

  // Scan for runs of identical bytes >= kMinRunLength
  [[nodiscard]] static std::vector<Run>
  findRuns(std::span<const std::byte> data) noexcept;

  // Write compressed format
  static void writeCompressed(std::span<const std::byte> source,
                             const std::vector<Run> &runs,
                             std::vector<std::byte> &output) noexcept;
};

} // namespace splash::kv
