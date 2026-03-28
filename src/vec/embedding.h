#ifndef RAY_EMBEDDING_H
#define RAY_EMBEDDING_H

#include <rayforce.h>
#include <string.h>

/* ===== Embedding Column Helpers ===== */

/* An embedding column is a RAY_F32 vector of length N*D where D is the
 * embedding dimension.  D is stored in a separate I32 atom that the
 * caller keeps alongside the column.  Access helpers: */

/* Create an embedding column for N rows of D-dimensional vectors. */
ray_t* ray_embedding_new(int64_t nrows, int32_t dim);

/* Get the raw float pointer for row `row` (0-indexed). */
static inline float* ray_embedding_row(ray_t* col, int32_t dim, int64_t row) {
    return (float*)ray_data(col) + row * dim;
}

/* Set one row's embedding from a float array. */
static inline void ray_embedding_set(ray_t* col, int32_t dim,
                                     int64_t row, const float* vec) {
    float* dst = ray_embedding_row(col, dim, row);
    memcpy(dst, vec, (size_t)dim * sizeof(float));
}

/* Number of rows in an embedding column. */
static inline int64_t ray_embedding_nrows(ray_t* col, int32_t dim) {
    return col->len / dim;
}

#endif /* RAY_EMBEDDING_H */
