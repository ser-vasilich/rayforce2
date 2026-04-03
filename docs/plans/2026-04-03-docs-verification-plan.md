# Documentation Verification & Update Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Verify every code example in docs against real execution, update stale content, add missing coverage for the ported Datalog engine, and fix all broken references.

**Architecture:** For each docs page, extract code examples, run them against `./rayforce`, replace fabricated output with real output. Add new pages for undocumented features. Update examples/rfl/ scripts.

**Tech Stack:** HTML docs, Rayfall (.rfl scripts), bash for verification

---

## Audit Summary

**Critical issues found:**

| Issue | Location | Details |
|-------|----------|---------|
| Wrong GitHub URL | quick-start.html, all sidebar navs | `singaraiona/rayforce` → `RayforceDB/rayforce2` |
| Stale Datalog docs | datalog.html | Missing: stratification, provenance, programmatic API, negation, assignment, temporal builtins, arbitrary-arity EDB |
| Fabricated output | datalog.html, quick-start.html | Code examples show output never verified against real execution |
| datalog.rfl uses `show` | examples/rfl/datalog.rfl | `show` exists but output not verified; uses old inline engine output |
| Missing deep guides | website/docs/ | No Datalog tutorial, no temporal reasoning guide, no migration guide |
| Sym ID display | All Datalog examples | Values show as integers (158, 162) not names — needs `sym-name` usage shown |

---

## Task 1: Fix GitHub URLs across all docs

**Files:** All 19 `website/docs/*.html` files + `website/index.html`

**Step 1:** Replace all occurrences of `singaraiona/rayforce` with `RayforceDB/rayforce2`

```bash
find website/ -name '*.html' -exec sed -i 's|singaraiona/rayforce|RayforceDB/rayforce2|g' {} +
```

**Step 2:** Verify no stale URLs remain

```bash
grep -r 'singaraiona' website/
```

**Step 3:** Commit

```bash
git add website/ && git commit -m "fix: update GitHub URLs to RayforceDB/rayforce2 across all docs"
```

---

## Task 2: Update examples/rfl/datalog.rfl for new engine

**Files:** `examples/rfl/datalog.rfl`

**Step 1:** Rewrite the example to demonstrate:
- EAV storage (datoms, assert-fact)
- Simple query with `println` (not `show`)
- Rule definition and query via rule
- Negation `(not ...)`
- Transitive closure (recursive rules)
- Pull queries
- `sym-name` for readable output
- Retract-fact
- The new programmatic API (`dl-program`, `dl-add-edb`, etc.)

**Step 2:** Run it and capture REAL output

```bash
./rayforce examples/rfl/datalog.rfl 2>&1
```

**Step 3:** Verify output matches expectations (row counts, correct joins)

**Step 4:** Commit

---

## Task 3: Rewrite website/docs/datalog.html

**Files:** `website/docs/datalog.html`

This is a complete rewrite. The current page covers the old inline engine. The new page must cover:

**Sections:**

1. **What is Datalog?** — keep, but update "how it maps to Rayforce" table
2. **EAV Triple Storage** — keep, verify examples
3. **Rules** — update with new syntax examples, show negation `(not ...)`
4. **Queries** — update with verified output from `./rayforce`
5. **Recursive Rules** — update transitive closure example with verified output
6. **Negation** — NEW section: `(not (?e :dept 'Eng))` with antijoin explanation
7. **Stratification** — NEW section: explain strata, how negation cycles are detected
8. **Provenance** — NEW section: `(dl-provenance prog pred)`, derivation tracking
9. **Programmatic API** — NEW section: `dl-program`, `dl-add-edb`, `dl-stratify`, `dl-eval`, `dl-query`
10. **Arbitrary-Arity EDB** — NEW section: registering custom tables as base relations
11. **Assignment in Rules** — NEW section: `(= ?d (- ?t2 ?t1))` computed columns
12. **Temporal Builtins** — NEW section: `before`, `duration_since`, `abs`
13. **Complete Example** — rewrite with VERIFIED output from running the actual script
14. **sym-name for Readable Output** — NEW section: converting sym IDs to names

**CRITICAL RULE:** Every code example must be run against `./rayforce` and the output captured verbatim. Use this pattern:

```bash
cat > /tmp/doc_test_N.rfl << 'EOF'
(code here)
EOF
./rayforce /tmp/doc_test_N.rfl 2>&1
```

Copy the EXACT output into the HTML.

---

## Task 4: Verify quick-start.html examples

**Files:** `website/docs/quick-start.html`

**Step 1:** Extract each code example from the page
**Step 2:** Run each example against `./rayforce`
**Step 3:** Replace any fabricated output with real output
**Step 4:** Fix the `git clone` URL
**Step 5:** Verify build commands (`make`, `make release`, `make test`)
**Step 6:** Commit

---

## Task 5: Verify Rayfall syntax + functions docs

**Files:** `website/docs/rayfall-syntax.html`, `website/docs/rayfall-functions.html`

**Step 1:** For each code example on both pages, create a test .rfl script
**Step 2:** Run against `./rayforce` and compare output
**Step 3:** Fix any discrepancies
**Step 4:** Add missing builtins: `datoms`, `assert-fact`, `scan-eav`, `pull`, `retract-fact`, `sym-name`, `union-all`, `table-distinct`, `antijoin`, `dl-program`, `dl-add-edb`, `dl-stratify`, `dl-eval`, `dl-query`, `dl-provenance`, `not` (in Datalog context)
**Step 5:** Commit

---

## Task 6: Verify queries docs (select, joins, pivot)

**Files:** `website/docs/queries-select.html`, `website/docs/queries-joins.html`, `website/docs/queries-pivot.html`

**Step 1:** Extract and run every Rayfall code example
**Step 2:** Replace fabricated output with real output
**Step 3:** Commit

---

## Task 7: Verify operations docs (math, string)

**Files:** `website/docs/operations-math.html`, `website/docs/operations-string.html`

**Step 1:** Extract and run every Rayfall code example
**Step 2:** Replace fabricated output with real output
**Step 3:** Commit

---

## Task 8: Verify C API docs

**Files:** `website/docs/c-api-core.html`, `website/docs/c-api-dag.html`

**Step 1:** For each C code example, verify it compiles with `cc -Iinclude -Isrc -c`
**Step 2:** Fix any API mismatches (function signatures, include paths)
**Step 3:** Add Datalog C API documentation: `dl_program_new`, `dl_add_edb`, `dl_add_rule`, `dl_stratify`, `dl_eval`, `dl_query`, `dl_get_provenance`
**Step 4:** Commit

---

## Task 9: Verify architecture + graph docs

**Files:** `website/docs/architecture-pipeline.html`, `website/docs/architecture-memory.html`, `website/docs/graph-storage.html`, `website/docs/graph-algorithms.html`

**Step 1:** Verify any Rayfall examples in these pages
**Step 2:** Update architecture page to mention the Datalog engine module
**Step 3:** Commit

---

## Task 10: Verify data types + storage docs

**Files:** `website/docs/data-types.html`, `website/docs/data-types-collections.html`, `website/docs/storage.html`

**Step 1:** Verify any Rayfall examples
**Step 2:** Update storage page to mention symbol persistence (`ray_sym_save`/`ray_sym_load`)
**Step 3:** Commit

---

## Task 11: Update README with verified examples

**Files:** `README.md`

**Step 1:** Run every code example in the README against `./rayforce`
**Step 2:** Replace any stale output with real output
**Step 3:** Ensure the C example compiles
**Step 4:** Add a Datalog section with a verified example
**Step 5:** Commit

---

## Task 12: Update landing page terminal animation

**Files:** `website/script.js`

**Step 1:** Run the terminal animation's Rayfall expressions against `./rayforce`
**Step 2:** Update output strings if they've changed
**Step 3:** Verify the table box-drawing output matches character-for-character
**Step 4:** Commit

---

## Execution Order

Tasks 1-2 first (quick fixes), then Task 3 (Datalog rewrite — biggest), then Tasks 4-12 in parallel (independent verification per page).

**Parallelization:** Tasks 4-10 can each be dispatched as separate agents since they touch different files.
