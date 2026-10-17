#!/usr/bin/env python3
"""
Expert Routing Pattern Analyzer

Инструментирует Splash для сбора routing traces и анализирует:
1. Expert frequency distribution (для hot cache selection)
2. Temporal locality (bigram patterns для prefetch)
3. Layer-wise stability (какие слои предсказуемые)
4. Hit rate projection (estimated cache performance)

Usage:
    python3 analyze_routing.py --model qwen35b --prompts validation.txt --output routing_stats.json
"""

import argparse
import json
import sys
from collections import defaultdict, Counter
from pathlib import Path
import numpy as np
from typing import Dict, List, Tuple
import matplotlib.pyplot as plt


class RoutingAnalyzer:
    """Analyzes MoE expert routing patterns."""
    
    def __init__(self, num_layers: int = 40, num_experts: int = 256):
        self.num_layers = num_layers
        self.num_experts = num_experts
        
        # Per-layer statistics
        self.frequencies = [Counter() for _ in range(num_layers)]
        self.bigrams = [defaultdict(Counter) for _ in range(num_layers)]
        self.history = [[] for _ in range(num_layers)]
        
        # Global stats
        self.total_tokens = 0
        self.total_selections = 0
    
    def record_routing(self, layer: int, token_id: int, experts: List[int]):
        """Record expert selection for one token."""
        if layer >= self.num_layers:
            return
        
        self.total_tokens += 1
        self.total_selections += len(experts)
        
        # Update frequency
        for expert in experts:
            self.frequencies[layer][expert] += 1
        
        # Update bigrams (prev token → current token)
        if self.history[layer]:
            prev_experts = self.history[layer][-1]
            for prev_expert in prev_experts:
                for curr_expert in experts:
                    self.bigrams[layer][prev_expert][curr_expert] += 1
        
        # Store history
        self.history[layer].append(experts)
    
    def get_top_experts(self, layer: int, k: int = 128) -> List[int]:
        """Get top-K most frequent experts for layer."""
        return [exp for exp, count in self.frequencies[layer].most_common(k)]
    
    def compute_power_law_metrics(self, layer: int) -> Dict:
        """Analyze expert usage distribution."""
        counts = sorted(self.frequencies[layer].values(), reverse=True)
        if not counts:
            return {}
        
        total = sum(counts)
        
        # Top-10% experts handle X% of traffic
        top_10pct = int(0.1 * self.num_experts)
        top_10pct_traffic = sum(counts[:top_10pct]) / total if total > 0 else 0
        
        # Gini coefficient (inequality measure)
        cumsum = np.cumsum(sorted(counts))
        n = len(counts)
        gini = (2 * sum((i + 1) * counts[i] for i in range(n))) / (n * sum(counts)) - (n + 1) / n
        
        return {
            'top_10pct_traffic': top_10pct_traffic,
            'gini_coefficient': gini,
            'unique_experts_used': len(counts),
            'coverage': len(counts) / self.num_experts,
        }
    
    def simulate_cache_hit_rate(self, layer: int, cache_size: int) -> float:
        """Simulate LRU cache hit rate for given cache size."""
        if not self.history[layer]:
            return 0.0
        
        cache = set()
        lru_order = []
        hits = 0
        total = 0
        
        for experts in self.history[layer]:
            for expert in experts:
                total += 1
                if expert in cache:
                    hits += 1
                    # Move to front (MRU)
                    lru_order.remove(expert)
                    lru_order.insert(0, expert)
                else:
                    # Add to cache
                    if len(cache) >= cache_size:
                        # Evict LRU
                        victim = lru_order.pop()
                        cache.remove(victim)
                    cache.add(expert)
                    lru_order.insert(0, expert)
        
        return hits / total if total > 0 else 0.0
    
    def compute_bigram_accuracy(self, layer: int) -> float:
        """Measure bigram prediction accuracy."""
        if len(self.history[layer]) < 2:
            return 0.0
        
        correct_predictions = 0
        total_predictions = 0
        
        for i in range(1, len(self.history[layer])):
            prev_experts = self.history[layer][i - 1]
            curr_experts = set(self.history[layer][i])
            
            # Predict: most common next experts after prev_experts
            predicted = set()
            for prev_expert in prev_experts:
                if prev_expert in self.bigrams[layer]:
                    # Top-8 most common followers
                    top_followers = [e for e, _ in self.bigrams[layer][prev_expert].most_common(8)]
                    predicted.update(top_followers)
            
            # Measure overlap
            overlap = len(predicted & curr_experts)
            correct_predictions += overlap
            total_predictions += len(curr_experts)
        
        return correct_predictions / total_predictions if total_predictions > 0 else 0.0
    
    def generate_report(self) -> Dict:
        """Generate comprehensive analysis report."""
        report = {
            'summary': {
                'total_tokens': self.total_tokens,
                'total_expert_selections': self.total_selections,
                'avg_experts_per_token': self.total_selections / self.total_tokens if self.total_tokens > 0 else 0,
            },
            'per_layer': []
        }
        
        for layer in range(self.num_layers):
            if not self.frequencies[layer]:
                continue
            
            metrics = self.compute_power_law_metrics(layer)
            
            # Cache simulation
            cache_hit_rates = {
                'cache_64': self.simulate_cache_hit_rate(layer, 64),
                'cache_128': self.simulate_cache_hit_rate(layer, 128),
                'cache_256': self.simulate_cache_hit_rate(layer, 256),
            }
            
            bigram_accuracy = self.compute_bigram_accuracy(layer)
            
            layer_report = {
                'layer': layer,
                'distribution': metrics,
                'cache_hit_rates': cache_hit_rates,
                'bigram_prediction_accuracy': bigram_accuracy,
                'top_10_experts': self.get_top_experts(layer, 10),
            }
            
            report['per_layer'].append(layer_report)
        
        # Global cache hit rate (average across layers)
        avg_hit_128 = np.mean([l['cache_hit_rates']['cache_128'] for l in report['per_layer']])
        report['summary']['estimated_hit_rate_128'] = avg_hit_128
        
        return report
    
    def plot_distribution(self, layer: int, output_path: str):
        """Plot expert usage distribution for one layer."""
        counts = sorted(self.frequencies[layer].values(), reverse=True)
        
        plt.figure(figsize=(10, 6))
        plt.plot(counts, linewidth=2)
        plt.xlabel('Expert Rank')
        plt.ylabel('Usage Count')
        plt.title(f'Expert Usage Distribution - Layer {layer}')
        plt.yscale('log')
        plt.grid(True, alpha=0.3)
        plt.tight_layout()
        plt.savefig(output_path, dpi=150)
        plt.close()
    
    def export_hot_cache_config(self, k: int = 128) -> Dict:
        """Export hot cache configuration (for pinning)."""
        config = {
            'cache_size': k,
            'hot_experts': []
        }
        
        for layer in range(self.num_layers):
            top_experts = self.get_top_experts(layer, k)
            config['hot_experts'].append({
                'layer': layer,
                'experts': top_experts
            })
        
        return config
    
    def save(self, path: str):
        """Save analyzer state to JSON."""
        data = {
            'num_layers': self.num_layers,
            'num_experts': self.num_experts,
            'total_tokens': self.total_tokens,
            'frequencies': [dict(counter) for counter in self.frequencies],
            'bigrams': [
                {k: dict(v) for k, v in bigram.items()}
                for bigram in self.bigrams
            ]
        }
        
        with open(path, 'w') as f:
            json.dump(data, f, indent=2)
    
    @staticmethod
    def load(path: str) -> 'RoutingAnalyzer':
        """Load analyzer state from JSON."""
        with open(path, 'r') as f:
            data = json.load(f)
        
        analyzer = RoutingAnalyzer(data['num_layers'], data['num_experts'])
        analyzer.total_tokens = data['total_tokens']
        analyzer.frequencies = [Counter(d) for d in data['frequencies']]
        analyzer.bigrams = [
            defaultdict(Counter, {int(k): Counter(v) for k, v in bigram.items()})
            for bigram in data['bigrams']
        ]
        
        return analyzer


def parse_routing_trace(trace_path: str, analyzer: RoutingAnalyzer):
    """Parse routing trace file (format: layer token_id expert1 expert2 ...)."""
    with open(trace_path, 'r') as f:
        for line in f:
            parts = line.strip().split()
            if len(parts) < 3:
                continue
            
            layer = int(parts[0])
            token_id = int(parts[1])
            experts = [int(x) for x in parts[2:]]
            
            analyzer.record_routing(layer, token_id, experts)


def main():
    parser = argparse.ArgumentParser(description='Analyze MoE expert routing patterns')
    parser.add_argument('--trace', type=str, required=True,
                        help='Path to routing trace file')
    parser.add_argument('--output', type=str, default='routing_analysis.json',
                        help='Output analysis report path')
    parser.add_argument('--hot-cache', type=int, default=128,
                        help='Hot cache size for config export')
    parser.add_argument('--plot-layer', type=int, default=None,
                        help='Plot distribution for specific layer')
    
    args = parser.parse_args()
    
    print(f"Loading routing trace from {args.trace}...")
    analyzer = RoutingAnalyzer()
    parse_routing_trace(args.trace, analyzer)
    
    print(f"Analyzed {analyzer.total_tokens} tokens")
    print(f"Total expert selections: {analyzer.total_selections}")
    
    print("\nGenerating report...")
    report = analyzer.generate_report()
    
    # Print summary
    print("\n" + "="*60)
    print("SUMMARY")
    print("="*60)
    print(f"Tokens processed: {report['summary']['total_tokens']}")
    print(f"Avg experts/token: {report['summary']['avg_experts_per_token']:.2f}")
    print(f"Estimated hit rate (128-expert cache): {report['summary']['estimated_hit_rate_128']*100:.1f}%")
    
    # Per-layer highlights
    print("\n" + "="*60)
    print("PER-LAYER METRICS (selected layers)")
    print("="*60)
    print(f"{'Layer':<6} {'Top10%':<8} {'Gini':<8} {'Hit@128':<10} {'Bigram':<10}")
    print("-"*60)
    
    for layer_report in report['per_layer'][::10]:  # Every 10th layer
        layer = layer_report['layer']
        dist = layer_report['distribution']
        hit = layer_report['cache_hit_rates']['cache_128']
        bigram = layer_report['bigram_prediction_accuracy']
        
        print(f"{layer:<6} {dist['top_10pct_traffic']*100:>6.1f}% "
              f"{dist['gini_coefficient']:>6.3f} {hit*100:>8.1f}% "
              f"{bigram*100:>8.1f}%")
    
    # Save full report
    with open(args.output, 'w') as f:
        json.dump(report, f, indent=2)
    print(f"\n✓ Full report saved to {args.output}")
    
    # Export hot cache config
    hot_cache_config = analyzer.export_hot_cache_config(args.hot_cache)
    hot_cache_path = args.output.replace('.json', '_hot_cache.json')
    with open(hot_cache_path, 'w') as f:
        json.dump(hot_cache_config, f, indent=2)
    print(f"✓ Hot cache config saved to {hot_cache_path}")
    
    # Save analyzer state
    analyzer_path = args.output.replace('.json', '_state.json')
    analyzer.save(analyzer_path)
    print(f"✓ Analyzer state saved to {analyzer_path}")
    
    # Plot if requested
    if args.plot_layer is not None:
        plot_path = f"expert_distribution_layer_{args.plot_layer}.png"
        analyzer.plot_distribution(args.plot_layer, plot_path)
        print(f"✓ Distribution plot saved to {plot_path}")
    
    # Recommendation
    print("\n" + "="*60)
    print("RECOMMENDATION")
    print("="*60)
    
    avg_hit = report['summary']['estimated_hit_rate_128']
    if avg_hit >= 0.75:
        print("✓ Excellent hit rate (≥75%)! Expert offloading is HIGHLY VIABLE.")
        print("  Expected slowdown: 10-20% with async prefetch.")
    elif avg_hit >= 0.60:
        print("✓ Good hit rate (60-75%). Expert offloading is VIABLE.")
        print("  Expected slowdown: 20-35% with async prefetch.")
    elif avg_hit >= 0.45:
        print("⚠ Moderate hit rate (45-60%). Expert offloading is MARGINAL.")
        print("  Expected slowdown: 35-50%. Consider 3-bit quantization instead.")
    else:
        print("✗ Poor hit rate (<45%). Expert offloading NOT RECOMMENDED.")
        print("  Expected slowdown: >50%. Use 3-bit quantization.")
    
    print("\nNext steps:")
    print(f"1. Review {args.output} for detailed layer-wise metrics")
    print(f"2. Use {hot_cache_path} to configure hot expert pinning")
    print("3. Proceed to prototype implementation (Phase 2)")


if __name__ == '__main__':
    main()
