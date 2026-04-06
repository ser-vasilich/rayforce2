/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.

 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:

 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.

 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#include "ops/internal.h"

/* --------------------------------------------------------------------------
 * exec_cosine_sim: cosine similarity between embedding column and query vector.
 * dot(a,b) / (||a|| * ||b||) per row.
 * Input: RAY_F32 embedding column (flat N*D floats)
 * Output: RAY_F64 vector of similarities (one per row)
 * -------------------------------------------------------------------------- */
ray_t* exec_cosine_sim(ray_graph_t* g, ray_op_t* op, ray_t* emb_vec) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    const float* query = ext->vector.query_vec;
    int32_t dim = ext->vector.dim;

    if (!query || dim <= 0) return ray_error("schema", NULL);
    if (emb_vec->type != RAY_F32) return ray_error("type", NULL);

    int64_t total = emb_vec->len;
    int64_t nrows = total / dim;
    if (nrows * dim != total) return ray_error("length", NULL);

    const float* data = (const float*)ray_data(emb_vec);

    /* Precompute query norm */
    double q_norm_sq = 0.0;
    for (int32_t j = 0; j < dim; j++) {
        q_norm_sq += (double)query[j] * (double)query[j];
    }
    double q_norm = sqrt(q_norm_sq);

    /* Compute per-row similarity */
    ray_t* result = ray_vec_new(RAY_F64, nrows);
    if (!result || RAY_IS_ERR(result)) return ray_error("oom", NULL);
    result->len = nrows;
    double* out = (double*)ray_data(result);

    for (int64_t i = 0; i < nrows; i++) {
        const float* row = data + i * dim;
        double dot = 0.0;
        double r_norm_sq = 0.0;
        for (int32_t j = 0; j < dim; j++) {
            dot += (double)row[j] * (double)query[j];
            r_norm_sq += (double)row[j] * (double)row[j];
        }
        double r_norm = sqrt(r_norm_sq);
        double denom = q_norm * r_norm;
        out[i] = (denom > 0.0) ? dot / denom : 0.0;
    }

    return result;
}

/* --------------------------------------------------------------------------
 * exec_euclidean_dist: euclidean distance between embedding column and query.
 * sqrt(sum((a_i - b_i)^2)) per row.
 * -------------------------------------------------------------------------- */
ray_t* exec_euclidean_dist(ray_graph_t* g, ray_op_t* op, ray_t* emb_vec) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    const float* query = ext->vector.query_vec;
    int32_t dim = ext->vector.dim;

    if (!query || dim <= 0) return ray_error("schema", NULL);
    if (emb_vec->type != RAY_F32) return ray_error("type", NULL);

    int64_t total = emb_vec->len;
    int64_t nrows = total / dim;
    if (nrows * dim != total) return ray_error("length", NULL);

    const float* data = (const float*)ray_data(emb_vec);

    ray_t* result = ray_vec_new(RAY_F64, nrows);
    if (!result || RAY_IS_ERR(result)) return ray_error("oom", NULL);
    result->len = nrows;
    double* out = (double*)ray_data(result);

    for (int64_t i = 0; i < nrows; i++) {
        const float* row = data + i * dim;
        double sum_sq = 0.0;
        for (int32_t j = 0; j < dim; j++) {
            double d = (double)row[j] - (double)query[j];
            sum_sq += d * d;
        }
        out[i] = sqrt(sum_sq);
    }

    return result;
}

/* --------------------------------------------------------------------------
 * exec_knn: brute-force K nearest neighbors via cosine similarity.
 * Returns RAY_TABLE with _rowid (I64) and _similarity (F64), sorted desc.
 * -------------------------------------------------------------------------- */

/* Min-heap entry for KNN (track worst of top-K) */
typedef struct {
    double  sim;
    int64_t rowid;
} knn_entry_t;

static void knn_heap_insert(knn_entry_t* heap, int64_t k, int64_t* size,
                             double sim, int64_t rowid) {
    if (*size < k) {
        /* Heap not full: insert and sift up */
        int64_t i = (*size)++;
        heap[i].sim = sim;
        heap[i].rowid = rowid;
        /* Sift up (min-heap: root = lowest similarity = worst of top-K) */
        while (i > 0) {
            int64_t parent = (i - 1) / 2;
            if (heap[parent].sim <= heap[i].sim) break;
            knn_entry_t tmp = heap[parent]; heap[parent] = heap[i]; heap[i] = tmp;
            i = parent;
        }
    } else if (sim > heap[0].sim) {
        /* Better than worst in heap: replace root and sift down */
        heap[0].sim = sim;
        heap[0].rowid = rowid;
        int64_t i = 0;
        while (1) {
            int64_t left = 2*i+1, right = 2*i+2, smallest = i;
            if (left < *size && heap[left].sim < heap[smallest].sim) smallest = left;
            if (right < *size && heap[right].sim < heap[smallest].sim) smallest = right;
            if (smallest == i) break;
            knn_entry_t tmp = heap[i]; heap[i] = heap[smallest]; heap[smallest] = tmp;
            i = smallest;
        }
    }
}

ray_t* exec_knn(ray_graph_t* g, ray_op_t* op, ray_t* emb_vec) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    const float* query = ext->vector.query_vec;
    int32_t dim = ext->vector.dim;
    int64_t k = ext->vector.k;

    if (!query || dim <= 0 || k <= 0) return ray_error("schema", NULL);
    if (emb_vec->type != RAY_F32) return ray_error("type", NULL);

    int64_t total = emb_vec->len;
    int64_t nrows = total / dim;
    if (nrows * dim != total) return ray_error("length", NULL);
    if (k > nrows) k = nrows;

    const float* data = (const float*)ray_data(emb_vec);

    /* Precompute query norm */
    double q_norm_sq = 0.0;
    for (int32_t j = 0; j < dim; j++) q_norm_sq += (double)query[j] * query[j];
    double q_norm = sqrt(q_norm_sq);

    /* Min-heap for top-K */
    ray_t* heap_hdr = NULL;
    knn_entry_t* heap = (knn_entry_t*)scratch_alloc(&heap_hdr, (size_t)k * sizeof(knn_entry_t));
    if (!heap) return ray_error("oom", NULL);
    int64_t heap_size = 0;

    for (int64_t i = 0; i < nrows; i++) {
        const float* row = data + i * dim;
        double dot = 0.0, r_norm_sq = 0.0;
        for (int32_t j = 0; j < dim; j++) {
            dot += (double)row[j] * query[j];
            r_norm_sq += (double)row[j] * row[j];
        }
        double r_norm = sqrt(r_norm_sq);
        double denom = q_norm * r_norm;
        double sim = (denom > 0.0) ? dot / denom : 0.0;
        knn_heap_insert(heap, k, &heap_size, sim, i);
    }

    /* Simple insertion sort (k is small) — descending by similarity */
    for (int64_t i = 1; i < heap_size; i++) {
        knn_entry_t key = heap[i];
        int64_t j = i - 1;
        while (j >= 0 && heap[j].sim < key.sim) {
            heap[j + 1] = heap[j];
            j--;
        }
        heap[j + 1] = key;
    }

    /* Build output table: _rowid (I64), _similarity (F64) */
    ray_t* rowid_vec = ray_vec_new(RAY_I64, heap_size);
    ray_t* sim_vec   = ray_vec_new(RAY_F64, heap_size);
    if (!rowid_vec || RAY_IS_ERR(rowid_vec) || !sim_vec || RAY_IS_ERR(sim_vec)) {
        scratch_free(heap_hdr);
        if (rowid_vec && !RAY_IS_ERR(rowid_vec)) ray_release(rowid_vec);
        if (sim_vec && !RAY_IS_ERR(sim_vec)) ray_release(sim_vec);
        return ray_error("oom", NULL);
    }

    int64_t* rdata = (int64_t*)ray_data(rowid_vec);
    double*  sdata = (double*)ray_data(sim_vec);
    for (int64_t i = 0; i < heap_size; i++) {
        rdata[i] = heap[i].rowid;
        sdata[i] = heap[i].sim;
    }
    rowid_vec->len = heap_size;
    sim_vec->len   = heap_size;

    scratch_free(heap_hdr);

    ray_t* result = ray_table_new(2);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(rowid_vec);
        ray_release(sim_vec);
        return ray_error("oom", NULL);
    }
    result = ray_table_add_col(result, sym_intern_safe("_rowid", 6), rowid_vec);
    ray_release(rowid_vec);
    result = ray_table_add_col(result, sym_intern_safe("_similarity", 11), sim_vec);
    ray_release(sim_vec);

    return result;
}

ray_t* exec_hnsw_knn(ray_graph_t* g, ray_op_t* op) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    ray_hnsw_t* idx = (ray_hnsw_t*)ext->hnsw.hnsw_idx;
    const float* query = ext->hnsw.query_vec;
    int32_t dim = ext->hnsw.dim;
    int64_t k = ext->hnsw.k;
    int32_t ef = ext->hnsw.ef_search;

    if (!idx || !query || dim <= 0 || k <= 0) return ray_error("schema", NULL);

    /* Pre-allocate output arrays */
    ray_t* ids_hdr = NULL;
    int64_t* out_ids = (int64_t*)scratch_alloc(&ids_hdr, (size_t)k * sizeof(int64_t));
    if (!out_ids) return ray_error("oom", NULL);

    ray_t* dists_hdr = NULL;
    double* out_dists = (double*)scratch_alloc(&dists_hdr, (size_t)k * sizeof(double));
    if (!out_dists) { scratch_free(ids_hdr); return ray_error("oom", NULL); }

    int64_t n_found = ray_hnsw_search(idx, query, dim, k, ef, out_ids, out_dists);

    /* Build output table: _rowid (I64), _similarity (F64) */
    ray_t* rowid_vec = ray_vec_new(RAY_I64, n_found);
    ray_t* sim_vec   = ray_vec_new(RAY_F64, n_found);
    if (!rowid_vec || RAY_IS_ERR(rowid_vec) || !sim_vec || RAY_IS_ERR(sim_vec)) {
        scratch_free(ids_hdr);
        scratch_free(dists_hdr);
        if (rowid_vec && !RAY_IS_ERR(rowid_vec)) ray_release(rowid_vec);
        if (sim_vec && !RAY_IS_ERR(sim_vec)) ray_release(sim_vec);
        return ray_error("oom", NULL);
    }

    int64_t* rdata = (int64_t*)ray_data(rowid_vec);
    double*  sdata = (double*)ray_data(sim_vec);
    for (int64_t i = 0; i < n_found; i++) {
        rdata[i] = out_ids[i];
        sdata[i] = 1.0 - out_dists[i];  /* convert distance back to similarity */
    }
    rowid_vec->len = n_found;
    sim_vec->len   = n_found;

    scratch_free(ids_hdr);
    scratch_free(dists_hdr);

    ray_t* result = ray_table_new(2);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(rowid_vec);
        ray_release(sim_vec);
        return ray_error("oom", NULL);
    }
    result = ray_table_add_col(result, sym_intern_safe("_rowid", 6), rowid_vec);
    ray_release(rowid_vec);
    result = ray_table_add_col(result, sym_intern_safe("_similarity", 11), sim_vec);
    ray_release(sim_vec);

    return result;
}
