# Runtime & Error System Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Introduce `ray_runtime_t` to centralize all global/TLS state, persistent per-thread VMs, and first-class `ray_t` error objects with descriptive messages.

**Architecture:** Three phases — (1) add `runtime.h/c` with new structs + `ray_error()` API, (2) migrate VM lifecycle and error creation sites, (3) clean up dead code. Each phase keeps the build green.

**Tech Stack:** Pure C17, munit test framework, no external deps.

---

### Task 1: Create `src/core/runtime.h` with new types

**Files:**
- Create: `src/core/runtime.h`

**Step 1: Write the header**

```c
#ifndef RAY_RUNTIME_H
#define RAY_RUNTIME_H

#include <rayforce.h>
#include <stdarg.h>

/* ===== Error Info (per-VM, ephemeral) ===== */

typedef struct {
    char msg[256];
} ray_err_info_t;

/* ===== Scope Frame (moved from env.c) ===== */

#define RAY_SCOPE_CAP  64
#define RAY_FRAME_CAP  64

typedef struct {
    int64_t keys[RAY_FRAME_CAP];
    ray_t*  vals[RAY_FRAME_CAP];
    int32_t count;
} ray_scope_frame_t;

/* ===== VM sub-types ===== */

#define RAY_VM_STACK_SIZE 1024
#define RAY_VM_TRAP_SIZE  16

typedef struct {
    ray_t   *fn;
    int32_t  fp;
    int32_t  ip;
} ray_vm_ctx_t;

typedef struct {
    int32_t  rp;
    int32_t  sp;
    int32_t  handler_ip;
    ray_t   *fn;
    int32_t  fp;
    int32_t  n_locals;
} ray_vm_trap_t;

/* ===== Per-thread VM ===== */

typedef struct {
    /* hot path */
    int32_t          sp;
    int32_t          fp;
    int32_t          rp;
    int32_t          id;
    ray_t           *fn;
    void            *heap;
    int32_t          tp;
    /* stacks */
    ray_t           *ps[RAY_VM_STACK_SIZE];
    ray_vm_ctx_t     rs[RAY_VM_STACK_SIZE];
    ray_vm_trap_t    ts[RAY_VM_TRAP_SIZE];
    /* cold — error/debug */
    ray_err_info_t   err;
    ray_t           *nfo;
    ray_t           *trace;
    ray_t           *raise_val;
    /* scope */
    ray_scope_frame_t scope_stack[RAY_SCOPE_CAP];
    int32_t          scope_depth;
} ray_vm_t;

/* ===== Runtime ===== */

typedef struct {
    ray_vm_t       **vms;
    int32_t          n_vms;
    void            *pool;
    void            *sym;
} ray_runtime_t;

/* Global runtime + per-thread VM */
extern ray_runtime_t *__RUNTIME;
extern _Thread_local ray_vm_t *__VM;

/* Lifecycle */
ray_runtime_t* ray_runtime_create(int argc, char** argv);
void           ray_runtime_destroy(ray_runtime_t* rt);

/* Error API — allocates ray_t with type=RAY_ERROR, sets __VM->err.msg */
ray_t* ray_error(const char* code, const char* fmt, ...);
ray_t* ray_verror(const char* code, const char* fmt, va_list ap);

/* Read error code from a RAY_ERROR object (returns pointer to 8-byte packed string) */
const char* ray_err_code(ray_t* err);

/* Read VM error detail message (NULL if empty) */
const char* ray_error_msg(void);

/* Clear VM error detail */
void ray_error_clear(void);

#endif /* RAY_RUNTIME_H */
```

**Step 2: Add `RAY_ERROR` type constant to `include/rayforce.h`**

In `rayforce.h`, after the existing type constants, add:

```c
#define RAY_ERROR     127  /* Error object: 8-byte packed ASCII code in sdata */
```

Update `RAY_IS_ERR` macro:

```c
/* Old: #define RAY_IS_ERR(p) ((uintptr_t)(p) < 32) */
#define RAY_IS_ERR(p) ((p) != NULL && (uintptr_t)(p) > 31 && (p)->type == RAY_ERROR)
```

Keep old `RAY_ERR_PTR` / `RAY_ERR_CODE` macros temporarily with `/* DEPRECATED */` comments so existing code compiles during migration.

**Step 3: Build and verify**

Run: `make clean && make`
Expected: compiles clean (new header not yet included anywhere)

**Step 4: Commit**

```bash
git add src/core/runtime.h include/rayforce.h
git commit -m "feat: add runtime.h with ray_runtime_t, ray_vm_t, ray_error API"
```

---

### Task 2: Implement `src/core/runtime.c` — `ray_error()` and lifecycle stubs

**Files:**
- Create: `src/core/runtime.c`
- Modify: `Makefile` (add to build)

**Step 1: Write runtime.c**

```c
#include "runtime.h"
#include <stdio.h>
#include <string.h>

ray_runtime_t *__RUNTIME = NULL;
_Thread_local ray_vm_t *__VM = NULL;

/* ---- Error API ---- */

ray_t* ray_verror(const char* code, const char* fmt, va_list ap) {
    /* Pack code into ray_t sdata (SSO slot) */
    ray_t* err = ray_alloc(0);
    if (!err) return NULL;  /* OOM during error — catastrophic */
    err->type = RAY_ERROR;
    err->slen = 0;
    memset(err->sdata, 0, 7);
    if (code) {
        size_t len = strlen(code);
        if (len > 7) len = 7;
        memcpy(err->sdata, code, len);
        err->slen = (uint8_t)len;
    }

    /* Format detail into VM error info */
    if (__VM && fmt) {
        vsnprintf(__VM->err.msg, sizeof(__VM->err.msg), fmt, ap);
    } else if (__VM) {
        __VM->err.msg[0] = '\0';
    }

    return err;
}

ray_t* ray_error(const char* code, const char* fmt, ...) {
    va_list ap;
    if (fmt) va_start(ap, fmt);
    ray_t* err = ray_verror(code, fmt, fmt ? ap : (va_list){0});
    if (fmt) va_end(ap);
    return err;
}

const char* ray_error_msg(void) {
    if (!__VM || !__VM->err.msg[0]) return NULL;
    return __VM->err.msg;
}

void ray_error_clear(void) {
    if (__VM) __VM->err.msg[0] = '\0';
}

/* ---- Lifecycle (stubs — wired up in later tasks) ---- */

ray_runtime_t* ray_runtime_create(int argc, char** argv) {
    (void)argc; (void)argv;
    /* TODO: allocate runtime, VMs, init subsystems */
    return NULL;
}

void ray_runtime_destroy(ray_runtime_t* rt) {
    (void)rt;
    /* TODO: tear down in reverse */
}
```

**Step 2: Add runtime.c to Makefile**

Find the `CORE_SRC` or equivalent variable and add `src/core/runtime.c`.

**Step 3: Build**

Run: `make clean && make`
Expected: compiles clean

**Step 4: Commit**

```bash
git add src/core/runtime.c Makefile
git commit -m "feat: implement ray_error() API and runtime lifecycle stubs"
```

---

### Task 3: Write tests for `ray_error()` and `RAY_IS_ERR`

**Files:**
- Modify: `test/test_err.c` (rewrite for new error model)
- Modify: `test/test_main.c` (if needed)

**Step 1: Rewrite test_err.c**

Replace the existing tests with ones that test the new model. The test file needs a setup/teardown that initializes the heap and a VM so `ray_error()` can allocate and write `__VM->err.msg`. For now, manually create a minimal VM:

```c
#include "munit.h"
#include "core/runtime.h"
#include "mem/heap.h"
#include <string.h>

/* Minimal VM for tests */
static ray_vm_t test_vm;

static void* err_setup(const MunitParameter params[], void* user_data) {
    (void)params; (void)user_data;
    memset(&test_vm, 0, sizeof(test_vm));
    __VM = &test_vm;
    return NULL;
}

static void err_teardown(void* fixture) {
    (void)fixture;
    __VM = NULL;
}

static MunitResult test_error_basic(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* err = ray_error("type", NULL);
    munit_assert_not_null(err);
    munit_assert_true(RAY_IS_ERR(err));
    munit_assert_int(err->slen, ==, 4);
    munit_assert_memory_equal(4, err->sdata, "type");
    ray_release(err);
    return MUNIT_OK;
}

static MunitResult test_error_with_message(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* err = ray_error("arity", "expected %d args, got %d", 2, 1);
    munit_assert_true(RAY_IS_ERR(err));
    munit_assert_int(err->slen, ==, 5);
    munit_assert_memory_equal(5, err->sdata, "arity");
    munit_assert_string_equal(ray_error_msg(), "expected 2 args, got 1");
    ray_release(err);
    return MUNIT_OK;
}

static MunitResult test_error_code_max_length(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* err = ray_error("longcode", "detail");
    munit_assert_true(RAY_IS_ERR(err));
    /* "longcode" is 8 chars but sdata is 7, truncated */
    munit_assert_int(err->slen, ==, 7);
    munit_assert_memory_equal(7, err->sdata, "longcod");
    ray_release(err);
    return MUNIT_OK;
}

static MunitResult test_error_null_not_error(const void* params, void* fixture) {
    (void)params; (void)fixture;
    munit_assert_false(RAY_IS_ERR(NULL));
    return MUNIT_OK;
}

static MunitResult test_error_normal_obj_not_error(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* i = ray_i64(42);
    munit_assert_false(RAY_IS_ERR(i));
    ray_release(i);
    return MUNIT_OK;
}

static MunitResult test_error_clear(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_error("test", "some detail");
    munit_assert_not_null(ray_error_msg());
    ray_error_clear();
    munit_assert_null(ray_error_msg());
    return MUNIT_OK;
}
```

Update the suite array and registration accordingly.

**Step 2: Run tests**

Run: `make test`
Expected: new error tests pass, other suites may have failures due to `RAY_IS_ERR` change — that's expected and fixed in later tasks.

**Step 3: Commit**

```bash
git add test/test_err.c
git commit -m "test: rewrite error tests for ray_error() and RAY_ERROR type"
```

---

### Task 4: Migrate `RAY_ERR_PTR` → `ray_error()` in eval.c

This is the biggest task (426 sites). The migration is mechanical:

| Old pattern | New pattern |
|---|---|
| `return RAY_ERR_PTR(RAY_ERR_TYPE)` | `return ray_error("type", NULL)` |
| `return RAY_ERR_PTR(RAY_ERR_OOM)` | `return ray_error("oom", NULL)` |
| `return RAY_ERR_PTR(RAY_ERR_DOMAIN)` | `return ray_error("domain", NULL)` |
| `return RAY_ERR_PTR(RAY_ERR_LIMIT)` | `return ray_error("limit", NULL)` |
| `return RAY_ERR_PTR(RAY_ERR_NAME)` | `return ray_error("name", NULL)` |
| `return RAY_ERR_PTR(RAY_ERR_RANGE)` | `return ray_error("range", NULL)` |
| `return RAY_ERR_PTR(RAY_ERR_PARSE)` | `return ray_error("parse", NULL)` |
| `return RAY_ERR_PTR(RAY_ERR_NYI)` | `return ray_error("nyi", NULL)` |
| `return RAY_ERR_PTR(RAY_ERR_LENGTH)` | `return ray_error("length", NULL)` |

**Files:**
- Modify: `src/lang/eval.c`

**Step 1: Add `#include "core/runtime.h"` to eval.c**

**Step 2: Search-and-replace all `RAY_ERR_PTR(RAY_ERR_*)` patterns**

Use sed or manual replacement. For each, convert to `ray_error("code", NULL)`. Where context makes a descriptive message obvious, add one:

Key sites to add messages:
- `call_lambda` arity check (new): `ray_error("arity", "expected %" PRId64 " args, got %" PRId64, param_count, argc)`
- `vm_error_limit`: `ray_error("limit", "stack overflow")`
- `vm_error_name`: `ray_error("name", NULL)` (the symbol name could be added later)
- `eval_depth > RAY_EVAL_MAX_DEPTH`: `ray_error("limit", "eval depth exceeded")`

**Step 3: Add arity check in `call_lambda`**

At top of `call_lambda`, after getting `param_count`:

```c
int64_t param_count = ray_len(LAMBDA_PARAMS(lambda));
if (argc != param_count)
    return ray_error("arity", "expected %" PRId64 " args, got %" PRId64, param_count, argc);
```

**Step 4: Fix the VM error cleanup path**

In `vm_error_cleanup`, the `vm_err_code` variable becomes a string. Change:

```c
/* Old */
ray_err_t vm_err_code = RAY_ERR_DOMAIN;
...
vm_error_limit:
    vm_err_code = RAY_ERR_LIMIT;
...
return RAY_ERR_PTR(vm_err_code);

/* New */
const char* vm_err_str = "domain";
...
vm_error_limit:
    vm_err_str = "limit";
...
return ray_error(vm_err_str, NULL);
```

**Step 5: Remove VM alloc/free from `vm_exec`**

Remove the `ray_alloc(sizeof(ray_vm_t))` / `ray_free(vm_block)` blocks. Use `__VM` directly. Reset VM state at entry:

```c
static ray_t* vm_exec(ray_t* lambda, ray_t** call_args, int64_t argc) {
    static void *dispatch[OP__COUNT] = { ... };

    ray_vm_t *vmp = __VM;
#define vm (*vmp)

    /* Reset VM state for this execution */
    vm.fn = lambda;
    ray_retain(lambda);
    int32_t n_locals = LAMBDA_NLOCALS(lambda);
    vm.fp = 0;
    vm.sp = n_locals;
    vm.rp = 0;
    vm.tp = 0;
    ...
```

On exit paths, don't free the VM block — just clean up stack contents.

**Step 6: Move TLS variables into __VM**

Replace:
- `g_eval_nfo` → `__VM->nfo`
- `g_error_trace` → `__VM->trace`
- `__raise_val` → `__VM->raise_val`

Update accessors:
- `ray_eval_get_nfo()` → `return __VM ? __VM->nfo : NULL;`
- `ray_get_error_trace()` → `return __VM ? __VM->trace : NULL;`
- `ray_clear_error_trace()` → clears `__VM->trace`

**Step 7: Build and run tests**

Run: `make clean && make && make test`
Expected: eval.c compiles, `--suite /err` passes, `--suite /lang` may need fixes

**Step 8: Commit**

```bash
git add src/lang/eval.c
git commit -m "refactor: migrate eval.c to ray_error() API, persistent VM, arity checks"
```

---

### Task 5: Migrate `RAY_ERR_PTR` in remaining source files

**Files:** (each independently, can be done in parallel)
- `src/ops/exec.c` (264 sites)
- `src/vec/vec.c` (36 sites)
- `src/store/col.c` (44 sites)
- `src/lang/parse.c` (26 sites)
- `src/store/splay.c` (18 sites)
- `src/store/part.c` (16 sites)
- `src/io/csv.c` (5 sites)
- `src/ops/fvec.c` (5 sites)
- `src/vec/sel.c`, `src/vec/list.c`, `src/table/table.c` (4 each)
- `src/ops/graph.c`, `src/core/block.c`, `src/mem/heap.c`, `src/lang/env.c`, `src/lang/nfo.c` (1-3 each)

Same mechanical transformation: `RAY_ERR_PTR(RAY_ERR_X)` → `ray_error("x", NULL)`.

**Important:** files that don't have `__VM` context (e.g., `heap.c`, `block.c`, `col.c`) — `ray_error()` will still allocate the error object correctly; `__VM->err.msg` just won't be written if `__VM` is NULL. This is fine — those are low-level paths where the code string alone is sufficient.

**Step 1: Add `#include "core/runtime.h"` to each file**

**Step 2: Replace all `RAY_ERR_PTR(RAY_ERR_*)` calls**

**Step 3: Build and test**

Run: `make clean && make && make test`

**Step 4: Commit**

```bash
git add src/ops/ src/vec/ src/store/ src/io/ src/table/ src/core/ src/lang/
git commit -m "refactor: migrate all source files to ray_error() API"
```

---

### Task 6: Migrate env.c scope stack to `__VM`

**Files:**
- Modify: `src/lang/env.c`
- Modify: `src/lang/env.h`

**Step 1: Remove TLS scope_stack/scope_depth from env.c**

Delete:
```c
static _Thread_local ray_scope_frame_t scope_stack[SCOPE_CAP];
static _Thread_local int32_t scope_depth = 0;
```

**Step 2: Replace all `scope_stack` / `scope_depth` references with `__VM->scope_stack` / `__VM->scope_depth`**

Update functions: `ray_env_push_scope`, `ray_env_pop_scope`, `ray_env_set_local`, `ray_env_find`, `ray_env_scope_depth`, `ray_env_init`, `ray_env_destroy`, `ray_env_reset`.

Remove `ray_scope_frame_t` typedef from env.c (now in runtime.h).
Remove `SCOPE_CAP` / `FRAME_CAP` defines from env.c (now `RAY_SCOPE_CAP` / `RAY_FRAME_CAP` in runtime.h).

**Step 3: Update env.h**

Remove `#include "lang/eval.h"` dependency if no longer needed. Add `#include "core/runtime.h"`.

**Step 4: Build and test**

Run: `make clean && make && make test`

**Step 5: Commit**

```bash
git add src/lang/env.c src/lang/env.h
git commit -m "refactor: move scope stack from TLS to ray_vm_t"
```

---

### Task 7: Update format.c and repl.c for new error model

**Files:**
- Modify: `src/app/format.c`
- Modify: `src/app/repl.c`

**Step 1: Update `fmt_obj` in format.c**

```c
/* Old */
if (RAY_IS_ERR(obj)) {
    ray_err_t code = RAY_ERR_CODE(obj);
    fmt_puts(b, "error: ");
    fmt_puts(b, ray_err_str(code));
    return;
}

/* New */
if (RAY_IS_ERR(obj)) {
    fmt_puts(b, "error: ");
    fmt_putc_n(b, obj->sdata, obj->slen);
    return;
}
```

**Step 2: Update `fmt_error_with_trace` in repl.c**

```c
/* Old */
ray_err_t code = RAY_ERR_CODE(err);
const char* msg = ray_err_str(code);
...
fprintf(fp, "  \xc3\x97 Error: %s", msg);

/* New */
const char* code = (const char*)err->sdata;
int code_len = err->slen;
const char* detail = ray_error_msg();
...
fprintf(fp, "  \xc3\x97 Error: %.*s", code_len, code);
if (detail && detail[0])
    fprintf(fp, " — %s", detail);
```

Also update `repl_print_result` which uses `RAY_ERR_CODE` / `ray_err_str` in the no-trace path.

**Step 3: Build and test**

Run: `make clean && make && make test`
Run: `./rayforce` and test REPL error display manually:
```
‣ (+ 1 "s")
  × Error: type — cannot add i64 and str
‣ (set f (fn [x y] (self (+ x y) 1)))
‣ (f 6)
  × Error: arity — expected 2 args, got 1
```

**Step 4: Commit**

```bash
git add src/app/format.c src/app/repl.c
git commit -m "feat: render descriptive error messages in REPL and formatter"
```

---

### Task 8: Wire up `ray_runtime_create` / `ray_runtime_destroy`

**Files:**
- Modify: `src/core/runtime.c`
- Modify: `src/lang/repl.c`
- Modify: `test/test_lang.c`

**Step 1: Implement `ray_runtime_create`**

```c
ray_runtime_t* ray_runtime_create(int argc, char** argv) {
    (void)argc; (void)argv;

    ray_heap_init();
    ray_sym_init();

    /* Allocate runtime via system allocator (heap not yet assigned to runtime) */
    ray_runtime_t* rt = (ray_runtime_t*)ray_sys_alloc(sizeof(ray_runtime_t));
    if (!rt) return NULL;
    memset(rt, 0, sizeof(*rt));

    /* Create main VM (id=0) */
    rt->n_vms = 1;
    rt->vms = (ray_vm_t**)ray_sys_alloc(sizeof(ray_vm_t*));
    rt->vms[0] = (ray_vm_t*)ray_sys_alloc(sizeof(ray_vm_t));
    memset(rt->vms[0], 0, sizeof(ray_vm_t));
    rt->vms[0]->id = 0;
    __VM = rt->vms[0];

    /* Init language (env + builtins) */
    ray_env_init();
    ray_register_builtins();

    __RUNTIME = rt;
    return rt;
}
```

**Step 2: Implement `ray_runtime_destroy`**

```c
void ray_runtime_destroy(ray_runtime_t* rt) {
    if (!rt) return;

    ray_env_destroy();
    ray_compile_reset();

    /* Free VMs */
    for (int32_t i = 0; i < rt->n_vms; i++) {
        if (rt->vms[i]->raise_val) ray_release(rt->vms[i]->raise_val);
        if (rt->vms[i]->trace) ray_release(rt->vms[i]->trace);
        ray_sys_free(rt->vms[i], sizeof(ray_vm_t));
    }
    ray_sys_free(rt->vms, rt->n_vms * sizeof(ray_vm_t*));

    __VM = NULL;
    __RUNTIME = NULL;

    ray_sym_destroy();
    ray_heap_destroy();

    ray_sys_free(rt, sizeof(ray_runtime_t));
}
```

**Step 3: Update `repl.c` main()**

```c
int main(int argc, char** argv) {
    ray_runtime_t* rt = ray_runtime_create(argc, argv);
    if (!rt) { fprintf(stderr, "failed to create runtime\n"); return 1; }
    ...
    ray_runtime_destroy(rt);
    return rc;
}
```

**Step 4: Update test_lang.c setup/teardown**

```c
static void* lang_setup(...) {
    ray_runtime_create(0, NULL);
    return NULL;
}
static void lang_teardown(void* fixture) {
    (void)fixture;
    ray_runtime_destroy(__RUNTIME);
}
```

**Step 5: Build and test**

Run: `make clean && make && make test`

**Step 6: Commit**

```bash
git add src/core/runtime.c src/lang/repl.c test/test_lang.c
git commit -m "feat: wire up ray_runtime_create/destroy, replace ray_lang_init"
```

---

### Task 9: Clean up dead code

**Files:**
- Modify: `include/rayforce.h` — remove deprecated `RAY_ERR_PTR`, `RAY_ERR_CODE`, `ray_err_t` enum (keep `ray_err_t` for the I/O return path), remove `ray_err_str` declaration
- Delete: `src/core/err.c` — absorbed into runtime.c
- Modify: `src/core/err.h` — minimal, just re-export `ray_err_t` enum for I/O functions
- Remove: `ray_lang_init` / `ray_lang_destroy` declarations from `eval.h`
- Remove: old `vm_ctx_t`, `vm_trap_t`, `ray_vm_t` from `eval.h` (now in runtime.h)
- Remove: `__VM` declaration from eval.c (now in runtime.c)
- Remove: TLS variables from eval.c (`g_eval_nfo`, `g_error_trace`, `__raise_val`)

**Step 1: Clean up rayforce.h**

Keep `ray_err_t` enum (still used by I/O/platform functions) but remove:
- `RAY_ERR_PTR` macro
- `RAY_ERR_CODE` macro
- `ray_err_str` declaration

Keep `RAY_IS_ERR` as the new version (type check).

**Step 2: Clean up eval.h**

Remove `VM_STACK_SIZE`, `vm_ctx_t`, `vm_trap_t`, `ray_vm_t` definitions — they now live in `runtime.h`. eval.h should `#include "core/runtime.h"` for backward compat with files that include eval.h expecting these types.

Remove `ray_lang_init` / `ray_lang_destroy` declarations.

**Step 3: Remove err.c**

Remove from Makefile build, delete file.

**Step 4: Build and test**

Run: `make clean && make && make test`
Run: `./rayforce.test` — all suites pass
Run: `./rayforce` — REPL works, errors display correctly

**Step 5: Commit**

```bash
git add -A
git commit -m "chore: remove deprecated error macros, dead TLS state, old VM types"
```

---

### Task 10: Add descriptive messages to key error sites

Now that the infrastructure is in place, go back and add descriptive messages where they matter most. This is the payoff — turning opaque errors into actionable diagnostics.

**Files:**
- Modify: `src/lang/eval.c` — enrich `ray_error()` calls

Key sites:

```c
/* Arity (already done in task 4) */
ray_error("arity", "expected %" PRId64 " args, got %" PRId64, param_count, argc);

/* Type errors in builtins */
ray_error("type", "%s expects %s, got %s", fn_name, expected_type, actual_type);

/* Name resolution */
ray_error("name", "'%.*s' is not defined", (int)len, name);

/* Stack overflow */
ray_error("limit", "stack overflow (depth %d)", vm.rp);

/* Eval depth */
ray_error("limit", "eval depth exceeded (max %d)", RAY_EVAL_MAX_DEPTH);
```

This task is open-ended — add messages incrementally based on which errors users hit most. Start with the ones that triggered this whole investigation: arity and limit.

**Step 1: Add messages to highest-value sites**

**Step 2: Test in REPL**

Run: `./rayforce`
```
‣ (set f (fn [x y] (self (+ x y) 1)))
‣ (f 6)
  × Error: arity — expected 2 args, got 1
‣ (f 6 7)
  × Error: limit — stack overflow (depth 1024)
```

**Step 3: Commit**

```bash
git add src/lang/eval.c
git commit -m "feat: add descriptive error messages for arity, limit, name errors"
```
