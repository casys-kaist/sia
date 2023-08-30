#ifndef DUMMY_THREAD_H
#define DUMMY_THREAD_H

#include "mkl.h"
#include "mkl_lapacke.h"
#include <pthread.h>
#include <mutex>
#include <unistd.h>

pthread_t dummy_thread;

std::mutex calc_end_mutex;
bool calc_end = false;

std::mutex wait_end_mutex;
bool wait_end = false;

void generate_dummy_thread();
void join_dummy_thread();
void *run_dummy_mkl(void *);

// A function that will be called before runtime
inline void generate_dummy_thread() {
    int ret = pthread_create(&dummy_thread, nullptr, run_dummy_mkl, nullptr);
    
    while(true) {
        calc_end_mutex.lock();
        if (calc_end) {
            calc_end_mutex.unlock();
            break;
        } else {
            calc_end_mutex.unlock();
        }
    }

    return;
}

// A dummy function that will not be called during runtime
void *run_dummy_mkl(void *param) {
    // Generate dummy matrix
    int m = 500;
    int n = 64;
    double *a = (double *)malloc(sizeof(double) * m * n);
    double *b = (double *)malloc(sizeof(double) * m);
    double *x = (double *)malloc(sizeof(double) * n);

    for (int i=0; i<m; i++){
        for (int j=0; j<n; j++) {
            a[i * n + j] = (double)((i + j + 1) % n);
        }
    }

    for (int i=0; i<m; i++) {
        b[i] = (double)(i + 1);
    }

    // Do calc
    lapack_int rank = 0;
    int fitting_res = LAPACKE_dgelss(LAPACK_ROW_MAJOR, m, n, 1, a, n, b, 1, x, -1, &rank);

    calc_end_mutex.lock();
    calc_end = true;
    calc_end_mutex.unlock();

    // Wait until runtime
    while(true) {
        wait_end_mutex.lock();
        if (wait_end) {
            wait_end_mutex.unlock();
            break;
        } else {
            wait_end_mutex.unlock();
        }
        sleep(1);
    }

    rank = 0;
    fitting_res = LAPACKE_dgelss(LAPACK_ROW_MAJOR, m, n, 1, a, n, b, 1, x, -1, &rank);

    free(a);
    free(b);
    free(x);

    return nullptr;
}

// A function that will be called after runtime
inline void join_dummy_thread() {
    wait_end_mutex.lock();
    wait_end = true;
    wait_end_mutex.unlock();
    
    void *status;
    pthread_join(dummy_thread, &status);
    return;
}

#endif