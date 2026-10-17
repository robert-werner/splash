#!/usr/bin/env python3
"""
Mixed workload latency measurement для priority scheduling evaluation.

Измеряет p50/p95/p99 latency для:
- Interactive requests (<1K tokens): короткие prompts, ожидают быстрого ответа
- Batch requests (>10K tokens): длинные prompts, throughput-oriented

Usage:
  python3 measure_mixed_latency.py --port 8081 --duration 120 --interactive-rate 4 --batch-rate 1
"""

import argparse
import json
import random
import sys
import time
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from typing import List, Dict

import requests

@dataclass
class LatencyMeasurement:
    """Single request latency measurement."""
    request_id: str
    workload_type: str  # "interactive" or "batch"
    prompt_tokens: int
    ttft_ms: float  # Time to first token
    total_ms: float  # Total completion time
    completion_tokens: int
    timestamp: float


class MixedWorkloadBenchmark:
    """Mixed workload latency benchmark."""
    
    def __init__(self, base_url: str, interactive_tokens: int = 512, 
                 batch_tokens: int = 12000):
        self.base_url = base_url
        self.interactive_tokens = interactive_tokens
        self.batch_tokens = batch_tokens
        self.measurements: List[LatencyMeasurement] = []
        
    def generate_prompt(self, num_tokens: int) -> str:
        """Generate prompt with approximately num_tokens tokens."""
        # Approximation: 4 characters per token in English
        words = ["The", "quick", "brown", "fox", "jumps", "over", "the", "lazy", "dog"]
        prompt = []
        estimated_tokens = 0
        
        while estimated_tokens < num_tokens:
            word = random.choice(words)
            prompt.append(word)
            estimated_tokens = len(" ".join(prompt)) // 4
            
        return " ".join(prompt)
    
    def send_request(self, workload_type: str, prompt_tokens: int) -> LatencyMeasurement:
        """Send single completion request and measure latency."""
        prompt = self.generate_prompt(prompt_tokens)
        request_id = f"{workload_type}_{int(time.time() * 1000000)}"
        
        payload = {
            "model": "default",
            "prompt": prompt,
            "max_tokens": 32,  # Short completion for latency measurement
            "temperature": 0.0,
            "stream": False
        }
        
        start_time = time.time()
        
        try:
            response = requests.post(
                f"{self.base_url}/v1/completions",
                json=payload,
                timeout=300
            )
            response.raise_for_status()
            
            end_time = time.time()
            total_ms = (end_time - start_time) * 1000
            
            result = response.json()
            completion_tokens = result.get("usage", {}).get("completion_tokens", 0)
            
            # TTFT approximation: assume uniform generation
            ttft_ms = total_ms / max(1, completion_tokens) if completion_tokens > 0 else total_ms
            
            measurement = LatencyMeasurement(
                request_id=request_id,
                workload_type=workload_type,
                prompt_tokens=prompt_tokens,
                ttft_ms=ttft_ms,
                total_ms=total_ms,
                completion_tokens=completion_tokens,
                timestamp=start_time
            )
            
            self.measurements.append(measurement)
            return measurement
            
        except Exception as e:
            print(f"Request {request_id} failed: {e}", file=sys.stderr)
            # Return failed measurement
            return LatencyMeasurement(
                request_id=request_id,
                workload_type=workload_type,
                prompt_tokens=prompt_tokens,
                ttft_ms=-1,
                total_ms=-1,
                completion_tokens=0,
                timestamp=start_time
            )
    
    def run_mixed_workload(self, duration_seconds: int, interactive_rps: float, 
                          batch_rps: float, max_workers: int = 16):
        """Run mixed workload for specified duration."""
        print(f"Starting mixed workload benchmark:")
        print(f"  Duration: {duration_seconds}s")
        print(f"  Interactive rate: {interactive_rps} req/s ({self.interactive_tokens} tokens)")
        print(f"  Batch rate: {batch_rps} req/s ({self.batch_tokens} tokens)")
        print()
        
        start_time = time.time()
        end_time = start_time + duration_seconds
        
        with ThreadPoolExecutor(max_workers=max_workers) as executor:
            futures = []
            
            while time.time() < end_time:
                current = time.time()
                
                # Schedule interactive requests
                if random.random() < interactive_rps:
                    future = executor.submit(
                        self.send_request, "interactive", self.interactive_tokens
                    )
                    futures.append(future)
                
                # Schedule batch requests
                if random.random() < batch_rps:
                    future = executor.submit(
                        self.send_request, "batch", self.batch_tokens
                    )
                    futures.append(future)
                
                # Sleep until next second
                elapsed = time.time() - current
                if elapsed < 1.0:
                    time.sleep(1.0 - elapsed)
            
            # Wait for all requests to complete
            print("\nWaiting for in-flight requests to complete...")
            for future in as_completed(futures):
                try:
                    future.result()
                except Exception as e:
                    print(f"Request error: {e}", file=sys.stderr)
        
        print(f"Benchmark complete. Total requests: {len(self.measurements)}")
    
    def compute_percentiles(self, values: List[float], percentiles: List[float]) -> Dict[float, float]:
        """Compute percentiles from list of values."""
        if not values:
            return {p: -1 for p in percentiles}
        
        sorted_values = sorted(values)
        result = {}
        
        for p in percentiles:
            idx = int(len(sorted_values) * p / 100.0)
            idx = min(idx, len(sorted_values) - 1)
            result[p] = sorted_values[idx]
        
        return result
    
    def analyze_results(self):
        """Analyze and print latency statistics."""
        # Filter successful measurements
        valid = [m for m in self.measurements if m.total_ms > 0]
        
        if not valid:
            print("No successful measurements!")
            return
        
        # Group by workload type
        by_type = defaultdict(list)
        for m in valid:
            by_type[m.workload_type].append(m)
        
        print("\n" + "="*80)
        print("LATENCY ANALYSIS")
        print("="*80)
        
        for workload_type in ["interactive", "batch"]:
            measurements = by_type[workload_type]
            if not measurements:
                continue
            
            print(f"\n{workload_type.upper()} REQUESTS ({len(measurements)} samples)")
            print("-" * 40)
            
            # TTFT latency
            ttft_values = [m.ttft_ms for m in measurements if m.ttft_ms > 0]
            if ttft_values:
                ttft_percentiles = self.compute_percentiles(ttft_values, [50, 95, 99])
                print(f"TTFT (Time to First Token):")
                print(f"  p50: {ttft_percentiles[50]:.1f} ms")
                print(f"  p95: {ttft_percentiles[95]:.1f} ms")
                print(f"  p99: {ttft_percentiles[99]:.1f} ms")
            
            # Total latency
            total_values = [m.total_ms for m in measurements]
            total_percentiles = self.compute_percentiles(total_values, [50, 95, 99])
            print(f"\nTotal latency:")
            print(f"  p50: {total_percentiles[50]:.1f} ms")
            print(f"  p95: {total_percentiles[95]:.1f} ms")
            print(f"  p99: {total_percentiles[99]:.1f} ms")
            
            # Throughput
            tokens_per_second = sum(m.completion_tokens for m in measurements) / sum(m.total_ms for m in measurements) * 1000
            print(f"\nThroughput: {tokens_per_second:.1f} tokens/s")
        
        # Overall stats
        print(f"\n{'OVERALL STATISTICS':^40}")
        print("-" * 40)
        print(f"Total requests: {len(valid)}")
        print(f"  Interactive: {len(by_type['interactive'])}")
        print(f"  Batch: {len(by_type['batch'])}")
        print(f"Failed requests: {len(self.measurements) - len(valid)}")
        
        # Export raw data
        output_file = "/tmp/mixed_latency_results.json"
        with open(output_file, "w") as f:
            json.dump([{
                "request_id": m.request_id,
                "workload_type": m.workload_type,
                "prompt_tokens": m.prompt_tokens,
                "ttft_ms": m.ttft_ms,
                "total_ms": m.total_ms,
                "completion_tokens": m.completion_tokens,
                "timestamp": m.timestamp
            } for m in self.measurements], f, indent=2)
        
        print(f"\nRaw data exported to: {output_file}")
        print("="*80)


def main():
    parser = argparse.ArgumentParser(
        description="Measure mixed workload latency for priority scheduling evaluation"
    )
    parser.add_argument("--port", type=int, default=8081, help="Server port")
    parser.add_argument("--duration", type=int, default=120, help="Benchmark duration in seconds")
    parser.add_argument("--interactive-rate", type=float, default=4.0, 
                       help="Interactive requests per second")
    parser.add_argument("--batch-rate", type=float, default=1.0,
                       help="Batch requests per second")
    parser.add_argument("--interactive-tokens", type=int, default=512,
                       help="Tokens per interactive request")
    parser.add_argument("--batch-tokens", type=int, default=12000,
                       help="Tokens per batch request")
    parser.add_argument("--max-workers", type=int, default=16,
                       help="Maximum concurrent requests")
    
    args = parser.parse_args()
    
    base_url = f"http://localhost:{args.port}"
    
    # Check server health
    try:
        response = requests.get(f"{base_url}/v1/models", timeout=5)
        response.raise_for_status()
        print(f"Server is ready at {base_url}")
    except Exception as e:
        print(f"Server is not available: {e}", file=sys.stderr)
        sys.exit(1)
    
    # Run benchmark
    benchmark = MixedWorkloadBenchmark(
        base_url=base_url,
        interactive_tokens=args.interactive_tokens,
        batch_tokens=args.batch_tokens
    )
    
    benchmark.run_mixed_workload(
        duration_seconds=args.duration,
        interactive_rps=args.interactive_rate,
        batch_rps=args.batch_rps,
        max_workers=args.max_workers
    )
    
    benchmark.analyze_results()


if __name__ == "__main__":
    main()
