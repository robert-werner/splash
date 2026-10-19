#!/usr/bin/env python3
"""
Binary protocol client for Splash engine.
Sends requests and receives events using the native binary protocol.
"""

import struct
import subprocess
import sys
import time
import os
import fcntl
import threading
import queue
from dataclasses import dataclass
from typing import Optional

# Import protocol implementation
sys.path.insert(0, "server")
from protocol import (
    RequestFrame,
    RequestPriority,
    SamplingParameters,
    ConstraintMode,
    RequestFlag,
    serialize_message,
    FrameParser,
    decode_frame,
    ReadyEvent,
    StartEvent,
    PromptProgressEvent,
    TokensEvent,
    DoneEvent,
    ErrorEvent,
    FinishReason,
)


@dataclass
class RequestStats:
    """Statistics from a completed request."""
    request_id: int
    finish_reason: FinishReason
    prompt_tokens: int
    completion_tokens: int
    prefill_micros: int
    decode_micros: int
    wall_micros: int
    generated_tokens: list[int]
    matched_prefix_tokens: int = 0
    
    @property
    def prefill_tok_s(self) -> float:
        """Prefill throughput in tokens/second."""
        if self.prefill_micros == 0:
            return 0.0
        return (self.prompt_tokens * 1_000_000) / self.prefill_micros
    
    @property
    def decode_tok_s(self) -> float:
        """Decode throughput in tokens/second."""
        if self.decode_micros == 0:
            return 0.0
        return (self.completion_tokens * 1_000_000) / self.decode_micros
    
    @property
    def wall_seconds(self) -> float:
        """Total wall time in seconds."""
        return self.wall_micros / 1_000_000
    
    @property
    def prefill_seconds(self) -> float:
        """Prefill time in seconds."""
        return self.prefill_micros / 1_000_000
    
    @property
    def decode_seconds(self) -> float:
        """Decode time in seconds."""
        return self.decode_micros / 1_000_000


class BinaryProtocolClient:
    """
    Client for Splash engine binary protocol.
    Manages engine subprocess and protocol communication.
    """
    
    def __init__(self, engine_path: str, model_dir: str, context_size: int,
                 memory: str = "auto", kv_format: str = "int8",
                 ane: str = "off", idle_release: str = "off"):
        """
        Initialize the client.
        
        Args:
            engine_path: Path to splash binary
            model_dir: Path to model directory
            context_size: Maximum context tokens
            memory: Memory budget ("auto" or bytes)
            kv_format: KV cache format ("int8" or "bf16")
            ane: Apple Neural Engine ("on" or "off")
            idle_release: Idle release timeout (seconds or "off")
        """
        self.engine_path = engine_path
        self.model_dir = model_dir
        self.context_size = context_size
        self.memory = memory
        self.kv_format = kv_format
        self.ane = ane
        self.idle_release = idle_release
        
        self.process: Optional[subprocess.Popen] = None
        self.parser = FrameParser()
        self.ready_event: Optional[ReadyEvent] = None
        self.next_request_id = 1
        
        # Background reader thread to avoid select() deadlocks with BufferedReader
        self.read_queue: queue.Queue[bytes] = queue.Queue()
        self.reader_thread: Optional[threading.Thread] = None
        self.reader_stop = threading.Event()
    
    def start(self) -> ReadyEvent:
        """Start the engine process and wait for ReadyEvent."""
        cmd = [
            self.engine_path, "serve-native",
            self.model_dir,
            str(self.context_size),
            self.memory,
            "--kv-format", self.kv_format,
            "--ane", self.ane,
            "--idle-release", self.idle_release,
        ]
        
        print(f"Starting engine: {' '.join(cmd)}", file=sys.stderr)
        
        self.process = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=sys.stderr,  # Forward stderr directly to avoid blocking
        )
        
        # Start background reader thread
        self.reader_stop.clear()
        self.reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
        self.reader_thread.start()
        
        # Wait for ReadyEvent
        self.ready_event = self._wait_for_ready()
        print(f"Engine ready: max_context={self.ready_event.max_context_tokens}, "
              f"max_concurrent={self.ready_event.max_concurrent_requests}, "
              f"vision={self.ready_event.vision}", file=sys.stderr)
        
        return self.ready_event
    
    def _reader_loop(self) -> None:
        """Background thread: read stdout and push chunks to queue."""
        if self.process is None or self.process.stdout is None:
            return
        
        try:
            # Set non-blocking mode
            fd = self.process.stdout.fileno()
            fl = fcntl.fcntl(fd, fcntl.F_GETFL)
            fcntl.fcntl(fd, fcntl.F_SETFL, fl | os.O_NONBLOCK)
            
            while not self.reader_stop.is_set():
                try:
                    chunk = os.read(fd, 4096)
                    if not chunk:
                        # EOF
                        break
                    self.read_queue.put(chunk)
                except BlockingIOError:
                    # No data available, sleep briefly
                    time.sleep(0.01)
                except Exception:
                    # Process likely died
                    break
        except Exception:
            pass
    
    def _wait_for_ready(self, timeout: float = 30.0) -> ReadyEvent:
        """Wait for ReadyEvent from engine."""
        if self.process is None:
            raise RuntimeError("Engine process not started")
        
        start = time.time()
        while True:
            remaining = timeout - (time.time() - start)
            if remaining <= 0:
                raise TimeoutError("Timed out waiting for ReadyEvent")
            
            # Check if process died
            if self.process.poll() is not None:
                raise RuntimeError(f"Engine exited (code {self.process.returncode}) without ReadyEvent")
            
            # Read from queue with timeout
            try:
                chunk = self.read_queue.get(timeout=min(remaining, 1.0))
            except queue.Empty:
                continue
            
            # Parse frames
            step = self.parser.consume(chunk)
            if step.issue:
                raise RuntimeError(f"Protocol error: {step.issue.describe()}")
            
            if step.frame:
                event = decode_frame(step.frame)
                if isinstance(event, ReadyEvent):
                    return event
                elif isinstance(event, ErrorEvent):
                    msg = event.message.decode('utf-8', errors='replace')
                    raise RuntimeError(f"Engine error: {msg}")
    
    def send_request(
        self,
        prompt_tokens: tuple[int, ...],
        max_output_tokens: int = 50,
        sampling: Optional[SamplingParameters] = None,
        report_progress: bool = False,
    ) -> int:
        """
        Send a generation request to the engine.
        
        Args:
            prompt_tokens: Token IDs for the prompt
            max_output_tokens: Maximum tokens to generate
            sampling: Sampling parameters (default: greedy)
            report_progress: Enable PromptProgressEvent during prefill
        
        Returns:
            request_id: ID for tracking this request
        """
        request_id = self.next_request_id
        self.next_request_id += 1
        
        if sampling is None:
            sampling = SamplingParameters()  # Greedy by default
        
        # Deadline: 10 minutes from now
        deadline_micros = 10 * 60 * 1_000_000
        now_micros = int(time.time() * 1_000_000)
        
        flags = RequestFlag(0)
        if report_progress:
            flags |= RequestFlag.RETURN_PROGRESS
        
        request = RequestFrame(
            request_id=request_id,
            priority=RequestPriority.FOREGROUND,
            absolute_deadline_unix_micros=now_micros + deadline_micros,
            remaining_deadline_micros=deadline_micros,
            logical_max_output_tokens=max_output_tokens,
            prompt_tokens=prompt_tokens,
            sampling=sampling,
            seed=0,  # Deterministic
            constraint=ConstraintMode.NONE,
            image_spans=(),
            image_pixels=b"",
            score_tokens=(),
            generation_prompt_tokens=0,
            flags=flags,
            cache_salt=0,
            thinking_budget=None,
        )
        
        # Serialize and send
        frame_bytes = serialize_message(request)
        self.process.stdin.write(frame_bytes)
        self.process.stdin.flush()
        
        return request_id
    
    def receive_events(self, request_id: int) -> RequestStats:
        """
        Receive events for a request until DoneEvent.
        
        Args:
            request_id: Request ID to track
        
        Returns:
            RequestStats with performance metrics and generated tokens
        """
        if self.process is None:
            raise RuntimeError("Engine process not started")
        
        generated_tokens = []
        matched_prefix = 0
        progress_reports = []
        
        start_time = time.time()
        global_timeout = 120.0  # 2 minutes total timeout
        
        while True:
            # Check global timeout
            elapsed = time.time() - start_time
            if elapsed > global_timeout:
                raise RuntimeError(
                    f"Request {request_id} timed out after {elapsed:.1f}s "
                    f"(generated {len(generated_tokens)} tokens, no DoneEvent received)"
                )
            
            # Check if process died
            if self.process.poll() is not None:
                raise RuntimeError(f"Engine exited (code {self.process.returncode}) mid-request")
            
            # Read from queue with timeout
            try:
                chunk = self.read_queue.get(timeout=10.0)
            except queue.Empty:
                continue
            
            # Parse frames (loop until all frames in chunk consumed)
            remaining_chunk = chunk
            while remaining_chunk:
                step = self.parser.consume(remaining_chunk)
                if step.issue:
                    raise RuntimeError(f"Protocol error: {step.issue.describe()}")
                
                # Advance to unconsumed portion
                remaining_chunk = remaining_chunk[step.consumed_bytes:]
                
                if not step.frame:
                    # Need more data
                    break
                
                event = decode_frame(step.frame)
                
                if isinstance(event, StartEvent):
                    if event.request_id == request_id:
                        matched_prefix = event.matched_prompt_tokens
                        print(f"Request {request_id} started on lane {event.lane}, "
                              f"matched {matched_prefix} prefix tokens", file=sys.stderr)
                
                elif isinstance(event, PromptProgressEvent):
                    if event.request_id == request_id:
                        progress_reports.append((event.processed_tokens, event.elapsed_micros))
                        print(f"Prefill progress: {event.processed_tokens} tokens in "
                              f"{event.elapsed_micros / 1_000_000:.3f}s", file=sys.stderr)
                
                elif isinstance(event, TokensEvent):
                    if event.request_id == request_id:
                        generated_tokens.extend(event.tokens)
                        print(f"Generated {len(event.tokens)} tokens", file=sys.stderr)
                
                elif isinstance(event, DoneEvent):
                    if event.request_id == request_id:
                        stats = RequestStats(
                            request_id=request_id,
                            finish_reason=event.reason,
                            prompt_tokens=event.prompt_tokens,
                            completion_tokens=event.completion_tokens,
                            prefill_micros=event.prefill_micros,
                            decode_micros=event.decode_micros,
                            wall_micros=event.wall_micros,
                            generated_tokens=generated_tokens,
                            matched_prefix_tokens=matched_prefix,
                        )
                        return stats
                
                elif isinstance(event, ErrorEvent):
                    if event.request_id == request_id:
                        code = event.code.decode('utf-8', errors='replace')
                        msg = event.message.decode('utf-8', errors='replace')
                        raise RuntimeError(f"Request {request_id} error: {code}: {msg}")
    
    def shutdown(self):
        """Shutdown the engine process."""
        # Stop reader thread
        self.reader_stop.set()
        if self.reader_thread:
            self.reader_thread.join(timeout=2.0)
        
        if self.process:
            self.process.stdin.close()
            self.process.wait(timeout=10)
            print(f"Engine exited with code {self.process.returncode}", file=sys.stderr)


def main():
    """Simple test of the binary protocol client."""
    import sys
    
    if len(sys.argv) < 3:
        print("Usage: binary_protocol_client.py <engine_path> <model_dir> [context_size]")
        sys.exit(1)
    
    engine_path = sys.argv[1]
    model_dir = sys.argv[2]
    context_size = int(sys.argv[3]) if len(sys.argv) > 3 else 2048
    
    # Simple test prompt: "Hello world" token IDs (example)
    # In practice, you'd use a tokenizer to encode text
    test_tokens = tuple(range(100, 120))  # Dummy token IDs
    
    client = BinaryProtocolClient(engine_path, model_dir, context_size)
    
    try:
        client.start()
        
        request_id = client.send_request(
            prompt_tokens=test_tokens,
            max_output_tokens=10,
            report_progress=True,
        )
        
        stats = client.receive_events(request_id)
        
        print("\n=== Request Stats ===")
        print(f"Request ID: {stats.request_id}")
        print(f"Finish reason: {stats.finish_reason.name}")
        print(f"Prompt tokens: {stats.prompt_tokens}")
        print(f"Completion tokens: {stats.completion_tokens}")
        print(f"Prefill: {stats.prefill_seconds:.3f}s ({stats.prefill_tok_s:.1f} tok/s)")
        print(f"Decode: {stats.decode_seconds:.3f}s ({stats.decode_tok_s:.1f} tok/s)")
        print(f"Wall time: {stats.wall_seconds:.3f}s")
        print(f"Generated tokens: {len(stats.generated_tokens)}")
        
    finally:
        client.shutdown()


if __name__ == "__main__":
    main()
