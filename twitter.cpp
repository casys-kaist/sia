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
#include <fstream>
#include <limits.h>
#include <libgen.h>

#include "../test_config.h"
#include "../lock.h"
#include "helper.h"
#include "sindex.h"
#include "sindex_impl.h"

double current_sec = 0.0;

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
size_t runtime = 10;
size_t fg_n = 1;
size_t bg_n = 1;

char* cluster_number = "12.2";

volatile bool running = false;
std::atomic<size_t> ready_threads(0);

struct alignas(CACHELINE_SIZE) FGParam {
  sindex_t *table;
  uint64_t throughput;
  uint32_t thread_id;
  bool alive;

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
  StrKey(const char* s) {
        for (int i = 0; i < len; i++) {
            buf[i] = s[i];
        }
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
      os << "0x" << key.buf[i] << " ";
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

inline void prepare_sindex(sindex_t *&table) {
  char filename[PATH_MAX];
  char __exec_path[PATH_MAX];
  char *ret = realpath("/proc/self/exe", __exec_path);
  char *exec_path = dirname((char *)__exec_path);
  sprintf(filename, "%s/../dataset/twitter/%s/load%s", exec_path, cluster_number, cluster_number);
  std::cout << "opening filename: " << filename << std::endl;

  size_t _tablesize = 0;
	std::ifstream tracefile(filename);
	std::string line;
	char key[MAX_KEY_SIZE];

  std::vector<index_key_t> exist_keys;
	while (std::getline(tracefile, line)) {
		sscanf(line.c_str(), "%s", key);
		index_key_t query_key(key);
		exist_keys.push_back(query_key);
		_tablesize++;
    if (_tablesize > 10000000) break;
	}
	printf("Loaded keys: %ld\n", _tablesize);

  COUT_VAR(exist_keys.size());

  // initilize SIndex (sort keys first)
  std::sort(exist_keys.begin(), exist_keys.end());
  std::vector<uint64_t> vals(exist_keys.size(), 1);
  table = new sindex_t(exist_keys, vals, fg_n, 1);
}

void *run_fg(void *param) {
  fg_param_t &thread_param = *(fg_param_t *)param;
  uint32_t thread_id = thread_param.thread_id;
  sindex_t *table = thread_param.table;

  std::mt19937 gen(SEED);
  std::uniform_real_distribution<> ratio_dis(0, 1);

  // Read workload trace file of the thread
    char filename[PATH_MAX];
    char __exec_path[PATH_MAX];
    char *ret = realpath("/proc/self/exe", __exec_path);
    char *exec_path = dirname((char *)__exec_path);
    sprintf(filename, "%s/../dataset/twitter/%s/workload_%02d", exec_path, cluster_number, thread_id);
    

    struct stat buf;
    int fd = open(filename, O_RDONLY);
    fstat(fd, &buf);

    char *file = (char*) mmap(0, buf.st_size, PROT_READ|PROT_WRITE, MAP_PRIVATE, fd, 0);

    printf("[twitter] Worker %d Ready.\n", thread_id);

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

    index_key_t query_key(line + 2);
    op = line[0];
    token = strtok_r(NULL, delim, &remains);

    if (!token) break;

    clock_gettime(CLOCK_MONOTONIC, &begin_t);
    switch (op) {
      case 'g':
      {
        res = table->get(query_key, dummy_value, thread_id);
        break;
      }
      case 'p':
      {
        res = table->put(query_key, dummy_value, thread_id);
        break;
      }
      case 'd':
      {
        res = table->remove(query_key, thread_id);
        break;
      }
      case 's':
      {
        std::vector<std::pair<index_key_t, uint64_t>> results;
        res = table->scan(query_key, 10, results, thread_id);
        break;
      }
    }
    clock_gettime(CLOCK_MONOTONIC, &end_t);
    thread_param.latency_sum += (end_t.tv_sec - begin_t.tv_sec) + (end_t.tv_nsec - begin_t.tv_nsec) / 1000000000.0;
    thread_param.latency_count++;
    thread_param.throughput++;
  }
  thread_param.alive = false;

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
      if (!threads_alive) break;
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
      {"cluster-number", required_argument, 0, 'w'},
      {0, 0, 0, 0}};
  std::string ops = "g:h:i:j:k:l:m:n:o:p:q:r:w:";
  int option_index = 0;

  while (1) {
    int c = getopt_long(argc, argv, ops.c_str(), long_options, &option_index);
    if (c == -1) break;

    switch (c) {
      case 0:
        if (long_options[option_index].flag != 0) break;
        abort();
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
      case 'w':
        cluster_number = optarg;
        break;
      default:
        abort();
    }
  }

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
  COUT_VAR(cluster_number);
}