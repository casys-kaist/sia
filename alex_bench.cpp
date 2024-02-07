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

#include "time.h"
#include <sstream>
#include <iostream>

#include "mkl.h"
#include "mkl_lapacke.h"
#include <signal.h>
#include "alex/alex.h"
#include "../zipf.hpp"

struct alignas(CACHELINE_SIZE) FGParam;

typedef FGParam fg_param_t;
typedef alex::AlexKey<char> index_key_t;
typedef alex::Alex<char, uint64_t> alex_t;

#include "alex/alex_bg.h"

inline void prepare_sindex(alex_t *&table);

void run_benchmark(alex_t *table, size_t sec);

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
size_t key_length = 64;

volatile bool running = false;
std::atomic<size_t> ready_threads(0);
std::vector<std::pair<index_key_t, uint64_t>> exist_keys;
std::vector<std::pair<index_key_t, uint64_t>> non_exist_keys;

struct alignas(CACHELINE_SIZE) FGParam {
  alex_t *table;
  uint64_t throughput;
  uint32_t thread_id;

  double latency_sum;
  int latency_count;
};

int main(int argc, char **argv) {
  parse_args(argc, argv);
  alex_t *tab_xi;
  prepare_sindex(tab_xi);

  is_initial = false;

  run_benchmark(tab_xi, runtime);
  if (tab_xi != nullptr) delete tab_xi;
}

std::mt19937 global_gen(seed);
std::uniform_int_distribution<int64_t> global_rand_int8(
    32, 127);

std::uniform_real_distribution<double> global_ratio_dis(0, 1);

void key_gen(uint8_t *buf) {
  for (size_t j=0; j<MAX_KEY_SIZE; j++){
    buf[j] = (uint8_t)global_rand_int8(global_gen);
  }

  return;
}

inline void prepare_sindex(alex_t *&table) {
  alex::max_key_length_ = key_length;
  table = new alex_t();

  exist_keys.reserve(initial_size);
  uint8_t c_str[MAX_KEY_SIZE + 1] = {0};
  for (size_t i = 0; i < initial_size; ++i) {  
      key_gen(c_str);
      index_key_t k;
      for (size_t idx=0; idx<key_length; idx++) {
        k.key_arr_[idx] = c_str[idx];
      }
      exist_keys.push_back(std::make_pair(k, 1));
  }

    if (insert_ratio > 0) {
        non_exist_keys.reserve(table_size);
        for (size_t i = 0; i < table_size; ++i) {
            key_gen(c_str);
            index_key_t k;
            for (size_t idx=0; idx<key_length; idx++) {
                k.key_arr_[idx] = c_str[idx];
            }
            non_exist_keys.push_back(std::make_pair(k, 1));
        }
    }

  COUT_VAR(exist_keys.size());
  COUT_VAR(non_exist_keys.size());

  // initilize SIndex (sort keys first)
  auto comp_func = [](auto const& a, auto const& b) { return a.first < b.first; };

  std::sort(exist_keys.begin(), exist_keys.end(), comp_func);
  if (insert_ratio > 0) {
#ifdef SEQUENTIAL_DIST
  std::sort(non_exist_keys.begin(), non_exist_keys.end(), comp_func);
#endif
#ifdef HOTSPOT_DIST
  std::sort(non_exist_keys.begin(), non_exist_keys.end(), comp_func);
#endif
#ifdef EXPONENT_DIST
  std::sort(non_exist_keys.begin(), non_exist_keys.end(), comp_func);
  // Shuffle with Exponential distribution
  std::mt19937 gen(seed);
  std::exponential_distribution<double> exp_dis(EXP_LAMBDA);
  std::vector<std::pair<double,
    std::pair<index_key_t, uint64_t>
  >> values;
  for (auto& s : non_exist_keys) {
    values.push_back(std::make_pair(exp_dis(gen), s));
  }
  std::sort(values.begin(), values.end(), comp_func);
  for(size_t i=0; i<non_exist_keys.size(); i++){
    non_exist_keys[i] = values[i].second;
  }
#endif
#ifdef ZIPF_DIST
  std::sort(non_exist_keys.begin(), non_exist_keys.end(), comp_func);
  // Shuffle with zipfian distribution
  std::default_random_engine generator;
  zipfian_int_distribution<int>::param_type p(1, 1e6, 0.99, 27.000);
  zipfian_int_distribution<int> zipf_dis(p);
  std::vector<std::pair<double, 
    std::pair<index_key_t, uint64_t>
  >> values;
  for (auto& s : non_exist_keys) {
    double z = (double)(zipf_dis(generator)) / (double)1e6;
    values.push_back(std::make_pair(z, s));
  }
  std::sort(values.begin(), values.end(), comp_func);
  for(size_t i=0; i<non_exist_keys.size(); i++){
    non_exist_keys[i] = values[i].second;
  }
#endif
  }
  std::cout << "start training\n";
  table->bulk_load(&exist_keys[0], exist_keys.size());
  alex_t *new_table = new alex_t();
  new_table->copy_tree_recursive(table->superroot_);
  std::cout << new_table->leaf_counter << ", " << new_table->node_counter << ", " << new_table->key_count << std::endl;
  alex::rcu_alloc();
}

void segfault_handler(int signum) {
    return; 
}

void *run_fg(void *param) {
    fg_param_t &thread_param = *(fg_param_t *)param;
    uint32_t thread_id = thread_param.thread_id;
    alex_t *table = thread_param.table;

    signal(SIGSEGV, segfault_handler);

    std::mt19937 gen(seed);
    std::uniform_real_distribution<double> ratio_dis(0, 1);

    size_t exist_key_n_per_thread = exist_keys.size() / fg_n;
    size_t exist_key_start = thread_id * exist_key_n_per_thread;
    size_t exist_key_end = (thread_id + 1) * exist_key_n_per_thread;
    std::vector<std::pair<index_key_t, uint64_t>> op_keys(exist_keys.begin() + exist_key_start,
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
    std::vector<std::pair<index_key_t, int>> latest_keys;
    for (int i=0; i<LATEST_KEY_NUM; i++) {
        latest_keys.push_back(op_keys[insert_i]);
        table->insert(op_keys[insert_i++].first, 3, thread_id);
    }
#endif
#ifdef HOTSPOT_DIST
    size_t hotspot_start = non_exist_key_index - 1;//ratio_dis(gen) * non_exist_key_n_per_thread + non_exist_key_index;
    size_t hotspot_end = end_i - 1; //std::min(hotspot_start + HOTSPOT_LENGTH, end_i) - 1;
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
                table->get_payload(op_keys[(read_i + delete_i) % end_i].first, thread_id);
                read_i++;
                if (unlikely(read_i == end_i)) read_i = 0;
            #endif
            #ifdef UNIFORM_DIST
                table->get_payload(op_keys[p * read_i - 1].first, thread_id);
            #endif
            #ifdef LATEST_DIST
                table->get_payload(latest_keys[p * LATEST_KEY_NUM].first, thread_id);
            #endif
            #ifdef HOTSPOT_DIST
                table->get_payload(op_keys[hotspot_start + (hotspot_end - hotspot_start) * p].first, thread_id);
            #endif
            #ifdef EXPONENT_DIST
                table->get_payload(op_keys[e * read_i - 1].first, thread_id);
            #endif
            #ifdef ZIPF_DIST
                table->get_payload(op_keys[z * read_i - 1].first, thread_id);
            #endif
        } else if (d <= read_ratio + update_ratio) {  // update
            #ifdef SEQUENTIAL_DIST
                table->get_payload(op_keys[(update_i + delete_i) % end_i].first, thread_id);
                update_i++;
                if (unlikely(update_i == end_i)) update_i = 0;
            #endif
            #ifdef UNIFORM_DIST
                table->get_payload(op_keys[p * insert_i - 1].first, thread_id);
            #endif
            #ifdef LATEST_DIST
                table->get_payload(latest_keys[p * LATEST_KEY_NUM].first, thread_id);
            #endif
            #ifdef HOTSPOT_DIST
                table->get_payload(op_keys[hotspot_start + (hotspot_end - hotspot_start) * p].first, thread_id);
            #endif
            #ifdef EXPONENT_DIST
                table->get_payload(op_keys[e * insert_i - 1].first, thread_id);
            #endif
            #ifdef ZIPF_DIST
                table->get_payload(op_keys[z * insert_i - 1].first, thread_id);
            #endif
        } else if (d <= read_ratio + update_ratio + insert_ratio) {  // insert
            #ifdef SEQUENTIAL_DIST
                table->insert(op_keys[insert_i].first, 3, thread_id);
                insert_i++;
                if (unlikely(insert_i == end_i)) insert_i = 0;
            #endif
            #ifdef UNIFORM_DIST
                table->insert(op_keys[insert_i].first, 3, thread_id);
                insert_i++;
                read_i = std::max(read_i, insert_i);
                if (unlikely(insert_i == end_i)) insert_i = 0;
            #endif
            #ifdef LATEST_DIST
                table->insert(op_keys[insert_i].first, 3, thread_id);
                latest_keys.pop_back();
                latest_keys.insert(latest_keys.begin(), op_keys[insert_i]);
                insert_i++;
                if (unlikely(insert_i == end_i)) insert_i = 0;
            #endif
            #ifdef HOTSPOT_DIST
                table->insert(op_keys[hotspot_start + (hotspot_end - hotspot_start) * p].first, 3, thread_id);
            #endif
            #ifdef EXPONENT_DIST
                table->insert(op_keys[insert_i].first, 3, thread_id);
                insert_i++;
                read_i = std::max(read_i, insert_i);
                if (unlikely(insert_i == end_i)) insert_i = 0;
            #endif
            #ifdef ZIPF_DIST
                table->insert(op_keys[insert_i].first, 3, thread_id);
                insert_i++;
                read_i = std::max(read_i, insert_i);
                if (unlikely(insert_i == end_i)) insert_i = 0;
            #endif
        } else if (d <= read_ratio + update_ratio + insert_ratio + delete_ratio) {  // remove
            #ifdef SEQUENTIAL_DIST
                table->erase_one(op_keys[delete_i].first);
                delete_i++;
                if (unlikely(delete_i == end_i)) delete_i = 0;
            #endif
            #ifdef UNIFORM_DIST
                table->erase_one(op_keys[p * insert_i].first);
            #endif
            #ifdef LATEST_DIST
                table->erase_one(op_keys[p * insert_i].first);
            #endif
            #ifdef HOTSPOT_DIST
                table->erase_one(op_keys[hotspot_start + (hotspot_end - hotspot_start) * p].first);
            #endif
            #ifdef EXPONENT_DIST
                table->erase_one(op_keys[e * insert_i].first);
            #endif
            #ifdef ZIPF_DIST
                table->erase_one(op_keys[z * insert_i].first);
            #endif
            //COUT_N_EXIT("remove not supported");
        } else {  // scan
            std::vector<std::pair<index_key_t, uint64_t>> res;
            #ifdef SEQUENTIAL_DIST
                auto start_iter = table->lower_bound(op_keys[(read_i + delete_i) % end_i].first);
                read_i++;
                if (unlikely(read_i == insert_i)) read_i = 0;
            #endif
            #ifdef UNIFORM_DIST
                auto start_iter = table->lower_bound(op_keys[p * read_i].first);
            #endif
            #ifdef LATEST_DIST
                auto start_iter = table->lower_bound(latest_keys[p * LATEST_KEY_NUM].first);
            #endif
            #ifdef HOTSPOT_DIST
                auto start_iter = table->lower_bound(op_keys[hotspot_start + (hotspot_end - hotspot_start) * p].first);
            #endif
            #ifdef EXPONENT_DIST
                auto start_iter = table->lower_bound(op_keys[e * read_i].first);
            #endif
            #ifdef ZIPF_DIST
                auto start_iter = table->lower_bound(op_keys[z * read_i].first);
            #endif
            size_t cnt=0;
            while(!start_iter.is_end()){
                res.push_back(std::make_pair(start_iter.key(), start_iter.payload()));
                start_iter++;
                cnt++;
                if (cnt == 10) break;
            }
        }

        clock_gettime(CLOCK_MONOTONIC, &end_t);
        thread_param.latency_sum += (end_t.tv_sec - begin_t.tv_sec) + (end_t.tv_nsec - begin_t.tv_nsec) / 1000000000.0;
        thread_param.latency_count++;
        thread_param.throughput++;
    }

    pthread_exit(nullptr);
}

void sig_handler(int signum) {
    return;
}

void run_benchmark(alex_t *table, size_t sec) {
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

  pthread_t bg_threads[bg_n];
  bg_param_t bg_params[bg_n];
  foreground_finished = false;
  
  for (size_t worker_i = 0; worker_i < bg_n; worker_i++) {
    bg_params[worker_i].thread_id = worker_i;
    bg_params[worker_i].table = table;
    int ret = pthread_create(&bg_threads[worker_i], nullptr, run_bg, 
                            (void *)&bg_params[worker_i]);
    if (ret) {
      std::cout << "Error when making background threads with code : " << ret << std::endl;
      abort();
    }
  }

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
	//break;
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

    //int rc = pthread_join(threads[i], &status);
    //if (rc) {
    //  COUT_N_EXIT("Error:unable to join," << rc);
    //}
  }

  size_t throughput = 0;
  for (auto &p : fg_params) {
    throughput += p.throughput;
  }
  
  std::ostringstream final_buf;
  final_buf << "[micro] Throughput(op/s): " << (int)(throughput / current_sec) << std::endl;
  final_buf << "[micro] Latency: " << (all_latency_sum / all_latency_count) << std::endl;
  std::cout << final_buf.str();
  std::flush(std::cout);
  return;
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
      {"delta-idx-size", required_argument, 0, 'p'},
      {"node-size", required_argument, 0, 'q'},
      {"initial-size", required_argument, 0, 'x'},
      {"target-size", required_argument, 0, 'y'},
      {"key-length", required_argument, 0, 'l'},
      {0, 0, 0, 0}};
  std::string ops = "a:b:c:d:e:f:g:h:i:x:y:l:p:q:";
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
      case 'x':
        initial_size = strtoul(optarg, NULL, 10);
        INVARIANT(table_size > 0);
        break;
      case 'y':
        target_size = strtoul(optarg, NULL, 10);
        INVARIANT(table_size > 0);
        break;
      case 'l':
        key_length = strtol(optarg, NULL, 10);
        break;
      case 'p':
        delta_idx_capacity_const = strtol(optarg, NULL, 10);
        break;
      case 'q':
        node_size_const = strtol(optarg, NULL, 10);
        break;
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
}
