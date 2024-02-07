/*
 * The code is part of the XIndex project.
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
 * For more about XIndex, visit:
 *     https://ppopp20.sigplan.org/details/PPoPP-2020-papers/13/XIndex-A-Scalable-Learned-Index-for-Multicore-Data-Storage
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
#include <vector>
#include <time.h>
#include <csignal>
#include <iostream>
#include <sstream>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <cassert>

#include <utility>
#include "../zipf.hpp"

// Include Cuckoo
extern "C" {
    #include "cuckoo-trie-code/cuckoo_trie.h"   
}

#define CACHELINE_SIZE (1 << 6)
struct alignas(CACHELINE_SIZE) FGParam;

template <size_t len>
class Key;

typedef FGParam fg_param_t;

typedef Key<MAX_KEY_SIZE> index_key_t;

inline void prepare_xindex(cuckoo_trie *table);

void run_benchmark(cuckoo_trie *table, size_t sec);

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
int empty_value = 0;

volatile bool running = false;
std::atomic<size_t> ready_threads(0);
std::vector<index_key_t> exist_keys;
std::vector<index_key_t> non_exist_keys;
// Used for checking memory usage
std::vector<uint8_t *> inserted_keys;

struct alignas(CACHELINE_SIZE) FGParam {
    cuckoo_trie *table;
    uint64_t throughput;
    uint32_t thread_id;

    double latency_sum;
    int latency_count;
};

// Global Variable
double current_sec = 0.0;

template <size_t len>
class Key {
    typedef std::array<double, len> model_key_t;

    public:
    static constexpr size_t model_key_size() { return len; }

    static Key max() {
        static Key max_key;
        memset(&max_key.buf, 255, len);
        return max_key;
    }
    static Key min() {
        static Key min_key;
        memset(&min_key.buf, 0, len);
        return min_key;
    }

    Key() { memset(&buf, 0, len); }
    Key(uint64_t key) { COUT_N_EXIT("str key no uint64"); }
    /* Key(const std::string &s) { */
    /*     memset(&buf, 0, len); */
    /*     memcpy(&buf, s.data(), s.size()); */
    /* } */
    Key(const std::string &s) {
        memset(&buf, 0, len);
        memcpy(&buf, s.data(), s.size());
    }

    Key(const Key &other) { memcpy(&buf, &other.buf, len); }
    Key &operator=(const Key &other) {
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

    bool less_than(const Key &other, size_t begin_i, size_t l) const {
        return memcmp(buf + begin_i, other.buf + begin_i, l) < 0;
    }

    friend bool operator<(const Key &l, const Key &r) {
        return memcmp(&l.buf, &r.buf, len) < 0;
    }
    friend bool operator>(const Key &l, const Key &r) {
        return memcmp(&l.buf, &r.buf, len) > 0;
    }
    friend bool operator>=(const Key &l, const Key &r) {
        return memcmp(&l.buf, &r.buf, len) >= 0;
    }
    friend bool operator<=(const Key &l, const Key &r) {
        return memcmp(&l.buf, &r.buf, len) <= 0;
    }
    friend bool operator==(const Key &l, const Key &r) {
        return memcmp(&l.buf, &r.buf, len) == 0;
    }
    friend bool operator!=(const Key &l, const Key &r) {
        return memcmp(&l.buf, &r.buf, len) != 0;
    }

    friend std::ostream &operator<<(std::ostream &os, const Key &key) {
        os << "key [" << std::hex;
        for (size_t i = 0; i < sizeof(Key); i++) {
            os << "0x" << key.buf[i] << " ";
        }
        os << "] (as byte)" << std::dec;
        return os;
    }

    uint8_t buf[len];
};

int main(int argc, char **argv) {
    parse_args(argc, argv);
    cuckoo_trie *table = ct_alloc(150000000);

    prepare_xindex(table);

    run_benchmark(table, runtime);

    ct_free(table);

    return 0;
}

std::mt19937 global_gen(seed);
std::uniform_int_distribution<int64_t> global_rand_int8(
    0, std::numeric_limits<uint8_t>::max());

std::uniform_real_distribution<double> global_ratio_dis(0, 1);

void key_gen(uint8_t *buf) {
  for (size_t j=0; j<sizeof(index_key_t); j++){
    buf[j] = (uint8_t)global_rand_int8(global_gen);
  }
  return;
}

inline void prepare_xindex(cuckoo_trie *table) {
    std::mt19937 gen(seed);
    std::uniform_int_distribution<int64_t> rand_int8(
        0, std::numeric_limits<uint8_t>::max());

    if (insert_ratio == 0.0) {
        initial_size = target_size;
    }
    exist_keys.reserve(initial_size);
    for (size_t i = 0; i < initial_size; ++i) {
        index_key_t k;
        key_gen(k.buf);

        uint8_t *kv_buf = (uint8_t *)malloc(kv_required_size(MAX_KEY_SIZE, 8));
        // Used for checking memory usage
        uint8_t *kv_buf_2 = (uint8_t *)malloc(kv_required_size(MAX_KEY_SIZE, 8));
        ct_kv *kv = (ct_kv *)kv_buf;
        kv_init(kv, MAX_KEY_SIZE, 8);
        memset(kv->bytes, 0, MAX_KEY_SIZE + 8);
        memcpy(kv->bytes, k.buf, MAX_KEY_SIZE);
        exist_keys.push_back(k);
        inserted_keys.push_back(kv_buf_2);
        auto res = ct_insert(table, kv);
        assert(res == S_OK);
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
    std::exponential_distribution<> exp_dis(EXP_LAMBDA);
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

    return;
}

void *run_fg(void *param) {
    fg_param_t &thread_param = *(fg_param_t *)param;
    uint32_t thread_id = thread_param.thread_id;
    cuckoo_trie *table = thread_param.table;

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
#define unlikely(____x____) __builtin_expect(____x____, 0)
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
        
        uint8_t *kv_buf = (uint8_t *)malloc(kv_required_size(MAX_KEY_SIZE, 8));
        ct_kv *kv = (ct_kv *)kv_buf;
        kv_init(kv, MAX_KEY_SIZE, 8);
        memset(kv->bytes, 0, MAX_KEY_SIZE + 8);
        memcpy(kv->bytes, op_keys[insert_i++].buf, MAX_KEY_SIZE);

        auto res = ct_insert(table, kv);
    }
#endif
#ifdef HOTSPOT_DIST
    size_t hotspot_start = non_exist_key_index - 1;//ratio_dis(gen) *  non_exist_key_n_per_thread + non_exist_key_index;
    size_t hotspot_end = end_i - 1; //std::min(hotspot_start + HOTSPOT_LENGTH, end_i) - 1;
#endif
#ifdef EXPONENT_DIST
    std::exponential_distribution<> exp_dis(EXP_LAMBDA);
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

    std::vector<uint8_t *> per_thread_inserted_keys;

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

        if (d <= read_ratio) {  // get
            ct_kv *res = NULL;
            #ifdef SEQUENTIAL_DIST
                res = ct_lookup(table, MAX_KEY_SIZE, op_keys[(read_i + delete_i) % end_i].buf);
                read_i++;
                if (unlikely(read_i == end_i)) read_i = 0;
            #endif
            #ifdef UNIFORM_DIST
                res = ct_lookup(table, MAX_KEY_SIZE, op_keys[p * read_i].buf);
            #endif
            #ifdef LATEST_DIST
                res = ct_lookup(table, MAX_KEY_SIZE, latest_keys[p * LATEST_KEY_NUM].buf);
            #endif
            #ifdef HOTSPOT_DIST
                res = ct_lookup(table, MAX_KEY_SIZE, op_keys[hotspot_start + (hotspot_end - hotspot_start) * p].buf);
            #endif
            #ifdef EXPONENT_DIST
                res = ct_lookup(table, MAX_KEY_SIZE, op_keys[e * read_i].buf);
            #endif
            #ifdef ZIPF_DIST
                res = ct_lookup(table, MAX_KEY_SIZE, op_keys[z * read_i].buf);
            #endif
        } else if (d <= read_ratio + update_ratio) {  // update

            #ifdef SEQUENTIAL_DIST
                index_key_t key = op_keys[(update_i + delete_i) % end_i];
                update_i++;
                if (unlikely(update_i == end_i)) update_i = 0;
            #endif
            #ifdef UNIFORM_DIST
                index_key_t key = op_keys[p * insert_i];
            #endif
            #ifdef LATEST_DIST
                index_key_t key = latest_keys[p * LATEST_KEY_NUM];
            #endif
            #ifdef HOTSPOT_DIST
                index_key_t key = op_keys[hotspot_start + (hotspot_end - hotspot_start) * p];
            #endif
            #ifdef EXPONENT_DIST
                index_key_t key = op_keys[e * insert_i];
            #endif
            #ifdef ZIPF_DIST
                index_key_t key = op_keys[z * insert_i];
            #endif

            uint8_t *kv_buf = (uint8_t *)malloc(kv_required_size(MAX_KEY_SIZE, 8));
            ct_kv *kv = (ct_kv *)kv_buf;
            kv_init(kv, MAX_KEY_SIZE, 8);
            memset(kv->bytes, 0, MAX_KEY_SIZE + 8);
            memcpy(kv->bytes, key.buf, MAX_KEY_SIZE);

            int res = ct_update(table, kv);

        } else if (d <= read_ratio + update_ratio + insert_ratio) {

            #ifdef SEQUENTIAL_DIST
                index_key_t key = op_keys[insert_i];
                insert_i++;
                if (unlikely(insert_i == end_i)) insert_i = 0;
            #endif
            #ifdef UNIFORM_DIST
                index_key_t key = op_keys[insert_i];
                insert_i++;
                read_i = std::max(read_i, insert_i);
                if (unlikely(insert_i == end_i)) insert_i = 0;
            #endif
            #ifdef LATEST_DIST
                index_key_t key = op_keys[insert_i];
                latest_keys.pop_back();
                latest_keys.insert(latest_keys.begin(), key);
                insert_i++;
                if (unlikely(insert_i == end_i)) insert_i = 0;
            #endif
            #ifdef HOTSPOT_DIST
                index_key_t key = op_keys[hotspot_start + (hotspot_end - hotspot_start) * p];
            #endif
            #ifdef EXPONENT_DIST
                index_key_t key = op_keys[insert_i];
                insert_i++;
                read_i = std::max(read_i, insert_i);
                if (unlikely(insert_i == end_i)) insert_i = 0;
            #endif
            #ifdef ZIPF_DIST
                index_key_t key = op_keys[insert_i];
                insert_i++;
                read_i = std::max(read_i, insert_i);
                if (unlikely(insert_i == end_i)) insert_i = 0;
            #endif

            uint8_t *kv_buf = (uint8_t *)malloc(kv_required_size(MAX_KEY_SIZE, 8));
            ct_kv *kv = (ct_kv *)kv_buf;
            kv_init(kv, MAX_KEY_SIZE, 8);
            memset(kv->bytes, 0, MAX_KEY_SIZE + 8);
            memcpy(kv->bytes, key.buf, MAX_KEY_SIZE);
            auto res = ct_insert(table, kv);

        } else if (d <= read_ratio + update_ratio + insert_ratio +
                delete_ratio) {  // remove
            COUT_N_EXIT("CUCKOO TRIE DOES NOT SUPPORT DELETION");
            break;
        } else {
            std::vector<ct_kv *> results;
            ct_iter *iter = ct_iter_alloc(table);
            const int range_records = 10;
            
            #ifdef SEQUENTIAL_DIST
                index_key_t key = op_keys[(read_i + delete_i) % end_i];
                read_i++;
                if (unlikely(read_i == insert_i)) read_i = 0;
            #endif
            #ifdef UNIFORM_DIST
                index_key_t key = op_keys[p * read_i];
            #endif
            #ifdef LATEST_DIST
                index_key_t key = latest_keys[p * LATEST_KEY_NUM];
            #endif
            #ifdef HOTSPOT_DIST
                index_key_t key = op_keys[hotspot_start + (hotspot_end - hotspot_start) * p];
            #endif
            #ifdef EXPONENT_DIST
                index_key_t key = op_keys[e * read_i];
            #endif
            #ifdef ZIPF_DIST
                index_key_t key = op_keys[z * read_i];
            #endif
            
            bool resume = false;
            do {
                ct_iter_goto(iter, MAX_KEY_SIZE, (uint8_t *)key.buf);
                for (int i=0; i<range_records; i++){
                    ct_kv *res = ct_iter_next(iter);
                    if (!res) {
                        if (i == 0) resume = true;
                        break;
                    }
                    else results.push_back(res);
                }
            } while(resume);
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

void run_benchmark(cuckoo_trie *table, size_t sec) {
    pthread_t threads[fg_n];
    fg_param_t fg_params[fg_n];
    // check if parameters are cacheline aligned
    for (size_t i = 0; i < fg_n; i++) {
        if ((uint64_t)(&(fg_params[i])) % CACHELINE_SIZE != 0) {
            COUT_N_EXIT("wrong parameter address: " << &(fg_params[i]));
        }
    }

    signal(SIGALRM, sig_handler);

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

    std::vector<size_t> tput_history(fg_n, 0);
    struct timespec begin_t, end_t;
    uint64_t total_keys = initial_size;

    running = true;

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
            break;
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
        //    COUT_N_EXIT("Error:unable to join," << rc);
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
        {"read", required_argument, 0, 'a'},
        {"insert", required_argument, 0, 'b'},
        {"remove", required_argument, 0, 'c'},
        {"update", required_argument, 0, 'd'},
        {"scan", required_argument, 0, 'e'},
        {"table-size", required_argument, 0, 'f'},
        {"runtime", required_argument, 0, 'g'},
        {"fg", required_argument, 0, 'h'},
        {"bg", required_argument, 0, 'i'},
        {"initial-size", required_argument, 0, 'p'},
        {"target-size", required_argument, 0, 'q'},
        {"workload-length", required_argument, 0, 'w'},
        {"workload-type", required_argument, 0, 't'},
        {0, 0, 0, 0}};
    std::string ops = "a:b:c:d:e:f:g:h:i:p:q:w:t:";
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
            case 'p':
                initial_size = strtoul(optarg, NULL, 10);
                INVARIANT(table_size > 0);
                break;
            case 'q':
                target_size = strtoul(optarg, NULL, 10);
                INVARIANT(table_size > 0);
                break;
            case 'x':
                initial_size = strtoul(optarg, NULL, 10);
                INVARIANT(table_size > 0);
                break;
            case 'y':
                target_size = strtoul(optarg, NULL, 10);
                INVARIANT(table_size > 0);
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
    COUT_VAR(table_size);
    COUT_VAR(initial_size);
    COUT_VAR(target_size);
}
