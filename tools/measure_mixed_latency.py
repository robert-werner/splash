#!/usr/bin/env python3
"""
Measure p50/p95/p99 latency for mixed workload (interactive + batch requests).

Usage:
    python3 tools/measure_mixed_latency.py --port 8081 --model MODEL --duration 120
"""

import argparse
import asyncio
import json
import random
import sys
import time
from collections import defaultdict
from dataclasses import dataclass
from typing import List

import aiohttp


@dataclass
class RequestMetrics:
    """Metrics for a single request."""
    request_id: str
    category: str  # 'interactive' or 'batch'
    prompt_tokens: int
    submitted_at: float
    first_token_at: float | None
    completed_at: float | None
    ttft: float | None  # time to first token
    total_latency: float | None
    success: bool
    error: str | None = None


class MixedWorkloadBenchmark:
    """Benchmark runner for mixed interactive/batch workload."""

    def __init__(self, base_url: str):
        self.base_url = base_url
        self.metrics: List[RequestMetrics] = []
        self.running = True

    async def send_request(self, prompt: str, category: str, max_tokens: int = 100) -> RequestMetrics:
        """Send a single completion request and measure latency."""
        request_id = f"{category}_{int(time.time() * 1000)}_{random.randint(1000, 9999)}"
        submitted_at = time.time()
        
        metric = RequestMetrics(
            request_id=request_id,
            category=category,
            prompt_tokens=len(prompt.split()),  # rough approximation
            submitted_at=submitted_at,
            first_token_at=None,
            completed_at=None,
            ttft=None,
            total_latency=None,
            success=False,
        )

        try:
            async with aiohttp.ClientSession() as session:
                payload = {
                    "model": "model",
                    "prompt": prompt,
                    "max_tokens": max_tokens,
                    "temperature": 0.7,
                    "stream": True,
                }
                
                async with session.post(
                    f"{self.base_url}/v1/completions",
                    json=payload,
                    timeout=aiohttp.ClientTimeout(total=300),
                ) as response:
                    if response.status != 200:
                        metric.error = f"HTTP {response.status}"
                        return metric
                    
                    first_token_received = False
                    async for line in response.content:
                        line = line.decode("utf-8").strip()
                        if not line or not line.startswith("data: "):
                            continue
                        
                        data_str = line[6:]  # Remove 'data: ' prefix
                        if data_str == "[DONE]":
                            break
                        
                        try:
                            chunk = json.loads(data_str)
                            if not first_token_received and chunk.get("choices"):
                                metric.first_token_at = time.time()
                                metric.ttft = metric.first_token_at - submitted_at
                                first_token_received = True
                        except json.JSONDecodeError:
                            continue
                    
                    metric.completed_at = time.time()
                    metric.total_latency = metric.completed_at - submitted_at
                    metric.success = True
        
        except Exception as e:
            metric.error = str(e)
        
        return metric

    def generate_interactive_prompt(self) -> str:
        """Generate short interactive prompt (<1K tokens)."""
        templates = [
            "Explain quantum computing in one sentence.",
            "What is the capital of France?",
            "Write a haiku about coding.",
            "Translate 'hello' to Spanish.",
            "What is 2+2?",
            "Define machine learning briefly.",
            "Name three programming languages.",
        ]
        return random.choice(templates)

    def generate_batch_prompt(self) -> str:
        """Generate long batch prompt (>10K tokens)."""
        # Create a large context by repeating text
        base = (
            "Analyze the following extensive dataset in detail. "
            "Consider multiple perspectives, historical context, and future implications. "
            "Provide comprehensive insights with specific examples. "
        ) * 100  # Repeat to create ~10K+ tokens
        
        return base + "\n\nProvide a detailed summary covering all aspects."

    async def interactive_worker(self, rate_per_sec: float):
        """Worker that sends interactive requests at specified rate."""
        interval = 1.0 / rate_per_sec if rate_per_sec > 0 else 1.0
        
        while self.running:
            prompt = self.generate_interactive_prompt()
            metric = await self.send_request(prompt, "interactive", max_tokens=50)
            self.metrics.append(metric)
            await asyncio.sleep(interval)

    async def batch_worker(self, rate_per_sec: float):
        """Worker that sends batch requests at specified rate."""
        interval = 1.0 / rate_per_sec if rate_per_sec > 0 else 5.0
        
        while self.running:
            prompt = self.generate_batch_prompt()
            metric = await self.send_request(prompt, "batch", max_tokens=200)
            self.metrics.append(metric)
            await asyncio.sleep(interval)

    async def run(self, duration: int, interactive_rate: float, batch_rate: float):
        """Run mixed workload benchmark for specified duration."""
        print(f"Starting mixed workload benchmark for {duration}s")
        print(f"Interactive rate: {interactive_rate} req/s, Batch rate: {batch_rate} req/s")
        
        tasks = [
            asyncio.create_task(self.interactive_worker(interactive_rate)),
            asyncio.create_task(self.batch_worker(batch_rate)),
        ]
        
        await asyncio.sleep(duration)
        self.running = False
        
        # Cancel workers
        for task in tasks:
            task.cancel()
        
        await asyncio.gather(*tasks, return_exceptions=True)

    def compute_percentiles(self, values: List[float], percentiles: List[int]) -> dict:
        """Compute percentiles from a list of values."""
        if not values:
            return {p: None for p in percentiles}
        
        sorted_values = sorted(values)
        results = {}
        for p in percentiles:
            idx = int(len(sorted_values) * p / 100.0)
            idx = min(idx, len(sorted_values) - 1)
            results[p] = sorted_values[idx]
        return results

    def report(self):
        """Generate and print latency report."""
        by_category = defaultdict(list)
        
        for metric in self.metrics:
            if metric.success and metric.ttft is not None:
                by_category[metric.category].append(metric.ttft)
        
        print("\n" + "=" * 70)
        print("MIXED WORKLOAD LATENCY REPORT")
        print("=" * 70)
        
        for category in ["interactive", "batch"]:
            values = by_category.get(category, [])
            if not values:
                print(f"\n{category.upper()}: No successful requests")
                continue
            
            percentiles = self.compute_percentiles(values, [50, 95, 99])
            
            print(f"\n{category.upper()} (n={len(values)}):")
            print(f"  p50 (median): {percentiles[50]*1000:.1f} ms")
            print(f"  p95:          {percentiles[95]*1000:.1f} ms")
            print(f"  p99:          {percentiles[99]*1000:.1f} ms")
            print(f"  min:          {min(values)*1000:.1f} ms")
            print(f"  max:          {max(values)*1000:.1f} ms")
        
        # Overall statistics
        all_ttft = [m.ttft for m in self.metrics if m.success and m.ttft is not None]
        success_count = sum(1 for m in self.metrics if m.success)
        failure_count = len(self.metrics) - success_count
        
        print(f"\nOVERALL:")
        print(f"  Total requests: {len(self.metrics)}")
        print(f"  Successful:     {success_count}")
        print(f"  Failed:         {failure_count}")
        
        if all_ttft:
            overall_percentiles = self.compute_percentiles(all_ttft, [50, 95, 99])
            print(f"\nOVERALL LATENCY (all categories):")
            print(f"  p50: {overall_percentiles[50]*1000:.1f} ms")
            print(f"  p95: {overall_percentiles[95]*1000:.1f} ms")
            print(f"  p99: {overall_percentiles[99]*1000:.1f} ms")
        
        print("=" * 70)


async def main():
    parser = argparse.ArgumentParser(description="Measure mixed workload latency")
    parser.add_argument("--port", type=int, default=8081, help="Server port")
    parser.add_argument("--duration", type=int, default=60, help="Benchmark duration in seconds")
    parser.add_argument("--interactive-rate", type=float, default=2.0, help="Interactive requests per second")
    parser.add_argument("--batch-rate", type=float, default=0.5, help="Batch requests per second")
    
    args = parser.parse_args()
    
    base_url = f"http://localhost:{args.port}"
    
    # Check server health
    try:
        async with aiohttp.ClientSession() as session:
            async with session.get(f"{base_url}/v1/models") as response:
                if response.status != 200:
                    print(f"Error: Server not ready (HTTP {response.status})")
                    return 1
    except Exception as e:
        print(f"Error: Cannot connect to server at {base_url}: {e}")
        return 1
    
    benchmark = MixedWorkloadBenchmark(base_url)
    await benchmark.run(args.duration, args.interactive_rate, args.batch_rate)
    benchmark.report()
    
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
