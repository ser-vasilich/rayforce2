# Poll Refactor Design

## Overview

Replace the current IPC event loop (ifdefs in function bodies, coupled to REPL) with a proper poll abstraction: one generic event loop in core with callback-based fd registration, platform implementations in separate files, REPL and IPC both as poll clients.

## Architecture

```
core/poll.h        — interface: ray_poll_t, ray_selector_t, callbacks, buffers
core/poll.c        — shared: buffer management, selector array, ray_poll_exit
core/epoll.c       — Linux: poll_create/destroy/register/deregister/run (#if __linux__)
core/kqueue.c      — macOS: same API (#if __APPLE__)
core/iocp.c        — Windows: same API (#if _WIN32)
core/sock.c        — socket abstraction (extracted from ipc.c)
core/ipc.c         — IPC protocol only: listen/accept/handshake/eval/respond + client API

app/main.c         — creates poll, starts IPC + REPL, calls poll_run
app/repl.c         — registers stdin in poll, read callback does term_getc + term_feed
app/term.c         — pure terminal I/O, no event loop, no ipc.h
```

## Poll Interface (poll.h)

```c
/* Selector types */
#define RAY_SEL_STDIN   0
#define RAY_SEL_SOCKET  3

/* Callbacks */
typedef int64_t (*ray_io_fn)(int64_t fd, uint8_t* buf, int64_t len);
typedef ray_t*  (*ray_read_fn)(ray_poll_t* poll, ray_selector_t* sel);
typedef void    (*ray_event_fn)(ray_poll_t* poll, ray_selector_t* sel);
typedef ray_t*  (*ray_data_fn)(ray_poll_t* poll, ray_selector_t* sel, void* data);

/* Buffer */
typedef struct ray_poll_buf {
    struct ray_poll_buf* next;
    int64_t size;
    int64_t offset;
    uint8_t data[];
} ray_poll_buf_t;

/* Selector — one per registered fd */
typedef struct ray_selector {
    int64_t          fd;
    int64_t          id;
    uint8_t          type;
    void*            data;
    ray_event_fn     open_fn;
    ray_event_fn     close_fn;
    ray_event_fn     error_fn;
    ray_data_fn      data_fn;
    struct { ray_poll_buf_t* buf; ray_io_fn recv_fn; ray_read_fn read_fn; } rx;
    struct { ray_poll_buf_t* buf; ray_io_fn send_fn; } tx;
} ray_selector_t;

/* Registration */
typedef struct ray_poll_reg {
    int64_t          fd;
    uint8_t          type;
    ray_event_fn     open_fn;
    ray_event_fn     close_fn;
    ray_event_fn     error_fn;
    ray_data_fn      data_fn;
    ray_io_fn        recv_fn;
    ray_io_fn        send_fn;
    ray_read_fn      read_fn;
    void*            data;
} ray_poll_reg_t;

/* Poll */
typedef struct ray_poll {
    int64_t          fd;       /* epoll/kqueue/iocp handle */
    int64_t          code;     /* exit code (-1 = running) */
    ray_selector_t** sels;     /* selector array */
    uint32_t         n_sels;
    uint32_t         sel_cap;
} ray_poll_t;
```

API:
```c
ray_poll_t*     ray_poll_create(void);
void            ray_poll_destroy(ray_poll_t* poll);
int64_t         ray_poll_register(ray_poll_t* poll, ray_poll_reg_t* reg);
void            ray_poll_deregister(ray_poll_t* poll, int64_t id);
int64_t         ray_poll_run(ray_poll_t* poll);
void            ray_poll_exit(ray_poll_t* poll, int64_t code);
ray_selector_t* ray_poll_get(ray_poll_t* poll, int64_t id);

ray_poll_buf_t* ray_poll_buf_new(int64_t size);
void            ray_poll_buf_free(ray_poll_buf_t* buf);
void            ray_poll_rx_request(ray_poll_t* poll, ray_selector_t* sel, int64_t size);
void            ray_poll_rx_extend(ray_poll_t* poll, ray_selector_t* sel, int64_t size);
void            ray_poll_send(ray_poll_t* poll, ray_selector_t* sel, ray_poll_buf_t* buf);
```

## Platform Files

Each file wraps entire implementation in a platform guard. All three are compiled by the wildcard Makefile — two compile to empty translation units.

- `epoll.c`: `#if defined(__linux__) ... #endif`
- `kqueue.c`: `#if defined(__APPLE__) ... #endif`
- `iocp.c`: `#if defined(_WIN32) ... #endif`

Each implements: `ray_poll_create`, `ray_poll_destroy`, `ray_poll_register`, `ray_poll_deregister`, `ray_poll_run`.

## IPC Protocol (ipc.c)

Thin protocol layer on top of poll:

- `ray_ipc_listen(poll, port)` — socket + register with `read_fn = ipc_accept`
- `ipc_accept` — accept + register new fd with `read_fn = ipc_read_handshake`
- State machine via callback swap: handshake → header → payload → eval → respond
- `ipc_on_data` — eval message, send response via `ray_poll_send`
- Client API: `ray_ipc_connect`, `ray_ipc_send`, `ray_ipc_close` (blocking)
- Compression stays here

## Socket (sock.c)

Pure TCP operations extracted from current ipc.c:

- `ray_sock_listen`, `ray_sock_accept`, `ray_sock_connect`
- `ray_sock_send`, `ray_sock_recv`, `ray_sock_close`
- `ray_sock_set_nonblocking`
- Minimal platform ifdefs (closesocket vs close, WSAStartup)

## REPL (repl.c)

- `ray_repl_create(poll)` — creates term, registers stdin in poll
- `repl_read` callback: `term_getc` + `term_feed`, eval when line complete
- No event loop code

## Terminal (term.c)

- No ipc.h, no epoll, no O_NONBLOCK, no poll_fd
- `ray_term_getc`: simple `read(STDIN_FILENO, ..., 1)` — only called when data ready
- `ray_term_feed`: unchanged event-driven key processing
- VMIN=1, VTIME=0 (blocking mode — poll guarantees readiness before call)

## main.c

```c
ray_poll_t* poll = ray_poll_create();
if (port > 0) ray_ipc_listen(poll, port);
if (file) ray_repl_run_file(file);
ray_repl_t* repl = ray_repl_create(poll);
ray_poll_run(poll);
ray_repl_destroy(repl);
ray_poll_destroy(poll);
```

## Files Changed

| File | Action |
|------|--------|
| `src/core/poll.h` | New: interface |
| `src/core/poll.c` | New: shared code (buffers, selectors) |
| `src/core/epoll.c` | New: Linux event loop |
| `src/core/kqueue.c` | New: macOS event loop |
| `src/core/iocp.c` | New: Windows event loop |
| `src/core/sock.h` | New: socket interface (extracted from ipc.h) |
| `src/core/sock.c` | New: socket impl (extracted from ipc.c) |
| `src/core/ipc.h` | Rewrite: remove socket/poll, keep protocol + client API |
| `src/core/ipc.c` | Rewrite: protocol on top of poll, remove event loop + sockets |
| `src/app/main.c` | Rewrite: create poll, register IPC + REPL, call poll_run |
| `src/app/repl.h` | Remove ipc_srv field |
| `src/app/repl.c` | Rewrite: register stdin in poll, callback-driven |
| `src/app/term.h` | Remove poll_fd, ipc_srv |
| `src/app/term.c` | Simplify: remove epoll, O_NONBLOCK, ipc.h |
