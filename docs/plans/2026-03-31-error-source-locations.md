# Error Source Location Tracking — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Rich error messages with file:line:col, source snippets, carets, and stack traces — matching rayforce 1's error output.

**Architecture:** Three-phase approach mirroring rayforce 1: (1) Parser records span_t (line/col ranges) for every AST node in an nfo hash table, (2) Compiler maps bytecode offsets to spans in a dbg vector stored in each lambda, (3) VM builds a trace of location frames on error and stack unwind. The formatter renders traces with source context.

**Tech Stack:** Pure C17, rayforce2's buddy allocator, existing ray_t union layout. No new dependencies.

---

## Background

Rayforce 1's error output looks like:
```
  x Error: type

   ╭─[repl:1:5]
 1 │ (+ 1 "s")
   │     ▲
   │     ╰─ type
   ╰─ in λ
```

Rayforce 2 currently shows only: `error: type error`

### Key Data Structures (from rayforce 1)

- **`span_t`**: `union { i64_t id; struct { u16 start_line, end_line, start_col, end_col; }; }` — 8 bytes
- **nfo**: `[filename_str, source_str, hashtable(node_ptr → span)]` — maps AST nodes to source positions
- **lambda->dbg**: `I64 vector of [offset0, span0, offset1, span1, ...]` — maps bytecode IP to source span
- **vm->trace**: list of `[span_id, filename, fn_name, source]` frames — built during error propagation

### Current State (rayforce 2)

| Component | State | Gap |
|-----------|-------|-----|
| Parser (`parse.c`) | Tracks `pos` pointer only, no line/col | Need line/col + span recording |
| Lambda | 5 slots: params, body, bc, consts, n_locals | Need nfo (slot 5) + dbg (slot 6) |
| Compiler (`compile.c`) | No debug info | Need DBG macro to record offset→span pairs |
| VM (`eval.c`) | No trace field in `ray_vm_t` | Need trace field + `bc_error_add_loc` |
| Formatter (`format.c`) | `error: %s` only | Need rich frame formatter with source snippets |

---

### Task 1: Add `span_t` type and nfo data structure

**Files:**
- Create: `src/lang/nfo.h`
- Create: `src/lang/nfo.c`

**Step 1: Create nfo.h with span_t and API declarations**

```c
// src/lang/nfo.h
#ifndef RAY_NFO_H
#define RAY_NFO_H

#include <rayforce.h>

/* Source span: line/column range packed into 8 bytes.
 * id==0 means "no span". */
typedef union {
    int64_t id;
    struct {
        uint16_t start_line;
        uint16_t end_line;
        uint16_t start_col;
        uint16_t end_col;
    };
} ray_span_t;

/* Create nfo object: [filename, source, key_vec, val_vec]
 * key_vec = I64 vector of AST node pointers
 * val_vec = I64 vector of span.id values
 * Uses parallel arrays instead of hash table for simplicity. */
ray_t* ray_nfo_create(const char* filename, size_t fname_len,
                      const char* source, size_t src_len);

/* Record a span for an AST node. */
void ray_nfo_insert(ray_t* nfo, ray_t* node, ray_span_t span);

/* Look up the span for an AST node. Returns span with id==0 if not found. */
ray_span_t ray_nfo_get(ray_t* nfo, ray_t* node);

/* Accessors for nfo fields */
#define NFO_FILENAME(nfo)  (((ray_t**)ray_data(nfo))[0])
#define NFO_SOURCE(nfo)    (((ray_t**)ray_data(nfo))[1])
#define NFO_KEYS(nfo)      (((ray_t**)ray_data(nfo))[2])
#define NFO_VALS(nfo)      (((ray_t**)ray_data(nfo))[3])

#endif
```

**Step 2: Implement nfo.c**

The nfo object is a RAY_LIST with 4 elements: [filename_str, source_str, keys_i64_vec, vals_i64_vec]. Insert appends to both vectors. Lookup is linear scan (fine for typical function sizes).

```c
// src/lang/nfo.c — key functions:
// ray_nfo_create: alloc LIST[4], create filename/source strings, empty I64 vecs
// ray_nfo_insert: append (intptr_t)node to keys, span.id to vals
// ray_nfo_get: linear scan keys for match, return corresponding val as span
```

**Step 3: Build and verify compilation**

Run: `make`
Expected: Clean build, zero warnings

**Step 4: Commit**

```
feat(nfo): add span_t type and nfo source-location store
```

---

### Task 2: Add line/column tracking to the parser

**Files:**
- Modify: `src/lang/parse.c` (parser struct, skip_ws, all parse_* functions)
- Modify: `src/lang/parse.h` (add nfo to ray_parse signature)

**Step 1: Extend parser state with line, column, nfo**

In `parse.c`, change:
```c
typedef struct {
    const char *src;    // original source start
    const char *pos;    // current position
    int32_t line;       // current line (0-based)
    int32_t col;        // current column (0-based)
    ray_t *nfo;         // nfo object (NULL if no debug info)
} ray_parser_t;
```

**Step 2: Track line/col in `skip_ws_and_comments` and character consumption**

Add helper `advance(parser, n)` that updates `pos`, `line`, `col` based on consumed characters (increment line on `\n`, reset col).

**Step 3: Record spans in each `parse_*` function**

Pattern for each parse function:
```c
static ray_t* parse_number(ray_parser_t* p) {
    ray_span_t span;
    span.start_line = p->line;
    span.start_col  = p->col;
    // ... existing parse logic ...
    span.end_line = p->line;
    span.end_col  = p->col;
    if (p->nfo) ray_nfo_insert(p->nfo, result, span);
    return result;
}
```

Apply to: `parse_number`, `parse_string`, `parse_symbol`, `parse_name`, `parse_list`, `parse_vector`, `parse_dict`, and `parse_timestamp`/`parse_time` if they exist.

**Step 4: Update `ray_parse` signature to accept optional nfo**

Add `ray_parse_with_nfo(source, nfo)` that creates the parser with nfo attached. Keep `ray_parse(source)` as a convenience wrapper that passes `NULL` nfo.

**Step 5: Build and run tests**

Run: `make test`
Expected: 561 tests pass (no behavior change — nfo is NULL by default)

**Step 6: Commit**

```
feat(parse): track line/column and record spans in nfo
```

---

### Task 3: Extend lambda to store nfo and dbg

**Files:**
- Modify: `src/lang/eval.h` (add LAMBDA_NFO, LAMBDA_DBG macros)
- Modify: `src/lang/eval.c` (lambda creation — allocate 7 slots instead of 5)

**Step 1: Add lambda slot macros**

In `eval.h`, after existing LAMBDA_ macros:
```c
#define LAMBDA_NFO(lam)       (((ray_t**)ray_data(lam))[5])
#define LAMBDA_DBG(lam)       (((ray_t**)ray_data(lam))[6])
```

**Step 2: Update lambda allocation to 7 slots**

Find all `ray_alloc(5 * sizeof(ray_t*))` for lambda creation in eval.c and change to `7 * sizeof(ray_t*)`. Initialize slots 5 and 6 to NULL.

Search for: the `fn` special form handler that creates lambdas. There should be a pattern like:
```c
ray_t* lam = ray_alloc(N * sizeof(ray_t*));
lam->type = RAY_LAMBDA;
```

Change N from 5 to 7, add `LAMBDA_NFO(lam) = NULL; LAMBDA_DBG(lam) = NULL;`

**Step 3: Update ray_release_owned_refs / ray_retain_owned_refs for lambda cleanup**

Find where lambda children are released (likely in heap.c `ray_release_owned_refs`) and add release for slots 5 and 6.

**Step 4: Build and run tests**

Run: `make test`
Expected: 561 tests pass

**Step 5: Commit**

```
feat(lambda): add nfo and dbg slots for debug info
```

---

### Task 4: Wire nfo through eval → parse → lambda

**Files:**
- Modify: `src/lang/eval.c` (fn special form, ray_eval_str)
- Modify: `src/app/repl.c` (eval_and_print)

**Step 1: Create nfo in `ray_eval_str` and pass through parse**

In `ray_eval_str(source)`:
```c
ray_t* nfo = ray_nfo_create("repl", 4, source, strlen(source));
ray_t* parsed = ray_parse_with_nfo(source, nfo);
if (RAY_IS_ERR(parsed)) { ray_release(nfo); return parsed; }
// Store nfo somewhere accessible during eval...
ray_t* result = ray_eval(parsed);
ray_release(parsed);
ray_release(nfo);
return result;
```

**Step 2: In the `fn` special form handler, attach nfo to lambda**

When `(fn [params] body)` is evaluated, if the current eval context has an nfo, set `LAMBDA_NFO(lam) = nfo; ray_retain(nfo);`

This requires threading the nfo through the eval context. Add a thread-local `static _Thread_local ray_t* g_eval_nfo = NULL;` that `ray_eval_str` sets before eval and clears after.

**Step 3: In `ray_load_file`, create nfo with actual filename**

```c
ray_t* nfo = ray_nfo_create(path, strlen(path), buf, sz);
```

**Step 4: Build and run tests**

Run: `make test`
Expected: 561 tests pass

**Step 5: Commit**

```
feat(eval): wire nfo through eval→parse→lambda
```

---

### Task 5: Compiler emits debug info (bytecode offset → span)

**Files:**
- Modify: `src/lang/compile.c` (compiler_t, compile_expr, compile_list, ray_compile)

**Step 1: Add dbg buffer to compiler_t**

```c
typedef struct {
    // ... existing fields ...
    ray_t   *dbg_obj;    /* I64 vector: pairs of [offset, span.id] */
    int64_t *dbg;
    int32_t  dbg_len;
    int32_t  dbg_cap;
} compiler_t;
```

**Step 2: Add DBG macro**

```c
#define EMIT_DBG(c, ast) do { \
    if ((c)->lambda && LAMBDA_NFO((c)->lambda)) { \
        ray_span_t _sp = ray_nfo_get(LAMBDA_NFO((c)->lambda), (ast)); \
        if (_sp.id != 0) { \
            dbg_append(c, (c)->code_len, _sp.id); \
        } \
    } \
} while(0)
```

**Step 3: Call EMIT_DBG at the start of each compile_expr call**

In `compile_expr()`, before the body:
```c
EMIT_DBG(c, ast);
```

And at the start of `compile_list()`:
```c
EMIT_DBG(c, ast);
```

**Step 4: Store dbg in lambda after compilation**

In `ray_compile()`, after successful compilation:
```c
LAMBDA_DBG(lambda) = c.dbg_obj;
ray_retain(c.dbg_obj);
```

**Step 5: Add `bc_dbg_get()` — lookup span by IP**

```c
// Returns span for the largest offset <= ip
ray_span_t bc_dbg_get(ray_t* dbg, int32_t ip);
```

Linear scan of the pairs vector, finding the best match.

**Step 6: Build and run tests**

Run: `make test`
Expected: 561 tests pass

**Step 7: Commit**

```
feat(compile): emit debug info mapping bytecode offsets to source spans
```

---

### Task 6: VM builds error trace on unwind

**Files:**
- Modify: `src/lang/eval.h` (add trace field to ray_vm_t)
- Modify: `src/lang/eval.c` (vm_exec error path, vm_error_unwind)

**Step 1: Add trace to ray_vm_t**

```c
typedef struct {
    // ... existing fields ...
    ray_t   *trace;    /* error trace: list of [span_id, filename, fn_name, source] frames */
} ray_vm_t;
```

Initialize to NULL in vm_exec.

**Step 2: Add `bc_error_add_loc()` function**

```c
static void bc_error_add_loc(ray_vm_t* vm, ray_t* fn, int32_t ip) {
    if (!fn || fn->type != RAY_LAMBDA) return;
    ray_t* dbg = LAMBDA_DBG(fn);
    if (!dbg) return;
    ray_span_t span = bc_dbg_get(dbg, ip);
    if (span.id == 0) return;
    ray_t* nfo = LAMBDA_NFO(fn);
    // Build frame: [span.id, filename, fn_name, source]
    // Append to vm->trace
}
```

**Step 3: Call bc_error_add_loc in vm_error path**

At `vm_error:` label, before cleanup:
```c
bc_error_add_loc(vmp, vm.fn, ip - 1);
```

**Step 4: Add stack unwinding with trace**

In the OP_RET error propagation and vm_error cleanup, walk the return stack:
```c
for (int32_t i = vm.rp - 1; i >= 0; i--) {
    if (vm.rs[i].fn)
        bc_error_add_loc(vmp, vm.rs[i].fn, vm.rs[i].ip - 1);
}
```

**Step 5: Make trace accessible for formatting**

Store the trace in a thread-local so the error formatter can access it:
```c
static _Thread_local ray_t* g_vm_error_trace = NULL;
```

Set it at vm_error before returning, clear after formatting.

**Step 6: Build and run tests**

Run: `make test`
Expected: 561 tests pass

**Step 7: Commit**

```
feat(vm): build error trace with source locations on unwind
```

---

### Task 7: Rich error formatter with source snippets

**Files:**
- Modify: `src/app/format.c` (add error frame formatter)
- Modify: `src/app/repl.c` (use rich formatter for errors)

**Step 1: Add `ray_fmt_error_trace()` function**

Produces output matching rayforce 1:
```
  x Error: type

   ╭─[repl:1:5]
 1 │ (+ 1 "s")
   │     ▲
   │     ╰─ type
   ╰─ in λ
```

Key logic:
- For each frame in trace: extract span, filename, source
- Find the source line(s) using span.start_line
- Print gutter with line numbers
- Highlight the span with colored text
- Show caret/underline at error position
- Show function name

**Step 2: Wire into repl_print_result**

```c
if (RAY_IS_ERR(val)) {
    ray_t* trace = ray_get_error_trace();  // thread-local
    if (trace) {
        ray_fmt_error_with_trace(fp, val, trace, use_color);
        ray_clear_error_trace();
    } else {
        // fallback to current simple format
    }
}
```

**Step 3: Build and test manually**

Run: `make release && printf '(+ 1 "s")\n' | ./rayforce`
Expected: Rich error output with source location

**Step 4: Commit**

```
feat(format): rich error output with source snippets and stack traces
```

---

### Task 8: Wire nfo through `load` builtin

**Files:**
- Modify: `src/lang/eval.c` (ray_load_file)

**Step 1: Create nfo with actual filename in load**

```c
ray_t* nfo = ray_nfo_create(path, path_len, buf, sz);
// set g_eval_nfo = nfo before eval
```

This ensures errors in loaded files show the correct filename and source.

**Step 2: Test with a file that has an error**

Create `/tmp/test_err.rfl`:
```
(set x 1)
(+ x "bad")
```

Run: `printf '(load "/tmp/test_err.rfl")\n' | ./rayforce`
Expected: Error shows `test_err.rfl:2:4` with source context

**Step 3: Commit**

```
feat(load): source locations include actual filenames
```

---

### Task 9: Tests for error locations

**Files:**
- Modify: `test/test_lang.c` (add error location tests)

**Step 1: Add test for parse error location**

```c
static MunitResult test_error_location_parse(...) {
    // Verify that a parse error on "(+ 1" mentions position
    // Use the trace API to check span fields
}
```

**Step 2: Add test for runtime error location**

```c
static MunitResult test_error_location_runtime(...) {
    // Verify that (+ 1 "s") produces a trace with line 1, col info
}
```

**Step 3: Add test for stack trace in nested calls**

```c
static MunitResult test_error_stack_trace(...) {
    // (set f (fn [x] (+ x "s")))
    // (f 1)
    // Verify trace has 2 frames
}
```

**Step 4: Run tests**

Run: `make test`
Expected: All tests pass including new ones

**Step 5: Commit**

```
test: add error location and stack trace tests
```

---

### Task 10: Final integration test and push

**Step 1: Full test suite**

Run: `make clean && make test`
Expected: All tests pass, zero warnings

**Step 2: Release build and manual test**

Run: `make clean && make release`
Test interactively in REPL with `:t`, error expressions, `load`.

**Step 3: Commit and push**

```
chore: error source locations — integration verified
```

---

## Dependency Order

```
Task 1 (nfo type)
  → Task 2 (parser spans)
  → Task 3 (lambda slots)
    → Task 4 (wire nfo)
      → Task 5 (compiler dbg)
        → Task 6 (VM trace)
          → Task 7 (formatter)
            → Task 8 (load)
            → Task 9 (tests)
              → Task 10 (ship)
```

Tasks are strictly sequential — each builds on the previous.
