# File Restructure Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Clean directory structure with one-word file names, proper layer separation, and no cross-layer pollution.

**Architecture:** Six phases of `git mv` + include-path fixups, each ending with `make test` green. The Makefile uses `$(wildcard src/*/*.c)` so moving files between directories needs zero Makefile changes — only `#include` paths need updating. eval.c (6700 lines) gets split: builtins, query bridge (select/update/join), and I/O functions move to `ops/`; only the evaluator core, VM, and registration table stay in `lang/`.

**Tech Stack:** Pure `git mv`, `sed`, `make test`

---

## Phase 1: Move runtime infrastructure from `ops/` to `core/`

Move pool, profile, and morsel — these are runtime services, not query operations.

### Task 1: Move pool to core/

**Files:**
- Move: `src/ops/pool.c` → `src/core/pool.c`
- Move: `src/ops/pool.h` → `src/core/pool.h`
- Fix includes in: all files that `#include "ops/pool.h"` or `#include "pool.h"`

**Step 1: Move files**
```bash
git mv src/ops/pool.c src/core/pool.c
git mv src/ops/pool.h src/core/pool.h
```

**Step 2: Fix include paths**

Files that include pool.h (found via dependency map):
- `src/lang/eval.c`: `"ops/pool.h"` → `"core/pool.h"`
- `src/lang/collection.c`: `"ops/pool.h"` → `"core/pool.h"`
- `src/ops/exec_internal.h`: `"pool.h"` → `"core/pool.h"`
- `src/io/csv.c`: check and fix path
- `src/core/pool.c`: internal includes — update any `"pool.h"` to use correct relative path

**Step 3: Build and test**
```bash
make clean && make test
```
Expected: 566 tests pass

**Step 4: Commit**
```bash
git add -A && git commit -m "refactor: move pool to core/"
```

### Task 2: Move profile to core/

**Files:**
- Move: `src/ops/profile.h` → `src/core/profile.h`
- Fix includes in: files that `#include "ops/profile.h"` or `#include "profile.h"`

**Step 1: Move file**
```bash
git mv src/ops/profile.h src/core/profile.h
```

**Step 2: Fix include paths**

Files that include profile.h:
- `src/lang/eval.c`: `"ops/profile.h"` → `"core/profile.h"`
- `src/io/repl.c`: `"ops/profile.h"` → `"core/profile.h"`
- `src/ops/exec_internal.h`: `"profile.h"` → `"core/profile.h"`

**Step 3: Build and test**
```bash
make clean && make test
```

**Step 4: Commit**
```bash
git add -A && git commit -m "refactor: move profile to core/"
```

### Task 3: Move morsel to core/

**Files:**
- Move: `src/ops/morsel.c` → `src/core/morsel.c`
- Move: `src/ops/morsel.h` → `src/core/morsel.h`
- Fix includes in consumers

**Step 1: Move files**
```bash
git mv src/ops/morsel.c src/core/morsel.c
git mv src/ops/morsel.h src/core/morsel.h
```

**Step 2: Fix include paths**

Check all files that include morsel.h and update paths. Morsel is included via ops.h or directly.

**Step 3: Build and test**
```bash
make clean && make test
```

**Step 4: Commit**
```bash
git add -A && git commit -m "refactor: move morsel to core/"
```

---

## Phase 2: Move REPL to app/, datalog to ops/

### Task 4: Move repl to app/

**Files:**
- Move: `src/io/repl.c` → `src/app/repl.c`
- Move: `src/io/repl.h` → `src/app/repl.h`
- Fix includes (repl.h is included by main.c and possibly term.c)

**Step 1: Move files**
```bash
git mv src/io/repl.c src/app/repl.c
git mv src/io/repl.h src/app/repl.h
```

**Step 2: Fix include paths in consumers**
- `src/app/main.c`: `"io/repl.h"` → `"app/repl.h"`
- `src/app/repl.c`: fix any internal includes

**Step 3: Build and test**
```bash
make clean && make test
```

**Step 4: Commit**
```bash
git add -A && git commit -m "refactor: move repl to app/"
```

### Task 5: Move datalog to ops/

**Files:**
- Move: `src/datalog/datalog.c` → `src/ops/datalog.c`
- Move: `src/datalog/datalog.h` → `src/ops/datalog.h`
- Remove: `src/datalog/` directory (after move)
- Fix includes in consumers

**Step 1: Move files**
```bash
git mv src/datalog/datalog.c src/ops/datalog.c
git mv src/datalog/datalog.h src/ops/datalog.h
```

**Step 2: Fix include paths**
- `src/lang/eval.c`: `"datalog/datalog.h"` → `"ops/datalog.h"`
- `src/lang/datalog_builtin.c`: `"datalog/datalog.h"` → `"ops/datalog.h"`
- `src/ops/datalog.c`: fix internal include

**Step 3: Build and test**
```bash
make clean && make test
```

**Step 4: Commit**
```bash
git add -A && git commit -m "refactor: move datalog to ops/"
```

---

## Phase 3: Rename files in ops/ — kill `_exec` suffixes and `exec_internal`

### Task 6: Rename exec_internal.h → internal.h

**Step 1: Rename**
```bash
git mv src/ops/exec_internal.h src/ops/internal.h
```

**Step 2: Fix all includes**

Every file in `src/ops/` that includes `"exec_internal.h"` or `"ops/exec_internal.h"` must change to `"internal.h"` or `"ops/internal.h"`. Files:
- `src/ops/exec.c`, `src/ops/string_exec.c`, `src/ops/group.c`, `src/ops/pivot_exec.c`,
  `src/ops/embedding_exec.c`, `src/ops/temporal_exec.c`, `src/ops/window.c`,
  `src/ops/filter.c`, `src/ops/graph_exec.c`, `src/ops/expr.c`, `src/ops/sort_exec.c`,
  `src/ops/join.c`

**Step 3: Build and test**
```bash
make clean && make test
```

**Step 4: Commit**
```bash
git add -A && git commit -m "refactor: rename exec_internal.h to internal.h"
```

### Task 7: Rename _exec files

All renames in one step — these are purely mechanical with no include changes needed (they include `internal.h` not each other):

**Step 1: Rename**
```bash
git mv src/ops/sort_exec.c src/ops/sort.c
git mv src/ops/string_exec.c src/ops/string.c
git mv src/ops/temporal_exec.c src/ops/temporal.c
git mv src/ops/pivot_exec.c src/ops/pivot.c
git mv src/ops/embedding_exec.c src/ops/embedding.c
git mv src/ops/graph_exec.c src/ops/traverse.c
```

Note: `graph_exec.c` → `traverse.c` because `graph.c` already exists (DAG construction). `traverse.c` contains graph traversal execution (BFS, shortest path, A*, etc.).

**Step 2: Build and test**
```bash
make clean && make test
```

**Step 3: Commit**
```bash
git add -A && git commit -m "refactor: drop _exec suffixes from ops/ files"
```

---

## Phase 4: Move builtin files from lang/ to ops/

These files are standalone — they include `eval_internal.h` but nothing includes them. They're just builtin function implementations.

### Task 8: Move standalone builtin files

**Step 1: Move all at once**
```bash
git mv src/lang/agg.c src/ops/agg.c
git mv src/lang/arith.c src/ops/arith.c
git mv src/lang/cmp.c src/ops/cmp.c
git mv src/lang/collection.c src/ops/collection.c
git mv src/lang/str_builtin.c src/ops/string_builtin.c
git mv src/lang/table_builtin.c src/ops/table_builtin.c
git mv src/lang/temporal.c src/ops/temporal_builtin.c
git mv src/lang/sort.c src/ops/sort_builtin.c
git mv src/lang/system.c src/ops/system.c
git mv src/lang/datalog_builtin.c src/ops/datalog_builtin.c
```

Wait — naming conflict: `src/ops/temporal.c` already exists (from task 7 rename of `temporal_exec.c`), and `src/ops/sort.c` already exists. We need to resolve these:

- `src/lang/temporal.c` (temporal builtins: date/time/timestamp constructors) → merge into `src/ops/temporal.c` (temporal executor), OR rename to `src/ops/chrono.c`
- `src/lang/sort.c` (sort builtins: asc/desc/iasc/idesc) → merge into `src/ops/sort.c` (sort executor), OR rename to `src/ops/order.c`
- `src/lang/str_builtin.c` → `src/ops/strop.c` (string operations)
- `src/lang/table_builtin.c` → `src/ops/tblop.c` (table operations)
- `src/lang/datalog_builtin.c` → merge into `src/ops/datalog.c`

Revised plan:

**Step 1: Move non-conflicting files**
```bash
git mv src/lang/agg.c src/ops/agg.c
git mv src/lang/arith.c src/ops/arith.c
git mv src/lang/cmp.c src/ops/cmp.c
git mv src/lang/collection.c src/ops/collection.c
git mv src/lang/system.c src/ops/system.c
git mv src/lang/str_builtin.c src/ops/strop.c
git mv src/lang/table_builtin.c src/ops/tblop.c
```

**Step 2: Fix include paths in moved files**

All moved files include `"lang/eval_internal.h"` — this stays as-is since eval_internal.h remains in `lang/`. The `-Isrc` flag means `"lang/eval_internal.h"` resolves correctly from any directory.

**Step 3: Build and test**
```bash
make clean && make test
```

**Step 4: Commit**
```bash
git add -A && git commit -m "refactor: move builtin files from lang/ to ops/"
```

### Task 9: Merge conflicting builtin files

These builtins need to be appended to their corresponding executor files, since both are about the same domain:

**Step 1: Merge temporal builtins into ops/temporal.c**

Append the contents of `src/lang/temporal.c` (temporal builtin functions) to the end of `src/ops/temporal.c`. Remove the duplicate `#include` lines. Delete `src/lang/temporal.c`.

**Step 2: Merge sort builtins into ops/sort.c**

Append the contents of `src/lang/sort.c` (sort builtin functions: asc/desc/iasc/idesc/rank/xasc/xdesc) to the end of `src/ops/sort.c`. Remove duplicate includes. Delete `src/lang/sort.c`.

**Step 3: Merge datalog builtins into ops/datalog.c**

Append the contents of `src/lang/datalog_builtin.c` to the end of `src/ops/datalog.c`. Remove duplicate includes. Delete `src/lang/datalog_builtin.c`.

**Step 4: Build and test**
```bash
make clean && make test
```

**Step 5: Commit**
```bash
git add -A && git commit -m "refactor: merge builtin files into their executor counterparts"
```

---

## Phase 5: Split eval.c — extract query bridge and I/O builtins to ops/

eval.c is 6700 lines. After phases 1-4, the standalone builtin files are already moved. What remains in eval.c are large inline sections that need extracting:

### Task 10: Extract query bridge (select/update/join) to ops/query.c

**Context:** Lines ~942-3459 of eval.c contain `ray_select_fn`, `ray_update`, `ray_insert`, `ray_upsert`, all join implementations, and `ray_xbar`. These are the DAG-bridge functions that build and execute query plans.

**Step 1: Create `src/ops/query.c`**

Extract these functions from eval.c into a new file:
- `resolve_binary_dag`, `resolve_agg_opcode`, `compile_expr_dag`, `is_agg_expr` (DAG compilation helpers)
- `ray_select_fn` (select query)
- `ray_xbar` (bucketing)
- `ray_update`, `ray_insert`, `ray_upsert`
- `join_impl`, `ray_left_join`, `ray_inner_join`, `ray_antijoin_fn`, `ray_window_join`, `ray_asof_join_fn`

The new file includes `"lang/eval_internal.h"` and `"ops/ops.h"` (same as the moved builtins).

**Step 2: Add function declarations to eval.h or eval_internal.h**

Any function that was `static` in eval.c but is now called from the registration table in eval.c needs a declaration in eval_internal.h (or a new `ops/query.h`).

**Step 3: Build and test**
```bash
make clean && make test
```

**Step 4: Commit**
```bash
git add -A && git commit -m "refactor: extract query bridge from eval.c to ops/query.c"
```

### Task 11: Extract I/O builtins to ops/builtins.c

**Context:** Lines ~3460-3770 of eval.c contain `ray_println`, `ray_show`, `ray_format_fn`, `ray_read_csv_fn`, `ray_write_csv_fn`, `ray_cast_fn`, `ray_type_fn`, `ray_read_file`, `ray_load_file`, `ray_write_file`, `fmt_interpolate`, `ray_resolve_fn`, `ray_timeit_fn`, `ray_exit_fn`, and misc builtins from ~5429-6215 (`ray_enlist`, `ray_dict_fn`, `ray_nil_fn`, `ray_where_fn`, `ray_group_fn`, `ray_concat_fn`, `ray_raze_fn`, `ray_within_fn`, `ray_fdiv_fn`).

**Step 1: Create `src/ops/builtins.c`**

Move all miscellaneous builtin implementations out of eval.c. The file includes `"lang/eval_internal.h"`.

**Step 2: Add declarations in eval_internal.h**

**Step 3: Build and test**
```bash
make clean && make test
```

**Step 4: Commit**
```bash
git add -A && git commit -m "refactor: extract remaining builtins from eval.c to ops/builtins.c"
```

---

## Phase 6: Clean up eval.c — the language core

### Task 12: Verify eval.c is clean

After phases 1-5, eval.c should contain only:
- Globals and helpers (interrupt, error trace, nfo)
- `ray_raise`, `ray_try` (error/exception)
- `ray_set`, `ray_let`, `ray_cond`, `ray_do` (special forms)
- `ray_fn` (lambda creation)
- `call_lambda`, `add_error_frame`, `add_eval_error_frame` (call mechanics)
- `vm_exec` (bytecode VM)
- `ray_register_builtins` (registration table — references all builtins via extern)
- `ray_lang_init`, `ray_lang_destroy` (lifecycle)
- `ray_eval` (tree-walking evaluator)
- `ray_eval_str` (convenience)
- FN_ATOMIC helpers (`atomic_map_binary_op`, `atomic_map_unary`, etc.)

**Step 1: Verify size**
```bash
wc -l src/lang/eval.c
```
Expected: ~2000-2500 lines (down from 6700)

**Step 2: Full test**
```bash
make clean && make test
```

**Step 3: Release build test**
```bash
make clean && make release
```

**Step 4: Commit**
```bash
git add -A && git commit -m "refactor: file restructure complete"
```

---

## Final directory structure

```
src/
  app/          main.c, term.c/h, repl.c/h
  core/         platform, runtime, types, block, heap, sys, cow, arena, pool, profile, morsel
  lang/         parse, compile, eval, env, format, nfo
  ops/          ops.h, internal.h, hash.h
                graph, opt, exec, plan, dump, fuse, fvec, pipe
                group, join, filter, sort, window, pivot, expr, traverse
                string, temporal, embedding
                agg, arith, cmp, collection, strop, tblop, system
                datalog, lftj
                query, builtins
  vec/          atom, vec, list, str, sel, embedding.h
  table/        sym, table
  store/        col, csr, serde, splay, fileio, hnsw, meta, part
  io/           csv
```

## Risk notes

- **The Makefile auto-discovers `src/*/*.c`** — no Makefile changes needed for moves.
- **`#include` paths use `-Isrc`** — so `"ops/pool.h"` works from any directory under src/.
- **Static functions becoming extern** — when extracting from eval.c, functions that were `static` need declarations added to headers. Watch for naming collisions.
- **eval_internal.h stays in lang/** — the moved builtin files continue to include `"lang/eval_internal.h"`. This is correct: the builtins need the eval helpers (`make_i64`, `is_null_atom`, `collection_elem`, etc.).
- **Test after every task** — `make clean && make test` must pass 566/566 after each commit.
