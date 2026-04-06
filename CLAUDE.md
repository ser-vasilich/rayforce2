# CLAUDE.md

## What is Rayforce?

Pure C17 zero-dependency columnar dataframe library with native graph engine. Lazy fusion API → operation DAG → optimizer → fused morsel-driven execution. CSR edge indices, graph traversal opcodes, worst-case optimal joins, and sideways information passing — all in the same pipeline.

## Build & Test

```bash
# Debug (ASan + UBSan) — default
make

# Release
make release

# Run all tests
make test

# Run a single test suite
./rayforce.test --suite /vec

# Run the Rayfall REPL (interactive or file mode)
./rayforce
./rayforce script.rfl
```

## Architecture

Core abstraction is `ray_t` — a 32-byte block header. Every object (atom, vector, list, table) is a `ray_t` with data following at byte 32.

**Memory**: buddy allocator with thread-local arenas, slab cache for small allocations, COW ref counting. Arena (bump) allocator (`ray_arena_t`) for bulk short-lived allocations — blocks carry `RAY_ATTR_ARENA` flag, making retain/release no-ops; entire arena freed at once.

**Null**: `RAY_NULL_OBJ` — static singleton (`type == RAY_NULL`, `RAY_ATTR_ARENA`), always a valid pointer. Returned by side-effect builtins (println, show). `RAY_IS_NULL(p)` tests for it. `is_null_atom(x)` recognizes all null forms: `RAY_NULL_OBJ`, sentinel nulls (`0Nl`/`0Ni`/`0Nd`/`0Nt`/`0Np`/`0Nf`). All nulls are falsy in `if` and equal via `==`. Sentinel nulls propagate through arithmetic; `RAY_NULL_OBJ` produces type errors.

**Execution pipeline**:
1. Build lazy DAG: `ray_graph_new(df)` → `ray_scan/ray_add/ray_filter/...` → `ray_execute(g, root)`
2. Optimizer: type inference → constant fold → SIP → factorize → predicate pushdown → filter reorder → fusion → DCE
3. Fused executor: bytecode over register slots, morsel-by-morsel (1024 elements)

**Strings**: two representations — `RAY_SYM` (dictionary-encoded symbol columns, integer indices into global intern table) and `RAY_STR` (variable-length 16-byte `ray_str_t` elements: strings <= 12 bytes stored inline, longer strings in a per-vector pool with 4-byte prefix for fast comparison rejection). All string opcodes (comparisons, STRLEN, UPPER/LOWER/TRIM, SUBSTR, REPLACE, CONCAT, IF) support both types. String transformation opcodes (STRLEN, UPPER/LOWER/TRIM, SUBSTR, REPLACE, CONCAT) propagate nulls: null input rows produce null output rows (CONCAT is null if any argument is null). Access via `ray_str_vec_get()`; executor uses `str_resolve()` to get element array + pool pointer. Hash via `ray_str_t_hash()`, compare via `ray_str_t_cmp()`/`ray_str_t_eq()`. During execution, `col_propagate_str_pool()` shares the source pool with the destination vector; both src and dst must be RAY_STR.

**Graph engine**: CSR edge indices (`ray_csr_t`, `ray_rel_t`) alongside columnar tables.
- Storage: double-indexed CSR (forward + reverse), persisted as column files, supports mmap
- Opcodes: `OP_EXPAND` (1-hop), `OP_VAR_EXPAND` (BFS), `OP_SHORTEST_PATH`, `OP_ASTAR` (A*), `OP_K_SHORTEST` (Yen's), `OP_CLUSTER_COEFF`, `OP_RANDOM_WALK`, `OP_WCO_JOIN` (LFTJ), `OP_BETWEENNESS` (Brandes), `OP_CLOSENESS` (closeness centrality), `OP_MST` (Kruskal)
- Factorized execution: `ray_fvec_t` / `ray_ftable_t` avoid materializing cross-products
- Optimizer: SIP pass propagates `RAY_SEL` bitmaps backward through `OP_EXPAND` chains

**Rayfall language**: Lisp-like query frontend. Parser produces `ray_t` objects directly (no separate AST). Tree-walking `ray_eval()` dispatches by function type (`RAY_UNARY`, `RAY_BINARY`, `RAY_VARY`). Lambdas compile lazily to bytecode and run in a stack-based computed-goto VM (`ray_vm_t`). `select`/`update` builtins bridge to Rayforce's DAG executor at runtime.
- Types: `RAY_LAMBDA` (user-defined), `RAY_UNARY`/`RAY_BINARY`/`RAY_VARY` (builtins)
- Function flags: `FN_ATOMIC` (auto-map over vectors), `FN_AGGR` (aggregation), `FN_SPECIAL_FORM` (unevaluated args)
- Entry points: `ray_lang_init()` / `ray_eval_str("(+ 1 2)")` / `ray_eval(parsed_obj)`
- VM: 1024-slot program stack + return stack, trap frames for `try`/`raise` error handling

**Per-VM heaps**: each heap carries a `heap_id` (u16 in `ray_t`), allocated via atomic bitmap. Cross-heap frees enqueue blocks to a lock-free LIFO (`ray_heap_flush_foreign()` reclaims them). Worker heaps merge back via `ray_heap_push_pending()` / `ray_heap_drain_pending()`.

## Code Conventions

- **Prefix**: all public symbols `ray_`, internal functions `static`
- **Constants**: `RAY_UPPER_SNAKE_CASE`
- **Types**: `ray_name_t` (typedef'd structs)
- **Morsel-only processing**: all vector loops chunk through `ray_morsel_t` (1024 elements)
- **Error returns**: `ray_t*` functions use `RAY_ERR_PTR()` / `RAY_IS_ERR()`; other functions return `ray_err_t`
- **COW cleanup**: after `ray_cow()` returns a new copy, all error paths must release it (`if (vec != original) ray_release(vec)`). Use `goto fail` pattern.
- **No external deps**: pure C17, single public header `include/rayforce.h`
- **No system allocator**: never use `malloc`/`calloc`/`realloc`/`free`. Use `ray_alloc()`/`ray_free()` for general allocation, `ray_arena_alloc()` for bulk short-lived blocks. `ray_sys_alloc`/`ray_sys_free` reserved for allocator internals only.
- **SIMD first**: performance work must prefer SIMD approaches. Profile before optimizing, benchmark after.
- **One-word file names**: no compound names like `exec_internal.h` or `sort_exec.c`. Use `internal.h`, `sort.c`.
- **Layer separation**: `lang/` = front-end only (parse/compile/eval/env/format). `ops/` = all execution + builtins. `core/` = runtime infrastructure (pool/profile/morsel). Never put builtins in `lang/`.

## Key File Paths

```
include/rayforce.h         Single public header (all types, opcodes, API, RAY_NULL_OBJ)

── app/ ── Application layer
src/app/main.c              Entry point — REPL or file mode
src/app/term.{h,c}         Terminal — raw mode, line editing, history, syntax highlighting, autocomplete
src/app/repl.{h,c}         REPL — eval loop, pretty-print, commands (:help, :t, :env, :clear, :q)

── core/ ── Runtime infrastructure
src/core/platform.{h,c}    OS detection, atomics, TLS, compiler intrinsics
src/core/runtime.{h,c}     Global state, lifecycle (ray_init/ray_destroy), RAY_NULL_OBJ singleton
src/core/types.{h,c}       Type metadata — elem size, type names
src/core/block.{h,c}       Block-level operations
src/core/pool.{h,c}        Thread pool — work-stealing, parallel dispatch
src/core/profile.h          Span-based profiler — zero overhead when inactive
src/core/morsel.{h,c}      Morsel iterator — 1024-element chunking

── mem/ ── Memory subsystem
src/mem/heap.{h,c}         Buddy allocator, per-VM heaps, cross-heap free deferral
src/mem/sys.{h,c}          System allocator (mmap/VirtualAlloc) — internal only
src/mem/cow.{h,c}          COW ref counting — ray_retain/ray_release
src/mem/arena.{h,c}        Arena (bump) allocator — bulk short-lived blocks

── lang/ ── Language front-end (parse → compile → eval)
src/lang/parse.{h,c}       Rayfall lexer (ASCII dispatch table) and recursive descent parser
src/lang/compile.c          Bytecode compiler (AST → opcodes for lambda functions, try/catch)
src/lang/eval.{h,c}        Tree-walking evaluator, bytecode VM (computed goto), builtin registration
src/lang/eval_internal.h    Shared helpers for builtins (make_i64, is_null_atom, collection_elem)
src/lang/env.{h,c}         Global environment and local scope stack for variable binding
src/lang/format.{h,c}      Value formatter — atoms, vectors, tables, errors
src/lang/nfo.{h,c}         Source location tracking for error messages

── ops/ ── Execution engine + builtins
src/ops/ops.h               Opcode definitions, lazy handle types, constants
src/ops/internal.h          Executor internals — read_col_i64, write_col_i64, shared macros
src/ops/graph.{h,c}        DAG construction (ray_expand, ray_var_expand, ray_scan, etc.)
src/ops/opt.{h,c}          Optimizer passes (type inference, SIP, factorize, pushdown, fusion, DCE)
src/ops/exec.{h,c}         Fused morsel-driven executor dispatch
src/ops/plan.{h,c}         Query plan construction
src/ops/fuse.{h,c}         Operator fusion pass
src/ops/pipe.{h,c}         Pipeline execution
src/ops/group.c             GROUP BY — hash aggregation, radix partitioning, parallel merge
src/ops/join.c              Hash join — inner, left, anti, window, asof
src/ops/filter.c            Filter execution — selection bitmaps
src/ops/sort.c              Sort executor + sort builtins (asc/desc/iasc/idesc/rank)
src/ops/window.c            Window functions
src/ops/pivot.c             Pivot/unpivot execution
src/ops/expr.c              Expression evaluation within DAG
src/ops/traverse.c          Graph traversal — BFS, shortest path, A*, centrality, MST
src/ops/string.c            String opcode execution (UPPER/LOWER/TRIM/SUBSTR/REPLACE/CONCAT)
src/ops/temporal.c          Temporal opcode execution + date/time/timestamp builtins
src/ops/embedding.c         Embedding/vector similarity execution
src/ops/query.c             Query bridge — select/update/insert/upsert/join builtins
src/ops/builtins.c          I/O builtins (println/show/format/read-csv/write-csv), cast, misc
src/ops/agg.c               Aggregation builtins (sum/count/avg/min/max/first/last/med/dev)
src/ops/arith.c             Arithmetic builtins (+, -, *, /, %, neg, round, floor, ceil)
src/ops/cmp.c               Comparison builtins (>, <, >=, <=, ==, !=)
src/ops/collection.c        Collection builtins (distinct, take, til, reverse, find, etc.)
src/ops/strop.c             String builtins (upper/lower/trim/substr/replace/concat/like)
src/ops/tblop.c             Table builtins (meta, cols, keys, xcols, xkey, rename, flip)
src/ops/system.c            System builtins (gc, system, getenv, read/write files, serde)
src/ops/datalog.{h,c}      Datalog engine + EAV builtins (rule, query, dl-eval)
src/ops/lftj.{h,c}         Leapfrog Triejoin — iterator, search, enumeration
src/ops/fvec.{h,c}         Factorized vectors — ray_fvec_t, ray_ftable_t
src/ops/hash.h              Hash functions
src/ops/dump.c              DAG dump/debug printing

── vec/ ── Vector/atom primitives
src/vec/vec.{h,c}          Vector operations — append, set, concat, slice, RAY_STR string pool
src/vec/atom.{h,c}         Atom constructors (ray_i64, ray_f64, ray_str, etc.)
src/vec/list.{h,c}         List operations
src/vec/str.{h,c}          RAY_STR string vector internals
src/vec/sel.c               Selection bitmap operations

── table/ ── Table + sym
src/table/table.{h,c}      Table construction, column access, row count
src/table/sym.{h,c}        Global sym intern table — arena-backed, append-only persistence

── store/ ── Persistence
src/store/col.{h,c}        Column file I/O — save/load with null bitmaps
src/store/csr.{h,c}        CSR storage — build, save, load, mmap, free
src/store/serde.{h,c}      Serialization/deserialization of ray_t objects
src/store/splay.{h,c}      Splayed table storage
src/store/fileio.{h,c}     Cross-platform file I/O — locking, fsync, atomic rename
src/store/hnsw.{h,c}       HNSW index for vector similarity search
src/store/meta.{h,c}       Table metadata persistence
src/store/part.{h,c}       Table partitioning

── io/ ── Data loading
src/io/csv.{h,c}           CSV loader — mmap, parallel parse, null handling, sym merge

── test/
test/test_lang.c            Rayfall language tests (lexer, parser, eval, VM, tables, joins)
test/test_exec.c            Executor tests (string ops, comparisons, conditionals, joins)
test/test_csr.c             Graph engine tests (56 tests)
test/test_opt.c             Optimizer pass tests
test/test_store.c           Storage tests (file I/O, sym persistence, col bounds)
test/test_sym.c             Sym table tests
test/test_str.c             RAY_STR string vector tests
test/test_arena.c           Arena allocator tests
```
