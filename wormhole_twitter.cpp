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
#include <fstream>
#include <limits.h>
#include <libgen.h>

// Include Wormhole
#include "wormhole/lib.h"
#include "wormhole/kv.h"
#include "wormhole/wh.h"

#define CACHELINE_SIZE (1 << 6)
struct alignas(CACHELINE_SIZE) FGParam;
template <size_t len>
class Key;

typedef FGParam fg_param_t;

typedef Key<MAX_KEY_SIZE> index_key_t;

inline void prepare_xindex(struct wormref *wh);

void run_benchmark(struct wormref *wh, size_t sec);

void *run_fg(void *param);

inline void parse_args(int, char **);

// parameters
size_t initial_size = 1000000;
size_t table_size = 150000000;
size_t target_size = 100000000;
size_t runtime = 10;
size_t fg_n = 1;
size_t bg_n = 1;
size_t seed = SEED;

// YCSB variables
char* cluster_number = "12.2";

volatile bool running = false;
std::atomic<size_t> ready_threads(0);

struct alignas(CACHELINE_SIZE) FGParam {
    struct wormref *wh;
    uint64_t throughput;
    uint32_t thread_id;
    bool alive;

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
    Key(const char* s) {
        int idx = 0;
        for (int i = 0; i < len; i++) {
            buf[i] = s[idx++];
        }
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
            os << (char)key.buf[i];
        }
        os << "] (as byte)" << std::dec;
        return os;
    }

    uint8_t buf[len];
};

int main(int argc, char **argv) {
    parse_args(argc, argv);
    struct wormhole *wh_raw = wormhole_create(NULL);
    struct wormref *wh = whsafe_ref(wh_raw);

    prepare_xindex(wh);

    run_benchmark(wh, runtime);
    
    wh_unref(wh);
    wh_destroy(wh_raw);

    return 0;
}

inline void prepare_xindex(struct wormref *wh) {
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

    while (std::getline(tracefile, line)) {
		sscanf(line.c_str(), "%s", key);
		index_key_t query_key(key);
        bool res = whsafe_put(wh, kv_create((char *)query_key.buf, MAX_KEY_SIZE, "abcdefgh", 8));
        assert(res);
		_tablesize++;
	}
	printf("Loaded keys: %ld\n", _tablesize);
    return;
}

void *run_fg(void *param) {
    fg_param_t &thread_param = *(fg_param_t *)param;
    uint32_t thread_id = thread_param.thread_id;
    struct wormref *wh = thread_param.wh;

    std::mt19937 gen(seed);
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

    while (running && token) {

        char op;
        int readcount;
        const char *line = token;
        uint64_t key;

        if (training_threads > 0) {
            std::unique_lock<std::mutex> lck(training_threads_mutex);             // Acquire lock
            training_threads_cond.wait(lck, []{return (training_threads == 0);}); // Release lock & wait
            lck.unlock();
        }

        index_key_t query_key(line + 2);
        op = line[0];
        token = strtok_r(NULL, delim, &remains);

        if (!token) break;

        switch (op) {
            case 'g':
            {
                struct kref kref;
                kv out;
                kref_ref_hash32(&kref, query_key.buf, MAX_KEY_SIZE);
                clock_gettime(CLOCK_MONOTONIC, &begin_t);
                auto res = whsafe_get(wh, &kref, &out);
                break;
            }
            case 'p':
            {
                clock_gettime(CLOCK_MONOTONIC, &begin_t);
                auto res = whsafe_put(wh, kv_create(query_key.buf, MAX_KEY_SIZE, "abcdefgh", 8));
                assert(res);
                break;
            }
            case 'd':
            {
                struct kref kref;
                kref_ref_hash32(&kref, query_key.buf, MAX_KEY_SIZE);
                clock_gettime(CLOCK_MONOTONIC, &begin_t);
                auto res = whsafe_del(wh, &kref);
                assert(res);
                break;
            }
            case 's':
            {
                struct wormhole_iter * const iter = wh_iter_create(wh);
                struct kref kref;
                std::vector<kv *> results;

                kref_ref_hash32(&kref, query_key.buf, MAX_KEY_SIZE);
                
                clock_gettime(CLOCK_MONOTONIC, &begin_t);

                int range_records = 10;

                whsafe_iter_seek(iter, &kref);
                for (int i=0; i<range_records; i++) {
                    if (!wormhole_iter_valid(iter)) break;
                    kv out;
                    auto res = wormhole_iter_peek(iter, &out);
                    results.push_back(res);
                    wormhole_iter_skip(iter, 1);
                }

                whsafe_iter_park(iter);
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

void run_benchmark(struct wormref *wh, size_t sec) {
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
        fg_params[worker_i].wh = wh;
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

    std::vector<size_t> tput_history(fg_n, 0);
    struct timespec begin_t, end_t;
    uint64_t total_keys = initial_size;

    running = true;

    //double current_sec = 0.0;
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
        int rc = pthread_join(threads[i], &status);
        if (rc) {
            COUT_N_EXIT("Error:unable to join," << rc);
        }
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
        {"initial-size", required_argument, 0, 'p'},
        {"target-size", required_argument, 0, 'q'},
        {"cluster-number", required_argument, 0, 'w'},
        {0, 0, 0, 0}};
    std::string ops = "f:g:h:i:p:q:w:";
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
            case 'p':
                initial_size = strtoul(optarg, NULL, 10);
                INVARIANT(table_size > 0);
                break;
            case 'q':
                target_size = strtoul(optarg, NULL, 10);
                INVARIANT(table_size > 0);
                break;
            case 'w':
                memcpy(&cluster_number, optarg, 1);
                break;
            default:
                abort();
        }
    }

    COUT_VAR(runtime);
    COUT_VAR(fg_n);
    COUT_VAR(bg_n);
    COUT_VAR(table_size);
    COUT_VAR(initial_size);
    COUT_VAR(target_size);
    COUT_VAR(cluster_number);
}
