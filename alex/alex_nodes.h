// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

/*
 * This file contains code for ALEX nodes. There are two types of nodes in ALEX:
 * - Model nodes (equivalent to internal/inner nodes of a B+ Tree)
 * - Data nodes, sometimes referred to as leaf nodes (equivalent to leaf nodes
 * of a B+ Tree)
 */

#pragma once

#include "alex_base.h"
#include "../lock.h"
#include <map>

#define ALEX_DATA_NODE_KEY_AT(i) key_slots_[i]
#define ALEX_DATA_NODE_PAYLOAD_AT(i) payload_slots_[i]

// Whether we use lzcnt and tzcnt when manipulating a bitmap (e.g., when finding
// the closest gap).
// If your hardware does not support lzcnt/tzcnt (e.g., your Intel CPU is
// pre-Haswell), set this to 0.
#define ALEX_USE_LZCNT 1

namespace alex {

//forward declaration.
template <class T, class P, class Alloc> class AlexModelNode;
template <class T, class P> struct TraversalNode;

// A parent class for both types of ALEX nodes
template <class T, class P, class Alloc = std::allocator<std::pair<AlexKey<T>, P>>>
class AlexNode {
 public:

  typedef AlexNode<T, P, Alloc> self_type;
  typedef AlexModelNode<T, P, Alloc> model_node_type;

  // Whether this node is a leaf (data) node
  bool is_leaf_ = false;

  // Power of 2 to which the pointer to this node is duplicated in its parent
  // model node
  // For example, if duplication_factor_ is 3, then there are 8 redundant
  // pointers to this node in its parent
  uint8_t duplication_factor_ = 0;

  // Node's level in the RMI. Root node is level 0
  short level_ = 0;

  // Both model nodes and data nodes nodes use models
  LinearModel<T> model_;

  // Could be either the expected or empirical cost, depending on how this field
  // is used
  double cost_ = 0.0;

  //parent of current node. Root is nullptr. Need to be given by parameter.
  model_node_type *parent_ = nullptr;

  // pivot key of node. Never updates
  AlexKey<T> pivot_key_;

  AlexNode() = default;
  explicit AlexNode(short level) : level_(level) {
    pivot_key_.key_arr_ = new T[max_key_length_];
    std::fill(pivot_key_.key_arr_, pivot_key_.key_arr_ + max_key_length_,
        STR_VAL_MAX);
  }
  AlexNode(short level, bool is_leaf) : is_leaf_(is_leaf), level_(level) {
    pivot_key_.key_arr_ = new T[max_key_length_];
    std::fill(pivot_key_.key_arr_, pivot_key_.key_arr_ + max_key_length_,
        STR_VAL_MAX);
  }
  AlexNode(short level, bool is_leaf, model_node_type *parent)
      : is_leaf_(is_leaf), level_(level), parent_(parent) {
    pivot_key_.key_arr_ = new T[max_key_length_];
    std::fill(pivot_key_.key_arr_, pivot_key_.key_arr_ + max_key_length_,
        STR_VAL_MAX);
  }

  AlexNode(self_type& other)
      : is_leaf_(other.is_leaf_),
        duplication_factor_(other.duplication_factor_),
        level_(other.level_),
        model_(other.model_),
        cost_(other.cost_),
        parent_(other.parent_) {
    pivot_key_ = other.pivot_key_;
  }
  virtual ~AlexNode() = default;

  // The size in bytes of all member variables in this class
  virtual long long node_size() const = 0;
};

template <class T, class P, class Alloc = std::allocator<std::pair<AlexKey<T>, P>>>
class AlexModelNode : public AlexNode<T, P, Alloc> {
 public:
  AlexNode<T, P, Alloc>**children_ = nullptr;
  pthread_rwlock_t children_rw_lock_ = PTHREAD_RWLOCK_INITIALIZER; 
  int num_children_ = 0;

  typedef AlexNode<T, P, Alloc> basic_node_type;
  typedef AlexModelNode<T, P, Alloc> self_type;
  typedef typename Alloc::template rebind<self_type>::other alloc_type;
  typedef typename Alloc::template rebind<basic_node_type*>::other
      pointer_alloc_type;

  const Alloc& allocator_;

  std::map<uint32_t, basic_node_type**> old_childrens_;
  myLock old_childrens_lock;

  explicit AlexModelNode(const Alloc& alloc = Alloc())
      : AlexNode<T, P, Alloc>(0, false), allocator_(alloc) {
      }

  explicit AlexModelNode(short level, const Alloc& alloc = Alloc())
      : AlexNode<T, P, Alloc>(level, false), allocator_(alloc) {
      }

  explicit AlexModelNode(short level, self_type *parent,
                         const Alloc& alloc = Alloc())
      : AlexNode<T, P, Alloc>(level, false, parent), allocator_(alloc){
      }

  ~AlexModelNode() {
    delete children_;
    pthread_rwlock_destroy(&children_rw_lock_);
  }

  AlexModelNode(const self_type& other)
      : AlexNode<T, P, Alloc>(other) {
    children_ = new basic_node_type *[other.num_children_];
    // std::copy(other.children_.val_, 
    //           other.children_.val_ + other.num_children_,
    //           children_.val_);
  }

  self_type& operator=(const self_type& other) {
    this->is_leaf_ = other.is_leaf_;
    this->duplication_factor_ = other.duplication_factor_;
    this->level_ = other.level_;
    this->model_ = other.model_;
    this->cost_ = other.cost_;
    this->parent_ = other.parent_;

    allocator_ = other.allocator_;

    delete children_;
    children_ = new basic_node_type*[other.num_children_];
    std::copy(other.children_, 
              other.children_ + other.num_children_, 
              children_);
    num_children_ = other.num_children_;
  }

  pointer_alloc_type pointer_allocator() {
    return pointer_alloc_type(allocator_);
  }

  long long node_size() const override {
    long long size = sizeof(self_type);
    size += num_children_ * sizeof(AlexNode<T, P, Alloc>*);  // pointers to children
    return size;
  }
};

/*
* Functions are organized into different sections:
* - Constructors and destructors
* - General helper functions
* - Iterator
* - Cost model
* - Bulk loading and model building (e.g., bulk_load, bulk_load_from_existing)
* - Lookups (e.g., find_key, find_lower, find_upper, lower_bound, upper_bound)
* - Inserts and resizes (e.g, insert)
* - Deletes (e.g., erase, erase_one)
* - Stats
* - Debugging
*/
template <class T, class P, class Compare = AlexCompare,
          class Alloc = std::allocator<std::pair<AlexKey<T>, P>>,
          bool allow_duplicates = true>
class AlexDataNode : public AlexNode<T, P, Alloc> {
 public:
  typedef std::pair<AlexKey<T>, P> V;
  typedef AlexNode<T, P, Alloc> basic_node_type;
  typedef AlexModelNode<T, P, Alloc> model_node_type;
  typedef AlexDataNode<T, P, Compare, Alloc, allow_duplicates> self_type;
  typedef typename Alloc::template rebind<AlexKey<T>>::other key_alloc_type;
  typedef typename Alloc::template rebind<self_type>::other alloc_type;
  typedef typename Alloc::template rebind<P>::other payload_alloc_type;
  typedef typename Alloc::template rebind<V>::other value_alloc_type;
  typedef typename Alloc::template rebind<uint64_t>::other bitmap_alloc_type;
  typedef typename Alloc::template rebind<basic_node_type*>::other
      pointer_alloc_type;

  const Compare& key_less_;
  const Alloc& allocator_;

  // Forward declaration
  template <typename node_type = self_type>
  class Iterator;
  typedef Iterator<> iterator_type;
  typedef Iterator<const self_type> const_iterator_type;

  AtomicVal<self_type*> next_leaf_ = AtomicVal<self_type*>(nullptr);
  AtomicVal<self_type*> prev_leaf_ = AtomicVal<self_type*>(nullptr);
  AtomicVal<self_type*> pending_left_leaf_ = AtomicVal<self_type*>(nullptr);
  AtomicVal<self_type*> pending_right_leaf_ = AtomicVal<self_type*>(nullptr);

  AlexKey<T>* key_slots_ = nullptr;  // holds keys
  P* payload_slots_ =
      nullptr;  // holds payloads, must be same size as key_slots
  AlexKey<T>* delta_idx_ = nullptr;  // holds keys
  P* delta_idx_payloads_ =
      nullptr;  // holds payloads, must be same size as key_slots
  AlexKey<T>* tmp_delta_idx_ = nullptr;  // holds keys
  P* tmp_delta_idx_payloads_ =
      nullptr;  // holds payloads, must be same size as key_slots

  pthread_mutex_t insert_mutex_ = PTHREAD_MUTEX_INITIALIZER;
  pthread_rwlock_t key_array_rw_lock_ = PTHREAD_RWLOCK_INITIALIZER;
  pthread_rwlock_t delta_index_rw_lock_ = PTHREAD_RWLOCK_INITIALIZER;
  pthread_rwlock_t tmp_delta_index_rw_lock_ = PTHREAD_RWLOCK_INITIALIZER;

  int node_status_ = INSERT_AT_DATA;

  int data_capacity_ = 0;  // size of key/data_slots array
  int delta_idx_capacity_ = 0; //size of delta index
  int tmp_delta_idx_capacity_ = 0; //size of temporary delta index
  int num_keys_ = 0;  // number of filled key/data slots (as opposed to gaps)
  int delta_num_keys_ = 0; //number of filled key/data slots in delta index
  int tmp_delta_num_keys_ = 0; //number of filled key/data slots in temporary delta index
  T *the_max_key_arr_; //theoretic maximum key_arr
  T *the_min_key_arr_; //theoretic minimum key_arr

  // Bitmap: each uint64_t represents 64 positions in reverse order
  // (i.e., each uint64_t is "read" from the right-most bit to the left-most
  // bit)
  uint64_t* bitmap_ = nullptr;
  int bitmap_size_ = 0;  // number of int64_t in bitmap
  uint64_t* delta_bitmap_ = nullptr;
  int delta_bitmap_size_ = 0;
  uint64_t* tmp_delta_bitmap_ = nullptr;
  int tmp_delta_bitmap_size_ = 0;

  //models for delta indexes
  LinearModel<T> delta_idx_model_;
  LinearModel<T> tmp_delta_idx_model_;

  //some variables related to delta index semantic
  bool child_just_splitted_ = false; //true if it is a child that just splitted out from parent
  AtomicVal<int> *reused_delta_idx_cnt_ = nullptr; //number of node's referencing this node's delta index
                                                   //generated when it splitted from specific data node
                                                   //valid only for first split or resizing
  int boundary_base_key_idx_; //A key index that should be considered when merging with delta index
                              //generated when it splitted from specific data node
                              //valid only for first split or resizing.
                              //should be a starting key of right child
  bool was_left_child_ = false; //was it a left child when splitted?
  bool was_right_child_ = false; //was it a right child when splitted?

  // Variables related to resizing (expansions and contractions)
  static constexpr double kMaxDensity_ = 1;  // density after contracting,
                                               // also determines the expansion
                                               // threshold
  static constexpr double kInitDensity_ =
      1;  // density of data nodes after bulk loading
  static constexpr double kMinDensity_ = 1;  // density after expanding, also
                                               // determines the contraction
                                               // threshold
  double expansion_threshold_ = 1;  // expand after m_num_keys is >= this number
  double contraction_threshold_ =
      0;  // contract after m_num_keys is < this number
  static constexpr int kDefaultMaxDataNodeBytes_ =
      1 << 24;  // by default, maximum data node size is 16MB
  int max_slots_ =
      kDefaultMaxDataNodeBytes_ /
      sizeof(V);  // cannot expand beyond this number of key/data slots

  // Counters used in cost models
  long long num_shifts_ = 0;                 // does not reset after resizing
  long long num_exp_search_iterations_ = 0;  // does not reset after resizing
  int num_lookups_ = 0;                      // does not reset after resizing
  int num_inserts_ = 0;                      // does not reset after resizing

  static constexpr double kAppendMostlyThreshold = 0.9;

  // Purely for benchmark debugging purposes
  double expected_avg_exp_search_iterations_ = 0;
  double expected_avg_shifts_ = 0;

  // Placed at the end of the key/data slots if there are gaps after the max key.
  // It was originally static constexpr, but I changed to normal AlexKey.
  AlexKey<T> kEndSentinel_; 

  /*** Constructors and destructors ***/

  AlexDataNode () : AlexNode<T, P, Alloc>(0, true) {
    the_max_key_arr_ = new T[max_key_length_];
    the_min_key_arr_ = new T[max_key_length_];
    kEndSentinel_.key_arr_ = new T[max_key_length_];

    std::fill(kEndSentinel_.key_arr_, kEndSentinel_.key_arr_ + max_key_length_,
        STR_VAL_MAX);
    std::fill(the_max_key_arr_, the_max_key_arr_ + max_key_length_,
        STR_VAL_MAX);
    std::fill(the_min_key_arr_, the_min_key_arr_ + max_key_length_, STR_VAL_MIN);
  }

  explicit AlexDataNode(model_node_type *parent,
        const Compare& comp = Compare(), const Alloc& alloc = Alloc())
      : AlexNode<T, P, Alloc>(0, true, parent), key_less_(comp), allocator_(alloc) {
    the_max_key_arr_ = new T[max_key_length_];
    the_min_key_arr_ = new T[max_key_length_];
    kEndSentinel_.key_arr_ = new T[max_key_length_];
    
    std::fill(kEndSentinel_.key_arr_, kEndSentinel_.key_arr_ + max_key_length_,
        STR_VAL_MAX);
    std::fill(the_max_key_arr_, the_max_key_arr_ + max_key_length_,
        STR_VAL_MAX);
    std::fill(the_min_key_arr_ , the_min_key_arr_ + max_key_length_, STR_VAL_MIN);
    
  }

  AlexDataNode(short level, int max_data_node_slots, model_node_type *parent,
               const Compare& comp = Compare(), const Alloc& alloc = Alloc())
      : AlexNode<T, P, Alloc>(level, true, parent),
        key_less_(comp),
        allocator_(alloc),
        max_slots_(max_data_node_slots) {
    the_max_key_arr_ = new T[max_key_length_];
    the_min_key_arr_ = new T[max_key_length_];
    kEndSentinel_.key_arr_ = new T[max_key_length_];

    std::fill(kEndSentinel_.key_arr_, kEndSentinel_.key_arr_ + max_key_length_,
        STR_VAL_MAX);
    std::fill(the_max_key_arr_, the_max_key_arr_ + max_key_length_,
        STR_VAL_MAX);
    std::fill(the_min_key_arr_, the_min_key_arr_ + max_key_length_, STR_VAL_MIN);
    
  }

  ~AlexDataNode() {
    if (key_slots_ != nullptr) {
      delete[] key_slots_;
      payload_allocator().deallocate(payload_slots_, data_capacity_);
      bitmap_allocator().deallocate(bitmap_, bitmap_size_);
    }
    delete[] the_max_key_arr_;
    delete[] the_min_key_arr_;

    if (delta_idx_ != nullptr) {
      if (reused_delta_idx_cnt_ != nullptr) {
        reused_delta_idx_cnt_->lock();
        reused_delta_idx_cnt_->val_ -= 1;
        if (reused_delta_idx_cnt_->val_ == 0) {
          delete reused_delta_idx_cnt_;
          delete[] delta_idx_;
        }
        else {reused_delta_idx_cnt_->unlock();}
      }
      else {
        delete[] delta_idx_;
        payload_allocator().deallocate(delta_idx_payloads_, delta_idx_capacity_);
        bitmap_allocator().deallocate(delta_bitmap_, delta_bitmap_size_);
      }
    }

    //note that temporary delta index is always deleted before destructor
    //if ALEX terminated normally.

    pthread_mutex_destroy(&insert_mutex_);
    pthread_rwlock_destroy(&key_array_rw_lock_);
    pthread_rwlock_destroy(&delta_index_rw_lock_);
    pthread_rwlock_destroy(&tmp_delta_index_rw_lock_);
  }

  AlexDataNode(self_type& other)
      : AlexNode<T, P, Alloc>(other),
        key_less_(other.key_less_),
        allocator_(other.allocator_),
        data_capacity_(other.data_capacity_),
        num_keys_(other.num_keys_),
        bitmap_size_(other.bitmap_size_),
        expansion_threshold_(other.expansion_threshold_),
        contraction_threshold_(other.contraction_threshold_),
        max_slots_(other.max_slots_),
        num_shifts_(other.num_shifts_),
        num_exp_search_iterations_(other.num_exp_search_iterations_),
        num_lookups_(other.num_lookups_),
        num_inserts_(other.num_inserts_),
        expected_avg_exp_search_iterations_(
            other.expected_avg_exp_search_iterations_),
        expected_avg_shifts_(other.expected_avg_shifts_) {
    /* deep copy of max/min_key array is needed
     * since deletion of either one of the datanode
     * would result to other one's key's data pointer invalid
     * for similar reason, kEndSentinel_ also needs deep copying. */
    the_max_key_arr_ = new T[max_key_length_];
    the_min_key_arr_ = new T[max_key_length_];
    kEndSentinel_.key_arr_ = new T[max_key_length_];

    std::copy(other.the_max_key_arr_, other.the_max_key_arr_ + max_key_length_,
        the_max_key_arr_);
    std::copy(other.the_min_key_arr_, other.the_min_key_arr_ + max_key_length_,
        the_min_key_arr_);
    std::copy(other.kEndSentinel_.key_arr_, other.kEndSentinel_.key_arr_ + max_key_length_, 
        kEndSentinel_.key_arr_);

    prev_leaf_.val_ = other.prev_leaf_.val_;
    next_leaf_.val_ = other.next_leaf_.val_;

    key_slots_ = new AlexKey<T>[other.data_capacity_]();
    std::copy(other.key_slots_, other.key_slots_ + other.data_capacity_,
              key_slots_);
    payload_slots_ = new (payload_allocator().allocate(other.data_capacity_))
        P[other.data_capacity_];
    std::copy(other.payload_slots_, other.payload_slots_ + other.data_capacity_,
              payload_slots_);
    bitmap_ = new (bitmap_allocator().allocate(other.bitmap_size_))
        uint64_t[other.bitmap_size_];
    std::copy(other.bitmap_, other.bitmap_ + other.bitmap_size_, bitmap_);
  }

  /*** Allocators ***/
  pointer_alloc_type pointer_allocator() {
    return pointer_alloc_type(allocator_);
  }

  payload_alloc_type payload_allocator() {
    return payload_alloc_type(allocator_);
  }

  key_alloc_type key_allocator() {
    return key_alloc_type(allocator_);
  }

  value_alloc_type value_allocator() { return value_alloc_type(allocator_); }

  bitmap_alloc_type bitmap_allocator() { return bitmap_alloc_type(allocator_); }

  /*** General helper functions ***/

  inline AlexKey<T>& get_key(int pos) const { return ALEX_DATA_NODE_KEY_AT(pos); }

  //newly added for actual content achieving without need for max_length data.
  inline T *get_key_arr(int pos) const { return get_key(pos).key_arr_; }

  inline P& get_payload(int pos, int mode = 0) const {
    if (mode == KEY_ARR) {return payload_slots_[pos];}
    else if (mode == DELTA_IDX) {return delta_idx_payloads_[pos];}
    else {return tmp_delta_idx_payloads_[pos];}
  }

  // Check whether the position corresponds to a key (as opposed to a gap)
  bool check_exists(int pos, int mode = 0) const {
    int bitmap_pos = pos >> 6;
    int bit_pos = pos - (bitmap_pos << 6);
    switch (mode) {
      case KEY_ARR:
        assert(pos >= 0 && pos < data_capacity_);
        return static_cast<bool>(bitmap_[bitmap_pos] & (1ULL << bit_pos));
      case DELTA_IDX:
        assert(pos >= 0 && pos < delta_idx_capacity_);
        return static_cast<bool>(delta_bitmap_[bitmap_pos] & (1ULL << bit_pos));
      case TMP_DELTA_IDX:
        assert(pos >= 0 && pos < tmp_delta_idx_capacity_);
        return static_cast<bool>(tmp_delta_bitmap_[bitmap_pos] & (1ULL << bit_pos));
    }
    return false;
  }

  // Mark the entry for position in the bitmap
  inline void set_bit(int pos) {
    assert(pos >= 0 && pos < data_capacity_);
    int bitmap_pos = pos >> 6;
    int bit_pos = pos - (bitmap_pos << 6);
    bitmap_[bitmap_pos] |= (1ULL << bit_pos);
  }

  // Mark the entry for position in the bitmap
  inline void set_bit(uint64_t bitmap[], int pos) {
    int bitmap_pos = pos >> 6;
    int bit_pos = pos - (bitmap_pos << 6);
    bitmap[bitmap_pos] |= (1ULL << bit_pos);
  }

  // Unmark the entry for position in the bitmap
  inline void unset_bit(int pos) {
    assert(pos >= 0 && pos < data_capacity_);
    int bitmap_pos = pos >> 6;
    int bit_pos = pos - (bitmap_pos << 6);
    bitmap_[bitmap_pos] &= ~(1ULL << bit_pos);
  }

  // Value of first (i.e., min) key
  T* first_key() const {
    for (int i = 0; i < data_capacity_; i++) {
      if (check_exists(i)) return get_key_arr(i);
    }
    return the_max_key_arr_;
  }

  // Value of last (i.e., max) key
  T* last_key() const {
    for (int i = data_capacity_ - 1; i >= 0; i--) {
      if (check_exists(i)) return get_key_arr(i);
    }
    return the_min_key_arr_;
  }

  // Position in key/data_slots of first (i.e., min) key
  int first_pos() const {
    for (int i = 0; i < data_capacity_; i++) {
      if (check_exists(i)) return i;
    }
    return 0;
  }

  // Position in key/data_slots of last (i.e., max) key
  int last_pos() const {
    for (int i = data_capacity_ - 1; i >= 0; i--) {
      if (check_exists(i)) return i;
    }
    return 0;
  }

  // Number of keys between positions left and right (exclusive) in
  // key/data_slots
  int num_keys_in_range(int left, int right) const {
    assert(left >= 0 && left <= right && right <= data_capacity_);
    int num_keys = 0;
    int left_bitmap_idx = left >> 6;
    int right_bitmap_idx = right >> 6;
    if (left_bitmap_idx == right_bitmap_idx) {
      uint64_t bitmap_data = bitmap_[left_bitmap_idx];
      int left_bit_pos = left - (left_bitmap_idx << 6);
      bitmap_data &= ~((1ULL << left_bit_pos) - 1);
      int right_bit_pos = right - (right_bitmap_idx << 6);
      bitmap_data &= ((1ULL << right_bit_pos) - 1);
      num_keys += _mm_popcnt_u64(bitmap_data);
    } else {
      uint64_t left_bitmap_data = bitmap_[left_bitmap_idx];
      int bit_pos = left - (left_bitmap_idx << 6);
      left_bitmap_data &= ~((1ULL << bit_pos) - 1);
      num_keys += _mm_popcnt_u64(left_bitmap_data);
      for (int i = left_bitmap_idx + 1; i < right_bitmap_idx; i++) {
        num_keys += _mm_popcnt_u64(bitmap_[i]);
      }
      if (right_bitmap_idx != bitmap_size_) {
        uint64_t right_bitmap_data = bitmap_[right_bitmap_idx];
        bit_pos = right - (right_bitmap_idx << 6);
        right_bitmap_data &= ((1ULL << bit_pos) - 1);
        num_keys += _mm_popcnt_u64(right_bitmap_data);
      }
    }
    return num_keys;
  }

  // True if a < b
  template <class K>
  forceinline bool key_less(const AlexKey<T>& a, const AlexKey<K>& b) const {
    return key_less_(a, b);
  }

  // True if a <= b
  template <class K>
  forceinline bool key_lessequal(const AlexKey<T>& a, const AlexKey<K>& b) const {
    return !key_less_(b, a);
  }

  // True if a > b
  template <class K>
  forceinline bool key_greater(const AlexKey<T>& a, const AlexKey<K>& b) const {
    return key_less_(b, a);
  }

  // True if a >= b
  template <class K>
  forceinline bool key_greaterequal(const AlexKey<T>& a, const AlexKey<K>& b) const {
    return !key_less_(a, b);
  }

  // True if a == b
  template <class K>
  forceinline bool key_equal(const AlexKey<T>& a, const AlexKey<K>& b) const {
    return !key_less_(a, b) && !key_less_(b, a);
  }

  /*** Iterator ***/

  // Forward iterator meant for iterating over a single data node.
  // By default, it is a "normal" non-const iterator.
  // Can be templated to be a const iterator.
  template <typename node_type>
  class Iterator {
   public:
    node_type *node_;
    int cur_idx_ = 0;  // current position in key/data_slots, -1 if at end
    int cur_bitmap_idx_ = 0;  // current position in bitmap
    uint64_t cur_bitmap_data_ =
        0;  // caches the relevant data in the current bitmap position
    uint64_t *bitmap;
    int bitmap_size;
    AlexKey<T>* key_slots;
    P *payload_slots;

    explicit Iterator(node_type* node) {
      node_ = node;
      bitmap = node->bitmap_;
      bitmap_size = node->bitmap_size_;
      key_slots = node->key_slots_;
      payload_slots = node->payload_slots_;
    }

    Iterator(node_type* node, int idx) : cur_idx_(idx) {
      node_ = node;
      bitmap = node->bitmap_;
      bitmap_size = node->bitmap_size_;
      key_slots = node->key_slots_;
      payload_slots = node->payload_slots_;
      initialize();
    }

    Iterator(node_type* node, int idx, bool isDeltaIdx) : cur_idx_(idx) {
      node_ = node;
      if (isDeltaIdx) { //for delta index
        bitmap = node->delta_bitmap_;
        bitmap_size = node->delta_bitmap_size_;
        key_slots = node->delta_idx_;
        payload_slots = node->delta_idx_payloads_;
      }
      else { //for normal node iterating.
        bitmap = node->bitmap_;
        bitmap_size = node->bitmap_size_;
        key_slots = node->key_slots_;
        payload_slots = node->payload_slots_;
      }
      initialize();
    }

    void initialize() {
      cur_bitmap_idx_ = cur_idx_ >> 6;
      cur_bitmap_data_ = bitmap[cur_bitmap_idx_];

      // Zero out extra bits
      int bit_pos = cur_idx_ - (cur_bitmap_idx_ << 6);
      cur_bitmap_data_ &= ~((1ULL << bit_pos) - 1);

      (*this)++;
    }

    void operator++(int) {
      while (cur_bitmap_data_ == 0) {
        cur_bitmap_idx_++;
        if (cur_bitmap_idx_ >= bitmap_size) {
          cur_idx_ = -1;
          return;
        }
        cur_bitmap_data_ = bitmap[cur_bitmap_idx_];
      }
      uint64_t bit = extract_rightmost_one(cur_bitmap_data_);
      cur_idx_ = get_offset(cur_bitmap_idx_, bit);
      cur_bitmap_data_ = remove_rightmost_one(cur_bitmap_data_);
    }

    V operator*() const {
      return std::make_pair(key_slots[cur_idx_],
                            payload_slots[cur_idx_]);
    }

    AlexKey<T>& key() const {
      return key_slots[cur_idx_];
    }

    P& payload() const {
      return payload_slots[cur_idx_];
    }

    bool is_end() const { return cur_idx_ == -1; }

    bool operator==(const Iterator& rhs) const {
      return (key_slots == rhs.key_slots) && (cur_idx_ == rhs.cur_idx_);
    }

    bool operator!=(const Iterator& rhs) const { return !(*this == rhs); };

    bool is_smaller(const Iterator& rhs) const {
      if (cur_idx_ == -1) return false;
      if (rhs.cur_idx_ == -1) return true;
      if (node_->key_less(key_slots[cur_idx_], rhs.key_slots[rhs.cur_idx_])) return true;
      return false;
    }
  };

  iterator_type begin() { return iterator_type(this, 0); }
  iterator_type delta_begin() { return iterator_type(this, 0, true); }

  /*** Cost model ***/

  // Empirical average number of shifts per insert
  double shifts_per_insert() const {
    if (num_inserts_ == 0) {
      return 0;
    }
    return num_shifts_ / static_cast<double>(num_inserts_);
  }

  // Empirical average number of exponential search iterations per operation
  // (either lookup or insert)
  double exp_search_iterations_per_operation() const {
    if (num_inserts_ + num_lookups_ == 0) {
      return 0;
    }
    return num_exp_search_iterations_ /
           static_cast<double>(num_inserts_ + num_lookups_);
  }

  double empirical_cost() const {
    if (num_inserts_ + num_lookups_ == 0) {
      return 0;
    }
    double frac_inserts =
        static_cast<double>(num_inserts_) / (num_inserts_ + num_lookups_);
    return kExpSearchIterationsWeight * exp_search_iterations_per_operation() +
           kShiftsWeight * shifts_per_insert() * frac_inserts;
  }

  // Empirical fraction of operations (either lookup or insert) that are inserts
  double frac_inserts() const {
    int num_ops = num_inserts_ + num_lookups_;
    if (num_ops == 0) {
      return 0;  // if no operations, assume no inserts
    }
    return static_cast<double>(num_inserts_) / (num_inserts_ + num_lookups_);
  }

  void reset_stats() {
    num_shifts_ = 0;
    num_exp_search_iterations_ = 0;
    num_lookups_ = 0;
    num_inserts_ = 0;
  }

  // Computes the expected cost of the current node
  double compute_expected_cost(double frac_inserts = 0) {
    if (num_keys_ == 0) {
      return 0;
    }

    ExpectedSearchIterationsAccumulator search_iters_accumulator;
    ExpectedShiftsAccumulator shifts_accumulator(data_capacity_);
    const_iterator_type it(this, 0);
    for (; !it.is_end(); it++) {
      int predicted_position = std::max(
          0, std::min(data_capacity_ - 1, this->model_.predict(it.key())));
      search_iters_accumulator.accumulate(it.cur_idx_, predicted_position);
      shifts_accumulator.accumulate(it.cur_idx_, predicted_position);
    }
    expected_avg_exp_search_iterations_ = search_iters_accumulator.get_stat();
    expected_avg_shifts_ = shifts_accumulator.get_stat();
    double cost =
        kExpSearchIterationsWeight * expected_avg_exp_search_iterations_ +
        kShiftsWeight * expected_avg_shifts_ * frac_inserts;
    return cost;
  }

  // Computes the expected cost of a data node constructed using the input dense
  // array of keys
  // Assumes existing_model is trained on the dense array of keys
  static double compute_expected_cost(
      const V* values, int num_keys, double density, double expected_insert_frac,
      const LinearModel<T>* existing_model = nullptr, bool use_sampling = false,
      DataNodeStats* stats = nullptr) {
    if (use_sampling) {
      return compute_expected_cost_sampling(values, num_keys, density,
                                            expected_insert_frac,
                                            existing_model, stats);
    }

    if (num_keys == 0) {
      return 0;
    }

    int data_capacity =
        std::max(static_cast<int>(num_keys / density), num_keys + 1);

    // Compute what the node's model would be
    LinearModel<T> model;
    if (existing_model == nullptr) {
      build_model(values, num_keys, &model);
    } else {
      for (unsigned int i = 0; i < max_key_length_; i++) {
        model.a_[i] = existing_model->a_[i];
      }
      model.b_ = existing_model->b_;
    }
    model.expand(static_cast<double>(data_capacity) / num_keys);

    // Compute expected stats in order to compute the expected cost
    double cost = 0;
    double expected_avg_exp_search_iterations = 0;
    double expected_avg_shifts = 0;
    if (expected_insert_frac == 0) {
      ExpectedSearchIterationsAccumulator acc;
      build_node_implicit(values, num_keys, data_capacity, &acc, &model);
      expected_avg_exp_search_iterations = acc.get_stat();
    } else {
      ExpectedIterationsAndShiftsAccumulator acc(data_capacity);
      build_node_implicit(values, num_keys, data_capacity, &acc, &model);
      expected_avg_exp_search_iterations =
          acc.get_expected_num_search_iterations();
      expected_avg_shifts = acc.get_expected_num_shifts();
    }
    cost = kExpSearchIterationsWeight * expected_avg_exp_search_iterations +
           kShiftsWeight * expected_avg_shifts * expected_insert_frac;

    if (stats) {
      stats->num_search_iterations = expected_avg_exp_search_iterations;
      stats->num_shifts = expected_avg_shifts;
    }

    return cost;
  }

  // Helper function for compute_expected_cost
  // Implicitly build the data node in order to collect the stats
  static void build_node_implicit(const V* values, int num_keys,
                                  int data_capacity, StatAccumulator* acc,
                                  const LinearModel<T>* model) {
    int last_position = -1;
    int keys_remaining = num_keys;
    for (int i = 0; i < num_keys; i++) {
      int predicted_position = std::max(
          0, std::min(data_capacity - 1, model->predict(values[i].first)));
      int actual_position =
          std::max<int>(predicted_position, last_position + 1);
      int positions_remaining = data_capacity - actual_position;
      if (positions_remaining < keys_remaining) {
        actual_position = data_capacity - keys_remaining;
        for (int j = i; j < num_keys; j++) {
          predicted_position = std::max(
              0, std::min(data_capacity - 1, model->predict(values[j].first)));
          acc->accumulate(actual_position, predicted_position);
          actual_position++;
        }
        break;
      }
      acc->accumulate(actual_position, predicted_position);
      last_position = actual_position;
      keys_remaining--;
    }
  }

  // Using sampling, approximates the expected cost of a data node constructed
  // using the input dense array of keys
  // Assumes existing_model is trained on the dense array of keys
  // Uses progressive sampling: keep increasing the sample size until the
  // computed stats stop changing drastically
  static double compute_expected_cost_sampling(
      const V* values, int num_keys, double density, double expected_insert_frac,
      const LinearModel<T>* existing_model = nullptr, DataNodeStats* stats = nullptr) {
    const static int min_sample_size = 25;

    // Stop increasing sample size if relative diff of stats between samples is
    // less than this
    const static double rel_diff_threshold = 0.2;

    // Equivalent threshold in log2-space
    const static double abs_log2_diff_threshold =
        std::log2(1 + rel_diff_threshold);

    // Increase sample size by this many times each iteration
    const static int sample_size_multiplier = 2;

    // If num_keys is below this threshold, we compute entropy exactly
    const static int exact_computation_size_threshold =
        (min_sample_size * sample_size_multiplier * sample_size_multiplier * 2);

    // Target fraction of the keys to use in the initial sample
    const static double init_sample_frac = 0.01;

    // If the number of keys is sufficiently small, we do not sample
    if (num_keys < exact_computation_size_threshold) {
      return compute_expected_cost(values, num_keys, density,
                                   expected_insert_frac, existing_model, false,
                                   stats);
    }

    LinearModel<T> model;  // trained for full dense array
    if (existing_model == nullptr) {
      build_model(values, num_keys, &model);
    } else {
      for (unsigned int i = 0; i < max_key_length_; i++) {
        model.a_[i] = existing_model->a_[i];
      }
      model.b_ = existing_model->b_;
    }

    // Compute initial sample size and step size
    // Right now, sample_num_keys holds the target sample num keys
    int sample_num_keys = std::max(
        static_cast<int>(num_keys * init_sample_frac), min_sample_size);
    int step_size = 1;
    double tmp_sample_size =
        num_keys;  // this helps us determine the right sample size
    while (tmp_sample_size >= sample_num_keys) {
      tmp_sample_size /= sample_size_multiplier;
      step_size *= sample_size_multiplier;
    }
    step_size /= sample_size_multiplier;
    sample_num_keys =
        num_keys /
        step_size;  // now sample_num_keys is the actual sample num keys

    std::vector<SampleDataNodeStats>
        sample_stats;  // stats computed usinig each sample
    bool compute_shifts = expected_insert_frac !=
                          0;  // whether we need to compute expected shifts
    double log2_num_keys = std::log2(num_keys);
    double expected_full_search_iters =
        0;  // extrapolated estimate for search iters on the full array
    double expected_full_shifts =
        0;  // extrapolated estimate shifts on the full array
    bool search_iters_computed =
        false;  // set to true when search iters is accurately computed
    bool shifts_computed =
        false;  // set to true when shifts is accurately computed

    // Progressively increase sample size
    while (true) {
      int sample_data_capacity = std::max(
          static_cast<int>(sample_num_keys / density), sample_num_keys + 1);
      LinearModel<T> sample_model(model.a_, model.b_);
      sample_model.expand(static_cast<double>(sample_data_capacity) / num_keys);

      // Compute stats using the sample
      if (expected_insert_frac == 0) {
        ExpectedSearchIterationsAccumulator acc;
        build_node_implicit_sampling(values, num_keys, sample_num_keys,
                                     sample_data_capacity, step_size, &acc,
                                     &sample_model);
        sample_stats.push_back({std::log2(sample_num_keys), acc.get_stat(), 0});
      } else {
        ExpectedIterationsAndShiftsAccumulator acc(sample_data_capacity);
        build_node_implicit_sampling(values, num_keys, sample_num_keys,
                                     sample_data_capacity, step_size, &acc,
                                     &sample_model);
        sample_stats.push_back({std::log2(sample_num_keys),
                                acc.get_expected_num_search_iterations(),
                                std::log2(acc.get_expected_num_shifts())});
      }

      if (sample_stats.size() >= 3) {
        // Check if the diff in stats is sufficiently small
        SampleDataNodeStats& s0 = sample_stats[sample_stats.size() - 3];
        SampleDataNodeStats& s1 = sample_stats[sample_stats.size() - 2];
        SampleDataNodeStats& s2 = sample_stats[sample_stats.size() - 1];
        // (y1 - y0) / (x1 - x0) = (y2 - y1) / (x2 - x1) --> y2 = (y1 - y0) /
        // (x1 - x0) * (x2 - x1) + y1
        double expected_s2_search_iters =
            (s1.num_search_iterations - s0.num_search_iterations) /
                (s1.log2_sample_size - s0.log2_sample_size) *
                (s2.log2_sample_size - s1.log2_sample_size) +
            s1.num_search_iterations;
        double rel_diff =
            std::abs((s2.num_search_iterations - expected_s2_search_iters) /
                     s2.num_search_iterations);
        if (rel_diff <= rel_diff_threshold || num_keys <= 2 * sample_num_keys) {
          search_iters_computed = true;
          expected_full_search_iters =
              (s2.num_search_iterations - s1.num_search_iterations) /
                  (s2.log2_sample_size - s1.log2_sample_size) *
                  (log2_num_keys - s2.log2_sample_size) +
              s2.num_search_iterations;
        }
        if (compute_shifts) {
          double expected_s2_log2_shifts =
              (s1.log2_num_shifts - s0.log2_num_shifts) /
                  (s1.log2_sample_size - s0.log2_sample_size) *
                  (s2.log2_sample_size - s1.log2_sample_size) +
              s1.log2_num_shifts;
          double abs_diff =
              std::abs((s2.log2_num_shifts - expected_s2_log2_shifts) /
                       s2.log2_num_shifts);
          if (abs_diff <= abs_log2_diff_threshold ||
              num_keys <= 2 * sample_num_keys) {
            shifts_computed = true;
            double expected_full_log2_shifts =
                (s2.log2_num_shifts - s1.log2_num_shifts) /
                    (s2.log2_sample_size - s1.log2_sample_size) *
                    (log2_num_keys - s2.log2_sample_size) +
                s2.log2_num_shifts;
            expected_full_shifts = std::pow(2, expected_full_log2_shifts);
          }
        }

        // If diff in stats is sufficiently small, return the approximate
        // expected cost
        if ((expected_insert_frac == 0 && search_iters_computed) ||
            (expected_insert_frac > 0 && search_iters_computed &&
             shifts_computed)) {
          double cost =
              kExpSearchIterationsWeight * expected_full_search_iters +
              kShiftsWeight * expected_full_shifts * expected_insert_frac;
          if (stats) {
            stats->num_search_iterations = expected_full_search_iters;
            stats->num_shifts = expected_full_shifts;
          }
          return cost;
        }
      }

      step_size /= sample_size_multiplier;
      sample_num_keys = num_keys / step_size;
    }
  }

  // Helper function for compute_expected_cost_sampling
  // Implicitly build the data node in order to collect the stats
  // keys is the full un-sampled array of keys
  // sample_num_keys and sample_data_capacity refer to a data node that is
  // created only over the sample
  // sample_model is trained for the sampled data node
  static void build_node_implicit_sampling(const V* values, int num_keys,
                                           int sample_num_keys,
                                           int sample_data_capacity,
                                           int step_size, StatAccumulator* ent,
                                           const LinearModel<T>* sample_model) {
    int last_position = -1;
    int sample_keys_remaining = sample_num_keys;
    for (int i = 0; i < num_keys; i += step_size) {
      int predicted_position =
          std::max(0, std::min(sample_data_capacity - 1,
                               sample_model->predict(values[i].first)));
      int actual_position =
          std::max<int>(predicted_position, last_position + 1);
      int positions_remaining = sample_data_capacity - actual_position;
      if (positions_remaining < sample_keys_remaining) {
        actual_position = sample_data_capacity - sample_keys_remaining;
        for (int j = i; j < num_keys; j += step_size) {
          predicted_position =
              std::max(0, std::min(sample_data_capacity - 1,
                                   sample_model->predict(values[j].first)));
          ent->accumulate(actual_position, predicted_position);
          actual_position++;
        }
        break;
      }
      ent->accumulate(actual_position, predicted_position);
      last_position = actual_position;
      sample_keys_remaining--;
    }
  }

  // Computes the expected cost of a data node constructed using the keys
  // between left and right in the
  // key/data_slots of an existing node
  // Assumes existing_model is trained on the dense array of keys
  static double compute_expected_cost_from_existing(
      AlexKey<T>** node_keys, int left, int right, double density,
      double expected_insert_frac, const LinearModel<T>* existing_model = nullptr,
      DataNodeStats* stats = nullptr) {
    //assert(left >= 0 && right <= node->data_capacity_);

    LinearModel<T> model;
    int num_actual_keys = 0;
    if (existing_model == nullptr) {
      LinearModelBuilder<T> builder(&model);
      for (int it = left, j = 0; it < right; it++, j++) {
        builder.add(*node_keys[it], j);
        num_actual_keys++;
      }
      builder.build();
    } else {
      num_actual_keys = right - left;
      for (unsigned int i = 0; i < max_key_length_; i++) {
        model.a_[i] = existing_model->a_[i];
      }
      model.b_ = existing_model->b_;
    }

    if (num_actual_keys == 0) {
      return 0;
    }
    int data_capacity = std::max(static_cast<int>(num_actual_keys / density),
                                 num_actual_keys + 1);
    model.expand(static_cast<double>(data_capacity) / num_actual_keys);

    // Compute expected stats in order to compute the expected cost
    double cost = 0;
    double expected_avg_exp_search_iterations = 0;
    double expected_avg_shifts = 0;
    if (expected_insert_frac == 0) {
      ExpectedSearchIterationsAccumulator acc;
      build_node_implicit_from_existing(node_keys, left, right, num_actual_keys,
                                        data_capacity, &acc, &model);
      expected_avg_exp_search_iterations = acc.get_stat();
    } else {
      ExpectedIterationsAndShiftsAccumulator acc(data_capacity);
      build_node_implicit_from_existing(node_keys, left, right, num_actual_keys,
                                        data_capacity, &acc, &model);
      expected_avg_exp_search_iterations =
          acc.get_expected_num_search_iterations();
      expected_avg_shifts = acc.get_expected_num_shifts();
    }
    cost = kExpSearchIterationsWeight * expected_avg_exp_search_iterations +
           kShiftsWeight * expected_avg_shifts * expected_insert_frac;

    if (stats) {
      stats->num_search_iterations = expected_avg_exp_search_iterations;
      stats->num_shifts = expected_avg_shifts;
    }

    return cost;
  }

  // Helper function for compute_expected_cost
  // Implicitly build the data node in order to collect the stats
  static void build_node_implicit_from_existing(AlexKey<T>** node_keys,
                                                int left, int right, int num_actual_keys,
                                                int data_capacity, StatAccumulator* acc,
                                                const LinearModel<T>* model) {
    int last_position = -1;
    int keys_remaining = num_actual_keys;
    for (int it = left; it < right; it++) {
      int predicted_position =
          std::max(0, std::min(data_capacity - 1, model->predict(*node_keys[it])));
      int actual_position =
          std::max<int>(predicted_position, last_position + 1);
      int positions_remaining = data_capacity - actual_position;
      if (positions_remaining < keys_remaining) {
        actual_position = data_capacity - keys_remaining;
        for (; actual_position < data_capacity; actual_position++, it++) {
          predicted_position = std::max(
              0, std::min(data_capacity - 1, model->predict(*node_keys[it])));
          acc->accumulate(actual_position, predicted_position);
          keys_remaining--;
        }
        break;
      }
      acc->accumulate(actual_position, predicted_position);
      last_position = actual_position;
      keys_remaining--;
    }
    if (keys_remaining != 0) {
      std::cout << "keys_remaining should be 0, but it is : " << keys_remaining << std::endl;
      abort();
    } //should have no more leftover keys
  }

  /*** Bulk loading and model building ***/

  // Initalize key/payload/bitmap arrays and relevant metadata
  void initialize(int num_keys, double density, int expected_min_numkey_per_data_node) {
    num_keys_ = num_keys;
    data_capacity_ =
        std::max(
          std::max(static_cast<int>(num_keys / density), num_keys + 1),
          expected_min_numkey_per_data_node
        );
    bitmap_size_ = static_cast<size_t>(std::ceil(data_capacity_ / 64.));
    bitmap_ = new (bitmap_allocator().allocate(bitmap_size_))
        uint64_t[bitmap_size_]();  // initialize to all false
    key_slots_ = new AlexKey<T>[data_capacity_]();
    payload_slots_ =
        new (payload_allocator().allocate(data_capacity_)) P[data_capacity_];
  }

  // Assumes pretrained_model is trained on dense array of keys
  // I also assumed that all DataNodes have properly initialized key length limit. (max_key_length_)
  // second condition must be handled when creating data node.
  void bulk_load(const V values[], int num_keys, int expected_min_numkey_per_data_node,
                 const LinearModel<T>* pretrained_model = nullptr,
                 bool train_with_sample = false) {
    /* minimal condition checking. */
    initialize(num_keys, kInitDensity_, expected_min_numkey_per_data_node);

    if (num_keys == 0) {
      expansion_threshold_ = data_capacity_;
      contraction_threshold_ = 0;
      for (int i = 0; i < data_capacity_; i++) {
        ALEX_DATA_NODE_KEY_AT(i) = kEndSentinel_;
      }
      return;
    }

    // Build model
    if (pretrained_model != nullptr) {
      for (unsigned int i = 0; i < max_key_length_; i++) {
        this->model_.a_[i] = pretrained_model->a_[i];
      }
      this->model_.b_ = pretrained_model->b_;
    } else {
      build_model(values, num_keys, &(this->model_), train_with_sample);
    }
    this->model_.expand(static_cast<double>(data_capacity_) / num_keys);

#if DEBUG_PRINT
    //for (int i = 0; i < num_keys; i++) {
    //  std::cout << values[i].first.key_arr_ << " is " << this->model_.predict_double(values[i].first) << std::endl;
    //}
#endif

    // Model-based inserts
    int last_position = -1;
    int keys_remaining = num_keys;
    for (int i = 0; i < num_keys; i++) {
      int position = this->model_.predict(values[i].first);
      position = std::max<int>(position, last_position + 1);

      int positions_remaining = data_capacity_ - position;
      if (positions_remaining < keys_remaining) {
        // fill the rest of the store contiguously
        int pos = data_capacity_ - keys_remaining;
        for (int j = last_position + 1; j < pos; j++) {
          ALEX_DATA_NODE_KEY_AT(j) = values[i].first;
        }
        for (int j = i; j < num_keys; j++) {
          key_slots_[pos] = values[j].first;
          payload_slots_[pos] = values[j].second;
          set_bit(pos);
          pos++;
        }
        last_position = pos - 1;
        break;
      }

      for (int j = last_position + 1; j < position; j++) {
        ALEX_DATA_NODE_KEY_AT(j) = values[i].first;
      }

      key_slots_[position] = values[i].first;
      payload_slots_[position] = values[i].second;
      set_bit(position);

      last_position = position;

      keys_remaining--;
    }

    for (int i = last_position + 1; i < data_capacity_; i++) {
      ALEX_DATA_NODE_KEY_AT(i) = kEndSentinel_;
    }

    expansion_threshold_ = std::min(std::max(data_capacity_ * kMaxDensity_,
                                             static_cast<double>(num_keys + 1)),
                                    static_cast<double>(data_capacity_));
    contraction_threshold_ = data_capacity_ * kMinDensity_;

    std::copy(values[0].first.key_arr_, values[0].first.key_arr_ + max_key_length_,
      this->pivot_key_.key_arr_);
    
#if DEBUG_PRINT
      //std::cout << values[0].first.key_arr_ << '\n';
      //std::cout << values[num_keys-1].first.key_arr_ << '\n';
      //std::cout << "with max length as " << max_key_length_ << '\n';
      //std::cout << "pivot_key_(data_node) : " << this->pivot_key_.key_arr_ << '\n';
#endif
  }

  // Bulk load using the keys between the left and right positions in
  // key/data_slots of an existing data node
  // keep_left and keep_right are set if the existing node was append-mostly
  // If the linear model and num_actual_keys have been precomputed, we can avoid
  // redundant work
  void bulk_load_from_existing(
      AlexKey<T>** leaf_keys, P* leaf_payloads, int left, int right, 
      uint64_t worker_id, const LinearModel<T>* precomputed_model,
      int precomputed_num_actual_keys,int expected_min_numkey_per_data_node) {
    //assert(left >= 0 && right <= node->data_capacity_);

    // Build model
    int num_actual_keys = precomputed_num_actual_keys;
    for (unsigned int i = 0; i < max_key_length_; i++) {
      this->model_.a_[i] = precomputed_model->a_[i];
    }
    this->model_.b_ = precomputed_model->b_;

    initialize(num_actual_keys, kMinDensity_, expected_min_numkey_per_data_node);
    if (num_actual_keys == 0) {
      expansion_threshold_ = data_capacity_;
      contraction_threshold_ = 0;
      for (int i = 0; i < data_capacity_; i++) {
        ALEX_DATA_NODE_KEY_AT(i) = kEndSentinel_;
      }
      return;
    }

    this->model_.expand(static_cast<double>(data_capacity_) / num_keys_);

    // Model-based inserts
    int last_position = -1;
    int keys_remaining = num_keys_;
    for (unsigned int i = 0; i < max_key_length_; i++) {
      this->pivot_key_.key_arr_[i] = leaf_keys[left]->key_arr_[i];
    }
    for (int it = left; it < right; it++) {
      int position = this->model_.predict(*leaf_keys[it]);
      position = std::max<int>(position, last_position + 1);

      int positions_remaining = data_capacity_ - position;
      if (positions_remaining < keys_remaining) {
        // fill the rest of the store contiguously
        int pos = data_capacity_ - keys_remaining;
        for (int j = last_position + 1; j < pos; j++) {
          ALEX_DATA_NODE_KEY_AT(j) = *leaf_keys[it];
        }
        for (; pos < data_capacity_; pos++, it++) {
          key_slots_[pos] = *leaf_keys[it];
          payload_slots_[pos] = leaf_payloads[it];
          set_bit(pos);
        }
        last_position = pos - 1;
        break;
      }

      for (int j = last_position + 1; j < position; j++) {
        ALEX_DATA_NODE_KEY_AT(j) = *leaf_keys[it];
      }

      key_slots_[position] = *leaf_keys[it];
      payload_slots_[position] = leaf_payloads[it];
      set_bit(position);

      last_position = position;

      keys_remaining--;
    }

    for (int i = last_position + 1; i < data_capacity_; i++) {
      ALEX_DATA_NODE_KEY_AT(i) = kEndSentinel_;
    }

    expansion_threshold_ =
        std::min(std::max(data_capacity_ * kMaxDensity_,
                          static_cast<double>(num_keys_ + 1)),
                 static_cast<double>(data_capacity_));
    contraction_threshold_ = data_capacity_ * kMinDensity_;
  }

  static void build_model(const V* values, int num_keys, LinearModel<T>* model,
                          bool use_sampling = false) {
    /* sampling only possible for integer, double type keys... for now.*/
    if (use_sampling && (max_key_length_ == 1)) {
      build_model_sampling(values, num_keys, model);
      return;
    }

    LinearModelBuilder<T> builder(model);
    for (int i = 0; i < num_keys; i++) {
      builder.add(values[i].first, i);
    }
    builder.build();
  }

  // Uses progressive non-random uniform sampling to build the model
  // Progressively increases sample size until model parameters are relatively
  // stable
  static void build_model_sampling(const V* values, int num_keys,
                                   LinearModel<T>* model,
                                   bool verbose = false) {
    const static int sample_size_lower_bound = 10;
    // If slope and intercept change by less than this much between samples,
    // return
    const static double rel_change_threshold = 0.01;
    // If intercept changes by less than this much between samples, return
    const static double abs_change_threshold = 0.5;
    // Increase sample size by this many times each iteration
    const static int sample_size_multiplier = 2;

    // If the number of keys is sufficiently small, we do not sample
    if (num_keys <= sample_size_lower_bound * sample_size_multiplier) {
      build_model(values, num_keys, model, false);
      return;
    }

    int step_size = 1;
    double sample_size = num_keys;
    while (sample_size >= sample_size_lower_bound) {
      sample_size /= sample_size_multiplier;
      step_size *= sample_size_multiplier;
    }
    step_size /= sample_size_multiplier;

    // Run with initial step size
    LinearModelBuilder<T> builder(model);
    for (int i = 0; i < num_keys; i += step_size) {
      builder.add(values[i].first, i);
    }
    builder.build();
    double prev_a[max_key_length_] = {0.0};
    for (unsigned int i = 0; i < max_key_length_; i++) {
      prev_a[i] = model->a_[i];
    }
    double prev_b = model->b_;
    if (verbose) {
      std::cout << "Build index, sample size: " << num_keys / step_size
                << " a: (";
      for (unsigned int i = 0; i < max_key_length_; i++) {
        std::cout << prev_a[i];
      } 
      std::cout << "), b: " << prev_b << std::endl;
    }

    // Keep decreasing step size (increasing sample size) until model does not
    // change significantly
    while (step_size > 1) {
      step_size /= sample_size_multiplier;
      // Need to avoid processing keys we already processed in previous samples
      int i = 0;
      while (i < num_keys) {
        i += step_size;
        for (int j = 1; (j < sample_size_multiplier) && (i < num_keys);
             j++, i += step_size) {
          builder.add(values[i].first, i);
        }
      }
      builder.build();

      double rel_change_in_a[max_key_length_] = {0.0};
      for (unsigned int i = 0; i < max_key_length_; i++) {
        rel_change_in_a[i] = std::abs((model->a_[i] - prev_a[i]) / prev_a[i]);
      }
      double abs_change_in_b = std::abs(model->b_ - prev_b);
      double rel_change_in_b = std::abs(abs_change_in_b / prev_b);
      if (verbose) {
        std::cout << "Build index, sample size: " << num_keys / step_size
                  << " new (a, b): (";
        for (unsigned int i = 0; i < max_key_length_; i++) {
          std::cout << model->a_[i];
        }
        std::cout << ", " << model->b_ << ") relative change : (";
        for (unsigned int i = 0; i < max_key_length_; i++) {
          std::cout << rel_change_in_a[i];
        }
        std::cout << ", " << rel_change_in_b << ")"
                  << std::endl;
      }
      char threshold = 1;
      for (unsigned int i = 0; i < max_key_length_; i++) {
        if (rel_change_in_a[i] > rel_change_threshold) {
          threshold = 0;
          break;
        }
      }
      if (threshold &&
          (rel_change_in_b < rel_change_threshold ||
           abs_change_in_b < abs_change_threshold)) {
        return;
      }
      for (unsigned int i = 0; i < max_key_length_; i++) {
        prev_a[i] = model->a_[i];
      }
      prev_b = model->b_;
    }
  }

  // Unused function: builds a spline model by connecting the smallest and
  // largest points instead of using
  // a linear regression
  //static void build_spline(const V* values, int num_keys,
  //                         const LinearModel<T>* model) {
  //  int y_max = num_keys - 1;
  //  int y_min = 0;
  //  model->a_ = static_cast<double>(y_max - y_min) /
  //              (values[y_max].first - values[y_min].first);
  //  model->b_ = -1.0 * values[y_min].first * model->a_;
  //}

  /*** Lookup ***/

  // Predicts the position of a key in data array using the model
  inline int predict_position(const AlexKey<T>& key, int mode = KEY_ARR) const {
    int position;
    switch(mode) {
      case KEY_ARR:
        position = this->model_.predict(key);
        position = std::max<int>(std::min<int>(position, data_capacity_ - 1), 0);
        break;
      case DELTA_IDX:
        assert(delta_idx_ != nullptr);
        position = delta_idx_model_.predict(key);
        position = std::max<int>(std::min<int>(position, delta_idx_capacity_ - 1), 0);
        break;
      case TMP_DELTA_IDX:
        if (tmp_delta_idx_ == nullptr) {
          std::cout << "leaf pointer - " << this << " has empty tmp delta?\n";
          abort();
        }
        position = tmp_delta_idx_model_.predict(key);
        position = std::max<int>(std::min<int>(position, tmp_delta_idx_capacity_ - 1), 0);
        break;
      default:
        abort();
    }
    return position;
  }

  // Searches for the last non-gap position equal to key
  // If no positions equal to key, returns -1
  int find_key(const AlexKey<T>& key, uint64_t worker_id, int mode) {
    //start searching when no write is running.
#if PROFILE
    auto find_key_start_time = std::chrono::high_resolution_clock::now();
    profileStats.find_key_call_cnt[worker_id]++;
#endif
    AlexKey<T> *ref_arr;
    int ref_capacity;
    switch(mode) {
      case DELTA_IDX:
        ref_arr = delta_idx_;
        ref_capacity = delta_idx_capacity_;
        break;
      case TMP_DELTA_IDX:
        ref_arr = tmp_delta_idx_;
        ref_capacity = tmp_delta_idx_capacity_;
        break;
      default:
        ref_arr = key_slots_;
        ref_capacity = data_capacity_;
        break;
    }

    num_lookups_++;
    int predicted_pos = predict_position(key, mode);

    // The last key slot with a certain value is guaranteed to be a real key
    // (instead of a gap)
    int pos = exponential_search_upper_bound(predicted_pos, key, ref_arr, ref_capacity) - 1;
    if (pos < 0 || !key_equal(ref_arr[pos], key)) {
      return -1;
    } else {
#if PROFILE
      auto find_key_end_time = std::chrono::high_resolution_clock::now();
      auto elapsed_time = std::chrono::duration_cast<std::chrono::fgTimeUnit>(find_key_end_time - find_key_start_time).count();
      profileStats.find_key_time[worker_id] += elapsed_time;
      profileStats.max_find_key_time[worker_id] =
        std::max(profileStats.max_find_key_time[worker_id], elapsed_time);
      profileStats.min_find_key_time[worker_id] =
        std::min(profileStats.min_find_key_time[worker_id], elapsed_time);
#endif

      return pos;
    }
  }

  // Searches for the first non-gap position no less than key
  // Returns position in range [0, data_capacity]
  // Compare with lower_bound()
  int find_lower(const AlexKey<T>& key) {
    num_lookups_++;
    int predicted_pos = predict_position(key);

    int pos = exponential_search_lower_bound(predicted_pos, key);
    return get_next_filled_position(pos, false);
  }

  // Searches for the first non-gap position greater than key
  // Returns position in range [0, data_capacity]
  // Compare with upper_bound()
  int find_upper(const AlexKey<T>& key) {
    num_lookups_++;
    int predicted_pos = predict_position(key);

    int pos = exponential_search_upper_bound(predicted_pos, key);
    return get_next_filled_position(pos, false);
  }

  // Finds position to insert a key.
  // First returned value takes prediction into account.
  // Second returned value is first valid position (i.e., upper_bound of key).
  // If there are duplicate keys, the insert position will be to the right of
  // all existing keys of the same value.
  std::pair<int, int> find_insert_position(const AlexKey<T>& key, uint64_t worker_id) {
    return find_insert_position(key, key_slots_, data_capacity_, KEY_ARR, worker_id);
  }

  std::pair<int, int> find_insert_position(const AlexKey<T>& key, AlexKey<T> *ref_arr, 
                                           int ref_capacity, int node_status, uint64_t worker_id) {
#if PROFILE
    profileStats.find_insert_position_call_cnt[worker_id]++;
    auto find_insert_position_start_time = std::chrono::high_resolution_clock::now();
#endif
    int predicted_pos = predict_position(key, node_status);
    // insert to the right of duplicate keys
    int pos = exponential_search_upper_bound(predicted_pos, key, ref_arr, ref_capacity);
#if PROFILE
    auto find_insert_position_end_time = std::chrono::high_resolution_clock::now();
    auto elapsed_time = std::chrono::duration_cast<std::chrono::fgTimeUnit>(
      find_insert_position_end_time - find_insert_position_start_time
    ).count();
    profileStats.find_insert_position_time[worker_id] += elapsed_time;
    profileStats.max_find_insert_position_time[worker_id] = 
      std::max(profileStats.max_find_insert_position_time[worker_id], elapsed_time);
    profileStats.min_find_insert_position_time[worker_id] =
      std::min(profileStats.min_find_insert_position_time[worker_id], elapsed_time);
#endif
    if (predicted_pos <= pos || check_exists(pos, node_status)) {
      return {pos, pos};
    } else {
      // Place inserted key as close as possible to the predicted position while
      // maintaining correctness
      return {std::min(predicted_pos, get_next_filled_position(pos, true, node_status) - 1),
              pos};
    }
  }

  // Starting from a position, return the first position that is not a gap
  // If no more filled positions, will return data_capacity
  // If exclusive is true, output is at least (pos + 1)
  // If exclusive is false, output can be pos itself
  int get_next_filled_position(int pos, bool exclusive, int node_status = INSERT_AT_DATA) const {
    int ref_capacity;
    uint64_t *ref_bitmap;
    int ref_bitmap_size;
    switch(node_status) {
      case INSERT_AT_DELTA:
        ref_capacity = delta_idx_capacity_;
        ref_bitmap = delta_bitmap_;
        ref_bitmap_size = delta_bitmap_size_;
        break;
      case INSERT_AT_TMPDELTA:
        ref_capacity = tmp_delta_idx_capacity_;
        ref_bitmap = tmp_delta_bitmap_;
        ref_bitmap_size = tmp_delta_bitmap_size_;
        break;
      default:
        ref_capacity = data_capacity_;
        ref_bitmap = bitmap_;
        ref_bitmap_size = bitmap_size_;
    }
    
    if (exclusive) {
      pos++;
      if (pos == ref_capacity) {
        return ref_capacity;
      }
    }

    int curBitmapIdx = pos >> 6;
    uint64_t curBitmapData = ref_bitmap[curBitmapIdx];

    // Zero out extra bits
    int bit_pos = pos - (curBitmapIdx << 6);
    curBitmapData &= ~((1ULL << (bit_pos)) - 1);

    while (curBitmapData == 0) {
      curBitmapIdx++;
      if (curBitmapIdx >= ref_bitmap_size) {
        return ref_capacity;
      }
      curBitmapData = ref_bitmap[curBitmapIdx];
    }
    uint64_t bit = extract_rightmost_one(curBitmapData);
    return get_offset(curBitmapIdx, bit);
  }

  // Searches for the first position greater than key
  // This could be the position for a gap (i.e., its bit in the bitmap is 0)
  // Returns position in range [0, data_capacity]
  // Compare with find_upper()
  int upper_bound(const AlexKey<T>& key) {
    num_lookups_++;
    int position = predict_position(key);
    return exponential_search_upper_bound(position, key);
  }

  // Searches for the first position greater than key, starting from position m
  // Returns position in range [0, data_capacity]
  int exponential_search_upper_bound(int m, const AlexKey<T>& key) {
    return exponential_search_upper_bound(m, key, key_slots_, data_capacity_);
  }

  inline int exponential_search_upper_bound(int m, const AlexKey<T>& key, 
                                            AlexKey<T> *ref_arr, int ref_capacity) {
    // Continue doubling the bound until it contains the upper bound. Then use
    // binary search.
    int bound = 1;
    int l, r;  // will do binary search in range [l, r)
    if (key_greater(ref_arr[m], key)) {
      int size = m;
      while (bound < size &&
             key_greater(ref_arr[m - bound], key)) {
        bound *= 2;
        num_exp_search_iterations_++;
      }
      l = m - std::min<int>(bound, size);
      r = m - bound / 2;
    } else {
      int size = ref_capacity - m;
      while (bound < size &&
             key_lessequal(ref_arr[m + bound], key)) {
        bound *= 2;
        num_exp_search_iterations_++;
      }
      l = m + bound / 2;
      r = m + std::min<int>(bound, size);
    }
    return binary_search_upper_bound(l, r, key, ref_arr);
  }

  // Searches for the first position greater than key in range [l, r)
  // https://stackoverflow.com/questions/6443569/implementation-of-c-lower-bound
  // Returns position in range [l, r]
  inline int binary_search_upper_bound(int l, int r, const AlexKey<T>& key, AlexKey<T>* ref_arr) const {
    while (l < r) {
      int mid = l + (r - l) / 2;
      if (key_lessequal(ref_arr[mid], key)) {
        l = mid + 1;
      } else {
        r = mid;
      }
    }
    return l;
  }

  // Searches for the first position no less than key
  // This could be the position for a gap (i.e., its bit in the bitmap is 0)
  // Returns position in range [0, data_capacity]
  // Compare with find_lower()
  int lower_bound(const AlexKey<T>& key) {
    num_lookups_++;
    int position = predict_position(key);
    return exponential_search_lower_bound(position, key);
  }

  // Searches for the first position no less than key, starting from position m
  // Returns position in range [0, data_capacity]
  inline int exponential_search_lower_bound(int m, const AlexKey<T>& key) {
    // Continue doubling the bound until it contains the lower bound. Then use
    // binary search.
    int bound = 1;
    int l, r;  // will do binary search in range [l, r)
    if (key_greaterequal(ALEX_DATA_NODE_KEY_AT(m), key)) {
      int size = m;
      while (bound < size &&
             key_greaterequal(ALEX_DATA_NODE_KEY_AT(m - bound), key)) {
        bound *= 2;
        num_exp_search_iterations_++;
      }
      l = m - std::min<int>(bound, size);
      r = m - bound / 2;
    } else {
      int size = data_capacity_ - m;
      while (bound < size && key_less(ALEX_DATA_NODE_KEY_AT(m + bound), key)) {
        bound *= 2;
        num_exp_search_iterations_++;
      }
      l = m + bound / 2;
      r = m + std::min<int>(bound, size);
    }
    return binary_search_lower_bound(l, r, key);
  }

  // Searches for the first position no less than key in range [l, r)
  // https://stackoverflow.com/questions/6443569/implementation-of-c-lower-bound
  // Returns position in range [l, r]
  inline int binary_search_lower_bound(int l, int r, const AlexKey<T>& key) const {
    while (l < r) {
      int mid = l + (r - l) / 2;
      if (key_greaterequal(ALEX_DATA_NODE_KEY_AT(mid), key)) {
        r = mid;
      } else {
        l = mid + 1;
      }
    }
    return l;
  }

  /*** delta index realted ***/

  //make temporal delta index for insert to use
  //while data node is being modified.
  void generate_new_delta_idx(int expected_min_numkey_per_data_node, uint64_t worker_id) {
    //make new delta index first.
    int new_delta_idx_capacity = (delta_idx_capacity_const != 0) ? delta_idx_capacity_const : std::max((num_keys_ + delta_num_keys_), 1024);
    auto new_delta_bitmap_size = static_cast<size_t>(std::ceil(new_delta_idx_capacity / 64.));
    auto new_delta_bitmap = new (bitmap_allocator().allocate(new_delta_bitmap_size))
        uint64_t[new_delta_bitmap_size]();
    AlexKey<T>* new_delta_idx =
        new AlexKey<T>[new_delta_idx_capacity]();
    P* new_delta_idx_payloads = new (payload_allocator().allocate(new_delta_idx_capacity))
        P[new_delta_idx_capacity];

    if (delta_idx_ == nullptr) {
#if DEBUG_PRINT
      coutLock.lock();
      std::cout << "t" << worker_id << " - making delta_idx_" << std::endl;
      coutLock.unlock();
#endif
      pthread_rwlock_wrlock(&delta_index_rw_lock_); //prevent reading in delta index before preparation
      delta_num_keys_ = 0;
      delta_idx_capacity_ = new_delta_idx_capacity;
      delta_idx_model_ = this->model_;
      delta_bitmap_ = new_delta_bitmap;
      delta_bitmap_size_ = new_delta_bitmap_size;
      delta_idx_payloads_ = new_delta_idx_payloads;
      delta_idx_ = new_delta_idx;
      for (int i = 0; i < delta_idx_capacity_; ++i) {
        delta_idx_[i] = kEndSentinel_;
      }
      node_status_ = INSERT_AT_DELTA;
      pthread_rwlock_unlock(&delta_index_rw_lock_);
    }
    else {
#if DEBUG_PRINT
      coutLock.lock();
      std::cout << "t" << worker_id << " - making tmp_delta_idx_" << std::endl;
      coutLock.unlock();
#endif
      pthread_rwlock_wrlock(&tmp_delta_index_rw_lock_); //prevent reading in temporary delta index before preparation
      tmp_delta_num_keys_ = 0;
      tmp_delta_idx_capacity_ = new_delta_idx_capacity;
      tmp_delta_idx_model_ = this->model_;
      tmp_delta_bitmap_ = new_delta_bitmap;
      tmp_delta_bitmap_size_ = new_delta_bitmap_size;
      tmp_delta_idx_payloads_ = new_delta_idx_payloads;
      tmp_delta_idx_ = new_delta_idx;
      for (int i = 0; i < tmp_delta_idx_capacity_; ++i) {
        tmp_delta_idx_[i] = kEndSentinel_;
      }
      node_status_ = INSERT_AT_TMPDELTA;
      pthread_rwlock_unlock(&tmp_delta_index_rw_lock_);
    }
#if DEBUG_PRINT
    coutLock.lock();
    std::cout << "t" << worker_id << " - finished making new delta / tmp delta index" << std::endl;
    coutLock.unlock();
#endif
  }

  //updating delta index after resize of node
  //may need to check if we could do better, no contention synchronization.
  void update_delta_idx_resize(uint64_t worker_id) {
#if DEBUG_PRINT
    coutLock.lock();
    std::cout << "t" << worker_id << "'s generated thread - updating delta / tmp delta index" << std::endl;
    coutLock.unlock();
#endif
    pthread_mutex_lock(&insert_mutex_);
    if (node_status_ == INSERT_AT_DELTA) {
      //leave it as it is. Just change the mode.
#if DEBUG_PRINT
      coutLock.lock();
      std::cout << "t" << worker_id << "'s generated thread - leaved delta_index_" << std::endl;
      coutLock.unlock();
#endif
      node_status_ = INSERT_AT_DATA;
      pthread_mutex_unlock(&insert_mutex_);
    }
    else {
      //need to move tmp_delta_idx_ to delta_idx_.
      //don't read from delta index while moving
      //it could use wrong metadata to iterate through delta index
      pthread_rwlock_wrlock(&delta_index_rw_lock_);

      //don't read from temporary delta index while moving
      //it could use wrong metadata to iterate through temporary delta index
      pthread_rwlock_wrlock(&tmp_delta_index_rw_lock_);

      //temporary saving
      auto old_delta_idx_ = delta_idx_;
      auto old_delta_bitmap_ = delta_bitmap_;
      auto old_delta_payloads_ = delta_idx_payloads_;

      //copying
      delta_idx_ = tmp_delta_idx_;
      delta_bitmap_ = tmp_delta_bitmap_;
      delta_idx_payloads_ = tmp_delta_idx_payloads_;
      delta_idx_capacity_ = tmp_delta_idx_capacity_;
      delta_num_keys_ = tmp_delta_num_keys_;
      delta_bitmap_size_ = tmp_delta_bitmap_size_;
      delta_idx_model_ = tmp_delta_idx_model_;

      //cleaning
      tmp_delta_idx_ = nullptr;
      tmp_delta_bitmap_ = nullptr;
      tmp_delta_idx_payloads_ = nullptr;
      tmp_delta_bitmap_size_ = 0;
      tmp_delta_idx_capacity_ = 0;
      node_status_ = INSERT_AT_DATA;
      pthread_rwlock_unlock(&tmp_delta_index_rw_lock_);
      pthread_rwlock_unlock(&delta_index_rw_lock_);
      pthread_mutex_unlock(&insert_mutex_);

      if (child_just_splitted_) {//if it's sharing delta index
#if DEBUG_PRINT
        coutLock.lock();
        std::cout << "t" << worker_id << "'s generated thread - was referencing parent delta index" << std::endl;
        coutLock.unlock();
#endif 
        child_just_splitted_ = false; //it now has new delta index
        reused_delta_idx_cnt_->lock();
        reused_delta_idx_cnt_->val_ -= 1;
        if (reused_delta_idx_cnt_->val_ != 0) {
          //somebody is referencing this delta index
          reused_delta_idx_cnt_->unlock();
          reused_delta_idx_cnt_ = nullptr;
          return;
        }
        else {
          //nobody is referencing this delta index
          delete reused_delta_idx_cnt_;
          reused_delta_idx_cnt_ = nullptr;
        }
      }
      delete[] old_delta_idx_;
      delete[] old_delta_bitmap_;
      delete[] old_delta_payloads_;
#if DEBUG_PRINT
      coutLock.lock();
      std::cout << "t" << worker_id << "'s generated thread - deleted old delta index" << std::endl;
      coutLock.unlock();
#endif 
    }
  }

  /*** Inserts and resizes ***/

  // Whether empirical cost deviates significantly from expected cost
  // Also returns false if empirical cost is sufficiently low and is not worth
  // splitting
  inline bool significant_cost_deviation() const {
    double emp_cost = empirical_cost();
    return emp_cost > kNodeLookupsWeight && emp_cost > 1.5 * this->cost_;
  }

  // Returns true if cost is catastrophically high and we want to force a split
  // The heuristic for this is if the number of shifts per insert (expected or
  // empirical) is over 100
  inline bool catastrophic_cost() const {
    return shifts_per_insert() > 100 || expected_avg_shifts_ > 100;
  }

  // First pair's first value in returned pair is fail flag:
  // 0 if successful insert , maybe with automatic expansion.
  // 1 if no insert because of significant cost deviation.
  // 2 if no insert because of "catastrophic" cost.
  // 3 if no insert because node is at max capacity.
  // 4 if we should expand.
  // 5 if we must expand because capacity is full in key_slots_ 
  // 6 if delta_index_ || tmp_delta_index_ is full making insert impossible.
  // -1 if key already exists and duplicates not allowed.
  //
  // First pair's second value in returned pair is position of inserted key, or of the
  // already-existing key.
  // -1 if no insertion.
  //
  // second pair has original data node pointer, and maybe new data node pointer
  std::pair<std::pair<int, int>, std::pair<self_type *, self_type *>> insert(
    const AlexKey<T>& key, const P& payload, uint64_t worker_id) {
    // Periodically check for catastrophe
#if DEBUG_PRINT
    //alex::coutLock.lock();
    //std::cout << "t" << worker_id << " - ";
    //std::cout << "alex_nodes.h - expected_avg_shifts_ : " << expected_avg_shifts_ << std::endl;
    //alex::coutLock.unlock();
#endif
    if (node_status_ == INSERT_AT_DATA) {
      return insert_at_data(key, payload, worker_id);
    }
    else {
      return insert_at_delta(key, payload, worker_id, node_status_);
    }
  }

  //case of insertion in key_slots_
  //can return all kinds of pair shown above
  std::pair<std::pair<int, int>, std::pair<self_type *, self_type *>> insert_at_data(
    const AlexKey<T>& key, const P& payload, uint64_t worker_id) {
#if DEBUG_PRINT
    alex::coutLock.lock();
    std::cout << "t" << worker_id << " - ";
    std::cout << "alex_nodes.h insert : inserting to key_slots_" << std::endl;
    alex::coutLock.unlock();
#endif
    int insertion_position = -1;
    std::pair<int, int> positions = find_insert_position(key, worker_id);
    int upper_bound_pos = positions.second;
    if (!allow_duplicates && upper_bound_pos > 0 &&
        key_equal(ALEX_DATA_NODE_KEY_AT(upper_bound_pos - 1), key)) {
      return {{-1, upper_bound_pos - 1}, {this, nullptr}};
    }
    insertion_position = positions.first;
    if (insertion_position < data_capacity_ &&
        !check_exists(insertion_position)) {
      insert_element_at(key, payload, insertion_position, worker_id, 1);
    } else {
      insertion_position =
          insert_using_shifts(key, payload, insertion_position, worker_id);
    }
    // Update stats
    num_keys_++;
    num_inserts_++;
    
    if (num_inserts_ % 1 == 0 && catastrophic_cost()) {
      return {{2, insertion_position}, {this, nullptr}};
    }

    // Check if node is full (based on expansion_threshold)
    if (num_keys_ == data_capacity_) {
      //MUST do some modification.
      return {{5, insertion_position}, {this, nullptr}};
    }
    if (num_keys_ >= expansion_threshold_) {
      if (significant_cost_deviation()) {
        return {{1, insertion_position}, {this, nullptr}};
      }
      if (catastrophic_cost()) {
        return {{2, insertion_position}, {this, nullptr}};
      }
      if (num_keys_ > max_slots_ * kMinDensity_) {
        return {{3, insertion_position}, {this, nullptr}};
      }
      //notify that it should expand.
      return {{4, insertion_position}, {this, nullptr}};
    }
    return {{0, insertion_position}, {this, nullptr}};
  }

  //case of insertion in delta_idx_ / tmp_delta_idx_
  //should succeed, or fail because of full capacity
  std::pair<std::pair<int, int>, std::pair<self_type *, self_type *>> insert_at_delta(
    const AlexKey<T>& key, const P& payload, uint64_t worker_id, int node_status) {
#if DEBUG_PRINT
    alex::coutLock.lock();
    std::cout << "t" << worker_id << " - ";
    std::cout << "alex_nodes.h insert : inserting to delta_index_" << std::endl;
    alex::coutLock.unlock();
#endif
    int insertion_position = -1;
    AlexKey<T> *ref_arr;
    int ref_capacity;
    switch(node_status) {
      case INSERT_AT_DELTA:
        ref_arr = delta_idx_;
        ref_capacity = delta_idx_capacity_;
        break;
      case INSERT_AT_TMPDELTA:
        ref_arr = tmp_delta_idx_;
        ref_capacity = tmp_delta_idx_capacity_;
        break;
      default:
        ref_arr = delta_idx_;
        ref_capacity = delta_idx_capacity_;
        break;
    }

    if (node_status == INSERT_AT_DELTA) {
      if (delta_num_keys_ == delta_idx_capacity_) {
#if DEBUG_PRINT
        alex::coutLock.lock();
        std::cout << "t" << worker_id << " - ";
        std::cout << "alex_nodes.h insert : failed inserting to delta_index_ because it's full" << std::endl;
        alex::coutLock.unlock();
#endif
        return {{6, 0}, {this, nullptr}};
      }
      delta_num_keys_++;
    }
    else {
      if (tmp_delta_num_keys_ == tmp_delta_idx_capacity_) {
#if DEBUG_PRINT
        alex::coutLock.lock();
        std::cout << "t" << worker_id << " - ";
        std::cout << "alex_nodes.h insert : failed inserting to tmp_delta_index_ because it's full" << std::endl;
        alex::coutLock.unlock();
#endif
        return {{6, 0}, {this, nullptr}};
      }
      tmp_delta_num_keys_++;
    }
    std::pair<int, int> positions = find_insert_position(key, ref_arr, ref_capacity, node_status, worker_id);
    int upper_bound_pos = positions.second;
    if (!allow_duplicates && upper_bound_pos > 0 &&
        key_equal(ref_arr[upper_bound_pos - 1], key)) {
      return {{-1, upper_bound_pos - 1}, {this, nullptr}};
    }
    insertion_position = positions.first;
    if (insertion_position < ref_capacity &&
        !check_exists(insertion_position, node_status)) {
      insert_element_at(key, payload, insertion_position, worker_id, 1, node_status);
    } else {
      insertion_position =
          insert_using_shifts(key, payload, insertion_position, worker_id, node_status);
    }
    
    return {{0, insertion_position}, {this, nullptr}};
  }

  //helper for resize
  //actual insert of resize happens here
  void resize_insert(P* new_payload_slots, uint64_t *new_bitmap, AlexKey<T> *new_key_slots, 
                     LinearModel<T> &new_model, int keys_remaining, int new_data_capacity,
                     const_iterator_type it, const_iterator_type delta_it) {
    AlexKey<T> key;
    P payload;
    int last_position = -1;

    while (keys_remaining > 0) {
      if (it.is_smaller(delta_it)) {
        key = it.key();
        payload = it.payload();
        it++;
      }
      else {
        key = delta_it.key();
        payload = delta_it.payload();
        delta_it++;
      }

      int position = new_model.predict(key);
      position = std::max<int>(position, last_position + 1);

      int positions_remaining = new_data_capacity - position;
      if (positions_remaining < keys_remaining) {
        // fill the rest of the store contiguously
        int pos = new_data_capacity - keys_remaining;
        for (int j = last_position + 1; j < pos; j++) {
          new_key_slots[j] = key;
        }
        if (pos < new_data_capacity) {
          new_key_slots[pos] = key;
          new_payload_slots[pos] = payload;
          set_bit(new_bitmap, pos);
          pos++;
        } else {break;}
        for (; pos < new_data_capacity; pos++) {
          if (it.is_smaller(delta_it)) {
            key = it.key();
            payload = it.payload();
            it++;
          }
          else {
            key = delta_it.key();
            payload = delta_it.payload();
            delta_it++;
          }
          new_key_slots[pos] = key;
          new_payload_slots[pos] = payload;
          set_bit(new_bitmap, pos);
        }
        last_position = pos - 1;
        break;
      }

      for (int j = last_position + 1; j < position; j++) {
        new_key_slots[j] = key;
      }

      new_key_slots[position] = key;
      new_payload_slots[position] = payload;
      set_bit(new_bitmap, position);

      last_position = position;

      keys_remaining--;
    }

    for (int i = last_position + 1; i < new_data_capacity; i++) {
      new_key_slots[i] = kEndSentinel_;
    }
  }

  // Resize the data node to the target density
  // For multithreading : makes new node with resized data node.
  void resize(double target_density, bool force_retrain) {
#if PROFILE
    profileStats.resize_call_cnt++;
    auto resize_start_time = std::chrono::high_resolution_clock::now();
#endif
    //we first obtain total number of keys in new data array
    int last_delta_num_keys = 0;
    if (child_just_splitted_) {
      if (was_left_child_) {
        const_iterator_type it(this, 0, true);
        while (!it.is_end() && it.cur_idx_ < boundary_base_key_idx_) {
          it++;
          last_delta_num_keys++;
        }
      }
      else {
        const_iterator_type it(this, boundary_base_key_idx_, true);
        while (!it.is_end()) {
          it++;
          last_delta_num_keys++;
        }
      }
    }
    else {
      last_delta_num_keys = node_status_ == INSERT_AT_DELTA ? 0 : delta_num_keys_;
    }

    int total_num_keys = last_delta_num_keys + num_keys_;
    if (total_num_keys == 0) {
      return;
    }

    int new_data_capacity =
        std::max(static_cast<int>(total_num_keys / target_density), 
                                  total_num_keys + 1);
    auto new_bitmap_size =
        static_cast<size_t>(std::ceil(new_data_capacity / 64.));
    auto new_bitmap = new (bitmap_allocator().allocate(new_bitmap_size))
        uint64_t[new_bitmap_size]();  // initialize to all false
    AlexKey<T>* new_key_slots =
        new AlexKey<T>[new_data_capacity]();
    P* new_payload_slots = new (payload_allocator().allocate(new_data_capacity))
        P[new_data_capacity];
    LinearModel<T> new_model(this->model_.a_, this->model_.b_);

    // Retrain model if the number of keys is sufficiently small (under 50)
    if (num_keys_ < 50 || force_retrain) {
      const_iterator_type it(this, 0);
      LinearModelBuilder<T> builder(&(new_model));
      for (int i = 0; it.cur_idx_ < data_capacity_ && !it.is_end(); it++, i++) {
        builder.add(it.key(), i);
      }
      builder.build();
      new_model.expand(static_cast<double>(new_data_capacity) / num_keys_);
    }
    else {
      new_model.expand(static_cast<double>(new_data_capacity) / data_capacity_);
    }

    int keys_remaining = total_num_keys;
    int delta_start_idx;
    if (child_just_splitted_ && was_right_child_) {delta_start_idx = boundary_base_key_idx_;}
    else {delta_start_idx = 0;}

    const_iterator_type it(this, 0);
    if (node_status_ == INSERT_AT_DELTA) {
      const_iterator_type delta_it(this);
      delta_it.cur_idx_ = -1;
      resize_insert(new_payload_slots, new_bitmap, new_key_slots, new_model,
                    keys_remaining, new_data_capacity, it, delta_it);
    }
    else if (node_status_ == INSERT_AT_TMPDELTA) {
      const_iterator_type delta_it(this, delta_start_idx, true);
      resize_insert(new_payload_slots, new_bitmap, new_key_slots, new_model,
                    keys_remaining, new_data_capacity, it, delta_it);
    }
    else {std::cout << "error on resize" << std::endl; abort();} //shouldn't happen

    auto old_key_slots = key_slots_;
    auto old_payload_slots = payload_slots_;
    auto old_bitmap = bitmap_;
    auto old_data_capacity = data_capacity_;
    auto old_bitmap_size = bitmap_size_;

    pthread_rwlock_wrlock(&key_array_rw_lock_); //since it's now altering data node itself.
    num_keys_ = total_num_keys;
    data_capacity_ = new_data_capacity;
    bitmap_size_ = new_bitmap_size;
    key_slots_ = new_key_slots;
    payload_slots_ = new_payload_slots;
    bitmap_ = new_bitmap;

    expansion_threshold_ =
        std::min(std::max(data_capacity_ * kMaxDensity_,
                          static_cast<double>(num_keys_ + 1)),
                 static_cast<double>(data_capacity_));
    contraction_threshold_ = data_capacity_ * kMinDensity_;
    this->model_ = new_model;
    pthread_rwlock_unlock(&key_array_rw_lock_);

    delete[] old_key_slots;
    payload_allocator().deallocate(old_payload_slots, old_data_capacity);
    bitmap_allocator().deallocate(old_bitmap, old_bitmap_size);

#if PROFILE
    auto resize_end_time = std::chrono::high_resolution_clock::now();
    auto elapsed_time = std::chrono::duration_cast<std::chrono::fgTimeUnit>(resize_end_time - resize_start_time).count();
    profileStats.resize_time += elapsed_time;
    profileStats.max_resize_time = 
      std::max(profileStats.max_resize_time.load(), elapsed_time);
    profileStats.min_resize_time =
      std::min(profileStats.min_resize_time.load(), elapsed_time);
#endif
  }


  // Insert key into pos. The caller must guarantee that pos is a gap.
  // mode 0 : rw_lock already obtained, no need for another write wait (for insert_using_shifts)
  // mode 1 : rw_lock not obtained, need to do write wait (for other use cases)
  void insert_element_at(const AlexKey<T>& key, P payload, int pos, 
                         uint64_t worker_id, int mode = 0, int node_status = 0) {
#if PROFILE
    auto insert_element_at_start_time = std::chrono::high_resolution_clock::now();
#endif
    AlexKey<T> *ref_key_arr;
    P* ref_payload_arr;
    pthread_rwlock_t *ref_rwlock;
    uint64_t *ref_bitmap;
    switch(node_status) {
      case INSERT_AT_DELTA:
        ref_key_arr = delta_idx_;
        ref_payload_arr = delta_idx_payloads_;
        ref_rwlock = &delta_index_rw_lock_;
        ref_bitmap = delta_bitmap_;
        break;
      case INSERT_AT_TMPDELTA:
        ref_key_arr = tmp_delta_idx_;
        ref_payload_arr = tmp_delta_idx_payloads_;
        ref_rwlock = &tmp_delta_index_rw_lock_;
        ref_bitmap = tmp_delta_bitmap_;
        break;
      default:
        ref_key_arr = key_slots_;
        ref_payload_arr = payload_slots_;
        ref_rwlock = &key_array_rw_lock_;
        ref_bitmap = bitmap_;
    }
    if (mode == 1) {
#if PROFILE
      profileStats.insert_element_at_call_cnt[worker_id]++;
#endif
      pthread_rwlock_wrlock(ref_rwlock); //synchronization
    }
    ref_key_arr[pos] = key;
    ref_payload_arr[pos] = payload;
    set_bit(ref_bitmap, pos);

    // Overwrite preceding gaps until we reach the previous element
    pos--;
    while (pos >= 0 && !check_exists(pos, node_status)) {
      ref_key_arr[pos] = key;
      pos--;
    }
    if (mode == 1) {
      pthread_rwlock_unlock(ref_rwlock);
#if PROFILE
      auto insert_element_at_end_time = std::chrono::high_resolution_clock::now();
      auto elapsed_time = std::chrono::duration_cast<std::chrono::fgTimeUnit>(insert_element_at_end_time - insert_element_at_start_time).count();
      profileStats.insert_element_at_time[worker_id] += elapsed_time;
      profileStats.max_insert_element_at_time[worker_id] =
        std::max(profileStats.max_insert_element_at_time[worker_id], elapsed_time);
      profileStats.min_insert_element_at_time[worker_id] =
        std::min(profileStats.min_insert_element_at_time[worker_id], elapsed_time);
#endif
    }
  }

  // Insert key into pos, shifting as necessary in the range [left, right)
  // Returns the actual position of insertion
  int insert_using_shifts(const AlexKey<T>& key, P payload, int pos, 
                          uint64_t worker_id, int node_status = 0) {
    // Find the closest gap
#if PROFILE
    profileStats.insert_using_shifts_call_cnt[worker_id]++;
    auto insert_using_shifts_start_time = std::chrono::high_resolution_clock::now();
#endif
    AlexKey<T> *ref_key_arr;
    P* ref_payload_arr;
    int ref_capacity;
    pthread_rwlock_t *ref_rwlock;
    uint64_t *ref_bitmap;
    int ref_bitmap_size;
    switch(node_status) {
      case INSERT_AT_DELTA:
        ref_key_arr = delta_idx_;
        ref_payload_arr = delta_idx_payloads_;
        ref_capacity = delta_idx_capacity_;
        ref_rwlock = &delta_index_rw_lock_;
        ref_bitmap = delta_bitmap_;
        ref_bitmap_size = delta_bitmap_size_;
        break;
      case INSERT_AT_TMPDELTA:
        ref_key_arr = tmp_delta_idx_;
        ref_payload_arr = tmp_delta_idx_payloads_;
        ref_capacity = tmp_delta_idx_capacity_;
        ref_rwlock = &tmp_delta_index_rw_lock_;
        ref_bitmap = tmp_delta_bitmap_;
        ref_bitmap_size = tmp_delta_bitmap_size_;
        break;
      default:
        ref_key_arr = key_slots_;
        ref_payload_arr = payload_slots_;
        ref_capacity = data_capacity_;
        ref_rwlock = &key_array_rw_lock_;
        ref_bitmap = bitmap_;
        ref_bitmap_size = bitmap_size_;
    }
    int gap_pos = closest_gap(pos, ref_capacity, ref_bitmap, ref_bitmap_size);
    set_bit(ref_bitmap, gap_pos);
    pthread_rwlock_wrlock(ref_rwlock); //for synchronization.
    if (gap_pos >= pos) {
      for (int i = gap_pos; i > pos; i--) {
        ref_key_arr[i] = ref_key_arr[i-1];
        ref_payload_arr[i] = ref_payload_arr[i-1];
      }
      insert_element_at(key, payload, pos, worker_id, 0, node_status);
      pthread_rwlock_unlock(ref_rwlock);
      num_shifts_ += gap_pos - pos;
#if PROFILE
      auto insert_using_shifts_end_time = std::chrono::high_resolution_clock::now();
      auto elapsed_time = std::chrono::duration_cast<std::chrono::fgTimeUnit>(insert_using_shifts_end_time - insert_using_shifts_start_time).count();
      profileStats.insert_using_shifts_time[worker_id] += elapsed_time;
      profileStats.max_insert_using_shifts_time[worker_id] =
        std::max(profileStats.max_insert_using_shifts_time[worker_id], elapsed_time);
      profileStats.min_insert_using_shifts_time[worker_id] =
        std::min(profileStats.min_insert_using_shifts_time[worker_id], elapsed_time);
#endif
      return pos;
    } else {
      for (int i = gap_pos; i < pos - 1; i++) {
        if (ref_key_arr[i+1].key_arr_ == nullptr) {
          std::cout << "node status : " << node_status << std::endl;
        }
        ref_key_arr[i] = ref_key_arr[i+1];
        ref_payload_arr[i] = ref_payload_arr[i+1];
      }
      insert_element_at(key, payload, pos - 1, worker_id, 0, node_status);
      pthread_rwlock_unlock(ref_rwlock);
      num_shifts_ += pos - gap_pos - 1;
#if PROFILE
      auto insert_using_shifts_end_time = std::chrono::high_resolution_clock::now();
      auto elapsed_time = std::chrono::duration_cast<std::chrono::fgTimeUnit>(insert_using_shifts_end_time - insert_using_shifts_start_time).count();
      profileStats.insert_using_shifts_time[worker_id] += elapsed_time;
      profileStats.max_insert_using_shifts_time[worker_id] =
        std::max(profileStats.max_insert_using_shifts_time[worker_id], elapsed_time);
      profileStats.min_insert_using_shifts_time[worker_id] =
        std::min(profileStats.min_insert_using_shifts_time[worker_id], elapsed_time);
#endif
      return pos - 1;
    }
  }

#if ALEX_USE_LZCNT
  // Returns position of closest gap to pos
  // Returns pos if pos is a gap
  int closest_gap(int pos) {
    return closest_gap(pos, data_capacity_, bitmap_, bitmap_size_);
  }

  int closest_gap(int pos, int ref_capacity, uint64_t *ref_bitmap, int ref_bitmap_size) const {
    pos = std::min(pos, ref_capacity - 1);
    int bitmap_pos = pos >> 6;
    int bit_pos = pos - (bitmap_pos << 6);
    if (ref_bitmap[bitmap_pos] == static_cast<uint64_t>(-1) ||
        (bitmap_pos == ref_bitmap_size - 1 &&
         _mm_popcnt_u64(ref_bitmap[bitmap_pos]) ==
             ref_capacity - ((ref_bitmap_size - 1) << 6))) {
      // no gaps in this block of 64 positions, start searching in adjacent
      // blocks
      int left_bitmap_pos = 0;
      int right_bitmap_pos = ((ref_capacity - 1) >> 6);  // inclusive
      int max_left_bitmap_offset = bitmap_pos - left_bitmap_pos;
      int max_right_bitmap_offset = right_bitmap_pos - bitmap_pos;
      int max_bidirectional_bitmap_offset =
          std::min<int>(max_left_bitmap_offset, max_right_bitmap_offset);
      int bitmap_distance = 1;
      while (bitmap_distance <= max_bidirectional_bitmap_offset) {
        uint64_t left_bitmap_data = ref_bitmap[bitmap_pos - bitmap_distance];
        uint64_t right_bitmap_data = ref_bitmap[bitmap_pos + bitmap_distance];
        if (left_bitmap_data != static_cast<uint64_t>(-1) &&
            right_bitmap_data != static_cast<uint64_t>(-1)) {
          int left_gap_pos = ((bitmap_pos - bitmap_distance + 1) << 6) -
                             static_cast<int>(_lzcnt_u64(~left_bitmap_data)) -
                             1;
          int right_gap_pos = ((bitmap_pos + bitmap_distance) << 6) +
                              static_cast<int>(_tzcnt_u64(~right_bitmap_data));
          if (pos - left_gap_pos <= right_gap_pos - pos ||
              right_gap_pos >= ref_capacity) {
            return left_gap_pos;
          } else {
            return right_gap_pos;
          }
        } else if (left_bitmap_data != static_cast<uint64_t>(-1)) {
          int left_gap_pos = ((bitmap_pos - bitmap_distance + 1) << 6) -
                             static_cast<int>(_lzcnt_u64(~left_bitmap_data)) -
                             1;
          // also need to check next block to the right
          if (bit_pos > 32 && bitmap_pos + bitmap_distance + 1 < ref_bitmap_size &&
              ref_bitmap[bitmap_pos + bitmap_distance + 1] !=
                  static_cast<uint64_t>(-1)) {
            int right_gap_pos =
                ((bitmap_pos + bitmap_distance + 1) << 6) +
                static_cast<int>(
                    _tzcnt_u64(~ref_bitmap[bitmap_pos + bitmap_distance + 1]));
            if (pos - left_gap_pos <= right_gap_pos - pos ||
                right_gap_pos >= ref_capacity) {
              return left_gap_pos;
            } else {
              return right_gap_pos;
            }
          } else {
            return left_gap_pos;
          }
        } else if (right_bitmap_data != static_cast<uint64_t>(-1)) {
          int right_gap_pos = ((bitmap_pos + bitmap_distance) << 6) +
                              static_cast<int>(_tzcnt_u64(~right_bitmap_data));
          if (right_gap_pos < ref_capacity) {
            // also need to check next block to the left
            if (bit_pos < 32 && bitmap_pos - bitmap_distance > 0 &&
                ref_bitmap[bitmap_pos - bitmap_distance - 1] !=
                    static_cast<uint64_t>(-1)) {
              int left_gap_pos =
                  ((bitmap_pos - bitmap_distance) << 6) -
                  static_cast<int>(
                      _lzcnt_u64(~ref_bitmap[bitmap_pos - bitmap_distance - 1])) -
                  1;
              if (pos - left_gap_pos <= right_gap_pos - pos ||
                  right_gap_pos >= ref_capacity) {
                return left_gap_pos;
              } else {
                return right_gap_pos;
              }
            } else {
              return right_gap_pos;
            }
          }
        }
        bitmap_distance++;
      }
      if (max_left_bitmap_offset > max_right_bitmap_offset) {
        for (int i = bitmap_pos - bitmap_distance; i >= left_bitmap_pos; i--) {
          if (ref_bitmap[i] != static_cast<uint64_t>(-1)) {
            return ((i + 1) << 6) - static_cast<int>(_lzcnt_u64(~ref_bitmap[i])) -
                   1;
          }
        }
      } else {
        for (int i = bitmap_pos + bitmap_distance; i <= right_bitmap_pos; i++) {
          if (ref_bitmap[i] != static_cast<uint64_t>(-1)) {
            int right_gap_pos =
                (i << 6) + static_cast<int>(_tzcnt_u64(~ref_bitmap[i]));
            if (right_gap_pos >= ref_capacity) {
              return -1;
            } else {
              return right_gap_pos;
            }
          }
        }
      }
      return -1;
    } else {
      // search within block of 64 positions
      uint64_t bitmap_data = ref_bitmap[bitmap_pos];
      int closest_right_gap_distance = 64;
      int closest_left_gap_distance = 64;
      // Logically gaps to the right of pos, in the bitmap these are gaps to the
      // left of pos's bit
      // This covers the case where pos is a gap
      // For example, if pos is 3, then bitmap '10101101' -> bitmap_right_gaps
      // '01010000'
      uint64_t bitmap_right_gaps = ~(bitmap_data | ((1ULL << bit_pos) - 1));
      if (bitmap_right_gaps != 0) {
        closest_right_gap_distance =
            static_cast<int>(_tzcnt_u64(bitmap_right_gaps)) - bit_pos;
      } else if (bitmap_pos + 1 < ref_bitmap_size) {
        // look in the next block to the right
        closest_right_gap_distance =
            64 + static_cast<int>(_tzcnt_u64(~ref_bitmap[bitmap_pos + 1])) -
            bit_pos;
      }
      // Logically gaps to the left of pos, in the bitmap these are gaps to the
      // right of pos's bit
      // For example, if pos is 3, then bitmap '10101101' -> bitmap_left_gaps
      // '00000010'
      uint64_t bitmap_left_gaps = (~bitmap_data) & ((1ULL << bit_pos) - 1);
      if (bitmap_left_gaps != 0) {
        closest_left_gap_distance =
            bit_pos - (63 - static_cast<int>(_lzcnt_u64(bitmap_left_gaps)));
      } else if (bitmap_pos > 0) {
        // look in the next block to the left
        closest_left_gap_distance =
            bit_pos + static_cast<int>(_lzcnt_u64(~ref_bitmap[bitmap_pos - 1])) +
            1;
      }

      if (closest_right_gap_distance < closest_left_gap_distance &&
          pos + closest_right_gap_distance < ref_capacity) {
        return pos + closest_right_gap_distance;
      } else {
        return pos - closest_left_gap_distance;
      }
    }
  }
#else
  // A slower version of closest_gap that does not use lzcnt and tzcnt
  // Does not return pos if pos is a gap
  int closest_gap(int pos) const {
    int max_left_offset = pos;
    int max_right_offset = data_capacity_ - pos - 1;
    int max_bidirectional_offset =
        std::min<int>(max_left_offset, max_right_offset);
    int distance = 1;
    while (distance <= max_bidirectional_offset) {
      if (!check_exists(pos - distance)) {
        return pos - distance;
      }
      if (!check_exists(pos + distance)) {
        return pos + distance;
      }
      distance++;
    }
    if (max_left_offset > max_right_offset) {
      for (int i = pos - distance; i >= 0; i--) {
        if (!check_exists(i)) return i;
      }
    } else {
      for (int i = pos + distance; i < data_capacity_; i++) {
        if (!check_exists(i)) return i;
      }
    }
    return -1;
  }
#endif

  /*** Stats ***/

  // Total size of node metadata
  long long node_size() const override { return sizeof(self_type); }

  // Total size in bytes of key/payload/data_slots and bitmap
  // NOTE THAT IT DOESN'T INCLUDE ALEX KEY'S POINTING ARRAY SIZE.
  long long data_size() const {
    long long data_size = data_capacity_ * sizeof(AlexKey<T>);
    data_size += data_capacity_ * sizeof(P);
    data_size += bitmap_size_ * sizeof(uint64_t);
    return data_size;
  }

  // Number of contiguous blocks of keys without gaps
  int num_packed_regions() const {
    int num_packed = 0;
    bool is_packed = check_exists(0);
    for (int i = 1; i < data_capacity_; i++) {
      if (check_exists(i) != is_packed) {
        if (is_packed) {
          num_packed++;
        }
        is_packed = !is_packed;
      }
    }
    if (is_packed) {
      num_packed++;
    }
    return num_packed;
  }

  // Check that a key exists in the key/data_slots
  // If validate_bitmap is true, confirm that the corresponding position in the
  // bitmap is correctly set to 1
  bool key_exists(const AlexKey<T>& key, bool validate_bitmap) const {
    for (int i = 0; i < data_capacity_ - 1; i++) {
      if (key_equal(ALEX_DATA_NODE_KEY_AT(i), key) &&
          (!validate_bitmap || check_exists(i))) {
        return true;
      }
    }
    return false;
  }

  std::string to_string() const {
    std::string str;
    str += "Num keys: " + std::to_string(num_keys_) + ", Capacity: " +
           std::to_string(data_capacity_) + ", Expansion Threshold: " +
           std::to_string(expansion_threshold_) + "\n";
    for (int i = 0; i < data_capacity_; i++) {
      AlexKey<T> cur_key = ALEX_DATA_NODE_KEY_AT(i);
      for (int j = 0; j < max_key_length_; j++) {
        str += (std::to_string(cur_key.key_arr_[j]) + " ");
      }
      str += "\n";
    }
    return str;
  }

  int erase_one(const AlexKey<T>& key) {
    int pos = find_lower(key);

    if (pos == data_capacity_ || !key_equal(ALEX_DATA_NODE_KEY_AT(pos), key))
      return 0;

    // Erase key at pos
    erase_one_at(pos);
    return 1;
  }

  void erase_one_at(int pos) {
    AlexKey<T> next_key;
    if (pos == data_capacity_ - 1) {
      next_key = kEndSentinel_;
    } else {
      next_key = ALEX_DATA_NODE_KEY_AT(pos + 1);
    }
    //delete ALEX_DATA_NODE_KEY_AT(pos);
    ALEX_DATA_NODE_KEY_AT(pos) = next_key;
    unset_bit(pos);
    pos--;

    // Erase preceding gaps until we reach an existing key
    while (pos >= 0 && !check_exists(pos)) {
      //delete ALEX_DATA_NODE_KEY_AT(pos);
      ALEX_DATA_NODE_KEY_AT(pos) = next_key;
      pos--;
    }

    num_keys_--;
  }
};

/* For use of alex
 * Save the traversal path down the RMI by having a linked list of these
 * structs. */
template<class T, class P>
struct TraversalNode {
  AlexModelNode<T,P>* node = nullptr;
  int bucketID = -1;
};
}