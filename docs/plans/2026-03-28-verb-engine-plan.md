# Unified Verb Engine — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Unify standalone Rayfall verb evaluation and the DAG executor — all vector operations go through one execution path with full type coverage, null handling, and parallel morsel dispatch.

**Architecture:** Phase 1 fixes the reduction kernel in exec.c (type coverage + nulls). Phase 2 adds lazy handle infrastructure. Phase 3 rewires eval.c builtins as thin DAG wrappers. Phase 4 adds materialization triggers. Each phase is independently valuable and testable.

**Tech Stack:** C17, no external deps. Uses ray_pool_dispatch for parallelism.

**Design doc:** `docs/plans/2026-03-28-verb-engine-design.md`

**Build & test commands:**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build
cd build && ctest --output-on-failure
```

**Reference files:**
- `src/ops/exec.c` — reduction kernel (lines 1755-2010), exec_node, ray_execute
- `src/lang/eval.c` — current builtins (ray_sum_fn ~line 295, etc.), ray_eval
- `include/rayforce.h` — ray_t struct (lines 280-316), attrs (lines 222-225), opcodes
- `src/ops/graph.c` — DAG construction (ray_graph_new, make_unary)
- `src/vec/vec.c` — ray_vec_is_null (lines 835-862)

---

## Phase 1: Fix Reduction Kernel

### Task 1.1: Add null_count to reduce_acc_t and null-aware reduce_range

**Files:** Modify `src/ops/exec.c:1759-1800`

- [ ] Add `int64_t null_count;` field to `reduce_acc_t` struct (line ~1763)
- [ ] Add `acc->null_count = 0;` to `reduce_acc_init` (line ~1771)
- [ ] Modify `reduce_range` to accept null bitmap parameters:
  ```c
  static void reduce_range(ray_t* input, int64_t start, int64_t end,
                           reduce_acc_t* acc, bool has_nulls,
                           const uint8_t* null_bm) {
  ```
- [ ] In the inner loop, add null skip logic:
  ```c
  for (int64_t row = start; row < end; row++) {
      if (has_nulls && (null_bm[row / 8] >> (row % 8)) & 1) {
          acc->null_count++;
          continue;
      }
      // existing accumulation...
  }
  ```
- [ ] Update `reduce_merge` to merge `null_count`: `dst->null_count += src->null_count;`
- [ ] Update all call sites of `reduce_range` (in `par_reduce_fn` and `exec_reduction`) to resolve null bitmap from input vector and pass it:
  ```c
  bool has_nulls = (input->attrs & RAY_ATTR_HAS_NULLS) != 0;
  const uint8_t* null_bm = NULL;
  if (has_nulls) {
      if (input->attrs & RAY_ATTR_NULLMAP_EXT)
          null_bm = (const uint8_t*)ray_data(input->ext_nullmap);
      else
          null_bm = input->nullmap;
  }
  ```
- [ ] Update `exec_reduction` result extraction: `OP_COUNT` returns `acc.cnt` (cnt already only counts non-null since we skip nulls), `OP_AVG` divides by `acc.cnt`
- [ ] Build: `cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build`
- [ ] Test: `cd build && ctest --output-on-failure` — all existing tests pass (no null vectors in current reduction tests means null path is no-op)
- [ ] Commit: `feat(exec): null-aware reduction kernel`

---

### Task 1.2: Full type dispatch in reduce_range

**Files:** Modify `src/ops/exec.c:1774-1800`

Currently `reduce_range` handles only `RAY_F64` and "cast everything to i64 via `read_col_i64`". Replace with macro-driven per-type loops that operate on native element widths.

- [ ] Define reduction loop macros above `reduce_range`:
  ```c
  #define REDUCE_LOOP_I(T, data, start, end, acc, has_nulls, null_bm) \
      do { \
          const T* d = (const T*)(data) + start; \
          for (int64_t i = 0, row = start; row < end; i++, row++) { \
              if (has_nulls && (null_bm[row/8] >> (row%8)) & 1) { acc->null_count++; continue; } \
              int64_t v = (int64_t)d[i]; \
              acc->sum_i += v; acc->sum_sq_i += v * v; acc->prod_i *= v; \
              if (v < acc->min_i) acc->min_i = v; \
              if (v > acc->max_i) acc->max_i = v; \
              if (!acc->has_first) { acc->first_i = v; acc->has_first = true; } \
              acc->last_i = v; acc->cnt++; \
          } \
      } while (0)

  #define REDUCE_LOOP_F(data, start, end, acc, has_nulls, null_bm) \
      do { \
          const double* d = (const double*)(data) + start; \
          for (int64_t i = 0, row = start; row < end; i++, row++) { \
              if (has_nulls && (null_bm[row/8] >> (row%8)) & 1) { acc->null_count++; continue; } \
              double v = d[i]; \
              acc->sum_f += v; acc->sum_sq_f += v * v; acc->prod_f *= v; \
              if (v < acc->min_f) acc->min_f = v; \
              if (v > acc->max_f) acc->max_f = v; \
              if (!acc->has_first) { acc->first_f = v; acc->has_first = true; } \
              acc->last_f = v; acc->cnt++; \
          } \
      } while (0)
  ```
- [ ] Rewrite `reduce_range` body with type switch:
  ```c
  void* base = ray_data(input);
  switch (input->type) {
      case RAY_BOOL: case RAY_U8:
          REDUCE_LOOP_I(uint8_t, base, start, end, acc, has_nulls, null_bm); break;
      case RAY_I16:
          REDUCE_LOOP_I(int16_t, base, start, end, acc, has_nulls, null_bm); break;
      case RAY_I32: case RAY_DATE: case RAY_TIME:
          REDUCE_LOOP_I(int32_t, base, start, end, acc, has_nulls, null_bm); break;
      case RAY_I64: case RAY_TIMESTAMP:
          REDUCE_LOOP_I(int64_t, base, start, end, acc, has_nulls, null_bm); break;
      case RAY_F64:
          REDUCE_LOOP_F(base, start, end, acc, has_nulls, null_bm); break;
      case RAY_SYM: {
          /* Adaptive-width SYM columns */
          for (int64_t row = start; row < end; row++) {
              if (has_nulls && (null_bm[row/8] >> (row%8)) & 1) { acc->null_count++; continue; }
              int64_t v = read_col_i64(base, row, input->type, input->attrs);
              acc->sum_i += v; acc->cnt++;
              if (v < acc->min_i) acc->min_i = v;
              if (v > acc->max_i) acc->max_i = v;
              if (!acc->has_first) { acc->first_i = v; acc->has_first = true; }
              acc->last_i = v;
          }
          break;
      }
      default: break;
  }
  ```
- [ ] Build and test — all existing tests pass
- [ ] Commit: `feat(exec): full type dispatch in reduce_range`

---

### Task 1.3: Add reduction tests with nulls and varied types

**Files:** Modify `test/test_exec.c`

- [ ] Add test: reduce I32 vector `[10, 20, 30]` → sum=60, count=3, min=10, max=30, avg=20.0
- [ ] Add test: reduce I16 vector `[1, 2, 3, 4, 5]` → sum=15
- [ ] Add test: reduce BOOL vector `[true, false, true]` → sum=2, count=3
- [ ] Add test: reduce I64 vector with nulls at indices 1,3 → `[10, NULL, 30, NULL, 50]` → sum=90, count=3
- [ ] Add test: reduce F64 vector with null at index 0 → `[NULL, 2.0, 3.0]` → avg=2.5, count=2
- [ ] Add test: empty vector → count=0, sum=0
- [ ] Build and test
- [ ] Commit: `test(exec): reduction tests for null handling and type coverage`

---

## Phase 2: Lazy Handle Infrastructure

### Task 2.1: Add RAY_LAZY type tag and lazy handle struct

**Files:** Modify `include/rayforce.h`

The `ray_t` attrs byte is fully used. Instead of a flag, use a dedicated type tag for lazy handles. When `type == RAY_LAZY`, the union fields carry the graph pointer and op node.

- [ ] Add type constant near the other type defines (around line 130):
  ```c
  #define RAY_LAZY      (-20)   /* lazy DAG handle (atom-like, negative) */
  ```
- [ ] Add struct for lazy handle data. Reuse the ray_t union — a lazy handle stores a graph pointer and op ID in the 8 bytes where `i64`/`f64`/`obj` normally live. Since we need two pointers (graph + op), use the first 16 bytes (nullmap region, which is unused for atoms):
  ```c
  /* Lazy DAG handle — type == RAY_LAZY.
   * Bytes 0-7:  ray_graph_t* pointer (overlays nullmap[0..7])
   * Bytes 8-15: ray_op_t* pointer    (overlays nullmap[8..15])
   * The lazy handle owns the graph — ray_release frees it. */
  ```
- [ ] Add accessor macros:
  ```c
  #define RAY_LAZY_GRAPH(p) (*(ray_graph_t**)((p)->nullmap))
  #define RAY_LAZY_OP(p)    (*(ray_op_t**)(((p)->nullmap) + 8))
  ```
- [ ] Add API declarations:
  ```c
  ray_t* ray_lazy_wrap(ray_graph_t* g, ray_op_t* op);
  ray_t* ray_lazy_append(ray_t* lazy, uint16_t opcode);
  ray_t* ray_materialize(ray_t* val);
  static inline bool ray_is_lazy(ray_t* x) { return x && !RAY_IS_ERR(x) && x->type == RAY_LAZY; }
  ```
- [ ] Build (compile only, functions not yet implemented)
- [ ] Commit: `feat: add RAY_LAZY type tag and accessor macros`

---

### Task 2.2: Implement lazy handle functions

**Files:** Modify `src/ops/graph.c` (or create new `src/ops/lazy.c` — prefer graph.c to keep DAG code together)

- [ ] Add `ray_graph_input_vec` — creates an OP_SCAN-like node that holds a pre-existing vector:
  ```c
  ray_op_t* ray_graph_input_vec(ray_graph_t* g, ray_t* vec) {
      ray_op_t* op = graph_alloc_node(g);
      op->opcode = OP_SCAN;
      op->arity = 0;
      op->out_type = vec->type;
      op->est_rows = (uint32_t)vec->len;
      /* Store the vector reference in the graph's input table */
      // ... bind vec as a pre-resolved scan
      return op;
  }
  ```
  **Note:** Study how `exec_node` handles OP_SCAN to determine the cleanest way to attach a pre-existing vector. The scan currently resolves from `g->table` — we may need a small array of "input vectors" on the graph, or use the ext node's `resolved` field if one exists.

- [ ] Implement `ray_lazy_wrap`:
  ```c
  ray_t* ray_lazy_wrap(ray_graph_t* g, ray_op_t* op) {
      ray_t* h = ray_alloc(0);  /* atom-sized, no data payload */
      if (!h) { ray_graph_free(g); return RAY_ERR_PTR(RAY_ERR_OOM); }
      h->type = RAY_LAZY;
      h->attrs = 0;
      RAY_LAZY_GRAPH(h) = g;
      RAY_LAZY_OP(h) = op;
      return h;
  }
  ```

- [ ] Implement `ray_lazy_append` — takes an existing lazy handle, adds an op node to its graph:
  ```c
  ray_t* ray_lazy_append(ray_t* lazy, uint16_t opcode) {
      ray_graph_t* g = RAY_LAZY_GRAPH(lazy);
      ray_op_t* prev = RAY_LAZY_OP(lazy);
      ray_op_t* op = make_unary(g, opcode, prev, /* out_type from opcode */);
      RAY_LAZY_OP(lazy) = op;
      return lazy;
  }
  ```

- [ ] Implement `ray_materialize`:
  ```c
  ray_t* ray_materialize(ray_t* val) {
      if (!ray_is_lazy(val)) return val;
      ray_graph_t* g = RAY_LAZY_GRAPH(val);
      ray_op_t* op = RAY_LAZY_OP(val);
      ray_t* result = ray_execute(g, op);
      ray_graph_free(g);
      ray_release(val);  /* free the lazy handle itself */
      return result;
  }
  ```

- [ ] Handle lazy handles in `ray_release` — if type is RAY_LAZY, free the owned graph before freeing the block. Check `src/mem/cow.c` or wherever `ray_release` dispatches by type.

- [ ] Build and test (basic: create lazy, materialize, verify result)
- [ ] Commit: `feat: implement lazy DAG handle creation and materialization`

---

### Task 2.3: Test lazy handle lifecycle

**Files:** Modify `test/test_exec.c` or create `test/test_lazy.c`

- [ ] Test: create I64 vector [1,2,3,4,5], wrap in lazy OP_SUM, materialize → 15
- [ ] Test: chain two ops — lazy OP_ADD(vec, vec) → OP_SUM, materialize → 30
- [ ] Test: materialize on atom passthrough — ray_materialize on non-lazy returns unchanged
- [ ] Test: ray_release on lazy handle frees graph (no leak under ASan)
- [ ] Build and test
- [ ] Commit: `test: lazy handle lifecycle tests`

---

## Phase 3: Rewire Eval.c Builtins

### Task 3.1: Replace aggregation builtins with lazy wrappers

**Files:** Modify `src/lang/eval.c:295-591`

Replace the current ray_sum_fn, ray_count_fn, ray_avg_fn, ray_min, ray_max, ray_first_fn, ray_last_fn, ray_med, ray_dev implementations. Each becomes ~10 lines:

- [ ] Add `#include "ops/graph.h"` to eval.c if not already present
- [ ] Replace `ray_sum_fn` (lines ~295-332) with:
  ```c
  ray_t* ray_sum_fn(ray_t* x) {
      if (ray_is_atom(x)) return ray_is_lazy(x) ? ray_lazy_append(x, OP_SUM) : clone_atom(x);
      if (!ray_is_vec(x) && !is_list(x)) return RAY_ERR_PTR(RAY_ERR_TYPE);
      if (ray_is_lazy(x)) return ray_lazy_append(x, OP_SUM);
      ray_graph_t* g = ray_graph_new(NULL);
      ray_op_t* in = ray_graph_input_vec(g, x);
      ray_op_t* op = ray_sum(g, in);
      return ray_lazy_wrap(g, op);
  }
  ```
  **Note:** `ray_sum(g, in)` is the DAG constructor in graph.c (line ~595), not the Rayfall builtin. They have different signatures — the DAG constructor takes `(graph, op)` and returns `ray_op_t*`.

- [ ] Apply same pattern for count, avg, min, max, first, last
- [ ] For `med` and `dev`: these don't have DAG opcodes yet (OP_MED, OP_DEV don't exist in the executor). **Two options:**
  - A: Add OP_MED and OP_DEV to exec.c — implement in exec_reduction
  - B: Keep med/dev as direct implementations that call `ray_materialize` on input if lazy, then compute
  - **Choose B for now (YAGNI)** — med requires sorting which doesn't fit the reduce_acc pattern. Dev could use sum_sq but we can add it later.
- [ ] For `med`: if input is lazy, materialize first, then compute on concrete vector
- [ ] For `dev`: same — materialize if lazy, then use `reduce_range` directly for sum/sum_sq/cnt
- [ ] Update function registrations if names changed (ray_sum_fn → still registered as "sum")
- [ ] Build and test
- [ ] Commit: `feat(eval): replace aggregation builtins with lazy DAG wrappers`

---

### Task 3.2: Replace collection builtins that operate on vectors

**Files:** Modify `src/lang/eval.c`

For builtins that take and return vectors (distinct, reverse, take, etc.), these should also become lazy if the DAG has corresponding opcodes. Check which opcodes exist.

- [ ] Check which collection ops have DAG opcodes:
  ```bash
  grep 'OP_DISTINCT\|OP_REVERSE\|OP_TAKE\|OP_TIL\|OP_SORT\|OP_FILTER' include/rayforce.h
  ```
- [ ] For ops WITH DAG opcodes: convert to lazy wrapper pattern
- [ ] For ops WITHOUT DAG opcodes: keep direct implementation, but add `ray_materialize(x)` at the top if input could be lazy
- [ ] `ray_til` stays as-is (it generates data, doesn't transform — already parallel via pool_dispatch)
- [ ] Build and test
- [ ] Commit: `feat(eval): lazy wrappers for collection builtins`

---

## Phase 4: Materialization Triggers in Eval

### Task 4.1: Add materialization at eval boundaries

**Files:** Modify `src/lang/eval.c` — the `ray_eval()` function and key dispatch points

- [ ] In `ray_eval()`, after evaluating a top-level expression (before returning to caller), materialize:
  ```c
  ray_t* result = /* ... eval ... */;
  if (ray_is_lazy(result))
      result = ray_materialize(result);
  return result;
  ```
  **But only at the outermost eval call** — not recursively. Track eval depth or add a `top_level` flag.

- [ ] In the REPL eval loop (`src/app/repl.c`), before printing:
  ```c
  if (ray_is_lazy(result))
      result = ray_materialize(result);
  ```

- [ ] In `rfl_cond` (if): materialize the condition before testing truthiness
- [ ] In `ray_at` (index): materialize the vector before indexing
- [ ] In `ray_lang_print`: materialize before printing
- [ ] In `rfl_set`/`rfl_let`: materialize before binding to env (or allow lazy binding — defer this decision)
- [ ] Build and test — all existing tests pass, REPL works:
  ```bash
  echo '(sum (til 100))' | ./build/rayforce
  ```
  Expected: `4950`
- [ ] Commit: `feat(eval): materialization triggers for lazy handles`

---

### Task 4.2: Integration tests

**Files:** Modify `test/test_lang.c`

- [ ] Test: `(sum (til 100))` → 4950
- [ ] Test: `(avg (til 10))` → 4.5
- [ ] Test: `(min (til 10))` → 0
- [ ] Test: `(max (til 10))` → 9
- [ ] Test: `(count (til 100))` → 100
- [ ] Test: `(first (til 10))` → 0
- [ ] Test: `(last (til 10))` → 9
- [ ] Test chain: `(sum (+ (til 10) (til 10)))` → 90 (if + supports lazy chaining) or just verify it works
- [ ] Test: `(if (> (sum (til 10)) 0) 1 0)` → 1 (materialization in condition)
- [ ] Build and test
- [ ] Commit: `test: verb engine integration tests`

---

## Dependency Graph

```
Phase 1 (sequential):
  1.1 null_count + null-aware reduce → 1.2 type dispatch → 1.3 tests

Phase 2 (sequential, after 1):
  2.1 RAY_LAZY type → 2.2 lazy functions → 2.3 tests

Phase 3 (after 2):
  3.1 aggregation builtins → 3.2 collection builtins

Phase 4 (after 3):
  4.1 materialization triggers → 4.2 integration tests
```

Phases are sequential. Each phase is independently shippable — Phase 1 alone improves the executor. Phase 2 adds infrastructure. Phase 3+4 complete the unification.
