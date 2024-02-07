// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

/*
 * This file contains utility code for using the fanout tree to help ALEX
 * decide the best fanout and key partitioning scheme for ALEX nodes
 * during bulk loading and node splitting.
 */

#pragma once

#include "alex_base.h"
#include "alex_nodes.h"

namespace alex {

namespace fanout_tree {

// A node of the fanout tree
struct FTNode {
  int level;    // level in the fanout tree
  int node_id;  // node's position within its level
  double cost;
  int left_boundary;  // start position in input array that this node represents
  int right_boundary;  // end position (exclusive) in input array that this node
                       // represents
  bool use = false;
  double expected_avg_search_iterations = 0;
  double expected_avg_shifts = 0;
  double *a = nullptr;  // linear model slope
  double b = 0;  // linear model intercept
  int num_keys = 0;
};

/*** Helpers ***/

// Collect all used fanout tree nodes and sort them
inline void collect_used_nodes(const std::vector<std::vector<FTNode>>& fanout_tree,
                        int max_level,
                        std::vector<FTNode>& used_fanout_tree_nodes) {
  max_level = std::min(max_level, static_cast<int>(fanout_tree.size()) - 1);
  for (int i = 0; i <= max_level; i++) {
    auto& level = fanout_tree[i];
    for (const FTNode& tree_node : level) {
      if (tree_node.use) {
        used_fanout_tree_nodes.push_back(tree_node);
      }
    }
  }
  std::sort(used_fanout_tree_nodes.begin(), used_fanout_tree_nodes.end(),
            [&](FTNode& left, FTNode& right) {
              // this is better than comparing boundary locations
              return (left.node_id << (max_level - left.level)) <
                     (right.node_id << (max_level - right.level));
            });
}

// Starting from a complete fanout tree of a certain depth, merge tree nodes
// upwards if doing so decreases the cost.
// Returns the new best cost.
// This is a helper function for finding the best fanout in a bottom-up fashion.
template <class T, class P>
static double merge_nodes_upwards(
    int start_level, double best_cost, int num_keys, int total_keys,
    std::vector<std::vector<FTNode>>& fanout_tree) {
  for (int level = start_level; level >= 1; level--) {
    int level_fanout = 1 << level;
    bool at_least_one_merge = false;
    for (int i = 0; i < level_fanout / 2; i++) {
      if (fanout_tree[level][2*i].use && fanout_tree[level][2*i+1].use) {
        int num_node_keys = fanout_tree[level - 1][i].num_keys;
        int num_left_keys = fanout_tree[level][2 * i].num_keys;
        int num_right_keys = fanout_tree[level][2 * i + 1].num_keys;
        double merging_cost_saving =
            (fanout_tree[level][2*i].cost * num_left_keys / num_node_keys) +
            (fanout_tree[level][2*i+1].cost * num_right_keys / num_node_keys) -
            fanout_tree[level - 1][i].cost +
            (kModelSizeWeight * sizeof(AlexDataNode<T, P>) * total_keys / num_node_keys);
        if (merging_cost_saving >= 0) {
          if (fanout_tree[level][2*i].left_boundary != fanout_tree[level-1][i].left_boundary
            ||fanout_tree[level][2*i+1].right_boundary != fanout_tree[level-1][i].right_boundary)
          {continue;} //shouldn't happen. semantic issue.
          fanout_tree[level][2*i].use = false;
          fanout_tree[level][2*i+1].use = false;
          fanout_tree[level-1][i].use = true;
          best_cost -= merging_cost_saving * num_node_keys / num_keys;
          at_least_one_merge = true;
#if DEBUG_PRINT
          //std::cout << "merging\n where first l/r boundary is " << fanout_tree[level][2 * i].left_boundary
          //          << " and " << fanout_tree[level][2 * i].right_boundary << '\n'
          //          << "and second l/r boundary is " << fanout_tree[level][2 * i + 1].left_boundary
          //          << " and " << fanout_tree[level][2 * i + 1].right_boundary << '\n'
          //          << "which will use following boundary : " << fanout_tree[level - 1][i].left_boundary
          //          << ", " << fanout_tree[level - 1][i].right_boundary << '\n';
#endif
        }
      }
    }
    if (!at_least_one_merge) {
      break;
    }
  }
  return best_cost;
}

/*** Methods used when bulk loading ***/

//push node info to used_fanout_tree_nodes
//used in compute_level
template <class T, class P>
void push_node(const std::pair<AlexKey<T>, P> values[], int num_keys,
                std::vector<FTNode>& used_fanout_tree_nodes, int level,
                int max_data_node_keys, int left_boundary, int right_boundary, double& cost,
                int i, double expected_insert_frac = 0,
                bool approximate_model_computation = true,
                bool approximate_cost_computation = false) {
  LinearModel<T> model;
  AlexDataNode<T, P>::build_model(values + left_boundary,
                                  right_boundary - left_boundary, &model,
                                  approximate_model_computation);

  DataNodeStats stats;
  double node_cost = AlexDataNode<T, P>::compute_expected_cost(
      values + left_boundary, right_boundary - left_boundary,
      AlexDataNode<T, P>::kInitDensity_, expected_insert_frac, &model,
      approximate_cost_computation, &stats);
  // If the node is too big to be a data node, proactively incorporate an
  // extra tree traversal level into the cost.
  if (right_boundary - left_boundary > max_data_node_keys) {
    node_cost += kNodeLookupsWeight;
  }

  cost += node_cost * (right_boundary - left_boundary) / num_keys;
  double *slope = new double[max_key_length_]();
  std::copy(model.a_, model.a_ + max_key_length_, slope);

  used_fanout_tree_nodes.push_back(
      {level, i, node_cost, left_boundary, right_boundary, false,
      stats.num_search_iterations, stats.num_shifts, slope, model.b_,
      right_boundary - left_boundary});
}

//push node to new_level
//used in find_best_fanout_existing_node
template <class T, class P>
void push_node_from_existing (AlexDataNode<T, P>* node, AlexKey<T>** node_keys, int left_boundary,
                              int right_boundary, int num_keys, double& cost, int i,
                              std::vector<FTNode>& new_level, int fanout_tree_level, int worker_id) {
  int num_actual_keys = 0;
  LinearModel<T> model;
  LinearModelBuilder<T> builder(&model);
  for (int j = 0, it = left_boundary; it < right_boundary; j++, it++) {
    builder.add(*node_keys[it], j);
    num_actual_keys++;
  }
#if PROFILE
  auto fanout_data_train_start_time = std::chrono::high_resolution_clock::now();
  profileStats.fanout_data_train_cnt++;
#endif
  builder.build();
#if PROFILE
  auto fanout_data_train_end_time = std::chrono::high_resolution_clock::now();
  auto data_elapsed_time = std::chrono::duration_cast<std::chrono::bgTimeUnit>(fanout_data_train_end_time - fanout_data_train_start_time).count();
  profileStats.fanout_data_train_time += data_elapsed_time;
  profileStats.max_fanout_data_train_time =
    std::max(profileStats.max_fanout_data_train_time.load(), data_elapsed_time);
  profileStats.min_fanout_data_train_time =
    std::min(profileStats.min_fanout_data_train_time.load(), data_elapsed_time);
#endif
  double empirical_insert_frac = node->frac_inserts();
  DataNodeStats stats;
  double node_cost =
      AlexDataNode<T, P>::compute_expected_cost_from_existing(
          node_keys, left_boundary, right_boundary,
          AlexDataNode<T, P>::kInitDensity_, empirical_insert_frac, &model,
          &stats);

  cost += node_cost * num_actual_keys / num_keys;

  double *slope = new double[max_key_length_];
  std::copy(model.a_, model.a_ + max_key_length_, slope);
  new_level.push_back({fanout_tree_level, i, node_cost, left_boundary,
                       right_boundary, false, stats.num_search_iterations,
                       stats.num_shifts, slope, model.b_,
                       num_actual_keys});
#if DEBUG_PRINT
        alex::coutLock.lock();
        std::cout << "t" << worker_id << "'s generated thread - ";
        std::cout << "left boundary is : " <<  left_boundary << '\n';
        std::cout << "right boundary is : " << right_boundary << std::endl;
        alex::coutLock.unlock();
#endif
}

// Computes one complete level of the fanout tree.
// For example, level 3 will have 8 tree nodes, which are returned through
// used_fanout_tree_nodes.
// Assumes node has already been trained to produce a CDF value in the range [0,
// 1).
template <class T, class P>
double compute_level(const std::pair<AlexKey<T>, P> values[], int num_keys, int total_keys,
                     std::vector<FTNode>& used_fanout_tree_nodes, int level,
                     LinearModel<T> &basic_model,
                     int max_data_node_keys, double expected_insert_frac = 0,
                     bool approximate_model_computation = true,
                     bool approximate_cost_computation = false) {

  //for string key, we need to obtain model by retraining, not CDF [0,1]
  //I THINK THIS MEANS, THAT WE MAY DON'T NEED [0,1] CDF TRAINING IN FIRST PLACE. CONSIDER IT.
  int fanout = 1 << level;
  double cost = 0.0;
  LinearModel<T> newLModel(basic_model);
  newLModel.expand(fanout);
  int left_boundary = 0;
  int right_boundary = 0;
#if DEBUG_PRINT
  std::cout << "compute_level searching for boundary with fanout : " << fanout << std::endl;
#endif
  for (int i = 0; i < fanout; i++) {
    left_boundary = right_boundary;
    /* some important change is made about right_boundary.
     * We are trying to obtain the pseudofirst AlexKey where prediction position results as 'i',
     * considering the current model. Since we can't pinpoint the specific key for strings
     * and since values are all sorted (upon research) when called, 
     * we try to find the pseudofirst key that is equal or larger than position i. */
    if (i == fanout - 1) {right_boundary = num_keys;}
    else {
      //binary searching for boundary
      int left_idx = left_boundary;
      int right_idx = num_keys;
      int mid = left_idx + (right_idx - left_idx) / 2;

      while (left_idx < right_idx) {
        int predicted_pos = newLModel.predict(values[mid].first);
        if (predicted_pos <= i) {
          left_idx = mid + 1;
        } else {
          right_idx = mid;
        }
        mid = left_idx + (right_idx - left_idx) / 2;
      }
      right_boundary = left_idx; //which should be same as right_idx
    }

    if (left_boundary == right_boundary) {
      //we don't allow empty data nodes.
      right_boundary++;
    }
    if (num_keys - right_boundary < fanout - i - 1) {
      //not enough keys... put 1 keys each for remaining data node
      right_boundary = num_keys - (fanout - i - 1);
      push_node(values, num_keys, used_fanout_tree_nodes, level, max_data_node_keys,
                left_boundary, right_boundary, cost, i, expected_insert_frac,
                approximate_model_computation, approximate_cost_computation);

      for (int j = i + 1; j < fanout; j++) {
        left_boundary = right_boundary;
        right_boundary++;
        push_node(values, num_keys, used_fanout_tree_nodes, level, max_data_node_keys,
                left_boundary, right_boundary, cost, j, expected_insert_frac,
                approximate_model_computation, approximate_cost_computation);
      }
      break;
    }
    else {
      //normal case
      push_node(values, num_keys, used_fanout_tree_nodes, level, max_data_node_keys,
                left_boundary, right_boundary, cost, i, expected_insert_frac,
                approximate_model_computation, approximate_cost_computation);
    }
  }
#if DEBUG_PRINT
    //std::cout << "compute_level boundary searching finished for fanout " << fanout << '\n';
#endif
  double traversal_cost =
      kNodeLookupsWeight +
      (kModelSizeWeight * fanout *
       (sizeof(AlexDataNode<T, P>) + sizeof(void*)) * total_keys / num_keys);
#if DEBUG_PRINT
  //std::cout << "total node_cost : " << cost << ", traversal_cost : " << traversal_cost << std::endl;
#endif
  cost += traversal_cost;
  return cost;
}

// Figures out the optimal partitioning of children in a "bottom-up" fashion
// (see paper for details).
// Assumes node has already been trained to produce a CDF value in the range [0,
// 1).
// Returns the depth of the best fanout tree and the total cost of the fanout
// tree.
template <class T, class P>
std::pair<int, double> find_best_fanout_bottom_up(
    const std::pair<AlexKey<T>, P> values[], int num_keys, const AlexNode<T, P>* node,
    int total_keys, std::vector<FTNode>& used_fanout_tree_nodes, int max_fanout,
    int max_data_node_keys, int expected_min_numkey_per_data_node,
    double expected_insert_frac = 0, bool approximate_model_computation = true,
    bool approximate_cost_computation = false) {
  // Repeatedly add levels to the fanout tree until the overall cost of each
  // level starts to increase
#if DEBUG_PRINT
  //std::cout << "called find_best_fanout_bottom_up" << std::endl;
#endif
  int best_level = 0;
  double best_cost = node->cost_ + kNodeLookupsWeight;
  std::vector<double> fanout_costs;
  std::vector<std::vector<FTNode>> fanout_tree;
  fanout_costs.push_back(best_cost);
  double *slope = new double[max_key_length_]();
  fanout_tree.push_back(
      {{0, 0, best_cost, 0, num_keys, false, 0, 0, slope, 0, num_keys}});
  
  LinearModel<T> basic_model;
  LinearModelBuilder<T> basic_model_builder(&basic_model);
  for (int i = 0; i < num_keys; i++) {
    basic_model_builder.add(values[i].first, ((double) i / (num_keys-1)));
  }
  basic_model_builder.build();

  for (int fanout = 2, fanout_tree_level = 1; 
       (fanout <= max_fanout) && (num_keys / fanout > expected_min_numkey_per_data_node);
       fanout *= 2, fanout_tree_level++) {
    std::vector<FTNode> new_level;
    double cost = compute_level<T, P>(
        values, num_keys, total_keys, new_level, fanout_tree_level, basic_model,
        max_data_node_keys, expected_insert_frac, approximate_model_computation,
        approximate_cost_computation);
    fanout_costs.push_back(cost);
    if (fanout_costs.size() >= 3 &&
        fanout_costs[fanout_costs.size() - 1] >
            fanout_costs[fanout_costs.size() - 2] &&
        fanout_costs[fanout_costs.size() - 2] >
            fanout_costs[fanout_costs.size() - 3]) {
      for (const FTNode& tree_node : new_level) {delete[] tree_node.a;}
      break;
    }
    if (cost < best_cost) {
      best_cost = cost;
      best_level = fanout_tree_level;
    }
    fanout_tree.push_back(new_level);
  }
  for (FTNode& tree_node : fanout_tree[best_level]) {
    tree_node.use = true;
  }

  // Merge nodes to improve cost
  best_cost = merge_nodes_upwards<T, P>(best_level, best_cost, num_keys,
                                        total_keys, fanout_tree);

  collect_used_nodes(fanout_tree, best_level, used_fanout_tree_nodes);

  for (const std::vector<FTNode>& level_fanout_tree : fanout_tree) {
    for (const FTNode& tree_node : level_fanout_tree) {
      if (!tree_node.use) {delete[] tree_node.a;}
    }
  }
#if DEBUG_PRINT
  std::cout << "find_best_fanout_bottom_up finished" << std::endl;
#endif
  return std::make_pair(best_level, best_cost);
}

/*** Method used when splitting after a node becomes full due to inserts ***/

// Figures out the optimal partitioning for the keys in an existing data node.
// Limit the maximum allowed fanout of the partitioning using max_fanout.
// This mirrors the logic of finding the best fanout "bottom-up" when bulk
// loading.
// Returns the depth of the best fanout tree.
// unlike original ALEX, we give an ordered sets of key and payload as parameter
template <class T, class P>
std::pair<int, double *> find_best_fanout_existing_node(
                                   AlexDataNode<T, P>* node, AlexKey<T>** node_keys, 
                                   LinearModel<T> &tmp_model, int total_keys, int num_keys,
                                   std::vector<FTNode>& used_fanout_tree_nodes, int max_fanout,
                                   uint64_t worker_id) {
  // Repeatedly add levels to the fanout tree until the overall cost of each
  // level starts to increase
#if PROFILE
  profileStats.find_best_fanout_existing_node_call_cnt++;
  auto find_fanout_existing_node_start_time = std::chrono::high_resolution_clock::now();
#endif
  int best_level = 0;
  double *best_param = new double[max_key_length_ + 1]();
  double best_cost = std::numeric_limits<double>::max();
  std::vector<double> fanout_costs;
  std::vector<std::vector<FTNode>> fanout_tree;

  for (int fanout = 1, fanout_tree_level = 0; fanout <= max_fanout;
       fanout *= 2, fanout_tree_level++) {
#if DEBUG_PRINT
    alex::coutLock.lock();
    std::cout << "t" << worker_id << "'s generated thread - ";
    std::cout << "find_best_fanout_existing_node searching for boundary with fanout : " << fanout << std::endl;
    alex::coutLock.unlock();
#endif

    std::vector<FTNode> new_level;
    double cost = 0.0;

    if (fanout != 1) {
      tmp_model.expand(2.0);
    }

#if DEBUG_PRINT
    alex::coutLock.lock();
    std::cout << "t" << worker_id << "'s generated thread - ";
    std::cout << "first key predicted as" << tmp_model.predict_double(*node_keys[0]) << '\n';
    std::cout << "last key predicted as" << tmp_model.predict_double(*node_keys[num_keys-1]) << std::endl;
    alex::coutLock.unlock();
#endif

    int left_boundary = 0;
    int right_boundary = 0;
#if PROFILE
      auto fanout_batch_stat_start_time = std::chrono::high_resolution_clock::now();
      profileStats.fanout_batch_stat_cnt++;
#endif
    for (int i = 0; i < fanout; i++) {
      left_boundary = right_boundary;
      if (i == fanout - 1) {right_boundary = num_keys;}
      else {
        //binary searching for boundary
        int left_idx = left_boundary;
        int right_idx = num_keys;
        int mid = left_idx + (right_idx - left_idx) / 2;

        while (left_idx < right_idx) {
          int predicted_pos = tmp_model.predict(*node_keys[mid]);
          if (predicted_pos <= i) {
            left_idx = mid + 1;
          } else {
            right_idx = mid;
          }
          mid = left_idx + (right_idx - left_idx) / 2;
        }
        right_boundary = left_idx;
      }

      if (left_boundary == right_boundary) {
        right_boundary++;
      }
      if (num_keys - right_boundary < fanout - i - 1) {
        //not enough keys... put 1 keys each for remaining data node
        right_boundary = num_keys - (fanout - i - 1);
        push_node_from_existing(node, node_keys, left_boundary, right_boundary, num_keys,
                                cost, i, new_level, fanout_tree_level, worker_id);
        for (int j = i + 1; j < fanout; j++) {
          left_boundary = right_boundary;
          right_boundary++;
          push_node_from_existing(node, node_keys, left_boundary, right_boundary, num_keys,
                                  cost, j, new_level, fanout_tree_level, worker_id);
        }
        break;
      }
      else {
        //normal case
        push_node_from_existing(node, node_keys, left_boundary, right_boundary, num_keys,
                                cost, i, new_level, fanout_tree_level, worker_id);
      }
    }
#if PROFILE
      auto fanout_batch_stat_end_time = std::chrono::high_resolution_clock::now();
      auto batch_elapsed_time = std::chrono::duration_cast<std::chrono::bgTimeUnit>(fanout_batch_stat_end_time - fanout_batch_stat_start_time).count();
      profileStats.fanout_batch_stat_time += batch_elapsed_time;
      profileStats.max_fanout_batch_stat_time =
        std::max(profileStats.max_fanout_batch_stat_time.load(), batch_elapsed_time);
      profileStats.min_fanout_batch_stat_time =
        std::min(profileStats.min_fanout_batch_stat_time.load(), batch_elapsed_time);
#endif
    // model weight reflects that it has global effect, not local effect
    double traversal_cost =
        kNodeLookupsWeight +
        (kModelSizeWeight * fanout *
         (sizeof(AlexDataNode<T, P>) + sizeof(void*)) * total_keys / num_keys);
    cost += traversal_cost;
    fanout_costs.push_back(cost);
    // stop after expanding fanout increases cost twice in a row
    if (fanout_costs.size() >= 3 &&
        fanout_costs[fanout_costs.size() - 1] >
            fanout_costs[fanout_costs.size() - 2] &&
        fanout_costs[fanout_costs.size() - 2] >
            fanout_costs[fanout_costs.size() - 3]) {
      for (const FTNode& tree_node : new_level) {delete[] tree_node.a;}
      break;
    }
    if (cost < best_cost) {
      best_cost = cost;
      best_level = fanout_tree_level;
      std::copy(tmp_model.a_, tmp_model.a_ + max_key_length_, best_param);
      best_param[max_key_length_] = tmp_model.b_;
    }
    fanout_tree.push_back(new_level);

#if DEBUG_PRINT
    alex::coutLock.lock();
    std::cout << "t" << worker_id << "'s generated thread - ";
    std::cout << "find_best_fanout_existing_node boundary searching finished for fanout " << fanout << std::endl;
    alex::coutLock.unlock();
#endif
  }

#if DEBUG_PRINT
    alex::coutLock.lock();
    std::cout << "t" << worker_id << "'s generated thread - ";
    std::cout << "chosen best level is : " << (1 << best_level) << std::endl;
    alex::coutLock.unlock();
#endif

  for (FTNode& tree_node : fanout_tree[best_level]) {
    tree_node.use = true;
  }

  for (const std::vector<FTNode>& level_fanout_tree : fanout_tree) {
    for (const FTNode& tree_node : level_fanout_tree) {
      if (!tree_node.use) {delete[] tree_node.a;}
    }
  }

  // Merge nodes to improve cost
  merge_nodes_upwards<T, P>(best_level, best_cost, num_keys, total_keys, fanout_tree);

  collect_used_nodes(fanout_tree, best_level, used_fanout_tree_nodes);
#if PROFILE
  auto find_fanout_existing_node_end_time = std::chrono::high_resolution_clock::now();
  auto elapsed_time = std::chrono::duration_cast<std::chrono::bgTimeUnit>(find_fanout_existing_node_end_time - find_fanout_existing_node_start_time).count();
  profileStats.find_best_fanout_existing_node_time += elapsed_time;
  profileStats.max_find_best_fanout_existing_node_time =
    std::max(profileStats.max_find_best_fanout_existing_node_time.load(), elapsed_time);
  profileStats.min_find_best_fanout_existing_node_time =
    std::min(profileStats.min_find_best_fanout_existing_node_time.load(), elapsed_time);
#endif

  return {best_level, best_param};
}

}  // namespace fanout_tree

}  // namespace alex
