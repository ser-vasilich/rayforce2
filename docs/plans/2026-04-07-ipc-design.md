# IPC Design

## Overview

Single-threaded async IPC server/client for Rayforce2. TCP-based, uses existing serde wire format, supports sync/async messaging, RLE/delta compression for large payloads. No external dependencies.

## Protocol

Uses existing `ray_ipc_header_t` (16 bytes) from `serde.h`:

```
Bytes 0-3:   prefix    0xcefadefa
Byte  4:     version   RAY_VERSION_MAJOR
Byte  5:     flags     bit 0: compressed (0=no, 1=yes)
Byte  6:     endian    0=little, 1=big
Byte  7:     msgtype   0=async, 1=sync, 2=response
Bytes 8-15:  size      payload size in bytes (compressed size if compressed)
```

**Message types:**
- Async (0): fire-and-forget, server evals, no response
- Sync (1): request-response, server evals, sends result as msgtype=2
- Response (2): result of a sync request

**Payload**: serialized `ray_t` via `ray_ser_raw`/`ray_de_raw`. String payload → `ray_eval_str()`. Object payload → `ray_eval()`.

**Handshake**: client sends version byte on connect. Server validates compatibility.

**Compression**: if serialized payload > 2000 bytes, attempt RLE/delta. If smaller, set flags bit 0, prepend 4-byte uncompressed size. Otherwise send uncompressed.

Compressed wire format:
```
[header with flags bit 0 set, size = compressed_size + 4]
[uint32 uncompressed_size]
[compressed payload]
```

## Socket Abstraction

Thin wrapper in `src/core/ipc.{h,c}`:

```c
typedef int ray_sock_t;  /* fd on Unix, SOCKET on Windows */

ray_sock_t ray_sock_listen(uint16_t port);
ray_sock_t ray_sock_accept(ray_sock_t srv);
ray_sock_t ray_sock_connect(const char* host, uint16_t port);
ray_err_t  ray_sock_send(ray_sock_t s, const void* buf, size_t len);
ray_err_t  ray_sock_recv(ray_sock_t s, void* buf, size_t len);
void       ray_sock_close(ray_sock_t s);
ray_err_t  ray_sock_set_nonblocking(ray_sock_t s);
```

## Event Loop

Single-threaded, uses epoll (Linux), kqueue (macOS), select (Windows fallback).

```c
typedef struct ray_ipc_server {
    ray_sock_t        listen_fd;
    int               poll_fd;       /* epoll_fd or kqueue_fd */
    ray_ipc_conn_t*   conns;         /* active connections */
    uint32_t          n_conns;
    uint32_t          conn_cap;
    bool              running;
} ray_ipc_server_t;

typedef struct ray_ipc_conn {
    ray_sock_t         fd;
    uint8_t*           rx_buf;      /* receive buffer (dynamic) */
    size_t             rx_len;      /* bytes received so far */
    size_t             rx_need;     /* bytes needed for current phase */
    uint8_t            phase;       /* 0=handshake, 1=header, 2=payload */
    ray_ipc_header_t   hdr;        /* current message header */
} ray_ipc_conn_t;
```

Event loop cycle:
1. epoll_wait/kevent with all fds (listen, clients, stdin)
2. stdin ready → process REPL input
3. listen socket ready → accept new connection
4. client socket ready → read, assemble message, eval, respond
5. Loop

## Compression

RLE/delta encoding: delta-encode bytes (current - previous), then RLE runs.

```c
size_t ray_ipc_compress(const uint8_t* src, size_t len,
                        uint8_t* dst, size_t dst_cap);
size_t ray_ipc_decompress(const uint8_t* src, size_t len,
                          uint8_t* dst, size_t dst_len);
```

RLE format: (count, value) pairs. Count varint-encoded (1 byte ≤ 127, 2 bytes ≤ 32767). Literal runs use negative count prefix + raw bytes.

Threshold: only compress when serialized payload > 2000 bytes and result is smaller.

## Server Mode

`./rayforce -p PORT` starts the IPC server. REPL stays interactive — stdin is in the same poll set as IPC sockets.

```
./rayforce -p 5000              # server + REPL
./rayforce -p 5000 script.rfl   # run script, then serve
```

Non-TTY mode: after script finishes, keep event loop running if -p specified.

## Builtins

In `src/ops/system.c`:

- `(hopen "host:port")` → connect, handshake, return handle (i64)
- `(hclose handle)` → close connection
- `(hsend handle msg)` → serialize, send sync, wait for response, return result

Handles stored in global array indexed by fd. Max 256 connections.

Server-side eval:
1. Deserialize payload → ray_t*
2. String → ray_eval_str(), object → ray_eval()
3. Serialize result, send as response (msgtype=2)
4. Async: eval, discard result

## Files

| File | Change |
|------|--------|
| `src/core/ipc.{h,c}` | New: socket abstraction, event loop, compression, server/client |
| `src/ops/system.c` | Add hopen/hclose/hsend builtins |
| `src/lang/eval.c` | Register hopen/hclose/hsend |
| `src/app/main.c` | Parse -p flag, start server, integrate event loop with REPL |
| `src/app/repl.{h,c}` | Expose REPL input processing for event loop integration |
