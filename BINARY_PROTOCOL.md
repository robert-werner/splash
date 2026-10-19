# Splash Binary Protocol Specification

This document specifies the binary protocol used by `serve-native` for stdin/stdout communication with the Splash inference engine.

## Overview

The protocol uses length-prefixed binary frames for bidirectional communication:
- **Client → Engine**: Request, Cancel, MaskResponse, StatusRequest, ReleaseRequest
- **Engine → Client**: Ready, Start, PromptProgress, Tokens, Done, Error, StatusJson, MaskRequest

All integers are **little-endian**. The protocol is defined in `runtime/engine/Protocol.{hpp,cpp}` (C++) and `server/protocol.py` (Python).

## Protocol Constants

```
Magic:              "SPLH" (4 bytes)
Protocol Version:   10 (uint16)
Frame Header Size:  16 bytes
Max Frame Payload:  256 MiB
```

## Frame Structure

Every frame consists of a **16-byte header** followed by a variable-length payload:

```
Offset | Size | Type   | Field
-------|------|--------|------------------
0      | 4    | bytes  | magic ("SPLH")
4      | 2    | uint16 | protocol_version (10)
6      | 2    | uint16 | frame_type
8      | 8    | uint64 | payload_length
16     | N    | bytes  | payload (N = payload_length)
```

## Frame Types

### Client Frames (sent to engine)

| Type            | Value  | Description                          |
|-----------------|--------|--------------------------------------|
| Request         | 0x0001 | Generate text from prompt            |
| Cancel          | 0x0002 | Cancel a running request             |
| MaskResponse    | 0x0003 | Token mask for constrained generation|
| StatusRequest   | 0x0004 | Query engine status                  |
| ReleaseRequest  | 0x0005 | Release model weights from memory    |

### Engine Events (received from engine)

| Type            | Value  | Description                          |
|-----------------|--------|--------------------------------------|
| Ready           | 0x0100 | Engine initialized and ready         |
| Start           | 0x0101 | Request execution started            |
| Tokens          | 0x0102 | Generated tokens                     |
| MaskRequest     | 0x0103 | Request token mask for constraints   |
| Done            | 0x0104 | Request completed                    |
| Error           | 0x0105 | Error occurred                       |
| StatusJson      | 0x0107 | Engine status (JSON)                 |
| PromptProgress  | 0x0108 | Prompt prefill progress              |

## Request Frame (0x0001)

The Request frame carries a text generation request. Its payload has a **fixed 114-byte header** followed by variable-length data.

### Fixed Header (114 bytes)

```
Offset | Size | Type   | Field
-------|------|--------|---------------------------
0      | 8    | uint64 | request_id (non-zero)
8      | 1    | uint8  | priority (0=foreground, 1=normal, 2=background)
9      | 1    | uint8  | constraint (0=none, 1=token_mask)
10     | 8    | uint64 | absolute_deadline_unix_micros
18     | 8    | uint64 | remaining_deadline_micros
26     | 4    | uint32 | logical_max_output_tokens
30     | 4    | uint32 | prompt_token_count
34     | 4    | uint32 | image_span_count
38     | 4    | float  | temperature
42     | 4    | float  | top_p
46     | 4    | uint32 | top_k
50     | 4    | float  | presence_penalty
54     | 4    | float  | frequency_penalty
58     | 4    | float  | repetition_penalty
62     | 4    | float  | min_p
66     | 8    | uint64 | seed
74     | 4    | uint32 | score_token_count
78     | 4    | uint32 | generation_prompt_tokens
82     | 4    | uint32 | flags (bit 1 = return_progress)
86     | 8    | uint64 | cache_salt_lo
94     | 8    | uint64 | cache_salt_hi
102    | 4    | uint32 | thinking_budget_tokens (0xFFFFFFFF = none)
106    | 4    | uint32 | thinking_end_token_count
110    | 4    | uint32 | thinking_budget_also_ends
```

### Variable Payload (after fixed header)

```
1. prompt_tokens: uint32[prompt_token_count]
2. image_spans: ImageSpan[image_span_count] (32 bytes each)
3. image_pixels: uint8[sum(span.pixel_bytes())]
4. score_tokens: uint32[score_token_count]
5. thinking_end_tokens: uint32[thinking_end_token_count]
```

### ImageSpan Structure (32 bytes)

```
Offset | Size | Type   | Field
-------|------|--------|------------------
0      | 4    | uint32 | offset (in prompt)
4      | 4    | uint32 | tokens (span length)
8      | 4    | uint32 | grid_height (patches)
12     | 4    | uint32 | grid_width (patches)
16     | 8    | uint64 | digest_lo
24     | 8    | uint64 | digest_hi
```

Pixel bytes per span: `grid_height * 16 * grid_width * 16 * 3` (RGB uint8)

### Sampling Parameters

- **temperature**: Sampling randomness (0.0 = greedy, >1.0 = more random)
- **top_p**: Nucleus sampling threshold (1.0 = disabled)
- **top_k**: Top-K sampling (0 = disabled)
- **presence_penalty**: Penalize repeated token presence
- **frequency_penalty**: Penalize repeated token frequency
- **repetition_penalty**: Multiplicative repetition penalty
- **min_p**: Minimum probability threshold
- **seed**: Random seed (0 = time-based)

## Ready Event (0x0100)

Engine is initialized and ready to accept requests.

```
Offset | Size | Type   | Field
-------|------|--------|---------------------------
0      | 4    | uint32 | max_concurrent_requests
4      | 4    | uint32 | max_context_tokens
8      | 1    | uint8  | vision (0=text-only, 1=supports images)
```

## Start Event (0x0101)

Request execution has started.

```
Offset | Size | Type   | Field
-------|------|--------|---------------------------
0      | 8    | uint64 | request_id
8      | 4    | uint32 | lane (execution slot)
12     | 4    | uint32 | matched_prompt_tokens (cache hit)
```

## PromptProgress Event (0x0108)

Periodic updates during prompt prefill (if `return_progress` flag is set).

```
Offset | Size | Type   | Field
-------|------|--------|---------------------------
0      | 8    | uint64 | request_id
8      | 4    | uint32 | processed_tokens
12     | 8    | uint64 | elapsed_micros
```

## Tokens Event (0x0102)

Generated tokens from the model.

```
Offset | Size | Type   | Field
-------|------|--------|---------------------------
0      | 8    | uint64 | request_id
8      | 4    | uint32 | sequence_offset (position in output)
12     | 4    | uint32 | token_count
16     | N*4  | uint32 | tokens[token_count]
```

## Done Event (0x0104)

Request completed successfully or was cancelled.

```
Offset | Size | Type   | Field
-------|------|--------|---------------------------
0      | 8    | uint64 | request_id
8      | 1    | uint8  | reason (0=stop, 1=length, 2=cancelled)
9      | 4    | uint32 | prompt_tokens
13     | 4    | uint32 | completion_tokens
17     | 8    | uint64 | prefill_micros
25     | 8    | uint64 | decode_micros
33     | 8    | uint64 | wall_micros
41     | 4    | uint32 | deduplicated_tokens
45     | 4    | uint32 | drafted_tokens (speculative)
49     | 4    | uint32 | accepted_draft_tokens
53     | 4    | uint32 | speculation_cycles
57     | 4    | uint32 | option_logit_count
61     | N*4  | float  | option_logits[option_logit_count]
```

### Finish Reasons

- **0 (Stop)**: Generation completed naturally (stop token)
- **1 (Length)**: Reached `logical_max_output_tokens` limit
- **2 (Cancelled)**: Request was cancelled by client

### Timing Fields

All timing values are in **microseconds**:
- **prefill_micros**: Time to process prompt tokens
- **decode_micros**: Time to generate completion tokens
- **wall_micros**: Total time from request start to completion

Calculate throughput:
```
prefill_tok/s = prompt_tokens * 1_000_000 / prefill_micros
decode_tok/s = completion_tokens * 1_000_000 / decode_micros
```

## Error Event (0x0105)

An error occurred during request processing.

```
Offset | Size | Type   | Field
-------|------|--------|---------------------------
0      | 1    | uint8  | failure_class (1=request, 2=engine, 3=protocol)
1      | 1    | uint8  | retryable (0=no, 1=yes)
2      | 8    | uint64 | request_id (0 for protocol errors)
10     | 4    | uint32 | code_length
14     | 4    | uint32 | message_length
18     | N    | bytes  | code (UTF-8)
18+N   | M    | bytes  | message (UTF-8)
```

### Failure Classes

- **1 (RequestError)**: Bad request parameters, can retry with different request
- **2 (EngineUnhealthy)**: Engine failure, supervisor should restart engine
- **3 (ProtocolFatal)**: Protocol violation, must close connection

## Cancel Frame (0x0002)

Cancel a running request.

```
Offset | Size | Type   | Field
-------|------|--------|---------------------------
0      | 8    | uint64 | request_id (must be non-zero)
```

## StatusRequest Frame (0x0004)

Query engine status (returns StatusJson event).

```
Offset | Size | Type   | Field
-------|------|--------|---------------------------
0      | 8    | uint64 | correlation_id
```

## StatusJson Event (0x0107)

Engine status response (JSON document).

```
Offset | Size | Type   | Field
-------|------|--------|---------------------------
0      | 8    | uint64 | correlation_id
8      | N    | bytes  | json (UTF-8)
```

## Example: Minimal Request

Here's a minimal Request frame for "Hello" (assuming token IDs [15496, 29892]):

```python
import struct
import time

# Frame header
magic = b'SPLH'
version = 10
frame_type = 0x0001  # Request
prompt_tokens = [15496, 29892]
payload_size = 114 + len(prompt_tokens) * 4

header = magic + struct.pack('<HHQ', version, frame_type, payload_size)

# Request payload (114 bytes fixed)
request_id = 1
priority = 1  # Normal
constraint = 0  # None
now_micros = int(time.time() * 1_000_000)
deadline_micros = 300 * 1_000_000  # 5 minutes

payload = struct.pack(
    '<QBBQQIIIIIIIIIIIQQQQIII',
    request_id,                        # 0: request_id
    priority,                          # 8: priority
    constraint,                        # 9: constraint
    now_micros + deadline_micros,      # 10: absolute_deadline
    deadline_micros,                   # 18: remaining_deadline
    50,                                # 26: max_output_tokens
    len(prompt_tokens),                # 30: prompt_token_count
    0,                                 # 34: image_span_count
    # Sampling parameters (38-73)
    # temperature, top_p, top_k, presence, frequency, repetition, min_p, seed
    *struct.unpack('8I', struct.pack('8f', 1.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0)),
    0,                                 # 74: score_token_count
    0,                                 # 78: generation_prompt_tokens
    0,                                 # 82: flags
    0, 0,                              # 86-93: cache_salt
    0xFFFFFFFF,                        # 102: thinking_budget (none)
    0, 0                               # 106-113: thinking_end_count, also_ends
)

# Append prompt tokens
for token in prompt_tokens:
    payload += struct.pack('<I', token)

frame = header + payload
```

## Error Handling

### Exit Code 76 (EX_PROTOCOL)

The engine exits with code 76 when it receives malformed protocol frames:
- Bad magic bytes
- Wrong protocol version
- Invalid frame type
- Payload length mismatch
- Truncated frame

### Connection Lifecycle

1. Client starts `serve-native` subprocess with stdin/stdout pipes
2. Engine sends **Ready** event
3. Client sends **Request** frames
4. Engine sends **Start**, **PromptProgress** (optional), **Tokens**, **Done** events
5. On clean shutdown: client closes stdin, engine flushes pending events and exits
6. On error: engine sends **Error** event, may exit depending on failure class

## Implementation Notes

### Little-Endian Encoding

All multi-byte integers use little-endian byte order:

```python
# Pack uint32 = 0x12345678
bytes([0x78, 0x56, 0x34, 0x12])

# Pack uint64 = 0x123456789ABCDEF0
bytes([0xF0, 0xDE, 0xBC, 0x9A, 0x78, 0x56, 0x34, 0x12])
```

### Float Encoding

Floats use IEEE 754 single precision (32-bit):

```python
import struct
temperature = 0.7
bytes_le = struct.pack('<f', temperature)  # Little-endian float
```

### Frame Parsing

Read frames incrementally:

```python
def read_frame(stream):
    # Read header
    header = stream.read(16)
    if len(header) < 16:
        raise EOFError("Incomplete header")
    
    magic, version, frame_type, payload_length = struct.unpack('<4sHHQ', header)
    
    if magic != b'SPLH':
        raise ValueError(f"Bad magic: {magic!r}")
    if version != 10:
        raise ValueError(f"Wrong version: {version}")
    
    # Read payload
    payload = stream.read(payload_length)
    if len(payload) < payload_length:
        raise EOFError("Incomplete payload")
    
    return frame_type, payload
```

## Testing

Test the protocol with `binary_protocol_client.py`:

```bash
# Run with 2K token prompt
python3 test_binary_client.py

# The client will:
# 1. Start serve-native subprocess
# 2. Wait for Ready event
# 3. Tokenize prompt_2k.txt
# 4. Send Request frame
# 5. Receive and parse events
# 6. Report timing and throughput
```

Expected output:
```
Engine ready: max_context=8192, concurrent=1, vision=False
Sending request with 2048 tokens...
Request 1 started on lane 0, matched 0 prefix tokens
Prefill progress: 2048 tokens in 0.234s
Generated 50 tokens

=== Request Stats ===
Prompt tokens: 2048
Completion tokens: 50
Prefill: 0.234s (8752.1 tok/s)
Decode: 0.789s (63.4 tok/s)
Wall time: 1.023s
```

## References

- **Protocol Implementation**: `runtime/engine/Protocol.{hpp,cpp}`
- **Python Codec**: `server/protocol.py`
- **Client Library**: `binary_protocol_client.py`
- **Golden Tests**: `dev/tests/engine/protocol_golden.txt`
- **Main Entry**: `runtime/main.mm` (serve-native command)
