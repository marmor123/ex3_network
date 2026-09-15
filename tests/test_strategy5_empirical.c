#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <smmintrin.h>
#include <emmintrin.h>

typedef enum {
    PG_INT,
    PG_FLOAT,
    PG_DOUBLE
} DATATYPE;

typedef enum {
    PG_SUM,
    PG_MIN,
    PG_MAX,
    PG_PROD
} OPERATION;

/* Helper wrappers for unaligned loads/stores */
static inline __m128i pg_load_si128_compat(const void *p) {
    return _mm_loadu_si128((const __m128i *)p);
}
static inline void pg_store_si128_compat(void *p, __m128i v) {
    _mm_storeu_si128((__m128i *)p, v);
}

#define PG_LOAD_SI128  pg_load_si128_compat
#define PG_STORE_SI128 pg_store_si128_compat

/* ========================================================================= */
/* 1. ORIGINAL IMPLEMENTATION (from pg.c:814-870)                            */
/* ========================================================================= */
#define PG_REDUCE_LOOP_4X_ORIG(vtype, load_fn, store_fn, vec_op, scalar_op, step) do { \
    for (; i + ((step) * 4) <= count; i += ((step) * 4)) { \
        vtype vd0 = load_fn(d + i); \
        vtype vd1 = load_fn(d + i + (step)); \
        vtype vd2 = load_fn(d + i + (step) * 2); \
        vtype vd3 = load_fn(d + i + (step) * 3); \
        vtype vs0 = load_fn(s + i); \
        vtype vs1 = load_fn(s + i + (step)); \
        vtype vs2 = load_fn(s + i + (step) * 2); \
        vtype vs3 = load_fn(s + i + (step) * 3); \
        store_fn(d + i, vec_op(vd0, vs0)); \
        store_fn(d + i + (step), vec_op(vd1, vs1)); \
        store_fn(d + i + (step) * 2, vec_op(vd2, vs2)); \
        store_fn(d + i + (step) * 3, vec_op(vd3, vs3)); \
    } \
    for (; i + (step) <= count; i += (step)) { \
        vtype vd = load_fn(d + i); \
        vtype vs = load_fn(s + i); \
        store_fn(d + i, vec_op(vd, vs)); \
    } \
    for (; i < count; i++) { scalar_op; } \
} while(0)

#define PG_REDUCE_OP_CASES_ORIG(vtype, load_fn, store_fn, add_vec, min_vec, max_vec, mul_vec, step) do { \
    int i = 0; \
    if (op == PG_SUM) { \
        PG_REDUCE_LOOP_4X_ORIG(vtype, load_fn, store_fn, add_vec, d[i] += s[i], step); \
    } else if (op == PG_MIN) { \
        PG_REDUCE_LOOP_4X_ORIG(vtype, load_fn, store_fn, min_vec, if (s[i] < d[i]) d[i] = s[i], step); \
    } else if (op == PG_MAX) { \
        PG_REDUCE_LOOP_4X_ORIG(vtype, load_fn, store_fn, max_vec, if (s[i] > d[i]) d[i] = s[i], step); \
    } else if (op == PG_PROD) { \
        PG_REDUCE_LOOP_4X_ORIG(vtype, load_fn, store_fn, mul_vec, d[i] *= s[i], step); \
    } \
} while(0)

__attribute__((noinline))
void pg_reduce_buffer_orig(void *dest, const void *src, int count,
                           DATATYPE datatype, OPERATION op) {
    if (count <= 0 || !dest || !src) return;

    switch (datatype) {
        case PG_INT: {
            int32_t *d = (int32_t *)dest;
            const int32_t *s = (const int32_t *)src;
            PG_REDUCE_OP_CASES_ORIG(__m128i,
                                    PG_LOAD_SI128, PG_STORE_SI128,
                                    _mm_add_epi32, _mm_min_epi32, _mm_max_epi32, _mm_mullo_epi32, 4);
            break;
        }
        case PG_FLOAT: {
            float *d = (float *)dest;
            const float *s = (const float *)src;
            PG_REDUCE_OP_CASES_ORIG(__m128,
                                    _mm_loadu_ps, _mm_storeu_ps,
                                    _mm_add_ps, _mm_min_ps, _mm_max_ps, _mm_mul_ps, 4);
            break;
        }
        case PG_DOUBLE: {
            double *d = (double *)dest;
            const double *s = (const double *)src;
            PG_REDUCE_OP_CASES_ORIG(__m128d,
                                    _mm_loadu_pd, _mm_storeu_pd,
                                    _mm_add_pd, _mm_min_pd, _mm_max_pd, _mm_mul_pd, 2);
            break;
        }
    }
}

/* ========================================================================= */
/* 2. REFACTORED HYGIENIC IMPLEMENTATION (from roadmap:882-922)              */
/* ========================================================================= */
#define PG_REDUCE_LOOP_4X_HYGIENIC(d_ptr, s_ptr, n_elems, vtype, load_fn, store_fn, vec_op, scalar_stmt, step) do { \
    int _i = 0; \
    for (; _i + ((step) * 4) <= (n_elems); _i += ((step) * 4)) { \
        vtype _vd0 = load_fn((d_ptr) + _i); \
        vtype _vd1 = load_fn((d_ptr) + _i + (step)); \
        vtype _vd2 = load_fn((d_ptr) + _i + (step) * 2); \
        vtype _vd3 = load_fn((d_ptr) + _i + (step) * 3); \
        vtype _vs0 = load_fn((s_ptr) + _i); \
        vtype _vs1 = load_fn((s_ptr) + _i + (step)); \
        vtype _vs2 = load_fn((s_ptr) + _i + (step) * 2); \
        vtype _vs3 = load_fn((s_ptr) + _i + (step) * 3); \
        store_fn((d_ptr) + _i,            vec_op(_vd0, _vs0)); \
        store_fn((d_ptr) + _i + (step),    vec_op(_vd1, _vs1)); \
        store_fn((d_ptr) + _i + (step) * 2, vec_op(_vd2, _vs2)); \
        store_fn((d_ptr) + _i + (step) * 3, vec_op(_vd3, _vs3)); \
    } \
    for (; _i + (step) <= (n_elems); _i += (step)) { \
        vtype _vd = load_fn((d_ptr) + _i); \
        vtype _vs = load_fn((s_ptr) + _i); \
        store_fn((d_ptr) + _i, vec_op(_vd, _vs)); \
    } \
    for (; _i < (n_elems); _i++) { scalar_stmt((d_ptr), (s_ptr), _i); } \
} while(0)

#define SCALAR_OP_SUM(d, s, i)  ((d)[(i)] += (s)[(i)])
#define SCALAR_OP_MIN(d, s, i)  do { if ((s)[(i)] < (d)[(i)]) (d)[(i)] = (s)[(i)]; } while(0)
#define SCALAR_OP_MAX(d, s, i)  do { if ((s)[(i)] > (d)[(i)]) (d)[(i)] = (s)[(i)]; } while(0)
#define SCALAR_OP_PROD(d, s, i) ((d)[(i)] *= (s)[(i)])

#define PG_REDUCE_OP_CASES_HYGIENIC(d_ptr, s_ptr, n_elems, op_val, vtype, load_fn, store_fn, add_vec, min_vec, max_vec, mul_vec, step) do { \
    if ((op_val) == PG_SUM) { \
        PG_REDUCE_LOOP_4X_HYGIENIC(d_ptr, s_ptr, n_elems, vtype, load_fn, store_fn, add_vec, SCALAR_OP_SUM, step); \
    } else if ((op_val) == PG_MIN) { \
        PG_REDUCE_LOOP_4X_HYGIENIC(d_ptr, s_ptr, n_elems, vtype, load_fn, store_fn, min_vec, SCALAR_OP_MIN, step); \
    } else if ((op_val) == PG_MAX) { \
        PG_REDUCE_LOOP_4X_HYGIENIC(d_ptr, s_ptr, n_elems, vtype, load_fn, store_fn, max_vec, SCALAR_OP_MAX, step); \
    } else if ((op_val) == PG_PROD) { \
        PG_REDUCE_LOOP_4X_HYGIENIC(d_ptr, s_ptr, n_elems, vtype, load_fn, store_fn, mul_vec, SCALAR_OP_PROD, step); \
    } \
} while(0)

__attribute__((noinline))
void pg_reduce_buffer_hygienic(void *dest, const void *src, int count,
                              DATATYPE datatype, OPERATION op) {
    if (count <= 0 || !dest || !src) return;

    switch (datatype) {
        case PG_INT: {
            int32_t *d = (int32_t *)dest;
            const int32_t *s = (const int32_t *)src;
            PG_REDUCE_OP_CASES_HYGIENIC(d, s, count, op, __m128i,
                                        PG_LOAD_SI128, PG_STORE_SI128,
                                        _mm_add_epi32, _mm_min_epi32, _mm_max_epi32, _mm_mullo_epi32, 4);
            break;
        }
        case PG_FLOAT: {
            float *d = (float *)dest;
            const float *s = (const float *)src;
            PG_REDUCE_OP_CASES_HYGIENIC(d, s, count, op, __m128,
                                        _mm_loadu_ps, _mm_storeu_ps,
                                        _mm_add_ps, _mm_min_ps, _mm_max_ps, _mm_mul_ps, 4);
            break;
        }
        case PG_DOUBLE: {
            double *d = (double *)dest;
            const double *s = (const double *)src;
            PG_REDUCE_OP_CASES_HYGIENIC(d, s, count, op, __m128d,
                                        _mm_loadu_pd, _mm_storeu_pd,
                                        _mm_add_pd, _mm_min_pd, _mm_max_pd, _mm_mul_pd, 2);
            break;
        }
    }
}

int main(void) {
    printf("=== STRATEGY 5 EMPIRICAL VERIFICATION HARNESS ===\n");

    const int test_sizes[] = { 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 255, 256, 1024 };
    const size_t num_sizes = sizeof(test_sizes) / sizeof(test_sizes[0]);
    int total_tests = 0;

    /* 1. Test PG_INT */
    printf("\n[Test 3.1] Testing PG_INT across all 4 operations...\n");
    for (int op = PG_SUM; op <= PG_PROD; op++) {
        for (size_t s_idx = 0; s_idx < num_sizes; s_idx++) {
            int n = test_sizes[s_idx];
            int32_t *d_orig = malloc(n * sizeof(int32_t));
            int32_t *d_hyg  = malloc(n * sizeof(int32_t));
            int32_t *s      = malloc(n * sizeof(int32_t));

            for (int k = 0; k < n; k++) {
                d_orig[k] = d_hyg[k] = (rand() % 1000) - 500;
                s[k] = (rand() % 1000) - 500;
                if (op == PG_PROD) {
                    /* keep small to avoid integer overflow */
                    d_orig[k] = d_hyg[k] = (rand() % 5) + 1;
                    s[k] = (rand() % 5) + 1;
                }
            }

            pg_reduce_buffer_orig(d_orig, s, n, PG_INT, (OPERATION)op);
            pg_reduce_buffer_hygienic(d_hyg, s, n, PG_INT, (OPERATION)op);

            assert(memcmp(d_orig, d_hyg, n * sizeof(int32_t)) == 0);
            free(d_orig); free(d_hyg); free(s);
            total_tests++;
        }
    }
    printf("  -> PASS: All PG_INT tests bit-for-bit identical!\n");

    /* 2. Test PG_FLOAT */
    printf("\n[Test 3.2] Testing PG_FLOAT across all 4 operations...\n");
    for (int op = PG_SUM; op <= PG_PROD; op++) {
        for (size_t s_idx = 0; s_idx < num_sizes; s_idx++) {
            int n = test_sizes[s_idx];
            float *d_orig = malloc(n * sizeof(float));
            float *d_hyg  = malloc(n * sizeof(float));
            float *s      = malloc(n * sizeof(float));

            for (int k = 0; k < n; k++) {
                d_orig[k] = d_hyg[k] = (float)((rand() % 1000) - 500) / 10.0f;
                s[k] = (float)((rand() % 1000) - 500) / 10.0f;
            }

            pg_reduce_buffer_orig(d_orig, s, n, PG_FLOAT, (OPERATION)op);
            pg_reduce_buffer_hygienic(d_hyg, s, n, PG_FLOAT, (OPERATION)op);

            assert(memcmp(d_orig, d_hyg, n * sizeof(float)) == 0);
            free(d_orig); free(d_hyg); free(s);
            total_tests++;
        }
    }
    printf("  -> PASS: All PG_FLOAT tests bit-for-bit identical!\n");

    /* 3. Test PG_DOUBLE */
    printf("\n[Test 3.3] Testing PG_DOUBLE across all 4 operations...\n");
    for (int op = PG_SUM; op <= PG_PROD; op++) {
        for (size_t s_idx = 0; s_idx < num_sizes; s_idx++) {
            int n = test_sizes[s_idx];
            double *d_orig = malloc(n * sizeof(double));
            double *d_hyg  = malloc(n * sizeof(double));
            double *s      = malloc(n * sizeof(double));

            for (int k = 0; k < n; k++) {
                d_orig[k] = d_hyg[k] = (double)((rand() % 10000) - 5000) / 100.0;
                s[k] = (double)((rand() % 10000) - 5000) / 100.0;
            }

            pg_reduce_buffer_orig(d_orig, s, n, PG_DOUBLE, (OPERATION)op);
            pg_reduce_buffer_hygienic(d_hyg, s, n, PG_DOUBLE, (OPERATION)op);

            assert(memcmp(d_orig, d_hyg, n * sizeof(double)) == 0);
            free(d_orig); free(d_hyg); free(s);
            total_tests++;
        }
    }
    printf("  -> PASS: All PG_DOUBLE tests bit-for-bit identical!\n");

    printf("\nTotal numerical test suites executed: %d\n", total_tests);
    printf(">>> STRATEGY 5 VERIFICATION COMPLETE: ALL CHECKS PASSED <<<\n");
    return 0;
}
