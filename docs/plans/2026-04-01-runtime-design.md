# Runtime & Error System Design

## Motivation

Rayforce2 has global/TLS state scattered across files (eval.c, env.c, heap.c, sym.c, pool.c, format.c). The VM is allocated/freed on every `vm_exec` call. Errors are small-integer sentinel pointers with no descriptive messages. This design introduces a centralized runtime, persistent per-thread VMs, and first-class error objects.

## Error Model

Errors are real `ray_t` objects with `type = RAY_ERROR (127)`. The 8-byte value slot (`slen` + `sdata[7]`) carries an ASCII error code packed as an `int64_t`:

```c
#define RAY_ERROR 127
#define RAY_IS_ERR(p) ((p) && (p)->type == RAY_ERROR)
```

### ray_error() API

```c
ray_t* ray_error(const char* code, const char* fmt, ...);
```

- Allocates a `ray_t` with `type = RAY_ERROR`
- Packs up to 8 bytes of `code` into the value slot (memcpy, zero-padded)
- Formats optional detail into `__VM->err.msg` via vsnprintf
- Returns the allocated error object

Usage:
```c
return ray_error("arity", "expected %d args, got %d", 2, 1);
return ray_error("type", "cannot add i64 and str");
return ray_error("limit", NULL);  // no detail
return ray_error("myerr", "custom user error");  // user-defined codes
```

### Error info in VM (ephemeral)

```c
typedef struct {
    char msg[256];    /* formatted detail from ray_error() */
} ray_err_info_t;
```

Lives in `ray_vm_t`. Set by `ray_error()`, consumed by the error renderer. Not part of the error value itself — the `ray_t` travels over IPC, the description is local diagnostics.

### What this replaces

Killed: `ray_err_t` enum, `RAY_ERR_PTR()`, `RAY_ERR_CODE()`, `ray_err_str()`, `err_strings[]`, the `(uintptr_t)(p) < 32` check. All replaced by `ray_error()` + `RAY_IS_ERR()`.

## VM Struct

Per-thread, created once at runtime init, freed at destroy:

```c
typedef struct {
    /* hot — first cache line */
    int32_t        sp;
    int32_t        fp;
    int32_t        rp;
    int32_t        id;
    ray_t         *fn;
    void          *heap;
    int32_t        tp;
    /* stacks */
    ray_t         *ps[VM_STACK_SIZE];
    vm_ctx_t       rs[VM_STACK_SIZE];
    vm_trap_t      ts[VM_TRAP_SIZE];
    /* cold — error/debug state */
    ray_err_info_t err;
    ray_t         *nfo;
    ray_t         *trace;
    ray_t         *raise_val;
    /* scope (moved from env.c TLS) */
    ray_scope_frame_t scope_stack[SCOPE_CAP];
    int32_t        scope_depth;
} ray_vm_t;

extern _Thread_local ray_vm_t *__VM;
```

`vm_exec` no longer allocates/frees a VM — it resets sp/fp/rp/tp and uses `__VM`.

## Runtime Struct

Single global, owns all shared state:

```c
typedef struct {
    ray_vm_t     **vms;       /* VM array (vms[0] = main thread) */
    int32_t        n_vms;
    void          *pool;      /* executor pool */
    void          *sym;       /* symbol table */
} ray_runtime_t;

extern ray_runtime_t *__RUNTIME;
```

### Lifecycle

```c
ray_runtime_t* ray_runtime_create(int argc, char** argv);
void           ray_runtime_destroy(ray_runtime_t* rt);
```

`ray_runtime_create`: allocates runtime, creates pool, creates VMs (one per thread, vms[0] = main, sets `__VM`), inits sym table, inits env, registers builtins.

`ray_runtime_destroy`: tears down in reverse.

`ray_lang_init()` / `ray_lang_destroy()` become thin wrappers or get replaced.

## File Layout

```
src/core/runtime.h    — ray_runtime_t, ray_vm_t, ray_err_info_t structs + API
src/core/runtime.c    — ray_runtime_create/destroy, ray_error()
```

- `err.h` / `err.c` absorbed into runtime
- `eval.h` loses ray_vm_t definition (moves to runtime.h), keeps eval/compile API
- `eval.c` loses VM alloc/free in vm_exec, resets __VM state at entry
- `env.c` loses TLS scope_stack/scope_depth, reads from __VM->

## Migration

All `RAY_ERR_PTR(RAY_ERR_*)` sites become `ray_error("code", ...)` calls. All `RAY_IS_ERR` checks updated from `(uintptr_t)(p) < 32` to `(p)->type == RAY_ERROR`. Error renderer uses `__VM->err.msg` when available, falls back to the 8-byte code from the ray_t.
