# Rayfall on Teide — Design Document

**Date:** 2026-03-27
**Goal:** Replace both Rayforce and Teide with a single project — Rayfall language frontend over Teide's columnar engine, taking the best of both.

## Overview

Bring the Rayfall query language into Teide as a first-class module (`src/lang/`). The result is a single library (`libteide`) that provides:
- Full Rayfall language compatibility (exact clone of syntax and semantics)
- Teide's columnar engine: morsel-driven execution, 10-pass optimizer, graph engine
- Embeddable `td_eval()` API + standalone REPL binary

## Architecture

### Pipeline

```
Rayfall source → Lexer → Parser → td_t objects → Eval / VM → Result
                                                      ↓
                                          (select/update on tables)
                                                      ↓
                                          DAG compile → Optimizer → Morsel executor
```

### Three Execution Layers (unified)

One stack-based VM with computed goto dispatch. One opcode set covering both scalar operations and vector/table operations. No separate DAG executor — the morsel loop is bytecode executed by the same VM.

```
eval() ←→ vm_eval()
  ↑            ↑
  └── call each other recursively
              ↓
         builtins (select/update) trigger DAG-mode
         bytecode emission within the same VM
```

| Situation | Flow |
|---|---|
| `(+ 1 2)` at top level | eval → dispatch `ray_add` |
| `(fn [x] (+ x 1))` called | eval → compile → vm_eval |
| Lambda calls `(select ...)` | vm_eval → OP_CALL1 → `ray_select` → DAG compile → morsel loop in VM |
| DAG column has lambda UDF | morsel loop → callback → vm_eval |
| Dynamic resolve inside VM | vm_eval → OP_CALLD → eval |

## Lexer

Hand-rolled with 128-byte ASCII dispatch table (same technique as kdb's `_PA[]`).

Single indexed read `PA(c)` classifies each byte into a token category — zero branches for character classification.

### Token types

```c
enum {
    TOK_EOF, TOK_LPAREN, TOK_RPAREN, TOK_LBRACK, TOK_RBRACK,
    TOK_LBRACE, TOK_RBRACE, TOK_COLON, TOK_QUOTE,
    TOK_I64, TOK_F64, TOK_DATE, TOK_TIME, TOK_TIMESTAMP,
    TOK_STRING, TOK_SYMBOL, TOK_NAME, TOK_TRUE, TOK_FALSE,
};
```

### Dispatch table

| ASCII range | Action | Notes |
|---|---|---|
| `0-9` | `PA_DIGIT` | Numbers, dates, timestamps |
| `a-z A-Z _` | `PA_ALPHA` | Names (builtins resolved at eval, not lex) |
| `(` `)` | `PA_LPAREN/RPAREN` | S-expression delimiters |
| `[` `]` | `PA_LBRACK/RBRACK` | Vector literals |
| `{` `}` | `PA_LBRACE/RBRACE` | Dict literals |
| `"` | `PA_STRING` | String literal |
| `'` | `PA_QUOTE` | Symbol prefix (`'AAPL`) |
| `:` | `PA_COLON` | Dict key-value separator |
| `;` | `PA_SEMI` | Comment to end of line |
| `-` | `PA_MINUS` | Negative number or name |
| space/tab | `PA_WS` | Skip |
| NUL | `PA_END` | End of input |

No infix operators in Rayfall — `+`, `*`, `/` etc. are just names inside s-expressions, handled as `PA_ALPHA`.

### Tokenizer struct

```c
typedef struct {
    const char *src;
    const char *pos;
    int         tok;
    td_t       *val;
} td_lexer_t;
```

## Parser

Recursive descent. No separate AST — the parser produces `td_t` objects directly.

### Grammar (entire language)

```
expr     = atom | list | vector | dict | quoted
atom     = number | string | name | bool
list     = '(' expr* ')'
vector   = '[' expr* ']'
dict     = '{' (name ':' expr)* '}'
quoted   = '\'' name
number   = i64 | f64 | date | time | timestamp
```

S-expressions make the parser trivially simple. Every `select`, `fn`, `if`, `let`, `set` is just a list where the first element is a name that resolves to a function object.

### Parse-time representation

| Rayfall construct | td_t node |
|---|---|
| `42` | `td_t` atom, `TD_I64` |
| `3.14` | `td_t` atom, `TD_F64` |
| `"hello"` | `td_t` atom, `TD_STR` |
| `'AAPL` | `td_t` atom, `TD_SYM` |
| `true`/`false` | `td_t` atom, `TD_BOOL` |
| `name` | `td_t` atom, `TD_SYM`, flag `TD_AST_NAME` |
| `[1 2 3]` | `td_t` vector, `TD_I64` |
| `(+ 1 2)` | `td_t` list: `[fn_obj, 1, 2]` |
| `{a: 1 b: 2}` | `td_t` dict |

Parse allocations go through an arena (`td_arena_t`) for cheap bulk free.

## Type System — Rayforce-style Function Objects

Functions are first-class `td_t` objects. Three new type tags:

| Type tag | Value | Signature | Example |
|---|---|---|---|
| `TD_UNARY` | 101 | `td_t* (*)(td_t*)` | `sum`, `count`, `neg`, `select` |
| `TD_BINARY` | 102 | `td_t* (*)(td_t*, td_t*)` | `+`, `-`, `*`, `set`, `let` |
| `TD_VARY` | 103 | `td_t* (*)(td_t**, int64_t)` | `if`, `do`, `map`, `list`, `left-join` |
| `TD_LAMBDA` | 100 | compiled body + env | user functions |

A builtin is a `td_t` atom with `type = -TD_UNARY/-TD_BINARY/-TD_VARY`, function pointer stored in a field, and `attrs` encoding flags:

```c
#define FN_NONE          0
#define FN_LEFT_ATOMIC   1   // auto-map left arg over vectors
#define FN_RIGHT_ATOMIC  2   // auto-map right arg over vectors
#define FN_ATOMIC        4   // auto-map all args over vectors
#define FN_AGGR          8   // aggregation function
#define FN_SPECIAL_FORM  16  // receives unevaluated args
```

### Builtin Registration

Single `init_functions()` using `REGISTER_FN` macro (same pattern as Rayforce `env.c`):

```c
// Unary
REGISTER_FN(functions, "sum",     TD_UNARY,  FN_ATOMIC | FN_AGGR,       ray_sum);
REGISTER_FN(functions, "count",   TD_UNARY,  FN_NONE | FN_AGGR,         ray_count);
REGISTER_FN(functions, "select",  TD_UNARY,  FN_NONE,                   ray_select);
REGISTER_FN(functions, "neg",     TD_UNARY,  FN_ATOMIC,                 ray_neg);

// Binary
REGISTER_FN(functions, "+",       TD_BINARY, FN_ATOMIC,                 ray_add);
REGISTER_FN(functions, "set",     TD_BINARY, FN_NONE | FN_SPECIAL_FORM, ray_set);
REGISTER_FN(functions, "let",     TD_BINARY, FN_NONE | FN_SPECIAL_FORM, ray_let);

// Vary
REGISTER_FN(functions, "if",      TD_VARY,   FN_NONE | FN_SPECIAL_FORM, ray_cond);
REGISTER_FN(functions, "map",     TD_VARY,   FN_NONE,                   ray_map);
REGISTER_FN(functions, "left-join", TD_VARY, FN_NONE,                   ray_left_join);
```

### Eval Dispatch

The evaluator checks the first element's type:

```c
td_t *fn = list[0];
switch (fn->type) {
    case -TD_UNARY:  return ((unary_f)fn->fn)(eval(list[1]));
    case -TD_BINARY: return ((binary_f)fn->fn)(eval(list[1]), eval(list[2]));
    case -TD_VARY:   return ((vary_f)fn->fn)(args, n);
}
```

No AST. No keyword tables. The function object itself carries everything needed.

## VM

Stack-based, computed goto dispatch. Same architecture as Rayforce. Cache-line aligned stack.

### VM struct

```c
typedef struct td_vm {
    // First cache line (64 bytes)
    int32_t sp;           // stack pointer
    int32_t fp;           // frame pointer
    int32_t rp;           // return stack pointer
    int32_t id;           // VM/thread ID
    td_t   *fn;           // current function
    // ...

    // Stacks
    td_t   *ps[VM_STACK_SIZE];  // program stack (aligned)
    ctx_t   rs[VM_STACK_SIZE];  // return stack
} __attribute__((aligned(64))) *td_vm_p;
```

### Opcodes

```c
enum {
    // Control flow
    OP_RET, OP_JMP, OP_JMPF,

    // Data
    OP_LOADCONST, OP_LOADENV, OP_STOREENV, OP_POP,

    // Symbol resolution
    OP_RESOLVE,

    // Function calls
    OP_CALL1, OP_CALL2, OP_CALLN,  // unary/binary/vary
    OP_CALLF,                       // call lambda (push frame)
    OP_CALLS,                       // self-recursive tail call
    OP_CALLD,                       // dynamic dispatch (→ eval)

    // Vector/table ops (morsel-aware, same dispatch loop)
    OP_SCAN, OP_FILTER, OP_GROUP, OP_JOIN,
    OP_SORT, OP_WINDOW, OP_HEAD, OP_TAIL, OP_SELECT,

    // Graph ops
    OP_EXPAND, OP_VAR_EXPAND, OP_SHORTEST_PATH,
    OP_PAGERANK, OP_DIJKSTRA, OP_LOUVAIN, ...
};
```

### Lambda lifecycle

```
parse → td_t list (body as nested lists)
first call → compile to bytecode (compile.c)
vm_eval → computed goto interpreter
```

### Returns, exceptions, recursion

- **Returns:** return stack of `ctx_t` frames `{fn, fp, ip}`. `OP_CALLF` pushes frame. `OP_RET` pops frame, restores state.
- **Exceptions:** trap frames on return stack (sentinel pattern, same as Rayforce). `OP_TRAP` pushes sentinel + handler IP. Error unwinds looking for trap frames.
- **Recursion:** each call pushes a frame. Tail calls (`OP_CALLS`) reuse current frame — overwrite args in place, reset ip.

## What We Keep From Each Project

### From Teide
- `td_t` 32-byte unified object header (no dead pointer fields)
- Buddy allocator + slab cache + arena allocator
- Adaptive-width symbol encoding (`TD_SYM_W8/16/32/64`)
- 10-pass query optimizer (type inference, SIP, factorize, predicate pushdown, filter reorder, fusion, DCE)
- Morsel-driven execution (1024 elements)
- Graph engine (CSR, all algorithms)
- COW refcounting
- SIMD vector operations

### From Rayforce
- Rayfall language (exact syntax and semantics)
- ASCII dispatch parser
- `TD_UNARY/TD_BINARY/TD_VARY` type system with `FN_*` attribute flags
- `REGISTER_FN` builtin registration pattern
- Stack-based VM with computed goto
- Lambda compilation (AST → bytecode)
- Per-VM heap with `heap_id` for O(1) ownership check
- Foreign block queue for cross-thread frees
- Pending-merge LIFO for worker heap teardown

### Improvements Over Both
- Single unified VM for both scalar and vector ops (no separate DAG executor)
- Teide's adaptive symbol width available to Rayfall queries
- Graph algorithms accessible from Rayfall (future, C-only for now)

## File Layout

New files inside `src/lang/`:

```
src/lang/
├── parse.c      # Lexer + parser (ASCII dispatch, recursive descent)
├── parse.h      # parse API
├── eval.c       # Tree-walking eval + builtin dispatch + VM (computed goto)
├── eval.h       # eval API, VM struct, opcodes
├── compile.c    # AST → bytecode compiler
├── env.c        # Builtin registration (init_functions, REGISTER_FN)
├── env.h        # Environment / global namespace
├── repl.c       # Interactive REPL (standalone binary)
```

Changes to existing files:
- `include/teide/td.h` — add `TD_UNARY/TD_BINARY/TD_VARY/TD_LAMBDA` type tags, `FN_*` flags, `TD_AST_NAME` flag
- `src/mem/` — add per-VM heap with heap_id, foreign block queue, pending-merge LIFO
- `CMakeLists.txt` — add `src/lang/*.c` to libteide, add `teide` REPL executable target

## Public API

```c
// Init / cleanup
int   td_init(void);           // init runtime (symbols, builtins, VM)
void  td_destroy(void);        // cleanup

// Parse + eval
td_t *td_eval(const char *source);   // parse + eval, return result
td_t *td_parse(const char *source);  // parse only, return object tree
```

No new dependencies. Pure C17, single header, zero external deps — same as today.
