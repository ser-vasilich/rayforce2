# Rayfall on Teide — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a full Rayfall language frontend to Teide — lexer, parser, eval, bytecode VM, REPL — so that `td_eval("(+ 1 2)")` works end-to-end.

**Architecture:** Rayforce-style function objects (`TD_UNARY/TD_BINARY/TD_VARY`) registered in a global environment. Parser produces `td_t` objects directly (no separate AST). Tree-walking `eval()` dispatches by function type. Lambdas compile to bytecode and run in a stack-based computed-goto VM. `select`/`update` builtins bridge to Teide's DAG executor at runtime.

**Tech Stack:** C17, munit test framework, Teide allocator (`td_alloc`/`td_arena_alloc`), no external dependencies.

**Design doc:** `docs/plans/2026-03-27-rayfall-on-teide-design.md`

**Reference codebases:**
- Rayforce parser: `/home/hetoku/data/work/rayforce/core/parse.c`
- Rayforce env/registration: `/home/hetoku/data/work/rayforce/core/env.c`
- Rayforce VM: `/home/hetoku/data/work/rayforce/core/eval.c`
- Rayforce compiler: `/home/hetoku/data/work/kdb/src/lang/compile.c`
- Rayforce lambda: `/home/hetoku/data/work/rayforce/core/lambda.h`
- kdb ASCII dispatch: `/home/hetoku/data/work/kdb/src/lang/parse.c` (lines 25-66)

**Build & test commands:**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build
cd build && ctest --output-on-failure
./build/test_teide --suite /lang
./build/test_teide --suite /lang/lex
```

---

## Phase 1: Type System Foundation

### Task 1.1: Add function type tags to td.h

**Files:** Modify `include/teide/td.h`

- [x] Add type tag defines after `TD_STR 21` (around line 128):
  - `TD_LAMBDA 100`, `TD_UNARY 101`, `TD_BINARY 102`, `TD_VARY 103`
  - `TD_ATOM_LAMBDA (-100)`, `TD_ATOM_UNARY (-101)`, `TD_ATOM_BINARY (-102)`, `TD_ATOM_VARY (-103)`
  - `TD_FN_NONE 0x00`, `TD_FN_LEFT_ATOMIC 0x01`, `TD_FN_RIGHT_ATOMIC 0x02`, `TD_FN_ATOMIC 0x04`, `TD_FN_AGGR 0x08`, `TD_FN_SPECIAL_FORM 0x10`
  - `TD_ATTR_NAME 0x20` (distinguishes symbol literal from name reference)
  - `typedef td_t* (*td_unary_fn)(td_t*)`
  - `typedef td_t* (*td_binary_fn)(td_t*, td_t*)`
  - `typedef td_t* (*td_vary_fn)(td_t**, int64_t)`
- [x] Verify build: `cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build`
- [x] Run existing tests: `cd build && ctest --output-on-failure` — all pass, no regressions
- [x] Commit: `feat(lang): add TD_UNARY/TD_BINARY/TD_VARY/TD_LAMBDA type tags and FN_* flags`

---

### Task 1.2: Function object constructors and environment

**Files:** Create `src/lang/env.h`, `src/lang/env.c`, `test/test_lang.c`; Modify `test/test_main.c`

- [x] Write `test/test_lang.c` with setup/teardown (`td_heap_init`/`td_sym_init`) and tests:
  - `test_fn_unary` — creates `TD_ATOM_UNARY` object, checks type + attrs
  - `test_fn_binary` — creates `TD_ATOM_BINARY` object
  - `test_fn_vary` — creates `TD_ATOM_VARY` object
- [x] Register `test_lang_suite` in `test/test_main.c` (extern + child_suites)
- [x] Run tests, verify linker failure (functions don't exist yet)
- [x] Create `src/lang/env.h`:
  - `td_fn_unary(name, fn_attrs, fn)` / `td_fn_binary(...)` / `td_fn_vary(...)`
  - `td_env_init()` / `td_env_destroy()` / `td_env_get(sym_id)` / `td_env_set(sym_id, val)`
- [x] Create `src/lang/env.c`:
  - Function constructors: `td_alloc(0)`, set `type = TD_ATOM_UNARY/...`, store fn pointer in `i64` field
  - Global env: fixed-size array of `{key, val}` pairs, linear scan (512 slots)
- [x] Run tests: `./build/test_teide --suite /lang` — 3/3 pass
- [x] Commit: `feat(lang): function object constructors and global environment`

---

## Phase 2: Lexer & Parser

### Task 2.1: ASCII dispatch table, lexer, and recursive descent parser

**Files:** Create `src/lang/parse.h`, `src/lang/parse.c`; Modify `test/test_lang.c`

- [x] Add lexer tests to `test/test_lang.c`:
  - `test_lex_i64` — `td_parse("42")` → `TD_ATOM_I64`, value 42
  - `test_lex_neg_i64` — `td_parse("-7")` → value -7
  - `test_lex_f64` — `td_parse("3.14")` → `TD_ATOM_F64`, value 3.14
  - `test_lex_string` — `td_parse("\"hello\"")` → `TD_ATOM_STR`
  - `test_lex_symbol` — `td_parse("'AAPL")` → `TD_ATOM_SYM`
  - `test_lex_bool` — `td_parse("true")` → `TD_ATOM_BOOL` b8=1, `td_parse("false")` → b8=0
- [x] Run tests, verify linker failure (`td_parse` not found)
- [x] Create `src/lang/parse.h`: declare `td_t* td_parse(const char* source)`
- [x] Create `src/lang/parse.c`:
  - 128-byte `_PA[]` ASCII dispatch table (reference: kdb `parse.c:25-66`)
  - `PA(c)` macro: single indexed read, zero branches
  - Actions: `PA_DIGIT`, `PA_ALPHA`, `PA_STRING`, `PA_QUOTE`, `PA_LPAREN/RPAREN`, `PA_LBRACK/RBRACK`, `PA_LBRACE/RBRACE`, `PA_COLON`, `PA_WS`, `PA_END`, `PA_MINUS`, `PA_SEMI`
  - `td_parser_t` struct: `{src, pos}`
  - `skip_ws_and_comments()` — skips whitespace + `;` line comments
  - `parse_number()` — integers, floats (dates/timestamps later)
  - `parse_string()` — `"..."` with escape handling
  - `parse_symbol()` — `'name` → `td_sym(intern(name))`
  - `parse_name()` — alphanumeric + operators → `td_sym` with `TD_ATTR_NAME`, check for `true`/`false`
  - `parse_vector()` — `[expr ...]` → list of parsed exprs
  - `parse_dict()` — `{key: val ...}` → dict
  - `parse_list()` — `(expr ...)` → list
  - `parse_expr()` — dispatch via `PA(*pos)` switch
  - `td_parse()` — public entry, creates parser state, calls `parse_expr`
- [x] Run tests: `./build/test_teide --suite /lang/lex` — all pass
- [x] Commit: `feat(lang): lexer with ASCII dispatch table and recursive descent parser`

---

### Task 2.2: S-expression and structural parsing tests

**Files:** Modify `test/test_lang.c`

- [x] Add parse tests:
  - `test_parse_sexpr` — `td_parse("(+ 1 2)")` → `TD_LIST`, len 3
  - `test_parse_nested` — `td_parse("(+ (* 2 3) 4)")` → outer list len 3, second elem is list len 3
  - `test_parse_vector` — `td_parse("[1 2 3]")` → list/vector len 3
  - `test_parse_empty_list` — `td_parse("()")` → `TD_LIST`, len 0
- [x] Run tests: `./build/test_teide --suite /lang/parse` — all pass
- [x] Commit: `test(lang): s-expression and vector parsing tests`

---

## Phase 3: Tree-Walking Eval

### Task 3.1: Eval with arithmetic builtins

**Files:** Create `src/lang/eval.h`, `src/lang/eval.c`; Modify `test/test_lang.c`, `include/teide/td.h`

- [x] Add eval tests:
  - `test_eval_literal` — `td_eval_str("42")` → i64 42
  - `test_eval_add` — `td_eval_str("(+ 1 2)")` → i64 3
  - `test_eval_nested_arith` — `td_eval_str("(+ (* 2 3) 4)")` → i64 10
  - `test_eval_sub` — `td_eval_str("(- 10 3)")` → i64 7
  - `test_eval_div` — `td_eval_str("(/ 10 3)")` → i64 3
  - `test_eval_cmp` — `td_eval_str("(> 5 3)")` → bool true
- [x] Update `lang_setup` to call `td_lang_init()` (inits env + registers builtins)
- [x] Create `src/lang/eval.h`:
  - `td_lang_init()` / `td_lang_destroy()`
  - `td_eval(td_t* obj)` — evaluate parsed object tree
  - `td_eval_str(const char* source)` — parse + eval convenience
- [x] Create `src/lang/eval.c`:
  - `td_eval()` — recursive tree-walking:
    - Atoms: return self (retain). If `TD_ATOM_SYM` with `TD_ATTR_NAME`: resolve from env
    - Non-list vectors: return self
    - List: eval first element, dispatch by type (`TD_ATOM_UNARY/BINARY/VARY`)
    - `TD_ATOM_UNARY`: `fn(eval(arg1))`
    - `TD_ATOM_BINARY`: `fn(eval(arg1), eval(arg2))` (or unevaluated if `FN_SPECIAL_FORM`)
    - `TD_ATOM_VARY`: `fn(args, n)` (eval all args unless `FN_SPECIAL_FORM`)
  - `td_eval_str()` — `td_parse(source)` then `td_eval(parsed)`
  - Builtin implementations: `ray_add`, `ray_sub`, `ray_mul`, `ray_div`, `ray_mod`, `ray_gt`, `ray_lt`, `ray_ge`, `ray_le`, `ray_eq`, `ray_ne`, `ray_not`, `ray_neg`
    - Handle `TD_ATOM_I64` + `TD_ATOM_I64` → `TD_ATOM_I64`
    - Handle `TD_ATOM_F64` mixed → `TD_ATOM_F64`
  - `td_lang_init()` — calls `td_env_init()`, registers all builtins via `REGISTER_FN` macro
- [x] Run tests: `./build/test_teide --suite /lang/eval` — all pass
- [x] Commit: `feat(lang): tree-walking eval with arithmetic builtins`

---

### Task 3.2: Variable binding (set, let) and conditionals (if, do)

**Files:** Modify `src/lang/eval.c`, `test/test_lang.c`

- [x] Add tests:
  - `test_eval_set` — `(do (set x 10) x)` → i64 10
  - `test_eval_if_true` — `(if true 1 2)` → i64 1
  - `test_eval_if_false` — `(if false 1 2)` → i64 2
  - `test_eval_let` — `(do (let x 5) (+ x 3))` → i64 8
- [x] Implement `ray_set` (`FN_SPECIAL_FORM, TD_BINARY`) — eval value, store in global env
- [x] Implement `ray_let` (`FN_SPECIAL_FORM, TD_BINARY`) — eval value, store in local scope
- [x] Implement `ray_cond` / `if` (`FN_SPECIAL_FORM, TD_VARY`) — eval condition, branch
- [x] Implement `ray_do` (`FN_SPECIAL_FORM, TD_VARY`) — eval each expr in sequence, return last
- [x] Register all in `td_lang_init()`
- [x] Run tests: all pass
- [x] Commit: `feat(lang): set/let variable binding and if/do control flow`

---

### Task 3.3: Lambda functions

**Files:** Modify `src/lang/eval.c`, `test/test_lang.c`

- [x] Add tests:
  - `test_eval_lambda` — `(do (set double (fn [x] (* x 2))) (double 5))` → i64 10
  - `test_eval_lambda_multi` — `(do (set add3 (fn [a b c] (+ a (+ b c)))) (add3 1 2 3))` → i64 6
  - `test_eval_lambda_let` — `(do (set f (fn [a b] (let c (+ a b)) (+ c 1))) (f 3 4))` → i64 8
- [x] Implement `ray_fn` (`FN_SPECIAL_FORM, TD_VARY`):
  - First arg is vector of param names (symbols)
  - Remaining args are body expressions
  - Creates `TD_LAMBDA` object (stores args list + body)
- [x] Implement lambda call in `td_eval()`:
  - When head evals to `TD_ATOM_LAMBDA`: bind args into local env frame, eval body
  - Local env frames: push/pop stack for lexical scoping
- [x] Run tests: all pass
- [x] Commit: `feat(lang): lambda functions with lexical binding`

---

## Phase 4: Bytecode Compiler & VM

### Task 4.1: Bytecode compiler (AST → bytecode)

**Files:** Create `src/lang/compile.c`; Modify `src/lang/eval.h`, `test/test_lang.c`

Reference: `/home/hetoku/data/work/kdb/src/lang/compile.c`

- [x] Add tests:
  - `test_compile_basic` — `(do (set f (fn [x] (+ x 1))) (f 10))` → i64 11 (same as interpreted)
  - `test_compile_closure` — verify compiled lambda matches interpreted result
- [x] Define opcodes in `eval.h`:
  - `OP_RET`, `OP_JMP`, `OP_JMPF`
  - `OP_LOADCONST`, `OP_LOADENV`, `OP_STOREENV`, `OP_POP`
  - `OP_RESOLVE`
  - `OP_CALL1`, `OP_CALL2`, `OP_CALLN`, `OP_CALLF`, `OP_CALLS`, `OP_CALLD`
- [x] Implement `td_compile(td_t* lambda)` in `compile.c`:
  - `compiler_t` struct: code buffer, constant pool, local tracking
  - Walk parsed body, emit bytecode:
    - Literals → `OP_LOADCONST`
    - Names → `OP_LOADENV` (local) or `OP_RESOLVE` (global)
    - Function calls → eval args, `OP_CALL1/2/N`
    - `if` → `OP_JMPF` + branch
    - `let` → `OP_STOREENV`
  - Store compiled bytecode + constants in lambda object
- [x] Trigger compilation on first call (lazy, same as Rayforce)
- [x] Run tests: pass
- [x] Commit: `feat(lang): bytecode compiler for lambda functions`

---

### Task 4.2: Stack-based VM with computed goto

**Files:** Modify `src/lang/eval.c`, `src/lang/eval.h`, `test/test_lang.c`

Reference: `/home/hetoku/data/work/kdb/src/lang/vm.c`, `/home/hetoku/data/work/rayforce/core/eval.c`

- [ ] Add tests:
  - `test_vm_fib` — `(do (set fib (fn [n] (if (<= n 1) n (+ (fib (- n 1)) (fib (- n 2)))))) (fib 10))` → i64 55
  - `test_vm_loop` — `(do (set sum-to (fn [n acc] (if (== n 0) acc (sum-to (- n 1) (+ acc n))))) (sum-to 100 0))` → i64 5050
- [ ] Define `td_vm_t` struct in `eval.h`:
  - `sp`, `fp`, `rp`, `id` (i32 each, first cache line)
  - `fn` (current lambda), `heap` pointer
  - `ps[VM_STACK_SIZE]` — program stack (cache-line aligned)
  - `rs[VM_STACK_SIZE]` — return stack (`ctx_t` = `{fn, fp, ip}`)
  - Thread-local `__VM` pointer
- [ ] Implement `td_vm_eval(td_t* lambda)` with computed goto:
  - Dispatch table: `static void *dispatch[] = { [OP_LOADCONST] = &&op_loadconst, ... }`
  - `DISPATCH()` macro, `PUSH()`/`POP()` macros
  - `OP_CALL1`: pop fn + arg, dispatch by fn type, push result
  - `OP_CALL2`: pop fn + 2 args, dispatch
  - `OP_CALLF`: push return frame, switch to callee bytecode
  - `OP_RET`: pop frame, restore ip/fp/fn, push result
  - `OP_CALLS`: tail call (reuse frame)
  - `OP_CALLD`: fallback to `td_eval()` for dynamic dispatch
- [ ] Wire into `td_eval()`: when calling a compiled lambda, route to `td_vm_eval()`
- [ ] Run tests: pass (fib, loop)
- [ ] Commit: `feat(lang): stack-based VM with computed goto dispatch`

---

### Task 4.3: Error handling (try/raise) with trap frames

**Files:** Modify `src/lang/eval.c`, `test/test_lang.c`

- [ ] Add tests:
  - `test_eval_try` — `(try (/ 10 0) (fn [e] 0))` → i64 0
  - `test_eval_raise` — `(try (raise "boom") (fn [e] 42))` → i64 42
- [ ] Implement `OP_TRAP` / `OP_TRAP_END` in VM:
  - `OP_TRAP`: push sentinel frame + handler IP to return stack
  - On error: unwind return stack looking for trap sentinel, jump to handler
  - Push error value for handler to read
- [ ] Implement `ray_raise` (`TD_UNARY`) and `ray_try` (`TD_BINARY, FN_SPECIAL_FORM`)
- [ ] Run tests: pass
- [ ] Commit: `feat(lang): try/raise error handling with trap frames`

---

## Phase 5: Collection & Higher-Order Builtins

### Task 5.1: Vector operations and FN_ATOMIC auto-mapping

**Files:** Modify `src/lang/eval.c`, `test/test_lang.c`

- [ ] Add tests:
  - `test_eval_vector_add` — `(+ [1 2 3] 10)` → vector [11 12 13], len 3
  - `test_eval_vector_add_vec` — `(+ [1 2 3] [4 5 6])` → vector [5 7 9]
  - `test_eval_sum` — `(sum [1 2 3 4 5])` → i64 15
  - `test_eval_count` — `(count [1 2 3])` → i64 3
  - `test_eval_avg` — `(avg [2 4 6])` → f64 4.0
  - `test_eval_min_max` — `(min [5 2 8])` → i64 2, `(max [5 2 8])` → i64 8
  - `test_eval_first_last` — `(first [1 2 3])` → i64 1, `(last [1 2 3])` → i64 3
- [ ] Implement `FN_ATOMIC` auto-mapping in eval dispatch:
  - If fn has `FN_ATOMIC` and any arg is a vector, map element-wise
  - Same logic as Rayforce's atomic dispatch
- [ ] Implement aggregation builtins: `ray_sum`, `ray_avg`, `ray_min`, `ray_max`, `ray_count`, `ray_first`, `ray_last`, `ray_med`, `ray_dev`
- [ ] Run tests: all pass
- [ ] Commit: `feat(lang): FN_ATOMIC auto-mapping and aggregation builtins`

---

### Task 5.2: Higher-order functions (map, fold, scan, filter)

**Files:** Modify `src/lang/eval.c`, `test/test_lang.c`

- [ ] Add tests:
  - `test_eval_map` — `(map + 1 [1 2 3])` → [2 3 4]
  - `test_eval_pmap` — `(pmap * 2 [1 2 3])` → [2 4 6]
  - `test_eval_fold` — `(fold + [1 2 3 4 5])` → i64 15
  - `test_eval_scan` — `(scan + [1 2 3 4 5])` → [1 3 6 10 15]
  - `test_eval_filter` — `(filter [1 2 3 4 5] [true false true false true])` → [1 3 5]
  - `test_eval_apply` — `(apply + [1 2] [3 4])` → [4 6]
- [ ] Implement `ray_map` (`TD_VARY`) — apply fn to each element
- [ ] Implement `ray_pmap` (`TD_VARY`) — parallel map (thread pool)
- [ ] Implement `ray_fold` (`TD_VARY`) — reduce with fn
- [ ] Implement `ray_scan` (`TD_VARY`) — running fold
- [ ] Implement `ray_filter` (`TD_BINARY`) — filter by boolean mask
- [ ] Implement `ray_apply` (`TD_VARY`) — zip-apply fn
- [ ] Run tests: all pass
- [ ] Commit: `feat(lang): higher-order functions (map, fold, scan, filter)`

---

### Task 5.3: Collection operations

**Files:** Modify `src/lang/eval.c`, `test/test_lang.c`

- [ ] Add tests:
  - `test_eval_distinct` — `(distinct [1 1 2 2 3])` → [1 2 3]
  - `test_eval_in` — `(in 2 [1 2 3])` → true
  - `test_eval_except` — `(except [1 2 3] [2])` → [1 3]
  - `test_eval_union` — `(union [1 2] [2 3])` → [1 2 3]
  - `test_eval_sect` — `(sect [1 2 3] [2 3 4])` → [2 3]
  - `test_eval_take` — `(take [1 2 3 4 5] 3)` → [1 2 3]
  - `test_eval_take_neg` — `(take [1 2 3 4 5] -3)` → [3 4 5]
  - `test_eval_at` — `(at [10 20 30] 1)` → i64 20
  - `test_eval_find` — `(find [1 2 3] 2)` → i64 1
  - `test_eval_reverse` — `(reverse [1 2 3])` → [3 2 1]
- [ ] Implement all collection builtins
- [ ] Run tests: all pass
- [ ] Commit: `feat(lang): collection operations (distinct, in, except, union, take, at, find)`

---

## Phase 6: Table Queries (DAG Bridge)

### Task 6.1: Table construction and column access

**Files:** Modify `src/lang/eval.c`, `test/test_lang.c`

- [ ] Add tests:
  - `test_eval_table` — `(table [a b] (list [1 2 3] [10 20 30]))` → `TD_TABLE`
  - `test_eval_at_table` — `(at t 'a)` → column vector, len 3
  - `test_eval_key_table` — `(key t)` → symbol vector of column names
  - `test_eval_count_table` — `(count t)` → i64 3 (row count)
- [ ] Implement `ray_table` (`TD_BINARY`) — col names + col data → `td_table_new`
- [ ] Implement `ray_at` (`TD_BINARY`) — index into vectors, dicts, tables by key
- [ ] Implement `ray_key` (`TD_UNARY`) — column names from table/dict
- [ ] Implement `ray_value` (`TD_UNARY`) — values from dict
- [ ] Run tests: all pass
- [ ] Commit: `feat(lang): table construction and column access`

---

### Task 6.2: select queries (bridge to Teide DAG)

**Files:** Modify `src/lang/eval.c`, `test/test_lang.c`

- [ ] Add tests:
  - `test_eval_select_all` — `(select {from: t})` → same table
  - `test_eval_select_where` — `(select {from: t where: (> salary 55000)})` → filtered table
  - `test_eval_select_cols` — `(select {name: name salary: salary from: t})` → projected table
  - `test_eval_select_groupby` — `(select {avg_sal: (avg salary) from: t by: dept})` → grouped table
  - `test_eval_select_xbar` — `(select {... by: (xbar timestamp 60000000000)})` → time-bucketed
- [ ] Implement `ray_select` (`TD_UNARY`):
  - Receive evaluated dict arg
  - Extract `from:`, `where:`, `by:`, output column expressions
  - When `from:` is a table:
    1. `td_graph_new(table)`
    2. Walk where/by/output expressions, emit DAG nodes (`td_scan`, `td_filter`, `td_group`, etc.)
    3. `td_optimize(g, root)` → `td_execute(g, root)`
  - Return result table
- [ ] Implement `ray_xbar` (`TD_BINARY, FN_ATOMIC`) — time bucketing
- [ ] Run tests: all pass
- [ ] Commit: `feat(lang): select queries bridging to Teide DAG executor`

---

### Task 6.3: update and insert

**Files:** Modify `src/lang/eval.c`, `test/test_lang.c`

- [ ] Add tests:
  - `test_eval_update` — `(update {salary: (* salary 1.1) from: t where: (= dept 'IT)})` → updated table
  - `test_eval_insert` — `(insert t (list "Charlie" 'IT 75000))` → table with new row
  - `test_eval_upsert` — `(upsert t 'id row)` → updated or inserted
- [ ] Implement `ray_update` (`TD_UNARY`) — similar to select but modifies columns
- [ ] Implement `ray_insert` (`TD_VARY`)
- [ ] Implement `ray_upsert` (`TD_VARY`)
- [ ] Run tests: all pass
- [ ] Commit: `feat(lang): update and insert table operations`

---

### Task 6.4: Joins

**Files:** Modify `src/lang/eval.c`, `test/test_lang.c`

- [ ] Add tests:
  - `test_eval_left_join` — `(left-join t1 t2 [key])` → joined table, all left rows kept
  - `test_eval_inner_join` — `(inner-join t1 t2 [key])` → only matching rows
  - `test_eval_window_join` — `(window-join t1 t2 [key] [time] window)` → time window join
- [ ] Implement `ray_left_join`, `ray_inner_join`, `ray_window_join` (`TD_VARY`):
  - Bridge to Teide's `td_join()` / `td_asof_join()` DAG nodes
- [ ] Run tests: all pass
- [ ] Commit: `feat(lang): join operations (left-join, inner-join, window-join)`

---

## Phase 7: I/O & REPL

### Task 7.1: I/O builtins

**Files:** Modify `src/lang/eval.c`, `test/test_lang.c`

- [ ] Add tests:
  - `test_eval_println` — `(println "hello")` → null, outputs to stdout
  - `test_eval_read_write_csv` — write temp CSV, read it back, verify table matches
  - `test_eval_as_cast` — `(as 'I64 "42")` → i64 42
  - `test_eval_type` — `(type 42)` → type code
- [ ] Implement `ray_println` (`TD_VARY`)
- [ ] Implement `ray_read_csv` (`TD_VARY`) — wire to `td_csv_load`
- [ ] Implement `ray_write_csv` (`TD_VARY`)
- [ ] Implement `ray_read` / `ray_write` (`TD_UNARY` / `TD_BINARY`)
- [ ] Implement `ray_cast_obj` / `as` (`TD_BINARY`)
- [ ] Implement `ray_type` (`TD_UNARY`)
- [ ] Run tests: all pass
- [ ] Commit: `feat(lang): I/O builtins (read-csv, write-csv, println, as, type)`

---

### Task 7.2: REPL binary

**Files:** Create `src/lang/repl.c`; Modify `CMakeLists.txt`

- [ ] Create `src/lang/repl.c`:
  - `main()`: init heap, sym, lang
  - File mode: `argc > 1` → read file, eval
  - REPL mode: `fgets` loop, `td_eval_str(line)`, print result
  - Cleanup: `td_lang_destroy`, `td_sym_destroy`, `td_heap_destroy`
- [ ] Add to `CMakeLists.txt`:
  - `add_executable(teide_repl src/lang/repl.c)`
  - `target_link_libraries(teide_repl PRIVATE teide_static)`
- [ ] Build: `cmake --build build`
- [ ] Smoke test: `echo '(+ 1 2)' | ./build/teide_repl` → outputs `3`
- [ ] Smoke test: `echo '(sum [1 2 3 4 5])' | ./build/teide_repl` → outputs `15`
- [ ] Commit: `feat(lang): Rayfall REPL binary`

---

## Phase 8: Heap Threading (Per-VM Heaps)

### Task 8.1: Add heap_id to td_t and per-VM heap allocation

**Files:** Modify `include/teide/td.h`, `src/mem/buddy.c`

Reference: `/home/hetoku/data/work/rayforce/core/heap.h`, `/home/hetoku/data/work/rayforce/core/heap.c`

- [ ] Add `heap_id` (u16) field to `td_t` header
- [ ] Implement bitmap-based heap ID allocator (atomic CAS, same as Rayforce)
- [ ] Add foreign block queue to heap struct (lock-free LIFO)
- [ ] Modify `td_free()`: check `block->heap_id == current_heap->id`, if not → enqueue to foreign blocks
- [ ] Implement `td_heap_flush_foreign()` — reclaim foreign blocks into own freelist
- [ ] Implement `td_heap_push_pending()` / `td_heap_drain_pending()` for worker teardown
- [ ] Add tests:
  - `test_cross_heap_free` — alloc on heap A, free from heap B context, verify no crash
  - `test_heap_pending_merge` — create worker heap, destroy it, verify blocks reclaimed
- [ ] Run tests: all pass
- [ ] Commit: `feat(mem): per-VM heap with heap_id and foreign block queue`

---

## Dependency Graph

```
Phase 1: Type Foundation
  1.1 Type tags ──→ 1.2 Constructors + env

Phase 2: Lexer
  1.2 ──→ 2.1 Parser ──→ 2.2 Parse tests

Phase 3: Eval (CRITICAL PATH)
  2.1 ──→ 3.1 Eval + arithmetic ──→ 3.2 set/let/if ──→ 3.3 Lambdas

Phase 4: VM (CRITICAL PATH)
  3.3 ──→ 4.1 Compiler ──→ 4.2 VM ──→ 4.3 try/raise

Phase 5: Collections (parallel after 3.1)
  3.1 ──→ 5.1 Vectors + FN_ATOMIC
  5.1 + 3.3 ──→ 5.2 map/fold/scan
  5.1 ──→ 5.3 Collection ops

Phase 6: Tables (parallel after 5.1)
  5.1 ──→ 6.1 Table construction
  6.1 ──→ 6.2 select (DAG bridge)
  6.2 ──→ 6.3 update/insert
  6.2 ──→ 6.4 Joins

Phase 7: I/O & REPL (parallel after 3.1)
  3.1 ──→ 7.1 I/O builtins
  3.1 ──→ 7.2 REPL

Phase 8: Heap (independent)
  8.1 Per-VM heaps (can be done anytime)
```

**Critical path:** 1.1 → 1.2 → 2.1 → 3.1 → 3.2 → 3.3 → 4.1 → 4.2 (8 tasks in sequence)
