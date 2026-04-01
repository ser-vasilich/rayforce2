# OP_PIVOT — DAG-Level Pivot Table

## Motivation

Current tree-walk pivot composes builtins sequentially: for each distinct pivot value, filter → group → aggregate → left-join. This is O(n × P) where P = number of distinct pivot values, and breaks on multi-index keys. A DAG-level pivot does a single hash-partitioned pass with parallel aggregation.

## Semantics

```
(pivot table index_cols pivot_col value_col agg_fn)
```

- `index_cols` — symbol or list of symbols: row keys (become result rows)
- `pivot_col` — symbol: column whose distinct values become result columns
- `value_col` — symbol: column to aggregate
- `agg_fn` — aggregation function (sum, avg, min, max, count, first, last, med, or lambda)

Output: table with `index_cols` + one column per distinct pivot value, containing aggregated values.

## Algorithm

### Phase 1: Hash partitioning (single scan, parallel)

Scan the table once. For each row, compute a composite key:
```
hash_key = hash(index_col_values..., pivot_col_value)
```

Build a hash table mapping `(index_key, pivot_value)` → accumulator.

This is structurally identical to `exec_group` but with a two-part key: the index part determines the output row, the pivot part determines the output column.

### Phase 2: Materialize result

1. Collect distinct index keys → these are the output rows
2. Collect distinct pivot values → these are the output column names
3. For each (row, column) cell: look up the accumulator, finalize aggregation
4. Build output table: index columns + one value column per pivot value

### Phase 3: Fill missing cells

Not every (index, pivot) combination may exist in the input. Missing cells get the null sentinel for the value type (0N for numerics, "" for strings).

## DAG Integration

### New opcode

```c
#define OP_PIVOT  77   /* next available opcode */
```

### DAG node extension

```c
struct {
    ray_op_t** index_cols;   /* array of OP_SCAN nodes for index columns */
    ray_op_t*  pivot_col;    /* OP_SCAN node for pivot column */
    ray_op_t*  value_col;    /* OP_SCAN node for value column */
    uint16_t   agg_op;       /* OP_SUM, OP_AVG, etc. */
    uint8_t    n_index;      /* number of index columns */
} pivot;
```

### Graph builder

```c
ray_op_t* ray_pivot_op(ray_graph_t* g,
                       ray_op_t** index_cols, uint8_t n_index,
                       ray_op_t* pivot_col,
                       ray_op_t* value_col,
                       uint16_t agg_op);
```

### Executor

`exec_pivot()` in exec.c — similar structure to `exec_group()`:

1. Resolve input columns from DAG nodes
2. Hash-partition into buckets keyed by (index_values, pivot_value)
3. For each bucket: maintain aggregation accumulator (reuse `reduce_acc_t`)
4. Materialize: build output table from accumulators

Can reuse existing infrastructure:
- `ray_pool_dispatch()` for parallel hashing
- `reduce_acc_t` for aggregation accumulators
- `radix_encode_fn` for key encoding
- Hash table from `exec_group`'s DA (direct aggregation) path

### Lang wrapper

`ray_pivot_fn` in eval.c becomes a thin wrapper:
1. Validate args
2. Map `agg_fn` to opcode (sum→OP_SUM, avg→OP_AVG, etc.)
3. For lambda agg_fn: fall back to tree-walk (custom functions can't be DAG-ified)
4. Build DAG graph with OP_PIVOT node
5. Execute via `ray_execute()`

### Public core function

Following the `ray_sort_indices` pattern, extract the core:

```c
/* Pivot table: single-pass parallel hash aggregation.
 * Returns new table with index_cols + one column per distinct pivot value. */
ray_t* ray_pivot_table(ray_t* tbl,
                       int64_t* index_syms, uint8_t n_index,
                       int64_t pivot_sym,
                       int64_t value_sym,
                       uint16_t agg_op);
```

DAG executor and lang builtin both call this core function.

## Performance

- **Single scan** — O(n) regardless of number of distinct pivot values
- **Parallel** — morsel-based hash partitioning via thread pool
- **Memory** — one accumulator per (index_key, pivot_value) pair
- **vs tree-walk** — eliminates N left-joins, N filter passes, N group passes

## Implementation order

1. Add `OP_PIVOT` opcode to ops.h
2. Add DAG node struct + graph builder to graph.c
3. Implement `ray_pivot_table()` core in exec.c (reuse exec_group hash infra)
4. Add `exec_pivot()` DAG executor entry in exec_node_inner
5. Update `ray_pivot_fn` in eval.c to use DAG path for known agg ops, tree-walk for lambdas
6. Test with pivot.rfl examples (single + multi-index)
7. Verify multi-index works correctly
