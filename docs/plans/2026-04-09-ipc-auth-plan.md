# IPC Authentication Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add kdb+-style shared-secret authentication to the IPC handshake, with `-u password` (auth required) and `-U password` (auth + read-only restriction).

**Architecture:** Server stores secret on `ray_poll_t`, client passes `user:password` during handshake. Restricted builtins marked with `RAY_FN_RESTRICTED` flag; eval dispatcher checks `__VM->restricted` before calling them. Zero runtime cost for non-IPC evals.

**Tech Stack:** Pure C17, existing IPC/serde infrastructure.

---

### Task 1: Add auth fields to poll and VM

**Files:**
- Modify: `src/core/poll.h:86-92` — add `auth_secret[256]`, `restricted` to `ray_poll_t`
- Modify: `src/core/runtime.h:45-66` — add `restricted` bool to `ray_vm_t`
- Modify: `src/lang/eval.h:33-37` — add `RAY_FN_RESTRICTED` flag

**Step 1: Add fields to `ray_poll_t`**

In `src/core/poll.h`, add after the `sel_cap` field:

```c
struct ray_poll {
    int64_t          fd;
    int64_t          code;
    ray_selector_t** sels;
    uint32_t         n_sels;
    uint32_t         sel_cap;
    char             auth_secret[256]; /* password from -u/-U, empty = no auth */
    bool             restricted;       /* true if -U (read-only IPC mode) */
};
```

**Step 2: Add `restricted` to `ray_vm_t`**

In `src/core/runtime.h`, add after `scope_depth`:

```c
    int32_t          scope_depth;
    bool             restricted;   /* IPC -U mode: reject RAY_FN_RESTRICTED builtins */
} ray_vm_t;
```

**Step 3: Add `RAY_FN_RESTRICTED` flag**

In `src/lang/eval.h`, add after the existing flags:

```c
#define RAY_FN_RESTRICTED    0x20  /* forbidden during -U restricted IPC evals */
```

**Step 4: Build**

```bash
make
```
Expected: compiles cleanly, no tests affected (fields are zero-initialized).

**Step 5: Commit**

```bash
git add src/core/poll.h src/core/runtime.h src/lang/eval.h
git commit -m "feat: add auth_secret/restricted fields to poll and VM"
```

---

### Task 2: Parse -u/-U flags in main.c

**Files:**
- Modify: `src/app/main.c:43-51` — add -u/-U arg parsing

**Step 1: Add flag parsing**

Replace the arg parsing block in `main.c`:

```c
    /* Parse args: [-i] [-p PORT] [-u PASSWORD] [-U PASSWORD] [file.rfl] */
    const char* auth_pw = NULL;
    bool auth_restricted = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interactive") == 0)
            interactive = 1;
        else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) && i + 1 < argc)
            port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
            auth_pw = argv[++i];
            auth_restricted = false;
        }
        else if (strcmp(argv[i], "-U") == 0 && i + 1 < argc) {
            auth_pw = argv[++i];
            auth_restricted = true;
        }
        else
            file = argv[i];
    }
```

**Step 2: Copy auth config into poll**

After `ray_poll_create()`, add:

```c
    ray_poll_t* poll = ray_poll_create();

    /* Configure auth */
    if (poll && auth_pw) {
        size_t pw_len = strlen(auth_pw);
        if (pw_len >= sizeof(poll->auth_secret))
            pw_len = sizeof(poll->auth_secret) - 1;
        memcpy(poll->auth_secret, auth_pw, pw_len);
        poll->auth_secret[pw_len] = '\0';
        poll->restricted = auth_restricted;
    }
```

**Step 3: Build**

```bash
make
```
Expected: compiles cleanly.

**Step 4: Commit**

```bash
git add src/app/main.c
git commit -m "feat: parse -u/-U flags for IPC auth"
```

---

### Task 3: Server-side auth handshake

**Files:**
- Modify: `src/core/ipc.c:253-257` — add `auth_required` to `ray_ipc_conn_data_t`
- Modify: `src/core/ipc.c:311-327` — rewrite `ipc_read_handshake` for auth flow
- Add new: `ipc_read_creds` function after `ipc_read_handshake`

**Step 1: Add poll pointer to conn data**

In `ray_ipc_conn_data_t`, the poll pointer is needed to access `auth_secret`. But we can get it from the callback args. Instead, store whether auth is required:

```c
typedef struct {
    ray_ipc_header_t hdr;
    uint8_t          phase;
    int64_t          listener_id;
    bool             auth_required;  /* server has -u/-U */
    bool             restricted;     /* server has -U */
} ray_ipc_conn_data_t;
```

**Step 2: Set auth flag on accept**

In `ipc_accept`, after creating `cd`, set:

```c
    cd->auth_required = (poll->auth_secret[0] != '\0');
    cd->restricted    = poll->restricted;
```

**Step 3: Rewrite `ipc_read_handshake`**

```c
static ray_t* ipc_read_handshake(ray_poll_t* poll, ray_selector_t* sel)
{
    if (!sel->rx.buf || sel->rx.buf->offset < 2) return NULL;

    ray_ipc_conn_data_t* cd = (ray_ipc_conn_data_t*)sel->data;

    /* Send handshake response: version + auth_required flag */
    uint8_t resp[2] = { RAY_VERSION_MAJOR, cd->auth_required ? 0x01 : 0x00 };
    ray_sock_send((ray_sock_t)sel->fd, resp, 2);

    if (cd->auth_required) {
        /* Wait for credentials: 1 byte length + up to 255 bytes */
        cd->phase = RAY_IPC_PHASE_HANDSHAKE; /* still in handshake */
        sel->rx.read_fn = ipc_read_creds;
        ray_poll_rx_request(poll, sel, 1);  /* first byte is length */
        return NULL;
    }

    /* No auth — switch to header phase */
    cd->phase = RAY_IPC_PHASE_HEADER;
    sel->rx.read_fn = ipc_read_header;
    ray_poll_rx_request(poll, sel, sizeof(ray_ipc_header_t));

    return NULL;
}
```

**Step 4: Add `ipc_read_creds`**

New function after `ipc_read_handshake`:

```c
static ray_t* ipc_read_creds(ray_poll_t* poll, ray_selector_t* sel)
{
    if (!sel->rx.buf || sel->rx.buf->offset < 1) return NULL;
    uint8_t cred_len = sel->rx.buf->data[0];

    /* Need length byte + credential bytes */
    if (sel->rx.buf->offset < 1 + cred_len) {
        ray_poll_rx_request(poll, sel, 1 + cred_len);
        return NULL;
    }

    ray_ipc_conn_data_t* cd = (ray_ipc_conn_data_t*)sel->data;

    /* Extract password: credential is "user:password\0", find first colon */
    const char* creds = (const char*)(sel->rx.buf->data + 1);
    const char* colon = memchr(creds, ':', cred_len);
    const char* pw = colon ? colon + 1 : creds;
    size_t pw_len = colon ? (size_t)(cred_len - (pw - creds)) : cred_len;
    /* Strip null terminator if present */
    if (pw_len > 0 && pw[pw_len - 1] == '\0') pw_len--;

    /* Validate password */
    bool ok = (pw_len == strlen(poll->auth_secret) &&
               memcmp(pw, poll->auth_secret, pw_len) == 0);

    uint8_t result = ok ? 0x00 : 0x01;
    ray_sock_send((ray_sock_t)sel->fd, &result, 1);

    if (!ok) {
        ray_poll_deregister(poll, sel->id);
        return NULL;
    }

    /* Auth passed — switch to header phase */
    cd->phase = RAY_IPC_PHASE_HEADER;
    sel->rx.read_fn = ipc_read_header;
    ray_poll_rx_request(poll, sel, sizeof(ray_ipc_header_t));

    return NULL;
}
```

**Step 5: Forward-declare `ipc_read_creds`**

Add after the existing forward declarations near line 259:

```c
static ray_t* ipc_read_creds(ray_poll_t* poll, ray_selector_t* sel);
```

**Step 6: Build**

```bash
make
```
Expected: compiles cleanly.

**Step 7: Commit**

```bash
git add src/core/ipc.c
git commit -m "feat: server-side auth handshake with credential validation"
```

---

### Task 4: Client-side auth handshake

**Files:**
- Modify: `src/core/ipc.h:80` — add creds params to `ray_ipc_connect`
- Modify: `src/core/ipc.c:779-818` — update `ray_ipc_connect` for auth flow
- Modify: `src/ops/system.c:483-512` — update `ray_hopen_fn` to parse `host:port:user:password`

**Step 1: Update `ray_ipc_connect` signature**

In `src/core/ipc.h`:

```c
int64_t   ray_ipc_connect(const char* host, uint16_t port,
                           const char* user, const char* password);
```

**Step 2: Update `ray_ipc_connect` implementation**

In `src/core/ipc.c`, replace the handshake section:

```c
int64_t ray_ipc_connect(const char* host, uint16_t port,
                         const char* user, const char* password)
{
    client_init();

    ray_sock_t fd = ray_sock_connect(host, port, 5000);
    if (fd == RAY_INVALID_SOCK) return -1;

    /* Send handshake */
    uint8_t hs[2] = { RAY_VERSION_MAJOR, 0x00 };
    if (ray_sock_send(fd, hs, 2) < 0) {
        ray_sock_close(fd);
        return -1;
    }

    /* Receive handshake response */
    uint8_t resp[2];
    if (recv_full(fd, resp, 2) < 0) {
        ray_sock_close(fd);
        return -1;
    }

    /* Auth required? */
    if (resp[1] == 0x01) {
        if (!password) {
            ray_sock_close(fd);
            return -2; /* auth required but no creds */
        }
        /* Build "user:password\0" credential string */
        char cred[256];
        int cred_len;
        if (user && user[0])
            cred_len = snprintf(cred, sizeof(cred), "%s:%s", user, password);
        else
            cred_len = snprintf(cred, sizeof(cred), ":%s", password);
        if (cred_len < 0 || cred_len >= (int)sizeof(cred)) {
            ray_sock_close(fd);
            return -1;
        }
        cred_len++; /* include null terminator */
        uint8_t len_byte = (uint8_t)cred_len;
        if (ray_sock_send(fd, &len_byte, 1) < 0 ||
            ray_sock_send(fd, cred, cred_len) < 0) {
            ray_sock_close(fd);
            return -1;
        }
        /* Read auth result */
        uint8_t auth_result;
        if (recv_full(fd, &auth_result, 1) < 0 || auth_result != 0x00) {
            ray_sock_close(fd);
            return -3; /* auth rejected */
        }
    }

    /* Clear timeouts */
#ifdef _WIN32
    { DWORD z = 0;
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&z, sizeof(z));
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&z, sizeof(z)); }
#else
    { struct timeval z = {0, 0};
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &z, sizeof(z));
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &z, sizeof(z)); }
#endif

    for (int i = 0; i < RAY_IPC_MAX_CONNS; i++) {
        if (g_client_fds[i] == RAY_INVALID_SOCK) {
            g_client_fds[i] = fd;
            if (i >= g_client_count) g_client_count = i + 1;
            return (int64_t)i;
        }
    }

    ray_sock_close(fd);
    return -1;
}
```

**Step 3: Update `ray_hopen_fn`**

In `src/ops/system.c`, rewrite to parse `host:port:user:password`:

```c
/* (hopen "host:port") or (hopen "host:port:user:password") → i64 handle */
ray_t* ray_hopen_fn(ray_t* x) {
    if (!ray_is_atom(x) || x->type != -RAY_STR)
        return ray_error("type", NULL);

    const char* s = ray_str_ptr(x);
    size_t slen = ray_str_len(x);

    /* Split on colons: host:port[:user:password] */
    const char* parts[4] = {0};
    size_t part_lens[4] = {0};
    int n_parts = 0;
    const char* start = s;
    for (size_t i = 0; i <= slen && n_parts < 4; i++) {
        if (i == slen || s[i] == ':') {
            parts[n_parts] = start;
            part_lens[n_parts] = (size_t)(&s[i] - start);
            n_parts++;
            start = &s[i + 1];
        }
    }

    if (n_parts < 2) return ray_error("domain", NULL);

    /* Extract host */
    char host[256];
    if (part_lens[0] >= sizeof(host)) return ray_error("domain", NULL);
    memcpy(host, parts[0], part_lens[0]);
    host[part_lens[0]] = '\0';

    /* Parse port */
    char port_str[8];
    if (part_lens[1] >= sizeof(port_str)) return ray_error("domain", NULL);
    memcpy(port_str, parts[1], part_lens[1]);
    port_str[part_lens[1]] = '\0';
    int port = atoi(port_str);
    if (port <= 0 || port > 65535) return ray_error("domain", NULL);

    /* Extract user:password if present */
    char user[128] = "";
    char password[128] = "";
    if (n_parts >= 4) {
        if (part_lens[2] < sizeof(user)) {
            memcpy(user, parts[2], part_lens[2]);
            user[part_lens[2]] = '\0';
        }
        if (part_lens[3] < sizeof(password)) {
            memcpy(password, parts[3], part_lens[3]);
            password[part_lens[3]] = '\0';
        }
    }

    const char* pw_ptr = (n_parts >= 4) ? password : NULL;
    const char* us_ptr = (n_parts >= 4) ? user : NULL;

    int64_t h = ray_ipc_connect(host, (uint16_t)port, us_ptr, pw_ptr);
    if (h == -2) return ray_error("access", "server requires authentication");
    if (h == -3) return ray_error("access", "authentication failed");
    if (h < 0) return ray_error("io", "connection refused: %s:%d", host, port);

    return make_i64(h);
}
```

**Step 4: Fix all callers of `ray_ipc_connect`**

Update the existing test calls in `test/test_store.c` to pass `NULL, NULL` for the new params:

Search for `ray_ipc_connect(` in `test/test_store.c` and add `NULL, NULL`:

```c
int64_t h = ray_ipc_connect("127.0.0.1", port, NULL, NULL);
```

There are 2 call sites: line ~1717 and line ~1769.

**Step 5: Build and test**

```bash
make test
```
Expected: all 587 tests pass (no-auth path unchanged).

**Step 6: Commit**

```bash
git add src/core/ipc.h src/core/ipc.c src/ops/system.c test/test_store.c
git commit -m "feat: client-side auth handshake — hopen host:port:user:password"
```

---

### Task 5: Restricted mode enforcement

**Files:**
- Modify: `src/lang/eval.c:2141-2210` — add restricted check in dispatch
- Modify: `src/lang/eval.c` — add `RAY_FN_RESTRICTED` to 14 builtin registrations
- Modify: `src/core/ipc.c:351-373` — set `__VM->restricted` in `ipc_read_payload`

**Step 1: Add restricted check in eval dispatch**

In `src/lang/eval.c`, in the `ray_eval` switch block, add a check before each builtin dispatch. The cleanest approach: add a helper and call it at each case entry.

Add helper near the top of the file (near other static helpers):

```c
static inline bool fn_is_restricted(ray_t* fn_obj) {
    return __VM->restricted && (fn_obj->attrs & RAY_FN_RESTRICTED);
}
```

In the `ray_eval` dispatch (line ~2141), add before the function call in each case:

For `RAY_UNARY` (after line 2144):
```c
        case RAY_UNARY: {
            if (n < 2) { ray_release(head); ret = ray_error("domain", NULL); goto out; }
            if (fn_is_restricted(head)) { ray_release(head); ret = ray_error("access", "restricted"); goto out; }
            ...
```

For `RAY_BINARY` (after line 2162):
```c
        case RAY_BINARY: {
            if (n < 3) { ray_release(head); ret = ray_error("domain", NULL); goto out; }
            if (fn_is_restricted(head)) { ray_release(head); ret = ray_error("access", "restricted"); goto out; }
            ...
```

For `RAY_VARY` (after line 2204):
```c
        case RAY_VARY: {
            if (fn_is_restricted(head)) { ray_release(head); ret = ray_error("access", "restricted"); goto out; }
            ...
```

Also add the same check in `ray_apply` (the VM dispatch path near line 1494):

For `RAY_UNARY` (after line 1495):
```c
        case RAY_UNARY:
            if (fn_is_restricted(fn_obj)) { for (int32_t i = 0; i < n; i++) ray_release(fn_args[i]); result = ray_error("access", "restricted"); break; }
            ...
```

For `RAY_BINARY` (after line 1500):
```c
        case RAY_BINARY:
            if (fn_is_restricted(fn_obj)) { for (int32_t i = 0; i < n; i++) ray_release(fn_args[i]); result = ray_error("access", "restricted"); break; }
            ...
```

For `RAY_VARY` (after line 1506):
```c
        case RAY_VARY:
            if (fn_is_restricted(fn_obj)) { for (int32_t i = 0; i < n; i++) ray_release(fn_args[i]); result = ray_error("access", "restricted"); break; }
            ...
```

**Step 2: Mark 14 builtins as restricted**

In `src/lang/eval.c`, update registrations by OR-ing `RAY_FN_RESTRICTED`:

```c
/* line 1814 */ register_binary("set", RAY_FN_SPECIAL_FORM | RAY_FN_RESTRICTED, ray_set_fn);
/* line 1874 */ register_vary("update",    RAY_FN_SPECIAL_FORM | RAY_FN_RESTRICTED, ray_update_fn);
/* line 1875 */ register_vary("insert",    RAY_FN_SPECIAL_FORM | RAY_FN_RESTRICTED, ray_insert_fn);
/* line 1876 */ register_vary("upsert",    RAY_FN_SPECIAL_FORM | RAY_FN_RESTRICTED, ray_upsert_fn);
/* line 1891 */ register_vary("read-csv",   RAY_FN_RESTRICTED, ray_read_csv_fn);
/* line 1892 */ register_vary("write-csv",  RAY_FN_RESTRICTED, ray_write_csv_fn);
/* line 1895 */ register_unary("read",      RAY_FN_RESTRICTED, ray_read_file_fn);
/* line 1896 */ register_binary("write",    RAY_FN_RESTRICTED, ray_write_file_fn);
/* line 1952 */ register_unary("system",     RAY_FN_RESTRICTED, ray_system_fn);
/* line 1953 */ register_unary("getenv",     RAY_FN_RESTRICTED, ray_getenv_fn);
/* line 1959 */ register_unary("hopen",     RAY_FN_RESTRICTED, ray_hopen_fn);
/* line 1960 */ register_unary("hclose",    RAY_FN_RESTRICTED, ray_hclose_fn);
/* line 1961 */ register_binary("hsend",    RAY_FN_RESTRICTED, ray_hsend_fn);
/* line 2000 */ register_vary("del",          RAY_FN_SPECIAL_FORM | RAY_FN_RESTRICTED, ray_del_fn);
```

**Step 3: Set `__VM->restricted` in IPC eval**

In `src/core/ipc.c`, modify `ipc_read_payload` to set the restricted flag:

```c
static ray_t* ipc_read_payload(ray_poll_t* poll, ray_selector_t* sel)
{
    ray_ipc_conn_data_t* cd = (ray_ipc_conn_data_t*)sel->data;

    if (!sel->rx.buf || sel->rx.buf->offset < cd->hdr.size)
        return NULL;

    /* Set restricted mode for this eval */
    bool prev_restricted = __VM->restricted;
    __VM->restricted = cd->restricted;

    /* Eval and produce result */
    ray_t* result = eval_payload(sel->rx.buf->data,
                                 (size_t)sel->rx.buf->offset, &cd->hdr);

    /* Restore */
    __VM->restricted = prev_restricted;

    /* Send response for sync messages */
    if (cd->hdr.msgtype == RAY_IPC_MSG_SYNC)
        send_response((ray_sock_t)sel->fd, result);
    if (result != RAY_NULL_OBJ) ray_release(result);

    /* Reset for next message */
    cd->phase = RAY_IPC_PHASE_HEADER;
    sel->rx.read_fn = ipc_read_header;
    ray_poll_rx_request(poll, sel, sizeof(ray_ipc_header_t));

    return NULL;
}
```

Also do the same for the legacy server path — in `conn_on_payload`:

```c
static void conn_on_payload(ray_ipc_server_t* srv, ray_ipc_conn_t* c)
{
    bool prev = __VM->restricted;
    __VM->restricted = srv->restricted;  /* need to add this field */

    ray_t* result = eval_payload(c->rx_buf, c->rx_len, &c->hdr);

    __VM->restricted = prev;
    ...
```

Add `bool restricted;` to `ray_ipc_server_t` in `ipc.h`:

```c
typedef struct ray_ipc_server {
    ray_sock_t        listen_fd;
    int               poll_fd;
    ray_ipc_conn_t    conns[RAY_IPC_MAX_CONNS];
    uint32_t          n_conns;
    bool              running;
    bool              restricted;  /* -U mode */
} ray_ipc_server_t;
```

**Step 4: Build and test**

```bash
make test
```
Expected: all tests pass. The `restricted` flag is false by default, so existing tests are unaffected.

**Step 5: Commit**

```bash
git add src/lang/eval.c src/core/ipc.c src/core/ipc.h
git commit -m "feat: restricted mode enforcement — RAY_FN_RESTRICTED on 14 builtins"
```

---

### Task 6: Tests

**Files:**
- Modify: `test/test_store.c` — add auth and restricted mode tests

**Step 1: Write auth handshake test**

Add after the existing `test_ipc_async_send` test:

```c
/* ---- IPC auth handshake ------------------------------------------------- */

static MunitResult test_ipc_auth_success(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_runtime_t* rt = ray_runtime_create(0, NULL);
    munit_assert_ptr_not_null(rt);

    ray_ipc_server_t srv;
    ray_err_t err = ray_ipc_server_init(&srv, 0);
    munit_assert_int(err, ==, RAY_OK);

    /* Configure auth on server */
    strcpy((char*)&srv + offsetof(ray_ipc_server_t, ???)); 
    /* Actually — the legacy server doesn't have auth_secret.
     * We need a different approach: test via the poll-based API. */

    /* Use poll-based server for auth tests */
    ray_poll_t* poll = ray_poll_create();
    munit_assert_ptr_not_null(poll);
    strcpy(poll->auth_secret, "secret123");
    poll->restricted = false;

    int64_t lid = ray_ipc_listen(poll, 0);
    munit_assert_int(lid, >=, 0);
    ray_selector_t* lsel = ray_poll_get(poll, lid);
    uint16_t port = get_listen_port((ray_sock_t)lsel->fd);
    munit_assert_int(port, >, 0);

    /* Client: connect with correct creds — should succeed */
    /* Need to poll in background. Use a thread. */
    /* ... similar pattern to test_ipc_sync_roundtrip ... */

    ray_poll_destroy(poll);
    ray_runtime_destroy(rt);
    return MUNIT_OK;
}
```

Given the complexity of poll-based auth testing with threads, write these tests using the legacy server API. Add `auth_secret[256]` to `ray_ipc_server_t` and wire up the legacy handshake path too.

Better approach: add auth to the legacy server path as well (Task 6a), then test with that.

**Step 1a: Add auth to legacy server**

Add `char auth_secret[256];` to `ray_ipc_server_t` (already done in Task 5).

Modify `conn_on_handshake` in `ipc.c`:

```c
static void conn_on_handshake(ray_ipc_server_t* srv, ray_ipc_conn_t* c)
{
    /* Send handshake response: version + auth_required */
    bool auth_req = (srv->auth_secret[0] != '\0');
    uint8_t resp[2] = { RAY_VERSION_MAJOR, auth_req ? 0x01 : 0x00 };
    ray_sock_send(c->fd, resp, 2);

    if (auth_req) {
        /* Wait for credentials: first read length byte */
        ray_sys_free(c->rx_buf);
        c->rx_buf  = NULL;
        c->rx_len  = 0;
        c->rx_need = 1; /* length byte */
        c->phase   = RAY_IPC_PHASE_CREDS;
        return;
    }

    ray_sys_free(c->rx_buf);
    c->rx_buf  = NULL;
    c->rx_len  = 0;
    c->rx_need = sizeof(ray_ipc_header_t);
    c->phase   = RAY_IPC_PHASE_HEADER;
}
```

Add `#define RAY_IPC_PHASE_CREDS 3` near the other phase defines.

Add `conn_on_creds`:

```c
static void conn_on_creds(ray_ipc_server_t* srv, ray_ipc_conn_t* c)
{
    if (c->rx_len == 1) {
        /* Got length byte — now need the credential bytes */
        uint8_t cred_len = c->rx_buf[0];
        c->rx_need = 1 + cred_len;
        return; /* conn_on_readable will call us again when full */
    }

    /* Full credentials received */
    uint8_t cred_len = c->rx_buf[0];
    const char* creds = (const char*)(c->rx_buf + 1);
    const char* colon = memchr(creds, ':', cred_len);
    const char* pw = colon ? colon + 1 : creds;
    size_t pw_len = colon ? (size_t)(cred_len - (pw - creds)) : cred_len;
    if (pw_len > 0 && pw[pw_len - 1] == '\0') pw_len--;

    bool ok = (pw_len == strlen(srv->auth_secret) &&
               memcmp(pw, srv->auth_secret, pw_len) == 0);

    uint8_t result = ok ? 0x00 : 0x01;
    ray_sock_send(c->fd, &result, 1);

    if (!ok) {
        conn_close(srv, c);
        return;
    }

    ray_sys_free(c->rx_buf);
    c->rx_buf  = NULL;
    c->rx_len  = 0;
    c->rx_need = sizeof(ray_ipc_header_t);
    c->phase   = RAY_IPC_PHASE_HEADER;
}
```

Update `conn_on_readable` switch to include `RAY_IPC_PHASE_CREDS`:

```c
    case RAY_IPC_PHASE_CREDS:     conn_on_creds(srv, c);     break;
```

**Step 1b: Write auth tests**

```c
/* ---- IPC auth: success -------------------------------------------------- */

static MunitResult test_ipc_auth_success(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_runtime_t* rt = ray_runtime_create(0, NULL);
    munit_assert_ptr_not_null(rt);

    ray_ipc_server_t srv;
    ray_err_t err = ray_ipc_server_init(&srv, 0);
    munit_assert_int(err, ==, RAY_OK);
    strcpy(srv.auth_secret, "secret123");

    uint16_t port = get_listen_port(srv.listen_fd);

    ray_vm_t* srv_vm = (ray_vm_t*)ray_sys_alloc(sizeof(ray_vm_t));
    memset(srv_vm, 0, sizeof(ray_vm_t));
    srv_vm->id = 1;
    ipc_thread_ctx_t ctx = { .srv = &srv, .vm = srv_vm };
    ray_thread_t tid;
    ray_thread_create(&tid, server_thread_fn, &ctx);

    /* Connect with correct creds */
    int64_t h = ray_ipc_connect("127.0.0.1", port, "admin", "secret123");
    munit_assert_int(h, >=, 0);

    /* Send sync query to verify connection works */
    ray_t* msg = ray_str("(+ 10 20)", 9);
    ray_t* result = ray_ipc_send(h, msg);
    ray_release(msg);
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->i64, ==, 30);
    ray_release(result);

    ray_ipc_close(h);
    srv.running = false;
    ray_thread_join(tid);
    ray_ipc_server_destroy(&srv);
    ray_sys_free(srv_vm);
    ray_runtime_destroy(rt);
    return MUNIT_OK;
}

/* ---- IPC auth: wrong password ------------------------------------------- */

static MunitResult test_ipc_auth_reject(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_runtime_t* rt = ray_runtime_create(0, NULL);
    munit_assert_ptr_not_null(rt);

    ray_ipc_server_t srv;
    ray_ipc_server_init(&srv, 0);
    strcpy(srv.auth_secret, "secret123");

    uint16_t port = get_listen_port(srv.listen_fd);

    ray_vm_t* srv_vm = (ray_vm_t*)ray_sys_alloc(sizeof(ray_vm_t));
    memset(srv_vm, 0, sizeof(ray_vm_t));
    ipc_thread_ctx_t ctx = { .srv = &srv, .vm = srv_vm };
    ray_thread_t tid;
    ray_thread_create(&tid, server_thread_fn, &ctx);

    /* Connect with wrong password — should fail */
    int64_t h = ray_ipc_connect("127.0.0.1", port, "admin", "wrong");
    munit_assert_int(h, ==, -3); /* auth rejected */

    srv.running = false;
    ray_thread_join(tid);
    ray_ipc_server_destroy(&srv);
    ray_sys_free(srv_vm);
    ray_runtime_destroy(rt);
    return MUNIT_OK;
}

/* ---- IPC auth: no creds when required ----------------------------------- */

static MunitResult test_ipc_auth_no_creds(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_runtime_t* rt = ray_runtime_create(0, NULL);
    munit_assert_ptr_not_null(rt);

    ray_ipc_server_t srv;
    ray_ipc_server_init(&srv, 0);
    strcpy(srv.auth_secret, "secret123");

    uint16_t port = get_listen_port(srv.listen_fd);

    ray_vm_t* srv_vm = (ray_vm_t*)ray_sys_alloc(sizeof(ray_vm_t));
    memset(srv_vm, 0, sizeof(ray_vm_t));
    ipc_thread_ctx_t ctx = { .srv = &srv, .vm = srv_vm };
    ray_thread_t tid;
    ray_thread_create(&tid, server_thread_fn, &ctx);

    /* Connect without creds — should fail */
    int64_t h = ray_ipc_connect("127.0.0.1", port, NULL, NULL);
    munit_assert_int(h, ==, -2); /* auth required */

    srv.running = false;
    ray_thread_join(tid);
    ray_ipc_server_destroy(&srv);
    ray_sys_free(srv_vm);
    ray_runtime_destroy(rt);
    return MUNIT_OK;
}

/* ---- IPC restricted mode ------------------------------------------------ */

static MunitResult test_ipc_restricted(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_runtime_t* rt = ray_runtime_create(0, NULL);
    munit_assert_ptr_not_null(rt);

    ray_ipc_server_t srv;
    ray_ipc_server_init(&srv, 0);
    strcpy(srv.auth_secret, "secret123");
    srv.restricted = true;

    uint16_t port = get_listen_port(srv.listen_fd);

    ray_vm_t* srv_vm = (ray_vm_t*)ray_sys_alloc(sizeof(ray_vm_t));
    memset(srv_vm, 0, sizeof(ray_vm_t));
    ipc_thread_ctx_t ctx = { .srv = &srv, .vm = srv_vm };
    ray_thread_t tid;
    ray_thread_create(&tid, server_thread_fn, &ctx);

    int64_t h = ray_ipc_connect("127.0.0.1", port, "admin", "secret123");
    munit_assert_int(h, >=, 0);

    /* Allowed: arithmetic */
    ray_t* msg = ray_str("(+ 1 2)", 7);
    ray_t* result = ray_ipc_send(h, msg);
    ray_release(msg);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->i64, ==, 3);
    ray_release(result);

    /* Blocked: set */
    msg = ray_str("(set x 42)", 10);
    result = ray_ipc_send(h, msg);
    ray_release(msg);
    munit_assert_true(RAY_IS_ERR(result));
    ray_release(result);

    /* Blocked: system */
    msg = ray_str("(system \"echo hi\")", 18);
    result = ray_ipc_send(h, msg);
    ray_release(msg);
    munit_assert_true(RAY_IS_ERR(result));
    ray_release(result);

    ray_ipc_close(h);
    srv.running = false;
    ray_thread_join(tid);
    ray_ipc_server_destroy(&srv);
    ray_sys_free(srv_vm);
    ray_runtime_destroy(rt);
    return MUNIT_OK;
}
```

**Step 2: Register tests in suite array**

Add to the test suite entries (after the async_send entry):

```c
    { "/ipc/auth_success",       test_ipc_auth_success,        NULL, NULL, 0, NULL },
    { "/ipc/auth_reject",        test_ipc_auth_reject,         NULL, NULL, 0, NULL },
    { "/ipc/auth_no_creds",      test_ipc_auth_no_creds,       NULL, NULL, 0, NULL },
    { "/ipc/restricted",         test_ipc_restricted,           NULL, NULL, 0, NULL },
```

**Step 3: Build and test**

```bash
make test
```
Expected: all tests pass including 4 new IPC auth tests.

**Step 4: Commit**

```bash
git add test/test_store.c src/core/ipc.c src/core/ipc.h
git commit -m "test: IPC auth handshake and restricted mode tests"
```

---

### Task 7: Verification

**Step 1: Full test suite**

```bash
make clean && make test
make clean && make release
```

**Step 2: Manual integration test**

Two terminals:

```bash
# Terminal 1: server with auth
./rayforce -p 5000 -u secret123

# Terminal 2: client with creds
echo '(do (set h (hopen "localhost:5000:admin:secret123")) (println (hsend h "(+ 1 2)")) (hclose h))' | ./rayforce
```
Expected: prints `3`.

```bash
# Terminal 2: client without creds — should fail
echo '(do (set h (hopen "localhost:5000")) (println h))' | ./rayforce
```
Expected: error about auth required.

**Step 3: Manual restricted mode test**

```bash
# Terminal 1: server with -U
./rayforce -p 5000 -U secret123

# Terminal 2: query works
echo '(do (set h (hopen "localhost:5000:admin:secret123")) (println (hsend h "(+ 1 2)")) (hclose h))' | ./rayforce
```
Expected: prints `3`.

```bash
# Terminal 2: mutation blocked
echo '(do (set h (hopen "localhost:5000:admin:secret123")) (println (hsend h "(set x 42)")) (hclose h))' | ./rayforce
```
Expected: error about restricted access.

**Step 4: Commit final verification pass**

```bash
git add -A
git commit -m "chore: verification pass — IPC auth + restricted mode"
```

---

## Summary

| Task | What | Files |
|------|------|-------|
| 1 | Add auth/restricted fields to poll, VM, eval flags | poll.h, runtime.h, eval.h |
| 2 | Parse -u/-U flags in main.c | main.c |
| 3 | Server-side auth handshake | ipc.c |
| 4 | Client-side auth + hopen format | ipc.h, ipc.c, system.c, test_store.c |
| 5 | Restricted mode enforcement | eval.c, ipc.c, ipc.h |
| 6 | Tests (auth success/reject/no-creds, restricted) | test_store.c, ipc.c, ipc.h |
| 7 | Verification (automated + manual) | — |
