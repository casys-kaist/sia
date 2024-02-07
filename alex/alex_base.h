// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

/* This file contains the classes for linear models and model builders, helpers
 * for the bitmap,
 * cost model weights, statistic accumulators for collecting cost model
 * statistics,
 * and other miscellaneous functions
 */

#pragma once

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <queue>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <typeinfo>
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#include <bitset>
#include <cassert>
#include <condition_variable>
#ifdef _WIN32
#include <intrin.h>
#include <limits.h>
typedef unsigned __int32 uint32_t;
#else
#include <stdint.h>
#endif
#include "mkl.h"
#include "mkl_lapacke.h"

#ifdef _MSC_VER
#define forceinline __forceinline
#elif defined(__GNUC__)
#define forceinline inline __attribute__((__always_inline__))
#elif defined(__CLANG__)
#if __has_attribute(__always_inline__)
#define forceinline inline __attribute__((__always_inline__))
#else
#define forceinline inline
#endif
#else
#define forceinline inline
#endif

/*** string's limit value ***/
#define STR_VAL_MAX 127
#define STR_VAL_MIN 0

/*** insertion location ***/
#define INSERT_AT_DATA 0
#define INSERT_AT_DELTA 1
#define INSERT_AT_TMPDELTA 2

/*** Mode ***/
#define KEY_ARR 0
#define DELTA_IDX 1
#define TMP_DELTA_IDX 2

/*** debug print ***/
#define DEBUG_PRINT 0

/*** profile ***/
#define PROFILE 0

/*** some utils for multithreading ***/
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define COUT_VAR(this) std::cout << #this << ": " << this << std::endl;
#define UNUSED(var) ((void)var)
#define CACHELINE_SIZE (1 << 6)

namespace alex {

static const size_t desired_training_key_n_ = 10000000; /* desired training key according to Xindex */

struct alignas(CACHELINE_SIZE) RCUStatus;
enum class Result;
struct IndexConfig;

typedef RCUStatus rcu_status_t;
typedef Result result_t;
typedef IndexConfig index_config_t;

unsigned int max_key_length_ = 1; //length of kesys.

/* AlexKey class. */
template<class T>
class AlexKey {
 public:
  T *key_arr_ = nullptr;

  AlexKey() {

    key_arr_ = new T[max_key_length_];
  }

  AlexKey(T *key_arr) {
    key_arr_ = new T[max_key_length_];
    std::copy(key_arr, key_arr + max_key_length_, key_arr_);
  }

  AlexKey(const AlexKey<T>& other) {
    key_arr_ = new T[max_key_length_]();
    std::copy(other.key_arr_, other.key_arr_ + max_key_length_, key_arr_);
  }

  ~AlexKey() {
      delete[] key_arr_;
  }

  AlexKey<T>& operator=(const AlexKey<T>& other) {
    assert(other.key_arr_ != nullptr);
    if (this != &other) {
      if ((key_arr_ == nullptr)) {
        key_arr_ = new T[max_key_length_];
      }
      std::copy(other.key_arr_, other.key_arr_ + max_key_length_, key_arr_);
    }
    return *this;
  }

  bool operator< (const AlexKey<T>& other) const {
    assert(key_arr_ != nullptr && other.key_arr_ != nullptr);
    for (unsigned int i = 0; i < max_key_length_; i++) {
      if (key_arr_[i] < other.key_arr_[i]) {return true;}
      else if (key_arr_[i] > other.key_arr_[i]) {return false;}
    }
    return false;
  }

};

/*** Linear model and model builder ***/

// Forward declaration
template <class T>
class LinearModelBuilder;

/* Linear Regression Model */
template <class T>
class LinearModel {
 public:
  double *a_ = nullptr;  // slope, we MUST initialize.
  double b_ = 0.0;  // intercept, we MUST initialize by ourself.

  LinearModel() {
    a_ = new double[max_key_length_]();
  }

  LinearModel(double a[], double b) {
    a_ = new double[max_key_length_];
    for (unsigned int i = 0; i < max_key_length_; i++) {
      a_[i] = a[i];
    }
    b_ = b;
  }

  ~LinearModel() {
    delete[] a_;
  }

  explicit LinearModel(const LinearModel& other) : b_(other.b_) {
      a_ = new double[max_key_length_];
      for (unsigned int i = 0; i < max_key_length_; i++) {
        a_[i] = other.a_[i];
      }
  }

  LinearModel& operator=(const LinearModel& other) {
    if (this != &other) {
      b_ = other.b_;
      if (a_ == nullptr) a_ = new double[max_key_length_];
      std::copy(other.a_, other.a_ + max_key_length_, a_);
    }
    return *this;
  }

  void expand(double expansion_factor) {
    assert(a_ != nullptr);
    for (unsigned int i = 0; i < max_key_length_; i++) {
      a_[i] *= expansion_factor;
    }
    b_ *= expansion_factor;
  }

  inline int predict(const AlexKey<T> &key) const {
    assert(a_ != nullptr && key.key_arr_ != nullptr);
    double result = 0.0;
    for (unsigned int i = 0; i < max_key_length_; i++) {
      result += static_cast<double>(key.key_arr_[i]) * a_[i];
    }
    return static_cast<int>(result + b_);
  }

  inline double predict_double(const AlexKey<T> &key) const {
    assert(a_ != nullptr && key.key_arr_ != nullptr);
    double result = 0.0;
    for (unsigned int i = 0; i < max_key_length_; i++) {
      result += static_cast<double>(key.key_arr_[i]) * a_[i];
    }
    return result + b_;
  }
};

/* LinearModelBuilder acts very similar to XIndex model preparing. */
template<class T>
class LinearModelBuilder {
 public:
  LinearModel<T>* model_ = nullptr;

  LinearModelBuilder(LinearModel<T>* model) : model_(model) {}

  inline void add(const AlexKey<T>& x, double y) {
    if (max_key_length_ == 1) { //single numeric
      count_++;
      x_sum_ += static_cast<long double>(x.key_arr_[0]);
      y_sum_ += static_cast<long double>(y);
      xx_sum_ += static_cast<long double>(x.key_arr_[0]) * x.key_arr_[0];
      xy_sum_ += static_cast<long double>(x.key_arr_[0]) * y;
      x_min_ = std::min<T>(x.key_arr_[0], x_min_);
      x_max_ = std::max<T>(x.key_arr_[0], x_max_);
      y_min_ = std::min<double>(y, y_min_);
      y_max_ = std::max<double>(y, y_max_);
    }
    else { //string
      training_keys_.push_back(x.key_arr_); //may need to deep copy?
      positions_.push_back(y);
    }
  }

  void build() {
    assert(model_->a_ != nullptr);

    if (max_key_length_ == 1) { /* single dimension */     
      if (count_ <= 1) {
        model_->a_[0] = 0;
        model_->b_ = static_cast<double>(y_sum_);
        return;
      }

      if (static_cast<long double>(count_) * xx_sum_ - x_sum_ * x_sum_ == 0) {
        // all values in a bucket have the same key.
        model_->a_[0] = 0;
        model_->b_ = static_cast<double>(y_sum_) / count_;
        return;
      }

      auto slope = static_cast<double>(
        (static_cast<long double>(count_) * xy_sum_ - x_sum_ * y_sum_) /
        (static_cast<long double>(count_) * xx_sum_ - x_sum_ * x_sum_));
      auto intercept = static_cast<double>(
        (y_sum_ - static_cast<long double>(slope) * x_sum_) / count_);
      model_->a_[0] = slope;
      model_->b_ = intercept;

      // If floating point precision errors, fit spline
      if (model_->a_[0] <= 0) {
        model_->a_[0] = (y_max_ - y_min_) / (double) (x_max_ - x_min_);
        model_->b_ = -static_cast<double>(x_min_) * model_->a_[0];
      }
      return;
    }

    //from here, it is about string.

    if (positions_.size() <= 1) {
      for (unsigned int i = 0; i < max_key_length_; i++) {
        model_->a_[i] = 0.0;
      }
      if (positions_.size() == 0) {model_->b_ = 0.0;}
      else {model_->b_ = positions_[0];}

      return;
    }

    // trim down samples to avoid large memory usage
    size_t step = 1;
    if (training_keys_.size() > desired_training_key_n_) {
      step = training_keys_.size() / desired_training_key_n_;
    }

    std::vector<size_t> useful_feat_index_;
    for (size_t feat_i = 0; feat_i < max_key_length_; feat_i++) {
      double first_val = (double) training_keys_[0][feat_i];
      for (size_t key_i = 0; key_i < training_keys_.size(); key_i += step) {
        if ((double) training_keys_[key_i][feat_i] != first_val) {
          useful_feat_index_.push_back(feat_i);
          break;
        }
      }
    }

    if (training_keys_.size() != 1 && useful_feat_index_.size() == 0) {
      //std::cout<<"all feats are the same"<<std::endl;
    }
    size_t useful_feat_n_ = useful_feat_index_.size();
    bool use_bias_ = true;

    // we may need multiple runs to avoid "not full rank" error
    int fitting_res = -1;
    while (fitting_res != 0) {
      // use LAPACK to solve least square problem, i.e., to minimize ||b-Ax||_2
      // where b is the actual positions, A is inputmodel_keys
      int m = training_keys_.size() / step;                     // number of samples
      int n = use_bias_ ? useful_feat_n_ + 1 : useful_feat_n_;  // number of features
      double *A = (double *) malloc(m * n * sizeof(double));
      double *b = (double *) malloc(std::max(m, n) * sizeof(double));
      if (A == nullptr || b == nullptr) {
        std::cout<<"cannot allocate memory for matrix A or b\n";
        std::cout<<"at "<<__FILE__<<":"<<__LINE__<<std::endl;
        abort();
      }

      for (int sample_i = 0; sample_i < m; ++sample_i) {
        // we only fit with useful features
        for (size_t useful_feat_i = 0; useful_feat_i < useful_feat_n_;
            useful_feat_i++) {
          A[sample_i * n + useful_feat_i] =
              training_keys_[sample_i * step][useful_feat_index_[useful_feat_i]];
        }
        if (use_bias_) {
          A[sample_i * n + useful_feat_n_] = 1;  // the extra 1
        }
        b[sample_i] = positions_[sample_i * step];
        assert(sample_i * step < training_keys_.size());
      }

      // fill the rest of b when m < n, otherwise nan value will cause failure
      for (int i = m; i < n; i++) {
        b[i] = 0;
      }

      fitting_res = LAPACKE_dgels(LAPACK_ROW_MAJOR, 'N', m, n, 1 /* nrhs */, A,
                                  n /* lda */, b, 1 /* ldb, i.e. nrhs */);

      if (fitting_res > 0) {
        // now we need to remove one column in matrix a
        // note that fitting_res indexes starting with 1
        if ((size_t)fitting_res > useful_feat_index_.size()) {
          use_bias_ = false;
        } else {
          size_t feat_i = fitting_res - 1;
          useful_feat_index_.erase(useful_feat_index_.begin() + feat_i);
          useful_feat_n_ = useful_feat_index_.size();
        }

        if (useful_feat_index_.size() == 0 && use_bias_ == false) {
          std::cout<<"impossible! cannot fail when there is only 1 bias column in matrix a\n";
          std::cout<<"at "<<__FILE__<<":"<<__LINE__<<std::endl;
          abort();
        }
      } else if (fitting_res < 0) {
        printf("%i-th parameter had an illegal value\n", -fitting_res);
        exit(-2);
      }

      // set weights to all zero
      for (size_t weight_i = 0; weight_i < max_key_length_; weight_i++) {
        model_->a_[weight_i] = 0;
      }
      // set weights of useful features
      for (size_t useful_feat_i = 0; useful_feat_i < useful_feat_index_.size();
          useful_feat_i++) {
        model_->a_[useful_feat_index_[useful_feat_i]] = b[useful_feat_i];
      }
      // set bias
      if (use_bias_) {
        model_->b_ = b[n - 1];
      }

      free(A);
      free(b);
      mkl_free_buffers();

#if DEBUG_PRINT
      //for debugging
      //alex::coutLock.lock();
      //std::cout << "current a_ (LMB build): ";
      //for (unsigned int i = 0; i < max_key_length_; i++) {
      //  std::cout << model_->a_[i] << " ";
      //}
      //std::cout << ", current b_ (LMB build) :" << model_->b_ << std::endl;
      //alex::coutLock.unlock();
#endif
    }
    assert(fitting_res == 0);
  }

 private:
  int count_ = 0;
  long double x_sum_ = 0;
  long double y_sum_ = 0;
  long double xx_sum_ = 0;
  long double xy_sum_ = 0;
  T x_min_ = std::numeric_limits<T>::max();
  T x_max_ = std::numeric_limits<T>::lowest();
  double y_min_ = std::numeric_limits<double>::max();
  double y_max_ = std::numeric_limits<double>::lowest();
  std::vector<T*> training_keys_;
  std::vector<double> positions_;
};

/*** Comparison ***/

struct AlexCompare {
  template <class T1, class T2>
  bool operator()(const AlexKey<T1>& x, const AlexKey<T2>& y) const {
    static_assert(
        std::is_arithmetic<T1>::value && std::is_arithmetic<T2>::value,
        "Comparison types must be numeric.");
    assert(x.key_arr_ != nullptr && y.key_arr_ != nullptr);
    for (unsigned int i = 0; i < max_key_length_; i++) {
      if (x.key_arr_[i] < y.key_arr_[i]) {return true;}
      else if (x.key_arr_[i] > y.key_arr_[i]) {return false;}
    }
    return false;
  }
};

/*** Helper methods for bitmap ***/

// Extract the rightmost 1 in the binary representation.
// e.g. extract_rightmost_one(010100100) = 000000100
inline uint64_t extract_rightmost_one(uint64_t value) {
  return value & -static_cast<int64_t>(value);
}

// Remove the rightmost 1 in the binary representation.
// e.g. remove_rightmost_one(010100100) = 010100000
inline uint64_t remove_rightmost_one(uint64_t value) {
  return value & (value - 1);
}

// Count the number of 1s in the binary representation.
// e.g. count_ones(010100100) = 3
inline int count_ones(uint64_t value) {
  return static_cast<int>(_mm_popcnt_u64(value));
}

// Get the offset of a bit in a bitmap.
// word_id is the word id of the bit in a bitmap
// bit is the word that contains the bit
inline int get_offset(int word_id, uint64_t bit) {
  return (word_id << 6) + count_ones(bit - 1);
}

/*** Cost model weights ***/

// Intra-node cost weights
constexpr double kExpSearchIterationsWeight = 20;
constexpr double kShiftsWeight = 0.5;

// TraverseToLeaf cost weights
constexpr double kNodeLookupsWeight = 20;
constexpr double kModelSizeWeight = 5e-7;

/*** Stat Accumulators ***/

struct DataNodeStats {
  double num_search_iterations = 0;
  double num_shifts = 0;
};

// Used when stats are computed using a sample
struct SampleDataNodeStats {
  double log2_sample_size = 0;
  double num_search_iterations = 0;
  double log2_num_shifts = 0;
};

// Accumulates stats that are used in the cost model, based on the actual vs
// predicted position of a key
class StatAccumulator {
 public:
  virtual ~StatAccumulator() = default;
  virtual void accumulate(int actual_position, int predicted_position) = 0;
  virtual double get_stat() = 0;
  virtual void reset() = 0;
};

// Mean log error represents the expected number of exponential search
// iterations when doing a lookup
class ExpectedSearchIterationsAccumulator : public StatAccumulator {
 public:
  void accumulate(int actual_position, int predicted_position) override {
    cumulative_log_error_ +=
        std::log2(std::abs(predicted_position - actual_position) + 1);
    count_++;
  }

  double get_stat() override {
    if (count_ == 0) return 0;
    return cumulative_log_error_ / count_;
  }

  void reset() override {
    cumulative_log_error_ = 0;
    count_ = 0;
  }

 public:
  double cumulative_log_error_ = 0;
  int count_ = 0;
};

// Mean shifts represents the expected number of shifts when doing an insert
class ExpectedShiftsAccumulator : public StatAccumulator {
 public:
  explicit ExpectedShiftsAccumulator(int data_capacity)
      : data_capacity_(data_capacity) {}

  // A dense region of n keys will contribute a total number of expected shifts
  // of approximately
  // ((n-1)/2)((n-1)/2 + 1) = n^2/4 - 1/4
  // This is exact for odd n and off by 0.25 for even n.
  // Therefore, we track n^2/4.
  void accumulate(int actual_position, int) override {
    if (actual_position > last_position_ + 1) {
      long long dense_region_length = last_position_ - dense_region_start_idx_ + 1;
      num_expected_shifts_ += (dense_region_length * dense_region_length) / 4;
      dense_region_start_idx_ = actual_position;
    }
    last_position_ = actual_position;
    count_++;
  }

  double get_stat() override {
    if (count_ == 0) return 0;
    // first need to accumulate statistics for current packed region
    long long dense_region_length = last_position_ - dense_region_start_idx_ + 1;
    long long cur_num_expected_shifts =
        num_expected_shifts_ + (dense_region_length * dense_region_length) / 4;
    return cur_num_expected_shifts / static_cast<double>(count_);
  }

  void reset() override {
    last_position_ = -1;
    dense_region_start_idx_ = 0;
    num_expected_shifts_ = 0;
    count_ = 0;
  }

 public:
  int last_position_ = -1;
  int dense_region_start_idx_ = 0;
  long long num_expected_shifts_ = 0;
  int count_ = 0;
  int data_capacity_ = -1;  // capacity of node
};

// Combines ExpectedSearchIterationsAccumulator and ExpectedShiftsAccumulator
class ExpectedIterationsAndShiftsAccumulator : public StatAccumulator {
 public:
  ExpectedIterationsAndShiftsAccumulator() = default;
  explicit ExpectedIterationsAndShiftsAccumulator(int data_capacity)
      : data_capacity_(data_capacity) {}

  void accumulate(int actual_position, int predicted_position) override {
    cumulative_log_error_ +=
        std::log2(std::abs(predicted_position - actual_position) + 1);

    if (actual_position > last_position_ + 1) {
      long long dense_region_length = last_position_ - dense_region_start_idx_ + 1;
      num_expected_shifts_ += (dense_region_length * dense_region_length) / 4;
      dense_region_start_idx_ = actual_position;
    }
    last_position_ = actual_position;

    count_++;
  }

  double get_stat() override {
    assert(false);  // this should not be used
    return 0;
  }

  double get_expected_num_search_iterations() {
    if (count_ == 0) return 0;
    return cumulative_log_error_ / count_;
  }

  double get_expected_num_shifts() {
    if (count_ == 0) return 0;
    long long dense_region_length = last_position_ - dense_region_start_idx_ + 1;
    long long cur_num_expected_shifts =
        num_expected_shifts_ + (dense_region_length * dense_region_length) / 4;
    return cur_num_expected_shifts / static_cast<double>(count_);
  }

  void reset() override {
    cumulative_log_error_ = 0;
    last_position_ = -1;
    dense_region_start_idx_ = 0;
    num_expected_shifts_ = 0;
    count_ = 0;
  }

 public:
  double cumulative_log_error_ = 0;
  int last_position_ = -1;
  int dense_region_start_idx_ = 0;
  long long num_expected_shifts_ = 0;
  int count_ = 0;
  int data_capacity_ = -1;  // capacity of node
};

/*** Miscellaneous helpers ***/

// https://stackoverflow.com/questions/364985/algorithm-for-finding-the-smallest-power-of-two-thats-greater-or-equal-to-a-giv
inline int pow_2_round_up(int x) {
  --x;
  x |= x >> 1;
  x |= x >> 2;
  x |= x >> 4;
  x |= x >> 8;
  x |= x >> 16;
  return x + 1;
}

// https://stackoverflow.com/questions/994593/how-to-do-an-integer-log2-in-c
inline int log_2_round_down(int x) {
  int res = 0;
  while (x >>= 1) ++res;
  return res;
}

// https://stackoverflow.com/questions/1666093/cpuid-implementations-in-c
class CPUID {
  uint32_t regs[4];

 public:
  explicit CPUID(unsigned i, unsigned j) {
#ifdef _WIN32
    __cpuidex((int*)regs, (int)i, (int)j);
#else
    asm volatile("cpuid"
                 : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]), "=d"(regs[3])
                 : "a"(i), "c"(j));
#endif
  }

  const uint32_t& EAX() const { return regs[0]; }
  const uint32_t& EBX() const { return regs[1]; }
  const uint32_t& ECX() const { return regs[2]; }
  const uint32_t& EDX() const { return regs[3]; }
};

// https://en.wikipedia.org/wiki/CPUID#EAX=7,_ECX=0:_Extended_Features
inline bool cpu_supports_bmi() {
  return static_cast<bool>(CPUID(7, 0).EBX() & (1 << 3));
}

#define fgStatType int64_t
#define bgStatType int64_t
#define lkStatType int64_t

#define fgTimeUnit nanoseconds
#define bgTimeUnit nanoseconds
#define lkTimeUnit nanoseconds

#if PROFILE
struct ProfileStats {
  uint32_t td_num; //number of threads

  //time related
  //for foreground threads
  fgStatType *get_payload_from_superroot_success_time,
             *max_get_payload_from_superroot_success_time,
             *min_get_payload_from_superroot_success_time;
  fgStatType *get_payload_from_parent_success_time,
             *max_get_payload_from_parent_success_time,
             *min_get_payload_from_parent_success_time;
  fgStatType *get_payload_from_superroot_fail_time,
             *max_get_payload_from_superroot_fail_time,
             *min_get_payload_from_superroot_fail_time;
  fgStatType *get_payload_from_parent_fail_time,
             *max_get_payload_from_parent_fail_time,
             *min_get_payload_from_parent_fail_time;
  fgStatType *insert_from_superroot_success_time,
             *max_insert_from_superroot_success_time,
             *min_insert_from_superroot_success_time;
  fgStatType *insert_from_parent_success_time,
             *max_insert_from_parent_success_time,
             *min_insert_from_parent_success_time;
  fgStatType *insert_from_superroot_fail_time,
             *max_insert_from_superroot_fail_time,
             *min_insert_from_superroot_fail_time;
  fgStatType *insert_from_parent_fail_time,
             *max_insert_from_parent_fail_time,
             *min_insert_from_parent_fail_time;
  fgStatType *get_leaf_from_get_payload_superroot_time,
             *max_get_leaf_from_get_payload_superroot_time,
             *min_get_leaf_from_get_payload_superroot_time;
  fgStatType *get_leaf_from_get_payload_directp_time,
             *max_get_leaf_from_get_payload_directp_time,
             *min_get_leaf_from_get_payload_directp_time;
  fgStatType *get_leaf_from_insert_superroot_time,
             *max_get_leaf_from_insert_superroot_time,
             *min_get_leaf_from_insert_superroot_time;
  fgStatType *get_leaf_from_insert_directp_time,
             *max_get_leaf_from_insert_directp_time,
             *min_get_leaf_from_insert_directp_time;
  fgStatType *find_key_time,
             *max_find_key_time,
             *min_find_key_time;
  fgStatType *insert_using_shifts_time,
             *max_insert_using_shifts_time,
             *min_insert_using_shifts_time;
  fgStatType *insert_element_at_time,
             *max_insert_element_at_time,
             *min_insert_element_at_time;
  fgStatType *find_insert_position_time,
             *max_find_insert_position_time,
             *min_find_insert_position_time;
  

  //fore background threads
  std::atomic<bgStatType> resize_time, max_resize_time, min_resize_time;
  std::atomic<bgStatType> find_best_fanout_existing_node_time,
                          max_find_best_fanout_existing_node_time,
                          min_find_best_fanout_existing_node_time;
  std::atomic<bgStatType> fanout_model_train_time,
                          max_fanout_model_train_time,
                          min_fanout_model_train_time;
  std::atomic<bgStatType> fanout_data_train_time,
                          max_fanout_data_train_time,
                          min_fanout_data_train_time;
  std::atomic<bgStatType> fanout_batch_stat_time,
                          max_fanout_batch_stat_time,
                          min_fanout_batch_stat_time;
  std::atomic<bgStatType> split_downwards_time,
                          max_split_downwards_time,
                          min_split_downwards_time;
  std::atomic<bgStatType> split_sideways_time,
                          max_split_sideways_time,
                          min_split_sideways_time;

  //for both fore/background threads
  std::atomic<lkStatType> lock_achieve_time,
                       max_lock_achieve_time,
                       min_lock_achieve_time;

  //count related
  //for foreground threads
  uint64_t *get_payload_superroot_call_cnt;
  uint64_t *get_payload_directp_call_cnt;
  uint64_t *get_payload_superroot_success_cnt;
  uint64_t *get_payload_directp_success_cnt;
  uint64_t *get_payload_superroot_fail_cnt;
  uint64_t *get_payload_directp_fail_cnt;
  uint64_t *insert_superroot_call_cnt;
  uint64_t *insert_directp_call_cnt;
  uint64_t *insert_superroot_success_cnt;
  uint64_t *insert_directp_success_cnt;
  uint64_t *insert_superroot_fail_cnt;
  uint64_t *insert_directp_fail_cnt;
  uint64_t *get_leaf_from_get_payload_superroot_call_cnt;
  uint64_t *get_leaf_from_get_payload_directp_call_cnt;
  uint64_t *get_leaf_from_insert_superroot_call_cnt;
  uint64_t *get_leaf_from_insert_directp_call_cnt;
  uint64_t *insert_using_shifts_call_cnt;
  uint64_t *insert_element_at_call_cnt;
  uint64_t *find_key_call_cnt;
  uint64_t *find_insert_position_call_cnt;

  //for background threads
  std::atomic<uint64_t> resize_call_cnt;
  std::atomic<uint64_t> find_best_fanout_existing_node_call_cnt;
  std::atomic<uint64_t> fanout_model_train_cnt;
  std::atomic<uint64_t> fanout_data_train_cnt;
  std::atomic<uint64_t> fanout_batch_stat_cnt;
  std::atomic<uint64_t> split_downwards_call_cnt;
  std::atomic<uint64_t> split_sideways_call_cnt;
  //AtomicVal<uint64_t> create_two_new_data_nodes_call_cnt;
  //AtomicVal<uint64_t> create_new_data_nodes_call_cnt;

  //for both fore/background threads
  std::atomic<uint64_t> lock_achieve_cnt;

  //initializing profile structures
  void profileInit (uint32_t thread_num) {
    td_num = thread_num;
    get_payload_from_superroot_success_time = new fgStatType[thread_num];
    max_get_payload_from_superroot_success_time = new fgStatType[thread_num];
    min_get_payload_from_superroot_success_time = new fgStatType[thread_num];
    get_payload_from_parent_success_time = new fgStatType[thread_num];
    max_get_payload_from_parent_success_time = new fgStatType[thread_num];
    min_get_payload_from_parent_success_time = new fgStatType[thread_num];
    get_payload_from_superroot_fail_time = new fgStatType[thread_num];
    max_get_payload_from_superroot_fail_time = new fgStatType[thread_num];
    min_get_payload_from_superroot_fail_time = new fgStatType[thread_num];
    get_payload_from_parent_fail_time = new fgStatType[thread_num];
    max_get_payload_from_parent_fail_time = new fgStatType[thread_num];
    min_get_payload_from_parent_fail_time = new fgStatType[thread_num];
    insert_from_superroot_success_time = new fgStatType[thread_num];
    max_insert_from_superroot_success_time = new fgStatType[thread_num];
    min_insert_from_superroot_success_time = new fgStatType[thread_num];
    insert_from_parent_success_time = new fgStatType[thread_num];
    max_insert_from_parent_success_time = new fgStatType[thread_num];
    min_insert_from_parent_success_time = new fgStatType[thread_num];
    insert_from_superroot_fail_time = new fgStatType[thread_num];
    max_insert_from_superroot_fail_time = new fgStatType[thread_num];
    min_insert_from_superroot_fail_time = new fgStatType[thread_num];
    insert_from_parent_fail_time = new fgStatType[thread_num];
    max_insert_from_parent_fail_time = new fgStatType[thread_num];
    min_insert_from_parent_fail_time = new fgStatType[thread_num];
    get_leaf_from_get_payload_superroot_time = new fgStatType[thread_num];
    max_get_leaf_from_get_payload_superroot_time = new fgStatType[thread_num];
    min_get_leaf_from_get_payload_superroot_time = new fgStatType[thread_num];
    get_leaf_from_get_payload_directp_time = new fgStatType[thread_num];
    max_get_leaf_from_get_payload_directp_time = new fgStatType[thread_num];
    min_get_leaf_from_get_payload_directp_time = new fgStatType[thread_num];
    get_leaf_from_insert_superroot_time = new fgStatType[thread_num];
    max_get_leaf_from_insert_superroot_time = new fgStatType[thread_num];
    min_get_leaf_from_insert_superroot_time = new fgStatType[thread_num];
    get_leaf_from_insert_directp_time = new fgStatType[thread_num];
    max_get_leaf_from_insert_directp_time = new fgStatType[thread_num];
    min_get_leaf_from_insert_directp_time = new fgStatType[thread_num];
    find_key_time = new fgStatType[thread_num];
    max_find_key_time = new fgStatType[thread_num];
    min_find_key_time = new fgStatType[thread_num];
    insert_using_shifts_time = new fgStatType[thread_num];
    max_insert_using_shifts_time = new fgStatType[thread_num];
    min_insert_using_shifts_time = new fgStatType[thread_num];
    insert_element_at_time = new fgStatType[thread_num];
    max_insert_element_at_time = new fgStatType[thread_num];
    min_insert_element_at_time = new fgStatType[thread_num];
    find_insert_position_time = new fgStatType[thread_num];
    max_find_insert_position_time = new fgStatType[thread_num];
    min_find_insert_position_time = new fgStatType[thread_num];
    get_payload_superroot_call_cnt = new uint64_t[thread_num];
    get_payload_directp_call_cnt = new uint64_t[thread_num];
    get_payload_superroot_success_cnt = new uint64_t[thread_num];
    get_payload_directp_success_cnt = new uint64_t[thread_num];
    get_payload_superroot_fail_cnt = new uint64_t[thread_num];
    get_payload_directp_fail_cnt = new uint64_t[thread_num];
    insert_superroot_call_cnt = new uint64_t[thread_num];
    insert_directp_call_cnt = new uint64_t[thread_num];
    insert_superroot_success_cnt = new uint64_t[thread_num];
    insert_directp_success_cnt = new uint64_t[thread_num];
    insert_superroot_fail_cnt = new uint64_t[thread_num];
    insert_directp_fail_cnt = new uint64_t[thread_num];
    get_leaf_from_get_payload_superroot_call_cnt = new uint64_t[thread_num];
    get_leaf_from_get_payload_directp_call_cnt = new uint64_t[thread_num];
    get_leaf_from_insert_superroot_call_cnt = new uint64_t[thread_num];
    get_leaf_from_insert_directp_call_cnt = new uint64_t[thread_num];
    insert_using_shifts_call_cnt = new uint64_t[thread_num];
    insert_element_at_call_cnt = new uint64_t[thread_num];
    find_key_call_cnt = new uint64_t[thread_num];
    find_insert_position_call_cnt = new uint64_t[thread_num];
  }

  void profileReInit () {
    for (uint32_t i = 0; i < td_num; ++i) {
      get_payload_from_superroot_success_time[i] = 0;
      max_get_payload_from_superroot_success_time[i] = std::numeric_limits<fgStatType>::min();
      min_get_payload_from_superroot_success_time[i] = std::numeric_limits<fgStatType>::max();
      
      get_payload_from_parent_success_time[i] = 0;
      max_get_payload_from_parent_success_time[i] = std::numeric_limits<fgStatType>::min();
      min_get_payload_from_parent_success_time[i] = std::numeric_limits<fgStatType>::max();

      get_payload_from_superroot_fail_time[i] = 0;
      max_get_payload_from_superroot_fail_time[i] = std::numeric_limits<fgStatType>::min();
      min_get_payload_from_superroot_fail_time[i] = std::numeric_limits<fgStatType>::max();

      get_payload_from_parent_fail_time[i] = 0;
      max_get_payload_from_parent_fail_time[i] = std::numeric_limits<fgStatType>::min();
      min_get_payload_from_parent_fail_time[i] = std::numeric_limits<fgStatType>::max();

      insert_from_superroot_success_time[i] = 0;
      max_insert_from_superroot_success_time[i] = std::numeric_limits<fgStatType>::min();
      min_insert_from_superroot_success_time[i] = std::numeric_limits<fgStatType>::max();

      insert_from_parent_success_time[i] = 0;
      max_insert_from_parent_success_time[i] = std::numeric_limits<fgStatType>::min();
      min_insert_from_parent_success_time[i] = std::numeric_limits<fgStatType>::max();

      insert_from_superroot_fail_time[i] = 0;
      max_insert_from_superroot_fail_time[i] = std::numeric_limits<fgStatType>::min();
      min_insert_from_superroot_fail_time[i] = std::numeric_limits<fgStatType>::max();

      insert_from_parent_fail_time[i] = 0;
      max_insert_from_parent_fail_time[i] = std::numeric_limits<fgStatType>::min();
      min_insert_from_parent_fail_time[i] = std::numeric_limits<fgStatType>::max();

      get_leaf_from_get_payload_superroot_time[i] = 0;
      max_get_leaf_from_get_payload_superroot_time[i] = std::numeric_limits<fgStatType>::min();
      min_get_leaf_from_get_payload_superroot_time[i] = std::numeric_limits<fgStatType>::max();

      get_leaf_from_get_payload_directp_time[i] = 0;
      max_get_leaf_from_get_payload_directp_time[i] = std::numeric_limits<fgStatType>::min();
      min_get_leaf_from_get_payload_directp_time[i] = std::numeric_limits<fgStatType>::max();
    
      get_leaf_from_insert_superroot_time[i] = 0;
      max_get_leaf_from_insert_superroot_time[i] = std::numeric_limits<fgStatType>::min();
      min_get_leaf_from_insert_superroot_time[i] = std::numeric_limits<fgStatType>::max();

      get_leaf_from_insert_directp_time[i] = 0;
      max_get_leaf_from_insert_directp_time[i] = std::numeric_limits<fgStatType>::min();
      min_get_leaf_from_insert_directp_time[i] = std::numeric_limits<fgStatType>::max();

      find_key_time[i] = 0;
      max_find_key_time[i] = std::numeric_limits<fgStatType>::min();
      min_find_key_time[i] = std::numeric_limits<fgStatType>::max();

      find_insert_position_time[i] = 0;
      max_find_insert_position_time[i] = std::numeric_limits<fgStatType>::min();
      min_find_insert_position_time[i] = std::numeric_limits<fgStatType>::max();

      insert_using_shifts_time[i] = 0;
      max_insert_using_shifts_time[i] = std::numeric_limits<fgStatType>::min();
      min_insert_using_shifts_time[i] = std::numeric_limits<fgStatType>::max();

      insert_element_at_time[i] = 0;
      max_insert_element_at_time[i] = std::numeric_limits<fgStatType>::min();
      min_insert_element_at_time[i] = std::numeric_limits<fgStatType>::max();

      get_payload_superroot_call_cnt[i] = 0;
      get_payload_directp_call_cnt[i] = 0;
      get_payload_superroot_success_cnt[i] = 0;
      get_payload_directp_success_cnt[i] = 0;
      get_payload_superroot_fail_cnt[i] = 0;
      get_payload_directp_fail_cnt[i] = 0;
      insert_superroot_call_cnt[i] = 0;
      insert_directp_call_cnt[i] = 0;
      insert_superroot_success_cnt[i] = 0;
      insert_directp_success_cnt[i] = 0;
      insert_superroot_fail_cnt[i] = 0;
      insert_directp_fail_cnt[i] = 0;
      get_leaf_from_get_payload_superroot_call_cnt[i] = 0;
      get_leaf_from_get_payload_directp_call_cnt[i] = 0;
      get_leaf_from_insert_superroot_call_cnt[i] = 0;
      get_leaf_from_insert_directp_call_cnt[i] = 0;
      insert_using_shifts_call_cnt[i] = 0;
      insert_element_at_call_cnt[i] = 0;
      find_key_call_cnt[i] = 0;
      find_insert_position_call_cnt[i] = 0;
    }

    resize_time = 0;
    max_resize_time = std::numeric_limits<bgStatType>::min();
    min_resize_time = std::numeric_limits<bgStatType>::max();

    find_best_fanout_existing_node_time = 0;
    max_find_best_fanout_existing_node_time = std::numeric_limits<bgStatType>::min();
    min_find_best_fanout_existing_node_time = std::numeric_limits<bgStatType>::max();

    fanout_model_train_time = 0;
    max_fanout_model_train_time = std::numeric_limits<bgStatType>::min();
    min_fanout_model_train_time = std::numeric_limits<bgStatType>::max();

    fanout_data_train_time = 0;
    max_fanout_data_train_time = std::numeric_limits<bgStatType>::min();
    min_fanout_data_train_time = std::numeric_limits<bgStatType>::max();

    fanout_batch_stat_time = 0;
    max_fanout_batch_stat_time = std::numeric_limits<bgStatType>::min();
    min_fanout_batch_stat_time = std::numeric_limits<bgStatType>::max();

    split_downwards_time = 0;
    max_split_downwards_time = std::numeric_limits<bgStatType>::min();
    min_split_downwards_time = std::numeric_limits<bgStatType>::max();

    split_sideways_time = 0;
    max_split_sideways_time = std::numeric_limits<bgStatType>::min();
    min_split_sideways_time = std::numeric_limits<bgStatType>::max();

    lock_achieve_time = 0;

    resize_call_cnt = 0;
    find_best_fanout_existing_node_call_cnt = 0;
    fanout_model_train_cnt = 0;
    fanout_data_train_cnt = 0;
    fanout_batch_stat_cnt = 0;
    split_downwards_call_cnt = 0;
    split_sideways_call_cnt = 0;
    lock_achieve_cnt = 0;
  }

  //deleting profile structures
  void profileDelete () {
    delete[] get_payload_from_superroot_success_time;
    delete[] max_get_payload_from_superroot_success_time;
    delete[] min_get_payload_from_superroot_success_time;
    delete[] get_payload_from_parent_success_time;
    delete[] max_get_payload_from_parent_success_time;
    delete[] min_get_payload_from_parent_success_time;
    delete[] get_payload_from_superroot_fail_time;
    delete[] max_get_payload_from_superroot_fail_time;
    delete[] min_get_payload_from_superroot_fail_time;
    delete[] get_payload_from_parent_fail_time;
    delete[] max_get_payload_from_parent_fail_time;
    delete[] min_get_payload_from_parent_fail_time;
    delete[] insert_from_superroot_success_time;
    delete[] max_insert_from_superroot_success_time;
    delete[] min_insert_from_superroot_success_time;
    delete[] insert_from_parent_success_time;
    delete[] max_insert_from_parent_success_time;
    delete[] min_insert_from_parent_success_time;
    delete[] insert_from_superroot_fail_time;
    delete[] max_insert_from_superroot_fail_time;
    delete[] min_insert_from_superroot_fail_time;
    delete[] insert_from_parent_fail_time;
    delete[] max_insert_from_parent_fail_time;
    delete[] min_insert_from_parent_fail_time;
    delete[] get_leaf_from_get_payload_superroot_time;
    delete[] max_get_leaf_from_get_payload_superroot_time;
    delete[] min_get_leaf_from_get_payload_superroot_time;
    delete[] get_leaf_from_get_payload_directp_time;
    delete[] max_get_leaf_from_get_payload_directp_time;
    delete[] min_get_leaf_from_get_payload_directp_time;
    delete[] get_leaf_from_insert_superroot_time;
    delete[] max_get_leaf_from_insert_superroot_time;
    delete[] min_get_leaf_from_insert_superroot_time;
    delete[] get_leaf_from_insert_directp_time;
    delete[] max_get_leaf_from_insert_directp_time;
    delete[] min_get_leaf_from_insert_directp_time;
    delete[] find_key_time;
    delete[] max_find_key_time;
    delete[] min_find_key_time;
    delete[] find_insert_position_time;
    delete[] max_find_insert_position_time;
    delete[] min_find_insert_position_time;
    delete[] insert_using_shifts_time;
    delete[] max_insert_using_shifts_time;
    delete[] min_insert_using_shifts_time;
    delete[] insert_element_at_time;
    delete[] max_insert_element_at_time;
    delete[] min_insert_element_at_time;
    delete[] get_payload_superroot_call_cnt;
    delete[] get_payload_directp_call_cnt;
    delete[] get_payload_superroot_success_cnt;
    delete[] get_payload_directp_success_cnt;
    delete[] get_payload_superroot_fail_cnt;
    delete[] get_payload_directp_fail_cnt;
    delete[] insert_superroot_call_cnt;
    delete[] insert_directp_call_cnt;
    delete[] insert_superroot_success_cnt;
    delete[] insert_directp_success_cnt;
    delete[] insert_superroot_fail_cnt;
    delete[] insert_directp_fail_cnt;
    delete[] get_leaf_from_get_payload_superroot_call_cnt;
    delete[] get_leaf_from_get_payload_directp_call_cnt;
    delete[] get_leaf_from_insert_superroot_call_cnt;
    delete[] get_leaf_from_insert_directp_call_cnt;
    delete[] insert_using_shifts_call_cnt;
    delete[] insert_element_at_call_cnt;
    delete[] find_key_call_cnt;
    delete[] find_insert_position_call_cnt;
  }

  //prints max, min, average time + call count
  void printProfileStats() {
    //container for saving total stats
    fgStatType get_payload_from_superroot_success_time_cumu = 0,
               max_get_payload_from_superroot_success_time_cumu = std::numeric_limits<fgStatType>::min(),
               min_get_payload_from_superroot_success_time_cumu = std::numeric_limits<fgStatType>::max();
    fgStatType get_payload_from_parent_success_time_cumu = 0,
               max_get_payload_from_parent_success_time_cumu = std::numeric_limits<fgStatType>::min(),
               min_get_payload_from_parent_success_time_cumu = std::numeric_limits<fgStatType>::max();
    fgStatType get_payload_from_superroot_fail_time_cumu = 0,
               max_get_payload_from_superroot_fail_time_cumu = std::numeric_limits<fgStatType>::min(),
               min_get_payload_from_superroot_fail_time_cumu = std::numeric_limits<fgStatType>::max();
    fgStatType get_payload_from_parent_fail_time_cumu = 0,
               max_get_payload_from_parent_fail_time_cumu = std::numeric_limits<fgStatType>::min(),
               min_get_payload_from_parent_fail_time_cumu = std::numeric_limits<fgStatType>::max();
    fgStatType insert_from_superroot_success_time_cumu = 0,
               max_insert_from_superroot_success_time_cumu = std::numeric_limits<fgStatType>::min(),
               min_insert_from_superroot_success_time_cumu = std::numeric_limits<fgStatType>::max();
    fgStatType insert_from_parent_success_time_cumu = 0,
               max_insert_from_parent_success_time_cumu = std::numeric_limits<fgStatType>::min(),
               min_insert_from_parent_success_time_cumu = std::numeric_limits<fgStatType>::max();
    fgStatType insert_from_superroot_fail_time_cumu = 0,
               max_insert_from_superroot_fail_time_cumu = std::numeric_limits<fgStatType>::min(),
               min_insert_from_superroot_fail_time_cumu = std::numeric_limits<fgStatType>::max();
    fgStatType insert_from_parent_fail_time_cumu = 0,
               max_insert_from_parent_fail_time_cumu = std::numeric_limits<fgStatType>::min(),
               min_insert_from_parent_fail_time_cumu = std::numeric_limits<fgStatType>::max();
    fgStatType get_leaf_from_get_payload_superroot_time_cumu = 0,
               max_get_leaf_from_get_payload_superroot_time_cumu = std::numeric_limits<fgStatType>::min(),
               min_get_leaf_from_get_payload_superroot_time_cumu = std::numeric_limits<fgStatType>::max();
    fgStatType get_leaf_from_get_payload_directp_time_cumu = 0,
               max_get_leaf_from_get_payload_directp_time_cumu = std::numeric_limits<fgStatType>::min(),
               min_get_leaf_from_get_payload_directp_time_cumu = std::numeric_limits<fgStatType>::max();
    fgStatType get_leaf_from_insert_superroot_time_cumu = 0,
               max_get_leaf_from_insert_superroot_time_cumu = std::numeric_limits<fgStatType>::min(),
               min_get_leaf_from_insert_superroot_time_cumu = std::numeric_limits<fgStatType>::max();
    fgStatType get_leaf_from_insert_directp_time_cumu = 0,
               max_get_leaf_from_insert_directp_time_cumu = std::numeric_limits<fgStatType>::min(),
               min_get_leaf_from_insert_directp_time_cumu = std::numeric_limits<fgStatType>::max();
    fgStatType find_key_time_cumu = 0,
               max_find_key_time_cumu = std::numeric_limits<fgStatType>::min(),
               min_find_key_time_cumu = std::numeric_limits<fgStatType>::max();
    fgStatType insert_using_shifts_time_cumu = 0,
               max_insert_using_shifts_time_cumu = std::numeric_limits<fgStatType>::min(),
               min_insert_using_shifts_time_cumu = std::numeric_limits<fgStatType>::max();
    fgStatType insert_element_at_time_cumu = 0,
               max_insert_element_at_time_cumu = std::numeric_limits<fgStatType>::min(),
               min_insert_element_at_time_cumu = std::numeric_limits<fgStatType>::max();
    fgStatType find_insert_position_time_cumu = 0,
               max_find_insert_position_time_cumu = std::numeric_limits<fgStatType>::min(),
               min_find_insert_position_time_cumu = std::numeric_limits<fgStatType>::max();
    
    uint64_t get_payload_superroot_call_cnt_total = 0;
    uint64_t get_payload_directp_call_cnt_total = 0;
    uint64_t get_payload_superroot_success_cnt_total = 0;
    uint64_t get_payload_directp_success_cnt_total = 0;
    uint64_t get_payload_superroot_fail_cnt_total = 0;
    uint64_t get_payload_directp_fail_cnt_total = 0;
    uint64_t insert_superroot_call_cnt_total = 0;
    uint64_t insert_directp_call_cnt_total = 0;
    uint64_t insert_superroot_success_cnt_total = 0;
    uint64_t insert_directp_success_cnt_total = 0;
    uint64_t insert_superroot_fail_cnt_total = 0;
    uint64_t insert_directp_fail_cnt_total = 0;
    uint64_t get_leaf_from_get_payload_superroot_call_cnt_total = 0;
    uint64_t get_leaf_from_get_payload_directp_call_cnt_total = 0;
    uint64_t get_leaf_from_insert_superroot_call_cnt_total = 0;
    uint64_t get_leaf_from_insert_directp_call_cnt_total = 0;
    uint64_t insert_using_shifts_call_cnt_total = 0;
    uint64_t insert_element_at_call_cnt_total = 0;
    uint64_t find_key_call_cnt_total = 0;
    uint64_t find_insert_position_call_cnt_total = 0;

    std::cout << "current batch's profile result is\n\n";

    for (uint32_t i = 0; i < td_num; ++i) {
      //computation
      get_payload_from_superroot_success_time_cumu += get_payload_superroot_success_cnt[i] ? 
          get_payload_from_superroot_success_time[i] / get_payload_superroot_success_cnt[i] : 0;
      max_get_payload_from_superroot_success_time_cumu = std::max(max_get_payload_from_superroot_success_time_cumu, max_get_payload_from_superroot_success_time[i]);
      min_get_payload_from_superroot_success_time_cumu = std::min(min_get_payload_from_superroot_success_time_cumu, min_get_payload_from_superroot_success_time[i]);

      get_payload_from_parent_success_time_cumu += get_payload_directp_success_cnt[i] ?
          get_payload_from_parent_success_time[i] / get_payload_directp_success_cnt[i] : 0;
      max_get_payload_from_parent_success_time_cumu = std::max(max_get_payload_from_parent_success_time_cumu, max_get_payload_from_parent_success_time[i]);
      min_get_payload_from_parent_success_time_cumu = std::min(min_get_payload_from_parent_success_time_cumu, min_insert_from_parent_success_time[i]);

      get_payload_from_superroot_fail_time_cumu += get_payload_superroot_fail_cnt[i] ?
          get_payload_from_superroot_fail_time[i] / get_payload_superroot_fail_cnt[i] : 0;
      max_get_payload_from_superroot_fail_time_cumu = std::max(max_get_payload_from_superroot_fail_time[i], max_get_payload_from_superroot_fail_time_cumu);
      min_get_payload_from_superroot_fail_time_cumu = std::min(min_get_payload_from_superroot_fail_time[i], min_get_payload_from_superroot_fail_time_cumu);

      get_payload_from_parent_fail_time_cumu += get_payload_directp_fail_cnt[i] ?
          get_payload_from_parent_fail_time[i] / get_payload_directp_fail_cnt[i] : 0;
      max_get_payload_from_parent_fail_time_cumu = std::max(max_get_payload_from_parent_fail_time[i], max_get_payload_from_parent_fail_time_cumu);
      min_get_payload_from_parent_fail_time_cumu = std::min(min_get_payload_from_parent_fail_time[i], min_get_payload_from_parent_fail_time_cumu);

      insert_from_superroot_success_time_cumu += insert_superroot_success_cnt[i] ? 
          insert_from_superroot_success_time[i] / insert_superroot_success_cnt[i] : 0;
      max_insert_from_superroot_success_time_cumu = std::max(max_insert_from_superroot_success_time_cumu, max_insert_from_superroot_success_time[i]);
      min_insert_from_superroot_success_time_cumu = std::min(min_insert_from_superroot_success_time_cumu, min_insert_from_superroot_success_time[i]);

      insert_from_parent_success_time_cumu += insert_directp_success_cnt[i] ?
          insert_from_parent_success_time[i] / insert_directp_success_cnt[i] : 0;
      max_insert_from_parent_success_time_cumu = std::max(max_insert_from_parent_success_time_cumu, max_insert_from_parent_success_time[i]);
      min_insert_from_parent_success_time_cumu = std::min(min_insert_from_parent_success_time_cumu, min_insert_from_parent_success_time[i]);

      insert_from_superroot_fail_time_cumu += insert_superroot_fail_cnt[i] ?
          insert_from_superroot_fail_time[i] / insert_superroot_fail_cnt[i] : 0;
      max_insert_from_superroot_fail_time_cumu = std::max(max_insert_from_superroot_fail_time[i], max_insert_from_superroot_fail_time_cumu);
      min_insert_from_superroot_fail_time_cumu = std::min(min_insert_from_superroot_fail_time[i], min_insert_from_superroot_fail_time_cumu);

      insert_from_parent_fail_time_cumu += insert_directp_fail_cnt[i] ?
          insert_from_parent_fail_time[i] / insert_directp_fail_cnt[i] : 0;
      max_insert_from_parent_fail_time_cumu = std::max(max_insert_from_parent_fail_time[i], max_insert_from_parent_fail_time_cumu);
      min_insert_from_parent_fail_time_cumu = std::min(min_insert_from_parent_fail_time[i], min_insert_from_parent_fail_time_cumu);

      get_leaf_from_get_payload_superroot_time_cumu += get_leaf_from_get_payload_superroot_call_cnt[i] ?
          get_leaf_from_get_payload_superroot_time[i] / get_leaf_from_get_payload_superroot_call_cnt[i] : 0;
      max_get_leaf_from_get_payload_superroot_time_cumu = std::max(max_get_leaf_from_get_payload_superroot_time[i], max_get_leaf_from_get_payload_superroot_time_cumu);
      min_get_leaf_from_get_payload_superroot_time_cumu = std::min(min_get_leaf_from_get_payload_superroot_time[i], min_get_leaf_from_get_payload_superroot_time_cumu);

      get_leaf_from_get_payload_directp_time_cumu += get_leaf_from_get_payload_directp_call_cnt[i] ?
          get_leaf_from_get_payload_directp_time[i] / get_leaf_from_get_payload_directp_call_cnt[i] : 0;
      max_get_leaf_from_get_payload_directp_time_cumu = std::max(max_get_leaf_from_get_payload_directp_time[i], max_get_leaf_from_get_payload_directp_time_cumu);
      min_get_leaf_from_get_payload_directp_time_cumu = std::min(min_get_leaf_from_get_payload_directp_time[i], min_get_leaf_from_get_payload_directp_time_cumu);

      get_leaf_from_insert_superroot_time_cumu += get_leaf_from_insert_superroot_call_cnt[i] ?
          get_leaf_from_insert_superroot_time[i] / get_leaf_from_insert_superroot_call_cnt[i] : 0;
      max_get_leaf_from_insert_superroot_time_cumu = std::max(max_get_leaf_from_insert_superroot_time[i], max_get_leaf_from_insert_superroot_time_cumu);
      min_get_leaf_from_insert_superroot_time_cumu = std::min(min_get_leaf_from_insert_superroot_time[i], min_get_leaf_from_insert_superroot_time_cumu);

      get_leaf_from_insert_directp_time_cumu += get_leaf_from_insert_directp_call_cnt[i] ?
          get_leaf_from_insert_directp_time[i] / get_leaf_from_insert_directp_call_cnt[i] : 0;
      max_get_leaf_from_insert_directp_time_cumu = std::max(max_get_leaf_from_insert_directp_time[i], max_get_leaf_from_insert_directp_time_cumu);
      min_get_leaf_from_insert_directp_time_cumu = std::min(min_get_leaf_from_insert_directp_time[i], min_get_leaf_from_insert_directp_time_cumu);

      find_key_time_cumu += find_key_call_cnt[i] ?
          find_key_time[i] / find_key_call_cnt[i] : 0;
      max_find_key_time_cumu = std::max(max_find_key_time[i], max_find_key_time_cumu);
      min_find_key_time_cumu = std::min(min_find_key_time[i], min_find_key_time_cumu);

      insert_using_shifts_time_cumu += insert_using_shifts_call_cnt[i] ?
         insert_using_shifts_time[i] / insert_using_shifts_call_cnt[i] : 0;
      max_insert_using_shifts_time_cumu = std::max(max_insert_using_shifts_time[i], max_insert_using_shifts_time_cumu);
      min_insert_using_shifts_time_cumu = std::min(min_insert_using_shifts_time[i], min_insert_using_shifts_time_cumu);

      insert_element_at_time_cumu += insert_element_at_call_cnt[i] ?
          insert_element_at_time[i] / insert_element_at_call_cnt[i] : 0;
      max_insert_element_at_time_cumu = std::max(max_insert_element_at_time[i], max_insert_element_at_time_cumu);
      min_insert_element_at_time_cumu = std::min(min_insert_element_at_time[i], min_insert_element_at_time_cumu);
      
      find_insert_position_time_cumu += find_insert_position_call_cnt[i] ?
          find_insert_position_time[i] / find_insert_position_call_cnt[i] : 0;
      max_find_insert_position_time_cumu = std::max(max_find_insert_position_time[i], max_find_insert_position_time_cumu);
      min_find_insert_position_time_cumu = std::min(min_find_insert_position_time[i], min_find_insert_position_time_cumu);

      get_payload_superroot_call_cnt_total += get_payload_superroot_call_cnt[i];
      get_payload_directp_call_cnt_total += get_payload_directp_call_cnt[i];
      get_payload_superroot_success_cnt_total += get_payload_superroot_success_cnt[i];
      get_payload_directp_success_cnt_total += get_payload_directp_success_cnt[i];
      get_payload_superroot_fail_cnt_total += get_payload_superroot_fail_cnt[i];
      get_payload_directp_fail_cnt_total += get_payload_directp_fail_cnt[i];
      insert_superroot_call_cnt_total += insert_superroot_call_cnt[i];
      insert_directp_call_cnt_total += insert_directp_call_cnt[i];
      insert_superroot_success_cnt_total += insert_superroot_success_cnt[i];
      insert_directp_success_cnt_total += insert_directp_success_cnt[i];
      insert_superroot_fail_cnt_total += insert_superroot_fail_cnt[i];
      insert_directp_fail_cnt_total += insert_directp_fail_cnt[i];
      get_leaf_from_get_payload_superroot_call_cnt_total += get_leaf_from_get_payload_superroot_call_cnt[i];
      get_leaf_from_get_payload_directp_call_cnt_total += get_leaf_from_get_payload_directp_call_cnt[i];
      get_leaf_from_insert_superroot_call_cnt_total += get_leaf_from_insert_superroot_call_cnt[i];
      get_leaf_from_insert_directp_call_cnt_total += get_leaf_from_insert_directp_call_cnt[i];
      insert_using_shifts_call_cnt_total += insert_using_shifts_call_cnt[i];
      insert_element_at_call_cnt_total += insert_element_at_call_cnt[i];
      find_key_call_cnt_total += find_key_call_cnt[i];
      find_insert_position_call_cnt_total += find_insert_position_call_cnt[i];

      //print
      std::cout << "-thread " << i << " -\n\n";

      std::cout << "-countings-\n"
              << "get_payload_superroot_call_cnt : " << get_payload_superroot_call_cnt[i] << '\n'
              << "get_payload_directp_call_cnt : " << get_payload_directp_call_cnt[i] << '\n'
              << "get_payload_superroot_success_cnt : " << get_payload_superroot_success_cnt[i] << '\n'
              << "get_payload_directp_success_cnt : " << get_payload_directp_success_cnt[i] << '\n'
              << "get_payload_superroot_fail_cnt : " << get_payload_superroot_fail_cnt[i] << '\n'
              << "get_payload_directp_fail_cnt : " << get_payload_directp_fail_cnt[i] << '\n'
              << "insert_superroot_call_cnt : " << insert_superroot_call_cnt[i] << '\n'
              << "insert_directp_call_cnt : " << insert_directp_call_cnt[i] << '\n'
              << "insert_superroot_success_cnt : " << insert_superroot_success_cnt[i] << '\n'
              << "insert_directp_success_cnt : " << insert_directp_success_cnt[i] << '\n'
              << "insert_superroot_fail_cnt : " << insert_superroot_fail_cnt[i] << '\n'
              << "insert_directp_fail_cnt : " << insert_directp_fail_cnt[i] << '\n'
              << "get_leaf_from_get_payload_superroot_call_cnt : " << get_leaf_from_get_payload_superroot_call_cnt[i] << '\n'
              << "get_leaf_from_get_payload_directp_call_cnt : " << get_leaf_from_get_payload_directp_call_cnt[i] << '\n'
              << "get_leaf_from_insert_superroot_call_cnt : " << get_leaf_from_insert_superroot_call_cnt[i] << '\n'
              << "get_leaf_from_insert_directp_call_cnt : " << get_leaf_from_insert_directp_call_cnt[i] << '\n'
              << "insert_using_shifts_call_cnt : " << insert_using_shifts_call_cnt[i] << '\n'
              << "insert_element_at_call_cnt : " << insert_element_at_call_cnt[i] << '\n'
              << "find_key_call_cnt : " << find_key_call_cnt[i] << '\n'
              << "find_insert_position_call_cnt : " << find_insert_position_call_cnt[i] << "\n\n";

      std::cout << "-average time-\n"
                << "get_payload_from_superroot_success_time : " << (get_payload_superroot_success_cnt[i] ? 
                    get_payload_from_superroot_success_time[i] / get_payload_superroot_success_cnt[i] : 0) << '\n'
                << "get_payload_from_parent_success_time : " << (get_payload_directp_success_cnt[i] ? 
                    get_payload_from_parent_success_time[i] / get_payload_directp_success_cnt[i] : 0) << '\n'
                << "get_payload_from_superroot_fail_time : " << (get_payload_superroot_fail_cnt[i] ? 
                    get_payload_from_superroot_fail_time[i] / get_payload_superroot_fail_cnt[i] : 0) << '\n'
                << "get_payload_from_parent_fail_time : " << (get_payload_directp_fail_cnt[i] ?
                    get_payload_from_parent_fail_time[i] / get_payload_directp_fail_cnt[i] : 0) << '\n'
                << "insert_from_superroot_success_time : " << (insert_superroot_success_cnt[i] ? 
                    insert_from_superroot_success_time[i] / insert_superroot_success_cnt[i] : 0) << '\n'
                << "insert_from_parent_success_time : " << (insert_directp_success_cnt[i] ?
                    insert_from_parent_success_time[i] / insert_directp_success_cnt[i] : 0) << '\n'
                << "insert_from_superroot_fail_time : " << (insert_superroot_fail_cnt[i] ? 
                    insert_from_superroot_fail_time[i] / insert_superroot_fail_cnt[i] : 0) << '\n'
                << "insert_from_parent_fail_time : " << (insert_directp_fail_cnt[i] ? 
                    insert_from_parent_fail_time[i] / insert_directp_fail_cnt[i] : 0) << '\n'
                << "get_leaf_from_get_payload_superroot_time : " << (get_leaf_from_get_payload_superroot_call_cnt[i] ?
                    get_leaf_from_get_payload_superroot_time[i] / get_leaf_from_get_payload_superroot_call_cnt[i] : 0) << '\n'
                << "get_leaf_from_get_payload_directp_time : " << (get_leaf_from_get_payload_directp_call_cnt[i] ?
                    get_leaf_from_get_payload_directp_time[i] / get_leaf_from_get_payload_directp_call_cnt[i] : 0) << '\n'
                << "get_leaf_from_insert_superroot_time : " << (get_leaf_from_insert_superroot_call_cnt[i] ?
                    get_leaf_from_insert_superroot_time[i] / get_leaf_from_insert_superroot_call_cnt[i] : 0) << '\n'
                << "get_leaf_from_insert_directp_time : " << (get_leaf_from_insert_directp_call_cnt[i] ?
                    get_leaf_from_insert_directp_time[i] / get_leaf_from_insert_directp_call_cnt[i] : 0) << '\n'
                << "find_key_time : " << (find_key_call_cnt[i] ? find_key_time[i] / find_key_call_cnt[i] : 0) << '\n'
                << "insert_using_shifts_time : " << (insert_using_shifts_call_cnt[i] ?
                    insert_using_shifts_time[i] / insert_using_shifts_call_cnt[i] : 0) << '\n'
                << "insert_element_at_time : " << (insert_element_at_call_cnt[i] ? 
                    insert_element_at_time[i] / insert_element_at_call_cnt[i] : 0) << '\n'
                << "find_insert_position_time : " << (find_insert_position_call_cnt[i] ? 
                    find_insert_position_time[i] / find_insert_position_call_cnt[i] : 0) << "\n\n";

      std::cout << "-max time-\n"
                << "max_get_payload_from_superroot_success_time : " << max_get_payload_from_superroot_success_time[i] << '\n'
                << "max_get_payload_from_parent_success_time : " << max_get_payload_from_parent_success_time[i] << '\n'
                << "max_get_payload_from_superroot_fail_time : " << max_get_payload_from_superroot_fail_time[i] << '\n'
                << "max_get_payload_from_parent_fail_time : " << max_get_payload_from_parent_fail_time[i] << '\n'
                << "max_insert_from_superroot_success_time : " << max_insert_from_superroot_success_time[i] << '\n'
                << "max_insert_from_parent_success_time : " << max_insert_from_parent_success_time[i] << '\n'
                << "max_insert_from_superroot_fail_time : " << max_insert_from_superroot_fail_time[i] << '\n'
                << "max_insert_from_parent_fail_time : " << max_insert_from_parent_fail_time[i] << '\n'
                << "max_get_leaf_from_get_payload_superroot_time : " << max_get_leaf_from_get_payload_superroot_time[i] << '\n'
                << "max_get_leaf_from_get_payload_directp_time : " << max_get_leaf_from_get_payload_directp_time[i] << '\n'
                << "max_get_leaf_from_insert_superroot_time : " << max_get_leaf_from_insert_superroot_time[i] << '\n'
                << "max_get_leaf_from_insert_directp_time : " << max_get_leaf_from_insert_directp_time[i] << '\n'
                << "max_find_key_time : " << max_find_key_time[i] << '\n'
                << "max_insert_using_shifts_time : " << max_insert_using_shifts_time[i] << '\n'
                << "max_insert_element_at_time : " << max_insert_element_at_time[i] << '\n'
                << "max_find_insert_position_time : " << max_find_insert_position_time[i] << "\n\n";

      std::cout << "-min time-\n"
                << "min_get_payload_from_superroot_success_time : " << min_get_payload_from_superroot_success_time[i] << '\n'
                << "min_get_payload_from_parent_success_time : " << min_get_payload_from_parent_success_time[i] << '\n'
                << "min_get_payload_from_superroot_fail_time : " << min_get_payload_from_superroot_fail_time[i] << '\n'
                << "min_get_payload_from_parent_fail_time : " << min_get_payload_from_parent_fail_time[i] << '\n'
                << "min_insert_from_superroot_success_time : " << min_insert_from_superroot_success_time[i] << '\n'
                << "min_insert_from_parent_success_time : " << min_insert_from_parent_success_time[i] << '\n'
                << "min_insert_from_superroot_fail_time : " << min_insert_from_superroot_fail_time[i] << '\n'
                << "min_insert_from_parent_fail_time : " << min_insert_from_parent_fail_time[i] << '\n'
                << "min_get_leaf_from_get_payload_superroot_time : " << min_get_leaf_from_get_payload_superroot_time[i] << '\n'
                << "min_get_leaf_from_get_payload_directp_time : " << min_get_leaf_from_get_payload_directp_time[i] << '\n'
                << "min_get_leaf_from_insert_superroot_time : " << min_get_leaf_from_insert_superroot_time[i] << '\n'
                << "min_get_leaf_from_insert_directp_time : " << min_get_leaf_from_insert_directp_time[i] << '\n'
                << "min_find_key_time : " << min_find_key_time[i] << '\n'
                << "min_insert_using_shifts_time : " << min_insert_using_shifts_time[i] << '\n'
                << "min_insert_element_at_time : " << min_insert_element_at_time[i] << '\n'
                << "min_find_insert_position_time : " << min_find_insert_position_time[i] << "\n\n";
    }

    std::cout << "-foreground total-\n\n";
    std::cout << "-countings-\n"
              << "get_payload_superroot_call_cnt_total : " << get_payload_superroot_call_cnt_total << '\n'
              << "get_payload_directp_call_cnt_total : " << get_payload_directp_call_cnt_total << '\n'
              << "get_payload_superroot_success_cnt_total : " << get_payload_superroot_success_cnt_total << '\n'
              << "get_payload_directp_success_cnt_total : " << get_payload_directp_success_cnt_total << '\n'
              << "get_payload_superroot_fail_cnt_total : " << get_payload_superroot_fail_cnt_total << '\n'
              << "get_payload_directp_fail_cnt_total : " << get_payload_directp_fail_cnt_total << '\n'
              << "insert_superroot_call_cnt_total : " << insert_superroot_call_cnt_total << '\n'
              << "insert_directp_call_cnt_total : " << insert_directp_call_cnt_total << '\n'
              << "insert_superroot_success_cnt_total : " << insert_superroot_success_cnt_total << '\n'
              << "insert_directp_success_cnt_total : " << insert_directp_success_cnt_total << '\n'
              << "insert_superroot_fail_cnt_total : " << insert_superroot_fail_cnt_total << '\n'
              << "insert_directp_fail_cnt_total : " << insert_directp_fail_cnt_total << '\n'
              << "get_leaf_from_get_payload_superroot_call_cnt_total : " << get_leaf_from_get_payload_superroot_call_cnt_total << '\n'
              << "get_leaf_from_get_payload_directp_call_cnt_total : " << get_leaf_from_get_payload_directp_call_cnt_total << '\n'
              << "get_leaf_from_insert_superroot_call_cnt_total : " << get_leaf_from_insert_superroot_call_cnt_total << '\n'
              << "get_leaf_from_insert_directp_call_cnt_total : " << get_leaf_from_insert_directp_call_cnt_total << '\n'
              << "insert_using_shifts_call_cnt_total : " << insert_using_shifts_call_cnt_total << '\n'
              << "insert_element_at_call_cnt_total : " << insert_element_at_call_cnt_total << '\n'
              << "find_key_call_cnt_total : " << find_key_call_cnt_total << '\n'
              << "find_insert_position_call_cnt_total : " << find_insert_position_call_cnt_total << "\n\n";

    std::cout << "-average time-\n"
              << "get_payload_from_superroot_success_time : " << get_payload_from_superroot_success_time_cumu / td_num << '\n'
              << "get_payload_from_parent_success_time : " << get_payload_from_parent_success_time_cumu / td_num << '\n'
              << "get_payload_from_superroot_fail_time : " << get_payload_from_superroot_fail_time_cumu / td_num << '\n'
              << "get_payload_from_parent_fail_time : " << get_payload_from_parent_fail_time_cumu / td_num << '\n'
              << "insert_from_superroot_success_time : " << insert_from_superroot_success_time_cumu / td_num << '\n'
              << "insert_from_parent_success_time : " << insert_from_parent_success_time_cumu / td_num << '\n'
              << "insert_from_superroot_fail_time : " << insert_from_superroot_fail_time_cumu / td_num << '\n'
              << "insert_from_parent_fail_time : " << insert_from_parent_fail_time_cumu / td_num << '\n'
              << "get_leaf_from_get_payload_superroot_time : " << get_leaf_from_get_payload_superroot_time_cumu / td_num  << '\n'
              << "get_leaf_from_get_payload_directp_time : " << get_leaf_from_get_payload_directp_time_cumu / td_num  << '\n'
              << "get_leaf_from_insert_superroot_time : " << get_leaf_from_insert_superroot_time_cumu / td_num  << '\n'
              << "get_leaf_from_insert_directp_time : " << get_leaf_from_insert_directp_time_cumu / td_num  << '\n'
              << "find_key_time : " << find_key_time_cumu / td_num << '\n'
              << "insert_using_shifts_time : " << insert_using_shifts_time_cumu / td_num << '\n'
              << "insert_element_at_time : " << insert_element_at_time_cumu / td_num  << '\n'
              << "find_insert_position_time : " << find_insert_position_time_cumu / td_num << "\n\n";


    std::cout << "-max time-\n"
              << "max_get_payload_from_superroot_success_time : " << max_get_payload_from_superroot_success_time_cumu << '\n'
              << "max_get_payload_from_parent_success_time : " << max_get_payload_from_parent_success_time_cumu << '\n'
              << "max_get_payload_from_superroot_fail_time : " << max_get_payload_from_superroot_fail_time_cumu << '\n'
              << "max_get_payload_from_parent_fail_time : " << max_get_payload_from_parent_fail_time_cumu << '\n'
              << "max_insert_from_superroot_success_time : " << max_insert_from_superroot_success_time_cumu << '\n'
              << "max_insert_from_parent_success_time : " << max_insert_from_parent_success_time_cumu << '\n'
              << "max_insert_from_superroot_fail_time : " << max_insert_from_superroot_fail_time_cumu << '\n'
              << "max_insert_from_parent_fail_time : " << max_insert_from_parent_fail_time_cumu << '\n'
              << "get_leaf_from_get_payload_superroot_time_cumu : " << get_leaf_from_get_payload_superroot_time_cumu << '\n'
              << "get_leaf_from_get_payload_directp_time_cumu : " << get_leaf_from_get_payload_directp_time_cumu << '\n'
              << "max_get_leaf_from_insert_superroot_time : " << max_get_leaf_from_insert_superroot_time_cumu << '\n'
              << "max_get_leaf_from_insert_directp_time : " << max_get_leaf_from_insert_directp_time_cumu << '\n'
              << "max_find_key_time : " << max_find_key_time_cumu << '\n'
              << "max_insert_using_shifts_time : " << max_insert_using_shifts_time_cumu << '\n'
              << "max_insert_element_at_time : " << max_insert_element_at_time_cumu << '\n'
              << "max_find_insert_position_time : " << max_find_insert_position_time_cumu << "\n\n";


    std::cout << "-min time-\n"
              << "min_get_payload_from_superroot_success_time : " << min_get_payload_from_superroot_success_time_cumu << '\n'
              << "min_get_payload_from_parent_success_time : " << min_get_payload_from_parent_success_time_cumu << '\n'
              << "min_get_payload_from_superroot_fail_time : " << min_get_payload_from_superroot_fail_time_cumu << '\n'
              << "min_get_payload_from_parent_fail_time : " << min_get_payload_from_parent_fail_time_cumu << '\n'
              << "min_insert_from_superroot_success_time : " << min_insert_from_superroot_success_time_cumu << '\n'
              << "min_insert_from_parent_success_time : " << min_insert_from_parent_success_time_cumu << '\n'
              << "min_insert_from_superroot_fail_time : " << min_insert_from_superroot_fail_time_cumu << '\n'
              << "min_insert_from_parent_fail_time : " << min_insert_from_parent_fail_time_cumu << '\n'
              << "min_get_leaf_from_get_payload_superroot_time_cumu : " << min_get_leaf_from_get_payload_superroot_time_cumu << '\n'
              << "min_get_leaf_from_get_payload_directp_time_cumu : " << min_get_leaf_from_get_payload_directp_time_cumu << '\n'
              << "min_get_leaf_from_insert_superroot_time : " << min_get_leaf_from_insert_superroot_time_cumu << '\n'
              << "min_get_leaf_from_insert_directp_time : " << min_get_leaf_from_insert_directp_time_cumu << '\n'
              << "min_find_key_time : " << min_find_key_time_cumu << '\n'
              << "min_insert_using_shifts_time : " << min_insert_using_shifts_time_cumu << '\n'
              << "min_insert_element_at_time : " << min_insert_element_at_time_cumu << '\n'
              << "min_find_insert_position_time : " << min_find_insert_position_time_cumu << "\n\n";


    std::cout << "-background threads-\n\n";
    std::cout << "-countings-\n"
              << "resize_call_cnt : " << resize_call_cnt << '\n'
              << "find_best_fanout_existing_node_call_cnt : " << find_best_fanout_existing_node_call_cnt << '\n'
              << "fanout_model_train_cnt : " << fanout_model_train_cnt << '\n'
              << "fanout_data_train_cnt : " << fanout_data_train_cnt << '\n'
              << "fanout_batch_stat_cnt : " << fanout_batch_stat_cnt << '\n'
              << "split_downwards_call_cnt : " << split_downwards_call_cnt << '\n'
              << "split_sideways_call_cnt : " << split_sideways_call_cnt << "\n\n";
    
    std::cout << "-average-\n"
              << "resize_time : " << (resize_call_cnt ? resize_time / resize_call_cnt : 0) << '\n'
              << "find_best_fanout_existing_node_time : " << (find_best_fanout_existing_node_call_cnt ?
                  find_best_fanout_existing_node_time / find_best_fanout_existing_node_call_cnt : 0) << '\n'
              << "fanout_model_train_time : " << (fanout_model_train_cnt ? 
                  fanout_model_train_time / fanout_model_train_cnt : 0) << '\n'
              << "fanout_data_train_time : " << (fanout_data_train_cnt ?
                  fanout_data_train_time / fanout_data_train_cnt : 0) << '\n'
              << "fanout_batch_stat_time : " << (fanout_batch_stat_cnt ?
                  fanout_batch_stat_time / fanout_batch_stat_cnt : 0) << '\n'
              << "split_downwards_time : " << (split_downwards_call_cnt ?
                  split_downwards_time / split_downwards_call_cnt : 0) << '\n'
              << "split_sideways_time : " << (split_sideways_call_cnt ? 
                  split_sideways_time / split_sideways_call_cnt : 0) << "\n\n";

    std::cout << "-max time-\n"
              << "resize_time : " << max_resize_time << '\n'
              << "find_best_fanout_existing_node_time : " << max_find_best_fanout_existing_node_time << '\n'
              << "fanout_model_train_time : " << max_fanout_model_train_time << '\n'
              << "fanout_data_train_time : " << max_fanout_data_train_time << '\n'
              << "fanout_batch_stat_time : " << max_fanout_batch_stat_time << '\n'
              << "split_downwards_time : " << max_split_downwards_time << '\n'
              << "split_sideways_time : " << max_split_sideways_time << "\n\n";
    
    std::cout << "-min time-\n"
              << "resize_time : " << min_resize_time << '\n'
              << "find_best_fanout_existing_node_time : " << min_find_best_fanout_existing_node_time << '\n'
              << "fanout_model_train_time : " << min_fanout_model_train_time << '\n'
              << "fanout_data_train_time : " << min_fanout_data_train_time << '\n'
              << "fanout_batch_stat_time : " << min_fanout_batch_stat_time << '\n'
              << "split_downwards_time : " << min_split_downwards_time << '\n'
              << "split_sideways_time : " << min_split_sideways_time << "\n\n";

    std::cout << "-lock-\n"
              << "lock_achive_cnt : " << lock_achieve_cnt << '\n'
              << "lock_achive_time average : " << (lock_achieve_cnt ? lock_achieve_time / lock_achieve_cnt : 0) << "\n\n";

  }
};

ProfileStats profileStats;
#endif

/* utils for multithreading
 * Many of this code is copied from Xindex */

inline void memory_fence() { asm volatile("mfence" : : : "memory"); }

/** @brief Compiler fence.
 * Prevents reordering of loads and stores by the compiler. Not intended to
 * synchronize the processor's caches. */
inline void fence() { asm volatile("" : : : "memory"); }

inline uint64_t cmpxchg(uint64_t *object, uint64_t expected,
                               uint64_t desired) {
  asm volatile("lock; cmpxchgq %2,%1"
               : "+a"(expected), "+m"(*object)
               : "r"(desired)
               : "cc");
  fence();
  return expected;
}

inline uint8_t cmpxchgb(uint8_t *object, uint8_t expected,
                               uint8_t desired) {
  asm volatile("lock; cmpxchgb %2,%1"
               : "+a"(expected), "+m"(*object)
               : "r"(desired)
               : "cc");
  fence();
  return expected;
}

template <class val_t>
struct AtomicVal {
  val_t val_;

  // 60 bits for version
  static const uint64_t version_mask = 0x0fffffffffffffff;
  static const uint64_t lock_mask = 0x1000000000000000;

  // lock - removed - is_ptr
  volatile uint64_t status;

  AtomicVal() : status(0) {}
  AtomicVal(val_t val) : val_(val), status(0) {}

  bool locked(uint64_t status) { return status & lock_mask; }
  uint64_t get_version(uint64_t status) { return status & version_mask; }

  void lock() {
#if PROFILE
    profileStats.lock_achieve_cnt++;
    auto lock_start_time = std::chrono::high_resolution_clock::now();
#endif
    while (true) {
      uint64_t old = status;
      uint64_t expected = old & ~lock_mask;  // expect to be unlocked
      uint64_t desired = old | lock_mask;    // desire to lock
      if (likely(cmpxchg((uint64_t *)&this->status, expected, desired) ==
                 expected)) {
#if PROFILE
        auto lock_end_time = std::chrono::high_resolution_clock::now();
        auto elapsed_time = std::chrono::duration_cast<std::chrono::nanoseconds>(lock_end_time - lock_start_time).count();
        profileStats.lock_achieve_time += elapsed_time;
#endif
        return;
      }
    }
  }
  void unlock() { status &= ~lock_mask; }
  void incr_version() {
    uint64_t version = get_version(status);
    UNUSED(version);
    status++;
    assert(get_version(status) == version + 1);
  }

  friend std::ostream &operator<<(std::ostream &os, const AtomicVal &leaf) {
    COUT_VAR(leaf.val_);
    COUT_VAR(leaf.locked);
    COUT_VAR(leaf.version);
    return os;
  }

  // semantics: atomically read the value and the `removed` flag
  val_t read() {
    while (true) {
      uint64_t status = this->status;
      memory_fence();
      val_t curr_val = this->val_;
      memory_fence();
      uint64_t current_status = this->status;
      memory_fence();

      if (unlikely(locked(current_status))) {  // check lock
        continue;
      }

      if (likely(get_version(status) ==
                 get_version(current_status))) {  // check version
        return curr_val;
      }
    }
  }

  bool update(const val_t &val) {
    lock();
    bool res;
    this->val_ = val;
    res = true;
    memory_fence();
    incr_version();
    memory_fence();
    unlock();
    return res;
  }

  bool increment() {
    lock();
    bool res;
    this->val_++;
    res = true;
    memory_fence();
    incr_version();
    memory_fence();
    unlock();
    return res;
  }

  bool decrement() {
    lock();
    bool res;
    this->val_--;
    res = true;
    memory_fence();
    incr_version();
    memory_fence();
    unlock();
    return res;
  }

  bool add(val_t cnt) {
    lock();
    bool res;
    this->val_ += cnt;
    res = true;
    memory_fence();
    incr_version();
    memory_fence();
    unlock();
    return res;
  }

  bool subtract(val_t cnt) {
    lock();
    bool res;
    this->val_ -= cnt;
    res = true;
    memory_fence();
    incr_version();
    memory_fence();
    unlock();
    return res;
  }
};

struct myLock {

  static const uint64_t lock_mask = 0x1000000000000000;

  // lock - removed - is_ptr
  volatile uint64_t status;

  myLock() : status(0) {}

  void lock() {
    while (true) {
      uint64_t old = status;
      uint64_t expected = old & ~lock_mask;  // expect to be unlocked
      uint64_t desired = old | lock_mask;    // desire to lock
      if (likely(cmpxchg((uint64_t *)&this->status, expected, desired) ==
                 expected)) {
        return;
      }
    }
  }
  void unlock() { status &= ~lock_mask; }
};

myLock coutLock;

struct RCUStatus {
  std::atomic<uint64_t> status;
  std::atomic<bool> waiting;
};

enum class Result { ok, failed, retry };

struct IndexConfig {
  double root_error_bound = 32;
  double root_memory_constraint = 1024 * 1024;
  double group_error_bound = 32;
  double group_error_tolerance = 4;
  size_t buffer_size_bound = 256;
  double buffer_size_tolerance = 3;
  size_t buffer_compact_threshold = 8;
  size_t worker_n = 0;
  std::unique_ptr<rcu_status_t[]> rcu_status;
  volatile bool exited = false;
};

index_config_t config;
std::mutex config_mutex;

// TODO replace it with user space RCU (e.g., qsbr)
void rcu_init() {
  config_mutex.lock();
  if (config.rcu_status.get() == nullptr) {abort();}
  for (size_t worker_i = 0; worker_i < config.worker_n; worker_i++) {
    config.rcu_status[worker_i].status = 0;
    config.rcu_status[worker_i].waiting = false;
  }
  config_mutex.unlock();
}

void rcu_alloc() {
  config_mutex.lock();
  if (config.rcu_status.get() == nullptr) {
    config.rcu_status = std::make_unique<rcu_status_t[]>(config.worker_n);
  }
  config_mutex.unlock();
}

void rcu_progress(const uint64_t worker_id) {
  config.rcu_status[worker_id].status++;
}

// wait for all workers whose 'waiting' is false
void rcu_barrier() {
  uint64_t prev_status[config.worker_n];
  for (size_t w_i = 0; w_i < config.worker_n; w_i++) {
    prev_status[w_i] = config.rcu_status[w_i].status;
  }
  for (size_t w_i = 0; w_i < config.worker_n; w_i++) {
    while (!config.rcu_status[w_i].waiting
           && config.rcu_status[w_i].status <= prev_status[w_i]
           && !config.exited)
      ;
  }
}

// wait for workers whose 'waiting' is false
void rcu_barrier(const uint64_t worker_id) {
  // set myself to waiting for barrier
#if DEBUG_PRINT
  coutLock.lock();
  std::cout << "t" << worker_id << " - waiting started in rcu_barrier" << std::endl;
  coutLock.unlock();
#endif
  config.rcu_status[worker_id].waiting = true;

  uint64_t prev_status[config.worker_n];
  for (size_t w_i = 0; w_i < config.worker_n; w_i++) {
    prev_status[w_i] = config.rcu_status[w_i].status;
  }
  for (size_t w_i = 0; w_i < config.worker_n; w_i++) {
    // skipped workers that is wating for barrier (include myself)
    while ( !config.rcu_status[w_i].waiting &&
            config.rcu_status[w_i].status <= prev_status[w_i] &&
            !config.exited)
      ;
  }
  config.rcu_status[worker_id].waiting = false;  // restore my state
#if DEBUG_PRINT
  coutLock.lock();
  std::cout << "t" << worker_id << " - waiting finished in rcu_barrier" << std::endl;
  coutLock.unlock();
#endif
}

/* for background threads */
std::mutex cvm;
std::condition_variable cv;
std::queue<std::pair<void *, int>> pending_modification_jobs_;

}