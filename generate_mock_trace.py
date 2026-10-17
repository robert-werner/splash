#!/usr/bin/env python3
"""
Mock Routing Trace Generator

Generates synthetic routing traces for testing analyzer.
Simulates realistic expert selection patterns:
- Power-law distribution (few hot experts, many cold)
- Temporal locality (nearby tokens use similar experts)
- Layer-wise variation (some layers more stable)

Usage:
    python3 generate_mock_trace.py --tokens 1000 --output mock_trace.txt
"""

import argparse
import random
import numpy as np
from typing import List


class MockRoutingGenerator:
    """Generates synthetic but realistic expert routing traces."""
    
    def __init__(self, num_layers: int = 40, num_experts: int = 256, top_k: int = 8):
        self.num_layers = num_layers
        self.num_experts = num_experts
        self.top_k = top_k
        
        # Per-layer expert probabilities (power-law distribution)
        self.expert_probs = []
        for layer in range(num_layers):
            # Zipf distribution (power law)
            alpha = 1.5 + random.uniform(-0.3, 0.3)  # Vary by layer
            probs = np.random.zipf(alpha, num_experts)
            probs = probs / probs.sum()  # Normalize
            self.expert_probs.append(probs)
        
        # Recent expert history (for temporal locality)
        self.recent_experts = [[] for _ in range(num_layers)]
    
    def generate_routing(self, layer: int, token_id: int) -> List[int]:
        """Generate expert selection for one token."""
        probs = self.expert_probs[layer].copy()
        
        # Temporal locality: boost recently used experts
        if self.recent_experts[layer]:
            recent = set(self.recent_experts[layer][-1])  # Last token's experts
            for expert in recent:
                probs[expert] *= 2.0  # 2x more likely to reuse
        
        # Normalize
        probs = probs / probs.sum()
        
        # Sample top-k experts
        experts = np.random.choice(
            self.num_experts,
            size=self.top_k,
            replace=False,
            p=probs
        )
        
        # Update history
        self.recent_experts[layer].append(list(experts))
        if len(self.recent_experts[layer]) > 10:
            self.recent_experts[layer].pop(0)
        
        return sorted(experts)
    
    def generate_trace(self, num_tokens: int, output_path: str):
        """Generate full trace for num_tokens."""
        with open(output_path, 'w') as f:
            for token_id in range(num_tokens):
                for layer in range(self.num_layers):
                    experts = self.generate_routing(layer, token_id)
                    # Format: layer token_id expert1 expert2 ... expertK
                    line = f"{layer} {token_id} " + " ".join(map(str, experts)) + "\n"
                    f.write(line)


def main():
    parser = argparse.ArgumentParser(description='Generate mock routing trace')
    parser.add_argument('--tokens', type=int, default=1000,
                        help='Number of tokens to generate')
    parser.add_argument('--layers', type=int, default=40,
                        help='Number of layers')
    parser.add_argument('--experts', type=int, default=256,
                        help='Number of experts per layer')
    parser.add_argument('--top-k', type=int, default=8,
                        help='Experts selected per token')
    parser.add_argument('--output', type=str, default='mock_routing_trace.txt',
                        help='Output trace file')
    
    args = parser.parse_args()
    
    print(f"Generating mock routing trace...")
    print(f"  Tokens: {args.tokens}")
    print(f"  Layers: {args.layers}")
    print(f"  Experts per layer: {args.experts}")
    print(f"  Top-K: {args.top_k}")
    
    generator = MockRoutingGenerator(args.layers, args.experts, args.top_k)
    generator.generate_trace(args.tokens, args.output)
    
    print(f"✓ Trace saved to {args.output}")
    print(f"  Total routing decisions: {args.tokens * args.layers}")
    print(f"  Total expert selections: {args.tokens * args.layers * args.top_k}")
    
    # Compute expected statistics
    print("\nExpected characteristics:")
    print("  Distribution: Power-law (Zipf α≈1.5)")
    print("  Temporal locality: 2x boost for recent experts")
    print("  Top-10% experts: ~70-80% of traffic (estimated)")
    
    print(f"\nNext step:")
    print(f"  python3 analyze_routing.py --trace {args.output} --output analysis.json")


if __name__ == '__main__':
    main()
