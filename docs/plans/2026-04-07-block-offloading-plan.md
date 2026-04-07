# Block Offloading Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Enable larger-than-RAM query execution on parted tables by streaming partition segments through the executor one at a time, with eager release after each segment.

**Architecture:** Replace the flat-materialization path in `OP_SCAN` (`exec.c:590-610`) with a segment iterator that feeds one partition at a time into the existing morsel pipeline. Add operator-aware merge functions (group, sort, filter). Enhance the existing `pass_partition_pruning` in `opt.c` to produce a `seg_mask` bitmap. Memory budget auto-detected from OS at `ray_init()` time.

**Tech Stack:** Pure C17, no new dependencies. Uses existing `sysconf(_SC_PHYS_PAGES)` / `GlobalMemoryStatusEx` for memory detection, existing mmap infrastructure for segment loading.

---

## Task 1: Memory Budget in Runtime [x]

**Files:**
- [x] Modify: `src/core/runtime.h:70-73` (add `mem_budget` field to `ray_runtime_s`)
- [x] Modify: `src/core/runtime.c` (detect budget at init)
- [x] Modify: `include/rayforce.h` (declare public API)
- [x] Test: `test/test_store.c` (add budget test)

**Step 1: Add `mem_budget` field to runtime struct**

In `src/core/runtime.h`, add to `ray_runtime_s`:

```c
typedef struct ray_runtime_s {
    ray_vm_t       **vms;
    int32_t          n_vms;
    int64_t          mem_budget;   /* 80% of physical RAM, bytes */
} ray_runtime_t;
```

**Step 2: Detect budget at init**

In `src/core/runtime.c`, inside `ray_runtime_create()`, after existing init code:

```c
/* Detect memory budget: 80% of physical RAM */
#if defined(_WIN32)
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms))
        rt->mem_budget = (int64_t)(ms.ullTotalPhys * 0.8);
    else
        rt->mem_budget = (int64_t)4 * 1024 * 1024 * 1024; /* 4 GB fallback */
#else
    long pages = sysconf(_SC_PHYS_PAGES);
    long psize = sysconf(_SC_PAGESIZE);
    if (pages > 0 && psize > 0)
        rt->mem_budget = (int64_t)((double)pages * (double)psize * 0.8);
    else
        rt->mem_budget = (int64_t)4 * 1024 * 1024 * 1024;
#endif
```

**Step 3: Add public API to `include/rayforce.h`**

```c
int64_t ray_mem_budget(void);      /* returns memory budget in bytes */
bool    ray_mem_pressure(void);    /* true if usage exceeds budget */
```

**Step 4: Implement in `src/core/runtime.c`**

```c
int64_t ray_mem_budget(void) {
    return __RUNTIME ? __RUNTIME->mem_budget : 0;
}

bool ray_mem_pressure(void) {
    if (!__RUNTIME) return false;
    ray_mem_stats_t st;
    ray_mem_stats(&st);
    return (int64_t)(st.bytes_allocated + st.direct_bytes) > __RUNTIME->mem_budget;
}
```

**Step 5: Write test**

In `test/test_store.c`, add:

```c
static MunitResult test_mem_budget(const void* params, void* fixture) {
    (void)params; (void)fixture;
    int64_t budget = ray_mem_budget();
    /* Budget should be > 0 (detected from OS) and < total physical RAM */
    munit_assert_int64(budget, >, 0);
    /* At startup with minimal allocations, should not be under pressure */
    munit_assert_false(ray_mem_pressure());
    return MUNIT_OK;
}
```

Register in the test suite array.

**Step 6: Build and run**

```bash
make && ./rayforce.test --suite /store
```
Expected: all store tests pass including new `test_mem_budget`.

**Step 7: Commit**

```bash
git add src/core/runtime.h src/core/runtime.c include/rayforce.h test/test_store.c
git commit -m "feat: memory budget detection in runtime (80% of physical RAM)"
```

---

## Task 2: Segment Iterator Structure

**Files:**
- Modify: `src/ops/ops.h:369` (add `ray_seg_iter_t` struct)

**Step 1: Add segment iterator struct**

In `src/ops/ops.h`, after the `ray_morsel_t` definition (line 379), add:

```c
/* ===== Segment Iterator (parted column streaming) ===== */

typedef struct {
    ray_t**   segs;        /* segment pointer array (from parted column data) */
    uint32_t  n_segs;      /* total segment count */
    uint32_t  cur_seg;     /* current segment index (starts at 0) */
    uint64_t* seg_mask;    /* pruning bitmap: bit N set = segment N active. NULL = all active */
} ray_seg_iter_t;

/* Init from a parted column. seg_mask may be NULL (all active). */
static inline void ray_seg_iter_init(ray_seg_iter_t* si, ray_t* parted, uint64_t* mask) {
    si->segs    = (ray_t**)ray_data(parted);
    si->n_segs  = (uint32_t)parted->len;
    si->cur_seg = 0;
    si->seg_mask = mask;
}

/* Advance to next active segment. Returns NULL when exhausted. */
static inline ray_t* ray_seg_iter_next(ray_seg_iter_t* si) {
    while (si->cur_seg < si->n_segs) {
        uint32_t idx = si->cur_seg++;
        if (si->seg_mask) {
            if (!(si->seg_mask[idx / 64] & (1ULL << (idx % 64))))
                continue; /* pruned */
        }
        ray_t* seg = si->segs[idx];
        if (seg && seg->len > 0) return seg;
    }
    return NULL;
}

/* Total rows across active (non-pruned) segments. */
static inline int64_t ray_seg_iter_total_rows(ray_seg_iter_t* si) {
    int64_t total = 0;
    for (uint32_t i = 0; i < si->n_segs; i++) {
        if (si->seg_mask && !(si->seg_mask[i / 64] & (1ULL << (i % 64))))
            continue;
        if (si->segs[i]) total += si->segs[i]->len;
    }
    return total;
}
```

**Step 2: Build**

```bash
make
```
Expected: compiles cleanly (struct and inlines only, no callers yet).

**Step 3: Commit**

```bash
git add src/ops/ops.h
git commit -m "feat: ray_seg_iter_t — segment iterator for parted column streaming"
```

---

## Task 3: Partition Pruning — Produce seg_mask

**Files:**
- Modify: `src/ops/ops.h` (add `seg_mask` field to `ray_op_ext_t`)
- Modify: `src/ops/opt.c:1611-1641` (replace stub `pass_partition_pruning` with bitmap producer)
- Test: `test/test_opt.c` (add pruning test)

**Step 1: Add `seg_mask` to `ray_op_ext_t`**

In `src/ops/ops.h`, inside the `ray_op_ext_t` union, find the base/common fields and add:

```c
uint64_t* seg_mask;   /* partition pruning bitmap (NULL = all active) */
```

This goes in the common area of `ray_op_ext_t` (not inside a union variant), since it applies to OP_SCAN nodes.

**Step 2: Rewrite `pass_partition_pruning` in `src/ops/opt.c:1611-1641`**

Replace the current stub with a full implementation:

```c
static void pass_partition_pruning(ray_graph_t* g, ray_op_t* root) {
    if (!g || !root) return;

    for (uint32_t i = 0; i < g->node_count; i++) {
        ray_op_t* n = &g->nodes[i];
        if (n->flags & OP_FLAG_DEAD) continue;
        if (n->opcode != OP_FILTER || n->arity != 2) continue;

        ray_op_t* data_in = n->inputs[0];
        ray_op_t* pred = n->inputs[1];
        if (!pred || pred->arity != 2) continue;

        /* Supported comparison opcodes */
        uint16_t cmp_op = pred->opcode;
        if (cmp_op != OP_EQ && cmp_op != OP_NE &&
            cmp_op != OP_LT && cmp_op != OP_GT &&
            cmp_op != OP_LE && cmp_op != OP_GE) continue;

        ray_op_t* lhs = pred->inputs[0];
        ray_op_t* rhs = pred->inputs[1];
        if (!lhs || !rhs) continue;

        ray_op_t* scan_node = NULL;
        ray_op_t* const_node = NULL;
        bool swapped = false;
        if (lhs->opcode == OP_SCAN && rhs->opcode == OP_CONST) {
            scan_node = lhs; const_node = rhs;
        } else if (rhs->opcode == OP_SCAN && lhs->opcode == OP_CONST) {
            scan_node = rhs; const_node = lhs; swapped = true;
        } else continue;

        if (scan_node->out_type != RAY_MAPCOMMON) continue;

        /* Get MAPCOMMON column to read partition keys */
        ray_op_ext_t* scan_ext = find_ext(g, scan_node->id);
        if (!scan_ext) continue;

        /* Resolve table */
        uint16_t stored_table_id = 0;
        memcpy(&stored_table_id, scan_ext->base.pad, sizeof(uint16_t));
        ray_t* tbl;
        if (stored_table_id > 0 && g->tables && (stored_table_id - 1) < g->n_tables)
            tbl = g->tables[stored_table_id - 1];
        else
            tbl = g->table;
        if (!tbl) continue;

        ray_t* mc_col = ray_table_get_col(tbl, scan_ext->sym);
        if (!mc_col || mc_col->type != RAY_MAPCOMMON) continue;

        /* Extract constant value */
        ray_op_ext_t* const_ext = find_ext(g, const_node->id);
        if (!const_ext || !const_ext->literal) continue;
        ray_t* lit = const_ext->literal;

        /* Read partition keys and row counts from MAPCOMMON */
        ray_t** mc_ptrs = (ray_t**)ray_data(mc_col);
        ray_t* key_values = mc_ptrs[0];
        if (!key_values) continue;
        int64_t n_parts = key_values->len;
        if (n_parts <= 0) continue;

        /* Allocate seg_mask bitmap */
        uint32_t n_words = (uint32_t)((n_parts + 63) / 64);
        uint64_t* mask = (uint64_t*)ray_sys_alloc(n_words * sizeof(uint64_t));
        if (!mask) continue;
        memset(mask, 0, n_words * sizeof(uint64_t));

        /* Compare each partition key against the constant */
        int64_t const_val = 0;
        if (lit->type == RAY_I64 || lit->type == RAY_DATE || lit->type == RAY_TIMESTAMP)
            memcpy(&const_val, ray_data(lit), sizeof(int64_t));
        else if (lit->type == RAY_I32 || lit->type == RAY_TIME) {
            int32_t v32; memcpy(&v32, ray_data(lit), sizeof(int32_t));
            const_val = v32;
        }

        /* Effective comparison: if swapped, reverse the operator direction */
        uint16_t eff_op = cmp_op;
        if (swapped) {
            if (cmp_op == OP_LT) eff_op = OP_GT;
            else if (cmp_op == OP_GT) eff_op = OP_LT;
            else if (cmp_op == OP_LE) eff_op = OP_GE;
            else if (cmp_op == OP_GE) eff_op = OP_LE;
        }

        bool any_active = false;
        for (int64_t p = 0; p < n_parts; p++) {
            int64_t pkey = 0;
            if (key_values->type == RAY_DATE || key_values->type == RAY_I32) {
                int32_t v32; memcpy(&v32, (char*)ray_data(key_values) + p * sizeof(int32_t), sizeof(int32_t));
                pkey = v32;
            } else {
                memcpy(&pkey, (char*)ray_data(key_values) + p * sizeof(int64_t), sizeof(int64_t));
            }

            bool pass = false;
            switch (eff_op) {
                case OP_EQ: pass = (pkey == const_val); break;
                case OP_NE: pass = (pkey != const_val); break;
                case OP_LT: pass = (pkey <  const_val); break;
                case OP_GT: pass = (pkey >  const_val); break;
                case OP_LE: pass = (pkey <= const_val); break;
                case OP_GE: pass = (pkey >= const_val); break;
            }
            if (pass) {
                mask[p / 64] |= (1ULL << (p % 64));
                any_active = true;
            }
        }

        if (!any_active) {
            ray_sys_free(mask);
            n->est_rows = 0;
            continue;
        }

        /* Attach seg_mask to all OP_SCAN nodes reading parted columns from same table.
         * Walk all nodes to find OP_SCANs with matching table_id. */
        for (uint32_t s = 0; s < g->node_count; s++) {
            ray_op_t* sn = &g->nodes[s];
            if (sn->flags & OP_FLAG_DEAD || sn->opcode != OP_SCAN) continue;
            if (sn == scan_node) continue; /* skip the MAPCOMMON scan itself */

            ray_op_ext_t* sn_ext = find_ext(g, sn->id);
            if (!sn_ext) continue;

            uint16_t sn_tid = 0;
            memcpy(&sn_tid, sn_ext->base.pad, sizeof(uint16_t));
            if (sn_tid != stored_table_id) continue;

            /* Check if column is parted */
            ray_t* sn_col = ray_table_get_col(tbl, sn_ext->sym);
            if (!sn_col || !RAY_IS_PARTED(sn_col->type)) continue;

            sn_ext->seg_mask = mask;
        }

        n->est_rows = 1; /* hint: most partitions skipped */
    }
}
```

**Step 3: Write test in `test/test_opt.c`**

Add a test that builds a parted table with known partition keys, constructs a DAG with a filter on the MAPCOMMON column, runs `ray_optimize`, and verifies that `seg_mask` is set on the scan nodes with correct bits.

```c
static MunitResult test_partition_pruning_mask(const void* params, void* fixture) {
    (void)params; (void)fixture;
    /* Build a parted table with 4 partitions: dates 2025.01.01 through 2025.04.01 */
    /* ... (construct parted table with MAPCOMMON) ... */
    /* Build DAG: scan(date) >= 2025.03.01 filter on data columns */
    /* Run ray_optimize */
    /* Verify seg_mask has bits 2,3 set (March, April), bits 0,1 clear */
    return MUNIT_OK;
}
```

The exact test construction will follow the pattern in `test/test_store.c:322-366` which already builds synthetic parted columns.

**Step 4: Build and run**

```bash
make && ./rayforce.test --suite /opt
```

**Step 5: Commit**

```bash
git add src/ops/ops.h src/ops/opt.c test/test_opt.c
git commit -m "feat: partition pruning produces seg_mask bitmap for parted OP_SCANs"
```

---

## Task 4: Streaming Scan — Replace Flat Materialization

**Files:**
- Modify: `src/ops/exec.c:568-614` (OP_SCAN case: add segment-streaming path)
- Modify: `src/ops/exec.c:1461-1484` (ray_execute: add segment loop orchestration)
- Modify: `src/ops/ops.h` (add fields to `ray_graph_t` for segment state)

**Step 1: Add segment state to `ray_graph_t`**

In `src/ops/ops.h`, add to `ray_graph_t`:

```c
typedef struct ray_graph {
    /* ... existing fields ... */
    /* Segment streaming state (block offloading) */
    int32_t    seg_idx;       /* current segment index (-1 = not streaming) */
    int32_t    seg_count;     /* total active segments */
    uint64_t*  seg_mask;      /* shared pruning mask (from optimizer, or NULL) */
    bool       is_streaming;  /* true if table has parted columns */
} ray_graph_t;
```

Initialize `seg_idx = -1`, `is_streaming = false` in `ray_graph_new()`.

**Step 2: Modify OP_SCAN to return current segment instead of materializing**

In `exec.c`, replace the parted-column block (lines 590-610) with:

```c
if (RAY_IS_PARTED(col->type)) {
    if (!g->is_streaming) {
        /* First encounter: detect streaming mode */
        g->is_streaming = true;
        g->seg_count = (int32_t)col->len;
        /* Find seg_mask from any scan ext that has one */
        if (!g->seg_mask) {
            for (uint32_t e = 0; e < g->ext_count; e++) {
                if (g->ext_nodes[e] && g->ext_nodes[e]->seg_mask) {
                    g->seg_mask = g->ext_nodes[e]->seg_mask;
                    break;
                }
            }
        }
        if (g->seg_idx < 0) g->seg_idx = 0;
    }
    /* Return segment at current index */
    ray_t** segs = (ray_t**)ray_data(col);
    ray_t* seg = (g->seg_idx < col->len) ? segs[g->seg_idx] : NULL;
    if (!seg) return ray_error("oom", NULL);
    ray_retain(seg);
    return seg;
}
```

**Step 3: Modify `ray_execute` to loop over segments**

Replace the body of `ray_execute` with segment-aware orchestration:

```c
ray_t* ray_execute(ray_graph_t* g, ray_op_t* root) {
    if (!g || !root) return ray_error("nyi", NULL);

    ray_pool_t* pool = ray_pool_get();
    if (pool)
        atomic_store_explicit(&pool->cancelled, 0, memory_order_relaxed);

    /* First execution pass — may detect streaming mode */
    g->seg_idx = 0;
    g->is_streaming = false;
    ray_t* result = exec_node(g, root);

    if (!g->is_streaming) {
        /* Non-parted table: existing path, unchanged */
        if (g->selection && result && !RAY_IS_ERR(result)
            && result->type == RAY_TABLE) {
            ray_t* compacted = sel_compact(g, result, g->selection);
            ray_release(result);
            ray_release(g->selection);
            g->selection = NULL;
            result = compacted;
        }
        return result;
    }

    /* Streaming mode: result from segment 0 is the accumulator.
     * Process remaining segments and merge. */
    if (!result || RAY_IS_ERR(result)) return result;

    int64_t total_rows = 0; /* for progress */
    /* Count total active rows across segments for progress tracking */
    /* (use first parted column found) */
    for (int64_t c = 0; g->table && c < ray_table_ncols(g->table); c++) {
        ray_t* col = ray_table_get_col_idx(g->table, c);
        if (col && RAY_IS_PARTED(col->type)) {
            ray_seg_iter_t si;
            ray_seg_iter_init(&si, col, g->seg_mask);
            total_rows = ray_seg_iter_total_rows(&si);
            break;
        }
    }

    ray_profile_progress_begin("query", total_rows);

    /* Advance progress for segment 0 */
    /* (rows from first segment already processed) */

    for (int32_t s = 1; s < g->seg_count; s++) {
        /* Check pruning mask */
        if (g->seg_mask) {
            if (!(g->seg_mask[s / 64] & (1ULL << (s % 64))))
                continue;
        }

        /* Check cancellation */
        if (pool_cancelled(pool)) {
            ray_release(result);
            ray_profile_progress_end();
            return ray_error("cancel", NULL);
        }

        /* Set segment index and re-execute DAG */
        g->seg_idx = s;
        g->selection = NULL; /* reset lazy selection for new segment */
        ray_t* partial = exec_node(g, root);

        if (!partial || RAY_IS_ERR(partial)) {
            ray_release(result);
            ray_profile_progress_end();
            return partial;
        }

        /* Compact partial if lazy selection pending */
        if (g->selection && partial && !RAY_IS_ERR(partial)
            && partial->type == RAY_TABLE) {
            ray_t* compacted = sel_compact(g, partial, g->selection);
            ray_release(partial);
            ray_release(g->selection);
            g->selection = NULL;
            partial = compacted;
        }

        /* Merge partial into accumulator */
        ray_t* merged = ray_result_merge(result, partial, root->opcode);
        ray_release(result);
        ray_release(partial);
        if (!merged || RAY_IS_ERR(merged)) {
            ray_profile_progress_end();
            return merged;
        }
        result = merged;
    }

    ray_profile_progress_end();
    return result;
}
```

**Step 4: Build (expect link error — `ray_result_merge` not yet implemented)**

```bash
make 2>&1 | head -5
```
Expected: linker error for `ray_result_merge`.

**Step 5: Commit (WIP)**

```bash
git add src/ops/ops.h src/ops/exec.c
git commit -m "wip: segment-streaming scan + execute loop (needs merge functions)"
```

---

## Task 5: Result Merge — Filter/Project (Concatenation)

**Files:**
- Create: forward declaration in `src/ops/internal.h`
- Modify: `src/ops/exec.c` (implement `ray_result_merge`)
- Test: `test/test_exec.c`

**Step 1: Declare merge function**

In `src/ops/internal.h`:

```c
/* Merge two partial results from partition-streamed execution.
 * Strategy depends on the root opcode:
 *   OP_GROUP  → re-aggregate matching keys
 *   OP_SORT   → merge-sort
 *   default   → table concatenation (filter/project/scan)
 */
ray_t* ray_result_merge(ray_t* accum, ray_t* partial, uint16_t root_opcode);
```

**Step 2: Implement basic concat merge in `exec.c`**

```c
ray_t* ray_result_merge(ray_t* accum, ray_t* partial, uint16_t root_opcode) {
    if (!accum || RAY_IS_ERR(accum)) return partial;
    if (!partial || RAY_IS_ERR(partial)) return accum;

    /* Table merge: concatenate each column */
    if (accum->type == RAY_TABLE && partial->type == RAY_TABLE) {
        int64_t ncols = ray_table_ncols(accum);
        ray_t* merged = ray_table_new(ncols);
        for (int64_t c = 0; c < ncols; c++) {
            int64_t name_id = ray_table_col_name(accum, c);
            ray_t* a_col = ray_table_get_col_idx(accum, c);
            ray_t* p_col = ray_table_get_col_idx(partial, c);
            if (!a_col || !p_col) continue;
            ray_t* combined = ray_vec_concat(a_col, p_col);
            if (!combined || RAY_IS_ERR(combined)) {
                ray_release(merged);
                return combined;
            }
            merged = ray_table_add_col(merged, name_id, combined);
            ray_release(combined);
        }
        return merged;
    }

    /* Vector merge: concatenate directly */
    if (accum->type != RAY_TABLE && partial->type != RAY_TABLE) {
        return ray_vec_concat(accum, partial);
    }

    return ray_error("type", NULL);
}
```

Note: this is the default path. `OP_GROUP` and `OP_SORT` merge will be added in subsequent tasks.

**Step 3: Write test**

Build a synthetic parted table with 2 segments, run a filter query through the DAG executor, verify the result contains rows from both segments that match the filter.

**Step 4: Build and run**

```bash
make && ./rayforce.test --suite /exec
```

**Step 5: Commit**

```bash
git add src/ops/internal.h src/ops/exec.c test/test_exec.c
git commit -m "feat: ray_result_merge — table/vector concatenation for streamed partitions"
```

---

## Task 6: Group Merge

**Files:**
- Modify: `src/ops/group.c` (add `ray_group_merge`)
- Modify: `src/ops/exec.c` (wire OP_GROUP case in `ray_result_merge`)
- Test: `test/test_exec.c`

**Step 1: Implement `ray_group_merge` in `group.c`**

The function takes two grouped result tables (same schema: key columns + aggregate columns) and re-aggregates matching keys.

```c
ray_t* ray_group_merge(ray_t* accum, ray_t* partial) {
    /* Both are tables with same schema: keys... then agg columns.
     * Strategy: concatenate both tables, then re-run GROUP on the result.
     * This is correct and reuses existing infrastructure.
     * For Phase 1, this is acceptable since partial results are small
     * (bounded by distinct key count, not raw row count). */
    int64_t ncols = ray_table_ncols(accum);
    ray_t* combined = ray_table_new(ncols);
    for (int64_t c = 0; c < ncols; c++) {
        int64_t name_id = ray_table_col_name(accum, c);
        ray_t* a = ray_table_get_col_idx(accum, c);
        ray_t* p = ray_table_get_col_idx(partial, c);
        if (!a || !p) continue;
        ray_t* cat = ray_vec_concat(a, p);
        if (!cat || RAY_IS_ERR(cat)) { ray_release(combined); return cat; }
        combined = ray_table_add_col(combined, name_id, cat);
        ray_release(cat);
    }
    return combined;
    /* Note: the caller (ray_execute) will need to re-aggregate this combined table.
     * This requires tracking group-by metadata. A simpler first approach is
     * to have the segment loop re-execute the GROUP node on the concatenated
     * partials after all segments are done. */
}
```

**IMPORTANT DESIGN NOTE:** The cleanest approach for Phase 1 is: each segment produces a partial group result (small table). After all segments, concatenate all partials and run one final GROUP pass. This reuses existing GROUP machinery and is correct for all aggregation types (sum, count, avg, min, max, first, last, med, dev). The extra cost is one GROUP over the concatenated partials — which is bounded by `n_segments * n_distinct_keys`, typically very small.

**Step 2: Wire into `ray_result_merge`**

In `exec.c`, inside `ray_result_merge`:

```c
if (root_opcode == OP_GROUP) {
    return ray_group_merge(accum, partial);
}
```

The final re-aggregation happens after the segment loop in `ray_execute` — add a post-loop GROUP pass using the original DAG's group-by keys and aggregation functions. This requires extracting the OP_GROUP ext metadata and reapplying.

**Step 3: Write test**

Build parted table with 3 segments, each containing rows with overlapping keys. Run a GROUP BY sum query. Verify the final result has correct sums across all partitions.

**Step 4: Build and run**

```bash
make && ./rayforce.test --suite /exec
```

**Step 5: Commit**

```bash
git add src/ops/group.c src/ops/exec.c test/test_exec.c
git commit -m "feat: group merge for partition-streamed execution"
```

---

## Task 7: Sort Merge

**Files:**
- Modify: `src/ops/sort.c` (add `ray_sort_merge`)
- Modify: `src/ops/exec.c` (wire OP_SORT case in `ray_result_merge`)
- Test: `test/test_exec.c`

**Step 1: Implement `ray_sort_merge` in `sort.c`**

Each partition produces a sorted segment. Merge uses k-way merge (for Phase 1 with 2 inputs at a time, a simple 2-way merge suffices since we accumulate left-to-right).

```c
ray_t* ray_sort_merge(ray_t* accum, ray_t* partial) {
    /* Both tables are individually sorted by the same key(s).
     * 2-way merge: walk both tables in order, emit rows in sorted order.
     * For Phase 1: concatenate + re-sort is acceptable since each
     * partial is already sorted (merge-sort's merge step). */
    /* Concatenate tables */
    int64_t ncols = ray_table_ncols(accum);
    ray_t* combined = ray_table_new(ncols);
    for (int64_t c = 0; c < ncols; c++) {
        int64_t name_id = ray_table_col_name(accum, c);
        ray_t* a = ray_table_get_col_idx(accum, c);
        ray_t* p = ray_table_get_col_idx(partial, c);
        if (!a || !p) continue;
        ray_t* cat = ray_vec_concat(a, p);
        if (!cat || RAY_IS_ERR(cat)) { ray_release(combined); return cat; }
        combined = ray_table_add_col(combined, name_id, cat);
        ray_release(cat);
    }
    return combined;
    /* Re-sort will be applied as a final pass after all segments,
     * same as group merge. This is correct for Phase 1 since
     * intermediates are small (post-filter/post-limit). */
}
```

**Step 2: Wire into `ray_result_merge`**

```c
if (root_opcode == OP_SORT) {
    return ray_sort_merge(accum, partial);
}
```

**Step 3: Write test**

Parted table, sort query. Verify output is globally sorted across all partitions.

**Step 4: Build and run**

```bash
make && ./rayforce.test --suite /exec
```

**Step 5: Commit**

```bash
git add src/ops/sort.c src/ops/exec.c test/test_exec.c
git commit -m "feat: sort merge for partition-streamed execution"
```

---

## Task 8: End-to-End Integration Test via Rayfall

**Files:**
- Create: `test_offload.rfl` (Rayfall script testing the full pipeline)
- Modify: `test/test_exec.c` (C-level integration test)

**Step 1: Create test data**

Write a C test that:
1. Creates a directory structure simulating a parted table with 4 date partitions
2. Each partition has 10,000 rows with `sym` (SYM) and `price` (F64) columns
3. Saves via `ray_splay_save`

**Step 2: Write integration tests**

Test cases:
1. **Full scan**: `ray_read_parted` → DAG scan → verify all rows returned
2. **Filter + scan**: filter on date >= partition 3 → verify only 2 partitions' rows
3. **Group by**: group by sym, sum price → verify correct totals across partitions
4. **Cancel**: start query, cancel mid-stream, verify clean error return
5. **Progress**: attach progress callback, verify total is correct and done advances

**Step 3: Verify no regression on non-parted tables**

Run full existing test suite to confirm zero overhead on the in-memory path:

```bash
make && ./rayforce.test
```
Expected: all existing tests pass unchanged.

**Step 4: Commit**

```bash
git add test/test_exec.c
git commit -m "test: end-to-end integration tests for block offloading"
```

---

## Task 9: Verify Non-Parted Performance (No Regression)

**Files:**
- No code changes — benchmarking only

**Step 1: Run existing benchmarks on non-parted in-memory tables**

```bash
make release
```

Run representative benchmarks (sort, group, filter) on in-memory tables and compare timings with the previous commit (before block offloading changes). The `OP_SCAN` path for non-parted columns must be identical — verify by inspecting the generated code:

```bash
objdump -d rayforce.test | grep -A 20 exec_node_inner | head -60
```

**Step 2: Profile with perf**

```bash
perf stat ./rayforce bench_sort.rfl
perf stat ./rayforce bench_h2o.rfl
```

Verify no timing regression.

**Step 3: Commit (benchmarks only, if adding benchmark scripts)**

```bash
git commit --allow-empty -m "perf: verified zero regression on non-parted table path"
```

---

## Task 10: Cleanup and Final Commit

**Files:**
- All modified files — review pass

**Step 1: Review all changes**

Ensure:
- [ ] `seg_mask` memory is freed in `ray_graph_free()` (only if owned, not shared)
- [ ] No memory leaks in segment loop (each partial released after merge)
- [ ] Progress tracking resets properly between queries
- [ ] Cancel flag cleared at `ray_execute` entry (already existing)
- [ ] `ray_mem_pressure()` is called but does not block (informational for Phase 2)
- [ ] No new compiler warnings with `-Wall -Wextra -Werror`

**Step 2: Full test suite**

```bash
make && ./rayforce.test
make release && ./rayforce.test
```

**Step 3: Final commit**

```bash
git add -A
git commit -m "feat: block offloading — partition-streamed execution for larger-than-RAM queries"
```

---

## Summary of Deliverables

| Task | What | Files |
|------|------|-------|
| 1 | Memory budget detection | runtime.h/c, rayforce.h |
| 2 | Segment iterator struct | ops.h |
| 3 | Partition pruning → seg_mask | ops.h, opt.c |
| 4 | Streaming scan + execute loop | ops.h, exec.c |
| 5 | Concat merge (filter/project) | internal.h, exec.c |
| 6 | Group merge | group.c, exec.c |
| 7 | Sort merge | sort.c, exec.c |
| 8 | Integration tests | test_exec.c |
| 9 | Performance regression check | benchmarks only |
| 10 | Cleanup + final review | all files |
