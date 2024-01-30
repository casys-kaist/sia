/*
 * The code is part of the SIndex project.
 *
 *    Copyright (C) 2020 Institute of Parallel and Distributed Systems (IPADS),
 * Shanghai Jiao Tong University. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "../test_config.h"
#include "../lock.h"

#include <getopt.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "helper.h"
#include "sindex.h"
#include "sindex_impl.h"
#include "time.h"
#include <sstream>
#include <iostream>

#include <signal.h>
#include "../zipf.hpp"

struct alignas(CACHELINE_SIZE) FGParam;
template <size_t len>
class StrKey;

typedef FGParam fg_param_t;
typedef StrKey<MAX_KEY_SIZE> index_key_t;
typedef sindex::SIndex<index_key_t, uint64_t> sindex_t;

inline void prepare_sindex(sindex_t *&table);

void run_benchmark(sindex_t *table, size_t sec);

void *run_fg(void *param);

inline void parse_args(int, char **);

// parameters
double read_ratio = 1;
double insert_ratio = 0;
double update_ratio = 0;
double delete_ratio = 0;
double scan_ratio = 0;
size_t initial_size = 1000000;
size_t table_size = 150000000;
size_t target_size = 100000000;
size_t runtime = 10;
size_t fg_n = 1;
size_t bg_n = 1;
size_t seed = SEED;

volatile bool running = false;
std::atomic<size_t> ready_threads(0);
std::vector<index_key_t> exist_keys;
std::vector<index_key_t> non_exist_keys;

struct alignas(CACHELINE_SIZE) FGParam {
  sindex_t *table;
  uint64_t throughput;
  uint32_t thread_id;

  double latency_sum;
  int latency_count;
#ifdef LATENCY_BREAKDOWN
  double group_traversal_sum = 0.0;
  uint32_t group_traversal_count = 0;
  double inference_sum = 0.0;
  uint32_t inference_count = 0;
  double linear_search_sum = 0.0;
  uint32_t linear_search_count = 0;
  double range_search_sum = 0.0;
  uint32_t range_search_count = 0;
  double buffer_search_sum = 0.0;
  uint32_t buffer_search_count = 0;
#endif
};

template <size_t len>
class StrKey {
  typedef std::array<double, len> model_key_t;

 public:
  static constexpr size_t model_key_size() { return len; }

  static StrKey max() {
    static StrKey max_key;
    memset(&max_key.buf, 255, len);
    return max_key;
  }
  static StrKey min() {
    static StrKey min_key;
    memset(&min_key.buf, 0, len);
    return min_key;
  }

  StrKey() { memset(&buf, 0, len); }
  StrKey(uint64_t key) { COUT_N_EXIT("str key no uint64"); }
  StrKey(const std::string &s) {
    memset(&buf, 0, len);
    memcpy(&buf, s.data(), s.size());
  }
  StrKey(const StrKey &other) { memcpy(&buf, &other.buf, len); }
  StrKey &operator=(const StrKey &other) {
    memcpy(&buf, &other.buf, len);
    return *this;
  }

  model_key_t to_model_key() const {
    model_key_t model_key;
    for (size_t i = 0; i < len; i++) {
      model_key[i] = buf[i];
    }
    return model_key;
  }

  void get_model_key(size_t begin_f, size_t l, double *target) const {
    for (size_t i = 0; i < l; i++) {
      target[i] = buf[i + begin_f];
    }
  }

  bool less_than(const StrKey &other, size_t begin_i, size_t l) const {
    return memcmp(buf + begin_i, other.buf + begin_i, l) < 0;
  }

  friend bool operator<(const StrKey &l, const StrKey &r) {
    return memcmp(&l.buf, &r.buf, len) < 0;
  }
  friend bool operator>(const StrKey &l, const StrKey &r) {
    return memcmp(&l.buf, &r.buf, len) > 0;
  }
  friend bool operator>=(const StrKey &l, const StrKey &r) {
    return memcmp(&l.buf, &r.buf, len) >= 0;
  }
  friend bool operator<=(const StrKey &l, const StrKey &r) {
    return memcmp(&l.buf, &r.buf, len) <= 0;
  }
  friend bool operator==(const StrKey &l, const StrKey &r) {
    return memcmp(&l.buf, &r.buf, len) == 0;
  }
  friend bool operator!=(const StrKey &l, const StrKey &r) {
    return memcmp(&l.buf, &r.buf, len) != 0;
  }

  friend std::ostream &operator<<(std::ostream &os, const StrKey &key) {
    os << "key [" << std::hex;
    for (size_t i = 0; i < sizeof(StrKey); i++) {
      os << key.buf[i] << " ";
    }
    os << "] (as byte)" << std::dec;
    return os;
  }

  uint8_t buf[len];
} PACKED;

int main(int argc, char **argv) {
  parse_args(argc, argv);
  sindex_t *tab_xi;
  prepare_sindex(tab_xi);

  is_initial = false;

  run_benchmark(tab_xi, runtime);
  if (tab_xi != nullptr) delete tab_xi;
}

std::mt19937 global_gen(seed);
std::uniform_int_distribution<int64_t> global_rand_int8(
    0, std::numeric_limits<uint8_t>::max());
std::uniform_int_distribution<uint64_t> global_rand_uint64(
    0, std::numeric_limits<uint64_t>::max());

std::uniform_real_distribution<double> global_ratio_dis(0, 1);


void key_gen(uint8_t *buf) {
  for (size_t j=0; j<sizeof(index_key_t); j++){
    buf[j] = (uint8_t)global_rand_int8(global_gen);
  }
  return;
}

inline void prepare_sindex(sindex_t *&table) {
  exist_keys.reserve(initial_size);
  for (size_t i = 0; i < initial_size; ++i) {
      index_key_t k;
      key_gen(k.buf);
      exist_keys.push_back(k);
  }

    if (insert_ratio > 0) {
        non_exist_keys.reserve(table_size);
        for (size_t i = 0; i < table_size; ++i) {
            index_key_t k;
            key_gen(k.buf);
            non_exist_keys.push_back(k);
        }
    }

  COUT_VAR(exist_keys.size());
  COUT_VAR(non_exist_keys.size());

  // initilize SIndex (sort keys first)
  std::sort(exist_keys.begin(), exist_keys.end());
#ifdef SEQUENTIAL_DIST
  std::sort(non_exist_keys.begin(), non_exist_keys.end());
#endif
#ifdef HOTSPOT_DIST
  std::sort(non_exist_keys.begin(), non_exist_keys.end());
#endif
#ifdef EXPONENT_DIST
  std::sort(non_exist_keys.begin(), non_exist_keys.end());
  // Shuffle with Exponential distribution
  std::mt19937 gen(seed);
  std::exponential_distribution<double> exp_dis(EXP_LAMBDA);
  std::vector<std::pair<double, index_key_t>> values;
  for (const index_key_t& s : non_exist_keys) {
    values.push_back(std::make_pair(exp_dis(gen), s));
  }
  std::sort(values.begin(), values.end());
  for(size_t i=0; i<non_exist_keys.size(); i++){
    non_exist_keys[i] = values[i].second;
  }
#endif
#ifdef ZIPF_DIST
  std::sort(non_exist_keys.begin(), non_exist_keys.end());
  // Shuffle with zipfian distribution
  std::default_random_engine generator;
  zipfian_int_distribution<int>::param_type p(1, 1e6, 0.99, 27.000);
  zipfian_int_distribution<int> zipf_dis(p);
  std::vector<std::pair<double, index_key_t>> values;
  for (const index_key_t& s : non_exist_keys) {
    double z = (double)(zipf_dis(generator)) / (double)1e6;
    values.push_back(std::make_pair(z, s));
  }
  std::sort(values.begin(), values.end());
  for(size_t i=0; i<non_exist_keys.size(); i++){
    non_exist_keys[i] = values[i].second;
  }
#endif
  std::vector<uint64_t> vals(exist_keys.size(), 1);
  table = new sindex_t(exist_keys, vals, fg_n, 1);
}

void segfault_handler(int signum) {
    return;
}

void *run_fg(void *param) {
    fg_param_t &thread_param = *(fg_param_t *)param;
    uint32_t thread_id = thread_param.thread_id;
    sindex_t *table = thread_param.table;

    signal(SIGSEGV, segfault_handler);

    std::mt19937 gen(seed);
    std::uniform_real_distribution<double> ratio_dis(0, 1);

    size_t exist_key_n_per_thread = exist_keys.size() / fg_n;
    size_t exist_key_start = thread_id * exist_key_n_per_thread;
    size_t exist_key_end = (thread_id + 1) * exist_key_n_per_thread;
    std::vector<index_key_t> op_keys(exist_keys.begin() + exist_key_start,
                                    exist_keys.begin() + exist_key_end);

    size_t non_exist_key_index = op_keys.size();
    size_t non_exist_key_n_per_thread = exist_key_n_per_thread;
    size_t non_exist_key_start = 0;
    size_t non_exist_key_end = exist_key_n_per_thread;
    if (non_exist_keys.size() > 0) {
        non_exist_key_n_per_thread = non_exist_keys.size() / fg_n;
        non_exist_key_start = thread_id * non_exist_key_n_per_thread,
        non_exist_key_end = (thread_id + 1) * non_exist_key_n_per_thread;
        op_keys.insert(op_keys.end(), non_exist_keys.begin() + non_exist_key_start,
                    non_exist_keys.begin() + non_exist_key_end);
    }

    COUT_THIS("[micro] Worker" << thread_id << " Ready.");

    ready_threads++;
    uint64_t dummy_value = 1234;

    const size_t end_i = op_keys.size();
#ifdef SEQUENTIAL_DIST
    size_t insert_i = exist_key_n_per_thread;
    size_t read_i = 0;
    size_t delete_i = 0;
    size_t update_i = 0;
#endif
#ifdef UNIFORM_DIST
    size_t insert_i = exist_key_n_per_thread;
    size_t read_i = insert_i;
#endif
#ifdef LATEST_DIST
    #define LATEST_KEY_NUM 10
    size_t insert_i = exist_key_n_per_thread;
    std::vector<index_key_t> latest_keys;
    for (int i=0; i<LATEST_KEY_NUM; i++) {
        latest_keys.push_back(op_keys[insert_i]);
        table->put(op_keys[insert_i++], dummy_value, thread_id);
    }
#endif
#ifdef HOTSPOT_DIST
    size_t hotspot_start = ratio_dis(gen) * non_exist_key_n_per_thread + non_exist_key_index;
    size_t hotspot_end = std::min(hotspot_start + HOTSPOT_LENGTH, end_i) - 1;
#endif
#ifdef EXPONENT_DIST
    std::exponential_distribution<double> exp_dis(EXP_LAMBDA);
    size_t insert_i = exist_key_n_per_thread;
    size_t read_i = insert_i;
#endif
#ifdef ZIPF_DIST
    std::default_random_engine generator;
    zipfian_int_distribution<int>::param_type p(1, 1e6, 0.99, 27.000);
    zipfian_int_distribution<int> zipf_dis(p);
    size_t insert_i = exist_key_n_per_thread;
    size_t read_i = insert_i;
#endif

    while (!running)
        ;

    struct timespec begin_t, end_t;

    while (running) {
        if (training_threads > 0) {
        std::unique_lock<std::mutex> lck(training_threads_mutex);             // Acquire lock
        training_threads_cond.wait(lck, []{return (training_threads == 0);}); // Release lock & wait
        }

        volatile double d = ratio_dis(gen);
        volatile double p = ratio_dis(gen);
        
    #ifdef EXPONENT_DIST
        volatile double e = exp_dis(gen);
    #endif
    #ifdef ZIPF_DIST
        volatile double z = (double)(zipf_dis(generator)) / (double)1e6;
    #endif

        clock_gettime(CLOCK_MONOTONIC, &begin_t);
      
        dummy_value = (int64_t)(1234 * p);
        if (d <= read_ratio) {  // get
            #ifdef SEQUENTIAL_DIST
                table->get(op_keys[(read_i + delete_i) % end_i], dummy_value, thread_id);
                read_i++;
                if (unlikely(read_i == end_i)) read_i = 0;
            #endif
            #ifdef UNIFORM_DIST
                table->get(op_keys[p * read_i - 1], dummy_value, thread_id);
            #endif
            #ifdef LATEST_DIST
                table->get(latest_keys[p * LATEST_KEY_NUM], dummy_value, thread_id);
            #endif
            #ifdef HOTSPOT_DIST
                table->get(op_keys[hotspot_start + (hotspot_end - hotspot_start) * p], dummy_value, thread_id);
            #endif
            #ifdef EXPONENT_DIST
                table->get(op_keys[e * read_i - 1], dummy_value, thread_id);
            #endif
            #ifdef ZIPF_DIST
                table->get(op_keys[z * read_i - 1], dummy_value, thread_id);
            #endif
        } else if (d <= read_ratio + update_ratio) {  // update
            #ifdef SEQUENTIAL_DIST
                table->put(op_keys[(update_i + delete_i) % end_i], dummy_value, thread_id);
                update_i++;
                if (unlikely(update_i == end_i)) update_i = 0;
            #endif
            #ifdef UNIFORM_DIST
                table->put(op_keys[p * insert_i - 1], dummy_value, thread_id);
            #endif
            #ifdef LATEST_DIST
                table->put(latest_keys[p * LATEST_KEY_NUM], dummy_value, thread_id);
            #endif
            #ifdef HOTSPOT_DIST
                table->put(op_keys[hotspot_start + (hotspot_end - hotspot_start) * p], dummy_value, thread_id);
            #endif
            #ifdef EXPONENT_DIST
                table->put(op_keys[e * insert_i - 1], dummy_value, thread_id);
            #endif
            #ifdef ZIPF_DIST
                table->put(op_keys[z * insert_i - 1], dummy_value, thread_id);
            #endif
        } else if (d <= read_ratio + update_ratio + insert_ratio) {  // insert
            #ifdef SEQUENTIAL_DIST
                table->put(op_keys[insert_i], dummy_value, thread_id);
                insert_i++;
                if (unlikely(insert_i == end_i)) insert_i = 0;
            #endif
            #ifdef UNIFORM_DIST
                table->put(op_keys[insert_i], dummy_value, thread_id);
                insert_i++;
                read_i = std::max(read_i, insert_i);
                if (unlikely(insert_i == end_i)) insert_i = 0;
            #endif
            #ifdef LATEST_DIST
                table->put(op_keys[insert_i], dummy_value, thread_id);
                latest_keys.pop_back();
                latest_keys.insert(latest_keys.begin(), op_keys[insert_i]);
                insert_i++;
                if (unlikely(insert_i == end_i)) insert_i = 0;
            #endif
            #ifdef HOTSPOT_DIST
                table->put(op_keys[hotspot_start + (hotspot_end - hotspot_start) * p], dummy_value, thread_id);
            #endif
            #ifdef EXPONENT_DIST
                table->put(op_keys[insert_i], dummy_value, thread_id);
                insert_i++;
                read_i = std::max(read_i, insert_i);
                if (unlikely(insert_i == end_i)) insert_i = 0;
            #endif
            #ifdef ZIPF_DIST
                table->put(op_keys[insert_i], dummy_value, thread_id);
                insert_i++;
                read_i = std::max(read_i, insert_i);
                if (unlikely(insert_i == end_i)) insert_i = 0;
            #endif
        } else if (d <= read_ratio + update_ratio + insert_ratio + delete_ratio) {  // remove
            #ifdef SEQUENTIAL_DIST
                table->remove(op_keys[delete_i], thread_id);
                delete_i++;
                if (unlikely(delete_i == end_i)) delete_i = 0;
            #endif
            #ifdef UNIFORM_DIST
                table->remove(op_keys[p * insert_i], thread_id);
            #endif
            #ifdef LATEST_DIST
                table->remove(op_keys[p * insert_i], thread_id);
            #endif
            #ifdef HOTSPOT_DIST
                table->remove(op_keys[hotspot_start + (hotspot_end - hotspot_start) * p], thread_id);
            #endif
            #ifdef EXPONENT_DIST
                table->remove(op_keys[e * insert_i], thread_id);
            #endif
            #ifdef ZIPF_DIST
                table->remove(op_keys[z * insert_i], thread_id);
            #endif
        } else {  // scan
            std::vector<std::pair<index_key_t, uint64_t>> results;
            #ifdef SEQUENTIAL_DIST
                table->scan(op_keys[(read_i + delete_i) % end_i], 10, results, thread_id);
                read_i++;
                if (unlikely(read_i == insert_i)) read_i = 0;
            #endif
            #ifdef UNIFORM_DIST
                table->scan(op_keys[p * read_i], 10, results, thread_id);
            #endif
            #ifdef LATEST_DIST
                table->scan(latest_keys[p * LATEST_KEY_NUM], 10, results, thread_id);
            #endif
            #ifdef HOTSPOT_DIST
                table->scan(op_keys[hotspot_start + (hotspot_end - hotspot_start) * p], 10, results, thread_id);
            #endif
            #ifdef EXPONENT_DIST
                table->scan(op_keys[e * read_i], 10, results, thread_id);
            #endif
            #ifdef ZIPF_DIST
                table->scan(op_keys[z * read_i], 10, results, thread_id);
            #endif
        }
        clock_gettime(CLOCK_MONOTONIC, &end_t);
        thread_param.latency_sum += (end_t.tv_sec - begin_t.tv_sec) + (end_t.tv_nsec - begin_t.tv_nsec) / 1000000000.0;
        thread_param.latency_count++;
        thread_param.throughput++;
    }

    #ifdef LATENCY_BREAKDOWN
    thread_param.group_traversal_sum = lt.group_traversal_sum;
    thread_param.group_traversal_count = lt.group_traversal_count;
    thread_param.inference_sum = lt.inference_sum;
    thread_param.inference_count = lt.inference_count;
    thread_param.linear_search_sum = lt.linear_search_sum;
    thread_param.linear_search_count = lt.linear_search_count;
    thread_param.range_search_sum = lt.range_search_sum;
    thread_param.range_search_count = lt.range_search_count;
    thread_param.buffer_search_sum = lt.buffer_search_sum;
    thread_param.buffer_search_count = lt.buffer_search_count;
    #endif

    pthread_exit(nullptr);
}

void sig_handler(int signum) {
    return;
}

void run_benchmark(sindex_t *table, size_t sec) {
  pthread_t threads[fg_n];
  fg_param_t fg_params[fg_n];
  // check if parameters are cacheline aligned
  for (size_t i = 0; i < fg_n; i++) {
    if ((uint64_t)(&(fg_params[i])) % CACHELINE_SIZE != 0) {
      COUT_N_EXIT("wrong parameter address: " << &(fg_params[i]));
    }
  }

  signal(SIGALRM, sig_handler);
  throughput_pid = getpid();

  running = false;
  for (size_t worker_i = 0; worker_i < fg_n; worker_i++) {
    fg_params[worker_i].table = table;
    fg_params[worker_i].thread_id = worker_i;
    fg_params[worker_i].throughput = 0;
    fg_params[worker_i].latency_count = 0;
    fg_params[worker_i].latency_sum = 0.0;
    int ret = pthread_create(&threads[worker_i], nullptr, run_fg,
                             (void *)&fg_params[worker_i]);
    if (ret) {
      COUT_N_EXIT("Error:" << ret);
    }
  }

  COUT_THIS("[micro] prepare data ...");
  while (ready_threads < fg_n) sleep(1);

  running = true;
  std::vector<size_t> tput_history(fg_n, 0);
  struct timespec begin_t, end_t;
  uint64_t total_keys = initial_size;
  double current_sec = 0.0;

  while (current_sec < sec) {
    if (training_threads > 0) {
      std::unique_lock<std::mutex> lck(training_threads_mutex);             // Acquire lock
      training_threads_cond.wait(lck, []{return (training_threads == 0);}); // Release lock & wait
    }
    
    clock_gettime(CLOCK_MONOTONIC, &begin_t);
    sleep(1);   // Sleep will immediately return when the thread got SIGALRM signal
    clock_gettime(CLOCK_MONOTONIC, &end_t);
    double interval = (end_t.tv_sec - begin_t.tv_sec) + (end_t.tv_nsec - begin_t.tv_nsec) / 1000000000.0;

    uint64_t tput = 0;
    for (size_t i = 0; i < fg_n; i++) {
        tput += fg_params[i].throughput - tput_history[i];
        tput_history[i] = fg_params[i].throughput;
    }

    total_keys += tput * insert_ratio;
    if ((insert_ratio != 0) && (total_keys >= target_size)) {
        std::ostringstream throughput_buf;
        current_sec += interval;
        throughput_buf << "[micro] >>> sec " << current_sec << " target throughput: " << (int)(tput / interval) << std::endl;
        std::cout << throughput_buf.str();
        std::flush(std::cout);
    } else {
        std::ostringstream throughput_buf;
        current_sec += interval;
        throughput_buf << "[micro] >>> sec " << current_sec << " throughput: " << (int)(tput / interval) << std::endl;
        std::cout << throughput_buf.str();
        std::flush(std::cout);
    }
  }

  running = false;
  void *status;

  double all_latency_sum = 0.0;
  int all_latency_count = 0;
  for (size_t i = 0; i < fg_n; i++) {
    all_latency_count += fg_params[i].latency_count;
    all_latency_sum += fg_params[i].latency_sum;

    #ifdef LATENCY_BREAKDOWN
    lt.group_traversal_sum = fg_params[i].group_traversal_sum;
    lt.group_traversal_count = fg_params[i].group_traversal_count;
    lt.inference_sum = fg_params[i].inference_sum;
    lt.inference_count = fg_params[i].inference_count;
    lt.linear_search_sum = fg_params[i].linear_search_sum;
    lt.linear_search_count = fg_params[i].linear_search_count;
    lt.range_search_sum = fg_params[i].range_search_sum;
    lt.range_search_count = fg_params[i].range_search_count;
    lt.buffer_search_sum = fg_params[i].buffer_search_sum;
    lt.buffer_search_count = fg_params[i].buffer_search_count;
    #endif

    //int rc = pthread_join(threads[i], &status);
    //if (rc) {
    //  COUT_N_EXIT("Error:unable to join," << rc);
    //}
  }

  size_t throughput = 0;
  for (auto &p : fg_params) {
    throughput += p.throughput;
  }
  
  #ifndef LATENCY_BREAKDOWN
  std::ostringstream final_buf;
  final_buf << "[micro] Throughput(op/s): " << (int)(throughput / current_sec) << std::endl;
  final_buf << "[micro] Latency: " << (all_latency_sum / all_latency_count) << std::endl;
  std::cout << final_buf.str();
  std::flush(std::cout);
  #endif

  #ifdef LATENCY_BREAKDOWN
  std::ostringstream latency_buf;
  latency_buf << "[micro] group traverse latency: " << lt.group_traversal_sum / lt.group_traversal_count << std::endl;
  latency_buf << "[micro] inference latency: " << lt.inference_sum / lt.inference_count << std::endl;
  latency_buf << "[micro] linear search latency: " << lt.linear_search_sum / lt.linear_search_count << std::endl;
  latency_buf << "[micro] range search latency: " << lt.range_search_sum / lt.range_search_count << std::endl;
  latency_buf << "[micro] buffer search latency: " << lt.buffer_search_sum / lt.buffer_search_count << std::endl;
  std::cout << latency_buf.str();
  std::flush(std::cout); 
  #endif
}

inline void parse_args(int argc, char **argv) {
  struct option long_options[] = {
      {"read", required_argument, 0, 'a'},
      {"insert", required_argument, 0, 'b'},
      {"remove", required_argument, 0, 'c'},
      {"update", required_argument, 0, 'd'},
      {"scan", required_argument, 0, 'e'},
      {"table-size", required_argument, 0, 'f'},
      {"runtime", required_argument, 0, 'g'},
      {"fg", required_argument, 0, 'h'},
      {"bg", required_argument, 0, 'i'},
      {"sindex-root-err-bound", required_argument, 0, 'j'},
      {"sindex-root-memory", required_argument, 0, 'k'},
      {"sindex-group-err-bound", required_argument, 0, 'l'},
      {"sindex-group-err-tolerance", required_argument, 0, 'm'},
      {"sindex-buf-size-bound", required_argument, 0, 'n'},
      {"sindex-buf-compact-threshold", required_argument, 0, 'o'},
      {"sindex-partial-len", required_argument, 0, 'p'},
      {"sindex-forward-step", required_argument, 0, 'q'},
      {"sindex-backward-step", required_argument, 0, 'r'},
      {"initial-size", required_argument, 0, 'x'},
      {"target-size", required_argument, 0, 'y'},
      {"ideal-training-time", required_argument, 0, 'z'},
      {0, 0, 0, 0}};
  std::string ops = "a:b:c:d:e:f:g:h:i:j:k:l:m:n:o:p:q:r:x:y:z:";
  int option_index = 0;

  while (1) {
    int c = getopt_long(argc, argv, ops.c_str(), long_options, &option_index);
    if (c == -1) break;

    switch (c) {
      case 0:
        if (long_options[option_index].flag != 0) break;
        abort();
        break;
      case 'a':
        read_ratio = strtod(optarg, NULL);
        INVARIANT(read_ratio >= 0 && read_ratio <= 1);
        break;
      case 'b':
        insert_ratio = strtod(optarg, NULL);
        INVARIANT(insert_ratio >= 0 && insert_ratio <= 1);
        break;
      case 'c':
        delete_ratio = strtod(optarg, NULL);
        INVARIANT(delete_ratio >= 0 && delete_ratio <= 1);
        break;
      case 'd':
        update_ratio = strtod(optarg, NULL);
        INVARIANT(update_ratio >= 0 && update_ratio <= 1);
        break;
      case 'e':
        scan_ratio = strtod(optarg, NULL);
        INVARIANT(scan_ratio >= 0 && scan_ratio <= 1);
        break;
      case 'f':
        table_size = strtoul(optarg, NULL, 10);
        INVARIANT(table_size > 0);
        break;
      case 'g':
        runtime = strtoul(optarg, NULL, 10);
        INVARIANT(runtime > 0);
        break;
      case 'h':
        fg_n = strtoul(optarg, NULL, 10);
        INVARIANT(fg_n > 0);
        break;
      case 'i':
        bg_n = strtoul(optarg, NULL, 10);
        break;
      case 'j':
        sindex::config.root_error_bound = strtol(optarg, NULL, 10);
        INVARIANT(sindex::config.root_error_bound > 0);
        break;
      case 'k':
        sindex::config.root_memory_constraint =
            strtol(optarg, NULL, 10) * 1024 * 1024;
        INVARIANT(sindex::config.root_memory_constraint > 0);
        break;
      case 'l':
        sindex::config.group_error_bound = strtol(optarg, NULL, 10);
        INVARIANT(sindex::config.group_error_bound > 0);
        break;
      case 'm':
        sindex::config.group_error_tolerance = strtol(optarg, NULL, 10);
        INVARIANT(sindex::config.group_error_tolerance > 0);
        break;
      case 'n':
        sindex::config.buffer_size_bound = strtol(optarg, NULL, 10);
        INVARIANT(sindex::config.buffer_size_bound > 0);
        break;
      case 'o':
        sindex::config.buffer_compact_threshold = strtol(optarg, NULL, 10);
        INVARIANT(sindex::config.buffer_compact_threshold > 0);
        break;
      case 'p':
        sindex::config.partial_len_bound = strtol(optarg, NULL, 10);
        INVARIANT(sindex::config.partial_len_bound > 0);
        break;
      case 'q':
        sindex::config.forward_step = strtol(optarg, NULL, 10);
        INVARIANT(sindex::config.forward_step > 0);
        break;
      case 'r':
        sindex::config.backward_step = strtol(optarg, NULL, 10);
        INVARIANT(sindex::config.backward_step > 0);
        break;
      case 'x':
        initial_size = strtoul(optarg, NULL, 10);
        INVARIANT(table_size > 0);
        break;
      case 'y':
        target_size = strtoul(optarg, NULL, 10);
        INVARIANT(table_size > 0);
        break;
      case 'z':
        ideal_training_interval = strtoul(optarg, NULL, 10);
      default:
        abort();
    }
  }

  COUT_THIS("[micro] Read:Insert:Update:Delete:Scan = "
            << read_ratio << ":" << insert_ratio << ":" << update_ratio << ":"
            << delete_ratio << ":" << scan_ratio)
  double ratio_sum =
      read_ratio + insert_ratio + delete_ratio + scan_ratio + update_ratio;
  INVARIANT(ratio_sum > 0.9999 && ratio_sum < 1.0001);  // avoid precision lost
  COUT_VAR(runtime);
  COUT_VAR(fg_n);
  COUT_VAR(bg_n);
  COUT_VAR(sindex::config.root_error_bound);
  COUT_VAR(sindex::config.root_memory_constraint);
  COUT_VAR(sindex::config.group_error_bound);
  COUT_VAR(sindex::config.group_error_tolerance);
  COUT_VAR(sindex::config.buffer_size_bound);
  COUT_VAR(sindex::config.buffer_size_tolerance);
  COUT_VAR(sindex::config.buffer_compact_threshold);
  COUT_VAR(sindex::config.partial_len_bound);
  COUT_VAR(sindex::config.forward_step);
  COUT_VAR(sindex::config.backward_step);
}
