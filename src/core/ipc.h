/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.

 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:

 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.

 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#ifndef RAY_IPC_H
#define RAY_IPC_H

#include <rayforce.h>
#include "store/serde.h"

/* ===== Socket Abstraction ===== */

#ifdef _WIN32
  typedef intptr_t ray_sock_t;
  #define RAY_INVALID_SOCK ((ray_sock_t)-1)
#else
  typedef int ray_sock_t;
  #define RAY_INVALID_SOCK (-1)
#endif

ray_sock_t ray_sock_listen(uint16_t port);
ray_sock_t ray_sock_accept(ray_sock_t srv);
ray_sock_t ray_sock_connect(const char* host, uint16_t port, int timeout_ms);
int64_t    ray_sock_send(ray_sock_t s, const void* buf, size_t len);
int64_t    ray_sock_recv(ray_sock_t s, void* buf, size_t len);
void       ray_sock_close(ray_sock_t s);
ray_err_t  ray_sock_set_nonblocking(ray_sock_t s);

/* ===== Compression ===== */

#define RAY_IPC_COMPRESS_THRESHOLD 2000

size_t ray_ipc_compress(const uint8_t* src, size_t len,
                        uint8_t* dst, size_t dst_cap);
size_t ray_ipc_decompress(const uint8_t* src, size_t clen,
                          uint8_t* dst, size_t dst_len);

/* ===== Connection State ===== */

#define RAY_IPC_PHASE_HANDSHAKE 0
#define RAY_IPC_PHASE_HEADER    1
#define RAY_IPC_PHASE_PAYLOAD   2

#define RAY_IPC_MAX_CONNS 256

#define RAY_IPC_MSG_ASYNC  0
#define RAY_IPC_MSG_SYNC   1
#define RAY_IPC_MSG_RESP   2

#define RAY_IPC_FLAG_COMPRESSED 0x01

typedef struct ray_ipc_conn {
    ray_sock_t        fd;
    uint8_t*          rx_buf;
    size_t            rx_len;
    size_t            rx_need;
    uint8_t           phase;
    ray_ipc_header_t  hdr;
} ray_ipc_conn_t;

/* ===== Server ===== */

typedef struct ray_ipc_server {
    ray_sock_t        listen_fd;
    int               poll_fd;
    ray_ipc_conn_t    conns[RAY_IPC_MAX_CONNS];
    uint32_t          n_conns;
    bool              running;
} ray_ipc_server_t;

ray_err_t ray_ipc_server_init(ray_ipc_server_t* srv, uint16_t port);
void      ray_ipc_server_destroy(ray_ipc_server_t* srv);
int       ray_ipc_poll(ray_ipc_server_t* srv, int timeout_ms);
ray_err_t ray_ipc_watch_fd(ray_ipc_server_t* srv, int fd);

/* Attach IPC server to an external poll fd (e.g., terminal's epoll/kqueue).
 * Moves the listen socket to the new poll fd. The server's own poll_fd is
 * closed and replaced. */
void      ray_ipc_attach(ray_ipc_server_t* srv, int poll_fd);

/* ===== Client ===== */

int64_t   ray_ipc_connect(const char* host, uint16_t port);
void      ray_ipc_close(int64_t handle);
ray_t*    ray_ipc_send(int64_t handle, ray_t* msg);
ray_err_t ray_ipc_send_async(int64_t handle, ray_t* msg);

#endif /* RAY_IPC_H */
