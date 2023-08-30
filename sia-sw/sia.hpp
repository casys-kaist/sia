#include <vector>
#include "mkl.h"
#include "mkl_lapacke.h"

namespace sindex {
inline void incremental_training(
    double *delta_a, int delta_m,
    double *delta_b, int delta_n,
    double *inserted_a, int inserted_m,
    double *inserted_b, int inserted_n,
    double **cached_matrix_ptr,
    double *model_weights
) {
    double *cached_matrix = *cached_matrix_ptr;

    // Calculate A^T * b
    std::vector<double> ATB(inserted_n, 0);
    cblas_dgemm(CblasRowMajor, CblasTrans, CblasNoTrans,
                inserted_n, 1, inserted_m, 1.0,
                inserted_a, inserted_n,
                inserted_b, 1,
                0.0, ATB.data(), 1);

    // QR Decomposition (Delta buffer)
    double *delta_tau = (double *)malloc(sizeof(double) * delta_m);
    LAPACKE_dgeqrf(LAPACK_ROW_MAJOR, delta_m, delta_n, delta_a, delta_n, delta_tau);
    free(delta_tau);

    // Merge 2 R matrices
    for (int i=0; i < delta_n; i++){
        for (int j=0; j < i; j++) {
            cached_matrix[i * delta_n + j] = 0.0;
        }
    }
    for (int i=delta_n; i < delta_n * 2; i++) {
        for (int j=delta_n; j < delta_n * 2; j++) {
            if (j < i) {
                cached_matrix[i * delta_n + (j - delta_n)] = 0.0;
            } else {
                cached_matrix[i * delta_n + (j - delta_n)] = delta_a[(i - delta_n) * delta_n + (j - delta_n)];
            }
        }
    }

    double *merge_tau = (double *)malloc(sizeof(double) * delta_n * 2);
    LAPACKE_dgeqrf(LAPACK_ROW_MAJOR, delta_n * 2, delta_n, cached_matrix, delta_n, merge_tau);
    free(merge_tau);

    // R^T Inverse
    std::vector<double> R_inv(delta_n * delta_n, 0);
    for (int i=0; i<delta_n; i++) {
        for (int j=0; j<delta_n; j++) {
        R_inv[i * delta_n + j] = cached_matrix[i * delta_n + j];
        }
    }
    LAPACKE_dtrtri(LAPACK_ROW_MAJOR, 'U', 'N', delta_n, R_inv.data(), delta_n);

    std::vector<double> R_inv_T(delta_n * delta_n, 0);
    for (int i=0; i<delta_n; i++) {
        for (int j=0; j<delta_n; j++) {
            R_inv_T[i * delta_n + j] = R_inv[j * delta_n + i];
        }
    }

    // R-1 * R-1^T
    std::vector<double> R_inv_inv_T(delta_n * delta_n, 0);
    cblas_dgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                delta_n, delta_n, delta_n, 1.0,
                R_inv.data(), delta_n,
                R_inv_T.data(), delta_n,
                0.0, R_inv_inv_T.data(), delta_n);
    
    // GEMV
    cblas_dgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                delta_n, 1, delta_n, 1.0,
                R_inv_inv_T.data(), delta_n,
                ATB.data(), 1,
                0.0, model_weights, 1);
    return;
}

inline void entire_training(
    double *a, int m,
    double *b, int n,
    double **cached_matrix_ptr,
    double *model_weights
) {
    double *cached_matrix = *cached_matrix_ptr;
    double *tau = (double *) malloc(sizeof(double) * m);

    LAPACKE_dgeqrf(LAPACK_ROW_MAJOR, m, n, a, n, tau);
    memcpy(cached_matrix, a, sizeof(double) * n * n);

    LAPACKE_dormqr(LAPACK_ROW_MAJOR, 'L', 'T', m, 1, n, a, n, tau, b, 1);
    
    LAPACKE_dtrtrs(LAPACK_ROW_MAJOR, 'U', 'N', 'N', n, 1, a, n, b, 1);

    free(tau);
    return;
}

}