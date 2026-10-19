// The INT4 KV format, end to end on the host: the page geometry the kernels
// and the pool must agree on, the nibble ownership rule its store relies on,
// a byte-level round trip of one page, and the budget arithmetic that gives
// the model's full context.
//
// A KV format that silently degrades to another is the failure this test
// exists to catch: geometry saying 16,896 bytes per token while a kernel
// writes INT8's 33,280 strides corrupts every page it touches, and INT4's
// half-byte elements make an INT8 value layout (dimension-major) pair two
// threadgroups per byte. Every check below therefore compares INT4 against
// the exact INT8 figure it must differ from, not merely against itself.

#include "TestChecks.hpp"
#include "TestKvPool.hpp"

#include "engine/KvPool.hpp"
#include "metal/abi/KvExtent.h"
#include "model/Model.hpp"
#include "ops/PagedAttention.hpp"
#include "ops/PagedKv.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using splash::engine::KvPool;
using splash::kv::Format;
using splash::kv::Layout;
using splash::test::TestKvStorage;
using splash::test::require;

// The model this engine serves: 16 layers, 4 KV heads, 256-wide heads.
constexpr Layout kInt8{16, 4, 256, Format::Int8};
constexpr Layout kInt4{16, 4, 256, Format::Int4};
constexpr uint32_t kHeads = kInt4.kvHeads;
constexpr uint32_t kDimension = kInt4.headDimension;
constexpr uint32_t kTokens = splash::kv::kPageTokens;
// Elements of one tensor of one page: every head, token and dimension.
constexpr uint64_t kElementsPerTensor = uint64_t{kHeads} * kTokens * kDimension;
// Bytes of one tensor of one page at INT4: two elements a byte.
constexpr uint64_t kTensorBytes = kElementsPerTensor / 2;

// --- The shader's index functions, as a host mirror -----------------------
//
// runtime/metal/kernels/common/kv_paging.h's splash_kv_value_index_of sends
// INT4 values through splash_kv_key_index (token-major, dimension innermost);
// every other format uses splash_kv_value_element (dimension-major, token
// innermost). kv_extent.h's SplashKvAddressing then halves the element index
// to reach the byte a two-element byte starts at. This mirror keeps the same
// two choices, so a change to either side shows up here as a disagreement
// with the ABI functions in metal/abi/KvExtent.h.
enum class ValueLayout { TokenMajor, DimensionMajor };

uint64_t valueElementOf(ValueLayout layout, uint32_t page, uint32_t head,
                        uint32_t token, uint32_t dimension) {
  const uint64_t slab = uint64_t{page} * kElementsPerTensor;
  if (layout == ValueLayout::TokenMajor)
    return slab + splash_kv_key_element(head, token, dimension);
  return slab + splash_kv_value_element(head, token, dimension);
}

// The byte that holds `element` of a two-element INT4 tensor: elements 2k and
// 2k + 1 share byte k, the even one in the low nibble.
uint64_t int4ByteOf(uint64_t element) { return element / 2; }

// The threadgroup that writes one row of one tensor of a chunk: prefill's
// splash_store_chunk_phase counts rows of keys, then the same rows of values,
// and a row is one (KV head, token). Decode's store indexes the same rows per
// lane, so a byte two tokens share is a byte two threadgroups share.
uint64_t storeGroupOf(uint32_t head, uint32_t token, uint32_t tokens) {
  return uint64_t{head} * tokens + token;
}

// The lane and simdgroup a dimension is stored by: one thread per dimension.
uint32_t laneOf(uint32_t dimension) { return dimension % 32; }
uint32_t simdgroupOf(uint32_t dimension) { return dimension / 32; }

void testPageGeometry() {
  // The four figures the format is quoted by: bytes per token, the page, the
  // data of one tensor and its scales.
  static_assert(kInt4.valid());
  static_assert(kInt4.bytesPerModelPage() == 16 * 2 * (16'384 + 512));
  static_assert(kInt4.bytesPerModelPage() == 540'672);
  static_assert(kInt4.bytesPerModelPage() / kTokens == 16'896);
  static_assert(kInt8.bytesPerModelPage() / kTokens == 33'280);
  static_assert(kInt4.dataBytesPerLayerPage() == 16'384);
  static_assert(kInt4.scaleBytesPerLayerPage() == 512);
  static_assert(kInt4.elementsPerLayerPage() == kElementsPerTensor);
  static_assert(kInt4.elementsPerScale() == kDimension);
  // INT4 keeps INT8's scales exactly and halves only the data: the two
  // formats differ in nothing else, so any host arithmetic that loses this
  // is a defect rather than a variant.
  static_assert(kInt4.dataBytesPerLayerPage() * 2 == kInt8.dataBytesPerLayerPage());
  static_assert(kInt4.scaleBytesPerLayerPage() == kInt8.scaleBytesPerLayerPage());
  static_assert(kInt4.scalesPerTensorLayerPage() == kInt8.scalesPerTensorLayerPage());
  static_assert(kInt4.elementsPerScale() == kInt8.elementsPerScale());

  // The ABI the kernels and PageStorage::spans share: one page's bytes of
  // each tensor, and where each layer's regions sit in an extent.
  constexpr uint32_t extentPages = kInt4.extentAlignmentPages();
  static_assert(extentPages == 128, "INT4's extents still align every region at 64 KiB");
  const auto data = static_cast<uint32_t>(kInt4.dataBytesPerLayerPage());
  const auto scale = static_cast<uint32_t>(kInt4.scaleBytesPerLayerPage());
  require(splash_kv_page_bytes(data, scale, SPLASH_KV_KEYS) == 16'384,
          "INT4's keys of a page are not half INT8's bytes");
  require(splash_kv_page_bytes(data, scale, SPLASH_KV_VALUES) == 16'384,
          "INT4's values of a page are not half INT8's bytes");
  require(splash_kv_page_bytes(data, scale, SPLASH_KV_KEY_SCALES) == 512,
          "INT4's key scales of a page are not INT8's");
  require(splash_kv_page_bytes(data, scale, SPLASH_KV_VALUE_SCALES) == 512,
          "INT4's value scales of a page are not INT8's");
  // Keys, their scales, values and value scales, in that order, tile each
  // layer's region without a gap: a kernel that wrote INT8 strides would run
  // into the next tensor.
  const std::array<uint64_t, 4> expected{
      0, uint64_t{extentPages} * data, uint64_t{extentPages} * (data + scale),
      uint64_t{extentPages} * (2 * uint64_t{data} + scale)};
  for (uint32_t tensor = SPLASH_KV_KEYS; tensor <= SPLASH_KV_VALUE_SCALES; ++tensor) {
    const uint64_t offset = splash_kv_offset(extentPages, data, scale, 0, tensor, 0);
    require(offset == expected[tensor], "INT4's tensor regions do not follow each other");
    require(offset % 2 == 0, "an INT4 tensor region does not start on a byte");
  }
  const uint64_t layerBytes = 2 * (uint64_t{data} + scale);
  require(splash_kv_offset(extentPages, data, scale, 0, SPLASH_KV_VALUE_SCALES,
                           extentPages) == uint64_t{extentPages} * layerBytes,
          "INT4's four tensor regions do not fill one layer's region");
  const SplashKvLayer layer = splash_kv_layer(extentPages, data, scale, 15);
  require(layer.offset == 15 * extentPages * layerBytes,
          "INT4's sixteenth layer does not start after the fifteen before it");
  require(layer.offset + extentPages * layerBytes ==
              uint64_t{extentPages} * kInt4.bytesPerModelPage(),
          "INT4's layers do not fill one extent");
  // A page of every layer of an extent, at INT4's geometry, is one page of
  // the pool: nothing but the layers' regions are charged to it.
  require(uint64_t{kInt4.attentionLayers} * layerBytes == kInt4.bytesPerModelPage(),
          "INT4's page bytes are not its layers' regions");
  // A pool of fewer pages than one alignment unit has no extent size, which
  // is what PageStorage's "a whole number of alignment units" guard needs.
  require(kInt4.extentPagesFor(extentPages - 1) == 0,
          "an INT4 pool below one alignment unit was sized");
  require(kInt4.minimumExtentPages() == extentPages &&
              kInt4.maximumExtentPages() % extentPages == 0,
          "INT4's extent sizes are not whole alignment units");
}

void testSlabAlignment() {
  // SplashKvAddressing forms a head's slab pointer as region + element / 2
  // for INT4, so every slab must start on an even element: a key row begins
  // at a multiple of the head dimension (256) and a value row, token-major,
  // at a multiple of 256 too, both even.
  for (uint32_t page : {0u, 1u, 7u, 127u}) {
    for (uint32_t head = 0; head < kHeads; ++head) {
      const uint64_t key = valueElementOf(ValueLayout::TokenMajor, page, head, 0, 0);
      require(key % 2 == 0, "an INT4 head's key slab does not start on a byte");
      require(int4ByteOf(key) * 2 == key, "halving an INT4 key slab start lost an element");
      // INT4 values are token-major, the way keys are: the same index, off the
      // page's slab.
      require(valueElementOf(ValueLayout::TokenMajor, page, head, 0, 0) ==
                  uint64_t{page} * kElementsPerTensor +
                      splash_kv_key_element(head, 0, 0),
              "INT4 values are not laid out token-major like keys");
    }
    // The last element of the slab halves exactly: a page's slab of one
    // tensor holds an even number of elements (heads * tokens * dimension).
    const uint64_t last = valueElementOf(ValueLayout::TokenMajor, page, kHeads - 1,
                                         kTokens - 1, kDimension - 1);
    require(last % 2 == 1, "the last element of an INT4 page is not odd");
    require(int4ByteOf(last) < uint64_t{page + 1} * kTensorBytes,
            "an INT4 page's last byte falls outside its data region");
  }
  // The dimension-major layout INT8 uses puts the same element elsewhere, so
  // the two layouts are distinguishable and the checks above have content.
  const uint64_t tokenMajor = valueElementOf(ValueLayout::TokenMajor, 2, 1, 5, 3);
  const uint64_t dimensionMajor = valueElementOf(ValueLayout::DimensionMajor, 2, 1, 5, 3);
  require(tokenMajor != dimensionMajor,
          "the token-major and dimension-major value layouts agree, so nothing is proven");
  // The slab each addressing function starts a head at is even in both
  // layouts: keys at (head * 32 + 0) * 256, values dimension-major at
  // (head * 256 + 0) * 32, values token-major as the keys. A dimension-major
  // value *row* is not: its tokens are consecutive elements, so only a slab
  // start is a safe base for halving - which is the shape SplashKvAddressing
  // uses, and the reason INT4 cannot reuse INT8's per-token addressing.
  for (uint32_t head = 0; head < kHeads; ++head) {
    require(splash_kv_key_element(head, 0, 0) % 2 == 0 &&
                splash_kv_value_element(head, 0, 0) % 2 == 0,
            "a head's slab starts on an odd element");
    require(splash_kv_value_element(head, 1, 0) % 2 == 1,
            "a dimension-major value row's second token is not its next element");
  }
}

void testNibbleOwnership() {
  // One page of one tensor, every element: which byte claims it, and which
  // threadgroup and lane write that byte. A byte claimed by two threadgroups
  // would be a lost update; claimed once or three times, the format is wrong.
  constexpr uint32_t page = 3;
  std::vector<uint8_t> claims(kTensorBytes, 0);
  // The lowest dimension claiming each byte.
  std::vector<uint32_t> writerDimension(kTensorBytes, kDimension);
  for (uint32_t head = 0; head < kHeads; ++head) {
    for (uint32_t token = 0; token < kTokens; ++token) {
      for (uint32_t dimension = 0; dimension < kDimension; ++dimension) {
        const uint64_t element =
            valueElementOf(ValueLayout::TokenMajor, page, head, token, dimension);
        const uint64_t byte = int4ByteOf(element) - uint64_t{page} * kTensorBytes;
        require(byte < claims.size(), "an INT4 element's byte is outside its page");
        // The pair of a byte is one token's dimensions 2k and 2k + 1: same KV
        // head, same token, so the same store threadgroup.
        const uint32_t partner = dimension % 2 == 0 ? dimension + 1 : dimension - 1;
        const uint64_t partnerElement =
            valueElementOf(ValueLayout::TokenMajor, page, head, token, partner);
        require(int4ByteOf(partnerElement) == int4ByteOf(element),
                "the two elements of an INT4 byte are not one token's neighbours");
        // A row is 128 bytes, and the store's threadgroup index is
        // head * chunk_tokens + token: the byte's row must be exactly that, so
        // both of its elements belong to one threadgroup and no row's bytes
        // reach into another's.
        require(byte / 128 == storeGroupOf(head, token, kTokens),
                "an INT4 byte's row is not its store threadgroup");
        // ... and the same simdgroup, one lane apart, the even one lower, so
        // simd_shuffle_down(quantized, 1) at the even lane returns the odd
        // neighbour's value: the even lane writes the whole byte.
        const uint32_t even = dimension - dimension % 2;
        require(simdgroupOf(even) == simdgroupOf(even + 1),
                "an INT4 byte's two elements are in different simdgroups");
        require(laneOf(even + 1) == laneOf(even) + 1,
                "an INT4 byte's odd element is not the lane above its even one");
        require(claims[byte] < 2, "three INT4 elements claim one byte");
        writerDimension[byte] = std::min(writerDimension[byte], dimension);
        ++claims[byte];
      }
    }
  }
  require(std::all_of(claims.begin(), claims.end(),
                      [](uint8_t count) { return count == 2; }),
          "an INT4 byte is not claimed by exactly two elements");
  // The lower element of each byte is its even dimension, which is the lane
  // simd_shuffle_down(q, 1) reads from: the even lane owns the byte.
  require(std::all_of(writerDimension.begin(), writerDimension.end(),
                      [](uint32_t dimension) { return dimension % 2 == 0; }),
          "an INT4 byte's lower element is not its even dimension");

  // The layout INT8 uses would pair two tokens of one dimension instead:
  // different rows, so different threadgroups, with no ordering between them.
  // That is why INT4 may not keep it, and this test fails if it returns.
  uint32_t crossThreadgroupPairs = 0;
  for (uint32_t head = 0; head < kHeads; ++head) {
    for (uint32_t token = 0; token + 1 < kTokens; token += 2) {
      for (uint32_t dimension = 0; dimension < kDimension; ++dimension) {
        const uint64_t element =
            valueElementOf(ValueLayout::DimensionMajor, page, head, token, dimension);
        const uint64_t next = valueElementOf(ValueLayout::DimensionMajor, page, head,
                                             token + 1, dimension);
        if (int4ByteOf(element) == int4ByteOf(next) &&
            storeGroupOf(head, token, kTokens) != storeGroupOf(head, token + 1, kTokens))
          ++crossThreadgroupPairs;
      }
    }
  }
  require(crossThreadgroupPairs == kHeads * (kTokens / 2) * kDimension,
          "the dimension-major layout does not pair every element across threadgroups, "
          "so INT4 could keep it");
}

// The store's quantization of one row (paged_store_row.h): the row's maximum
// magnitude, [-7, 7] codes, the even lane packing its own code low and its
// neighbour's high.
std::vector<uint8_t> storeRow(const std::vector<float> &row) {
  require(row.size() == kDimension, "an INT4 store row is not one head dimension");
  float maximum = 0.0f;
  for (float value : row) maximum = std::max(maximum, std::abs(value));
  std::vector<uint8_t> bytes(kDimension / 2);
  for (uint32_t dimension = 0; dimension < kDimension; dimension += 2) {
    const auto code = [&](uint32_t at) {
      return maximum == 0.0f
                 ? 0
                 : std::clamp(int(std::rint(row[at] * 7.0f / maximum)), -7, 7);
    };
    // Only the even lane writes, taking the odd lane's nibble over the
    // simdgroup rather than reading back a byte another thread may not have
    // written yet.
    bytes[dimension / 2] = uint8_t(uint32_t(code(dimension) & 0xF) |
                                   (uint32_t(code(dimension + 1) & 0xF) << 4));
  }
  return bytes;
}

// The tensor read the attention kernels do: element e of a token-major INT4
// tensor at byte e / 2, the even element in the low nibble, sign-extended
// from four bits (metal::int4b_format's packing).
int readElement(const uint8_t *bytes, uint32_t dimension) {
  const uint8_t byte = bytes[dimension / 2];
  const uint32_t nibble = dimension % 2 == 0 ? (byte & 0x0Fu) : (byte >> 4);
  return int(nibble) - ((nibble & 0x8u) != 0 ? 16 : 0);
}

std::vector<float> rowValues(uint32_t head, uint32_t token) {
  std::vector<float> row(kDimension);
  for (uint32_t dimension = 0; dimension < kDimension; ++dimension)
    row[dimension] = 2.5f * std::sin(float(token * 17 + head * 31 + dimension) * 0.013f);
  return row;
}

void testStoreRoundTrip() {
  // Every head and token of one page, one row at a time, as the store writes
  // it: a row's bytes lie inside the row's own span of the tensor, and the
  // attention read of the same tensor returns the stored codes in order.
  constexpr uint32_t page = 1;
  std::vector<uint8_t> tensor(kTensorBytes, 0);
  std::vector<uint8_t> written(kTensorBytes, 0);
  for (uint32_t head = 0; head < kHeads; ++head) {
    for (uint32_t token = 0; token < kTokens; ++token) {
      const std::vector<uint8_t> bytes = storeRow(rowValues(head, token));
      const uint64_t element =
          valueElementOf(ValueLayout::TokenMajor, page, head, token, 0) -
          uint64_t{page} * kElementsPerTensor;
      const uint64_t first = int4ByteOf(element);
      std::copy(bytes.begin(), bytes.end(), tensor.begin() + first);
      for (uint64_t byte = 0; byte < bytes.size(); ++byte) {
        require(written[first + byte] == 0, "two INT4 rows write one byte");
        written[first + byte] = 1;
      }
      // The next row starts exactly where this one ended: no padding, and no
      // overlap with the row after it.
      require(first + bytes.size() ==
                  int4ByteOf(valueElementOf(ValueLayout::TokenMajor, page, head,
                                            token + 1, 0) -
                             uint64_t{page} * kElementsPerTensor),
              "INT4 rows are not adjacent in a token-major tensor");
    }
  }
  require(std::all_of(written.begin(), written.end(), [](uint8_t flag) { return flag == 1; }),
          "an INT4 page's byte was written by no row");
  require(tensor.size() == 16'384, "an INT4 page of one tensor is not 16,384 bytes");

  // A read of the page, element by element, gives back the codes the store
  // wrote, at the element indices the QK and PV operands use ({dimension,
  // token}, dimension innermost, for both tensors).
  for (uint32_t head = 0; head < kHeads; ++head) {
    for (uint32_t token = 0; token < kTokens; ++token) {
      const std::vector<float> row = rowValues(head, token);
      float maximum = 0.0f;
      for (float value : row) maximum = std::max(maximum, std::abs(value));
      const uint64_t element =
          valueElementOf(ValueLayout::TokenMajor, page, head, token, 0) -
          uint64_t{page} * kElementsPerTensor;
      const uint8_t *stored = tensor.data() + int4ByteOf(element);
      for (uint32_t dimension = 0; dimension < kDimension; ++dimension) {
        const int code = readElement(stored, dimension);
        const int expected = maximum == 0.0f
                                 ? 0
                                 : std::clamp(int(std::rint(row[dimension] * 7.0f / maximum)),
                                              -7, 7);
        require(code == expected, "an INT4 element does not read back as its code");
        const float decoded = float(code) * (maximum / 7.0f);
        require(std::abs(decoded - row[dimension]) <= maximum / 14.0f + 1.0e-6f,
                "an INT4 element is off by more than half its scale");
      }
      // The low nibble is the even dimension: an inverted packing would read
      // the pair's codes swapped, which the check above would already fail.
      const uint32_t low = stored[0] & 0x0Fu;
      require(int(low) - ((low & 0x8u) != 0 ? 16 : 0) == readElement(stored, 0),
              "the low nibble of an INT4 byte is not its even dimension");
    }
  }

  // A row of zeros stores zeros, not a division by zero.
  const std::vector<float> zeros(kDimension, 0.0f);
  const std::vector<uint8_t> zeroBytes = storeRow(zeros);
  require(std::all_of(zeroBytes.begin(), zeroBytes.end(),
                      [](uint8_t byte) { return byte == 0; }),
          "a zero INT4 row did not store zeros");
}

void testPoolGeometry() {
  // Layout -> ExtentStorage -> KvPool: the pool charges exactly the layout's
  // page bytes, so an extent of 128 INT4 pages is 66 MiB, not INT8's 130.
  constexpr uint32_t extentPages = kInt4.extentAlignmentPages();
  constexpr uint32_t pages = 4 * extentPages;
  TestKvStorage storage(pages, kInt4.bytesPerModelPage(), extentPages);
  KvPool pool(storage, extentPages);
  require(pool.bytesPerPage() == kInt4.bytesPerModelPage(),
          "the KV pool does not charge an INT4 page the layout's bytes");
  require(pool.bytesPerPage() == 16'896 * uint64_t{kTokens},
          "the pool's INT4 page is not 16,896 bytes per token");
  const auto acquired = pool.acquirePages(3);
  require(acquired.granted() && acquired.pages.size() == 3,
          "an INT4 pool did not grant pages");
  const auto snapshot = pool.snapshot();
  require(snapshot.allocatedBytes == uint64_t{extentPages} * kInt4.bytesPerModelPage() &&
              snapshot.pagesAllocated == extentPages,
          "an INT4 pool's extent is not 128 pages of the layout");
  // The same page count costs INT4 exactly INT8's bytes less the halved
  // data: the scales are identical.
  const uint64_t int8Extent = uint64_t{extentPages} * kInt8.bytesPerModelPage();
  const uint64_t int4Extent = uint64_t{extentPages} * kInt4.bytesPerModelPage();
  require(int8Extent - int4Extent ==
              uint64_t{extentPages} * kInt4.attentionLayers * 2 * 16'384,
          "INT4's extent is not INT8's less exactly the halved data");
}

// The engine's own plan arithmetic, from Layout and the budget: pages the
// budget buys, whole extents of them, and the context a request gets. The
// constants are the engine's (engine/MemoryPlan.cpp), so the numbers below
// are the numbers a launch prints.
uint32_t contextFor(uint64_t availableBytes, const Layout &layout) {
  const uint64_t pages = availableBytes / layout.bytesPerModelPage();
  const uint32_t extentPages = layout.extentPagesFor(pages);
  if (!extentPages) return 0;
  const uint64_t capacityPages = pages - pages % extentPages;
  const uint64_t tokens = capacityPages * splash::kv::kPageTokens;
  const uint64_t scratch = splash::model::ExecutionLimits::speculativeScratchTokens;
  return static_cast<uint32_t>(
      std::min<uint64_t>(SPLASH_MAXIMUM_CONTEXT_TOKENS, tokens > scratch ? tokens - scratch : 0));
}

void testContextArithmetic() {
  // The two contexts the engine reported on this 24 GiB machine at INT8: with
  // the ANE split holding 520 MiB (512 pages) and without it. They pin the
  // budget one request's KV sees to [5,452,595,200, 5,453,660,160) and
  // [5,997,854,720, 5,998,919,680) bytes; the whole interval is checked, so
  // the result does not depend on which of the two a launch has.
  const std::array<std::pair<uint64_t, uint64_t>, 2> budgets{
      std::pair<uint64_t, uint64_t>{5'452'595'200, 5'453'660'160},
      std::pair<uint64_t, uint64_t>{5'997'854'720, 5'998'919'680}};
  require(contextFor(budgets[0].first, kInt8) == 163'833,
          "INT8's context with the ANE split does not reproduce the engine's 163,833");
  require(contextFor(budgets[1].first, kInt8) == 180'217,
          "INT8's context without the ANE split does not reproduce the engine's 180,217");
  for (const auto &[low, high] : budgets) {
    for (uint64_t available = low; available < high;
         available += kInt4.bytesPerModelPage())
      require(contextFor(available, kInt4) == SPLASH_MAXIMUM_CONTEXT_TOKENS,
              "INT4 does not reach the model's 262,144 tokens on an INT8 budget");
    // ... and with room to spare: the smaller budget still buys 262,144
    // tokens plus the speculative scratch.
    require(low / kInt4.bytesPerModelPage() >= 8'448,
            "INT4's budget does not hold 262,144 tokens of pages");
    require(contextFor(low / 2, kInt4) >= 131'072,
            "half an INT8 budget does not give INT4 half the model's context");
  }
  // The point of the format: one page of 32 tokens costs 528 KiB at INT4
  // against INT8's 1040 KiB, so the 180,217-token history INT8 served in
  // 5.59 GiB costs 3.0 GiB at INT4.
  constexpr uint64_t history = 180'217;
  const uint64_t pages = (history + kTokens - 1) / kTokens;
  require(pages * kInt8.bytesPerModelPage() <= budgets[1].second,
          "the INT8 history the engine served does not fit the budget it served in");
  require(pages * kInt8.bytesPerModelPage() == 5'997'854'720 &&
              pages * kInt4.bytesPerModelPage() == 3'045'064'704,
          "a 180,217-token history does not cost INT8 5.59 GiB and INT4 2.84 GiB");
  // INT4 is not exactly half: it keeps every scale, so one page is
  // 2 * 540,672 - 16 * 2 * 512 = 1,064,960 bytes, INT8's.
  require(2 * kInt4.bytesPerModelPage() - uint64_t{kInt4.attentionLayers} * 2 * 512 ==
              kInt8.bytesPerModelPage(),
          "INT4's page is not half of INT8's plus INT8's scales");
}

void testPlans() {
  // Layout -> PagedAttention: the plan a launch encodes must name the INT4
  // kernels. A format that fell through to INT8's names would read INT4
  // pages as INT8 data, so the names are pinned here, not just the geometry.
  for (const uint32_t queryHeads : {24U, 16U}) {
    const Layout layout{16, queryHeads == 24 ? 4U : 2U, 256, Format::Int4};
    const Layout int8{layout.attentionLayers, layout.kvHeads, 256, Format::Int8};
    const std::string suffix = layout.kvHeads == 4 ? "" : "_kv2_g8";
    for (const uint32_t rows : {1U, 8U, 2048U}) {
      const auto plan = splash::ops::PagedAttention::prefillPlan(rows, queryHeads, layout);
      require(plan.splitPipeline == "prefill_attention_q4_split" + suffix,
              "an INT4 prefill plan does not run the INT4 split kernel");
      require(plan.reducePipeline == "prefill_attention_reduce" + suffix,
              "an INT4 prefill plan does not run the shared reduce kernel");
      // The format changes no scratch: the same rows size the partials the
      // same way.
      require(plan.workspace.partialsBytes ==
                  splash::ops::PagedAttention::prefillPlan(rows, queryHeads, int8)
                      .workspace.partialsBytes,
              "INT4's prefill scratch differs from INT8's");
    }
    const std::array<uint32_t, 1> history{4096};
    const auto verify = splash::ops::PagedAttention::verifyPlan(1, queryHeads, layout, history);
    require(verify.splitPipeline == "verify_attention_q4_split" + suffix,
            "an INT4 verify plan does not run the INT4 split kernel");
    require(verify.reducePipeline == "verify_attention_reduce" + suffix,
            "an INT4 verify plan does not run the shared reduce kernel");
    require(splash::ops::PagedAttention::prefillPlan(8, queryHeads, int8).splitPipeline !=
                splash::ops::PagedAttention::prefillPlan(8, queryHeads, layout).splitPipeline,
            "INT4 and INT8 prefill plans name one kernel");
    require(splash::ops::PagedAttention::verifyPlan(1, queryHeads, int8, history).splitPipeline !=
                splash::ops::PagedAttention::verifyPlan(1, queryHeads, layout, history)
                    .splitPipeline,
            "INT4 and INT8 verify plans name one kernel");
  }
}

} // namespace

int main() {
  try {
    testPageGeometry();
    testSlabAlignment();
    testNibbleOwnership();
    testStoreRoundTrip();
    testPoolGeometry();
    testContextArithmetic();
    testPlans();
    std::cout << "int4_kv_test: ok\n";
    return EXIT_SUCCESS;
  } catch (const std::exception &error) {
    std::cerr << "int4_kv_test failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
