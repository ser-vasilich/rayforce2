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

#ifndef _WIN32
  #define _GNU_SOURCE
#endif

#include "core/ipc.h"
#include "mem/sys.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <fcntl.h>
  #include <netdb.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <arpa/inet.h>
  #include <netinet/tcp.h>
  #include <unistd.h>
#endif

#if defined(__linux__)
  #include <sys/epoll.h>
  #define RAY_IPC_MAX_EVENTS 64
#elif defined(__APPLE__)
  #include <sys/event.h>
  #define RAY_IPC_MAX_EVENTS 64
#endif

#include "lang/eval.h"

/* ===== Socket Abstraction ===== */

ray_sock_t ray_sock_listen(uint16_t port)
{
    ray_sock_t fd = (ray_sock_t)socket(AF_INET, SOCK_STREAM, 0);
    if (fd == RAY_INVALID_SOCK) return RAY_INVALID_SOCK;

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ray_sock_close(fd);
        return RAY_INVALID_SOCK;
    }
    if (listen(fd, 128) < 0) {
        ray_sock_close(fd);
        return RAY_INVALID_SOCK;
    }
    return fd;
}

ray_sock_t ray_sock_accept(ray_sock_t srv)
{
    ray_sock_t fd;
    do {
        fd = (ray_sock_t)accept(srv, NULL, NULL);
    } while (fd == RAY_INVALID_SOCK && errno == EINTR);

    if (fd == RAY_INVALID_SOCK) return RAY_INVALID_SOCK;

    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&yes, sizeof(yes));
    return fd;
}

ray_sock_t ray_sock_connect(const char* host, uint16_t port, int timeout_ms)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
        return RAY_INVALID_SOCK;

    ray_sock_t fd = (ray_sock_t)socket(res->ai_family, res->ai_socktype,
                                        res->ai_protocol);
    if (fd == RAY_INVALID_SOCK) {
        freeaddrinfo(res);
        return RAY_INVALID_SOCK;
    }

    /* Set send/recv timeout if requested */
    if (timeout_ms > 0) {
#ifdef _WIN32
        DWORD tv = (DWORD)timeout_ms;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
#else
        struct timeval tv;
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
    }

    if (connect(fd, res->ai_addr, (socklen_t)res->ai_addrlen) < 0) {
        ray_sock_close(fd);
        freeaddrinfo(res);
        return RAY_INVALID_SOCK;
    }
    freeaddrinfo(res);

    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&yes, sizeof(yes));
    return fd;
}

int64_t ray_sock_send(ray_sock_t s, const void* buf, size_t len)
{
    const uint8_t* p   = (const uint8_t*)buf;
    size_t         rem = len;
    while (rem > 0) {
#ifdef _WIN32
        int n = send(s, (const char*)p, (int)rem, 0);
#else
        ssize_t n = send(s, p, rem, MSG_NOSIGNAL);
#endif
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return -1;
        }
        p   += n;
        rem -= (size_t)n;
    }
    return (int64_t)len;
}

int64_t ray_sock_recv(ray_sock_t s, void* buf, size_t len)
{
    for (;;) {
#ifdef _WIN32
        int n = recv(s, (char*)buf, (int)len, 0);
#else
        ssize_t n = recv(s, buf, len, 0);
#endif
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        return (int64_t)n;   /* 0 = peer closed */
    }
}

void ray_sock_close(ray_sock_t s)
{
    if (s == RAY_INVALID_SOCK) return;
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

ray_err_t ray_sock_set_nonblocking(ray_sock_t s)
{
#ifdef _WIN32
    u_long mode = 1;
    if (ioctlsocket(s, FIONBIO, &mode) != 0)
        return RAY_ERR_IO;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return RAY_ERR_IO;
    if (fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0)
        return RAY_ERR_IO;
#endif
    return RAY_OK;
}

/* ===== Compression (delta + RLE) ===== */

size_t ray_ipc_compress(const uint8_t* src, size_t len,
                        uint8_t* dst, size_t dst_cap)
{
    if (len <= RAY_IPC_COMPRESS_THRESHOLD) return 0;

    /* Step 1: delta-encode into temporary buffer */
    uint8_t* delta = (uint8_t*)ray_sys_alloc(len);
    if (!delta) return 0;

    delta[0] = src[0];
    for (size_t i = 1; i < len; i++)
        delta[i] = (uint8_t)(src[i] - src[i - 1]);

    /* Step 2: RLE-compress the delta stream */
    size_t di = 0;   /* destination index */
    size_t si = 0;   /* source index into delta */

    while (si < len) {
        /* Check for a run of identical bytes (need at least 2) */
        if (si + 1 < len && delta[si] == delta[si + 1]) {
            uint8_t val = delta[si];
            size_t run = 1;
            while (si + run < len && delta[si + run] == val && run < 127)
                run++;
            if (di + 2 > dst_cap) { ray_sys_free(delta); return 0; }
            dst[di++] = (uint8_t)run;        /* positive count = run */
            dst[di++] = val;
            si += run;
        } else {
            /* Literal sequence: collect non-repeating bytes */
            size_t start = si;
            size_t llen = 0;
            while (si < len && llen < 128) {
                /* Stop if we see a run of 2+ identical bytes ahead */
                if (si + 1 < len && delta[si] == delta[si + 1])
                    break;
                si++;
                llen++;
            }
            /* Encode: negative count followed by raw bytes */
            if (di + 1 + llen > dst_cap) { ray_sys_free(delta); return 0; }
            dst[di++] = (uint8_t)(-(int8_t)llen);  /* -1..-128 */
            memcpy(dst + di, delta + start, llen);
            di += llen;
        }
    }

    ray_sys_free(delta);

    /* Not worth compressing if result >= original */
    if (di >= len) return 0;
    return di;
}

size_t ray_ipc_decompress(const uint8_t* src, size_t clen,
                          uint8_t* dst, size_t dst_len)
{
    /* Step 1: RLE-decode */
    uint8_t* decoded = (uint8_t*)ray_sys_alloc(dst_len);
    if (!decoded) return 0;

    size_t si = 0;   /* source index */
    size_t di = 0;   /* decoded index */

    while (si < clen && di < dst_len) {
        int8_t count = (int8_t)src[si++];
        if (count > 0) {
            /* Run: repeat single byte count times */
            if (si >= clen) { ray_sys_free(decoded); return 0; }
            uint8_t val = src[si++];
            size_t n = (size_t)count;
            if (di + n > dst_len) { ray_sys_free(decoded); return 0; }
            memset(decoded + di, val, n);
            di += n;
        } else {
            /* Literal: copy |count| bytes */
            size_t n = (size_t)(-(int)count);
            if (si + n > clen || di + n > dst_len) {
                ray_sys_free(decoded);
                return 0;
            }
            memcpy(decoded + di, src + si, n);
            si += n;
            di += n;
        }
    }

    /* Step 2: un-delta */
    dst[0] = decoded[0];
    for (size_t i = 1; i < di; i++)
        dst[i] = (uint8_t)(decoded[i] + dst[i - 1]);

    ray_sys_free(decoded);
    return di;
}

/* ===== Server ===== */

static void conn_close(ray_ipc_server_t* srv, ray_ipc_conn_t* c)
{
#if defined(__linux__)
    epoll_ctl(srv->poll_fd, EPOLL_CTL_DEL, c->fd, NULL);
#elif defined(__APPLE__)
    struct kevent kev;
    EV_SET(&kev, c->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    kevent(srv->poll_fd, &kev, 1, NULL, 0, NULL);
#else
    (void)srv;
#endif

    ray_sock_close(c->fd);
    if (c->rx_buf) ray_sys_free(c->rx_buf);
    c->fd      = RAY_INVALID_SOCK;
    c->rx_buf  = NULL;
    c->rx_len  = 0;
    c->rx_need = 0;

    /* Compact: move last conn into this slot */
    uint32_t idx = (uint32_t)(c - srv->conns);
    if (idx + 1 < srv->n_conns)
        srv->conns[idx] = srv->conns[srv->n_conns - 1];
    if (srv->n_conns > 0) srv->n_conns--;
}

static void conn_send_response(ray_ipc_conn_t* c, ray_t* result)
{
    /* Serialize — fall back to null if result is unserializable */
    int64_t ser_size = ray_serde_size(result);
    if (ser_size <= 0) return;

    uint8_t* payload = (uint8_t*)ray_sys_alloc((size_t)ser_size);
    if (!payload) return;
    ray_ser_raw(payload, result);

    /* Try compression */
    uint8_t* send_buf = NULL;
    size_t   send_len = 0;
    uint8_t  flags    = 0;

    if ((size_t)ser_size > RAY_IPC_COMPRESS_THRESHOLD) {
        uint8_t* comp = (uint8_t*)ray_sys_alloc((size_t)ser_size);
        if (comp) {
            size_t clen = ray_ipc_compress(payload, (size_t)ser_size,
                                           comp, (size_t)ser_size);
            if (clen > 0 && clen + 4 < (size_t)ser_size) {
                /* Use compressed: 4-byte uncompressed size + compressed data */
                send_len = clen + 4;
                send_buf = (uint8_t*)ray_sys_alloc(send_len);
                if (send_buf) {
                    uint32_t uncomp = (uint32_t)ser_size;
                    memcpy(send_buf, &uncomp, 4);
                    memcpy(send_buf + 4, comp, clen);
                    flags = RAY_IPC_FLAG_COMPRESSED;
                }
            }
            ray_sys_free(comp);
        }
    }

    if (!send_buf) {
        /* Send uncompressed */
        send_buf = payload;
        send_len = (size_t)ser_size;
        payload  = NULL;  /* don't double-free */
    }

    /* Build and send header + payload */
    ray_ipc_header_t hdr = {
        .prefix  = RAY_SERDE_PREFIX,
        .version = RAY_VERSION_MAJOR,
        .flags   = flags,
        .endian  = 0,
        .msgtype = RAY_IPC_MSG_RESP,
        .size    = (int64_t)send_len,
    };
    ray_sock_send(c->fd, &hdr, sizeof(hdr));
    ray_sock_send(c->fd, send_buf, send_len);

    ray_sys_free(send_buf);
    if (payload) ray_sys_free(payload);
}

static void conn_on_handshake(ray_ipc_server_t* srv, ray_ipc_conn_t* c)
{
    (void)srv;
    /* Validate: rx_buf[1] should be 0x00 (null terminator) */
    uint8_t version = c->rx_buf[0];
    (void)version;  /* version check can be added later */

    /* Send handshake response: our version + null */
    uint8_t resp[2] = { RAY_VERSION_MAJOR, 0x00 };
    ray_sock_send(c->fd, resp, 2);

    /* Switch to header phase */
    ray_sys_free(c->rx_buf);
    c->rx_buf  = NULL;
    c->rx_len  = 0;
    c->rx_need = sizeof(ray_ipc_header_t);  /* 16 bytes */
    c->phase   = RAY_IPC_PHASE_HEADER;
}

static void conn_on_header(ray_ipc_server_t* srv, ray_ipc_conn_t* c)
{
    /* Copy header from rx_buf */
    memcpy(&c->hdr, c->rx_buf, sizeof(ray_ipc_header_t));

    /* Validate prefix */
    if (c->hdr.prefix != RAY_SERDE_PREFIX) { conn_close(srv, c); return; }
    if (c->hdr.size <= 0)                  { conn_close(srv, c); return; }
    if (c->hdr.size > 256 * 1024 * 1024)   { conn_close(srv, c); return; }

    /* Reallocate buffer for payload */
    ray_sys_free(c->rx_buf);
    c->rx_buf = (uint8_t*)ray_sys_alloc((size_t)c->hdr.size);
    if (!c->rx_buf) { conn_close(srv, c); return; }
    c->rx_len  = 0;
    c->rx_need = (size_t)c->hdr.size;
    c->phase   = RAY_IPC_PHASE_PAYLOAD;
}

static void conn_on_payload(ray_ipc_server_t* srv, ray_ipc_conn_t* c)
{
    uint8_t* payload     = c->rx_buf;
    size_t   payload_len = c->rx_len;

    /* Decompress if needed */
    uint8_t* decompressed = NULL;
    if (c->hdr.flags & RAY_IPC_FLAG_COMPRESSED) {
        /* First 4 bytes = uncompressed size */
        if (payload_len < 4) { conn_close(srv, c); return; }
        uint32_t uncomp_size;
        memcpy(&uncomp_size, payload, 4);
        decompressed = (uint8_t*)ray_sys_alloc(uncomp_size);
        if (!decompressed) { conn_close(srv, c); return; }
        size_t dlen = ray_ipc_decompress(payload + 4, payload_len - 4,
                                         decompressed, uncomp_size);
        if (dlen != uncomp_size) {
            ray_sys_free(decompressed);
            conn_close(srv, c);
            return;
        }
        payload     = decompressed;
        payload_len = uncomp_size;
    }

    /* Deserialize */
    int64_t de_len = (int64_t)payload_len;
    ray_t*  msg    = ray_de_raw(payload, &de_len);
    if (decompressed) ray_sys_free(decompressed);

    /* Eval */
    ray_t* result = NULL;
    if (msg && !RAY_IS_ERR(msg)) {
        if (msg->type == -RAY_STR) {
            /* String atom: eval as text */
            const char* str  = ray_str_ptr(msg);
            size_t      slen = ray_str_len(msg);
            if (str && slen > 0) {
                /* Need null-terminated copy for ray_eval_str */
                char* tmp = (char*)ray_sys_alloc(slen + 1);
                if (tmp) {
                    memcpy(tmp, str, slen);
                    tmp[slen] = '\0';
                    result = ray_eval_str(tmp);
                    ray_sys_free(tmp);
                }
            }
            ray_release(msg);
        } else {
            /* Object message: eval directly */
            result = ray_eval(msg);
            ray_release(msg);
        }
    }
    if (!result) result = RAY_NULL_OBJ;

    /* Send response for sync messages */
    if (c->hdr.msgtype == RAY_IPC_MSG_SYNC) {
        conn_send_response(c, result);
    }
    if (result != RAY_NULL_OBJ) ray_release(result);

    /* Reset for next message */
    ray_sys_free(c->rx_buf);
    c->rx_buf  = NULL;
    c->rx_len  = 0;
    c->rx_need = sizeof(ray_ipc_header_t);
    c->phase   = RAY_IPC_PHASE_HEADER;
}

static void conn_on_readable(ray_ipc_server_t* srv, ray_ipc_conn_t* c)
{
    /* Ensure rx_buf has space */
    if (!c->rx_buf) {
        c->rx_buf = (uint8_t*)ray_sys_alloc(c->rx_need);
        if (!c->rx_buf) { conn_close(srv, c); return; }
    }

    /* Read available data */
    int64_t n = ray_sock_recv(c->fd, c->rx_buf + c->rx_len,
                              c->rx_need - c->rx_len);
    if (n <= 0) { conn_close(srv, c); return; }  /* closed or error */
    c->rx_len += (size_t)n;

    /* Not enough data yet */
    if (c->rx_len < c->rx_need) return;

    /* Phase complete — process it */
    switch (c->phase) {
    case RAY_IPC_PHASE_HANDSHAKE:
        conn_on_handshake(srv, c);
        break;
    case RAY_IPC_PHASE_HEADER:
        conn_on_header(srv, c);
        break;
    case RAY_IPC_PHASE_PAYLOAD:
        conn_on_payload(srv, c);
        break;
    }
}

ray_err_t ray_ipc_server_init(ray_ipc_server_t* srv, uint16_t port)
{
    memset(srv, 0, sizeof(*srv));
    srv->listen_fd = ray_sock_listen(port);
    if (srv->listen_fd == RAY_INVALID_SOCK) return RAY_ERR_IO;
    ray_sock_set_nonblocking(srv->listen_fd);

#if defined(__linux__)
    srv->poll_fd = epoll_create1(0);
    if (srv->poll_fd < 0) {
        ray_sock_close(srv->listen_fd);
        return RAY_ERR_IO;
    }
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = srv->listen_fd };
    epoll_ctl(srv->poll_fd, EPOLL_CTL_ADD, srv->listen_fd, &ev);
#elif defined(__APPLE__)
    srv->poll_fd = kqueue();
    if (srv->poll_fd < 0) {
        ray_sock_close(srv->listen_fd);
        return RAY_ERR_IO;
    }
    struct kevent kev;
    EV_SET(&kev, srv->listen_fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    kevent(srv->poll_fd, &kev, 1, NULL, 0, NULL);
#else
    srv->poll_fd = -1;  /* use select in poll */
#endif

    srv->running = true;
    return RAY_OK;
}

void ray_ipc_server_destroy(ray_ipc_server_t* srv)
{
    /* Close all connections */
    for (uint32_t i = 0; i < srv->n_conns; i++) {
        ray_ipc_conn_t* c = &srv->conns[i];
        if (c->fd != RAY_INVALID_SOCK) {
            if (c->rx_buf) ray_sys_free(c->rx_buf);
            ray_sock_close(c->fd);
        }
    }
    srv->n_conns = 0;

    /* Close listen socket */
    ray_sock_close(srv->listen_fd);
    srv->listen_fd = RAY_INVALID_SOCK;

    /* Close poll fd */
    if (srv->poll_fd >= 0) {
#ifdef _WIN32
        /* no-op */
#else
        close(srv->poll_fd);
#endif
    }
    srv->poll_fd = -1;
    srv->running = false;
}

ray_err_t ray_ipc_watch_fd(ray_ipc_server_t* srv, int fd)
{
#if defined(__linux__)
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = fd };
    if (epoll_ctl(srv->poll_fd, EPOLL_CTL_ADD, fd, &ev) < 0)
        return RAY_ERR_IO;
#elif defined(__APPLE__)
    struct kevent kev;
    EV_SET(&kev, fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    if (kevent(srv->poll_fd, &kev, 1, NULL, 0, NULL) < 0)
        return RAY_ERR_IO;
#else
    (void)srv; (void)fd;  /* handled in select */
#endif
    return RAY_OK;
}

void ray_ipc_attach(ray_ipc_server_t* srv, int poll_fd)
{
    if (!srv || poll_fd < 0) return;

    /* Add listen socket to the external poll fd */
#if defined(__linux__)
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = srv->listen_fd };
    epoll_ctl(poll_fd, EPOLL_CTL_ADD, srv->listen_fd, &ev);
#elif defined(__APPLE__)
    struct kevent kev;
    EV_SET(&kev, srv->listen_fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    kevent(poll_fd, &kev, 1, NULL, 0, NULL);
#endif

    /* Close old poll fd and use the external one */
    if (srv->poll_fd >= 0 && srv->poll_fd != poll_fd) {
#ifndef _WIN32
        close(srv->poll_fd);
#endif
    }
    srv->poll_fd = poll_fd;
}

int ray_ipc_poll(ray_ipc_server_t* srv, int timeout_ms)
{
    int ready = 0;

#if defined(__linux__)
    struct epoll_event events[RAY_IPC_MAX_EVENTS];
    int nfds = epoll_wait(srv->poll_fd, events, RAY_IPC_MAX_EVENTS, timeout_ms);
    if (nfds < 0) return (errno == EINTR) ? 0 : -1;

    for (int i = 0; i < nfds; i++) {
        int fd = events[i].data.fd;

        if (fd == srv->listen_fd) {
            /* Accept new connection */
            ray_sock_t new_fd = ray_sock_accept(srv->listen_fd);
            if (new_fd == RAY_INVALID_SOCK) continue;
            ray_sock_set_nonblocking(new_fd);
            if (srv->n_conns >= RAY_IPC_MAX_CONNS) {
                ray_sock_close(new_fd);
                continue;
            }
            ray_ipc_conn_t* c = &srv->conns[srv->n_conns++];
            c->fd      = new_fd;
            c->rx_buf  = NULL;
            c->rx_len  = 0;
            c->rx_need = 2;  /* handshake: version byte + null */
            c->phase   = RAY_IPC_PHASE_HANDSHAKE;
            struct epoll_event cev = { .events = EPOLLIN, .data.fd = new_fd };
            epoll_ctl(srv->poll_fd, EPOLL_CTL_ADD, new_fd, &cev);
        } else {
            /* Find connection or count as external fd */
            bool found = false;
            for (uint32_t j = 0; j < srv->n_conns; j++) {
                if (srv->conns[j].fd == fd) {
                    conn_on_readable(srv, &srv->conns[j]);
                    found = true;
                    break;
                }
            }
            if (!found) ready++;  /* external watched fd */
        }
    }

#elif defined(__APPLE__)
    struct kevent events[RAY_IPC_MAX_EVENTS];
    struct timespec ts;
    struct timespec* tsp = NULL;
    if (timeout_ms >= 0) {
        ts.tv_sec  = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
        tsp = &ts;
    }
    int nfds = kevent(srv->poll_fd, NULL, 0, events, RAY_IPC_MAX_EVENTS, tsp);
    if (nfds < 0) return (errno == EINTR) ? 0 : -1;

    for (int i = 0; i < nfds; i++) {
        int fd = (int)events[i].ident;

        if (fd == srv->listen_fd) {
            /* Accept new connection */
            ray_sock_t new_fd = ray_sock_accept(srv->listen_fd);
            if (new_fd == RAY_INVALID_SOCK) continue;
            ray_sock_set_nonblocking(new_fd);
            if (srv->n_conns >= RAY_IPC_MAX_CONNS) {
                ray_sock_close(new_fd);
                continue;
            }
            ray_ipc_conn_t* c = &srv->conns[srv->n_conns++];
            c->fd      = new_fd;
            c->rx_buf  = NULL;
            c->rx_len  = 0;
            c->rx_need = 2;
            c->phase   = RAY_IPC_PHASE_HANDSHAKE;
            struct kevent kev;
            EV_SET(&kev, new_fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
            kevent(srv->poll_fd, &kev, 1, NULL, 0, NULL);
        } else {
            bool found = false;
            for (uint32_t j = 0; j < srv->n_conns; j++) {
                if (srv->conns[j].fd == fd) {
                    conn_on_readable(srv, &srv->conns[j]);
                    found = true;
                    break;
                }
            }
            if (!found) ready++;
        }
    }

#else  /* Windows: select-based fallback */
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(srv->listen_fd, &rfds);
    ray_sock_t maxfd = srv->listen_fd;
    for (uint32_t i = 0; i < srv->n_conns; i++) {
        FD_SET(srv->conns[i].fd, &rfds);
        if (srv->conns[i].fd > maxfd) maxfd = srv->conns[i].fd;
    }

    struct timeval tv;
    struct timeval* tvp = NULL;
    if (timeout_ms >= 0) {
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        tvp = &tv;
    }

    int nfds = select((int)(maxfd + 1), &rfds, NULL, NULL, tvp);
    if (nfds < 0) return (errno == EINTR) ? 0 : -1;

    if (FD_ISSET(srv->listen_fd, &rfds)) {
        ray_sock_t new_fd = ray_sock_accept(srv->listen_fd);
        if (new_fd != RAY_INVALID_SOCK) {
            ray_sock_set_nonblocking(new_fd);
            if (srv->n_conns >= RAY_IPC_MAX_CONNS) {
                ray_sock_close(new_fd);
            } else {
                ray_ipc_conn_t* c = &srv->conns[srv->n_conns++];
                c->fd      = new_fd;
                c->rx_buf  = NULL;
                c->rx_len  = 0;
                c->rx_need = 2;
                c->phase   = RAY_IPC_PHASE_HANDSHAKE;
            }
        }
    }

    /* Iterate in reverse so conn_close swap-compaction doesn't skip entries */
    for (uint32_t i = srv->n_conns; i > 0; ) {
        --i;
        if (srv->conns[i].fd != RAY_INVALID_SOCK && FD_ISSET(srv->conns[i].fd, &rfds))
            conn_on_readable(srv, &srv->conns[i]);
    }
#endif

    return ready;
}

/* ===== Client ===== */

static ray_sock_t g_client_fds[RAY_IPC_MAX_CONNS];
static int        g_client_count = 0;
static bool       g_client_init = false;

static void client_init(void) {
    if (g_client_init) return;
    for (int i = 0; i < RAY_IPC_MAX_CONNS; i++)
        g_client_fds[i] = RAY_INVALID_SOCK;
    g_client_init = true;
}

static int64_t recv_full(ray_sock_t fd, void* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        int64_t n = ray_sock_recv(fd, (uint8_t*)buf + total, len - total);
        if (n <= 0) return -1;
        total += (size_t)n;
    }
    return (int64_t)total;
}

static int64_t client_send_msg(int64_t handle, ray_t* msg, uint8_t msgtype)
{
    if (handle < 0 || handle >= RAY_IPC_MAX_CONNS) return -2;
    ray_sock_t fd = g_client_fds[handle];
    if (fd == RAY_INVALID_SOCK) return -2;  /* dead handle */

    /* Serialize */
    int64_t ser_size = ray_serde_size(msg);
    if (ser_size <= 0) return -1;

    uint8_t* payload = (uint8_t*)ray_sys_alloc((size_t)ser_size);
    if (!payload) return -1;
    ray_ser_raw(payload, msg);

    /* Try compression */
    uint8_t* send_buf = NULL;
    size_t   send_len = 0;
    uint8_t  flags    = 0;

    if ((size_t)ser_size > RAY_IPC_COMPRESS_THRESHOLD) {
        uint8_t* comp = (uint8_t*)ray_sys_alloc((size_t)ser_size);
        if (comp) {
            size_t clen = ray_ipc_compress(payload, (size_t)ser_size,
                                           comp, (size_t)ser_size);
            if (clen > 0 && clen + 4 < (size_t)ser_size) {
                send_len = clen + 4;
                send_buf = (uint8_t*)ray_sys_alloc(send_len);
                if (send_buf) {
                    uint32_t uncomp = (uint32_t)ser_size;
                    memcpy(send_buf, &uncomp, 4);
                    memcpy(send_buf + 4, comp, clen);
                    flags = RAY_IPC_FLAG_COMPRESSED;
                }
            }
            ray_sys_free(comp);
        }
    }

    if (!send_buf) {
        send_buf = payload;
        send_len = (size_t)ser_size;
        payload  = NULL;
    }

    /* Build and send header + payload */
    ray_ipc_header_t hdr = {
        .prefix  = RAY_SERDE_PREFIX,
        .version = RAY_VERSION_MAJOR,
        .flags   = flags,
        .endian  = 0,
        .msgtype = msgtype,
        .size    = (int64_t)send_len,
    };

    int64_t rc = ray_sock_send(fd, &hdr, sizeof(hdr));
    if (rc < 0) { ray_sys_free(send_buf); if (payload) ray_sys_free(payload); return -1; }
    rc = ray_sock_send(fd, send_buf, send_len);

    ray_sys_free(send_buf);
    if (payload) ray_sys_free(payload);
    return rc < 0 ? -1 : 0;
}

int64_t ray_ipc_connect(const char* host, uint16_t port)
{
    client_init();

    ray_sock_t fd = ray_sock_connect(host, port, 5000);
    if (fd == RAY_INVALID_SOCK) return -1;

    /* Send handshake: version + null */
    uint8_t hs[2] = { RAY_VERSION_MAJOR, 0x00 };
    if (ray_sock_send(fd, hs, 2) < 0) {
        ray_sock_close(fd);
        return -1;
    }

    /* Receive handshake response */
    uint8_t resp[2];
    if (recv_full(fd, resp, 2) < 0 || resp[1] != 0x00) {
        ray_sock_close(fd);
        return -1;
    }

    /* Clear connect/handshake timeout — data transfer has no time limit */
#ifdef _WIN32
    { DWORD z = 0;
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&z, sizeof(z));
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&z, sizeof(z)); }
#else
    { struct timeval z = {0, 0};
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &z, sizeof(z));
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &z, sizeof(z)); }
#endif

    /* Find free slot */
    for (int i = 0; i < RAY_IPC_MAX_CONNS; i++) {
        if (g_client_fds[i] == RAY_INVALID_SOCK) {
            g_client_fds[i] = fd;
            if (i >= g_client_count) g_client_count = i + 1;
            return (int64_t)i;
        }
    }

    /* No free slot */
    ray_sock_close(fd);
    return -1;
}

void ray_ipc_close(int64_t handle)
{
    if (handle < 0 || handle >= RAY_IPC_MAX_CONNS) return;
    if (g_client_fds[handle] == RAY_INVALID_SOCK) return;
    ray_sock_close(g_client_fds[handle]);
    g_client_fds[handle] = RAY_INVALID_SOCK;
}

ray_t* ray_ipc_send(int64_t handle, ray_t* msg)
{
    { int64_t sr = client_send_msg(handle, msg, RAY_IPC_MSG_SYNC);
      if (sr == -2) return ray_error("io", "connection closed");
      if (sr < 0) return ray_error("io", "ipc send failed"); }

    ray_sock_t fd = g_client_fds[handle];

    /* Receive response header */
    ray_ipc_header_t hdr;
    if (recv_full(fd, &hdr, sizeof(hdr)) < 0) {
        ray_ipc_close(handle);  /* invalidate poisoned handle */
        return ray_error("io", "ipc recv header failed");
    }
    if (hdr.prefix != RAY_SERDE_PREFIX || hdr.size <= 0) {
        ray_ipc_close(handle);
        return ray_error("io", "ipc bad response header");
    }

    /* Receive response payload */
    uint8_t* payload = (uint8_t*)ray_sys_alloc((size_t)hdr.size);
    if (!payload) return ray_error("oom", NULL);
    if (recv_full(fd, payload, (size_t)hdr.size) < 0) {
        ray_sys_free(payload);
        ray_ipc_close(handle);
        return ray_error("io", "ipc recv payload failed");
    }

    /* Decompress if needed */
    uint8_t* deser_buf     = payload;
    size_t   deser_len     = (size_t)hdr.size;
    uint8_t* decompressed  = NULL;

    if (hdr.flags & RAY_IPC_FLAG_COMPRESSED) {
        if (deser_len < 4) { ray_sys_free(payload); return ray_error("io", "ipc compressed payload too short"); }
        uint32_t uncomp_size;
        memcpy(&uncomp_size, payload, 4);
        decompressed = (uint8_t*)ray_sys_alloc(uncomp_size);
        if (!decompressed) { ray_sys_free(payload); return ray_error("oom", NULL); }
        size_t dlen = ray_ipc_decompress(payload + 4, deser_len - 4,
                                         decompressed, uncomp_size);
        if (dlen != uncomp_size) {
            ray_sys_free(decompressed);
            ray_sys_free(payload);
            return ray_error("io", "ipc decompress failed");
        }
        deser_buf = decompressed;
        deser_len = uncomp_size;
    }

    /* Deserialize */
    int64_t de_len = (int64_t)deser_len;
    ray_t*  result = ray_de_raw(deser_buf, &de_len);

    if (decompressed) ray_sys_free(decompressed);
    ray_sys_free(payload);

    return result ? result : RAY_NULL_OBJ;
}

ray_err_t ray_ipc_send_async(int64_t handle, ray_t* msg)
{
    if (client_send_msg(handle, msg, RAY_IPC_MSG_ASYNC) < 0)
        return RAY_ERR_IO;
    return RAY_OK;
}
