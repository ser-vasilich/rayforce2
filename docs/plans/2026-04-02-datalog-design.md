# Datalog Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add Datalog rule-based query support to Rayforce — Datomic-style `(rule ...)`, `(query ...)`, and `(pull ...)` forms that compile to the existing DAG executor for vectorized columnar execution.

**Architecture:** Three layers: (1) engine primitives (antijoin, union-all), (2) EAV triple storage on top of Rayforce tables, (3) Datalog compiler that translates rules + queries into `ray_graph_t` DAG nodes and runs a semi-naive fixpoint loop using existing `ray_execute()`.

**Tech Stack:** Pure C17, extends existing Rayfall language in `src/lang/eval.c`, adds DAG opcodes in `src/ops/`, no new dependencies.

---

## Phase 1: Engine Primitives

### Task 1: OP_ANTIJOIN — Anti-Semi-Join

The antijoin keeps rows from the left table that have NO match in the right table on the join keys. Essential for: (a) computing deltas in fixpoint loop (new facts not already known), (b) Datalog negation `(not ...)`.

**Files:**
- Modify: `src/ops/ops.h` — add `#define OP_ANTIJOIN 78`, add to `ray_op_ext_t` join union member
- Modify: `src/ops/graph.c` — add `ray_antijoin()` graph builder, add to `graph_fixup_ext_ptrs`
- Modify: `src/ops/exec.c` — add `exec_antijoin()` (reuse hash join build side, probe with no-match emit)
- Modify: `src/ops/dump.c` — add `"ANTIJOIN"` name
- Create: test in `test/test_exec.c` or `test/test_lang.c`

**Implementation notes:**
- Reuse the existing radix-partitioned hash join infrastructure from `exec_join`
- Build hash table from right side, probe left side, emit rows with NO match
- `join_type` in ext: use value 3 for antijoin (0=inner, 1=left, 2=full, 3=anti)
- Actually simpler: create dedicated `OP_ANTIJOIN` opcode to keep it clean

**Step 1:** Add opcode + ext struct + graph builder
**Step 2:** Implement `exec_antijoin` in exec.c
**Step 3:** Add Rayfall builtin `(antijoin t1 t2 [keys])`
**Step 4:** Write test: `(antijoin (table [x] (list [1 2 3])) (table [x] (list [2 3 4])) ['x])` → `[1]`
**Step 5:** Commit

---

### Task 2: Table Union-All (DAG-level)

Concatenate two tables row-wise. Needed for: combining results from multiple rule clauses (OR), and accumulating facts in fixpoint loop.

**Files:**
- Modify: `src/lang/eval.c` — add `ray_table_concat_fn` builtin
- Test with Rayfall

**Implementation notes:**
- For each column in left table, concat with matching column from right table
- Use existing `ray_vec_concat` for typed vectors
- For RAY_STR columns, iterate and `ray_str_vec_append` each element
- Schema must match (same column names and types)
- Register as `(table-concat t1 t2)` or `(union-all t1 t2)` builtin

**Step 1:** Implement `ray_table_concat_fn` in eval.c
**Step 2:** Test: `(union-all (table [x] (list [1 2])) (table [x] (list [3 4])))` → 4 rows
**Step 3:** Commit

---

### Task 3: Table Distinct (eval-level)

Remove duplicate rows from a table. Needed for: deduplication after union-all in fixpoint loop.

**Files:**
- Modify: `src/lang/eval.c` — add `ray_table_distinct_fn` builtin

**Implementation notes:**
- Already have `ray_distinct` at DAG level for GROUP BY (no aggs = distinct)
- For eval-level: build a DAG with all columns as GROUP keys, execute
- Register as `(table-distinct t)` builtin

**Step 1:** Implement using DAG GROUP with all columns as keys
**Step 2:** Test
**Step 3:** Commit

---

## Phase 2: EAV Triple Storage

### Task 4: Datoms Table Schema

Define the EAV (Entity-Attribute-Value) triple storage format on top of Rayforce tables.

**Design:**
```
; A datoms table has 4 columns:
; entity (i64), attribute (sym), value (i64/f64/sym/str), tx (i64)
;
; Example:
; (set db (table [e a v tx]
;   (list [1 1 1 2 2 3]
;         [name name dept salary dept title]
;         [Alice Bob IT 75000 HR "Senior"]
;         [1 1 1 1 1 1])))
```

**Files:**
- Create: `src/lang/datalog.h` — EAV constants, helper macros
- Modify: `src/lang/eval.c` — add `(datoms)` builtin to create empty datoms table
- Modify: `src/lang/eval.c` — add `(assert-fact db e a v)` to insert triples
- Modify: `src/lang/eval.c` — add `(retract-fact db e a v)` to remove triples

**Implementation notes:**
- Value column is polymorphic — store as RAY_I64 for numeric, use sym for symbols
- For mixed types, use a tagged representation or separate tables per value type
- Simpler approach: value is always symbol ID (intern everything as symbols)
- Entity IDs are auto-incrementing i64
- Transaction IDs track insertion order

**Step 1:** Define schema constants in datalog.h
**Step 2:** Implement `(datoms)` constructor
**Step 3:** Implement `(assert-fact db e a v)` — appends a triple
**Step 4:** Test roundtrip: assert facts, query them back
**Step 5:** Commit

---

### Task 5: EAV Index Scans

Fast lookups on EAV triples. The Datalog compiler needs efficient scans by attribute, by entity, and by (entity, attribute).

**Files:**
- Modify: `src/lang/eval.c` — add `(scan-eav db attr)` and `(scan-eav db entity attr)` builtins

**Implementation notes:**
- `(scan-eav db 'name)` → filter datoms where a == 'name, return [e, v] table
- `(scan-eav db 42 'name)` → filter datoms where e == 42 AND a == 'name, return value
- Compile to DAG: `ray_scan("a") → ray_filter(== attr) → ray_select([e, v])`
- These are the primitives that Datalog pattern matching compiles to

**Step 1:** Implement scan-eav builtins
**Step 2:** Test with asserted facts
**Step 3:** Commit

---

## Phase 3: Datalog Compiler

### Task 6: Rule Definition — `(rule ...)`

Parse and store Datalog rules. Rules define derived relations.

**Syntax:**
```lisp
(rule (selected ?a)           ; head: relation name + variables
  (?a :assertion/name ?n)     ; body clause 1: triple pattern
  (_ :select_assertion/assertion ?a))  ; body clause 2
```

**Files:**
- Create: `src/lang/datalog.c` — rule storage, pattern matching structures
- Create: `src/lang/datalog.h` — types for rules, patterns, variables
- Modify: `src/lang/eval.c` — register `rule` as special form
- Modify: `Makefile` — add datalog.o

**Data structures:**
```c
typedef struct {
    int64_t  name_sym;      /* head relation name (interned symbol) */
    uint8_t  n_head_vars;   /* number of variables in head */
    int64_t* head_vars;     /* variable symbol IDs */
    uint8_t  n_body;        /* number of body clauses */
    ray_dl_clause_t* body;  /* array of body clauses */
} ray_dl_rule_t;

typedef struct {
    enum { DL_TRIPLE, DL_RULE_INVOKE, DL_FILTER, DL_NOT } type;
    union {
        struct { int64_t entity, attr, value; bool e_var, a_var, v_var; } triple;
        struct { int64_t name; uint8_t n_args; int64_t* args; } invoke;
        struct { ray_t* expr; } filter;
        struct { ray_dl_clause_t* inner; uint8_t n_inner; } negation;
    };
} ray_dl_clause_t;
```

**Step 1:** Define types in datalog.h
**Step 2:** Parse `(rule head body...)` in eval.c, store in global rule table
**Step 3:** Test: define a rule, verify it's stored
**Step 4:** Commit

---

### Task 7: Query Compilation — `(query ...)`

Compile a Datalog query into a `ray_graph_t` DAG.

**Syntax:**
```lisp
(query
  (find ?a ?n)                   ; output variables
  (where
    (selected ?a)                ; rule invocation
    (?a :assertion/name ?n)))    ; triple pattern
```

**Compilation strategy:**
1. For each triple pattern `(?e :attr ?v)` → `ray_scan(datoms, "a") → ray_filter(== attr) → ray_select([e_col, v_col])`
2. For each rule invocation `(selected ?a)` → inline the rule's compiled body as a subgraph
3. For shared variables between clauses → `ray_join` on the shared column
4. For `(find ?a ?n)` → `ray_select` to project output columns
5. For `(not ...)` → `ray_antijoin`

**Files:**
- Modify: `src/lang/datalog.c` — add `dl_compile_query()` function
- Modify: `src/lang/eval.c` — register `query` as special form

**Step 1:** Implement triple pattern → DAG compilation
**Step 2:** Implement variable unification (shared variables → joins)
**Step 3:** Implement rule inlining (expand rule bodies as subgraphs)
**Step 4:** Test non-recursive query end-to-end
**Step 5:** Commit

---

### Task 8: Semi-Naive Fixpoint Evaluation

Handle recursive rules by iterating until no new facts appear.

**Algorithm:**
```
delta = apply_all_rules(rules, db)           // initial pass
db = union-all(db, delta)
db = table-distinct(db)

while (delta.nrows > 0) {
    new_delta = apply_rules_with_delta(rules, db, delta)
    new_delta = antijoin(new_delta, db)      // keep only NEW rows
    new_delta = table-distinct(new_delta)
    db = union-all(db, new_delta)
    delta = new_delta
}
return db
```

**Files:**
- Modify: `src/lang/datalog.c` — add `dl_fixpoint()` function
- Uses: `ray_execute`, `ray_antijoin`, `table-concat`, `table-distinct`

**Implementation notes:**
- Semi-naive optimization: each iteration only processes delta (new facts from last round)
- For each rule, generate variant where ONE body atom scans delta instead of full db
- The fixpoint loop is ~50 lines of C calling existing Rayforce primitives
- Termination guaranteed for stratified Datalog (no recursion through negation)

**Step 1:** Implement basic fixpoint loop (naive — full rescan each iteration)
**Step 2:** Add semi-naive optimization (delta-based)
**Step 3:** Test with recursive transitive closure: `(rule (path ?x ?y) (?x :edge ?y))` + `(rule (path ?x ?z) (?x :edge ?y) (path ?y ?z))`
**Step 4:** Commit

---

### Task 9: Pull Queries — `(pull ...)`

Entity-centric retrieval: given an entity ID, return all its attributes as a dictionary.

**Syntax:**
```lisp
(pull db 42 *)           ; all attributes of entity 42
(pull db 42 [name dept]) ; specific attributes
```

**Files:**
- Modify: `src/lang/eval.c` — register `pull` as builtin
- Modify: `src/lang/datalog.c` — add `dl_pull()` function

**Implementation notes:**
- `(pull db ?e *)` → scan datoms where e == ?e, pivot to {attr: value} dict
- `(pull db ?e [a1 a2])` → scan + filter attrs, pivot
- Returns a Rayfall dict: `{name: "Alice" dept: "IT" salary: 75000}`
- Nested pull `(pull db ?e [* {:dept [*]}])` → recursive entity resolution

**Step 1:** Implement basic pull (all attributes)
**Step 2:** Implement selective pull (specific attributes)
**Step 3:** Test with asserted EAV data
**Step 4:** Commit

---

## Phase 4: Integration + Examples

### Task 10: End-to-End Example — Datomic Comparison

Port the exact example from the architecture document into a working `.rfl` script.

**File:** `examples/rfl/datalog.rfl`

```lisp
; Create EAV database
(set db (datoms))

; Assert facts
(set db (assert-fact db 1 'assertion/name "Main theorem"))
(set db (assert-fact db 2 'assertion/name "Lemma 1"))
(set db (assert-fact db 3 'assertion/name "1.1"))
(set db (assert-fact db 4 'assertion/name "1"))
(set db (assert-fact db 5 'select_assertion/assertion 1))
(set db (assert-fact db 6 'select_assertion/assertion 3))

; Define rules
(rule (selected ?a)
  (?a :assertion/name ?n)
  (_ :select_assertion/assertion ?a))

; Query
(println (query db
  (find ?a ?n)
  (where
    (selected ?a)
    (?a :assertion/name ?n))))
```

**Step 1:** Write the example
**Step 2:** Run it end-to-end, verify output
**Step 3:** Write recursive example (transitive closure)
**Step 4:** Commit

---

### Task 11: Architecture Doc for Website

Port the Teide Datalog architecture document to the Rayforce website, replacing all `td_*` references with `ray_*`.

**File:** `website/docs/datalog.html`

**Step 1:** Adapt the HTML, replace Teide → Rayforce, td_ → ray_
**Step 2:** Update the comparison table (Rayforce now HAS Datalog)
**Step 3:** Add to docs sidebar navigation
**Step 4:** Commit + push

---

## Execution Order

```
Phase 1: Engine Primitives (Tasks 1-3)
  ├── Task 1: OP_ANTIJOIN      ← foundation, blocks everything
  ├── Task 2: union-all        ← needed for fixpoint
  └── Task 3: table-distinct   ← needed for fixpoint

Phase 2: EAV Storage (Tasks 4-5)
  ├── Task 4: Datoms table     ← triple storage schema
  └── Task 5: EAV index scans  ← fast pattern matching

Phase 3: Datalog Compiler (Tasks 6-9)
  ├── Task 6: Rule definition   ← (rule ...) syntax
  ├── Task 7: Query compilation ← (query ...) → DAG
  ├── Task 8: Fixpoint loop    ← recursive rules
  └── Task 9: Pull queries     ← entity resolution

Phase 4: Integration (Tasks 10-11)
  ├── Task 10: End-to-end example
  └── Task 11: Website docs
```

Total estimated: ~3000-4000 lines of new C code, mostly in `src/lang/datalog.c` + `src/lang/eval.c`, plus ~200 lines in `src/ops/` for OP_ANTIJOIN.
