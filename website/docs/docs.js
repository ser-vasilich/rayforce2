document.addEventListener('DOMContentLoaded', () => {
  'use strict';

  /* ── Search Index ────────────────────────────── */
  /* IDs must match actual h2/h3 id= attributes in each HTML file. */
  /* Pages with no section IDs use id:"" (links to page top). */
  const searchIndex = [
    {title: "Documentation", url: "index.html", sections: [
      {id: "", title: "Overview", text: "Everything you need to build, query, and analyze with Rayforce"}
    ]},
    {title: "Quick Start", url: "quick-start.html", sections: [
      {id: "", title: "Quick Start", text: "Build from source, start the REPL, run your first query, load CSV"}
    ]},
    {title: "Getting Started Tutorial", url: "tutorial.html", sections: [
      {id: "", title: "Tutorial", text: "Hands-on walkthrough: tables, queries, joins, pivot, graphs"},
      {id: "create-table", title: "Your First Table", text: "Stock trades with Symbol, Price, Qty columns"},
      {id: "filtering", title: "Filtering Rows", text: "select with where clause to filter rows"},
      {id: "grouping", title: "Grouping and Aggregation", text: "Group by column, compute aggregates like avg, sum, count"},
      {id: "sorting", title: "Sorting", text: "xasc ascending, xdesc descending sort"},
      {id: "joining", title: "Joining Tables", text: "Join tables on shared key columns, ij inner join, lj left join"},
      {id: "pivoting", title: "Pivoting", text: "Cross-tabulate data with pivot, row key, column key"},
      {id: "loading-csv", title: "Loading CSV Files", text: "read-csv with type inference and parallel parsing"},
      {id: "saving-csv", title: "Saving Results to CSV", text: "write-csv to save any table"}
    ]},
    {title: "Building a Knowledge Base with Datalog", url: "guide-datalog.html", sections: [
      {id: "", title: "Datalog Guide", text: "Model entities and relationships as triples, write rules, query"},
      {id: "modeling-triples", title: "Modeling Data as Triples", text: "Entity-attribute-value triples, datoms database"},
      {id: "querying", title: "Querying Relationships", text: "query with find and where clauses, variables with ?"},
      {id: "rules", title: "Writing Rules", text: "Derived relations, pattern matching"},
      {id: "recursive", title: "Recursive Queries", text: "Transitive closure, management hierarchy"},
      {id: "negation", title: "Negation", text: "not in where clause to exclude entities"},
      {id: "retract", title: "Retracting Facts", text: "retract-fact to remove triples"},
      {id: "sym-name", title: "Reading Sym IDs", text: "sym-name to convert symbol IDs to strings"},
      {id: "org-chart", title: "Org Chart Example", text: "Complete org chart with CEO, VPs, team leads"}
    ]},
    {title: "Analytics Cookbook", url: "guide-analytics.html", sections: [
      {id: "", title: "Analytics Cookbook", text: "Recipes for time-series, top-N, running totals, pivots"},
      {id: "time-series", title: "Time-Series Aggregation", text: "Daily sales records, total amount per day"},
      {id: "top-n", title: "Top-N Queries", text: "Find the top 3 products by revenue"},
      {id: "running-totals", title: "Running Totals", text: "Cumulative sum over values, sums"},
      {id: "pivot", title: "Pivoting Cross-Tabulation", text: "Cross-tabulate sales by region and product"},
      {id: "asof-join", title: "ASOF Joins", text: "Match each trade to most recent quote, aj temporal join"},
      {id: "window-join", title: "Window Joins", text: "Aggregate quote data within a time window, wj"},
      {id: "csv-workflow", title: "Working with CSV", text: "Load, transform, and save CSV data"}
    ]},
    {title: "Data Persistence Guide", url: "guide-storage.html", sections: [
      {id: "", title: "Storage Guide", text: "Save and load data: CSV, columnar, splayed, partitioned, mmap"},
      {id: "csv-io", title: "CSV I/O", text: "read-csv and write-csv for data exchange"},
      {id: "symbol-table", title: "Symbol Table Persistence", text: "Global symbol intern table, ray_sym_save, ray_sym_load"},
      {id: "columnar-files", title: "Columnar Files", text: "ray_col_save, ray_col_load, ray_col_mmap, native binary"},
      {id: "splayed-tables", title: "Splayed Tables", text: "ray_splay_save, ray_splay_load, one file per column"},
      {id: "partitioned-tables", title: "Partitioned Tables", text: "Date-partitioned storage, ray_part_load"},
      {id: "mmap", title: "Memory-Mapped I/O", text: "Zero-copy access, mmap, read-only vectors"},
      {id: "choosing", title: "Choosing a Storage Layer", text: "CSV vs columnar vs splayed vs partitioned"}
    ]},
    {title: "Syntax & Types", url: "rayfall-syntax.html", sections: [
      {id: "", title: "Rayfall Syntax", text: "Lisp-like query language with prefix notation, rich types"},
      {id: "atoms", title: "Atoms", text: "Integers, floats, booleans, symbols, strings, dates, times"},
      {id: "vectors", title: "Vectors", text: "Homogeneous typed arrays, square bracket syntax"},
      {id: "lists", title: "Lists", text: "Heterogeneous containers with list function"},
      {id: "tables", title: "Tables", text: "Named columns of equal-length vectors"},
      {id: "dictionaries", title: "Dictionaries", text: "Key-value mapping with dict function"},
      {id: "function-calls", title: "Function Calls", text: "Prefix notation (fn arg1 arg2), nested expressions"},
      {id: "quoting", title: "Quoting", text: "Quote to prevent evaluation, backtick for symbols"},
      {id: "comments", title: "Comments", text: "Semicolon ; starts a line comment"},
      {id: "control-flow", title: "Control Flow", text: "if cond do while for set def let"},
      {id: "lambdas", title: "Lambdas", text: "Anonymous functions with fn keyword"},
      {id: "select-update", title: "Select & Update", text: "Query verbs: select, update, where, by"},
      {id: "c-api", title: "C API", text: "ray_t abstraction, vector operations, table construction"},
      {id: "dag-execution", title: "DAG Execution", text: "Lazy DAG, optimizer, fused morsel-driven executor"},
      {id: "graph-engine", title: "Graph Engine", text: "CSR format, edge indices, graph algorithms"},
      {id: "graph-algorithms", title: "Graph Algorithms", text: "Traversal, shortest paths, centrality, joins"},
      {id: "pipeline", title: "Pipeline", text: "Type inference, constant folding, predicate pushdown, fusion"},
      {id: "memory-model", title: "Memory Model", text: "Buddy allocator, slab cache, COW, arena allocator"},
      {id: "storage", title: "Storage", text: "Column files, symbol table, CSV, file I/O"}
    ]},
    {title: "Functions Reference", url: "rayfall-functions.html", sections: [
      {id: "", title: "Functions Reference", text: "Complete reference of all Rayfall builtin functions"},
      {id: "arithmetic", title: "Arithmetic", text: "add subtract multiply divide mod abs neg ceil floor round"},
      {id: "comparison", title: "Comparison", text: "equal not-equal less greater min max clamp"},
      {id: "logic", title: "Logic", text: "and or not boolean operations"},
      {id: "aggregation", title: "Aggregation", text: "sum avg count min max first last var dev med"},
      {id: "higher-order", title: "Higher-Order", text: "each peach over scan apply functional"},
      {id: "collection", title: "Collection", text: "til count reverse where group distinct enlist raze"},
      {id: "sorting", title: "Sorting", text: "asc desc xasc xdesc iasc idesc rank"},
      {id: "table-ops", title: "Table Operations", text: "select update delete table cols meta xcol xcols"},
      {id: "query", title: "Query", text: "select update where by group-by"},
      {id: "joins", title: "Joins", text: "ij lj aj wj inner left asof window join"},
      {id: "pivot", title: "Pivot", text: "pivot unpivot cross-tabulation"},
      {id: "string-ops", title: "String Operations", text: "upper lower trim strlen substr replace concat like"},
      {id: "temporal", title: "Temporal", text: "date time timestamp year month day hour minute second"},
      {id: "type-ops", title: "Type Operations", text: "type cast null fills type-of"},
      {id: "io", title: "I/O", text: "read-csv write-csv show print"},
      {id: "control", title: "Control Flow", text: "if cond do while for def set"},
      {id: "system", title: "System", text: "eval parse exit gc env"},
      {id: "serialization", title: "Serialization", text: "save load persist data"},
      {id: "eav", title: "EAV", text: "datoms assert-fact retract-fact query"},
      {id: "table-set-ops", title: "Table Set Operations", text: "union inter except set operations"},
      {id: "datalog", title: "Datalog Functions", text: "dl-program dl-add-rule dl-run rules queries"},
      {id: "datalog-program", title: "Datalog Program", text: "dl-program-new dl-add-edb dl-eval dl-query"}
    ]},
    {title: "Scalar Types", url: "data-types.html", sections: [
      {id: "", title: "Scalar Types", text: "All scalar types: integer, float, boolean, symbol, string, date, time"},
      {id: "type-table", title: "Type Table", text: "Complete type reference with sizes and null values"},
      {id: "atoms-vectors", title: "Atoms & Vectors", text: "Scalar atoms and homogeneous typed vector arrays"},
      {id: "ray-t-header", title: "ray_t Header", text: "32-byte block header, type, attrs, len, nullmap"},
      {id: "type-casting", title: "Type Casting", text: "Cast between types, implicit and explicit conversion"},
      {id: "temporal-types", title: "Temporal Types", text: "Date, time, timestamp with nanosecond precision"},
      {id: "sym-type", title: "Symbol Type", text: "RAY_SYM interned string identifiers, dictionary encoding"},
      {id: "str-type", title: "String Type", text: "RAY_STR variable-length strings, inline or pooled"},
      {id: "c-api-types", title: "C API Types", text: "Type constants, constructors, ray_type_size"}
    ]},
    {title: "Collections", url: "data-types-collections.html", sections: [
      {id: "", title: "Collections", text: "Vectors, lists, tables, dictionaries — compound data types"},
      {id: "vectors", title: "Vectors", text: "Homogeneous typed arrays, the core column type"},
      {id: "morsel-iteration", title: "Morsel Iteration", text: "1024-element morsel-by-morsel processing"},
      {id: "null-handling", title: "Null Handling", text: "Null bitmap, RAY_ATTR_HAS_NULLS, typed nulls"},
      {id: "cow-semantics", title: "COW Semantics", text: "Copy-on-write ref counting for vectors"},
      {id: "vector-operations", title: "Vector Operations", text: "Append, set, get, slice, concat, from_raw"},
      {id: "lists", title: "Lists", text: "Heterogeneous containers for mixed types"},
      {id: "tables", title: "Tables", text: "Named columns of equal-length vectors, dataframe"},
      {id: "dictionaries", title: "Dictionaries", text: "Key-value mapping, dict function"},
      {id: "selections", title: "Selections", text: "RAY_SEL bitmaps for filtered row sets"},
      {id: "parted-types", title: "Parted Types", text: "RAY_PARTED segmented columns for partitioned tables"}
    ]},
    {title: "Select & Update", url: "queries-select.html", sections: [
      {id: "", title: "Select & Update Queries", text: "The select and update query verbs with filtering, grouping"},
      {id: "select", title: "Select", text: "Query rows with select, project columns"},
      {id: "select-basic", title: "Basic Select", text: "Simple column projection and computed columns"},
      {id: "select-filter", title: "Select with Filter", text: "where clause for row filtering"},
      {id: "select-groupby", title: "Select Group By", text: "by clause for grouping and aggregation"},
      {id: "select-sort", title: "Sorting", text: "asc desc clause for ascending descending sort order, multi-column mixed direction"},
      {id: "select-take", title: "Limiting Rows", text: "take clause for head tail range slice, positive negative, two-element vector"},
      {id: "select-combined", title: "Combining Clauses", text: "where by asc desc take combined in one query"},
      {id: "update", title: "Update", text: "Modify columns in-place with update verb"},
      {id: "insert", title: "Insert", text: "Add rows to a table"},
      {id: "upsert", title: "Upsert", text: "Insert or update matching rows"},
      {id: "delete", title: "Delete", text: "Remove rows from a table"}
    ]},
    {title: "Joins", url: "queries-joins.html", sections: [
      {id: "", title: "Join Operations", text: "Inner join, left join, asof join, window join"},
      {id: "inner-join", title: "Inner Join", text: "ij — match rows on shared key columns"},
      {id: "left-join", title: "Left Join", text: "lj — keep all left rows, fill missing with null"},
      {id: "asof-join", title: "ASOF Join", text: "aj — temporal join, match nearest prior value"},
      {id: "window-join", title: "Window Join", text: "wj — aggregate within a time window"}
    ]},
    {title: "Pivot & Window", url: "queries-pivot.html", sections: [
      {id: "", title: "Pivot & Window Functions", text: "Cross-tabulation, unpivot, window operations"},
      {id: "pivot", title: "Pivot", text: "Cross-tabulate with row key, column key, value, aggregation"},
      {id: "pivot-custom", title: "Custom Pivot", text: "Custom aggregation functions in pivot"},
      {id: "pivot-multi", title: "Multi-Column Pivot", text: "Pivot with multiple value columns"},
      {id: "window-functions", title: "Window Functions", text: "Moving average, cumulative sum, rank, lag, lead"}
    ]},
    {title: "Math & Aggregation", url: "operations-math.html", sections: [
      {id: "", title: "Math & Aggregation", text: "Arithmetic, comparison, aggregates, logic, ordering"},
      {id: "arithmetic", title: "Arithmetic", text: "add + subtract - multiply * divide % modulo"},
      {id: "comparison", title: "Comparison", text: "equal not-equal less greater min max"},
      {id: "logic", title: "Logic", text: "and or not boolean operations"},
      {id: "aggregation", title: "Aggregation", text: "sum avg count min max first last var dev med"},
      {id: "ordering", title: "Ordering", text: "asc desc sort rank"},
      {id: "generation", title: "Generation", text: "til rand roll deal random number generation"},
      {id: "higher-order", title: "Higher-Order", text: "each peach over scan functional operations"}
    ]},
    {title: "String Operations", url: "operations-string.html", sections: [
      {id: "", title: "String Operations", text: "String manipulation: upper, lower, trim, substr, replace, concat"},
      {id: "string-types", title: "String Types", text: "RAY_SYM dictionary-encoded and RAY_STR variable-length"},
      {id: "ray-sym", title: "RAY_SYM Symbols", text: "Interned identifiers, integer indices into global table"},
      {id: "ray-str", title: "RAY_STR Strings", text: "Variable-length 16-byte elements, inline or pooled"},
      {id: "null-propagation", title: "Null Propagation", text: "Null input rows produce null output rows"},
      {id: "string-functions", title: "String Functions", text: "upper lower trim ltrim rtrim strlen substr replace"},
      {id: "concat", title: "Concatenation", text: "concat join strings together"},
      {id: "like", title: "Pattern Matching", text: "like glob wildcard pattern comparison"},
      {id: "split", title: "Split", text: "Split strings by delimiter"},
      {id: "format", title: "Format", text: "String formatting and conversion"},
      {id: "dag-string-ops", title: "DAG String Ops", text: "String operations in the DAG executor"}
    ]},
    {title: "Files & Partitions", url: "storage.html", sections: [
      {id: "", title: "Storage Reference", text: "Columnar file I/O, splayed tables, partitioned storage, CSV"},
      {id: "col-files", title: "Columnar Files", text: "Native binary format, 32-byte header, direct mmap"},
      {id: "splayed-tables", title: "Splayed Tables", text: "Directory of column files, .d schema, one file per column"},
      {id: "partitioned-tables", title: "Date-Partitioned Tables", text: "Date directories, splayed tables per partition"},
      {id: "symbol-persistence", title: "Symbol Persistence", text: "ray_sym_save, ray_sym_load, intern table persistence"},
      {id: "csv", title: "CSV Import/Export", text: "ray_csv_load, write-csv, parallel parse, type inference"},
      {id: "cross-platform", title: "Cross-Platform I/O", text: "File locking, fsync, atomic rename"}
    ]},
    {title: "Core API", url: "c-api-core.html", sections: [
      {id: "", title: "C API Core", text: "ray_t abstraction, vectors, tables, atoms, memory management"},
      {id: "lifecycle", title: "Lifecycle", text: "ray_heap_init, ray_heap_destroy, ray_sym_init, ray_sym_destroy"},
      {id: "memory", title: "Memory Management", text: "ray_alloc, ray_free, ray_retain, ray_release, ray_cow"},
      {id: "atoms", title: "Atom API", text: "ray_i64, ray_f64, ray_bool, ray_sym, ray_date constructors"},
      {id: "vectors", title: "Vector API", text: "ray_vec_new, ray_vec_from_raw, ray_vec_append, ray_vec_get"},
      {id: "string-vectors", title: "String Vectors", text: "ray_str_vec_get, ray_str_vec_set, ray_str_vec_append"},
      {id: "lists", title: "Lists", text: "ray_list_new, ray_list_append, ray_list_get, ray_list_set"},
      {id: "tables", title: "Tables", text: "ray_table_new, ray_table_add_col, ray_table_get_col"},
      {id: "errors", title: "Error Handling", text: "ray_error, RAY_IS_ERR, ray_err_t error codes"}
    ]},
    {title: "DAG & Execution", url: "c-api-dag.html", sections: [
      {id: "", title: "DAG API", text: "Build lazy DAGs, chain operations, execute with fused morsel executor"},
      {id: "dag-construction", title: "DAG Construction", text: "ray_graph_new, ray_graph_free, build operation DAG"},
      {id: "source-ops", title: "Source Operations", text: "ray_scan, attach table as data source node"},
      {id: "ray-filter", title: "Filter", text: "ray_filter — predicate pushdown, boolean expression"},
      {id: "ray-group", title: "Group", text: "ray_group — group by columns with aggregate functions"},
      {id: "ray-sort-op", title: "Sort", text: "ray_sort — sort by columns ascending or descending"},
      {id: "ray-join", title: "Join", text: "ray_join — inner join on key columns"},
      {id: "ray-asof-join", title: "ASOF Join", text: "ray_asof_join — temporal join, nearest prior"},
      {id: "graph-ops", title: "Graph Operations", text: "ray_expand, ray_var_expand, ray_shortest_path"},
      {id: "optimizer-executor", title: "Optimizer & Executor", text: "ray_optimize, ray_execute, morsel-driven execution"},
      {id: "csr", title: "CSR", text: "ray_rel_build, ray_rel_from_edges, edge indices"},
      {id: "ray-rel-io", title: "Relationship I/O", text: "ray_rel_save, ray_rel_load, ray_rel_mmap persistence"},
      {id: "ray-col-io", title: "Column I/O", text: "ray_col_save, ray_col_load, ray_col_mmap column files"},
      {id: "ray-splay", title: "Splayed Tables", text: "ray_splay_save, ray_splay_load directory storage"},
      {id: "ray-csv", title: "CSV", text: "ray_csv_load parallel CSV reader"},
      {id: "datalog", title: "Datalog API", text: "dl-program, dl-add-rule, dl-eval, dl-query"},
      {id: "examples", title: "Examples", text: "Filter-group, BFS traversal, join examples"}
    ]},
    {title: "CSR Storage", url: "graph-storage.html", sections: [
      {id: "", title: "CSR Graph Storage", text: "Double-indexed CSR edge indices, forward + reverse, mmap, persistence"}
    ]},
    {title: "Graph Algorithms", url: "graph-algorithms.html", sections: [
      {id: "", title: "Graph Algorithms", text: "Traversal, shortest paths, centrality, clustering, joins"},
      {id: "traversal", title: "Traversal", text: "OP_EXPAND 1-hop, OP_VAR_EXPAND BFS variable-length paths"},
      {id: "shortest-path", title: "Shortest Path", text: "Dijkstra, A*, Yen's K-shortest paths"},
      {id: "centrality", title: "Centrality", text: "Betweenness Brandes, closeness centrality scores"},
      {id: "community", title: "Community", text: "Clustering coefficient, random walk"},
      {id: "optimal-joins", title: "Worst-Case Optimal Joins", text: "Leapfrog Triejoin LFTJ, OP_WCO_JOIN"},
      {id: "spanning", title: "Spanning Tree", text: "OP_MST Kruskal's minimum spanning tree"},
      {id: "similarity", title: "Similarity", text: "Graph similarity measures"},
      {id: "ordering", title: "Ordering", text: "Topological ordering, node ordering"},
      {id: "sip", title: "Sideways Information Passing", text: "RAY_SEL bitmap propagation through OP_EXPAND"},
      {id: "factorized", title: "Factorized Execution", text: "ray_fvec_t, ray_ftable_t avoid cross-products"}
    ]},
    {title: "Pipeline & Optimizer", url: "architecture-pipeline.html", sections: [
      {id: "", title: "Execution Pipeline", text: "Lazy DAG, optimizer passes, fused morsel-driven execution, type inference, predicate pushdown, fusion, DCE"}
    ]},
    {title: "Memory Model", url: "architecture-memory.html", sections: [
      {id: "", title: "Memory Architecture", text: "Buddy allocator, slab cache, COW ref counting, arena allocator, per-VM heaps, mmap"}
    ]},
    {title: "Block Offloading", url: "architecture-offloading.html", sections: [
      {id: "", title: "Block Offloading", text: "Larger-than-RAM query execution, partition streaming, segment iterator, memory budget"},
      {id: "how-it-works", title: "How It Works", text: "Segment detection, segment table construction, execution and merge, result assembly"},
      {id: "partition-pruning", title: "Partition Pruning", text: "Skip partitions that don't match filter predicates, seg_mask bitmap, date and integer pruning"},
      {id: "memory-budget", title: "Memory Budget", text: "Auto-detect physical RAM, 80% budget, ray_mem_budget, ray_mem_pressure"},
      {id: "streamable-ops", title: "Streamable Operations", text: "Element-wise, string, temporal, filter, select, alias operations safe for streaming"},
      {id: "limitations", title: "Current Limitations", text: "No streaming for aggregations sorts joins, literal constants only, single partition column"}
    ]},
    {title: "Datalog Rules & Queries", url: "datalog.html", sections: [
      {id: "", title: "Datalog", text: "Declarative logic programming, rules, queries, fixpoint evaluation"},
      {id: "what-is-datalog", title: "What is Datalog", text: "Declarative logic programming language for queries"},
      {id: "eav-storage", title: "EAV Storage", text: "Entity-attribute-value triple store, datoms, assert-fact"},
      {id: "rules", title: "Rules", text: "Derived relations, pattern matching"},
      {id: "queries", title: "Queries", text: "Pattern matching with find and where clauses"},
      {id: "negation", title: "Negation", text: "not in where clause to exclude entities"},
      {id: "recursive-rules", title: "Recursive Rules & Fixpoint", text: "Semi-naive evaluation, transitive closure"},
      {id: "stratification", title: "Stratification", text: "Evaluation order for negated predicates"},
      {id: "pull-queries", title: "Pull Queries", text: "Entity-centric retrieval by entity ID"},
      {id: "sym-name", title: "sym-name", text: "Convert symbol intern IDs to readable strings"},
      {id: "programmatic-api", title: "Programmatic API", text: "dl-program, dl-add-rule, dl-run for advanced use"},
      {id: "retract-fact", title: "Retracting Facts", text: "retract-fact to remove triples from database"},
      {id: "mapping", title: "How Datalog Maps to Rayforce", text: "Datalog compiles to DAG operations, filters, joins"},
      {id: "complete-example", title: "Complete Example", text: "Full Datalog workflow: EAV, rules, queries, retract"}
    ]}
  ];

  /* ── Search Logic ────────────────────────────── */
  const sidebar = document.querySelector('.docs-sidebar');
  if (!sidebar) return;

  const sections = sidebar.querySelectorAll('.sidebar-section');
  const searchBox = document.createElement('div');
  searchBox.className = 'sidebar-search';
  searchBox.innerHTML =
    '<div class="search-input-wrap">' +
      '<svg class="search-icon" viewBox="0 0 20 20" fill="none" stroke="currentColor" stroke-width="2">' +
        '<circle cx="8.5" cy="8.5" r="5.5"/><line x1="13" y1="13" x2="18" y2="18"/>' +
      '</svg>' +
      '<input type="text" class="search-input" placeholder="Search docs..." aria-label="Search documentation">' +
      '<kbd class="search-kbd">/</kbd>' +
    '</div>' +
    '<div class="search-results" style="display:none"></div>';
  sidebar.insertBefore(searchBox, sidebar.firstChild);

  const input = searchBox.querySelector('.search-input');
  const resultsEl = searchBox.querySelector('.search-results');
  const kbdEl = searchBox.querySelector('.search-kbd');

  /* Keyboard shortcut: / to focus search */
  document.addEventListener('keydown', (e) => {
    if (e.key === '/' && !e.ctrlKey && !e.metaKey && !e.altKey) {
      const tag = document.activeElement.tagName;
      if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') return;
      e.preventDefault();
      input.focus();
    }
    if (e.key === 'Escape' && document.activeElement === input) {
      input.blur();
      input.value = '';
      showNav();
    }
  });

  input.addEventListener('focus', () => { kbdEl.style.display = 'none'; });
  input.addEventListener('blur', () => { if (!input.value) kbdEl.style.display = ''; });

  function showNav() {
    resultsEl.style.display = 'none';
    sections.forEach(s => s.style.display = '');
  }

  function hideNav() {
    sections.forEach(s => s.style.display = 'none');
    resultsEl.style.display = '';
  }

  function search(query) {
    if (!query) { showNav(); return; }
    hideNav();

    const terms = query.toLowerCase().split(/\s+/).filter(Boolean);
    const hits = [];

    for (const page of searchIndex) {
      for (const sec of page.sections) {
        const haystack = (page.title + ' ' + sec.title + ' ' + sec.text).toLowerCase();
        const score = terms.reduce((s, t) => s + (haystack.includes(t) ? 1 : 0), 0);
        if (score === terms.length) {
          hits.push({page, sec, score});
        }
      }
    }

    if (hits.length === 0) {
      resultsEl.innerHTML = '<div class="search-empty">No results</div>';
      return;
    }

    /* Deduplicate: show at most 2 sections per page */
    const byPage = {};
    for (const h of hits) {
      const key = h.page.url;
      if (!byPage[key]) byPage[key] = [];
      if (byPage[key].length < 2) byPage[key].push(h);
    }

    let html = '';
    for (const key in byPage) {
      const group = byPage[key];
      const page = group[0].page;
      html += '<div class="search-page">' +
        '<div class="search-page-title">' + esc(page.title) + '</div>';
      for (const h of group) {
        const url = h.sec.id ? (page.url + '#' + h.sec.id) : page.url;
        html += '<a href="' + url + '" class="search-hit">' +
          '<span class="search-hit-title">' + highlight(h.sec.title, terms) + '</span>' +
          '<span class="search-hit-text">' + highlight(h.sec.text, terms) + '</span>' +
          '</a>';
      }
      html += '</div>';
    }
    resultsEl.innerHTML = html;
  }

  function esc(s) {
    return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  }

  function highlight(s, terms) {
    let out = esc(s);
    for (const t of terms) {
      const re = new RegExp('(' + t.replace(/[.*+?^${}()|[\]\\]/g, '\\$&') + ')', 'gi');
      out = out.replace(re, '<mark>$1</mark>');
    }
    return out;
  }

  input.addEventListener('input', () => search(input.value.trim()));

  /* Close mobile sidebar when a search result is clicked */
  resultsEl.addEventListener('click', (e) => {
    if (e.target.closest('.search-hit')) {
      sidebar.classList.remove('open');
    }
  });

  /* ── Mobile sidebar toggle ───────────────────── */
  const toggle = document.querySelector('.docs-sidebar-toggle');
  if (toggle && sidebar) {
    toggle.addEventListener('click', () => {
      const isOpen = sidebar.classList.toggle('open');
      toggle.setAttribute('aria-expanded', String(isOpen));
    });
    sidebar.querySelectorAll('.sidebar-link').forEach((link) => {
      link.addEventListener('click', () => {
        sidebar.classList.remove('open');
      });
    });
  }

  /* ── Highlight active page ───────────────────── */
  const currentPage = window.location.pathname.split('/').pop() || 'index.html';
  document.querySelectorAll('.sidebar-link').forEach((link) => {
    const href = link.getAttribute('href');
    if (href === currentPage || (currentPage === '' && href === 'index.html')) {
      link.classList.add('active');
    }
  });

  /* ── Smooth scroll for anchor links ──────────── */
  document.querySelectorAll('a[href^="#"]').forEach((anchor) => {
    anchor.addEventListener('click', (e) => {
      const target = document.querySelector(anchor.getAttribute('href'));
      if (target) {
        e.preventDefault();
        target.scrollIntoView({ behavior: 'smooth' });
      }
    });
  });

  /* ── Active section highlighting on scroll ───── */
  const headings = document.querySelectorAll('.docs-content h2[id], .docs-content h3[id]');
  if (headings.length > 0) {
    const observer = new IntersectionObserver(
      (entries) => {
        entries.forEach((entry) => {
          if (entry.isIntersecting) {
            const id = entry.target.getAttribute('id');
            document.querySelectorAll('.sidebar-link').forEach((link) => {
              link.classList.remove('active-section');
              if (link.getAttribute('href') === '#' + id) {
                link.classList.add('active-section');
              }
            });
          }
        });
      },
      { rootMargin: '-80px 0px -60% 0px', threshold: 0 }
    );
    headings.forEach((h) => observer.observe(h));
  }
});
