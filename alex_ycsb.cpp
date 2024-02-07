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

#include <getopt.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <random>
#include <string>
#include <vector>
#include <time.h>
#include <csignal>
#include <iostream>
#include <sstream>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <limits.h>
#include <libgen.h>

#include "../test_config.h"
#include "../lock.h"
#include "alex/alex.h"
#include "mkl.h"

double current_sec = 0.0;

struct alignas(CACHELINE_SIZE) FGParam;
template <size_t len>
class StrKey;

typedef FGParam fg_param_t;
typedef alex::AlexKey<char> index_key_t;
typedef alex::Alex<char, uint64_t> alex_t;

#include "alex/alex_bg.h"

inline void prepare_sindex(alex_t *&table);

void run_benchmark(alex_t *table, size_t sec);

void *run_fg(void *param);

inline void parse_args(int, char **);

// parameters
size_t table_size = 1000000;
size_t runtime = 10;
size_t fg_n = 1;
size_t bg_n = 1;
size_t key_length = 16;

char workload_type = 'a';
char* dataset_name = "10m_100m";

volatile bool running = false;
std::atomic<size_t> ready_threads(0);

struct alignas(CACHELINE_SIZE) FGParam {
  alex_t *table;
  uint64_t throughput;
  uint32_t thread_id;
  bool alive;

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

inline void prepare_sindex(alex_t *&table) {
  alex::max_key_length_ = key_length;
  table = new alex_t();

  char filename[PATH_MAX];
  char __exec_path[PATH_MAX];
  char *ret = realpath("/proc/self/exe", __exec_path);
  char *exec_path = dirname((char *)__exec_path);
  sprintf(filename, "%s/../dataset/%s/Workload%c/workload_%c_load", exec_path, dataset_name, workload_type, workload_type);

  struct stat buf;
  int fd = open(filename, O_RDONLY);
  if ( fd < 0 ) {
      printf("Error: %s\n", strerror(errno));
  }
  fstat(fd, &buf);

  char *file = (char*) mmap(0, buf.st_size, PROT_READ|PROT_WRITE, MAP_PRIVATE, fd, 0);
  const char *delim = "\n";
  char *remains = NULL;
  char *token = strtok_r(file, delim, &remains);

  std::vector<std::pair<index_key_t, uint64_t>> exist_keys;
  exist_keys.reserve(table_size);
  int cnt = 0;
  while (token)
  {
      const char * line = token;
      // skip 6 digits for %c user
      index_key_t query_key;
      for (size_t idx=0; idx<key_length; idx++) {
        query_key.key_arr_[idx] = *(line + idx + 6);
      }
      token = strtok_r(NULL, delim, &remains);

      exist_keys.push_back(std::make_pair(query_key, 1));
      cnt++;
      if(cnt == table_size) break;
  }

  COUT_VAR(exist_keys.size());

  // initilize SIndex (sort keys first)
  std::sort(exist_keys.begin(), exist_keys.end(),
             [](auto const& a, auto const& b) { return a.first < b.first; });
  table->bulk_load(&exist_keys[0], exist_keys.size());
  alex::rcu_alloc();
}

void *run_fg(void *param) {
  fg_param_t &thread_param = *(fg_param_t *)param;
  uint32_t thread_id = thread_param.thread_id;
  alex_t *table = thread_param.table;

  std::mt19937 gen(SEED);
  std::uniform_real_distribution<> ratio_dis(0, 1);

  // Read workload trace file of the thread
    char filename[PATH_MAX];
    char __exec_path[PATH_MAX];
    char *ret = realpath("/proc/self/exe", __exec_path);
    char *exec_path = dirname((char *)__exec_path);
    sprintf(filename, "%s/../../dataset/%s/Workload%c/workload_%c_worker_%d", exec_path, dataset_name, workload_type, workload_type, thread_id);
    
    struct stat buf;
    int fd = open(filename, O_RDONLY);
    fstat(fd, &buf);

    char *file = (char*) mmap(0, buf.st_size, PROT_READ|PROT_WRITE, MAP_PRIVATE, fd, 0);

    printf("[ycsb] Worker %d Ready.\n", thread_id);

    volatile bool res = false;
    uint64_t dummy_value = 1234;

    const char *delim = "\n";
    char *remains = NULL;
    char *token = strtok_r(file, delim, &remains);

    ready_threads++;

  while (!running)
    ;

  struct timespec begin_t, end_t;

  while (running) {
    char op;
    int readcount;
    const char *line = token;
    int64_t key;

    if (training_threads > 0) {
        std::unique_lock<std::mutex> lck(training_threads_mutex);             // Acquire lock
        training_threads_cond.wait(lck, []{return (training_threads == 0);}); // Release lock & wait
        lck.unlock();
    }

    // We are not using the `key` here, but just leaving it for now
    op = line[0];
    index_key_t query_key;
    for(size_t idx=0; idx<key_length; idx++) {
      query_key.key_arr_[idx] = *(line + 6 + idx);
    }
    token = strtok_r(NULL, delim, &remains);

    if (!token) break;

    clock_gettime(CLOCK_MONOTONIC, &begin_t);
    
    switch (op) {
      case 'r':
      {
        auto iter = table->get_payload(query_key, thread_id);
        break;
      }
      case 'u':
      {
        auto iter = table->get_payload(query_key, thread_id);
        break;
      }
      case 'i':
      {
        auto result = table->insert(query_key, 3, thread_id);
        break;
      }
      case 'd':
      {
        //table->erase_one(query_key);
        break;
      }
      case 's':
      {
        std::vector<std::pair<index_key_t, uint64_t>> results;
        auto start_iter = table->lower_bound(query_key);
        size_t cnt=0;
        while(!start_iter.is_end()){
            results.push_back(std::make_pair(start_iter.key(), start_iter.payload()));
            start_iter++;
            cnt++;
            if (cnt == 10) break;
        }
        break;
      }
    }
    clock_gettime(CLOCK_MONOTONIC, &end_t);
    thread_param.latency_sum += (end_t.tv_sec - begin_t.tv_sec) + (end_t.tv_nsec - begin_t.tv_nsec) / 1000000000.0;
    thread_param.latency_count++;
    thread_param.throughput++;
  }
  thread_param.alive = false;


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
    fg_params[worker_i].alive = true;
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
  uint64_t temp_throughput = 0;
  double temp_sec = 0.0;
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
      bool threads_alive = false;
      for (size_t i = 0; i < fg_n; i++) {
          tput += fg_params[i].throughput - tput_history[i];
          tput_history[i] = fg_params[i].throughput;
          threads_alive |= fg_params[i].alive;
      }

      std::ostringstream throughput_buf;
      current_sec += interval;
      throughput_buf << "[micro] >>> sec " << current_sec << " throughput: " << (int)(tput / interval) << std::endl;
      std::cout << throughput_buf.str();
      std::flush(std::cout);
      if (!threads_alive) {
          std::ostringstream temp_buf;
          temp_buf << "temp throughput: " << (int)(temp_throughput / temp_sec) << std::endl;
          std::cout << temp_buf.str();
          std::flush(std::cout);
          break;
      } else {
          temp_throughput += tput;
          temp_sec = current_sec;
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

}

inline void parse_args(int argc, char **argv) {
  struct option long_options[] = {
      {"table-size", required_argument, 0, 'f'},
      {"runtime", required_argument, 0, 'g'},
      {"fg", required_argument, 0, 'h'},
      {"bg", required_argument, 0, 'i'},
      {"delta-idx-size", required_argument, 0, 'p'},
      {"node-size", required_argument, 0, 'q'},
      {"dataset-name", required_argument, 0, 'w'},
      {"workload-type", required_argument, 0, 't'},
      {"key-length", required_argument, 0, 'l'},
      {0, 0, 0, 0}};
  std::string ops = "f:g:h:i:w:t:l:p:q:";
  int option_index = 0;

  while (1) {
    int c = getopt_long(argc, argv, ops.c_str(), long_options, &option_index);
    if (c == -1) break;

    switch (c) {
      case 0:
        if (long_options[option_index].flag != 0) break;
        abort();
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
      case 'w':
          dataset_name = optarg;
          break;
      case 't':
          memcpy(&workload_type, optarg, 1);
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

  COUT_VAR(runtime);
  COUT_VAR(fg_n);
  COUT_VAR(bg_n);
  COUT_VAR(dataset_name);
  COUT_VAR(workload_type);
}

