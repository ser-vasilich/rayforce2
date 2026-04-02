<p align="center">
  <img src="docs/logo.svg" alt="Rayforce" width="360">
</p>

<p align="center">
  Columnar analytics and graph traversal in one fused pipeline.
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="MIT License"></a>
  <a href="include/rayforce.h"><img src="https://img.shields.io/badge/header-rayforce.h-informational" alt="Single Header"></a>
  <a href="https://rayforcedb.github.io/rayforce2/"><img src="https://img.shields.io/badge/docs-website-e9a033" alt="Docs"></a>
</p>

---

Rayforce is a pure C17 zero-dependency embeddable engine where analytics
operations and graph traversals share a single operation DAG, pass through a
10-pass optimizer, and execute as fused morsel-driven bytecode. One header. No
malloc. 22 graph algorithms. 143 query builtins.

Rayforce unifies two engines:
- **Teide** — columnar analytics (morsel-fused execution, radix-partitioned hash joins, 10-pass optimizer)
- **Rayforce** — graph engine (CSR storage, BFS/DFS, PageRank, Dijkstra, A*, Leapfrog TrieJoin)

Both now share a single DAG, a single optimizer, and a single executor.

## Quick Start

```bash
make            # debug build (ASan + UBSan)
make release    # optimized build
make test       # 563 tests across 32 suites
./rayforce      # start the Rayfall REPL
```

## Rayfall REPL

Rayforce ships with **Rayfall** — a Lisp-like query language with 143 builtins:

```lisp
rf> (set t (table [Symbol Side Qty]
      (list [AAPL GOOG MSFT AAPL GOOG]
            [Buy Sell Buy Sell Buy]
            [100 200 150 300 250])))

rf> (select {from:t by: Symbol Qty: (sum Qty)})
┌────────┬─────┐
│ Symbol │ Qty │
├────────┼─────┤
│ AAPL   │ 400 │
│ GOOG   │ 450 │
│ MSFT   │ 150 │
└────────┴─────┘

rf> (pivot t 'Symbol 'Side 'Qty sum)
┌────────┬─────┬──────┐
│ Symbol │ Buy │ Sell │
├────────┼─────┼──────┤
│ AAPL   │ 100 │  300 │
│ GOOG   │ 250 │  200 │
│ MSFT   │ 150 │    0 │
└────────┴─────┴──────┘
```

## C API

Single public header: [`include/rayforce.h`](include/rayforce.h)

```c
#include <rayforce.h>

int main(void) {
    ray_heap_init();
    ray_sym_init();

    ray_t* trades = ray_read_csv("trades.csv");

    ray_graph_t* g = ray_graph_new(trades);
    ray_op_t* region = ray_filter(g, ray_scan(g, "region"),
        ray_eq(g, ray_scan(g, "flag"), ray_const_i64(g, 0)));
    ray_op_t* amount = ray_filter(g, ray_scan(g, "amount"),
        ray_eq(g, ray_scan(g, "flag"), ray_const_i64(g, 0)));

    ray_op_t* keys[] = { region };
    uint16_t ops[]   = { OP_SUM };
    ray_op_t* ins[]  = { amount };
    ray_op_t* grp = ray_group(g, keys, 1, ops, ins, 1);

    ray_t* result = ray_execute(g, ray_optimize(g, grp));

    ray_release(result);
    ray_graph_free(g);
    ray_release(trades);
    ray_sym_destroy();
    ray_heap_destroy();
}
```

## Capabilities

|                              | Rayforce | DuckDB | Polars |
|------------------------------|:--------:|:------:|:------:|
| Native graph engine (CSR)    |    ✓     |        |        |
| 22 graph algorithms          |    ✓     |        |        |
| Worst-case optimal joins     |    ✓     |        |        |
| Factorized execution         |    ✓     |        |        |
| SIP optimizer                |    ✓     |        |        |
| Embeddable (single header)   |    ✓     |        |        |
| Zero external dependencies   |    ✓     |        |        |
| Built-in query language       |    ✓     |        |        |
| Fused morsel pipelines       |    ✓     |   ✓    |   ✓    |
| 10-pass query optimizer      |    ✓     |   ✓    |        |
| COW ref counting             |    ✓     |        |   ✓    |
| Custom memory allocator      |    ✓     |   ✓    |        |
| Window functions & ASOF join |    ✓     |   ✓    |   ✓    |

## How It Works

**Build** — Construct a lazy DAG with 40+ operators: scans, filters, joins,
aggregations, window functions, graph traversals. Nothing executes yet.

**Optimize** — 10 rewrite passes: type inference → constant folding → SIP →
factorize → predicate pushdown → filter reorder → projection pushdown →
partition pruning → fusion → DCE.

**Execute** — Fused morsel-driven bytecode processes 1024-element chunks that
stay L1-resident. Radix-partitioned hash joins size partitions to fit L2.
Thread pool dispatches morsels in parallel.

## Features

**Execution engine**
- Lazy DAG with 40+ operators — nothing runs until `ray_execute`
- 10-pass optimizer with sideways information passing
- Fused morsel-driven bytecode — element-wise ops merged into single-pass chunks
- Radix-partitioned hash joins sized for L2 cache
- Thread pool with parallel morsel dispatch

**Graph engine**
- Double-indexed CSR storage (forward + reverse), mmap support
- 22 algorithms: BFS, DFS, Dijkstra, A*, PageRank, Louvain, Betweenness, LFTJ, ...
- Factorized execution avoids materializing cross-products
- SIP propagates selection bitmaps backward through expand chains

**Rayfall language**
- 143 builtins: arithmetic, string, aggregation, joins, higher-order, I/O
- Lambdas compile lazily to bytecode, run in computed-goto VM
- `select`/`update`/`pivot` bridge to the DAG optimizer at runtime

**Memory**
- Buddy allocator with slab cache — O(1) for ~90% of allocations
- Thread-local arenas, lock-free allocation, COW ref counting
- No system allocator — `ray_alloc`/`ray_free` for everything

**Storage**
- Columnar `.col` files with mmap, splayed tables, date-partitioned tables
- CSV reader with parallel mmap parse, type inference, null handling

## Project Structure

```
include/rayforce.h         Single public header (all types, opcodes, API)
src/mem/                    Buddy allocator, slab cache, arena, COW
src/core/                   Type system, platform abstraction, runtime
src/vec/                    Vector, list, string, selection bitmap ops
src/table/                  Table, symbol intern table
src/store/                  Column files, CSR, splayed/parted tables, HNSW
src/ops/                    DAG, optimizer (10 passes), fused executor, LFTJ
src/io/                     CSV reader/writer (parallel mmap)
src/lang/                   Rayfall parser, evaluator, bytecode VM (143 builtins)
src/app/                    REPL, terminal, pretty-printer
test/                       563 tests across 32 suites
examples/rfl/               18 Rayfall example scripts
examples/c/                 4 C API examples
website/                    Documentation site (GitHub Pages)
```

## Documentation

Full docs: **[rayforcedb.github.io/rayforce2](https://rayforcedb.github.io/rayforce2/)**

- [Quick Start](https://rayforcedb.github.io/rayforce2/docs/quick-start.html) — build, REPL, first query
- [Rayfall Language](https://rayforcedb.github.io/rayforce2/docs/rayfall-syntax.html) — syntax, 143 builtins
- [Data Types](https://rayforcedb.github.io/rayforce2/docs/data-types.html) — 12 types, collections
- [Queries](https://rayforcedb.github.io/rayforce2/docs/queries-select.html) — select, joins, pivot, window
- [C API](https://rayforcedb.github.io/rayforce2/docs/c-api-core.html) — 65 public functions
- [Graph Engine](https://rayforcedb.github.io/rayforce2/docs/graph-algorithms.html) — 22 algorithms
- [Architecture](https://rayforcedb.github.io/rayforce2/docs/architecture-pipeline.html) — DAG, optimizer, memory

## License

[MIT](LICENSE)
