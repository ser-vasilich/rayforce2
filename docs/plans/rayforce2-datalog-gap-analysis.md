# Rayforce2 Datalog: Gap Analysis for Exomem Integration

**Author:** theaspirational (teide-exomem maintainer)
**Date:** 2026-04-02
**Context:** Assessing what Rayforce2's Datalog layer needs in order to support teide-exomem — a temporal Datalog knowledge base with provenance, confidence scoring, and MCP server integration — which currently runs on a forked teide/teide-rs with a custom `src/datalog/` module (~1600 lines C, ~230 lines Rust FFI).

---

## 1. Background

### What teide-exomem does

teide-exomem is a persistent, temporal Datalog knowledge base exposed as an MCP (Model Context Protocol) server. AI agents assert facts, load rule programs, trigger evaluation, query results, and inspect derivation provenance — all via tool calls. The system supports:

- **Stratified semi-naive evaluation** with negation
- **Temporal builtins** (before, duration\_since, overlaps, meets, decay) and MTL operators
- **Provenance tracking** — which rule derived each tuple, used to build proof trees
- **Confidence scoring** derived from provenance chains
- **Persistent columnar storage** via splayed tables + symbol table round-tripping
- **Dual evaluator** — C engine for eligible programs, Rust fallback for complex builtins

### What the fork added to TeideDB/teide

The fork (`feature/datalog-ops` branch, ~4800 lines across teide + teide-rs) added:

- **`src/datalog/datalog.c` + `datalog.h`** — standalone Datalog engine: program lifecycle, rule builder, stratification, semi-naive fixpoint, 3 builtins, provenance, expression AST
- **`OP_UNION_ALL` (102), `OP_ANTIJOIN` (103)** — new executor opcodes
- **`td_table_insert_row`, `td_table_count`, `td_table_empty`** — table manipulation helpers
- **Rust FFI** — `DlProgram`, `DlRule`, `DlExpr` safe wrappers, column allocators, symbol persistence, splayed table save/load

Full inventory: see `datalog-feature-upstream-notes.md`.

### How teide-exomem uses it

teide-exomem builds Datalog programs **programmatically at runtime** from parsed AST:

```
parse(".dl source") -> Program AST
                          |
              for each rule in program:
                DlRule::new(head_pred, arity)
                  .head_var(0, idx)
                  .add_atom("pred", 2)        // DL_POS
                  .add_neg("other", 2)         // DL_NEG
                  .add_cmp(CMP_GT, x, y)       // DL_CMP
                  .add_assign(z, expr)          // DL_ASSIGN
                  .add_builtin(BEFORE, 3)       // DL_BUILTIN
                  .add_interval(f, s, e)        // DL_INTERVAL
                          |
              prog.add_edb("relation", table, arity)  // arbitrary-arity tables
              prog.add_rule(&rule)
              prog.stratify()
              prog.eval()
              prog.query("derived") -> Table
              prog.get_provenance_vec("derived") -> Vec<i64>
```

This is not a REPL workflow — it's an embedded reasoning engine inside a long-lived server process.

---

## 2. What Rayforce2's Datalog can do today

Based on the current implementation in `src/lang/eval.c` (lines 10766–12081) and the published docs at `ng.rayforcedb.com/docs/datalog.html`:

| Capability | Status |
|-----------|--------|
| EAV triple storage (`datoms`, `assert-fact`, `scan-eav`) | Done |
| Rules with `?`-prefixed variables, shared-variable joins | Done |
| `(query db (find ...) (where ...))` syntax | Done |
| OR semantics via multiple rule heads | Done |
| Recursive rules with semi-naive fixpoint (max 1000 iter) | Done |
| Pull queries for entity-centric retrieval | Done |
| Compilation to DAG executor (same passes as `select`) | Done |
| `OP_ANTIJOIN` (opcode 78) | Done |
| `union-all`, `table-distinct` eval builtins | Done |

This is a solid query convenience layer. The fixpoint algorithm is the same semi-naive approach used in the fork. The DAG compilation is clean and performant.

---

## 3. Gap analysis

### 3.1 Hard blockers — engine cannot express these

#### No stratification

Rules go into a global `dl_rules[64]` array. Recursion is detected per-rule and routed to the fixpoint loop. There is no dependency graph, no stratum assignment, and no check for negation cycles.

**Why it matters:** Any program with stratified negation (common pattern: derive X, then derive not-X in a higher stratum) will silently produce wrong results or diverge. teide-exomem relies on stratification for every non-trivial rule program.

**Reference implementation:** `teide/src/datalog/datalog.c:337-416` — topological sort over negation dependencies, ~80 lines.

#### No provenance tracking

The engine has zero derivation tracking. There is no record of which rule derived which tuple.

**Why it matters:** teide-exomem's `explain` tool builds recursive proof trees from provenance data. Confidence scoring is derived from provenance chains. Without it, the core value proposition of the knowledge base (inspectable, explainable reasoning) is lost.

**Reference implementation:** `teide/src/datalog/datalog.c:1316-1364` — post-fixpoint rule attribution pass, ~50 lines. Stores an i64 column of rule indices per IDB relation.

#### No assignment in rule bodies (`DL_ASSIGN`)

Clause classification (`eval.c:11137-11163`) recognizes 3 kinds: triple pattern, rule invocation, filter. There is no `X = expr` form.

**Why it matters:** Rules like "derive `distance(X, Y, D)` where `D = |T2 - T1|`" require computed columns. teide-exomem uses assignments for temporal arithmetic and derived attributes.

#### No builtins in rule bodies (`DL_BUILTIN`)

Rayfall builtins exist as top-level functions but cannot appear as body literals inside `(rule ...)` or `(query ... (where ...))`.

**Why it matters:** All temporal reasoning — `before(S, E, T)`, `duration_since(T1, T2, D)`, `abs(X, Y)` — is expressed as rule-body builtins. This is the core of teide-exomem's temporal logic.

**Reference implementation:** `teide/src/datalog/datalog.c:507-577` — 3 builtins, ~70 lines each. Extensible dispatch via builtin ID.

#### No expression AST in rule comparisons

Filters only support `(op ?var const)` — no `(> (+ ?x ?y) ?z)`, no compound expressions.

**Why it matters:** teide-exomem uses expression-based comparisons for computed guards. The fork has `DL_CMP_EXPR` with a full `dl_expr_t` AST (const, var, binop nodes).

**Reference implementation:** `teide/src/datalog/datalog.c:432-487` — expression evaluator, ~55 lines.

### 3.2 Structural mismatches — workaroundable but costly

#### EAV-only base facts

All base fact scans are hardcoded to `ray_scan_eav_fn` (`eval.c:11172-11177`). Unknown predicates return an error (`eval.c:11377`). There is no custom base relation registry.

**Why it matters:** teide-exomem uses arbitrary-arity relations (2-ary through 6-ary). Forcing everything through EAV triples means:
- ~Nx data volume increase (one triple per attribute per entity)
- Join explosion for multi-attribute patterns
- Complete rewrite of the codec and column-store persistence layer

**Two options:**
1. Add a base relation registry (a `ray_dl_base_t bases[64]` parallel to `dl_rules[]`) that maps predicate names to tables, bypassing EAV scan for registered relations
2. Keep EAV but accept the performance and complexity cost

Option 1 is strongly recommended — it's a small addition (~50 lines) with large payoff.

#### No temporal interval support (`DL_INTERVAL`)

No equivalent of `F @[S, E]` — binding a fact's temporal interval into start/end variables.

**Why it matters:** teide-exomem stores temporal intervals per fact and exposes them to rules via a special `__intervals(fact_id, start, end)` EDB table + interval bind syntax. This is used for temporal window queries and Allen interval algebra.

**Workaround:** Model as a regular 3-column base relation (if arbitrary-arity base relations are supported).

#### No negation syntax

The docs mapping table mentions `(not ...) -> ray_antijoin`, but the clause classifier has no handler for it. Antijoin is only used internally by the fixpoint loop for delta computation.

**Why it matters:** Explicit negation in rule bodies is fundamental to Datalog with stratified negation. Without syntax support, users cannot express "derive X where Y holds and Z does not."

**Fix:** Add a 4th clause kind to `dl_classify_clause()` for `(not (pred ?args...))`. The antijoin machinery already exists — this is wiring, not new logic.

### 3.3 Integration blockers — needed for persistent knowledge base

| Gap | Description | Effort |
|-----|-------------|--------|
| No `ray_sym_save` / `ray_sym_load` | Symbol table doesn't round-trip across restarts | Small — serialize the intern table to file |
| No splayed table save/load API | No equivalent of `td_splay_save` / `td_splay_load` | Medium — columnar persistence format |
| No retract-fact | Cannot remove triples from datoms | Small — filter + rebuild, or mark deleted |
| Global rule state (max 64, append-only) | Must `ray_lang_destroy()` + `ray_lang_init()` to reset, which nukes all env state | Medium — add `ray_dl_rules_reset()` or scoped rule sets |

---

## 4. Approaches

### 4A: Port the Datalog module into Rayforce2

Take `teide/src/datalog/datalog.c` (1629 lines) and adapt it to Rayforce2's data structures:

- `td_t*` -> `ray_t*`
- `td_sym_intern` -> `ray_sym_intern`
- `td_graph_t` / `td_op_t` -> `ray_graph_t` / `ray_op_t`
- `td_join` / `td_antijoin` / `td_union_all` -> `ray_join` / `ray_antijoin` / `ray_union_all`
- `td_vec_*` -> `ray_vec_*`

**What you get:**
- Full stratification with cycle detection
- 6 body literal types (POS, NEG, CMP, ASSIGN, BUILTIN, INTERVAL)
- Expression AST evaluation
- Provenance tracking
- Pluggable builtin dispatch
- Arbitrary-arity EDB tables
- Programmatic C API usable from FFI

**What you also get (for free):**
- All of this compiles to Rayforce's existing DAG executor — it's the same graph-building pattern
- The module is self-contained — 1 header, 1 source file, no additional dependencies
- Can be exposed through Rayfall syntax as well (wrap the C API in special forms)

**Integration with existing Rayfall Datalog:**
The current `(rule ...)` / `(query ...)` forms could be kept as sugar that internally calls the ported module, or replaced entirely.

**Effort estimate:** ~2 weeks. Most work is mechanical API translation. The logic (stratification algorithm, fixpoint loop, expression evaluator, provenance pass) transfers unchanged.

**Files to create/modify:**
- Create: `src/datalog/datalog.c`, `src/datalog/datalog.h` (port from teide)
- Modify: `src/ops/ops.h` — already has `OP_ANTIJOIN`, may need opcode adjustments
- Modify: `src/lang/eval.c` — wire Rayfall forms to the new module
- Modify: `Makefile` — add `src/datalog/datalog.o`

### 4B: Extend Rayforce2's eval.c inline

Add the missing features directly into the existing Rayfall evaluator without introducing a separate module:

1. **Stratification** (~150 lines)
   - Build predicate dependency graph from `dl_rules[]`
   - Topological sort with negation-edge detection
   - Evaluate strata in order
   - Port from `teide/src/datalog/datalog.c:337-416`

2. **Extended clause classifier** (~100 lines)
   - Add `DL_NEG` (negation): `(not (pred ?args...))` -> antijoin
   - Add `DL_ASSIGN` (assignment): `(= ?var expr)` -> computed column
   - Add `DL_BUILTIN` (builtin predicate): `(before ?s ?e ?t)` -> temporal filter

3. **Expression evaluation in filters** (~100 lines)
   - Support `(> (+ ?x ?y) ?z)` compound expressions
   - Recursive AST evaluation against intermediate tables

4. **Provenance tracking** (~200 lines)
   - After fixpoint, re-run each rule to attribute tuples to rule indices
   - Store as parallel i64 column per IDB relation
   - Expose via `(provenance db pred)` form

5. **Base relation registry** (~50 lines)
   - `ray_dl_base_t bases[64]` parallel to `dl_rules[]`
   - New clause kind: when predicate matches a registered base, scan that table
   - Enables arbitrary-arity EDB without forcing EAV decomposition

6. **Negation wiring** (~30 lines)
   - 4th case in `dl_classify_clause()` for `(not ...)`
   - Route to existing `ray_antijoin` machinery

**Effort estimate:** ~2 weeks. More invasive (touches eval.c in multiple places) but keeps everything in the Rayfall paradigm with no new module.

### 4C: Hybrid — minimal engine extensions + Rayfall emission from Rust

A middle ground: add only the critical engine features to Rayforce2, then have teide-exomem construct Rayfall source strings and evaluate via `ray_eval_str()` FFI.

**Engine additions needed:**
- Stratification (must be engine-level)
- Base relation registry (must be engine-level)
- Negation clause support (must be engine-level)
- Provenance (must be engine-level)
- `ray_sym_save` / `ray_sym_load` (must be engine-level)

**teide-exomem side:**
- Replace DlRule builder calls with Rayfall string construction
- Replace `prog.add_edb()` with `ray_env_set()` via FFI to inject tables
- Replace `prog.query()` with `ray_eval_str("(query ...)")` + result extraction

**Builtins / assignments / expressions:** Implement as Rayfall-native forms rather than Datalog body literal types. For example, assignments become `(= ?var (+ ?x ?y))` in the where clause, builtins become `(before ?s ?e ?t)` as a new clause kind.

**Effort estimate:** ~2 weeks engine-side + ~1 week teide-exomem rewrite. More total work but keeps the Rust side using the standard Rayfall interface rather than a custom C API.

---

## 5. Recommendation

**4A (port the module) is the cleanest path** for both sides:

- For Rayforce2: you get a production-tested Datalog engine with stratification, provenance, and extensible builtins — features that align with the design vision in `docs/plans/2026-04-02-datalog-design.md` (which already planned for negation via antijoin and semi-naive fixpoint). The module is self-contained and doesn't pollute eval.c.
- For teide-exomem: minimal rewrite — just swap `teide::` FFI calls for `rayforce::` equivalents in the existing engine.rs. The programmatic C API surface stays the same.
- For the community: Rayforce2 gains a real Datalog engine comparable to Souffle or Datafrog, not just a query sugar layer.

The fork's `datalog.c` was deliberately written as a standalone module with a clean header API precisely to make upstreaming feasible. The port is mostly mechanical `td_` -> `ray_` renaming with no algorithmic changes.

---

## 6. Reference: feature-level comparison matrix

| Feature | Teide fork | Rayforce2 current | Needed by exomem |
|---------|-----------|-------------------|-----------------|
| Semi-naive fixpoint | Yes | Yes | Yes |
| OR semantics (multi-head) | Yes | Yes | Yes |
| Antijoin operator | Yes (OP 103) | Yes (OP 78) | Yes |
| Union-all operator | Yes (OP 102) | Yes (eval-level) | Yes |
| Stratification | Yes | **No** | **Yes** |
| Provenance | Yes | **No** | **Yes** |
| Negated body atoms | Yes (DL_NEG) | **No** (syntax absent) | **Yes** |
| Comparison (var-var) | Yes (DL_CMP) | Partial (simple filters) | Yes |
| Comparison (expr-expr) | Yes (DL_CMP_EXPR) | **No** | Yes |
| Assignment | Yes (DL_ASSIGN) | **No** | **Yes** |
| Builtins in rules | Yes (3 impl + 3 reserved) | **No** | **Yes** |
| Interval bind | Yes (DL_INTERVAL) | **No** | Yes |
| Arbitrary-arity EDB | Yes | **No** (EAV only) | **Yes** |
| Expression AST | Yes (dl_expr_t) | **No** | **Yes** |
| Pull queries | No | Yes | No |
| Symbol persistence | Yes (sym_save/load) | **No** | **Yes** |
| Splayed table storage | Yes (td_splay_save) | **No** | **Yes** |
| Retraction | Yes (via exomem) | **No** | **Yes** |
| Programmatic C API | Yes (dl_program_*) | **No** (Rayfall only) | **Yes** |
| Rule limit | 128 | 64 | 128 preferred |
| Max body literals | 16 | 32 | 16 sufficient |
| Max arity | 16 | 16 | 16 sufficient |

**Bold** = gap that blocks teide-exomem integration.

---

## Appendix: key file references

### Teide fork (feature/datalog-ops)
- `teide/src/datalog/datalog.h` — public API header (243 lines)
- `teide/src/datalog/datalog.c` — full implementation (1629 lines)
- `teide/test/test_datalog.c` — 11 test cases
- `teide-rs/src/datalog.rs` — safe Rust wrapper (231 lines)
- `teide-rs/src/ffi.rs` — FFI bindings including `datalog_ffi` module

### Rayforce2
- `src/lang/eval.c:10766-12081` — current Datalog implementation
- `src/lang/eval.c:11035-11044` — rule storage (`dl_rules[64]`)
- `src/lang/eval.c:11137-11163` — clause classifier (3 kinds only)
- `src/lang/eval.c:11446-11613` — semi-naive fixpoint loop
- `src/ops/ops.h:78` — OP_ANTIJOIN
- `docs/plans/2026-04-02-datalog-design.md` — original design plan

### teide-exomem (consumer)
- `src/eval/engine.rs:528-629` — rule builder (the code that would need to change)
- `src/eval/engine.rs:410-449` — EDB table registration
- `src/eval/engine.rs:919-1017` — builtin wiring
- `src/eval/codec.rs` — value encoding (i64 <-> Value)
- `src/eval/provenance.rs` — derivation tracking
- `src/eval/cstore.rs` — persistent column store
- `src/eval/seminaive.rs` — dual evaluator dispatch
