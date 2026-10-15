#pragma once
#include "ops/Int4KvCache.hpp"
#include "ops/VisionConditional.hpp"
#include "ops/KernelFusion.hpp"
#include "ops/SpeculativeDecodingV2.hpp"

// Phase 3 config additions for EngineConfig
// Add these fields to EngineConfig struct:
//
//   std::optional<ops::Int4KvCacheConfig> int4Cache = std::nullopt;
//   std::optional<ops::VisionConditionalConfig> visionConditional = std::nullopt;
//   std::optional<ops::KernelFusionConfig> kernelFusion = std::nullopt;
//   std::optional<ops::SpeculativeDecodingV2Config> speculativeDecoding = std::nullopt;
