#!/usr/bin/env python3
"""
Test priority scheduling: measure p50/p95/p99 latency for mixed workload.
Sends short interactive requests alongside long batch requests.
"""

import asyncio
import time
import json
import statistics
from typing import List, Dict
from dataclasses import dataclass
import aiohttp


@dataclass
class RequestMetrics:
    """Metrics for a single request."""
    tokens: int
    type: str  # 'interactive' or 'batch'
    start_time: float
    first_token_time: float
    end_time: float
    
    @property
    def time_to_first_token(self) -> float:
        return self.first_token_time - self.start_time
    
    @property
    def total_time(self) -> float:
        return self.end_time - self.start_time


async def send_request(
    session: aiohttp.ClientSession,
    prompt_tokens: int,
    max_new_tokens: int,
    request_type: str
) -> RequestMetrics:
    """Send a single completion request and measure latency."""
    # Simple prompt repeating tokens
    prompt = "Hello " * prompt_tokens
    
    start_time = time.time()
    first_token_time = None
    
    payload = {
        "model": "test",
        "prompt": prompt,
        "max_tokens": max_new_tokens,
        "stream": True,
        "temperature": 0.7,
    }
    
    async with session.post(
        "http://localhost:8081/v1/completions",
        json=payload
    ) as response:
        async for line in response.content:
            if not line:
                continue
            
            line = line.decode('utf-8').strip()
            if not line.startswith('data: '):
                continue
            
            data_str = line[6:]
            if data_str == '[DONE]':
                break
            
            try:
                data = json.loads(data_str)
                if first_token_time is None and data.get('choices'):
                    first_token_time = time.time()
            except json.JSONDecodeError:
                pass
    
    end_time = time.time()
    
    return RequestMetrics(
        tokens=prompt_tokens,
        type=request_type,
        start_time=start_time,
        first_token_time=first_token_time or end_time,
        end_time=end_time
    )


async def run_mixed_workload(
    num_interactive: int = 20,
    num_batch: int = 5,
    interactive_tokens: int = 500,
    batch_tokens: int = 15000,
    max_new_tokens: int = 100
) -> List[RequestMetrics]:
    """Run mixed workload with interactive and batch requests."""
    connector = aiohttp.TCPConnector(limit=100)
    async with aiohttp.ClientSession(connector=connector) as session:
        tasks = []
        
        # Create interactive requests
        for _ in range(num_interactive):
            tasks.append(send_request(
                session, interactive_tokens, max_new_tokens, 'interactive'
            ))
            await asyncio.sleep(0.5)  # Stagger arrivals
        
        # Create batch requests
        for _ in range(num_batch):
            tasks.append(send_request(
                session, batch_tokens, max_new_tokens, 'batch'
            ))
            await asyncio.sleep(1.0)
        
        # Wait for all requests
        results = await asyncio.gather(*tasks, return_exceptions=True)
        
        # Filter out exceptions
        valid_results = [r for r in results if isinstance(r, RequestMetrics)]
        return valid_results


def analyze_results(metrics: List[RequestMetrics]) -> Dict:
    """Analyze latency metrics."""
    interactive = [m for m in metrics if m.type == 'interactive']
    batch = [m for m in metrics if m.type == 'batch']
    
    def compute_percentiles(times: List[float]) -> Dict:
        if not times:
            return {'p50': 0, 'p95': 0, 'p99': 0, 'mean': 0}
        sorted_times = sorted(times)
        return {
            'p50': statistics.quantiles(sorted_times, n=100)[49],
            'p95': statistics.quantiles(sorted_times, n=100)[94],
            'p99': statistics.quantiles(sorted_times, n=100)[98],
            'mean': statistics.mean(sorted_times)
        }
    
    interactive_ttft = [m.time_to_first_token for m in interactive]
    batch_ttft = [m.time_to_first_token for m in batch]
    
    return {
        'interactive': {
            'count': len(interactive),
            'ttft_ms': {k: v * 1000 for k, v in compute_percentiles(interactive_ttft).items()}
        },
        'batch': {
            'count': len(batch),
            'ttft_ms': {k: v * 1000 for k, v in compute_percentiles(batch_ttft).items()}
        },
        'all': {
            'count': len(metrics),
            'ttft_ms': {
                k: v * 1000 
                for k, v in compute_percentiles(
                    [m.time_to_first_token for m in metrics]
                ).items()
            }
        }
    }


async def main():
    """Run benchmarks with and without priority scheduling."""
    print("=== Mixed Workload Latency Benchmark ===\n")
    print("Running baseline (no priority scheduling)...")
    
    # Wait for server to be ready
    await asyncio.sleep(2)
    
    baseline_metrics = await run_mixed_workload()
    baseline_results = analyze_results(baseline_metrics)
    
    print("\n--- Baseline Results ---")
    print(json.dumps(baseline_results, indent=2))
    
    print("\n\nTest complete!")
    print(f"\nInteractive requests: {baseline_results['interactive']['count']}")
    print(f"Batch requests: {baseline_results['batch']['count']}")
    print(f"\nInteractive TTFT p99: {baseline_results['interactive']['ttft_ms']['p99']:.0f}ms")
    print(f"All requests TTFT p99: {baseline_results['all']['ttft_ms']['p99']:.0f}ms")


if __name__ == "__main__":
    asyncio.run(main())
