# CLAUDE.md

## What is Rayforce?

Pure C17 zero-dependency columnar dataframe library with native graph engine. Lazy fusion API → operation DAG → optimizer → fused morsel-driven execution. CSR edge indices, graph traversal opcodes, worst-case optimal joins, and sideways information passing — all in the same pipeline.

## Build & Test

```bash
# Debug (ASan + UBSan)
cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build

# Release
cmake -B build_release -DCMAKE_BUILD_TYPE=Release && cmake --build build_release

# Run all tests
cd build && ctest --output-on-failure

# Run a single test suite
./build/test_rayforce --suite /vec

# Run the Rayfall REPL (interactive or file mode)
./build/rayforce
./build/rayforce script.rfl
```

## Architecture

Core abstraction is `ray_t` — a 32-byte block header. Every object (atom, vector, list, table) is a `ray_t` with data following at byte 32.

**Memory**: buddy allocator with thread-local arenas, slab cache for small allocations, COW ref counting. Arena (bump) allocator (`ray_arena_t`) for bulk short-lived allocations — blocks carry `RAY_ATTR_ARENA` flag, making retain/release no-ops; entire arena freed at once.

**Execution pipeline**:
1. Build lazy DAG: `ray_graph_new(df)` → `ray_scan/ray_add/ray_filter/...` → `ray_execute(g, root)`
2. Optimizer: type inference → constant fold → SIP → factorize → predicate pushdown → filter reorder → fusion → DCE
3. Fused executor: bytecode over register slots, morsel-by-morsel (1024 elements)

**Strings**: two representations — `RAY_SYM` (dictionary-encoded symbol columns, integer indices into global intern table) and `RAY_STR` (variable-length 16-byte `ray_str_t` elements: strings <= 12 bytes stored inline, longer strings in a per-vector pool with 4-byte prefix for fast comparison rejection). All string opcodes (comparisons, STRLEN, UPPER/LOWER/TRIM, SUBSTR, REPLACE, CONCAT, IF) support both types. String transformation opcodes (STRLEN, UPPER/LOWER/TRIM, SUBSTR, REPLACE, CONCAT) propagate nulls: null input rows produce null output rows (CONCAT is null if any argument is null). Access via `ray_str_vec_get()`; executor uses `str_resolve()` to get element array + pool pointer. Hash via `ray_str_t_hash()`, compare via `ray_str_t_cmp()`/`ray_str_t_eq()`. During execution, `col_propagate_str_pool()` shares the source pool with the destination vector; both src and dst must be RAY_STR.

**Graph engine**: CSR edge indices (`ray_csr_t`, `ray_rel_t`) alongside columnar tables.
- Storage: double-indexed CSR (forward + reverse), persisted as `.col` files, supports mmap
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

## Key File Paths

```
include/rayforce.h         Single public header (all types, opcodes, API)
src/store/csr.{h,c}        CSR storage — build, save, load, mmap, free
src/ops/graph.c             DAG construction (ray_expand, ray_var_expand, etc.)
src/ops/exec.c              Fused morsel-driven executor (all opcodes)
src/ops/opt.c               Optimizer passes (type inference, SIP, factorize, predicate pushdown, filter reorder, fusion, DCE)
src/ops/lftj.{h,c}         Leapfrog Triejoin — iterator, search, enumeration
src/ops/fvec.{h,c}         Factorized vectors — ray_fvec_t, ray_ftable_t
src/store/fileio.{h,c}     Cross-platform file I/O — locking (flock/LockFileEx), fsync, atomic rename
src/table/sym.{h,c}        Global sym intern table — arena-backed string atoms, save/load, append-only persistence, file locking
src/mem/arena.{h,c}        Arena (bump) allocator — ray_arena_t, bulk alloc for sym table
test/test_arena.c           Arena allocator tests (alloc, reset, destroy, sym integration)
test/test_csr.c             Graph engine tests (56 tests)
test/test_opt.c             Optimizer pass tests (filter reorder, predicate pushdown)
test/test_store.c           Storage tests (file I/O, sym persistence, col bounds validation)
test/test_sym.c             Sym table tests (save/load roundtrip, append-only, corruption)
test/test_str.c             RAY_STR string vector tests (slice, concat, hash, comparisons)
test/test_exec.c            Executor tests (string ops, comparisons, conditionals, joins)
src/vec/vec.c               Vector operations — append, set, concat, slice, RAY_STR string vectors with pool
src/io/csv.{h,c}           CSV loader — mmap, parallel parse, null handling, sym merge
src/lang/parse.{h,c}       Rayfall lexer (ASCII dispatch table) and recursive descent parser
src/lang/eval.{h,c}        Tree-walking evaluator, bytecode VM (computed goto), all builtins
src/lang/compile.c          Bytecode compiler (AST → opcodes for lambda functions)
src/lang/env.{h,c}         Global environment and local scope stack for variable binding; prefix lookup for completion/highlighting
src/lang/repl.c             Rayfall REPL binary — main() entry point, delegates to app/repl module
src/app/term.{h,c}         Terminal layer — raw mode, line editing, history, syntax highlighting, bracket matching, autocomplete, multi-line input
src/app/repl.{h,c}         REPL module — eval loop, pretty-print (tables/vectors/errors), REPL commands (:help, :timeit, :quit, :env, :clear), signal handling
test/test_lang.c            Rayfall language tests (lexer, parser, eval, VM, tables, joins)
bench/bench_csv*.c          CSV loading benchmarks (build with -DRAYFORCE_BENCH=ON)
```
