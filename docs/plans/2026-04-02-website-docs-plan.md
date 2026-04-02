# Website & Documentation Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build a complete marketing website + documentation site for Rayforce2, highlighting both the columnar engine (formerly Teide) and graph engine heritage from rayforce1.

**Architecture:** Static HTML/CSS/JS site in `website/` deployed via GitHub Pages. Marketing landing page at root, docs site at `/docs/`. Teidelum-style dark theme (Oswald headings, Inter body, JetBrains Mono code). No build tools — plain HTML.

**Tech Stack:** HTML5, CSS3, vanilla JS, GitHub Actions for deployment

---

## Task 1: Project Scaffolding + Assets

**Files:**
- Create: `website/index.html` (placeholder)
- Create: `website/style.css` (empty)
- Create: `website/script.js` (empty)
- Create: `website/.nojekyll`
- Create: `website/CNAME` (empty for now)
- Create: `website/docs/` (directory)
- Copy: `website/assets/favicon.svg` (from rayforce1)
- Copy: `website/assets/logo-light.svg` (from rayforce1)
- Copy: `website/assets/logo-dark.svg` (from rayforce1)

**Step 1: Create directory structure**

```bash
mkdir -p website/assets website/docs
touch website/.nojekyll
```

**Step 2: Copy logos from rayforce1**

```bash
cp /home/hetoku/data/work/rayforce/docs/docs/images/favicon.svg website/assets/favicon.svg
cp /home/hetoku/data/work/rayforce/docs/docs/images/logo_light.svg website/assets/logo-light.svg
cp /home/hetoku/data/work/rayforce/docs/docs/images/logo_white.svg website/assets/logo-white.svg
cp /home/hetoku/data/work/rayforce/docs/docs/images/logo_dark_full.svg website/assets/logo-dark-full.svg
cp /home/hetoku/data/work/rayforce/docs/docs/images/logo_light_full.svg website/assets/logo-light-full.svg
```

**Step 3: Commit**

```bash
git add website/
git commit -m "chore: scaffold website directory with rayforce1 logos"
```

---

## Task 2: GitHub Actions — Pages Deployment

**Files:**
- Create: `.github/workflows/pages.yml`

**Step 1: Write workflow**

```yaml
name: Deploy to GitHub Pages

on:
  push:
    branches: [master]
    paths: ['website/**']
  workflow_dispatch:

permissions:
  contents: read
  pages: write
  id-token: write

concurrency:
  group: pages
  cancel-in-progress: true

jobs:
  deploy:
    runs-on: ubuntu-latest
    environment:
      name: github-pages
      url: ${{ steps.deployment.outputs.page_url }}
    steps:
      - uses: actions/checkout@v4
      - uses: actions/configure-pages@v4
      - uses: actions/upload-pages-artifact@v3
        with:
          path: website
      - id: deployment
        uses: actions/deploy-pages@v4
```

**Step 2: Commit and push**

```bash
git add .github/workflows/pages.yml
git commit -m "ci: add GitHub Pages deployment for website/"
git push
```

---

## Task 3: CSS — Teidelum-Style Theme

**Files:**
- Create: `website/style.css`

**Design tokens (from teidelum, adapted for Rayforce brand):**
- `--bg: #0a0e13` (dark background)
- `--primary: #e9a033` (Rayforce gold — from logo)
- `--accent: #5eead4` (teal accent — kept from teidelum)
- `--font: Inter`, `--font-heading: Oswald`, `--mono: JetBrains Mono`
- Same card style, rounded nav pill, hero gradient, section borders

**Step 1: Write the full CSS**

Port the entire teidelum `style.css` (596 lines), replacing:
- `--primary: #4b6777` → `--primary: #e9a033` (Rayforce gold)
- `--primary-light: #6b8a9e` → `--primary-light: #f0b954`
- `--primary-lighter: #8ba8b8` → `--primary-lighter: #f5d080`
- `--primary-pale: #dce8ee` → `--primary-pale: #fdf0d8`
- `--primary-dark: #3a5261` → `--primary-dark: #c4821a`
- All hero gradient colors using new primary
- All box-shadow colors using new primary

Keep `--accent: #5eead4` and `--accent-blue: #60a5fa` unchanged.

**Step 2: Commit**

```bash
git add website/style.css
git commit -m "style: add teidelum-based dark theme with Rayforce gold branding"
```

---

## Task 4: Landing Page — Hero + Nav

**Files:**
- Create: `website/index.html`

**Content structure:**

```
Nav: [Logo] Rayforce  |  Features  Docs  Architecture  GitHub →
Hero:
  Eyebrow: PURE C17 · ZERO DEPS · GRAPH + ANALYTICS
  H1: Columnar analytics and
      graph traversal in one pipeline
  Desc: Rayforce is a zero-dependency embeddable engine where analytics
        and graph operations share a single DAG, pass through a 10-pass
        optimizer, and execute as fused morsel-driven bytecode.
        Formerly Teide (columnar) + Rayforce (graph) — now unified.
  [Get Started →]  [View on GitHub]
  
  Numbers row:
  - Single Header: include/rayforce.h — one file, full API
  - 10-Pass Optimizer: SIP, factorize, predicate pushdown, fusion
  - 22 Graph Algorithms: PageRank to A* in the same pipeline
  - 143 Builtins: Rayfall — a Lisp-like query language with REPL
```

**Step 1: Write index.html with nav + hero section**

Full HTML with Google Fonts link, favicon, meta tags. Nav matches teidelum pattern (floating pill). Hero with gradient background + grid lines.

**Step 2: Commit**

```bash
git add website/index.html
git commit -m "feat: landing page hero section with nav"
```

---

## Task 5: Landing Page — Features Section

**Files:**
- Modify: `website/index.html`

**6 feature cards in a 3x2 grid:**

1. **Fused Execution** — Morsel-driven bytecode over 1024-element chunks. Element-wise ops fused into single-pass pipelines that stay L1-resident.

2. **Graph Engine** — Double-indexed CSR with 22 algorithms. BFS, Dijkstra, A*, PageRank, Louvain, Leapfrog TrieJoin — all in the same DAG.

3. **Rayfall Language** — Lisp-like query REPL with 143 builtins. Lambdas compile lazily to bytecode. select/update bridge to the DAG optimizer.

4. **10-Pass Optimizer** — Type inference → constant folding → SIP → factorize → predicate pushdown → filter reorder → projection pushdown → partition pruning → fusion → DCE.

5. **Custom Allocator** — Buddy allocator with slab cache. Thread-local arenas, lock-free allocation, COW ref counting. No malloc.

6. **Zero Dependencies** — Pure C17, single public header. Builds with make. ~16K lines of engine code. Embeds into any C/C++ project.

**Step 1: Add features section HTML**
**Step 2: Commit**

---

## Task 6: Landing Page — Architecture Diagram + Terminal Demo

**Files:**
- Modify: `website/index.html`

**Architecture section:**
- Embed the existing `docs/architecture.svg` or create a simplified SVG showing: `Rayfall → Lazy DAG → 10-Pass Optimizer → Fused Morsel Executor → Result`

**Terminal demo section:**
- Animated terminal showing Rayfall REPL session:
  - Frame 1: `$ ./rayforce` → banner
  - Frame 2: `(set t (table [Symbol Price Qty] ...))` → table display
  - Frame 3: `(select {from:t by: Symbol Price: (avg Price)})` → grouped result
  - Frame 4: `(pivot t 'Symbol 'Side 'Qty sum)` → pivot result
- Use same typing animation pattern as teidelum (25-60ms per char, frame-based)

**Step 1: Add architecture + terminal sections**
**Step 2: Commit**

---

## Task 7: Landing Page — CTA + Footer

**Files:**
- Modify: `website/index.html`

**CTA section:**
```
H2: Start building in 30 seconds
Terminal: make && ./rayforce
[Get Started →]  [View on GitHub]
```

**Footer:**
- `Rayforce · MIT Licensed · RayforceDB`
- Links: GitHub, Docs, License

**Step 1: Add CTA + footer**
**Step 2: Commit**

---

## Task 8: Landing Page — JavaScript

**Files:**
- Create: `website/script.js`

**Features (port from teidelum):**
- Scroll-triggered IntersectionObserver for fade-in cards
- Mobile nav toggle with hamburger
- Nav shadow on scroll + active section detection
- Scroll-to-top button
- Terminal typing animation with Rayfall REPL frames

**Step 1: Write script.js**
**Step 2: Test locally** — `python3 -m http.server 8080 -d website/`
**Step 3: Commit**

---

## Task 9: Docs Site — CSS + Layout + Index

**Files:**
- Create: `website/docs/docs.css`
- Create: `website/docs/docs.js`
- Create: `website/docs/index.html`

**Layout:** Sidebar nav (left) + content (right). Same dark theme as landing page. Sidebar has collapsible sections. Mobile: sidebar collapses to hamburger.

**Sidebar structure:**
```
Getting Started
  Quick Start
  Building from Source
Rayfall Language
  Syntax & Types
  Variables & Functions
  Control Flow
  Lambdas & VM
Data Types
  Overview
  Integers & Floats
  Strings & Symbols
  Temporal Types
  Collections
Queries
  Select & Update
  Joins
  Pivot & Window
Operations
  Math & Comparison
  String Operations
  Aggregations
  Ordering & Iteration
C API Reference
  Memory & Lifecycle
  Atoms & Vectors
  Tables & DAG
  Execution & Storage
Graph Engine
  CSR Storage
  Algorithms
  Traversal Patterns
Architecture
  DAG Pipeline
  Optimizer Passes
  Memory Model
  Morsel Execution
Storage
  Columnar Files
  Splayed Tables
  Partitions & CSV
```

**Step 1: Write docs.css (sidebar + content layout)**
**Step 2: Write docs.js (sidebar toggle)**
**Step 3: Write docs/index.html (overview page with links to all sections)**
**Step 4: Commit**

---

## Task 10: Docs — Quick Start

**Files:**
- Create: `website/docs/quick-start.html`

**Content:**
- Prerequisites (C17 compiler, make)
- Build: `make` (debug) / `make release`
- Run tests: `make test`
- Start REPL: `./rayforce`
- First steps: create a table, run a query, load CSV
- Run example: `./rayforce examples/rfl/pivot.rfl`

**Step 1: Write quick-start.html**
**Step 2: Commit**

---

## Task 11: Docs — Rayfall Language (4 pages)

**Files:**
- Create: `website/docs/rayfall-syntax.html`
- Create: `website/docs/rayfall-functions.html`
- Create: `website/docs/rayfall-control-flow.html`
- Create: `website/docs/rayfall-lambdas.html`

**rayfall-syntax.html:**
- Atoms: `42`, `3.14`, `true`, `'symbol`, `"string"`, `2024.01.15`, `09:30:00.000`
- Vectors: `[1 2 3]`, `[A B C]`
- Lists: `(list [1 2] [3 4])`
- Tables: `(table [col1 col2] (list vec1 vec2))`
- Function calls: `(fn arg1 arg2)`
- Quoting: `'symbol`, `(quote expr)`
- Comments: `; line comment`

**rayfall-functions.html:**
- All 143 builtins organized by category (arithmetic, comparison, string, aggregation, table, join, higher-order, type, date/time, I/O, control flow, list, utility, ordering, indexing)
- Each with signature, description, example

**rayfall-control-flow.html:**
- `if`, `do`, `set`, `let`, `try`/`raise`
- Conditional expressions
- Error handling

**rayfall-lambdas.html:**
- `(fn [x y] (+ x y))` syntax
- Closures and captured variables
- Lazy bytecode compilation to VM
- Self-reference for recursion: `(set fib (fn [n] ...))`
- Applying lambdas: `map`, `fold`, `scan`

**Step 1-4: Write each page**
**Step 5: Commit all 4**

---

## Task 12: Docs — Data Types (2 pages)

**Files:**
- Create: `website/docs/data-types.html`
- Create: `website/docs/data-types-collections.html`

**data-types.html:**
- Type hierarchy table: RAY_BOOL(1), RAY_U8(2), RAY_I16(3), RAY_I32(4), RAY_I64(5), RAY_F64(6), RAY_SYM(7), RAY_STR(13), RAY_DATE(8), RAY_TIME(9), RAY_TIMESTAMP(10), RAY_GUID(11)
- Each type: storage size, null value, examples, cast behavior
- The `ray_t` 32-byte header explained (nullmap, type, attrs, refcount, len)

**data-types-collections.html:**
- Vectors: typed arrays, morsel iteration, null bitmaps
- Lists: boxed heterogeneous collections
- Tables: schema + column vectors, column access
- Dictionaries: key-value pairs via RAY_ATTR_DICT
- Selection bitmaps: RAY_SEL for lazy filtering

**Step 1-2: Write each page**
**Step 3: Commit**

---

## Task 13: Docs — Queries (3 pages)

**Files:**
- Create: `website/docs/queries-select.html`
- Create: `website/docs/queries-joins.html`
- Create: `website/docs/queries-pivot.html`

**queries-select.html:**
- `(select {from: t col: (expr)})` — projection
- `(select {from: t where: (pred)})` — filtering
- `(select {from: t by: key col: (agg col)})` — group-by with aggregation
- `(update {from: t col: (expr)})` — column mutation
- `(insert t (table ...))`, `(upsert t ...)`, `(delete t (where ...))`

**queries-joins.html:**
- `(inner-join t1 t2 [keys])` — equi-join
- `(left-join t1 t2 [keys])` — left outer join
- `(asof-join t1 t2 [keys] time-col)` — time-series alignment
- `(window-join t1 t2 [keys] time-col)` — window join
- How joins compile to DAG: radix-partitioned hash join

**queries-pivot.html:**
- `(pivot table index_col pivot_col value_col agg_fn)` — wide format
- Single-pass DAG implementation (OP_PIVOT)
- Multi-index pivot
- Window functions: ROW_NUMBER, RANK, DENSE_RANK, NTILE, LAG, LEAD

**Step 1-3: Write each page**
**Step 4: Commit**

---

## Task 14: Docs — Operations (2 pages)

**Files:**
- Create: `website/docs/operations-math.html`
- Create: `website/docs/operations-string.html`

**operations-math.html:**
- Arithmetic: +, -, *, /, %, neg, abs, sqrt, round, floor, ceil
- Comparison: ==, !=, <, <=, >, >=
- Logic: and, or, not
- Aggregation: sum, count, avg, min, max, med, dev, first, last, distinct
- Ordering: asc, desc, iasc, idesc, xasc, xdesc
- Iteration: til, take, drop, reverse, where, find, within, xbar, xrank
- Higher-order: map, fold, scan, apply, filter, pmap

**operations-string.html:**
- concat, upper, lower, strlen, substr, replace, trim, like, ilike, split, format
- RAY_SYM vs RAY_STR semantics
- String pool internals (inline SSO ≤12 bytes, pool for longer)
- Null propagation rules

**Step 1-2: Write each page**
**Step 3: Commit**

---

## Task 15: Docs — C API Reference (2 pages)

**Files:**
- Create: `website/docs/c-api-core.html`
- Create: `website/docs/c-api-dag.html`

**c-api-core.html:**
- Lifecycle: `ray_heap_init`, `ray_heap_destroy`, `ray_sym_init`, `ray_sym_destroy`
- Memory: `ray_alloc`, `ray_free`, `ray_retain`, `ray_release`, `ray_cow`
- Atoms: all constructors with examples
- Vectors: `ray_vec_new`, `ray_vec_append`, `ray_vec_set`, `ray_vec_get`, `ray_vec_slice`, `ray_vec_concat`, `ray_vec_from_raw`
- String vectors: `ray_str_vec_append`, `ray_str_vec_get`, `ray_str_vec_set`
- Lists: `ray_list_new`, `ray_list_append`, `ray_list_get`
- Tables: `ray_table_new`, `ray_table_add_col`, `ray_table_get_col`, `ray_table_ncols`, `ray_table_nrows`
- Each function: signature, description, code example, error behavior

**c-api-dag.html:**
- Graph construction: `ray_graph_new`, `ray_scan`, `ray_const_*`
- Unary/binary ops: `ray_neg`, `ray_add`, `ray_eq`, `ray_like`, ...
- Aggregations: `ray_sum`, `ray_count`, `ray_avg`, `ray_group`, `ray_distinct`
- Structural: `ray_filter`, `ray_sort_op`, `ray_join`, `ray_asof_join`, `ray_head`, `ray_tail`
- Window: `ray_window_op`
- Graph: `ray_expand`, `ray_var_expand`, `ray_shortest_path`, `ray_wco_join`
- Optimizer + executor: `ray_optimize`, `ray_execute`
- Storage: `ray_col_save`, `ray_col_load`, `ray_splay_save`, `ray_read_csv`
- CSR: `ray_rel_build`, `ray_rel_from_edges`, `ray_rel_save`, `ray_rel_load`, `ray_rel_mmap`

**Step 1-2: Write each page**
**Step 3: Commit**

---

## Task 16: Docs — Graph Engine (2 pages)

**Files:**
- Create: `website/docs/graph-storage.html`
- Create: `website/docs/graph-algorithms.html`

**graph-storage.html:**
- CSR format explained (forward + reverse indices)
- Building from edge lists: `ray_rel_from_edges`
- Persistence: save/load/mmap `.col` files
- Memory layout: `ray_csr_t`, `ray_rel_t`
- Double-indexed CSR for bidirectional traversal

**graph-algorithms.html:**
- All 22 opcodes organized by category:
  - **Traversal**: EXPAND, VAR_EXPAND, DFS, RANDOM_WALK
  - **Shortest path**: SHORTEST_PATH, DIJKSTRA, ASTAR, K_SHORTEST
  - **Centrality**: PAGERANK, DEGREE_CENT, BETWEENNESS, CLOSENESS
  - **Community**: LOUVAIN, CONNECTED_COMP, CLUSTER_COEFF
  - **Joins**: WCO_JOIN (Leapfrog TrieJoin)
  - **Spanning**: MST (Kruskal)
  - **Similarity**: COSINE_SIM, EUCLIDEAN_DIST, KNN, HNSW_KNN
  - **Ordering**: TOPSORT
- Each algorithm: use case, complexity, DAG construction example, Rayfall example
- SIP optimization for graph queries
- Factorized execution (avoid cross-product materialization)

**Step 1-2: Write each page**
**Step 3: Commit**

---

## Task 17: Docs — Architecture (2 pages)

**Files:**
- Create: `website/docs/architecture-pipeline.html`
- Create: `website/docs/architecture-memory.html`

**architecture-pipeline.html:**
- The 3-phase pipeline: Build → Optimize → Execute
- DAG node structure: `ray_op_t` (32 bytes), `ray_op_ext_t` (extended nodes)
- Optimizer passes in detail (all 10)
- Morsel-driven execution: 1024-element chunks, register slots, computed goto
- Fusion: how adjacent element-wise ops merge into single-pass bytecode
- Parallelism: thread pool, morsel dispatch, radix-partitioned hash join

**architecture-memory.html:**
- `ray_t` block layout (32-byte header + data)
- Buddy allocator: order-based free lists, coalescing
- Slab cache: 5 orders for common sizes, O(1) alloc/free
- Thread-local heaps: `heap_id`, atomic bitmap allocation
- Cross-heap deferred free: lock-free LIFO
- Arena allocator: `ray_arena_t`, `RAY_ATTR_ARENA` flag
- COW ref counting: `ray_retain`, `ray_release`, `ray_cow`

**Step 1-2: Write each page**
**Step 3: Commit**

---

## Task 18: Docs — Storage

**Files:**
- Create: `website/docs/storage.html`

**Content:**
- Columnar `.col` files: format, save/load/mmap
- Splayed tables: directory-per-table, one file per column
- Date-partitioned tables: `YYYY.MM.DD/` directories
- CSV I/O: `ray_read_csv`, parallel parse, type inference, null handling
- Symbol table persistence: append-only, file locking, save/load

**Step 1: Write storage.html**
**Step 2: Commit**

---

## Task 19: Update README.md

**Files:**
- Modify: `README.md`

**Changes:**
- Update build instructions: `make` / `make release` / `make test` (not cmake)
- Add Rayfall REPL examples alongside C examples
- Add link to website/docs
- Update test count (563 tests)
- Add "Heritage" section: "Rayforce2 unifies Teide (columnar engine) and Rayforce (graph engine)"
- Update project structure for current directory layout

**Step 1: Update README.md**
**Step 2: Commit**

---

## Task 20: Final Integration + Push

**Steps:**
1. Test all HTML pages locally: `python3 -m http.server 8080 -d website/`
2. Verify all internal links work
3. Verify responsive layout on mobile viewport
4. Run `make test` to ensure no code changes broke anything
5. Final commit + push to trigger GH Pages deployment

```bash
git push origin master
```

---

## Execution Order

Tasks 1-2 (scaffolding + CI) → Task 3 (CSS) → Tasks 4-8 (landing page) → Task 9 (docs layout) → Tasks 10-18 (docs pages) → Task 19 (README) → Task 20 (integration)

Total: ~25 HTML files, ~1 CSS file, ~1 JS file, ~1 workflow YAML, ~1 README update.

Estimated effort: Tasks are independent once scaffolding is done. Docs pages (Tasks 10-18) can be parallelized with subagents.
