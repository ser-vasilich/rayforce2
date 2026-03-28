<p align="center">
  <img src="docs/logo.svg" alt="Rayforce" width="360">
</p>

<p align="center">
  Analytics and graph traversal in one fused pipeline.
</p>

<p align="center">
  <a href="https://github.com/RayforceDB/rayforce/actions/workflows/ci.yml"><img src="https://github.com/RayforceDB/rayforce/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="MIT License"></a>
  <a href="include/rayforce.h"><img src="https://img.shields.io/badge/header-rayforce.h-informational" alt="Single Header"></a>
</p>

---

Rayforce is an embeddable columnar compute engine where analytics operations and
graph traversals live in the same operation DAG, pass through a 10-pass
optimizer, and execute as fused morsel-driven bytecode. Pure C17. Zero
dependencies. One header.

## Capabilities

|                              | Rayforce | DuckDB | Polars |
|------------------------------|:-----:|:------:|:------:|
| Native graph engine (CSR)    |   ✓   |        |        |
| Worst-case optimal joins     |   ✓   |        |        |
| Factorized execution         |   ✓   |        |        |
| SIP optimizer                |   ✓   |        |        |
| Embeddable (single header)   |   ✓   |        |        |
| Zero external dependencies   |   ✓   |        |        |
| Fused morsel pipelines       |   ✓   |   ✓    |   ✓    |
| 10-pass query optimizer      |   ✓   |   ✓    |        |
| COW ref counting             |   ✓   |        |   ✓    |
| Custom memory allocator      |   ✓   |   ✓    |        |
| Window functions & ASOF join |   ✓   |   ✓    |   ✓    |

Rayforce is not a SQL database. It is designed for workloads that mix analytics
with graph traversal in a single fused pipeline — without stitching tools
together.

## How It Works

<picture>
  <img src="docs/architecture.svg" alt="Architecture: User Code → Lazy DAG → Optimizer → Fused Morsel Executor → Result" width="520">
</picture>

**Build** — Construct a lazy DAG with 40+ operators: scans, filters, joins,
aggregations, window functions, graph traversals. Nothing executes yet.

**Optimize** — The DAG passes through 10 rewrite passes: type inference →
constant folding → sideways information passing → factorize → predicate
pushdown → filter reorder → projection pushdown → partition pruning → fusion →
dead code elimination.

**Execute** — Fused morsel-driven bytecode processes 1024-element chunks that
stay L1-resident. Radix-partitioned hash joins size partitions to fit L2.
Thread pool dispatches morsels in parallel.

## Memory Model

<picture>
  <img src="docs/memory.svg" alt="Memory Model: Heap with buddy allocator, slab cache, thread-local arenas, ray_t block layout" width="600">
</picture>

Everything is a `ray_t` — a 32-byte block header. Atoms, vectors, lists,
tables, selection bitmaps. Buddy allocator with slab cache handles ~90% of
allocations in O(1). Thread-local arenas enable lock-free allocation. COW ref
counting gives zero-copy slices and shared columns.

## Examples

### Filter + group + sum

```c
#include <rayforce.h>

int main(void) {
    ray_heap_init();
    ray_sym_init();

    ray_t* trades = ray_read_csv("trades.csv");

    /* Build the operation DAG — nothing executes yet */
    ray_graph_t* g = ray_graph_new(trades);

    /* Filter: keep only rows where flag == 0 */
    ray_op_t* flag = ray_scan(g, "flag");
    ray_op_t* pred = ray_eq(g, flag, ray_const_i64(g, 0));

    ray_op_t* region = ray_filter(g, ray_scan(g, "region"), pred);
    ray_op_t* amount = ray_filter(g, ray_scan(g, "amount"), pred);

    /* Group by region, sum amounts */
    ray_op_t* keys[]    = { region };
    uint16_t agg_ops[] = { OP_SUM };
    ray_op_t* agg_ins[] = { amount };
    ray_op_t* grp = ray_group(g, keys, 1, agg_ops, agg_ins, 1);

    /* Optimize (10 passes) and execute */
    ray_t* result = ray_execute(g, ray_optimize(g, grp));

    /* result:
     *   region | sum_amount
     *   -------|----------
     *        0 |    166583
     *        1 |    166742
     *        2 |    166900
     *        3 |    167058
     *        4 |    167217
     */

    if (result && !RAY_IS_ERR(result)) ray_release(result);
    ray_graph_free(g);
    ray_release(trades);
    ray_sym_destroy();
    ray_heap_destroy();
    return 0;
}
```

### Graph traversal: BFS from a start node

```c
#include <rayforce.h>

int main(void) {
    ray_heap_init();
    ray_sym_init();

    /* Build a directed graph: 0→1, 0→2, 1→2, 1→3, 2→3, 3→0 */
    ray_t* src = ray_vec_from_raw(RAY_I64, (int64_t[]){0,0,1,1,2,3}, 6);
    ray_t* dst = ray_vec_from_raw(RAY_I64, (int64_t[]){1,2,2,3,3,0}, 6);

    ray_t* edges = ray_table_new(2);
    edges = ray_table_add_col(edges, ray_sym_intern("src", 3), src);
    edges = ray_table_add_col(edges, ray_sym_intern("dst", 3), dst);
    ray_release(src);
    ray_release(dst);

    /* Double-indexed CSR (forward + reverse) */
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, true);

    /* Start at node 0, BFS 1..3 hops forward */
    ray_t* start = ray_vec_from_raw(RAY_I64, (int64_t[]){0}, 1);
    ray_t* nodes = ray_table_new(1);
    nodes = ray_table_add_col(nodes, ray_sym_intern("id", 2), start);
    ray_release(start);

    ray_graph_t* g = ray_graph_new(nodes);
    ray_op_t* reach = ray_var_expand(g, ray_scan(g, "id"), rel, 0, 1, 3, false);

    ray_t* result = ray_execute(g, ray_optimize(g, reach));

    /* result (BFS from node 0, depth 1..3):
     *   src | dst | depth
     *   ----|-----|------
     *     0 |   1 |     1
     *     0 |   2 |     1
     *     0 |   3 |     2
     */

    if (result && !RAY_IS_ERR(result)) ray_release(result);
    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(edges);
    ray_release(nodes);
    ray_sym_destroy();
    ray_heap_destroy();
    return 0;
}
```

### Join two tables

```c
#include <rayforce.h>

int main(void) {
    ray_heap_init();
    ray_sym_init();

    ray_t* orders = ray_read_csv("orders.csv");
    ray_t* custs  = ray_read_csv("customers.csv");

    ray_graph_t* g = ray_graph_new(orders);

    ray_op_t* lo = ray_const_table(g, orders);
    ray_op_t* ro = ray_const_table(g, custs);

    /* Inner join on customer_id */
    ray_op_t* lk[] = { ray_scan(g, "customer_id") };
    ray_op_t* rk[] = { ray_scan(g, "customer_id") };
    ray_op_t* joined = ray_join(g, lo, lk, ro, rk, 1, 0);

    ray_t* result = ray_execute(g, ray_optimize(g, joined));

    /* result:
     *   customer_id | amount | name
     *   ------------|--------|--------
     *             1 |    250 | Alice
     *             2 |    180 | Bob
     *             2 |    340 | Bob
     *             3 |    120 | Charlie
     */

    if (result && !RAY_IS_ERR(result)) ray_release(result);
    ray_graph_free(g);
    ray_release(orders);
    ray_release(custs);
    ray_sym_destroy();
    ray_heap_destroy();
    return 0;
}
```

## Features

**Execution engine**
- Lazy DAG with 40+ operators — nothing runs until `ray_execute`
- 10-pass optimizer with sideways information passing and graph-aware rewriting
- Fused morsel-driven bytecode — element-wise ops merged into single-pass chunks
- Radix-partitioned hash joins sized for L2 cache
- Thread pool with parallel morsel dispatch

**Graph engine**
- Double-indexed CSR storage (forward + reverse), mmap support
- 1-hop expand, variable-length BFS, shortest path
- Worst-case optimal joins via Leapfrog Triejoin (triangles, k-cliques)
- Factorized execution avoids materializing cross-products
- SIP propagates selection bitmaps backward through expand chains

**Data types & operations**
- Unified 32-byte `ray_t` block header — atoms, vectors, lists, tables, bitmaps
- Dictionary-encoded symbols (8/16/32/64-bit adaptive-width indices)
- Variable-length strings with inline SSO and per-vector pool
- Window functions: ROW_NUMBER, RANK, DENSE_RANK, NTILE, SUM, AVG, LAG, LEAD, ...
- ASOF joins for time-series alignment
- Full null propagation across all string and arithmetic operations

**Memory & storage**
- Buddy allocator with slab cache — O(1) for ~90% of allocations
- Thread-local arenas, lock-free allocation, COW ref counting
- Columnar `.col` files, splayed tables, date-partitioned tables, mmap
- Arena allocator for bulk short-lived allocations

**I/O**
- CSV reader with type inference, configurable delimiters, null handling
- Zero external dependencies — pure C17, single public header

## API Overview

Single public header: [`include/rayforce.h`](include/rayforce.h)

| Category | Functions |
|-------------------|-------------------------------------------------------------------------|
| **Lifecycle** | `ray_heap_init`, `ray_heap_destroy`, `ray_sym_init`, `ray_sym_destroy` |
| **Memory** | `ray_alloc`, `ray_free`, `ray_retain`, `ray_release`, `ray_cow` |
| **Atoms** | `ray_bool`, `ray_i64`, `ray_f64`, `ray_str`, `ray_sym`, `ray_date`, ... |
| **Vectors** | `ray_vec_new`, `ray_vec_append`, `ray_vec_set`, `ray_vec_get`, `ray_vec_slice`, `ray_vec_concat`, `ray_vec_from_raw` |
| **Tables** | `ray_table_new`, `ray_table_add_col`, `ray_table_get_col`, `ray_table_ncols`, `ray_table_nrows` |
| **DAG sources** | `ray_graph_new`, `ray_scan`, `ray_const_i64`, `ray_const_f64`, `ray_const_str`, `ray_const_table` |
| **Unary ops** | `ray_neg`, `ray_abs`, `ray_not`, `ray_sqrt_op`, `ray_log_op`, `ray_isnull`, `ray_cast`, `ray_upper`, `ray_lower`, `ray_trim_op` |
| **Binary ops** | `ray_add`, `ray_sub`, `ray_mul`, `ray_div`, `ray_mod`, `ray_eq`, `ray_ne`, `ray_lt`, `ray_le`, `ray_gt`, `ray_ge`, `ray_and`, `ray_or`, `ray_like` |
| **Aggregations** | `ray_sum`, `ray_count`, `ray_avg`, `ray_min_op`, `ray_max_op`, `ray_first`, `ray_last`, `ray_stddev`, `ray_count_distinct` |
| **Structural** | `ray_filter`, `ray_sort_op`, `ray_group`, `ray_distinct`, `ray_join`, `ray_asof_join`, `ray_select`, `ray_head`, `ray_tail` |
| **Window** | `ray_window_op` (ROW_NUMBER, RANK, DENSE_RANK, NTILE, SUM, AVG, LAG, LEAD, ...) |
| **Graph** | `ray_expand`, `ray_var_expand`, `ray_shortest_path`, `ray_wco_join` |
| **CSR / Relations**| `ray_rel_build`, `ray_rel_from_edges`, `ray_rel_save`, `ray_rel_load`, `ray_rel_mmap`, `ray_rel_free` |
| **Optimizer** | `ray_optimize`, `ray_fuse_pass` |
| **Executor** | `ray_execute` |
| **Storage** | `ray_col_save`, `ray_col_load`, `ray_col_mmap`, `ray_splay_save`, `ray_splay_load`, `ray_part_load` |
| **CSV** | `ray_read_csv`, `ray_read_csv_opts`, `ray_write_csv` |
| **Parallelism** | `ray_pool_init`, `ray_pool_destroy`, `ray_parallel_begin`, `ray_parallel_end` |

## Performance

Key design choices that make Rayforce fast:

- **Morsel-fused execution** — element-wise ops fused into a single pass over 1024-element chunks, maximizing L1 cache residency
- **Radix-partitioned hash joins** — adaptive radix bits (2..14) size partitions to fit L2 cache
- **Buddy + slab allocator** — O(1) alloc/free for common sizes, no system allocator overhead
- **COW ref counting** — zero-copy slices and shared columns, copy only on mutation
- **Selection bitmaps** — `RAY_SEL` segments skip entire morsels when all rows pass or all are filtered

Benchmarks: [rayforce-bench](https://github.com/RayforceDB/rayforce-bench)

## Build

```bash
# Debug (ASan + UBSan)
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# Release
cmake -B build_release -DCMAKE_BUILD_TYPE=Release
cmake --build build_release

# Run all tests (270+ tests across 28 suites)
cd build && ctest --output-on-failure

# Run a single test suite
./build/test_rayforce --suite /vec
```

## Project Structure

```
include/rayforce.h         Single public header (all types, opcodes, API)
src/mem/                    Buddy allocator, slab cache, VM abstraction
src/core/                   Type system, atoms, strings, symbols
src/vec/                    Vector operations, morsel iterator
src/table/                  Table construction, column access, schema
src/store/                  Column files, splayed tables, partitions, CSR
src/ops/                    DAG construction, optimizer, executor, LFTJ
src/io/                     CSV reader/writer
test/                       270+ tests across 28 suites
bench/                      Benchmark harness
```

## License

[MIT](LICENSE)
