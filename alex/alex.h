// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

/*
 * ALEX with key type T and payload type P, combined type V=std::pair<T, P>.
 * Iterating through keys is done using an "Iterator".
 * Iterating through tree nodes is done using a "NodeIterator".
 *
 * Core user-facing API of Alex:
 * - Alex()
 * - void bulk_load(V values[], int num_keys)
 * - void insert(T key, P payload)
 * - Iterator begin()
 * - Iterator end()
 * - Iterator lower_bound(T key)
 * - Iterator upper_bound(T key)
 *
 * User-facing API of Iterator:
 * - void operator ++ ()  // post increment
 * - V operator * ()  // does not return reference to V by default
 * - const T& key ()
 * - P& payload ()
 * - bool is_end()
 * - bool operator == (const Iterator & rhs)
 * - bool operator != (const Iterator & rhs)
 */

#pragma once

#include <fstream>
#include <iostream>
#include <stack>
#include <type_traits>
#include <iomanip> //only for printing some debugging message.

#include "alex_base.h"
#include "alex_fanout_tree.h"
#include "alex_nodes.h"

// Whether we account for floating-point precision issues when traversing down
// ALEX.
// These issues rarely occur in practice but can cause incorrect behavior.
// Turning this on will cause slight performance overhead due to extra
// computation and possibly accessing two data nodes to perform a lookup.
#define ALEX_SAFE_LOOKUP 1

namespace alex {

template <class T, class P, class Compare = AlexCompare,
          class Alloc = std::allocator<std::pair<AlexKey<T>, P>>,
          bool allow_duplicates = true>
class Alex {
  static_assert(std::is_arithmetic<T>::value, "ALEX key type must be numeric.");
  static_assert(std::is_same<Compare, AlexCompare>::value,
                "Must use AlexCompare.");

 public:
  // Value type, returned by dereferencing an iterator
  typedef std::pair<AlexKey<T>, P> V;

  // ALEX class aliases
  typedef Alex<T, P, Compare, Alloc, allow_duplicates> self_type;
  typedef AlexNode<T, P, Alloc> node_type;
  typedef AlexModelNode<T, P, Alloc> model_node_type;
  typedef AlexDataNode<T, P, Compare, Alloc, allow_duplicates> data_node_type;

  // Forward declaration for iterators
  class Iterator;
  class ConstIterator;
  class ReverseIterator;
  class ConstReverseIterator;
  class NodeIterator;  // Iterates through all nodes with pre-order traversal

  node_type* root_node_ = nullptr;
  model_node_type* superroot_ =
      nullptr;  // phantom node that is the root's parent

  /* User-changeable parameters */
  struct Params {
    // When bulk loading, Alex can use provided knowledge of the expected
    // fraction of operations that will be inserts
    // For simplicity, operations are either point lookups ("reads") or inserts
    // ("writes)
    // i.e., 0 means we expect a read-only workload, 1 means write-only
    double expected_insert_frac = 0;
    // Maximum node size, in bytes. By default, 16MB.
    // Higher values result in better average throughput, but worse tail/max
    // insert latency
    int max_node_size = 1 << 24;
    // Approximate model computation: bulk load faster by using sampling to
    // train models
    bool approximate_model_computation = true;
    // Approximate cost computation: bulk load faster by using sampling to
    // compute cost
    bool approximate_cost_computation = false;
  };
  Params params_;

  /* Setting max node size automatically changes these parameters */
  struct DerivedParams {
    // The defaults here assume the default max node size of 16MB
    int max_fanout = 1 << 21;  // assumes 8-byte pointers
    int max_data_node_slots = (1 << 24) / sizeof(V);
  };
  DerivedParams derived_params_;

  int expected_min_numkey_per_data_node_ = node_size_const;

  /* Counters, useful for benchmarking and profiling */
  std::atomic<int> num_keys;

 
  /* Structs used internally */
  /* Statistics related to the key domain.
   * The index can hold keys outside the domain, but lookups/inserts on those
   * keys will be inefficient.
   * If enough keys fall outside the key domain, then we expand the key domain.
   */
  struct InternalStats {
    T *key_domain_min_ = nullptr; // we need to initialize this for every initializer
    T *key_domain_max_ = nullptr; // we need to initialize this for every initializer
  };
  InternalStats istats_;

  /* Used when finding the best way to propagate up the RMI when splitting
   * upwards.
   * Cost is in terms of additional model size created through splitting
   * upwards, measured in units of pointers.
   * One instance of this struct is created for each node on the traversal path.
   * User should take into account the cost of metadata for new model nodes
   * (base_cost). */
  struct SplitDecisionCosts {
    static constexpr double base_cost =
        static_cast<double>(sizeof(model_node_type)) / sizeof(void*);
    // Additional cost due to this node if propagation stops at this node.
    // Equal to 0 if redundant slot exists, otherwise number of new pointers due
    // to node expansion.
    double stop_cost = 0;
    // Additional cost due to this node if propagation continues past this node.
    // Equal to number of new pointers due to node splitting, plus size of
    // metadata of new model node.
    double split_cost = 0;
  };

  // At least this many keys must be outside the domain before a domain
  // expansion is triggered.
  static const int kMinOutOfDomainKeys = 5;
  // After this many keys are outside the domain, a domain expansion must be
  // triggered.
  static const int kMaxOutOfDomainKeys = 1000;
  // When the number of max out-of-domain (OOD) keys is between the min and
  // max, expand the domain if the number of OOD keys is greater than the
  // expected number of OOD due to randomness by greater than the tolereance
  // factor.
  static const int kOutOfDomainToleranceFactor = 2;

  Compare key_less_ = Compare();
  Alloc allocator_ = Alloc();

  /*** Constructors and setters ***/

 public:
 /* basic initialization can handle up to 4 parameters
  * 1) max key length of each keys. default value is 1. 
  * 3) compare function used for comparing. Default is basic AlexCompare
  * 4) allocation function used for allocation. Default is basic allocator. */
  Alex() {
    // key_domain setup
    std::atomic_init(&num_keys, 0);
    istats_.key_domain_min_ = new T[max_key_length_];
    istats_.key_domain_max_ = new T[max_key_length_];
    std::fill(istats_.key_domain_min_, istats_.key_domain_min_ + max_key_length_,
        STR_VAL_MIN);
    std::fill(istats_.key_domain_max_, istats_.key_domain_max_ + max_key_length_, STR_VAL_MAX);
    
    // Set up root as empty data node
    auto empty_data_node = new (data_node_allocator().allocate(1))
        data_node_type(nullptr, key_less_, allocator_);
    empty_data_node->bulk_load(nullptr, 0, expected_min_numkey_per_data_node_);
    root_node_ = empty_data_node;
    create_superroot();
  }

  Alex(const Compare& comp, const Alloc& alloc = Alloc())
      : key_less_(comp), allocator_(alloc) {
    std::atomic_init(&num_keys, 0);
    // key_domain setup
    istats_.key_domain_min_ = new T[max_key_length_];
    istats_.key_domain_max_ = new T[max_key_length_];
    std::fill(istats_.key_domain_min_, istats_.key_domain_min_ + max_key_length_,
        STR_VAL_MIN);
    std::fill(istats_.key_domain_max_, istats_.key_domain_max_ + max_key_length_, STR_VAL_MAX);

    // Set up root as empty data node
    auto empty_data_node = new (data_node_allocator().allocate(1))
        data_node_type(nullptr, key_less_, allocator_);
    empty_data_node->bulk_load(nullptr, 0, expected_min_numkey_per_data_node_);
    root_node_ = empty_data_node;
    create_superroot();
  }

  Alex(const Alloc& alloc) : allocator_(alloc) {
    std::atomic_init(&num_keys, 0);
    // key_domain setup
    istats_.key_domain_min_ = new T[max_key_length_];
    istats_.key_domain_max_ = new T[max_key_length_];
    std::fill(istats_.key_domain_min_, istats_.key_domain_min_ + max_key_length_,
        STR_VAL_MIN);
    std::fill(istats_.key_domain_max_, istats_.key_domain_max_ + max_key_length_, STR_VAL_MAX);

    // Set up root as empty data node
    auto empty_data_node = new (data_node_allocator().allocate(1))
        data_node_type(nullptr, key_less_, allocator_);
    empty_data_node->bulk_load(nullptr, 0, expected_min_numkey_per_data_node_);
    root_node_ = empty_data_node;
    create_superroot();
  }

  //NOTE : destruction should be done when multithreading
  ~Alex() {
    for (NodeIterator node_it = NodeIterator(this); !node_it.is_end();
         node_it.next()) {
      delete_node(node_it.current());
    }
    delete_node(superroot_);
    delete[] istats_.key_domain_min_;
    delete[] istats_.key_domain_max_;
  }

  // Below 4 constructors initializes with range [first, last). 
  // The range does not need to be sorted. 
  // This creates a temporary copy of the data. 
  // If possible, we recommend directly using bulk_load() instead.
  // NEED FIX (max_key_length issue, not urgent
  //           possible but not implemented since it's not used yet.)
  template <class InputIterator>
  explicit Alex(InputIterator first, InputIterator last,
                const Compare& comp = Compare(), const Alloc& alloc = Alloc())
      : key_less_(comp), allocator_(alloc) {
    std::atomic_init(&num_keys, 0);
    // key_domain setup
    istats_.key_domain_min_ = new T[max_key_length_];
    istats_.key_domain_max_ = new T[max_key_length_];
    std::fill(istats_.key_domain_min_, istats_.key_domain_min_ + max_key_length_,
        STR_VAL_MIN);
    std::fill(istats_.key_domain_max_, istats_.key_domain_max_ + max_key_length_, STR_VAL_MAX);

    std::vector<V> values;
    for (auto it = first; it != last; ++it) {
      values.push_back(*it);
    }
    std::sort(values.begin(), values.end(),
            [this](auto const& a, auto const& b) {return a.first < b.first;});
    bulk_load(values.data(), static_cast<int>(values.size()));
  }

  template <class InputIterator>
  explicit Alex(InputIterator first, InputIterator last,
                const Alloc& alloc = Alloc())
      : allocator_(alloc) {
    std::atomic_init(&num_keys, 0);
    // key_domain setup
    istats_.key_domain_min_ = new T[max_key_length_];
    istats_.key_domain_max_ = new T[max_key_length_];
    std::fill(istats_.key_domain_min_, istats_.key_domain_min_ + max_key_length_,
        STR_VAL_MIN);
    std::fill(istats_.key_domain_max_, istats_.key_domain_max_ + max_key_length_, STR_VAL_MAX);

    std::vector<V> values;
    for (auto it = first; it != last; ++it) {
      values.push_back(*it);
    }
    std::sort(values.begin(), values.end(),
            [this](auto const& a, auto const& b) {return a.first < b.first;});
    bulk_load(values.data(), static_cast<int>(values.size()));
  }

  //IF YOUT WANT TO USE BELOW THREE FUNCTIONS IN MULTITHREAD ALEX,
  //PLEASE CHECK IF NO THREAD IS OPERAING FOR ALEX THAT'S BEING COPIED.
  explicit Alex(const self_type& other)
      : params_(other.params_),
        derived_params_(other.derived_params_),
        istats_(other.istats_),
        key_less_(other.key_less_),
        allocator_(other.allocator_) {
    istats_.key_domain_min_ = new T[max_key_length_];
    istats_.key_domain_max_ = new T[max_key_length_];
    std::copy(other.istats_.key_domain_min_, other.istats_.key_domain_min_ + max_key_length_,
        istats_.key_domain_min_);
    std::copy(other.istats_.key_domain_max_, other.istats_.key_domain_max_ + max_key_length_,
        istats_.key_domain_max_);
    superroot_ =
        static_cast<model_node_type*>(copy_tree_recursive(other.superroot_));
    std::cout << "nodes: " << node_counter << std::endl;
    std::cout << "datas: " << leaf_counter << std::endl;
    root_node_ = superroot_->children_[0];
    //num_keys = other.num_keys;
  }

  Alex& operator=(const self_type& other) {
    if (this != &other) {
      for (NodeIterator node_it = NodeIterator(this); !node_it.is_end();
           node_it.next()) {
        delete_node(node_it.current());
      }
      delete_node(superroot_);
      delete[] istats_.key_domain_min_;
      delete[] istats_.key_domain_max_;
      params_ = other.params_;
      derived_params_ = other.derived_params_;
      istats_ = other.istats_;
      num_keys = other.num_keys;
      key_less_ = other.key_less_;
      allocator_ = other.allocator_;
      istats_.key_domain_min_ = new T[max_key_length_];
      istats_.key_domain_max_ = new T[max_key_length_];
      std::copy(other.istats_.key_domain_min_, other.istats_.key_domain_min_ + max_key_length_,
          istats_.key_domain_min_);
      std::copy(other.istats_.key_domain_max_, other.istats_.key_domain_max_ + max_key_length_,
          istats_.key_domain_max_);
      superroot_ =
          static_cast<model_node_type*>(copy_tree_recursive(other.superroot_));
      root_node_ = superroot_->children_[0];
    }
    return *this;
  }

  void swap(const self_type& other) {
    std::swap(params_, other.params_);
    std::swap(derived_params_, other.derived_params_);
    std::swap(key_less_, other.key_less_);
    std::swap(allocator_, other.allocator_);
    
    auto arb_num_keys = num_keys;
    num_keys = other.num_keys;
    other.num_keys = num_keys;

    std::swap(istats_.key_domain_min_, other.istats_.key_domain_min_);
    std::swap(istats_.key_domain_max_, other.istats_.key_domain_max_);
    std::swap(superroot_, other.superroot_);
    std::swap(root_node_, other.root_node_);
  }

  int leaf_counter = 0;
  int node_counter = 0;
  int key_count = 0;
 
  // Deep copy of tree starting at given node
  // ALEX SHOULDN'T BE WORKED BY OTHER THREADS IN THIS CASE.
  node_type* copy_tree_recursive(const node_type* node) {
    if (!node) return nullptr;
    if (node->is_leaf_) {
      leaf_counter++;

      key_count += node->node_size();
      return nullptr;
      //return new (data_node_allocator().allocate(1))
      //    data_node_type(*static_cast<const data_node_type*>(node));
    } else {
      // auto node_copy = new (model_node_allocator().allocate(1))

      node_counter++;
      //     model_node_type(*static_cast<const model_node_type*>(node));
      int cur = 0;
      while (cur < static_cast<const model_node_type*>(node)->num_children_) {
        node_type* child_node = static_cast<const model_node_type*>(node)->children_[cur];
        node_type* child_node_copy = copy_tree_recursive(child_node);
        int repeats = 1;// << child_node_copy->duplication_factor_;
        // for (int i = cur; i < cur + repeats; i++) {
        //   node->children_[i] = child_node_copy;
        // }
        cur += repeats;
      }
      return nullptr;
    }
  }

 public:
  // When bulk loading, Alex can use provided knowledge of the expected fraction
  // of operations that will be inserts
  // For simplicity, operations are either point lookups ("reads") or inserts
  // ("writes)
  // i.e., 0 means we expect a read-only workload, 1 means write-only
  // This is only useful if you set it before bulk loading
  void set_expected_insert_frac(double expected_insert_frac) {
    assert(expected_insert_frac >= 0 && expected_insert_frac <= 1);
    params_.expected_insert_frac = expected_insert_frac;
  }

  // Maximum node size, in bytes.
  // Higher values result in better average throughput, but worse tail/max
  // insert latency.
  void set_max_node_size(int max_node_size) {
    assert(max_node_size >= sizeof(V));
    params_.max_node_size = max_node_size;
    derived_params_.max_fanout = params_.max_node_size / sizeof(void*);
    derived_params_.max_data_node_slots = params_.max_node_size / sizeof(V);
  }

  // Bulk load faster by using sampling to train models.
  // This is only useful if you set it before bulk loading.
  void set_approximate_model_computation(bool approximate_model_computation) {
    params_.approximate_model_computation = approximate_model_computation;
  }

  // Bulk load faster by using sampling to compute cost.
  // This is only useful if you set it before bulk loading.
  void set_approximate_cost_computation(bool approximate_cost_computation) {
    params_.approximate_cost_computation = approximate_cost_computation;
  }

  /*** General helpers ***/

 public:
#if ALEX_SAFE_LOOKUP
  forceinline data_node_type* get_leaf(
    AlexKey<T> key, const uint64_t worker_id,
    int mode = 1, std::vector<TraversalNode<T, P>>* traversal_path = nullptr) {
#if DEBUG_PRINT
      alex::coutLock.lock();
      std::cout << "t" << worker_id << " - ";
      std::cout << "traveling from superroot" << std::endl;
      alex::coutLock.unlock();
#endif
      return get_leaf_from_parent(key, worker_id, superroot_, mode, traversal_path);
  }

#endif
// Return the data node that contains the key (if it exists).
// Also optionally return the traversal path to the data node.
// traversal_path should be empty when calling this function.
// The returned traversal path begins with superroot and ends with the data
// node's parent.
// Mode 0 : It's for looking the existing key. It should check boundaries.
// Mode 1 : It's for inserting new key. It checks boundaries, but could extend it.
#if ALEX_SAFE_LOOKUP
  forceinline data_node_type* get_leaf_from_parent(
      AlexKey<T> key, const uint64_t worker_id, node_type *starting_parent,
      int mode = 1, std::vector<TraversalNode<T, P>>* traversal_path = nullptr) {
#if PROFILE
    auto get_leaf_from_parent_start_time = std::chrono::high_resolution_clock::now();
#endif
    node_type* cur = starting_parent == superroot_ ? root_node_ : starting_parent;
#if PROFILE
    if (mode == 0 && starting_parent == superroot_) {
      profileStats.get_leaf_from_get_payload_superroot_call_cnt[worker_id]++;
    }
    else if (mode == 0 && starting_parent != superroot_) {
      profileStats.get_leaf_from_get_payload_directp_call_cnt[worker_id]++;
    }
    else if (mode == 1 && starting_parent == superroot_) {
      profileStats.get_leaf_from_insert_superroot_call_cnt[worker_id]++;
    }
    else {
      profileStats.get_leaf_from_insert_directp_call_cnt[worker_id]++;
    }
#endif

    if (cur->is_leaf_) {
      //normally shouldn't happen, since normally starting node is always model node.
      //...unless it's root node
      if (traversal_path) {
        traversal_path->push_back({superroot_, 0});
      }
      return static_cast<data_node_type*>(cur);
    }

    while (true) {
      auto node = static_cast<model_node_type*>(cur);
      pthread_rwlock_rdlock(&(node->children_rw_lock_));
      node_type **cur_children = node->children_;
      int num_children = node->num_children_;
      double bucketID_prediction = node->model_.predict_double(key);
      int bucketID = static_cast<int>(bucketID_prediction);
      bucketID =
          std::min<int>(std::max<int>(bucketID, 0), num_children - 1);
      cur = cur_children[bucketID];
      int cur_duplication_factor = 1 << cur->duplication_factor_;
      bucketID = bucketID - (bucketID % cur_duplication_factor);
      AlexKey<T> *cur_pivot_key = &(cur->pivot_key_);
#if DEBUG_PRINT
      alex::coutLock.lock();
      std::cout << "t" << worker_id << " - ";
      std::cout << "initial bucket : " << bucketID << '\n';
      std::cout << "t" << worker_id << " - ";
      std::cout << "pivot_key_ : " << cur_pivot_key->key_arr_ << std::endl;
      alex::coutLock.unlock();
#endif
      bool smaller_than_pivot = key_less_(key, *(cur_pivot_key));
      while (smaller_than_pivot) {
        //keep going left.
        if (bucketID == 0) {return nullptr;} //error
        bucketID -= 1;
        cur = cur_children[bucketID];
        cur_duplication_factor = 1 << cur->duplication_factor_;
        bucketID = bucketID - (bucketID % cur_duplication_factor);
        cur_pivot_key = &(cur->pivot_key_);
        smaller_than_pivot = key_less_(key, *(cur_pivot_key));
      }

      bool larger_than_pivot = key_less_(*(cur_pivot_key), key);
      if (larger_than_pivot) {
        while (true) {
          int next_bucketID = bucketID + cur_duplication_factor;
          if (next_bucketID >= num_children) {break;}
          auto cur_next = cur_children[next_bucketID];
          int cur_next_duplication_factor = 1 << cur_next->duplication_factor_;
          AlexKey<T> *cur_next_pivot_key = &(cur_next->pivot_key_);
          if (key_less_(key, *(cur_next_pivot_key))) {break;}
          bucketID = next_bucketID;
          cur = cur_next;
          cur_duplication_factor = cur_next_duplication_factor;
        }
      }

      if (traversal_path) {
        traversal_path->push_back({node, bucketID});
      }

      pthread_rwlock_unlock(&(node->children_rw_lock_));

      if (cur->is_leaf_) {
        // we don't do rcu_progress here, since we are entering data node.
        // rcu_progress should be called at adequate point where the users finished using this data node.
        // If done ignorantly, it could cause null pointer access (because of destruction by other thread)
#if PROFILE
        auto get_leaf_from_parent_end_time = std::chrono::high_resolution_clock::now();
        auto elapsed_time = std::chrono::duration_cast<std::chrono::fgTimeUnit>(get_leaf_from_parent_end_time - get_leaf_from_parent_start_time).count();
        if (mode == 0 && starting_parent == superroot_) {
          profileStats.get_leaf_from_get_payload_superroot_time[worker_id] += elapsed_time;
          profileStats.max_get_leaf_from_get_payload_superroot_time[worker_id] =
            std::max(profileStats.max_get_leaf_from_get_payload_superroot_time[worker_id], elapsed_time);
          profileStats.min_get_leaf_from_get_payload_superroot_time[worker_id] =
            std::min(profileStats.max_get_leaf_from_get_payload_superroot_time[worker_id], elapsed_time);
        }
        else if (mode == 0 && starting_parent != superroot_) {
          profileStats.get_leaf_from_get_payload_directp_time[worker_id] += elapsed_time;
          profileStats.max_get_leaf_from_get_payload_directp_time[worker_id] =
            std::max(profileStats.max_get_leaf_from_get_payload_directp_time[worker_id], elapsed_time);
          profileStats.min_get_leaf_from_get_payload_directp_time[worker_id] =
            std::min(profileStats.min_get_leaf_from_get_payload_directp_time[worker_id], elapsed_time);
        }
        else if (starting_parent == superroot_) {
          profileStats.get_leaf_from_insert_superroot_time[worker_id] += elapsed_time;
          profileStats.max_get_leaf_from_insert_superroot_time[worker_id] =
            std::max(profileStats.max_get_leaf_from_insert_superroot_time[worker_id], elapsed_time);
          profileStats.min_get_leaf_from_insert_superroot_time[worker_id] =
            std::min(profileStats.max_get_leaf_from_insert_superroot_time[worker_id], elapsed_time);
        }
        else {
          profileStats.get_leaf_from_insert_directp_time[worker_id] += elapsed_time;
          profileStats.max_get_leaf_from_insert_directp_time[worker_id] =
            std::max(profileStats.max_get_leaf_from_insert_directp_time[worker_id], elapsed_time);
          profileStats.min_get_leaf_from_insert_directp_time[worker_id] =
            std::min(profileStats.min_get_leaf_from_insert_directp_time[worker_id], elapsed_time);
        }
#endif
        return (data_node_type *) cur;
      }
      //entering model node, need to progress
      //chosen model nodes are never destroyed, (without erase implementation, not used currently.)
      //Synchronization issue will be checked by another while loop.
      rcu_progress(worker_id);
    }
  }
#else
  data_node_type* get_leaf(
      AlexKey<T> key, std::vector<TraversalNode>* traversal_path = nullptr) const {
    return nullptr; //not implemented
  }
#endif

 
  // Honestly, can't understand why below 4 functions exists 
  // (first_data_node / last_data_node / get_min_key / get_max_key)
  // (since it's declared private and not used anywhere)
  // Return left-most data node
  data_node_type* first_data_node() const {
    node_type* cur = root_node_;

    while (!cur->is_leaf_) {
      cur = static_cast<model_node_type*>(cur)->children_[0];
    }
    return static_cast<data_node_type*>(cur);
  }

  // Return right-most data node
  data_node_type* last_data_node() const {
    node_type* cur = root_node_;

    while (!cur->is_leaf_) {
      auto node = static_cast<model_node_type*>(cur);
      cur = node->children_[node->num_children_ - 1];
    }
    return static_cast<data_node_type*>(cur);
  }

  // Returns minimum key in the index
  T *get_min_key() const { return first_data_node()->first_key(); }

  // Returns maximum key in the index
  T *get_max_key() const { return last_data_node()->last_key(); }

  // Link all data nodes together. Used after bulk loading.
  void link_all_data_nodes() {
    data_node_type* prev_leaf = nullptr;
    for (NodeIterator node_it = NodeIterator(this); !node_it.is_end();
         node_it.next()) {
      node_type* cur = node_it.current();
      if (cur->is_leaf_) {
        auto node = static_cast<data_node_type*>(cur);
        if (prev_leaf != nullptr) {
          prev_leaf->next_leaf_.val_ = node;
          node->prev_leaf_.val_ = prev_leaf;
        }
        prev_leaf = node;
      }
    }
  }

  // Link the new data nodes together when old data node is replaced by two new
  // data nodes.
  void link_data_nodes(data_node_type* old_leaf,
                       data_node_type* left_leaf, data_node_type* right_leaf) {
    data_node_type *old_leaf_prev_leaf = old_leaf->prev_leaf_.read();
    data_node_type *old_leaf_next_leaf = old_leaf->next_leaf_.read();
    if (old_leaf_prev_leaf != nullptr) {
      data_node_type *olpl_pending_rl = old_leaf_prev_leaf->pending_right_leaf_.read();
      if (olpl_pending_rl != nullptr) {
        olpl_pending_rl->next_leaf_.update(left_leaf);
        left_leaf->prev_leaf_.update(olpl_pending_rl);
      }
      else {
        old_leaf_prev_leaf->next_leaf_.update(left_leaf);
        left_leaf->prev_leaf_.update(old_leaf_prev_leaf);
      }
    }
    else {
      left_leaf->prev_leaf_.update(nullptr);
    }
    left_leaf->next_leaf_.update(right_leaf);
    right_leaf->prev_leaf_.update(left_leaf);
    if (old_leaf_next_leaf != nullptr) {
      data_node_type *olnl_pending_ll = old_leaf_next_leaf->pending_left_leaf_.read();
      if (olnl_pending_ll != nullptr) {
        olnl_pending_ll->prev_leaf_.update(right_leaf);
        right_leaf->next_leaf_.update(olnl_pending_ll);
      }
      else {
        old_leaf_next_leaf->prev_leaf_.update(right_leaf);
        right_leaf->next_leaf_.update(old_leaf_next_leaf);
      }
    }
    else {
      right_leaf->next_leaf_.update(nullptr);
    }
  }

  /*** Allocators and comparators ***/

 public:
  Alloc get_allocator() const { return allocator_; }

  Compare key_comp() const { return key_less_; }

 
  typename model_node_type::alloc_type model_node_allocator() {
    return typename model_node_type::alloc_type(allocator_);
  }

  typename data_node_type::alloc_type data_node_allocator() {
    return typename data_node_type::alloc_type(allocator_);
  }

  typename model_node_type::pointer_alloc_type pointer_allocator() {
    return typename model_node_type::pointer_alloc_type(allocator_);
  }

  void delete_node(node_type* node) {
    if (node == nullptr) {
      return;
    } else if (node->is_leaf_) {
      data_node_allocator().destroy(static_cast<data_node_type*>(node));
      data_node_allocator().deallocate(static_cast<data_node_type*>(node), 1);
    } else {
      model_node_allocator().destroy(static_cast<model_node_type*>(node));
      model_node_allocator().deallocate(static_cast<model_node_type*>(node), 1);
    }
  }

  // True if a == b
  template <class K>
  forceinline bool key_equal(const AlexKey<T>& a, const AlexKey<K>& b) const {
    return !key_less_(a, b) && !key_less_(b, a);
  }

  /*** Bulk loading ***/

 public:
  // values should be the sorted array of key-payload pairs.
  // The number of elements should be num_keys.
  // The index must be empty when calling this method.
  void bulk_load(const V values[], int num_keys) {
    if (this->num_keys > 0 || num_keys <= 0) {
      return;
    }
    delete_node(root_node_);  // delete the empty root node from constructor

    this->num_keys = num_keys;

    // Build temporary root model, which outputs a CDF in the range [0, 1]
    root_node_ =
        new (model_node_allocator().allocate(1)) model_node_type(0, nullptr, allocator_);
    AlexKey<T> min_key = values[0].first;
    AlexKey<T> max_key = values[num_keys - 1].first;

    if (true) { //for string key
      LinearModelBuilder<T> root_model_builder(&(root_node_->model_));
      for (int i = 0; i < num_keys; i++) {
#if DEBUG_PRINT
        //printf("adding : %f\n", (double) (i) / (num_keys-1));
#endif
        root_model_builder.add(values[i].first, (double) (i) / (num_keys-1));
      }
      root_model_builder.build();
    }
    else { //for numeric key
      std::cout << "Please use only string keys" << std::endl;
      abort();
    }
#if DEBUG_PRINT
    //for (int i = 0; i < num_keys; i++) {
    //  std::cout << "inserting " << values[i].first.key_arr_ << '\n';
    //}
    //std::cout << "left prediction result (bulk_load) " 
    //          << root_node_->model_.predict_double(values[1].first) 
    //          << std::endl;
    //std::cout << "right prediction result (bulk_load) " 
    //          << root_node_->model_.predict_double(values[num_keys-2].first) 
    //          << std::endl;
#endif

    // Compute cost of root node
    LinearModel<T> root_data_node_model;
    data_node_type::build_model(values, num_keys, &root_data_node_model,
                                params_.approximate_model_computation);
    DataNodeStats stats;
    root_node_->cost_ = data_node_type::compute_expected_cost(
        values, num_keys, data_node_type::kInitDensity_,
        params_.expected_insert_frac, &root_data_node_model,
        params_.approximate_cost_computation, &stats);

    // Recursively bulk load
    bulk_load_node(values, num_keys, root_node_, nullptr, num_keys,
                   &root_data_node_model);

    create_superroot();
    update_superroot_key_domain();
    link_all_data_nodes();

#if DEBUG_PRINT
    //std::cout << "structure's min_key after bln : " << istats_.key_domain_min_ << std::endl;
    //std::cout << "structure's max_key after bln : " << istats_.key_domain_max_ << std::endl;
#endif
  }

 
  // Only call this after creating a root node
  void create_superroot() {
    if (!root_node_) return;
    delete_node(superroot_);
    superroot_ = new (model_node_allocator().allocate(1))
        model_node_type(static_cast<short>(root_node_->level_ - 1), nullptr, allocator_);
    superroot_->num_children_ = 1;
    superroot_->children_ = new node_type*[1];
    superroot_->model_.a_ = new double[max_key_length_]();
    root_node_->parent_ = superroot_;
    update_superroot_pointer();
  }

  // Updates the key domain based on the min/max keys and retrains the model.
  // Should only be called immediately after bulk loading
  void update_superroot_key_domain() {
    T *min_key_arr, *max_key_arr;
    //min/max should always be '!' and '~...~'
    //the reason we are doing this cumbersome process is because
    //'!' may not be inserted at the first data node.
    //We need some way to handle this. May be fixed by unbiasing keys.
    min_key_arr = (T *) malloc(max_key_length_);
    max_key_arr = (T *) malloc(max_key_length_);
    for (unsigned int i = 0; i < max_key_length_; i++) {
      max_key_arr[i] = STR_VAL_MAX;
      min_key_arr[i] = STR_VAL_MIN;
    }

#if DEBUG_PRINT
    //for (unsigned int i = 0; i < max_key_length_; i++) {
    //  std::cout << min_key_arr[i] << ' ';
    //}
    //std::cout << std::endl;
    //for (unsigned int i = 0; i < max_key_length_; i++) {
    //  std::cout << max_key_arr[i] << ' ';
    //}
    //std::cout << std::endl;
#endif
    std::copy(min_key_arr, min_key_arr + max_key_length_, istats_.key_domain_min_);
    std::copy(max_key_arr, max_key_arr + max_key_length_, istats_.key_domain_max_);
    std::copy(min_key_arr, min_key_arr + max_key_length_, superroot_->pivot_key_.key_arr_);

    AlexKey<T> mintmpkey(istats_.key_domain_min_);
    AlexKey<T> maxtmpkey(istats_.key_domain_max_);
    if (key_equal(mintmpkey, maxtmpkey)) {//keys are equal
      unsigned int non_zero_cnt_ = 0;

      for (unsigned int i = 0; i < max_key_length_; i++) {
        if (istats_.key_domain_min_[i] == 0) {
          superroot_->model_.a_[i] = 0;
        }
        else {
          superroot_->model_.a_[i] = 1 / istats_.key_domain_min_[i];
          non_zero_cnt_ += 1;
        }
      }
      
      for (unsigned int i = 0; i < max_key_length_; i++) {
        superroot_->model_.a_[i] /= non_zero_cnt_;
      }
      superroot_->model_.b_ = 0;
    }
    else {//keys are not equal
      double direction_vector_[max_key_length_] = {0.0};
      
      for (unsigned int i = 0; i < max_key_length_; i++) {
        direction_vector_[i] = (double) istats_.key_domain_max_[i] - istats_.key_domain_min_[i];
      }
      superroot_->model_.b_ = 0.0;
      unsigned int non_zero_cnt_ = 0;
      for (unsigned int i = 0; i < max_key_length_; i++) {
        if (direction_vector_[i] == 0) {
          superroot_->model_.a_[i] = 0;
        }
        else {
          superroot_->model_.a_[i] = 1 / (direction_vector_[i]);
          superroot_->model_.b_ -= istats_.key_domain_min_[i] / direction_vector_[i];
          non_zero_cnt_ += 1;
        }
      }
      
      for (unsigned int i = 0; i < max_key_length_; i++) {
        superroot_->model_.a_[i] /= non_zero_cnt_;
      }
      superroot_->model_.b_ /= non_zero_cnt_;
    }

    if (typeid(T) == typeid(char)) { //need to free malloced objects.
      free(min_key_arr);
      free(max_key_arr);
    }

#if DEBUG_PRINT
    //std::cout << "left prediction result (uskd) " << superroot_->model_.predict_double(mintmpkey) << std::endl;
    //std::cout << "right prediction result (uskd) " << superroot_->model_.predict_double(maxtmpkey) << std::endl;
#endif
  }

  void update_superroot_pointer() {
    superroot_->children_[0] = root_node_;
    superroot_->level_ = static_cast<short>(root_node_->level_ - 1);
  }

  // Recursively bulk load a single node.
  // Assumes node has already been trained to output [0, 1), has cost.
  // Figures out the optimal partitioning of children.
  // node is trained as if it's a model node.
  // data_node_model is what the node's model would be if it were a data node of
  // dense keys.
  void bulk_load_node(const V values[], int num_keys, node_type*& node,
                      model_node_type* parent, int total_keys,
                      const LinearModel<T>* data_node_model = nullptr) {
    // Automatically convert to data node when it is impossible to be better
    // than current cost
#if DEBUG_PRINT
    std::cout << "called bulk_load_node!" << std::endl;
#endif
    if (num_keys <= derived_params_.max_data_node_slots *
                        data_node_type::kInitDensity_ &&
        (node->cost_ < kNodeLookupsWeight || node->model_.a_ == 0) &&
        (node != root_node_)) {
      auto data_node = new (data_node_allocator().allocate(1))
          data_node_type(node->level_, derived_params_.max_data_node_slots,
                         parent, key_less_, allocator_);
      data_node->bulk_load(values, num_keys, expected_min_numkey_per_data_node_,
                           data_node_model, params_.approximate_model_computation);
      data_node->cost_ = node->cost_;
      delete_node(node);
      node = data_node;
#if DEBUG_PRINT
      std::cout << "returned because it can't be better" << std::endl;
#endif
      return;
    }

    // Use a fanout tree to determine the best way to divide the key space into
    // child nodes
    std::vector<fanout_tree::FTNode> used_fanout_tree_nodes;
    std::pair<int, double> best_fanout_stats;
    int max_data_node_keys = static_cast<int>(
        derived_params_.max_data_node_slots * data_node_type::kInitDensity_);
    best_fanout_stats = fanout_tree::find_best_fanout_bottom_up<T, P>(
        values, num_keys, node, total_keys, used_fanout_tree_nodes,
        derived_params_.max_fanout, max_data_node_keys, expected_min_numkey_per_data_node_,
        params_.expected_insert_frac, params_.approximate_model_computation,
        params_.approximate_cost_computation);
    int best_fanout_tree_depth = best_fanout_stats.first;
    double best_fanout_tree_cost = best_fanout_stats.second;

    // Decide whether this node should be a model node or data node
    if (best_fanout_tree_cost < node->cost_ ||
        num_keys > derived_params_.max_data_node_slots *
                       data_node_type::kInitDensity_) {
#if DEBUG_PRINT
      std::cout << "decided that current bulk_load_node calling node should be model node" << std::endl;
#endif
      // Convert to model node based on the output of the fanout tree
      auto model_node = new (model_node_allocator().allocate(1))
          model_node_type(node->level_, parent, allocator_);
      if (best_fanout_tree_depth == 0) {
        // slightly hacky: we assume this means that the node is relatively
        // uniform but we need to split in
        // order to satisfy the max node size, so we compute the fanout that
        // would satisfy that condition in expectation
        //std::cout << "hitted hacky case in bulk_load" << std::endl;
        best_fanout_tree_depth =
            std::max(static_cast<int>(std::log2(static_cast<double>(num_keys) /
                                       derived_params_.max_data_node_slots)) + 1, 1);
        //clear pointers used in fanout_tree (O(N)), and then empty used_fanout_tree_nodes.
        for (fanout_tree::FTNode& tree_node : used_fanout_tree_nodes) {
          delete[] tree_node.a;
        }
        used_fanout_tree_nodes.clear();
        int max_data_node_keys = static_cast<int>(
            derived_params_.max_data_node_slots * data_node_type::kInitDensity_);
#if DEBUG_PRINT
        std::cout << "computing level for depth" << std::endl;
#endif
        while (true) {
          fanout_tree::compute_level<T, P>(
            values, num_keys, total_keys, used_fanout_tree_nodes,
            best_fanout_tree_depth, node->model_, max_data_node_keys,
            params_.expected_insert_frac, params_.approximate_model_computation,
            params_.approximate_cost_computation);
          
          if (used_fanout_tree_nodes.front().right_boundary == num_keys) {
            //std::cout << "retry" << '\n';
            for (fanout_tree::FTNode& tree_node : used_fanout_tree_nodes) {
              delete[] tree_node.a;
            }
            used_fanout_tree_nodes.clear();
            best_fanout_tree_depth <<= 1;
            if (best_fanout_tree_depth > derived_params_.max_fanout) {
              std::cout << values[0].first.key_arr_ << '\n';
              std::cout << values[num_keys - 1].first.key_arr_ << '\n';
              std::cout << "bad case in bulk_load_node. unsolvable" << std::endl;
              abort();
            }
          }
          else break;
          
        }
#if DEBUG_PRINT
        std::cout << "finished level computing" << std::endl;
#endif
      }
      int fanout = 1 << best_fanout_tree_depth;
#if DEBUG_PRINT
      std::cout << "chosen fanout is... : " << fanout << std::endl;
#endif
      //obtianing CDF resulting to [0,fanout]
      LinearModel<T> tmp_model;
      LinearModelBuilder<T> tmp_model_builder(&tmp_model);
      for (int i = 0; i < num_keys; i++) {
        tmp_model_builder.add(values[i].first, ((double) i * fanout / (num_keys-1)));
      }
      tmp_model_builder.build();
      for (unsigned int i = 0; i < max_key_length_; i++) {
        model_node->model_.a_[i] = tmp_model.a_[i];
      }
      model_node->model_.b_ = tmp_model.b_; 
      
      model_node->num_children_ = fanout;
      model_node->children_ = new node_type*[fanout];

      // Instantiate all the child nodes and recurse
      int cur = 0;
#if DEBUG_PRINT
      //int cumu_repeat = 0;
#endif
      for (fanout_tree::FTNode& tree_node : used_fanout_tree_nodes) {
        auto child_node = new (model_node_allocator().allocate(1))
            model_node_type(static_cast<short>(node->level_ + 1), model_node, allocator_);
        child_node->cost_ = tree_node.cost;
        child_node->duplication_factor_ =
            static_cast<uint8_t>(best_fanout_tree_depth - tree_node.level);
        int repeats = 1 << child_node->duplication_factor_;
#if DEBUG_PRINT
        std::cout << "left boundary is : ";
        if (tree_node.left_boundary == num_keys) {
          for (unsigned int i = 0; i < max_key_length_; i++) {
            std::cout << (char) values[tree_node.left_boundary - 1].first.key_arr_[i];
          }
        }
        else {
          for (unsigned int i = 0; i < max_key_length_; i++) {
            std::cout << (char) values[tree_node.left_boundary].first.key_arr_[i];
          }
        }
        std::cout << std::endl;
        std::cout << "right boundary is : ";
        for (unsigned int i = 0; i < max_key_length_; i++) {
          std::cout << (char) values[tree_node.right_boundary - 1].first.key_arr_[i];
        }
        std::cout << std::endl;
#endif

        //obtain CDF with range [0,1]
        int num_keys = tree_node.right_boundary - tree_node.left_boundary;
        LinearModelBuilder<T> child_model_builder(&child_node->model_);
#if DEBUG_PRINT
        //printf("l_idx : %d, f_idx : %d, num_keys : %d\n", l_idx, f_idx, num_keys);
#endif
        if (num_keys == 0) {std::cout << "shouldn't happen" << std::endl;}
        if (num_keys == 1) {
          child_model_builder.add(values[tree_node.left_boundary].first, 1.0);
        }
        else {
          for (int i = tree_node.right_boundary; i < tree_node.left_boundary; i++) {
            child_model_builder.add(values[i].first, (double) (i-tree_node.left_boundary)/(num_keys-1));
          }
        }
        child_model_builder.build();

#if DEBUG_PRINT
        //T left_key[max_key_length_];
        //T right_key[max_key_length_];
        //for (unsigned int i = 0; i < max_key_length_; i++) {
        //  left_key[i] = left_boundary[i];
        //  right_key[i] = right_boundary[i];
        //}
        //std::cout << "left prediction result (bln) " << child_node->model_.predict_double(AlexKey<T>(left_key)) << std::endl;
        //std::cout << "right prediction result (bln) " << child_node->model_.predict_double(AlexKey<T>(right_key)) << std::endl;
#endif

        model_node->children_[cur] = child_node;
        LinearModel<T> child_data_node_model(tree_node.a, tree_node.b);
        bulk_load_node(values + tree_node.left_boundary,
                       tree_node.right_boundary - tree_node.left_boundary,
                       model_node->children_[cur], model_node, total_keys,
                       &child_data_node_model);
        model_node->children_[cur]->duplication_factor_ =
            static_cast<uint8_t>(best_fanout_tree_depth - tree_node.level);
        
        if (model_node->children_[cur]->is_leaf_) {
          static_cast<data_node_type*>(model_node->children_[cur])
              ->expected_avg_exp_search_iterations_ =
              tree_node.expected_avg_search_iterations;
          static_cast<data_node_type*>(model_node->children_[cur])
              ->expected_avg_shifts_ = tree_node.expected_avg_shifts;
        }
        for (int i = cur + 1; i < cur + repeats; i++) {
          model_node->children_[i] = model_node->children_[cur];
        }
        cur += repeats;
      }

      /* update pivot_key_ for new model node*/
      std::copy(values[0].first.key_arr_, values[0].first.key_arr_ + max_key_length_,
        model_node->pivot_key_.key_arr_);
      
      
#if DEBUG_PRINT
      std::cout << "info for model node : " << model_node << '\n';
      std::cout << "pivot_key_(model_node) : " << model_node->pivot_key_.key_arr_ << '\n';
      for (int i = 0; i < fanout; i++) {
        std::cout << i << "'s initial pointer value is : " << model_node->children_[i] << '\n';
        std::cout << i << "'s pivot_key_ is : " << model_node->children_[i]->pivot_key_.key_arr_ << '\n';
      }
      std::cout << std::flush;
#endif

      delete_node(node);
      node = model_node;
    } else {
#if DEBUG_PRINT
      std::cout << "decided that current bulk_load_node calling node should be data node" << std::endl;
#endif
      // Convert to data node
      auto data_node = new (data_node_allocator().allocate(1))
          data_node_type(node->level_, derived_params_.max_data_node_slots,
                         parent, key_less_, allocator_);
      data_node->bulk_load(values, num_keys, expected_min_numkey_per_data_node_,
                           data_node_model, params_.approximate_model_computation);
      data_node->cost_ = node->cost_;
      delete_node(node);
      node = data_node;
    }

    //empty used_fanout_tree_nodes for preventing memory leakage.
    for (fanout_tree::FTNode& tree_node : used_fanout_tree_nodes) {
      delete[] tree_node.a;
    }
#if DEBUG_PRINT
    std::cout << "returned using fanout" << std::endl;
#endif
  }

  // Caller needs to set the level, duplication factor, and neighbor pointers of
  // the returned data node
  static data_node_type* bulk_load_leaf_node_from_existing(
      const data_node_type* existing_node, AlexKey<T>** leaf_keys, P* leaf_payloads, 
      int left, int right, uint64_t worker_id, self_type *this_ptr,
      bool compute_cost = true, const fanout_tree::FTNode* tree_node = nullptr) {
    auto node = new (this_ptr->data_node_allocator().allocate(1))
        data_node_type(existing_node->parent_, this_ptr->key_less_, this_ptr->allocator_);

    // Use the model and num_keys saved in the tree node so we don't have to
    // recompute it
    LinearModel<T> precomputed_model(tree_node->a, tree_node->b);
    node->bulk_load_from_existing(leaf_keys, leaf_payloads,
                                  left, right, worker_id, &precomputed_model,
                                  tree_node->num_keys, this_ptr->expected_min_numkey_per_data_node_);

    node->max_slots_ = this_ptr->derived_params_.max_data_node_slots;
    if (compute_cost) {
      node->cost_ = node->compute_expected_cost(existing_node->frac_inserts());
    }
    return node;
  }

  /*** Lookup ***/

  size_t count(const AlexKey<T>& key) {
    ConstIterator it = lower_bound(key);
    size_t num_equal = 0;
    while (!it.is_end() && key_equal(it.key(), key)) {
      num_equal++;
      ++it;
    }
    return num_equal;
  }

  // Returns an iterator to the first key no less than the input value
  //returns end iterator on error.
  // WARNING : iterator may cause error if other threads are also operating on ALEX
  // NOTE : the user should adequately call rcu_progress with thread_id for proper progress
  //        or use it when no other thread is working on ALEX.
  typename self_type::Iterator lower_bound(const AlexKey<T>& key) {
    data_node_type* leaf = get_leaf(key, 0);
    if (leaf == nullptr) {return end();}
    int idx = leaf->find_lower(key);
    return Iterator(leaf, idx);  // automatically handles the case where idx ==
                                 // leaf->data_capacity
  }

  typename self_type::ConstIterator lower_bound(const AlexKey<T>& key) const {
    data_node_type* leaf = get_leaf(key, 0);
    if (leaf == nullptr) {return cend();}
    int idx = leaf->find_lower(key);
    return ConstIterator(leaf, idx);  // automatically handles the case where
                                      // idx == leaf->data_capacity
  }

  // Returns an iterator to the first key greater than the input value
  // returns end iterator on error
  // WARNING : iterator may cause error if other threads are also operating on ALEX
  // NOTE : the user should adequately call rcu_progress with thread_id for proper progress
  //        or use it when no other thread is working on ALEX.
  typename self_type::Iterator upper_bound(const AlexKey<T>& key) {
    data_node_type* leaf = typeid(T) == typeid(char) ? get_leaf(key, 0) : get_leaf(key);
    if (leaf == nullptr) {return end();}
    int idx = leaf->find_upper(key);
    return Iterator(leaf, idx);  // automatically handles the case where idx ==
                                 // leaf->data_capacity
  }

  typename self_type::ConstIterator upper_bound(const AlexKey<T>& key) const {
    data_node_type* leaf = typeid(T) == typeid(char) ? get_leaf(key, 0) : get_leaf(key);
    if (leaf == nullptr) {return cend();}
    int idx = leaf->find_upper(key);
    return ConstIterator(leaf, idx);  // automatically handles the case where
                                      // idx == leaf->data_capacity
  }

  std::pair<Iterator, Iterator> equal_range(const AlexKey<T>& key) {
    return std::pair<Iterator, Iterator>(lower_bound(key), upper_bound(key));
  }

  std::pair<ConstIterator, ConstIterator> equal_range(const AlexKey<T>& key) const {
    return std::pair<ConstIterator, ConstIterator>(lower_bound(key),
                                                   upper_bound(key));
  }

  //helper for get_payload
  //obtain timing
#if PROFILE
  void update_profileStats_get_payload_success(std::chrono::time_point<std::chrono::high_resolution_clock> start_time,
                                            model_node_type *last_parent, uint64_t worker_id){
    auto get_payload_from_parent_end_time = std::chrono::high_resolution_clock::now();
    auto elapsed_time = std::chrono::duration_cast<std::chrono::fgTimeUnit>(get_payload_from_parent_end_time - start_time).count();
    if (last_parent == superroot_) {
      profileStats.get_payload_from_superroot_success_time[worker_id] += elapsed_time;
      profileStats.get_payload_superroot_success_cnt[worker_id]++;
      profileStats.max_get_payload_from_superroot_success_time[worker_id] =
        std::max(profileStats.max_get_payload_from_superroot_success_time[worker_id], elapsed_time);
      profileStats.min_get_payload_from_superroot_success_time[worker_id] =
        std::min(profileStats.min_get_payload_from_superroot_success_time[worker_id], elapsed_time);
    }
    else {
      profileStats.get_payload_from_parent_success_time[worker_id] += elapsed_time;
      profileStats.get_payload_directp_success_cnt[worker_id]++;
      profileStats.max_get_payload_from_parent_success_time[worker_id] =
        std::max(profileStats.max_get_payload_from_parent_success_time[worker_id], elapsed_time);
      profileStats.min_get_payload_from_parent_success_time[worker_id] =
        std::min(profileStats.min_get_payload_from_parent_success_time[worker_id], elapsed_time);
    }
  }

  void update_profileStats_get_payload_fail(std::chrono::time_point<std::chrono::high_resolution_clock> start_time,
                                            model_node_type *last_parent, uint64_t worker_id){
    auto get_payload_from_parent_end_time = std::chrono::high_resolution_clock::now();
    auto elapsed_time = std::chrono::duration_cast<std::chrono::fgTimeUnit>(get_payload_from_parent_end_time - start_time).count();
    if (last_parent == superroot_) {
      profileStats.get_payload_from_superroot_fail_time[worker_id] += elapsed_time;
      profileStats.get_payload_superroot_fail_cnt[worker_id]++;
      profileStats.max_get_payload_from_superroot_fail_time[worker_id] =
        std::max(profileStats.max_get_payload_from_superroot_fail_time[worker_id], elapsed_time);
      profileStats.min_get_payload_from_superroot_fail_time[worker_id] =
        std::min(profileStats.min_get_payload_from_superroot_fail_time[worker_id], elapsed_time);
    }
    else {
      profileStats.get_payload_from_parent_fail_time[worker_id] += elapsed_time;
      profileStats.get_payload_directp_fail_cnt[worker_id]++;
      profileStats.max_get_payload_from_parent_fail_time[worker_id] =
        std::max(profileStats.max_get_payload_from_parent_fail_time[worker_id], elapsed_time);
      profileStats.min_get_payload_from_parent_fail_time[worker_id] =
        std::min(profileStats.min_get_payload_from_parent_fail_time[worker_id], elapsed_time);
    }
  }
#endif

  // Returns whether payload search was successful, and the payload itself if it was successful.
  // This avoids the overhead of creating an iterator
public:
  std::tuple<int, P, model_node_type *> get_payload(const AlexKey<T>& key, uint64_t worker_id) {
    return get_payload_from_parent(key, superroot_, worker_id);
  }

  //first element returns...
  //0 on success.
  //1 if failed because write is writing
  //2 if failed because not foundable
  std::tuple<int, P, model_node_type *> get_payload_from_parent(const AlexKey<T>& key, model_node_type *last_parent, uint64_t worker_id) {
#if PROFILE
    if (last_parent == superroot_) {
      profileStats.get_payload_superroot_call_cnt[worker_id]++;
    }
    else {
      profileStats.get_payload_directp_call_cnt[worker_id]++;
    }
    auto get_payload_from_parent_start_time = std::chrono::high_resolution_clock::now();
#endif
    data_node_type* leaf = get_leaf_from_parent(key, worker_id, last_parent, 0);
    if (leaf == nullptr) {
      rcu_progress(worker_id);
      return {2, 0, nullptr};
    }

    //try reading. If failed, retry later
    if (pthread_rwlock_tryrdlock(&(leaf->key_array_rw_lock_))) {
      auto parent = leaf->parent_;
      rcu_progress(worker_id);
#if PROFILE
      update_profileStats_get_payload_fail(get_payload_from_parent_start_time, last_parent, worker_id);
#endif
      return {1, 0, parent};
    }
    int idx = leaf->find_key(key, worker_id, KEY_ARR);
    
    if (idx >= 0) { //successful
      P rval = leaf->get_payload(idx, KEY_ARR);
      pthread_rwlock_unlock(&(leaf->key_array_rw_lock_));
      rcu_progress(worker_id);
#if PROFILE
      update_profileStats_get_payload_success(get_payload_from_parent_start_time, last_parent, worker_id);
#endif
      return {0, rval, nullptr};
    }

    //failed finding in key_array. Try finding in delta index if it exists
    pthread_rwlock_unlock(&(leaf->key_array_rw_lock_));
    if (leaf->delta_idx_ == nullptr) { //delta index doesn't exist
      auto parent = leaf->parent_;
      rcu_progress(worker_id);
  #if PROFILE
      update_profileStats_get_payload_fail(get_payload_from_parent_start_time, last_parent, worker_id);
  #endif
      return {1, 0, parent};
    }
    else if (pthread_rwlock_tryrdlock(&(leaf->delta_index_rw_lock_))) { //failed obtaining lock
      auto parent = leaf->parent_;
      rcu_progress(worker_id);
  #if PROFILE
      update_profileStats_get_payload_fail(get_payload_from_parent_start_time, last_parent, worker_id);
  #endif
      return {1, 0, parent};
    }

    //succeeded, so try finding key
    idx = leaf->find_key(key, worker_id, DELTA_IDX);

    if (idx >= 0) { //successful
      P rval = leaf->get_payload(idx, DELTA_IDX);
      pthread_rwlock_unlock(&(leaf->delta_index_rw_lock_));
      rcu_progress(worker_id);
#if PROFILE
      update_profileStats_get_payload_success(get_payload_from_parent_start_time, last_parent, worker_id);
#endif
      return {0, rval, nullptr};
    }

    //failed finding in key_array. Try finding in delta index if it exists
    pthread_rwlock_unlock(&(leaf->delta_index_rw_lock_));
    if (leaf->tmp_delta_idx_ == nullptr) {//but tmp_delta_idx_ doesn't exist. failed
      auto parent = leaf->parent_;
      rcu_progress(worker_id);
  #if PROFILE
      update_profileStats_get_payload_fail(get_payload_from_parent_start_time, last_parent, worker_id);
  #endif
      return {1, 0, parent};
    }
    if (pthread_rwlock_tryrdlock(&(leaf->tmp_delta_index_rw_lock_))) {//failed obtaining lock
      auto parent = leaf->parent_;
      rcu_progress(worker_id);
  #if PROFILE
      update_profileStats_get_payload_fail(get_payload_from_parent_start_time, last_parent, worker_id);
  #endif
      return {1, 0, parent};
    }

    idx = leaf->find_key(key, worker_id, TMP_DELTA_IDX);

    if (idx >= 0) { //successful
      P rval = leaf->get_payload(idx, TMP_DELTA_IDX);
      pthread_rwlock_unlock(&(leaf->tmp_delta_index_rw_lock_));
      rcu_progress(worker_id);
#if PROFILE
      update_profileStats_get_payload_success(get_payload_from_parent_start_time, last_parent, worker_id);
#endif
      return {0, rval, nullptr};
    }
    else {
      //failed. Try later.
      pthread_rwlock_unlock(&(leaf->tmp_delta_index_rw_lock_));
      auto parent = leaf->parent_;
      rcu_progress(worker_id);
#if PROFILE
      update_profileStats_get_payload_fail(get_payload_from_parent_start_time, last_parent, worker_id);
#endif
      return {1, 0, parent};
    }
  }

  // Looks for the last key no greater than the input value
  // Conceptually, this is equal to the last key before upper_bound()
  // returns end iterator on error
  // WARNING : iterator may cause error if other threads are also operating on ALEX
  // NOTE : the user should adequately call rcu_progress with thread_id for proper progress
  //        or use it when no other thread is working on ALEX.
  typename self_type::Iterator find_last_no_greater_than(const AlexKey<T>& key) {
    data_node_type* leaf = get_leaf(key, 0);
    if (leaf == nullptr) {return end();}
    const int idx = leaf->upper_bound(key) - 1;
    if (idx >= 0) {
      return Iterator(leaf, idx);
    }

    // Edge case: need to check previous data node(s)
    while (true) {
      if (leaf->prev_leaf_.val_ == nullptr) {
        return Iterator(leaf, 0);
      }
      leaf = leaf->prev_leaf_.val_;
      if (leaf->num_keys_ > 0) {
        return Iterator(leaf, leaf->last_pos());
      }
    }
  }

  typename self_type::Iterator begin() {
    node_type* cur = root_node_;

    while (!cur->is_leaf_) {
      cur = static_cast<model_node_type*>(cur)->children_[0];
    }
    return Iterator(static_cast<data_node_type*>(cur), 0);
  }

  typename self_type::Iterator end() {
    Iterator it = Iterator();
    it.cur_leaf_ = nullptr;
    it.cur_idx_ = 0;
    return it;
  }

  typename self_type::ConstIterator cbegin() const {
    node_type* cur = root_node_;

    while (!cur->is_leaf_) {
      cur = static_cast<model_node_type*>(cur)->children_[0];
    }
    return ConstIterator(static_cast<data_node_type*>(cur), 0);
  }

  typename self_type::ConstIterator cend() const {
    ConstIterator it = ConstIterator();
    it.cur_leaf_ = nullptr;
    it.cur_idx_ = 0;
    return it;
  }

  typename self_type::ReverseIterator rbegin() {
    node_type* cur = root_node_;

    while (!cur->is_leaf_) {
      auto model_node = static_cast<model_node_type*>(cur);
      cur = model_node->children_[model_node->num_children_ - 1];
    }
    auto data_node = static_cast<data_node_type*>(cur);
    return ReverseIterator(data_node, data_node->data_capacity_ - 1);
  }

  typename self_type::ReverseIterator rend() {
    ReverseIterator it = ReverseIterator();
    it.cur_leaf_ = nullptr;
    it.cur_idx_ = 0;
    return it;
  }

  typename self_type::ConstReverseIterator crbegin() const {
    node_type* cur = root_node_;

    while (!cur->is_leaf_) {
      auto model_node = static_cast<model_node_type*>(cur);
      cur = model_node->children_[model_node->num_children_ - 1];
    }
    auto data_node = static_cast<data_node_type*>(cur);
    return ConstReverseIterator(data_node, data_node->data_capacity_ - 1);
  }

  typename self_type::ConstReverseIterator crend() const {
    ConstReverseIterator it = ConstReverseIterator();
    it.cur_leaf_ = nullptr;
    it.cur_idx_ = 0;
    return it;
  }

  /*** Insert ***/

 public:
  int erase_one(const AlexKey<T>& key) {
    data_node_type* leaf = get_leaf_from_parent(key, 0, superroot_, 0);
    if (leaf == nullptr) {return 0;}
    int num_erased = leaf->erase_one(key);
    // stats_.num_keys -= num_erased;
    // if (leaf->num_keys_ == 0) {
    //   merge(leaf, key);
    // }
    // if (key > istats_.key_domain_max_) {
    //   istats_.num_keys_above_key_domain -= num_erased;
    // } else if (key < istats_.key_domain_min_) {
    //   istats_.num_keys_below_key_domain -= num_erased;
    // }
    return num_erased;
  }

  std::pair<Iterator, bool> insert(const V& value, uint64_t worker_id) {
    return insert(value.first, value.second, worker_id);
  }

  template <class InputIterator>
  void insert(InputIterator first, InputIterator last, uint64_t worker_id) {
    for (auto it = first; it != last; ++it) {
      insert(*it, worker_id);
    }
  }

  std::tuple<Iterator, bool, model_node_type *> insert(const AlexKey<T>& key, const P& payload, uint64_t worker_id) {
    return insert_from_parent(key, payload, superroot_, worker_id);
  }

  // This will NOT do an update of an existing key.
  // To perform an update or read-modify-write, do a lookup and modify the
  // payload's value.
  // Returns iterator to inserted element, and whether the insert happened or
  // not.
  // Insert does not happen if duplicates are not allowed and duplicate is
  // found.
  // If it failed finding a leaf, it returns iterator with null leaf with 0 index.
  // If we need to retry later, it returns iterator with null leaf with 1 index
  std::tuple<Iterator, bool, model_node_type *> insert_from_parent(const AlexKey<T>& key, const P& payload, 
                                               model_node_type *last_parent, uint64_t worker_id) {
    // in string ALEX, keys should not fall outside the key domain
#if PROFILE
    if (last_parent == superroot_) {
      profileStats.insert_superroot_call_cnt[worker_id]++;
    }
    else {
      profileStats.insert_directp_call_cnt[worker_id]++;
    }
    auto insert_from_parent_start_time = std::chrono::high_resolution_clock::now();
#endif
    char larger_key = 0;
    char smaller_key = 0;
    for (unsigned int i = 0; i < max_key_length_; i++) {
      if (key.key_arr_[i] > istats_.key_domain_max_[i]) {larger_key = 1; break;}
      else if (key.key_arr_[i] < istats_.key_domain_min_[i]) {smaller_key = 1; break;}
    }
    if (larger_key || smaller_key) {
      // std::cout << "worker id : " << worker_id 
      //           << " root expansion should not happen." << std::endl;
      // abort();
    }

    std::vector<TraversalNode<T, P>> traversal_path;
    data_node_type* leaf = get_leaf_from_parent(key, worker_id, last_parent, 1, &traversal_path);
    if (leaf == nullptr) {
      //failed finding leaf, shouldn't happen in normal cases.
      rcu_progress(worker_id);
      return {Iterator(nullptr, 0), false, nullptr};
    } 
    
    model_node_type *parent = traversal_path.back().node;
    if (pthread_mutex_trylock(&leaf->insert_mutex_)) {
      //failed obtaining mutex
      rcu_progress(worker_id);
#if PROFILE
      auto insert_from_parent_end_time = std::chrono::high_resolution_clock::now();
      auto elapsed_time = std::chrono::duration_cast<std::chrono::fgTimeUnit>(insert_from_parent_end_time - insert_from_parent_start_time).count();
      if (last_parent == superroot_) {
        profileStats.insert_from_superroot_fail_time[worker_id] += elapsed_time;
        profileStats.insert_superroot_fail_cnt[worker_id]++;
        profileStats.max_insert_from_superroot_fail_time[worker_id] =
          std::max(profileStats.max_insert_from_superroot_fail_time[worker_id], elapsed_time);
        profileStats.min_insert_from_superroot_fail_time[worker_id] =
          std::min(profileStats.min_insert_from_superroot_fail_time[worker_id], elapsed_time);
      }
      else {
        profileStats.insert_from_parent_fail_time[worker_id] += elapsed_time;
        profileStats.insert_directp_fail_cnt[worker_id]++;
        profileStats.max_insert_from_parent_fail_time[worker_id] =
          std::max(profileStats.max_insert_from_parent_fail_time[worker_id], elapsed_time);
        profileStats.min_insert_from_parent_fail_time[worker_id] =
          std::min(profileStats.min_insert_from_parent_fail_time[worker_id], elapsed_time);
      }
#endif
      return {Iterator(nullptr, 1), false, parent};
    }
#if DEBUG_PRINT
    alex::coutLock.lock();
    std::cout << "t" << worker_id << " - in final, decided to insert at bucketID : "
              << traversal_path.back().bucketID << '\n';
    std::cout << "it's node status is : " << leaf->node_status_ << std::endl;
    alex::coutLock.unlock();
#endif

    // Nonzero fail flag means that the insert did not happen
    std::pair<std::pair<int, int>, std::pair<data_node_type *, data_node_type *>> ret 
      = leaf->insert(key, payload, worker_id);
    int fail = ret.first.first;
    int insert_pos = ret.first.second;
    leaf = ret.second.first;
    //data_node_type *maybe_new_data_node = ret.second.second;

    if (fail == -1) {
      // Duplicate found and duplicates not allowed
      pthread_mutex_unlock(&leaf->insert_mutex_);
      rcu_progress(worker_id);
#if PROFILE
      auto insert_from_parent_end_time = std::chrono::high_resolution_clock::now();
      auto elapsed_time = std::chrono::duration_cast<std::chrono::fgTimeUnit>(insert_from_parent_end_time - insert_from_parent_start_time).count();
      if (last_parent == superroot_) {
        profileStats.insert_from_superroot_fail_time[worker_id] += elapsed_time;
        profileStats.insert_superroot_fail_cnt[worker_id]++;
        profileStats.max_insert_from_superroot_fail_time[worker_id] =
          std::max(profileStats.max_insert_from_superroot_fail_time[worker_id], elapsed_time);
        profileStats.min_insert_from_superroot_fail_time[worker_id] =
          std::min(profileStats.min_insert_from_superroot_fail_time[worker_id], elapsed_time);
      }
      else {
        profileStats.insert_from_parent_fail_time[worker_id] += elapsed_time;
        profileStats.insert_directp_fail_cnt[worker_id]++;
        profileStats.max_insert_from_parent_fail_time[worker_id] =
          std::max(profileStats.max_insert_from_parent_fail_time[worker_id], elapsed_time);
        profileStats.min_insert_from_parent_fail_time[worker_id] =
          std::min(profileStats.min_insert_from_parent_fail_time[worker_id], elapsed_time);
      }
#endif
      return {Iterator(leaf, insert_pos), false, nullptr};
    }
    else if (fail == 6) {
      //delta/tmp_delta is full... try later.
      pthread_mutex_unlock(&leaf->insert_mutex_);
      rcu_progress(worker_id);
#if PROFILE
      auto insert_from_parent_end_time = std::chrono::high_resolution_clock::now();
      auto elapsed_time = std::chrono::duration_cast<std::chrono::fgTimeUnit>(insert_from_parent_end_time - insert_from_parent_start_time).count();
      if (last_parent == superroot_) {
        profileStats.insert_from_superroot_fail_time[worker_id] += elapsed_time;
        profileStats.insert_superroot_fail_cnt[worker_id]++;
        profileStats.max_insert_from_superroot_fail_time[worker_id] =
          std::max(profileStats.max_insert_from_superroot_fail_time[worker_id], elapsed_time);
        profileStats.min_insert_from_superroot_fail_time[worker_id] =
          std::min(profileStats.min_insert_from_superroot_fail_time[worker_id], elapsed_time);
      }
      else {
        profileStats.insert_from_parent_fail_time[worker_id] += elapsed_time;
        profileStats.insert_directp_fail_cnt[worker_id]++;
        profileStats.max_insert_from_parent_fail_time[worker_id] =
          std::max(profileStats.max_insert_from_parent_fail_time[worker_id], elapsed_time);
        profileStats.min_insert_from_parent_fail_time[worker_id] =
          std::min(profileStats.min_insert_from_parent_fail_time[worker_id], elapsed_time);
      }
#endif
      return {Iterator(nullptr, 2), false, parent};
    }
    else if (!fail) {//succeded without modification
#if DEBUG_PRINT
      alex::coutLock.lock();
      std::cout << "t" << worker_id << " - ";
      std::cout << "alex.h insert : succeeded insertion and processing" << std::endl;
      alex::coutLock.unlock();
#endif
      pthread_mutex_unlock(&leaf->insert_mutex_);
      num_keys++;
      rcu_progress(worker_id);
#if PROFILE
      auto insert_from_parent_end_time = std::chrono::high_resolution_clock::now();
      auto elapsed_time = std::chrono::duration_cast<std::chrono::fgTimeUnit>(insert_from_parent_end_time - insert_from_parent_start_time).count();
      if (last_parent == superroot_) {
        profileStats.insert_from_superroot_success_time[worker_id] += elapsed_time;
        profileStats.insert_superroot_success_cnt[worker_id]++;
        profileStats.max_insert_from_superroot_success_time[worker_id] =
          std::max(profileStats.max_insert_from_superroot_success_time[worker_id], elapsed_time);
        profileStats.min_insert_from_superroot_success_time[worker_id] =
          std::min(profileStats.min_insert_from_superroot_success_time[worker_id], elapsed_time);
      }
      else {
        profileStats.insert_from_parent_success_time[worker_id] += elapsed_time;
        profileStats.insert_directp_success_cnt[worker_id]++;
        profileStats.max_insert_from_parent_success_time[worker_id] =
          std::max(profileStats.max_insert_from_parent_success_time[worker_id], elapsed_time);
        profileStats.min_insert_from_parent_success_time[worker_id] =
          std::min(profileStats.min_insert_from_parent_success_time[worker_id], elapsed_time);
      }
#endif
      return {Iterator(leaf, insert_pos), true, nullptr}; //iterator could be invalid.
    }
    else { //succeeded, but needs to modify
      if (fail == 4) { //need to expand
        expandParam *param = new expandParam();
        param->leaf = leaf;
        param->worker_id = worker_id;
  #if DEBUG_PRINT
        coutLock.lock();
        std::cout << "t" << worker_id << " - generating new delta index for leaf " << leaf << '\n'
                  << "with bucketID " << traversal_path.back().bucketID << " and parent " << leaf->parent_ << std::endl;
        coutLock.unlock();
  #endif
        leaf->generate_new_delta_idx(expected_min_numkey_per_data_node_, worker_id);
        {
          std::lock_guard<std::mutex> lk(cvm);
          std::pair<void *, int> job_pair = {param, 1};
          pending_modification_jobs_.push(job_pair);
        }
        cv.notify_one();
      }
      else {
        //create thread that handles modification and let it handle
        alexIParam *param = new alexIParam();
        param->leaf = leaf;
        param->worker_id = worker_id;
        param->bucketID = traversal_path.back().bucketID;
        param->this_ptr = this;
  #if DEBUG_PRINT
          coutLock.lock();
          std::cout << "t" << worker_id << " - generating new delta index for leaf " << leaf << '\n'
                    << "with bucketID " << traversal_path.back().bucketID << " and parent " << leaf->parent_ << std::endl;
          coutLock.unlock();
  #endif
        leaf->generate_new_delta_idx(expected_min_numkey_per_data_node_, worker_id);
        {
          std::lock_guard<std::mutex> lk(cvm);
          std::pair<void *, int> job_pair = {param, 0};
         pending_modification_jobs_.push(job_pair);
        }
        cv.notify_one();
      }

      //original thread returns and retry later. (need to rcu_progress)
      pthread_mutex_unlock(&leaf->insert_mutex_);
      rcu_progress(worker_id);

#if PROFILE
      auto insert_from_parent_end_time = std::chrono::high_resolution_clock::now();
      auto elapsed_time = std::chrono::duration_cast<std::chrono::fgTimeUnit>(insert_from_parent_end_time - insert_from_parent_start_time).count();

      if (last_parent == superroot_) {
        profileStats.insert_from_superroot_success_time[worker_id] += elapsed_time;
        profileStats.insert_superroot_success_cnt[worker_id]++;
        profileStats.max_insert_from_superroot_success_time[worker_id] =
          std::max(profileStats.max_insert_from_superroot_success_time[worker_id], elapsed_time);
        profileStats.min_insert_from_superroot_success_time[worker_id] =
          std::min(profileStats.min_insert_from_superroot_success_time[worker_id], elapsed_time);
      }
      else {
        profileStats.insert_from_parent_success_time[worker_id] += elapsed_time;
        profileStats.insert_directp_success_cnt[worker_id]++;
        profileStats.max_insert_from_parent_success_time[worker_id] =
          std::max(profileStats.max_insert_from_parent_success_time[worker_id], elapsed_time);
        profileStats.min_insert_from_parent_success_time[worker_id] =
          std::min(profileStats.min_insert_from_parent_success_time[worker_id], elapsed_time);
      }
#endif
      return {Iterator(leaf, insert_pos), true, nullptr}; //iterator could be invalid.
    }
  }

  struct expandParam {
    data_node_type *leaf;
    uint64_t worker_id;
  };

  struct alexIParam {
    data_node_type *leaf;
    uint64_t worker_id;
    int bucketID;
    self_type *this_ptr;
  };

  void expand_handler(void *param) {
    expandParam *Eparam = (expandParam *)param;
    data_node_type *leaf = Eparam->leaf;
    uint64_t worker_id = Eparam->worker_id;

#if DEBUG_PRINT
    alex::coutLock.lock();
    std::cout << "t" << worker_id << " - failed and made a thread to resize" << '\n';
    std::cout << "id is : " << pthread_self() << '\n';
    std::cout << "parent is : " << leaf->parent_ << '\n';
    std::cout << "leaf's node_status_ is : " << leaf->node_status_ << std::endl;
    alex::coutLock.unlock();
#endif

    leaf->resize(data_node_type::kMinDensity_, false);
    leaf->update_delta_idx_resize(worker_id);

#if DEBUG_PRINT
    alex::coutLock.lock();
    std::cout << "t" << worker_id << "'s generated thread for parent " << leaf->parent_ << " - ";
    std::cout << "alex.h expanded data node" << std::endl;
    alex::coutLock.unlock();
#endif

    //will use the original data node!
    delete Eparam;
  }

  void insert_fail_handler(void *param) {
    //parameter obtaining
    alexIParam *Iparam = (alexIParam *) param;
    data_node_type *leaf = Iparam->leaf;
    uint64_t worker_id = Iparam->worker_id;
    int bucketID = Iparam->bucketID;
    self_type *this_ptr = Iparam->this_ptr;

    model_node_type* parent = leaf->parent_;
#if DEBUG_PRINT
    alex::coutLock.lock();
    std::cout << "t" << worker_id << " - failed and made a thread to resize/split node\n";
    std::cout << "id is : " << pthread_self() << '\n';
    std::cout << "parent is : " << parent << '\n';
    std::cout << "bucketID : " << bucketID << '\n';
    std::cout << "leaf's node_status_ is : " << leaf->node_status_ << std::endl;
    //std::cout << "reason is : " << fail << std::endl;
    alex::coutLock.unlock();
#endif

    std::vector<fanout_tree::FTNode> used_fanout_tree_nodes;

    int fanout_tree_depth = 1;
    double *model_param = nullptr;

    //make array of holding key/payload pointers
    //used for inserting and fanout finding
    //we also make model for find_best_fanout_existing_node
    typedef typename AlexDataNode<T, P>::const_iterator_type node_iterator; 
    int total_num_keys;
    int leaf_status = leaf->node_status_;
    bool leaf_just_splitted = leaf->child_just_splitted_;
    if (leaf_just_splitted) {
      int leaf_delta_insert_keys_ = 0;
      if (leaf->was_left_child_) {
        node_iterator it(leaf, 0, true);
        while (!it.is_end() && it.cur_idx_ < leaf->boundary_base_key_idx_) {
          it++;
          leaf_delta_insert_keys_++;
        }
      }
      else {
        node_iterator it(leaf, leaf->boundary_base_key_idx_, true);
        while (!it.is_end()) {
          it++;
          leaf_delta_insert_keys_++;
        }
      }
#if DEBUG_PRINT
      coutLock.lock();
      std::cout << "t" << worker_id << "'s generated thread - "
                << leaf->was_left_child_ << ", " << leaf->boundary_base_key_idx_ << ", "
                << leaf_delta_insert_keys_ << ", " << leaf->num_keys_ << std::endl;
      coutLock.unlock();
#endif
      total_num_keys = leaf->num_keys_ + leaf_delta_insert_keys_;
    }
    else {
#if DEBUG_PRINT
      coutLock.lock();
      std::cout << "t" << worker_id << "'s generated thread - "
                << leaf_status << ", " << leaf->num_keys_;
      if (leaf_status != INSERT_AT_DELTA) {
        std::cout << ", " << leaf->delta_num_keys_;
      }
      std::cout << std::endl;
      coutLock.unlock();
#endif
      total_num_keys = leaf->num_keys_ + (leaf_status == INSERT_AT_DELTA ? 0 : leaf->delta_num_keys_);
    }

#if DEBUG_PRINT
    coutLock.lock();
    std::cout << "t" << worker_id << "'s generated thread - total number of keys : "
              << total_num_keys << std::endl;
    coutLock.unlock();
#endif

    AlexKey<T>** leaf_keys = new AlexKey<T>*[total_num_keys];
    P* leaf_payloads = new P[total_num_keys];

    node_iterator it(leaf, 0);
    LinearModel<T> tmp_model;
    LinearModelBuilder<T> tmp_model_builder(&tmp_model);
    int key_cnt = 0;
    if (leaf_status == INSERT_AT_DELTA) {
      while (it.cur_idx_ != -1) {
        tmp_model_builder.add(it.key(), ((double) key_cnt / (total_num_keys - 1)));
        leaf_keys[key_cnt] = &it.key();
        leaf_payloads[key_cnt] = it.payload();
        key_cnt++;
        it++;
      }
    }
    else if (leaf_status == INSERT_AT_TMPDELTA) {
      int delta_start_idx;
      if (leaf_just_splitted && leaf->was_right_child_) {
        delta_start_idx = leaf->boundary_base_key_idx_;
      }
      else {delta_start_idx = 0;}
      AlexKey<T> *key_ptr;
      P payload;

      node_iterator delta_it(leaf, delta_start_idx, true);
      while (key_cnt < total_num_keys) {
        if (it.is_smaller(delta_it)) {
          key_ptr = &it.key();
          payload = it.payload();
          it++;
        }
        else {
          key_ptr = &delta_it.key();
          payload = it.payload();
          delta_it++;
        }
        
        tmp_model_builder.add(*key_ptr, ((double) key_cnt / (total_num_keys - 1)));
        leaf_keys[key_cnt] = key_ptr;
        leaf_payloads[key_cnt] = payload;
        key_cnt++;
      }
    }
    else {
      std::cout << "error before find best fanout existing node" << std::endl;
      abort();
    }

    //for erroneous condition handling
    if (key_cnt != total_num_keys) {
      std::cout << "key_cnt mismatch on insert handling" << std::endl;
      abort();
    }

#if PROFILE
    auto fanout_model_train_start_time = std::chrono::high_resolution_clock::now();
    profileStats.fanout_model_train_cnt++;
#endif
    tmp_model_builder.build();    
#if PROFILE
    auto fanout_model_train_end_time = std::chrono::high_resolution_clock::now();
    auto train_elapsed_time = std::chrono::duration_cast<std::chrono::bgTimeUnit>(fanout_model_train_end_time - fanout_model_train_start_time).count();
    profileStats.fanout_model_train_time += train_elapsed_time;
    profileStats.max_fanout_model_train_time =
      std::max(profileStats.max_fanout_model_train_time.load(), train_elapsed_time);
    profileStats.min_fanout_model_train_time =
      std::min(profileStats.min_fanout_model_train_time.load(), train_elapsed_time);
#endif

    //obtain best fanout
    auto ret = fanout_tree::find_best_fanout_existing_node<T, P>(
          leaf, leaf_keys, tmp_model, 
          this_ptr->num_keys.load(), total_num_keys, 
          used_fanout_tree_nodes, 2, worker_id);
    fanout_tree_depth = ret.first;
    model_param = ret.second;
              
    int best_fanout = 1 << fanout_tree_depth;

    if (fanout_tree_depth == 0) {
#if DEBUG_PRINT
      alex::coutLock.lock();
      std::cout << "t" << worker_id << "'s generated thread for parent " << parent << " - ";
      std::cout << "failed and decided to expand" << std::endl;
      alex::coutLock.unlock();
#endif
      // expand existing data node and retrain model
      leaf->resize(data_node_type::kMinDensity_, true);
      leaf->reset_stats();
      fanout_tree::FTNode& tree_node = used_fanout_tree_nodes[0];
      leaf->cost_ = tree_node.cost;
      leaf->expected_avg_exp_search_iterations_ =
          tree_node.expected_avg_search_iterations;
      leaf->expected_avg_shifts_ = tree_node.expected_avg_shifts;
      leaf->update_delta_idx_resize(worker_id);
    } else {
      // either split sideways or downwards
      // synchronization is covered automatically in splitting functions.
      bool should_split_downwards =
          (parent->num_children_ * best_fanout /
                   (1 << leaf->duplication_factor_) >
               this_ptr->derived_params_.max_fanout ||
           parent->level_ == this_ptr->superroot_->level_ ||
           (fanout_tree_depth > leaf->duplication_factor_));
      if (should_split_downwards) {
#if DEBUG_PRINT
        alex::coutLock.lock();
        std::cout << "t" << worker_id << "'s generated thread for parent " << parent << " - ";
        std::cout << "failed and decided to split downwards" << std::endl;
        alex::coutLock.unlock();
#endif
        split_downwards(parent, bucketID, fanout_tree_depth, model_param, used_fanout_tree_nodes,
                        leaf_keys, leaf_payloads, worker_id, this_ptr);
      } else {
#if DEBUG_PRINT
        alex::coutLock.lock();
        std::cout << "t" << worker_id << "'s generated thread for parent " << parent << " - ";
        std::cout << "failed and decided to split sideways" << std::endl;
        alex::coutLock.unlock();
#endif
        split_sideways(parent, bucketID, fanout_tree_depth, used_fanout_tree_nodes,
                       leaf_keys, leaf_payloads, worker_id, this_ptr);
      }
    }

    delete[] ret.second;

    //empty used_fanout_tree_nodes for preventing memory leakage.
    for (fanout_tree::FTNode& tree_node : used_fanout_tree_nodes) {delete[] tree_node.a;}

    //cleanup
    delete Iparam;
    delete[] leaf_keys;
    delete[] leaf_payloads;

#if DEBUG_PRINT
    coutLock.lock();
    std::cout << "t" << worker_id << "'s generated thread for parent " << parent << " - ";
    std::cout << "finished modifying resize/split" << std::endl;
    coutLock.unlock();
#endif
  }

  // Splits downwards in the manner determined by the fanout tree and updates
  // the pointers of the parent.
  // If no fanout tree is provided, then splits downward in two. Returns the
  // newly created model node.
  static void split_downwards(
      model_node_type* parent, int bucketID, int fanout_tree_depth, double *model_param,
      std::vector<fanout_tree::FTNode>& used_fanout_tree_nodes,
      AlexKey<T>** leaf_keys, P* leaf_payloads, 
      uint64_t worker_id, self_type *this_ptr) {
#if PROFILE
    profileStats.split_downwards_call_cnt++;
    auto split_downwards_start_time = std::chrono::high_resolution_clock::now();
#endif
#if DEBUG_PRINT
    alex::coutLock.lock();
    std::cout << "t" << worker_id << "'s generated thread for parent " << parent << " - ";
    std::cout << "...bucketID : " << bucketID << std::endl;
    alex::coutLock.unlock();
#endif
    auto leaf = static_cast<data_node_type*> (parent->children_[bucketID]);
#if DEBUG_PRINT
    alex::coutLock.lock();
    std::cout << "t" << worker_id << "'s generated thread for parent " << parent << " - ";
    std::cout << "and leaf is : " << leaf << std::endl;
    alex::coutLock.unlock();
#endif

    // Create the new model node that will replace the current data node
    int fanout = 1 << fanout_tree_depth;
    auto new_node = new (this_ptr->model_node_allocator().allocate(1))
        model_node_type(leaf->level_, parent, this_ptr->allocator_);
    new_node->duplication_factor_ = leaf->duplication_factor_;
    new_node->num_children_ = fanout;
    new_node->children_ = new node_type*[fanout];
    //needs to initialize pivot key in case of split_downwards.
    std::copy(leaf->pivot_key_.key_arr_, leaf->pivot_key_.key_arr_ + max_key_length_,
              new_node->pivot_key_.key_arr_);


    int repeats = 1 << leaf->duplication_factor_;
    int start_bucketID =
        bucketID - (bucketID % repeats);  // first bucket with same child
    int end_bucketID =
        start_bucketID + repeats;  // first bucket with different child

    std::copy(model_param, model_param + max_key_length_, new_node->model_.a_);
    new_node->model_.b_ = model_param[max_key_length_];

#if DEBUG_PRINT
    //alex::coutLock.lock();
    //std::cout << "t" << worker_id << "'s generated thread - ";
    //std::cout << "left prediction result (sd) " << new_node->model_.predict_double(leaf->key_slots_[leaf->first_pos()]) << std::endl;
    //std::cout << "right prediction result (sd) " << new_node->model_.predict_double(leaf->key_slots_[leaf->last_pos()]) << std::endl;
    //alex::coutLock.unlock();
#endif

    // Create new data nodes
    if (used_fanout_tree_nodes.empty()) {
      std::cout << "used_fanout_tree_nodes empty" << std::endl;
      abort(); //shouldn't happen
    } else {
      create_new_data_nodes(leaf, new_node, fanout_tree_depth, used_fanout_tree_nodes, 
                            leaf_keys, leaf_payloads, worker_id, this_ptr);
    }

    //substitute pointers in parent model node
    //pthread_rwlock_wrlock(&(parent->children_rw_lock_));
    for (int i = start_bucketID; i < end_bucketID; i++) {
      parent->children_[i] = new_node;
    }
#if DEBUG_PRINT
    alex::coutLock.lock();
    std::cout << "t" << worker_id << "'s generated thread for parent " << parent << " - ";
    std::cout << "split_downwards parent children_\n";
    for (int i = 0; i < parent->num_children_; i++) {
      std::cout << i << " : " << parent->children_[i] << '\n';
    }
    std::cout << "t" << worker_id << "'s generated thread for parent " << parent << " - ";
    std::cout << "pivot_key_(model_node) : " << new_node->pivot_key_.key_arr_ << '\n';
    for (int i = 0; i < fanout; i++) {
        std::cout << i << "'s pivot_key_ is : "
                  << new_node->children_[i]->pivot_key_.key_arr_ << '\n';
    }
    std::cout << std::flush;
    alex::coutLock.unlock();
#endif
    //pthread_rwlock_unlock(&(parent->children_rw_lock_));
    if (parent == this_ptr->superroot_) {
#if DEBUG_PRINT
      alex::coutLock.lock();
      std::cout << "t" << worker_id << "'s generated thread for parent " << parent << " - "
                << "root node splitted downwards" << std::endl;
      alex::coutLock.unlock();
#endif
      this_ptr->root_node_ = new_node;
    }

    //destroy unused leaf and metadata after waiting.
    rcu_barrier();
    pthread_mutex_unlock(&leaf->insert_mutex_);
    this_ptr->delete_node(leaf);
#if PROFILE
    auto split_downwards_end_time = std::chrono::high_resolution_clock::now();
    auto elapsed_time = std::chrono::duration_cast<std::chrono::bgTimeUnit>(split_downwards_end_time - split_downwards_start_time).count();
    profileStats.split_downwards_time += elapsed_time;
    profileStats.max_split_downwards_time =
      std::max(profileStats.max_split_downwards_time.load(), elapsed_time);
    profileStats.min_split_downwards_time =
      std::min(profileStats.min_split_downwards_time.load(), elapsed_time);
#endif
  }

  // Splits data node sideways in the manner determined by the fanout tree.
  // If no fanout tree is provided, then splits sideways in two.
  static void split_sideways(model_node_type* parent, int bucketID,
                      int fanout_tree_depth,
                      std::vector<fanout_tree::FTNode>& used_fanout_tree_nodes,
                      AlexKey<T>** leaf_keys, P* leaf_payloads,
                      uint64_t worker_id, self_type *this_ptr) {
#if PROFILE
    profileStats.split_sideways_call_cnt++;
    auto split_sideways_start_time = std::chrono::high_resolution_clock::now();
#endif
    auto leaf = static_cast<data_node_type*>(parent->children_[bucketID]);

    int fanout = 1 << fanout_tree_depth;
    int repeats = 1 << leaf->duplication_factor_;
    if (fanout > repeats) {
      //in multithreading, because of synchronization issue of duplication_fcator_
      //we don't do model expansion.
      ;
    }
    int start_bucketID =
        bucketID - (bucketID % repeats);  // first bucket with same child

    if (used_fanout_tree_nodes.empty()) {
      std::cout << "used_fanout_tree_nodes empty" << std::endl;
      abort(); //shouldn't happen
    } else {
      // Extra duplication factor is required when there are more redundant
      // pointers than necessary
      int extra_duplication_factor =
          std::max(0, leaf->duplication_factor_ - fanout_tree_depth);
      create_new_data_nodes(leaf, parent, fanout_tree_depth,
                            used_fanout_tree_nodes, leaf_keys, leaf_payloads,
                            worker_id, this_ptr, 1, start_bucketID, extra_duplication_factor);
    }

    rcu_barrier();
    this_ptr->delete_node(leaf);
#if PROFILE
    auto split_sideways_end_time = std::chrono::high_resolution_clock::now();
    auto elapsed_time = std::chrono::duration_cast<std::chrono::bgTimeUnit>(split_sideways_end_time - split_sideways_start_time).count();
    profileStats.split_sideways_time += elapsed_time;
    profileStats.max_split_sideways_time =
      std::max(profileStats.max_split_sideways_time.load(), elapsed_time);
    profileStats.min_split_sideways_time =
      std::min(profileStats.min_split_sideways_time.load(), elapsed_time);
#endif
  }

  //final working for split_sideways.
  //noticable part is that old_node's parent is same as parameter 'parent'.
  void cndn_final_work_for_split_sideways(data_node_type *old_node, model_node_type *parent, int old_node_status,
                                          int mid_boundary, AlexKey<T>** leaf_keys, int start_bucketID, 
                                          AlexKey<T>* old_node_activated_delta_idx, 
                                          std::vector<std::pair<node_type *, int>> &generated_nodes,
                                          uint64_t worker_id) {
    //We now stop inserting to delta index/tmp delta index of old node
    //Then we update new data node's delta index as old node's delta / tmp delta index
    //We than update 'already existing' model node's metadata.
    //After unlock of children_rw_lock_, threads will start inserting to new node.
    pthread_mutex_lock(&old_node->insert_mutex_);
    //pthread_rwlock_wrlock(&parent->children_rw_lock_);
    int old_node_activated_delta_idx_capacity_;
    int old_node_activated_delta_idx_num_keys_;
    int old_node_activated_delta_idx_bitmap_size_;

    if (old_node_status == INSERT_AT_DELTA) {
      old_node_activated_delta_idx_capacity_ = old_node->delta_idx_capacity_;
      old_node_activated_delta_idx_num_keys_ = old_node->delta_num_keys_;
      old_node_activated_delta_idx_bitmap_size_ = old_node->delta_bitmap_size_;
    }
    else {
      old_node_activated_delta_idx_capacity_ = old_node->tmp_delta_idx_capacity_;
      old_node_activated_delta_idx_num_keys_ = old_node->tmp_delta_num_keys_;
      old_node_activated_delta_idx_bitmap_size_ = old_node->tmp_delta_bitmap_size_;
    }

    int left_idx = 0;
    int right_idx = old_node_activated_delta_idx_capacity_;
    int mid = left_idx + (right_idx - left_idx) / 2;

    while (left_idx < right_idx) {
      if (old_node_activated_delta_idx[mid] < *leaf_keys[mid_boundary]) {
        left_idx = mid + 1;
      }
      else {
        right_idx = mid;
      }
      mid = left_idx + (right_idx - left_idx) / 2;
    }

    int cur = start_bucketID;
    for (auto it = generated_nodes.begin(); it != generated_nodes.end(); ++it) { //should be two
      auto generated_node_pair = *it;
      data_node_type *generated_node = (data_node_type *) generated_node_pair.first;
      generated_node->delta_idx_capacity_ = old_node_activated_delta_idx_capacity_;
      generated_node->delta_num_keys_ = old_node_activated_delta_idx_num_keys_;
      generated_node->delta_bitmap_size_ = old_node_activated_delta_idx_bitmap_size_;
      generated_node->boundary_base_key_idx_ = left_idx;

      for (int i = cur; i < cur + generated_node_pair.second; ++i) {
        parent->children_[i] = generated_node_pair.first;
      }
      cur += generated_node_pair.second;
    }
#if DEBUG_PRINT
      alex::coutLock.lock();
      std::cout << "t" << worker_id << "'s generated thread for parent " << parent << " - ";
      std::cout << "cndn children_\n";
      for (int i = 0 ; i < parent->num_children_; i++) {
        std::cout << i << " : " << parent->children_[i] << '\n';
      }
      std::cout << "delta idx capacity is " << old_node_activated_delta_idx_capacity_ << '\n'; 
      std::cout << "boundary base key idx is : " << left_idx << '\n';
      std::cout << "delta index's key count is : " << old_node_activated_delta_idx_num_keys_ << '\n';
      std::cout << std::flush;
      alex::coutLock.unlock();
#endif
    pthread_mutex_unlock(&old_node->insert_mutex_);  
    //pthread_rwlock_unlock(&parent->children_rw_lock_);
    //now, the threads will insert to this new node.

    //some cleanups
    //preventing deletion of old delta index, since it's used by children.
    if (old_node_status == INSERT_AT_DELTA) {old_node->delta_idx_ = nullptr;}
    else if (old_node_status == INSERT_AT_TMPDELTA) {old_node->tmp_delta_idx_ = nullptr;}
    else {//shouldn't happen
      std::cout << "modified node that wasn't supposed to be modified?" << std::endl;
      abort();
    }
    return;
  }

  //final working for split downwards
  //noticable part is that old_node's parent is NOT same as parameter parent.
  //This makes semantic difference compared to above function
  void cndn_final_work_for_split_downwards(data_node_type *old_node, model_node_type *parent, int old_node_status,
                                          int mid_boundary, AlexKey<T>** leaf_keys, int start_bucketID, 
                                          AlexKey<T>* old_node_activated_delta_idx, 
                                          std::vector<std::pair<node_type *, int>> &generated_nodes,
                                          uint64_t worker_id) {
    //We 'first' updae 'new' model node's metadata. (parent)
    int cur = start_bucketID;
    for (auto it = generated_nodes.begin(); it != generated_nodes.end(); ++it) { //should be two
      auto generated_node_pair = *it;
      for (int i = cur; i < cur + generated_node_pair.second; ++i) {
        parent->children_[i] = generated_node_pair.first;
      }
      cur += generated_node_pair.second;
    }

    //We now stop allowing insertion to old node
    //than move the delta/tmp delta index of old node to new node's
    //Then we actually move the new model node to already existing model node's children.
    pthread_mutex_lock(&old_node->insert_mutex_);
    int old_node_activated_delta_idx_capacity_;
    int old_node_activated_delta_idx_num_keys_;
    int old_node_activated_delta_idx_bitmap_size_;

    if (old_node_status == INSERT_AT_DELTA) {
      old_node_activated_delta_idx_capacity_ = old_node->delta_idx_capacity_;
      old_node_activated_delta_idx_num_keys_ = old_node->delta_num_keys_;
      old_node_activated_delta_idx_bitmap_size_ = old_node->delta_bitmap_size_;
    }
    else {
      old_node_activated_delta_idx_capacity_ = old_node->tmp_delta_idx_capacity_;
      old_node_activated_delta_idx_num_keys_ = old_node->tmp_delta_num_keys_;
      old_node_activated_delta_idx_bitmap_size_ = old_node->tmp_delta_bitmap_size_;
    }

    int left_idx = 0;
    int right_idx = old_node_activated_delta_idx_capacity_;
    int mid = left_idx + (right_idx - left_idx) / 2;

    while (left_idx < right_idx) {
      if (old_node_activated_delta_idx[mid] < *leaf_keys[mid_boundary]) {
        left_idx = mid + 1;
      }
      else {
        right_idx = mid;
      }
      mid = left_idx + (right_idx - left_idx) / 2;
    }

    for (auto it = generated_nodes.begin(); it != generated_nodes.end(); ++it) { //should be two
      auto generated_node_pair = *it;
      data_node_type *generated_node = (data_node_type *) generated_node_pair.first;
      generated_node->delta_idx_capacity_ = old_node_activated_delta_idx_capacity_;
      generated_node->delta_num_keys_ = old_node_activated_delta_idx_num_keys_;
      generated_node->delta_bitmap_size_ = old_node_activated_delta_idx_bitmap_size_;
      generated_node->boundary_base_key_idx_ = left_idx;
    }
#if DEBUG_PRINT
      alex::coutLock.lock();
      std::cout << "t" << worker_id << "'s generated thread for parent " << parent << " - ";
      std::cout << "cndn children_\n";
      for (int i = 0 ; i < parent->num_children_; i++) {
        std::cout << i << " : " << parent->children_[i] << '\n';
      }
      std::cout << "delta idx capacity is " << old_node_activated_delta_idx_capacity_ << '\n'; 
      std::cout << "boundary base key idx is : " << left_idx << '\n';
      std::cout << "delta index's key count is : " << old_node_activated_delta_idx_num_keys_ << '\n';
      std::cout << std::flush;
      alex::coutLock.unlock();
#endif
    if (old_node_status == INSERT_AT_DELTA) {old_node->delta_idx_ = nullptr;}
    else if (old_node_status == INSERT_AT_TMPDELTA) {old_node->tmp_delta_idx_ = nullptr;}
    else {//shouldn't happen
      std::cout << "modified node that wasn't supposed to be modified?" << std::endl;
      abort();
    }

    //lock should be unlocked by split_downwards
    return;
  }

  // Create new data nodes from the keys in the old data node according to the
  // fanout tree, insert the new
  // nodes as children of the parent model node starting from a given position,
  // and link the new data nodes together.
  // Helper for splitting when using a fanout tree.
  // mode 0 : for split_downwards
  // mode 1 : for split_sideways
 static void create_new_data_nodes(
      data_node_type* old_node, model_node_type* parent,
      int fanout_tree_depth, std::vector<fanout_tree::FTNode>& used_fanout_tree_nodes,
      AlexKey<T>** leaf_keys, P* leaf_payloads, uint64_t worker_id, self_type *this_ptr, 
      int mode = 0, int start_bucketID = 0, int extra_duplication_factor = 0) {
#if DEBUG_PRINT
    //alex::coutLock.lock();
    //std::cout << "t" << worker_id << "'s generated thread for parent " << parent << " - ";
    //std::cout << "called create_new_dn" << std::endl;
    //std::cout << "old node is " << old_node << std::endl;
    //alex::coutLock.unlock();
#endif
    // Create the new data nodes
    int cur = start_bucketID;  // first bucket with same child
    std::vector<std::pair<node_type *, int>> generated_nodes;
#if DEBUG_PRINT
    alex::coutLock.lock();
    std::cout << "t" << worker_id << "'s generated thread for parent " << parent << " - ";
    std::cout << "starting bucket is" << start_bucketID << std::endl;
    alex::coutLock.unlock();
#endif
    data_node_type* prev_leaf =
        old_node->prev_leaf_.read();  // used for linking the new data nodes
    int old_node_status = old_node->node_status_;
    AlexKey<T>* old_node_activated_delta_idx;
    P* old_node_activated_delta_payload;
    uint64_t* old_node_activated_bitmap;
    LinearModel<T> old_node_activated_delta_model;

    if (old_node_status == INSERT_AT_DELTA) {
      old_node_activated_delta_idx = old_node->delta_idx_;
      old_node_activated_delta_payload = old_node->delta_idx_payloads_;
      old_node_activated_bitmap = old_node->delta_bitmap_;
      old_node_activated_delta_model = old_node->delta_idx_model_;
    }
    else {
      old_node_activated_delta_idx = old_node->tmp_delta_idx_;
      old_node_activated_delta_payload = old_node->tmp_delta_idx_payloads_;
      old_node_activated_bitmap = old_node->tmp_delta_bitmap_;
      old_node_activated_delta_model = old_node->tmp_delta_idx_model_;
    }

#if DEBUG_PRINT
    alex::coutLock.lock();
    std::cout << "t" << worker_id << "'s generated thread for parent " << parent << " - ";
    std::cout << "initial prev_leaf is : " << prev_leaf << std::endl;
    alex::coutLock.unlock();
#endif
    int left_boundary = 0;
    int right_boundary = 0;
    int mid_boundary = 0;
    // Keys may be re-assigned to an adjacent fanout tree node due to off-by-one
    // errors
    bool first_iter = true;
    AtomicVal<int>* reused_delta_idx_cnt = new AtomicVal<int>(0);
    //we assume iteration is always done exactly twice, since we only split by two.
    for (fanout_tree::FTNode& tree_node : used_fanout_tree_nodes) {
      left_boundary = right_boundary;
      auto duplication_factor = static_cast<uint8_t>(
          fanout_tree_depth - tree_node.level + extra_duplication_factor);
      int child_node_repeats = 1 << duplication_factor;
      right_boundary = tree_node.right_boundary;
      data_node_type* child_node = bulk_load_leaf_node_from_existing(
          old_node, leaf_keys, leaf_payloads, left_boundary, right_boundary, 
          worker_id, this_ptr, false, &tree_node);
#if DEBUG_PRINT
      alex::coutLock.lock();
      std::cout << "t" << worker_id << "'s generated thread for parent " << parent << " - ";
      std::cout << "child_node pointer : " << child_node << std::endl;
      alex::coutLock.unlock();
#endif
      //new data node's basic metadata update
      child_node->level_ = static_cast<short>(parent->level_ + 1);
      child_node->cost_ = tree_node.cost;
      child_node->duplication_factor_ = duplication_factor;
      child_node->expected_avg_exp_search_iterations_ =
          tree_node.expected_avg_search_iterations;
      child_node->expected_avg_shifts_ = tree_node.expected_avg_shifts;
      
      //new data node's delta index related metadata update
      child_node->delta_idx_ = old_node_activated_delta_idx;
      child_node->delta_idx_payloads_ = old_node_activated_delta_payload;
      child_node->delta_bitmap_ = old_node_activated_bitmap;
      child_node->child_just_splitted_ = true;
      child_node->reused_delta_idx_cnt_ = reused_delta_idx_cnt;
      reused_delta_idx_cnt->val_ += 1;
      child_node->delta_idx_model_ = old_node_activated_delta_model;

      if (first_iter) { //left leaf is not a new data node. Should be first node only.
        mid_boundary = right_boundary;
        child_node->was_left_child_ = true;
        old_node->pending_left_leaf_.update(child_node);
#if DEBUG_PRINT
        //alex::coutLock.lock();
        //std::cout << "t" << worker_id << "'s generated thread for parent " << parent << " - ";
        //std::cout << "updated pll with " << child_node << std::endl;
        //alex::coutLock.unlock();
#endif
        if (prev_leaf != nullptr) {
          data_node_type *prev_leaf_pending_rl = prev_leaf->pending_right_leaf_.read();
          if (prev_leaf_pending_rl != nullptr) {
            child_node->prev_leaf_.update(prev_leaf_pending_rl);
            prev_leaf_pending_rl->next_leaf_.update(child_node);
          }
          else {
#if DEBUG_PRINT
            alex::coutLock.lock();
            std::cout << "t" << worker_id << "'s generated thread for parent " << parent << " - ";
            std::cout << "child_node's prev_leaf_ is " << prev_leaf << std::endl;
            alex::coutLock.unlock();
#endif
            child_node->prev_leaf_.update(prev_leaf);
            prev_leaf->next_leaf_.update(child_node);
          }
        }
        else {
          child_node->prev_leaf_.update(nullptr);
        }
        first_iter = false;
      }
      else { //left leaf is a new data node. Should be second node only
        child_node->was_right_child_ = true;
#if DEBUG_PRINT
        alex::coutLock.lock();
        std::cout << "t" << worker_id << "'s generated thread for parent " << parent << " - ";
        std::cout << "child_node's prev_leaf_ is " << prev_leaf << std::endl;
        alex::coutLock.unlock();
#endif
        child_node->prev_leaf_.update(prev_leaf);
        prev_leaf->next_leaf_.update(child_node);
      }
      child_node->parent_ = parent;
      cur += child_node_repeats;
      generated_nodes.push_back({child_node, child_node_repeats});
      prev_leaf = child_node;
#if DEBUG_PRINT
      alex::coutLock.lock();
      std::cout << "t" << worker_id << "'s generated thread for parent " << parent << " - ";
      std::cout << "new data node made with pivot_key_ as : "
                << child_node->pivot_key_.key_arr_
                << std::endl;
      alex::coutLock.unlock();
#endif
    }

    //update right-most leaf's next/prev leaf.
    old_node->pending_right_leaf_.update(prev_leaf);
#if DEBUG_PRINT
    //alex::coutLock.lock();
    //std::cout << "t" << worker_id << "'s generated thread for parent " << parent << " - ";
    //std::cout << "updated prl with " << prev_leaf << std::endl;
    //alex::coutLock.unlock();
#endif
    data_node_type *next_leaf = old_node->next_leaf_.read();
    if (next_leaf != nullptr) {
      data_node_type *next_leaf_pending_ll = next_leaf->pending_left_leaf_.read();
      if (next_leaf_pending_ll != nullptr) {
        prev_leaf->next_leaf_.update(next_leaf_pending_ll);
        next_leaf_pending_ll->prev_leaf_.update(prev_leaf);
      }
      else {
        prev_leaf->next_leaf_.update(next_leaf);
        next_leaf->prev_leaf_.update(prev_leaf);
      }
    }
    else {
      prev_leaf->next_leaf_.update(nullptr);
    }

    if (mode) {
      this_ptr->cndn_final_work_for_split_sideways(old_node, parent, old_node_status,
          mid_boundary, leaf_keys, start_bucketID, old_node_activated_delta_idx,
           generated_nodes, worker_id);
    }
    else {
      this_ptr->cndn_final_work_for_split_downwards(old_node, parent, old_node_status,
          mid_boundary, leaf_keys, start_bucketID, old_node_activated_delta_idx,
           generated_nodes, worker_id);
    }

#if DEBUG_PRINT
    alex::coutLock.lock();
    std::cout << "t" << worker_id << "'s generated thread for parent " << parent << " - ";
    std::cout << "finished create_new_dn" << std::endl;
    alex::coutLock.unlock();
#endif
  }

  /*** Stats ***/

 public:
  // Number of elements
  size_t size() { return static_cast<size_t>(num_keys); }

  // True if there are no elements
  bool empty() const { return (size() == 0); }

  // This is just a function required by the STL standard. ALEX can hold more
  // items.
  size_t max_size() const { return size_t(-1); }

  // Size in bytes of all the keys, payloads, and bitmaps stored in this index
  long long data_size() const {
    long long size = 0;
    for (NodeIterator node_it = NodeIterator(this); !node_it.is_end();
         node_it.next()) {
      node_type* cur = node_it.current();
      if (cur->is_leaf_) {
        size += static_cast<data_node_type*>(cur)->data_size();
      }
    }
    return size;
  }

  // Size in bytes of all the model nodes (including pointers) and metadata in
  // data nodes
  // should only be called when alex structure is not being modified.
  long long model_size() const {
    long long size = 0;
    for (NodeIterator node_it = NodeIterator(this); !node_it.is_end();
         node_it.next()) {
      size += node_it.current()->node_size();
    }
    return size;
  }

  /*** Iterators ***/

 public:
  class Iterator {
   public:
    data_node_type* cur_leaf_ = nullptr;  // current data node
    int cur_idx_ = 0;         // current position in key/data_slots of data node
    int cur_bitmap_idx_ = 0;  // current position in bitmap
    uint64_t cur_bitmap_data_ = 0;  // caches the relevant data in the current
                                    // bitmap position

    Iterator() {}

    Iterator(data_node_type* leaf, int idx) : cur_leaf_(leaf), cur_idx_(idx) {
      initialize();
    }

    Iterator(const Iterator& other)
        : cur_leaf_(other.cur_leaf_),
          cur_idx_(other.cur_idx_),
          cur_bitmap_idx_(other.cur_bitmap_idx_),
          cur_bitmap_data_(other.cur_bitmap_data_) {}

    Iterator(const ReverseIterator& other)
        : cur_leaf_(other.cur_leaf_), cur_idx_(other.cur_idx_) {
      initialize();
    }

    Iterator& operator=(const Iterator& other) {
      if (this != &other) {
        cur_idx_ = other.cur_idx_;
        cur_leaf_ = other.cur_leaf_;
        cur_bitmap_idx_ = other.cur_bitmap_idx_;
        cur_bitmap_data_ = other.cur_bitmap_data_;
      }
      return *this;
    }

    Iterator& operator++() {
      advance();
      return *this;
    }

    Iterator operator++(int) {
      Iterator tmp = *this;
      advance();
      return tmp;
    }

    // Does not return a reference because keys and payloads are stored
    // separately.
    // If possible, use key() and payload() instead.
    V operator*() const {
      return std::make_pair(cur_leaf_->key_slots_[cur_idx_],
                            cur_leaf_->payload_slots_[cur_idx_].val_);
    }

    const AlexKey<T>& key() const {return ((data_node_type *) cur_leaf_)->get_key(cur_idx_); }

    P& payload() const { return cur_leaf_->get_payload(cur_idx_); }

    bool is_end() const { return cur_leaf_ == nullptr; }

    bool operator==(const Iterator& rhs) const {
      return cur_idx_ == rhs.cur_idx_ && cur_leaf_ == rhs.cur_leaf_;
    }

    bool operator!=(const Iterator& rhs) const { return !(*this == rhs); };

   
    void initialize() {
      if (!cur_leaf_) return;
      assert(cur_idx_ >= 0);
      if (cur_idx_ >= cur_leaf_->data_capacity_) {
        cur_leaf_ = cur_leaf_->next_leaf_.read();
        cur_idx_ = 0;
        if (!cur_leaf_) return;
      }

      cur_bitmap_idx_ = cur_idx_ >> 6;
      cur_bitmap_data_ = cur_leaf_->bitmap_[cur_bitmap_idx_];

      // Zero out extra bits
      int bit_pos = cur_idx_ - (cur_bitmap_idx_ << 6);
      cur_bitmap_data_ &= ~((1ULL << bit_pos) - 1);

      (*this)++;
    }

    forceinline void advance() {
      while (cur_bitmap_data_ == 0) {
        cur_bitmap_idx_++;
        if (cur_bitmap_idx_ >= cur_leaf_->bitmap_size_) {
          cur_leaf_ = cur_leaf_->next_leaf_.read();
          cur_idx_ = 0;
          if (cur_leaf_ == nullptr) {
            return;
          }
          cur_bitmap_idx_ = 0;
        }
        cur_bitmap_data_ = cur_leaf_->bitmap_[cur_bitmap_idx_];
      }
      uint64_t bit = extract_rightmost_one(cur_bitmap_data_);
      cur_idx_ = get_offset(cur_bitmap_idx_, bit);
      cur_bitmap_data_ = remove_rightmost_one(cur_bitmap_data_);
    }
  };

  class ConstIterator {
   public:
    const data_node_type* cur_leaf_ = nullptr;  // current data node
    int cur_idx_ = 0;         // current position in key/data_slots of data node
    int cur_bitmap_idx_ = 0;  // current position in bitmap
    uint64_t cur_bitmap_data_ = 0;  // caches the relevant data in the current
                                    // bitmap position

    ConstIterator() {}

    ConstIterator(const data_node_type* leaf, int idx)
        : cur_leaf_(leaf), cur_idx_(idx) {
      initialize();
    }

    ConstIterator(const Iterator& other)
        : cur_leaf_(other.cur_leaf_),
          cur_idx_(other.cur_idx_),
          cur_bitmap_idx_(other.cur_bitmap_idx_),
          cur_bitmap_data_(other.cur_bitmap_data_) {}

    ConstIterator(const ConstIterator& other)
        : cur_leaf_(other.cur_leaf_),
          cur_idx_(other.cur_idx_),
          cur_bitmap_idx_(other.cur_bitmap_idx_),
          cur_bitmap_data_(other.cur_bitmap_data_) {}

    ConstIterator(const ReverseIterator& other)
        : cur_leaf_(other.cur_leaf_), cur_idx_(other.cur_idx_) {
      initialize();
    }

    ConstIterator(const ConstReverseIterator& other)
        : cur_leaf_(other.cur_leaf_), cur_idx_(other.cur_idx_) {
      initialize();
    }

    ConstIterator& operator=(const ConstIterator& other) {
      if (this != &other) {
        cur_idx_ = other.cur_idx_;
        cur_leaf_ = other.cur_leaf_;
        cur_bitmap_idx_ = other.cur_bitmap_idx_;
        cur_bitmap_data_ = other.cur_bitmap_data_;
      }
      return *this;
    }

    ConstIterator& operator++() {
      advance();
      return *this;
    }

    ConstIterator operator++(int) {
      ConstIterator tmp = *this;
      advance();
      return tmp;
    }

    // Does not return a reference because keys and payloads are stored
    // separately.
    // If possible, use key() and payload() instead.
    V operator*() const {
      return std::make_pair(cur_leaf_->key_slots_[cur_idx_],
                            cur_leaf_->payload_slots_[cur_idx_]);
    }

    const AlexKey<T>& key() const { return cur_leaf_->get_key(cur_idx_); }

    const P& payload() const { return cur_leaf_->get_payload(cur_idx_); }

    bool is_end() const { return cur_leaf_ == nullptr; }

    bool operator==(const ConstIterator& rhs) const {
      return cur_idx_ == rhs.cur_idx_ && cur_leaf_ == rhs.cur_leaf_;
    }

    bool operator!=(const ConstIterator& rhs) const { return !(*this == rhs); };

   
    void initialize() {
      if (!cur_leaf_) return;
      assert(cur_idx_ >= 0);
      if (cur_idx_ >= cur_leaf_->data_capacity_) {
        cur_leaf_ = cur_leaf_->next_leaf_;
        cur_idx_ = 0;
        if (!cur_leaf_) return;
      }

      cur_bitmap_idx_ = cur_idx_ >> 6;
      cur_bitmap_data_ = cur_leaf_->bitmap_[cur_bitmap_idx_];

      // Zero out extra bits
      int bit_pos = cur_idx_ - (cur_bitmap_idx_ << 6);
      cur_bitmap_data_ &= ~((1ULL << bit_pos) - 1);

      (*this)++;
    }

    forceinline void advance() {
      while (cur_bitmap_data_ == 0) {
        cur_bitmap_idx_++;
        if (cur_bitmap_idx_ >= cur_leaf_->bitmap_size_) {
          cur_leaf_ = cur_leaf_->next_leaf_;
          cur_idx_ = 0;
          if (cur_leaf_ == nullptr) {
            return;
          }
          cur_bitmap_idx_ = 0;
        }
        cur_bitmap_data_ = cur_leaf_->bitmap_[cur_bitmap_idx_];
      }
      uint64_t bit = extract_rightmost_one(cur_bitmap_data_);
      cur_idx_ = get_offset(cur_bitmap_idx_, bit);
      cur_bitmap_data_ = remove_rightmost_one(cur_bitmap_data_);
    }
  };

  class ReverseIterator {
   public:
    data_node_type* cur_leaf_ = nullptr;  // current data node
    int cur_idx_ = 0;         // current position in key/data_slots of data node
    int cur_bitmap_idx_ = 0;  // current position in bitmap
    uint64_t cur_bitmap_data_ = 0;  // caches the relevant data in the current
                                    // bitmap position

    ReverseIterator() {}

    ReverseIterator(data_node_type* leaf, int idx)
        : cur_leaf_(leaf), cur_idx_(idx) {
      initialize();
    }

    ReverseIterator(const ReverseIterator& other)
        : cur_leaf_(other.cur_leaf_),
          cur_idx_(other.cur_idx_),
          cur_bitmap_idx_(other.cur_bitmap_idx_),
          cur_bitmap_data_(other.cur_bitmap_data_) {}

    ReverseIterator(const Iterator& other)
        : cur_leaf_(other.cur_leaf_), cur_idx_(other.cur_idx_) {
      initialize();
    }

    ReverseIterator& operator=(const ReverseIterator& other) {
      if (this != &other) {
        cur_idx_ = other.cur_idx_;
        cur_leaf_ = other.cur_leaf_;
        cur_bitmap_idx_ = other.cur_bitmap_idx_;
        cur_bitmap_data_ = other.cur_bitmap_data_;
      }
      return *this;
    }

    ReverseIterator& operator++() {
      advance();
      return *this;
    }

    ReverseIterator operator++(int) {
      ReverseIterator tmp = *this;
      advance();
      return tmp;
    }

    // Does not return a reference because keys and payloads are stored
    // separately.
    // If possible, use key() and payload() instead.
    V operator*() const {
      return std::make_pair(cur_leaf_->key_slots_[cur_idx_],
                            cur_leaf_->payload_slots_[cur_idx_]);
    }

    const AlexKey<T>& key() const { return cur_leaf_->get_key(cur_idx_); }

    P& payload() const { return cur_leaf_->get_payload(cur_idx_); }

    bool is_end() const { return cur_leaf_ == nullptr; }

    bool operator==(const ReverseIterator& rhs) const {
      return cur_idx_ == rhs.cur_idx_ && cur_leaf_ == rhs.cur_leaf_;
    }

    bool operator!=(const ReverseIterator& rhs) const {
      return !(*this == rhs);
    };

   
    void initialize() {
      if (!cur_leaf_) return;
      assert(cur_idx_ >= 0);
      if (cur_idx_ >= cur_leaf_->data_capacity_) {
        cur_leaf_ = cur_leaf_->next_leaf_;
        cur_idx_ = 0;
        if (!cur_leaf_) return;
      }

      cur_bitmap_idx_ = cur_idx_ >> 6;
      cur_bitmap_data_ = cur_leaf_->bitmap_[cur_bitmap_idx_];

      // Zero out extra bits
      int bit_pos = cur_idx_ - (cur_bitmap_idx_ << 6);
      cur_bitmap_data_ &= (1ULL << bit_pos) | ((1ULL << bit_pos) - 1);

      advance();
    }

    forceinline void advance() {
      while (cur_bitmap_data_ == 0) {
        cur_bitmap_idx_--;
        if (cur_bitmap_idx_ < 0) {
          cur_leaf_ = cur_leaf_->prev_leaf_.read();
          if (cur_leaf_ == nullptr) {
            cur_idx_ = 0;
            return;
          }
          cur_idx_ = cur_leaf_->data_capacity_ - 1;
          cur_bitmap_idx_ = cur_leaf_->bitmap_size_ - 1;
        }
        cur_bitmap_data_ = cur_leaf_->bitmap_[cur_bitmap_idx_];
      }
      assert(cpu_supports_bmi());
      int bit_pos = static_cast<int>(63 - _lzcnt_u64(cur_bitmap_data_));
      cur_idx_ = (cur_bitmap_idx_ << 6) + bit_pos;
      cur_bitmap_data_ &= ~(1ULL << bit_pos);
    }
  };

  class ConstReverseIterator {
   public:
    const data_node_type* cur_leaf_ = nullptr;  // current data node
    int cur_idx_ = 0;         // current position in key/data_slots of data node
    int cur_bitmap_idx_ = 0;  // current position in bitmap
    uint64_t cur_bitmap_data_ = 0;  // caches the relevant data in the current
                                    // bitmap position

    ConstReverseIterator() {}

    ConstReverseIterator(const data_node_type* leaf, int idx)
        : cur_leaf_(leaf), cur_idx_(idx) {
      initialize();
    }

    ConstReverseIterator(const ConstReverseIterator& other)
        : cur_leaf_(other.cur_leaf_),
          cur_idx_(other.cur_idx_),
          cur_bitmap_idx_(other.cur_bitmap_idx_),
          cur_bitmap_data_(other.cur_bitmap_data_) {}

    ConstReverseIterator(const ReverseIterator& other)
        : cur_leaf_(other.cur_leaf_),
          cur_idx_(other.cur_idx_),
          cur_bitmap_idx_(other.cur_bitmap_idx_),
          cur_bitmap_data_(other.cur_bitmap_data_) {}

    ConstReverseIterator(const Iterator& other)
        : cur_leaf_(other.cur_leaf_), cur_idx_(other.cur_idx_) {
      initialize();
    }

    ConstReverseIterator(const ConstIterator& other)
        : cur_leaf_(other.cur_leaf_), cur_idx_(other.cur_idx_) {
      initialize();
    }

    ConstReverseIterator& operator=(const ConstReverseIterator& other) {
      if (this != &other) {
        cur_idx_ = other.cur_idx_;
        cur_leaf_ = other.cur_leaf_;
        cur_bitmap_idx_ = other.cur_bitmap_idx_;
        cur_bitmap_data_ = other.cur_bitmap_data_;
      }
      return *this;
    }

    ConstReverseIterator& operator++() {
      advance();
      return *this;
    }

    ConstReverseIterator operator++(int) {
      ConstReverseIterator tmp = *this;
      advance();
      return tmp;
    }

    // Does not return a reference because keys and payloads are stored
    // separately.
    // If possible, use key() and payload() instead.
    V operator*() const {
      return std::make_pair(cur_leaf_->key_slots_[cur_idx_],
                            cur_leaf_->payload_slots_[cur_idx_]);
    }

    const AlexKey<T>& key() const { return cur_leaf_->get_key(cur_idx_); }

    const P& payload() const { return cur_leaf_->get_payload(cur_idx_); }

    bool is_end() const { return cur_leaf_ == nullptr; }

    bool operator==(const ConstReverseIterator& rhs) const {
      return cur_idx_ == rhs.cur_idx_ && cur_leaf_ == rhs.cur_leaf_;
    }

    bool operator!=(const ConstReverseIterator& rhs) const {
      return !(*this == rhs);
    };

   
    void initialize() {
      if (!cur_leaf_) return;
      assert(cur_idx_ >= 0);
      if (cur_idx_ >= cur_leaf_->data_capacity_) {
        cur_leaf_ = cur_leaf_->next_leaf_;
        cur_idx_ = 0;
        if (!cur_leaf_) return;
      }

      cur_bitmap_idx_ = cur_idx_ >> 6;
      cur_bitmap_data_ = cur_leaf_->bitmap_[cur_bitmap_idx_];

      // Zero out extra bits
      int bit_pos = cur_idx_ - (cur_bitmap_idx_ << 6);
      cur_bitmap_data_ &= (1ULL << bit_pos) | ((1ULL << bit_pos) - 1);

      advance();
    }

    forceinline void advance() {
      while (cur_bitmap_data_ == 0) {
        cur_bitmap_idx_--;
        if (cur_bitmap_idx_ < 0) {
          cur_leaf_ = cur_leaf_->prev_leaf_.read();
          if (cur_leaf_ == nullptr) {
            cur_idx_ = 0;
            return;
          }
          cur_idx_ = cur_leaf_->data_capacity_ - 1;
          cur_bitmap_idx_ = cur_leaf_->bitmap_size_ - 1;
        }
        cur_bitmap_data_ = cur_leaf_->bitmap_[cur_bitmap_idx_];
      }
      assert(cpu_supports_bmi());
      int bit_pos = static_cast<int>(63 - _lzcnt_u64(cur_bitmap_data_));
      cur_idx_ = (cur_bitmap_idx_ << 6) + bit_pos;
      cur_bitmap_data_ &= ~(1ULL << bit_pos);
    }
  };

  // Iterates through all nodes with pre-order traversal
  class NodeIterator {
   public:
    const self_type* index_;
    node_type* cur_node_;
    std::stack<node_type*> node_stack_;  // helps with traversal

    // Start with root as cur and all children of root in stack
    explicit NodeIterator(const self_type* index)
        : index_(index), cur_node_(index->root_node_) {
      if (cur_node_ && !cur_node_->is_leaf_) {
        auto node = static_cast<model_node_type*>(cur_node_);
        node_stack_.push(node->children_[node->num_children_ - 1]);
        for (int i = node->num_children_ - 2; i >= 0; i--) {
          if (node->children_[i] != node->children_[i + 1]) {
            node_stack_.push(node->children_[i]);
          }
        }
      }
    }

    node_type* current() const { return cur_node_; }

    node_type* next() {
      if (node_stack_.empty()) {
        cur_node_ = nullptr;
        return nullptr;
      }

      cur_node_ = node_stack_.top();
      node_stack_.pop();

      if (!cur_node_->is_leaf_) {
        auto node = static_cast<model_node_type*>(cur_node_);
        node_stack_.push(node->children_[node->num_children_ - 1]);
        for (int i = node->num_children_ - 2; i >= 0; i--) {
          if (node->children_[i] != node->children_[i + 1]) {
            node_stack_.push(node->children_[i]);
          }
        }
      }

      return cur_node_;
    }

    bool is_end() const { return cur_node_ == nullptr; }
  };
};
}  // namespace alex
