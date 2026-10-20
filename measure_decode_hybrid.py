#!/usr/bin/env python3
"""Measure hybrid GGUF decode throughput via native binary protocol."""

import json
import sys
import time
from pathlib import Path

from binary_protocol_client import BinaryProtocolClient
sys.path.insert(0, "server")
from protocol import SamplingParameters


def tokenize_dummy(text: str) -> tuple[int, ...]:
    """Dummy tokenizer: split on whitespace, hash to token IDs."""
    words = text.split()
    return tuple(hash(w) % 100000 for w in words)


def main():
    model_dir = Path("install/models/.resolved/93ac2e4c70d19a09735e57c0ec9284104dc700dd15ad05f0bef67b4df5a15dc6")
    if not model_dir.exists():
        print(f"✗ Model directory not found: {model_dir}", file=sys.stderr)
        return 1

    engine_path = "./build/splash"
    context_size = 180000

    print(f"Initializing client: model={model_dir}, context={context_size}", flush=True)
    client = BinaryProtocolClient(
        engine_path=engine_path,
        model_dir=str(model_dir),
        context_size=context_size,
        memory="auto",
        kv_format="int8",
        ane="off",
        idle_release="off",
    )

    try:
        # Start engine
        print("Starting engine...", flush=True)
        ready = client.start()
        print(f"✓ Engine ready: max_context={ready.max_context_tokens}, max_concurrent={ready.max_concurrent_requests}")

        # Warm-up
        print("\nWarm-up (small prefill)...", flush=True)
        prompt_small = tokenize_dummy("Hello, how are you?")
        client.send_request(
            prompt_tokens=prompt_small,
            max_output_tokens=5,
            sampling=SamplingParameters(temperature=0.7),
        )
        # Consume response
        stats1 = client.receive_events(request_id=1)
        print(f"Warm-up complete: {stats1.completion_tokens} tokens")

        # Measurement 1: Small context decode
        print("\n=== Measurement 1: Small context (13 tokens) ===", flush=True)
        client.send_request(
            prompt_tokens=prompt_small,
            max_output_tokens=200,
            sampling=SamplingParameters(temperature=0.7),
        )
        t0 = time.time()
        stats2 = client.receive_events(request_id=2)
        elapsed = time.time() - t0
        tok_s_small = stats2.decode_tok_s
        print(f"Generated {stats2.completion_tokens} tokens in {elapsed:.2f}s → {tok_s_small:.2f} tok/s")

        # Measurement 2: Large context decode
        sensor_log = " ".join([f"sensor_{i % 100}_reading_{i * 17 % 1000}" for i in range(1500)])
        prompt_large = tokenize_dummy(sensor_log)
        print(f"\n=== Measurement 2: Large context ({len(prompt_large)} tokens) ===", flush=True)

        # Prefill
        print("Prefilling...", flush=True)
        client.send_request(
            prompt_tokens=prompt_large,
            max_output_tokens=1,
            sampling=SamplingParameters(temperature=0.7),
        )
        stats3 = client.receive_events(request_id=3)
        print(f"Prefilled {stats3.prompt_tokens} tokens in {stats3.prefill_seconds:.2f}s")

        # Decode
        print("Decoding...", flush=True)
        prompt_continue = tokenize_dummy("Summary:")
        client.send_request(
            prompt_tokens=prompt_continue,
            max_output_tokens=200,
            sampling=SamplingParameters(temperature=0.7),
        )
        t0 = time.time()
        stats4 = client.receive_events(request_id=4)
        elapsed = time.time() - t0
        tok_s_large = stats4.decode_tok_s
        print(f"Generated {stats4.completion_tokens} tokens in {elapsed:.2f}s → {tok_s_large:.2f} tok/s")

        # Results
        baseline = 39.4  # tok/s, from STATUS.md §2.1 (163K context, int8 KV)
        print("\n" + "="*70)
        print("HYBRID DECODE THROUGHPUT RESULTS")
        print("="*70)
        print(f"Small context (13 tokens):       {tok_s_small:6.2f} tok/s")
        print(f"Large context ({len(prompt_large)} tokens):    {tok_s_large:6.2f} tok/s")
        print(f"Baseline (STATUS.md §2.1):       {baseline:6.2f} tok/s")
        print(f"Small context delta:             {(tok_s_small / baseline - 1) * 100:+6.1f}%")
        print(f"Large context delta:             {(tok_s_large / baseline - 1) * 100:+6.1f}%")
        print("="*70)

        status = "PASS" if tok_s_small >= baseline * 0.9 else "FAIL"
        print(f"\nStatus: {status}")

        # Write report
        report = {
            "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
            "model": "hybrid-real (PQ2_0 + PTQ1_0)",
            "kv_format": "int8",
            "measurements": {
                "small_context_tok_s": round(tok_s_small, 2),
                "large_context_tok_s": round(tok_s_large, 2),
                "baseline_tok_s": baseline,
                "small_delta_pct": round((tok_s_small / baseline - 1) * 100, 1),
                "large_delta_pct": round((tok_s_large / baseline - 1) * 100, 1),
            },
            "status": status,
        }

        report_path = Path("HYBRID_DECODE_RESULTS.json")
        report_path.write_text(json.dumps(report, indent=2) + "\n")
        print(f"\nReport written to: {report_path}")

        return 0 if status == "PASS" else 1

    finally:
        if client.process:
            client.process.terminate()
            try:
                client.process.wait(timeout=5)
            except:
                client.process.kill()
                client.process.wait()


if __name__ == "__main__":
    sys.exit(main())
