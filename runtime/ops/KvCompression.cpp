#include "ops/KvCompression.hpp"

#include <algorithm>
#include <cstring>

namespace splash::kv {

std::vector<KvCompression::Run>
KvCompression::findRuns(std::span<const std::byte> data) noexcept {
  std::vector<Run> runs;
  if (data.size() < kMinRunLength)
    return runs;

  size_t i = 0;
  while (i < data.size()) {
    const std::byte value = data[i];
    size_t runLength = 1;

    // Scan for consecutive identical bytes
    while (i + runLength < data.size() && data[i + runLength] == value) {
      ++runLength;
      if (runLength == 0xFFFF) // uint16_t max
        break;
    }

    // Record run if it meets minimum length
    if (runLength >= kMinRunLength) {
      runs.push_back({static_cast<uint32_t>(i),
                      static_cast<uint16_t>(runLength),
                      value});
    }

    i += runLength;
  }

  return runs;
}

void KvCompression::writeCompressed(std::span<const std::byte> source,
                                   const std::vector<Run> &runs,
                                   std::vector<std::byte> &output) noexcept {
  // Reserve pessimistic upper bound
  output.reserve(12 + runs.size() * 7 + source.size());

  // Write header: magic (4B) + version (4B)
  const uint32_t magic = kMagic;
  const uint32_t version = kVersion;
  output.insert(output.end(),
                reinterpret_cast<const std::byte*>(&magic),
                reinterpret_cast<const std::byte*>(&magic) + 4);
  output.insert(output.end(),
                reinterpret_cast<const std::byte*>(&version),
                reinterpret_cast<const std::byte*>(&version) + 4);

  // Write run count
  const uint32_t runCount = static_cast<uint32_t>(runs.size());
  output.insert(output.end(),
                reinterpret_cast<const std::byte*>(&runCount),
                reinterpret_cast<const std::byte*>(&runCount) + 4);

  // Write run table: (offset: 4B, length: 2B, value: 1B) per run
  for (const auto &run : runs) {
    output.insert(output.end(),
                  reinterpret_cast<const std::byte*>(&run.offset),
                  reinterpret_cast<const std::byte*>(&run.offset) + 4);
    output.insert(output.end(),
                  reinterpret_cast<const std::byte*>(&run.length),
                  reinterpret_cast<const std::byte*>(&run.length) + 2);
    output.push_back(run.value);
  }

  // Write non-run data verbatim
  size_t sourcePos = 0;
  for (const auto &run : runs) {
    // Copy gap before this run
    if (run.offset > sourcePos) {
      output.insert(output.end(),
                    source.begin() + sourcePos,
                    source.begin() + run.offset);
    }
    sourcePos = run.offset + run.length;
  }

  // Copy remaining data after last run
  if (sourcePos < source.size()) {
    output.insert(output.end(),
                  source.begin() + sourcePos,
                  source.end());
  }
}

std::vector<std::byte>
KvCompression::compress(std::span<const std::byte> source,
                       CompressionStats &stats) noexcept {
  stats.originalBytes = source.size();
  stats.compressedBytes = 0;
  stats.runCount = 0;
  stats.compressionRatio = 1.0;
  stats.compressed = false;

  if (source.empty())
    return {};

  // Find runs
  const auto runs = findRuns(source);
  if (runs.empty())
    return {}; // No compression benefit

  // Calculate compressed size
  size_t compressedSize = 12; // header
  compressedSize += runs.size() * 7; // run table entries
  size_t coveredBytes = 0;
  for (const auto &run : runs) {
    coveredBytes += run.length;
  }
  compressedSize += (source.size() - coveredBytes); // non-run data

  // Check compression threshold
  const double ratio = static_cast<double>(compressedSize) / source.size();
  stats.compressionRatio = ratio;
  stats.runCount = static_cast<uint32_t>(runs.size());

  if (ratio > (1.0 - kCompressionThreshold)) {
    // Compression savings insufficient
    return {};
  }

  // Perform compression
  std::vector<std::byte> compressed;
  writeCompressed(source, runs, compressed);

  stats.compressedBytes = compressed.size();
  stats.compressed = true;
  return compressed;
}

bool KvCompression::validate(std::span<const std::byte> compressed) noexcept {
  if (compressed.size() < 12)
    return false;

  // Check magic
  uint32_t magic = 0;
  std::memcpy(&magic, compressed.data(), 4);
  if (magic != kMagic)
    return false;

  // Check version
  uint32_t version = 0;
  std::memcpy(&version, compressed.data() + 4, 4);
  if (version != kVersion)
    return false;

  // Check run count
  uint32_t runCount = 0;
  std::memcpy(&runCount, compressed.data() + 8, 4);

  // Validate size is sufficient for run table
  const size_t expectedHeaderSize = 12 + runCount * 7;
  if (compressed.size() < expectedHeaderSize)
    return false;

  return true;
}

bool KvCompression::decompress(std::span<const std::byte> compressed,
                               std::span<std::byte> destination) noexcept {
  if (!validate(compressed))
    return false;

  // Read run count
  uint32_t runCount = 0;
  std::memcpy(&runCount, compressed.data() + 8, 4);

  // Parse run table
  std::vector<Run> runs;
  runs.reserve(runCount);
  size_t pos = 12;

  for (uint32_t i = 0; i < runCount; ++i) {
    if (pos + 7 > compressed.size())
      return false;

    Run run;
    std::memcpy(&run.offset, compressed.data() + pos, 4);
    pos += 4;
    std::memcpy(&run.length, compressed.data() + pos, 2);
    pos += 2;
    run.value = compressed[pos];
    pos += 1;

    runs.push_back(run);
  }

  // Decompress data
  size_t destPos = 0;
  size_t compressedDataPos = pos;

  for (const auto &run : runs) {
    // Copy gap before this run
    if (run.offset > destPos) {
      const size_t gapSize = run.offset - destPos;
      if (compressedDataPos + gapSize > compressed.size())
        return false;
      if (destPos + gapSize > destination.size())
        return false;

      std::memcpy(destination.data() + destPos,
                  compressed.data() + compressedDataPos,
                  gapSize);
      destPos += gapSize;
      compressedDataPos += gapSize;
    }

    // Expand run
    if (destPos + run.length > destination.size())
      return false;

    std::memset(destination.data() + destPos,
                static_cast<int>(run.value),
                run.length);
    destPos += run.length;
  }

  // Copy remaining data after last run
  if (compressedDataPos < compressed.size()) {
    const size_t remaining = compressed.size() - compressedDataPos;
    if (destPos + remaining > destination.size())
      return false;

    std::memcpy(destination.data() + destPos,
                compressed.data() + compressedDataPos,
                remaining);
    destPos += remaining;
  }

  return true;
}

} // namespace splash::kv
