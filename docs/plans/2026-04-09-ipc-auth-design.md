# IPC Authentication Design

## Overview

Add kdb+-style shared-secret authentication to the IPC handshake. Server started with `-u password` requires clients to authenticate. `-U password` additionally restricts IPC evals to read-only (no mutation, no filesystem, no IPC chaining).

## Wire Format

Current handshake (no auth):
```
Client → Server:  [version_byte, 0x00]          (2 bytes)
Server → Client:  [version_byte, 0x00]          (2 bytes)
```

New handshake (auth):
```
Client → Server:  [version_byte, 0x00]          (2 bytes)
Server → Client:  [version_byte, auth_required]  (2 bytes, 0x00=no auth, 0x01=auth required)
Client → Server:  [len_u8, "user:password\0"]    (1 + len bytes, max 255)
Server → Client:  [0x00]                         (1 byte: 0x00=accepted, 0x01=rejected → close)
```

If server has no `-u`/`-U`, `auth_required = 0x00` and no extra round-trip (backward compatible).

## Server Config

Two new fields on `ray_poll_t`:

```c
char     auth_secret[256];  /* password from -u/-U, empty = no auth */
bool     restricted;        /* true if -U (restricted mode) */
```

`main.c` parses `-u`/`-U` args, copies password into `poll->auth_secret`, sets `poll->restricted`.

Server-side validation: split received credential on first `:`, compare password portion against `auth_secret`. Username is accepted but ignored (kdb+ behavior).

## Client Config

`hopen` format: `(hopen "host:port:user:password")`

- 2 fields (`host:port`): no auth
- 4 fields (`host:port:user:password`): sends `user:password` during handshake
- If server requires auth and client provides no creds, handshake fails

## Restricted Mode (-U)

`-U` restricts IPC-originated evals. A `restricted` flag on `ray_vm_t`:

```c
typedef struct {
    /* ... existing fields ... */
    bool             restricted;   /* IPC -U mode: reject restricted builtins */
} ray_vm_t;
```

A new function flag `RAY_FN_RESTRICTED` (`0x20`) marks builtins that are forbidden in restricted mode. The eval dispatcher checks `__VM->restricted && (fn_flags & RAY_FN_RESTRICTED)` and returns an error.

`eval_payload` in `ipc.c` sets `__VM->restricted = poll->restricted` before eval, clears after.

### Restricted builtins (14 total)

**I/O:** `read-csv`, `write-csv`, `read-file`, `write-file`, `system`, `getenv`
**Mutation:** `set`, `delete`, `update`, `upsert`, `insert`
**IPC:** `hopen`, `hclose`, `hsend`

## Files Changed

| File | Change |
|------|--------|
| `src/core/poll.h` | Add `auth_secret[256]`, `restricted` to `ray_poll_t` |
| `src/core/runtime.h` | Add `restricted` bool to `ray_vm_t` |
| `src/core/ipc.c` | Auth handshake in `ipc_read_handshake`, pass restricted flag to eval |
| `src/lang/eval.h` | Add `RAY_FN_RESTRICTED` flag (`0x20`) |
| `src/lang/eval.c` | Check restricted flag in dispatch, add flag to 14 builtin registrations |
| `src/ops/system.c` | (no change — restriction handled at dispatch level) |
| `src/app/main.c` | Parse `-u`/`-U` flags |
| `test/test_store.c` | Auth handshake tests, restricted mode tests |
