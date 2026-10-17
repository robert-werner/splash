# SSD-backed KV Cache Extension: 262K → 1M+ Tokens

## Architecture

### Two-Tier System
- **Hot tier (RAM)**: 0-200K tokens, current performance (262K capable)
- **Cold tier (SSD)**: 200K-1M tokens, async page fault + prefetch

### Core Components

1. **ExtendedKvTier** (runtime/engine/ExtendedKvTier.{hpp,cpp})
   - Wraps two SlotFile instances (hot + cold)
   - Manages tier promotion/demotion
   - Implements async prefetch for sequential patterns

2. **Async Page Fault**
   - Attention proceeds on hot pages while cold pages load in parallel
   - Non-blocking restore mechanism
   - Completion callbacks wake engine

3. **LRU Eviction**
   - Hot → Cold under memory pressure
   - Access tracking per block
   - Configurable hot tier boundary

4. **Sequential Prefetch**
   - Detects sequential access patterns (books, documents)
   - Loads next N pages ahead
   - Configurable prefetch window (default: 16 pages = 1K tokens)

## Performance Targets (M5 Pro, 1TB SSD @ ~3GB/s)

- **Sequential read** (book): 55-60 tok/s
- **Random access**: 45-50 tok/s  
- **Hot tier**: unchanged from current 262K baseline
- **Page size**: 64 tokens/page × 16KB/page = ~16MB per 1K tokens

## Configuration

### CLI Flags (to be added)
```
--max-context-extended 1000000      # Enable extended context (default: disabled)
--cold-tier-path /path/to/ssd       # SSD storage directory
--hot-tier-tokens 200000            # Hot/cold boundary (default: 200K)
--prefetch-pages 16                 # Prefetch window size
```

### EngineConfig Extension
```cpp
struct EngineConfig {
  // ... existing fields ...
  
  // Extended context support
  uint32_t maxContextExtended = 0;     // 0 = disabled, >0 = extended mode
  std::string coldTierPath;            // SSD storage path
  uint32_t hotTierTokens = 200 * 1024; // Hot tier boundary
  uint32_t prefetchPages = 16;         // Prefetch window
};
```

## Integration Points

1. **Cache.cpp**: Use ExtendedKvTier when maxContextExtended > 0
2. **Engine.cpp**: Pass extended config to Cache constructor
3. **Bootstrap**: Validate SSD path, measure sequential throughput
4. **Status endpoint**: Report hot/cold tier statistics

## Implementation Status

- [x] ExtendedKvTier.hpp header
- [x] ExtendedKvTier.cpp implementation
- [ ] Fix compilation errors
- [ ] CLI parameter parsing
- [ ] EngineConfig integration
- [ ] Cache.cpp integration
- [ ] Sequential access detection
- [ ] Prefetch mechanism
- [ ] Performance benchmarks
- [ ] Tests

## Memory Budget

INT4 KV format (current):
- 262K tokens: 4.2 GB resident
- 1M tokens hot+cold: 16.9 GB total (3.2 GB resident + 13.7 GB SSD)

## Testing Plan

1. **Unit tests**: ExtendedKvTier tier transitions, prefetch logic
2. **Integration tests**: 500K token book, random access patterns
3. **Benchmarks**: Sequential vs random throughput measurements
4. **Stress tests**: Memory pressure, SSD full, cold tier eviction

## Future Optimizations

1. **INT4 → INT2 compression** for cold tier (2x space savings)
2. **Smart prefetch heuristics** (not just sequential)
3. **Multi-level cold tier** (SSD → remote storage)
4. **Compression-aware page sizes**
