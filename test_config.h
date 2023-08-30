#ifndef TEST_CONFIG
#define TEST_CONFIG

#include <iostream>

#define MAX_KEY_SIZE 64
#define SEED 38688

#define ENTIRE_RANDOM
#define PRINT_LATENCY

//#define SINGLE_THREADED
#define MAX_RANGE_RECORDS 100

// Used for Latency Breakdown
//#define LATENCY_BREAKDOWN

// Used for setting training Interval
#define IDEAL_TRAINING_INTERVAL 300

#define TMP_INPUT_PATH "/tmp/input_matrix/"
#define TMP_OUTPUT_PATH "/tmp/output_matrix/"

#define USE_PREFIX

//#define LIMIT_THROUGHPUT
//#define MAX_THROUGHPUT 7500

#define COUT_THIS(this) std::cout << this << std::endl;
#define COUT_VAR(this) std::cout << #this << ": " << this << std::endl;
#define COUT_POS() COUT_THIS("at " << __FILE__ << ":" << __LINE__)
#define COUT_N_EXIT(msg) \
  COUT_THIS(msg);        \
  COUT_POS();            \
  abort();
#define INVARIANT(cond)            \
  if (!(cond)) {                   \
    COUT_THIS(#cond << " failed"); \
    COUT_POS();                    \
    abort();                       \
  }

#endif
