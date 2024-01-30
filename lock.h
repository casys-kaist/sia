#ifndef LOCK_H
#define LOCK_H

#include "test_config.h"
#include <mutex>
#include <condition_variable>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <atomic>

uint32_t training_threads = 0;
uint32_t training_iter = 0;
uint32_t ideal_training_interval = 1;
std::mutex training_threads_mutex;
std::condition_variable training_threads_cond;
pthread_t bg_thread;

bool finished = false;
pid_t throughput_pid;

// Used for SIndex
bool is_initial = true;

// Used for Latency Breakdown
typedef struct {
    double group_traversal_sum = 0.0;   // Used as tree traversal time in
    uint32_t group_traversal_count = 0; // traditional indexes
    double inference_sum = 0.0;
    uint32_t inference_count = 0;
    double linear_search_sum = 0.0;
    uint32_t linear_search_count = 0;
    double range_search_sum = 0.0;
    uint32_t range_search_count = 0;
    double buffer_search_sum = 0.0;
    uint32_t buffer_search_count = 0;
    double hash_sum = 0.0;
    uint32_t hash_count = 0;
} LatencyData_t;
thread_local LatencyData_t lt;

#define GET_INTERVAL(begin_t, end_t) (((end_t).tv_sec - (begin_t).tv_sec) + ((end_t).tv_nsec - (begin_t).tv_nsec) / 1000000000.0)

#endif