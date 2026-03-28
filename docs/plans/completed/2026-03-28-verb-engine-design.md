# Unified Verb Engine — Design

**Goal:** Unify standalone Rayfall verb evaluation and the DAG executor into a single execution path. All vector operations go through the DAG — one place for type dispatch, null handling, and parallel morsel execution.

## Problem

Verbs currently exist in two places:
- **eval.c** — toy implementations: sequential loops, 3 types (I64/F64/I32), no null awareness, no parallelism
- **exec.c** — proper morsel-parallel pipeline, but only reachable through `select`/`update` queries

`(sum col)` at the REPL and `(select {s: (sum col) from: t})` should use the same engine. Currently they don't.

## Core Principles

- All vector operations go through the DAG executor
- Atom-on-atom operations stay eager (`(+ 1 2)` → `3`, no DAG)
- Lazy DAG construction during eval — the tree-walker builds DAG nodes instead of computing
- Dynamic code (`eval`, `list`) works naturally because it goes through the same tree-walker
- VM/bytecode doesn't change — verb function pointers become DAG-aware
- "Compute everything" accumulator — one kernel handles sum/min/max/count/avg/first/last/var/stddev

## Lazy DAG Construction

When `ray_eval()` encounters a function call, it checks arguments:

- **Atom arguments only** → compute eagerly, return atom
- **Any vector argument** → build a DAG node, return a lazy handle

Chain building happens naturally. For `(sum (+ col1 col2))`:
1. Eval `(+ col1 col2)` → both args are vectors → build `OP_ADD(col1, col2)`, return lazy handle
2. Eval `(sum <lazy>)` → argument is lazy → append `OP_SUM` to existing lazy DAG, return lazy handle
3. Top-level needs to print → materializes: `ray_execute()` on the accumulated DAG → returns scalar

**Materialization triggers:**
- Top-level expression result (REPL print, assignment)
- Scalar needed: branch condition in `if`, index in `at`
- Passed to a non-DAG-aware context (user lambda indexing elements)

**Graph ownership:** The lazy `ray_t` owns the graph. `ray_release()` frees it. Materialization replaces the lazy handle with the concrete result and frees the graph.

**Tree expressions:** `(+ (sum col1) (sum col2))` — two independent lazy handles, each materializes independently when `+` sees two scalar results. Multi-aggregate fusion on the same column is already handled by the DAG executor inside `select`/`update`; the standalone path doesn't need that optimization.

## Lambdas and Dynamic Code

The VM doesn't change. Verb calls in bytecode are function pointer dispatch. The function pointers (`ray_sum`, `ray_avg`, etc.) become DAG-aware — they return lazy handles when given vector inputs, compute eagerly on atoms. The VM doesn't know or care.

Dynamic code like `(eval (list 'sum 'col1))` works because `eval` goes through `ray_eval()`, which sees `sum` applied to a vector and builds a DAG node — same as static code.

## Verb Kernel Architecture

The `reduce_range()` function in exec.c is the single source of truth for all reduction verbs.

### Type Coverage

Type switch outside the inner loop — branch once per morsel chunk, not per element:

```c
switch (type) {
    case RAY_I64:  REDUCE_LOOP(int64_t, i); break;
    case RAY_F64:  REDUCE_LOOP(double, f);  break;
    case RAY_I32:  REDUCE_LOOP_WIDEN(int32_t, i); break;
    case RAY_I16:  REDUCE_LOOP_WIDEN(int16_t, i); break;
    case RAY_BOOL: case RAY_U8:
                   REDUCE_LOOP_WIDEN(uint8_t, i); break;
    case RAY_DATE: case RAY_TIME: case RAY_TIMESTAMP:
                   REDUCE_LOOP_WIDEN(int32_t, i); break;
}
```

`REDUCE_LOOP` reads at native width and accumulates. `REDUCE_LOOP_WIDEN` reads narrow types, widens to i64 before accumulating.

### Null Handling

```c
for (row = start; row < end; row++) {
    if (has_nulls && bitmap_test(null_bm, row)) continue;
    val = data[row];
    acc->sum += val;
    ...
}
```

Null bitmap pointer and `has_nulls` flag resolved once before the loop. When no nulls exist (`RAY_ATTR_HAS_NULLS` not set), branch predictor eliminates the check.

The `reduce_acc_t` gets a `null_count` field:
- `avg` divides by `cnt - null_count`
- `count` returns `cnt - null_count`
- `min`/`max` on all-null input returns null

### Parallel Dispatch

Unchanged — `par_reduce_fn()` splits across workers via `ray_pool_dispatch()`, each worker accumulates into its own `reduce_acc_t`, `reduce_merge()` combines results. The kernels just get better type coverage and null awareness.

## Eval.c Builtins as Thin Wrappers

Current: 100+ line implementations per verb. New: ~10 lines each.

```c
ray_t* ray_sum(ray_t* x) {
    if (ray_is_atom(x)) return x;
    if (ray_is_lazy(x)) {
        return ray_lazy_append(x, OP_SUM);
    }
    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* scan = ray_scan_vec(g, x);
    ray_op_t* op = ray_reduce(g, scan, OP_SUM);
    return ray_lazy_wrap(g, op);
}
```

`ray_lazy_append` takes an existing lazy handle's graph and adds a node — this is how `(sum (+ col1 col2))` builds one graph with two nodes instead of two separate graphs.

## What Changes

- `reduce_range()` in exec.c — full type switch, null bitmap skipping, macro-driven loops
- `reduce_acc_t` — add `null_count` field
- eval.c builtins — replace with thin lazy wrappers
- `ray_t` — add `RAY_ATTR_LAZY` flag, lazy handle fields (graph pointer, op node pointer)
- `ray_eval()` — verb on vector args returns lazy handle
- `ray_release()` — lazy handle frees owned graph
- Add `ray_materialize(ray_t*)` — force execution of lazy handle

## What Doesn't Change

- VM/bytecode — verb calls still function pointer dispatch
- Compiler — no new opcodes
- Parser — no changes
- DAG executor pipeline — already morsel-parallel
- Group-by aggregation in exec.c — already works, shares improved `reduce_range`
- `select`/`update` — already builds DAGs

## Implementation Order

1. Fix `reduce_range` — type coverage + null handling (pure exec.c, existing tests validate)
2. Add lazy handle infrastructure (`RAY_ATTR_LAZY`, materialize, graph ownership)
3. Rewrite eval.c builtins as lazy wrappers
4. Add materialization triggers in eval (top-level, `if`, `at`, print)
