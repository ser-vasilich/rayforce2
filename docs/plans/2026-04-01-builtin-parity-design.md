# Builtin Parity: Port 42 Missing Builtins from Rayforce1

## Goal

Reach feature parity with rayforce1's Rayfall language. All 96 rayforce1 doc tests must pass against rayforce2. 6 IPC/persistence builtins deferred (hopen, hclose, get-parted, set-parted, get-splayed, set-splayed).

## Principle

**DAG first, lang wraps it.** If an operation can benefit from morsel-parallel execution, implement the DAG op + executor kernel first. The lang builtin is a thin wrapper that builds a DAG graph and executes it. This gives us both standalone use and in-query use for free.

## Implementation Phases

### Phase 1: Sorting & Ordering (7 builtins)

DAG infrastructure: `OP_SORT` (61) already fully implemented with radix sort + merge sort fallback. `exec_sort()` handles multi-column, desc flags, nulls-first.

| Builtin | Args | What it does | DAG path |
|---------|------|--------------|----------|
| `asc` | unary(vec) | Sort vector ascending | Build 1-col OP_SORT, gather result |
| `desc` | unary(vec) | Sort vector descending | Build 1-col OP_SORT desc=1, gather |
| `iasc` | unary(vec) | Return ascending sort indices | Build OP_SORT, return index array |
| `idesc` | unary(vec) | Return descending sort indices | Build OP_SORT desc=1, return index array |
| `rank` | unary(vec) | Rank positions (dense) | iasc of iasc |
| `xasc` | binary(tbl, cols) | Sort table by columns ascending | Build multi-col OP_SORT |
| `xdesc` | binary(tbl, cols) | Sort table by columns descending | Build multi-col OP_SORT desc=1 |

### Phase 2: Pattern Matching (1 builtin)

DAG infrastructure: `OP_LIKE` (36) already exists in ops.h. Need to verify executor kernel exists.

| Builtin | Args | What it does | DAG path |
|---------|------|--------------|----------|
| `like` | binary(str, pattern) | Glob-style match (*, ?, []) | OP_LIKE on vector, tree-walk on atoms |

### Phase 3: Temporal Clocks (3 builtins)

No DAG needed — these return the current system time as atoms.

| Builtin | Args | What it does |
|---------|------|--------------|
| `date` | unary(sym) | Current date (local/global) |
| `time` | unary(sym) | Current time (local/global) |
| `timestamp` | unary(sym) | Current timestamp (local/global) |

### Phase 4: Table Operations (4 builtins)

| Builtin | Args | What it does | DAG path |
|---------|------|--------------|----------|
| `pivot` | vary(tbl, idx, cols, vals, aggfn) | Pivot table | New OP_PIVOT DAG op |
| `meta` | unary(any) | Object metadata dict | Tree-walk (introspection) |
| `row` | binary(tbl, idx) | Single row from table | Tree-walk |
| `modify` | vary(tbl, col, fn) | Modify table column | Tree-walk or OP_SELECT variant |

### Phase 5: Eval/Meta (5 builtins)

No DAG — these are language-level meta-operations.

| Builtin | Args | What it does |
|---------|------|--------------|
| `eval` | unary(ast) | Evaluate parsed AST |
| `parse` | unary(str) | Parse string to AST |
| `quote` | special(expr) | Prevent evaluation |
| `return` | unary(val) | Early return from function |
| `del` | unary(sym) | Delete variable from env |

### Phase 6: Functional (4 builtins)

No DAG — higher-order functions with user-provided lambdas.

| Builtin | Args | What it does |
|---------|------|--------------|
| `fold-left` | vary(fn, init, coll) | Left fold with initial value |
| `fold-right` | vary(fn, init, coll) | Right fold with initial value |
| `scan-left` | vary(fn, init, coll) | Left scan (intermediate results) |
| `scan-right` | vary(fn, init, coll) | Right scan |

### Phase 7: System/Introspection (10 builtins)

No DAG — system-level operations.

| Builtin | Args | What it does |
|---------|------|--------------|
| `args` | nullary | Command line arguments |
| `env` | nullary | List all env bindings |
| `gc` | nullary | Force garbage collection |
| `internals` | nullary | Internal state dump |
| `memstat` | nullary | Memory statistics |
| `sysinfo` | nullary | System info (cores, memory) |
| `system` | unary(str) | Run shell command |
| `os-get-var` | unary(str) | getenv |
| `os-set-var` | binary(str, str) | setenv |
| `timer` | unary(expr) | High-res timing |

### Phase 8: Misc (8 builtins)

| Builtin | Args | What it does | DAG path |
|---------|------|--------------|----------|
| `diverse` | unary(vec) | All elements unique? | Could use DAG hash/group |
| `get` | binary(dict, key) | Dictionary lookup | Tree-walk |
| `print` | unary(any) | Print without newline | Tree-walk |
| `rc` | unary(any) | Reference count | Tree-walk |
| `remove` | binary(dict, key) | Remove key from dict | Tree-walk |
| `unify` | binary(vec, vec) | Type unification | Tree-walk |
| `xrank` | binary(n, vec) | Ranked grouping | DAG sort + partition |
| `enum` | binary(sym, vec) | Dictionary-encoded enum | Tree-walk |

### Phase 9: Validation

- Run all rayforce1 doc tests against rayforce2
- Run all rfl examples
- Fix any formatting/display differences (C8→str, etc.)
- Target: 96/96 doc tests pass

## Deferred (6 builtins)

- `hopen`, `hclose` — IPC/networking
- `get-parted`, `set-parted`, `get-splayed`, `set-splayed` — persistence
- `loadfn` — dynamic library loading
