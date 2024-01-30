#ifndef TEST_CONFIG
#define TEST_CONFIG

#include <iostream>

// #define MAX_KEY_SIZE 12
#define SEED 38688

#define ENTIRE_RANDOM

// Used for Latency Breakdown
// #define LATENCY_BREAKDOWN

// Microbench Query Patterns
// #define SEQUENTIAL_DIST
// #define UNIFORM_DIST
// #define LATEST_DIST
// #define EXPONENT_DIST
// #define ZIPF_DIST
// #define HOTSPOT_DIST
#define EXP_LAMBDA 10 // Used in exponent dist
#define HOTSPOT_LENGTH 1000000 // Used in hotspot dist

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
