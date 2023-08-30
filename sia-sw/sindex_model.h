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

#if !defined(SINDEX_MODEL_H)
#define SINDEX_MODEL_H

#include "sia.hpp"

namespace sindex {

static const size_t DESIRED_TRAINING_KEY_N = 10000000;

inline void incremental_model_prepare(
  const std::vector<std::pair<double *, size_t>> &delta_model_keys,
  const std::vector<std::pair<double *, size_t>> &inserted_model_keys,
  double *weights, size_t feature_len, double **cached_matrix_ptr
){
  for (size_t w_i = 0; w_i < feature_len + 1; w_i++) weights[w_i] = 0;
  if (delta_model_keys.size() == 0) return;

  assert(cached_matrix != *cached_matrix_ptr);

  int delta_m = delta_model_keys.size();
  int delta_n = feature_len + 1;
  std::vector<double> delta_a(delta_m * delta_n, 0);
  std::vector<double> delta_b(std::max(delta_m, delta_n), 0);

  for (int sample_i = 0; sample_i < delta_m; sample_i++) {
    for (size_t i=0; i<delta_n - 1; i++){
      delta_a[sample_i * delta_n + i] = delta_model_keys[sample_i].first[i];
    }
    delta_a[sample_i * delta_n + delta_n - 1] = 1;
    delta_b[sample_i] = delta_model_keys[sample_i].second;
  }

  int inserted_m = inserted_model_keys.size();
  int inserted_n = delta_n;
  std::vector<double> inserted_a(inserted_m * inserted_n, 0);
  std::vector<double> inserted_b(std::max(inserted_m, inserted_n), 0);

  for (int sample_i = 0; sample_i < inserted_m; sample_i++) {
    for (size_t i=0; i<inserted_n - 1; i++) {
      inserted_a[sample_i * inserted_n + i] = inserted_model_keys[sample_i].first[i];
    }
    inserted_a[sample_i * inserted_n + inserted_n - 1] = 1;
    inserted_b[sample_i] = inserted_model_keys[sample_i].second;
  }

  std::vector<double> answers(delta_n, 0);

  // Trigger incremental training
  incremental_training(delta_a.data(), delta_m,
                      delta_b.data(), delta_n,
                      inserted_a.data(), inserted_m,
                      inserted_b.data(), inserted_n,
                      cached_matrix_ptr,
                      answers.data());

  // set weights of useful features
  memcpy(weights, answers.data(), delta_n * sizeof(double));
  return;
}

inline void model_prepare(const std::vector<double *> &model_key_ptrs,
                          const std::vector<size_t> &positions, double *weights,
                          size_t feature_len, double **cached_matrix_ptr) {
  assert(model_key_ptrs.size() == positions.size());
  // set weights to all zero
  for (size_t w_i = 0; w_i < feature_len + 1; w_i++) weights[w_i] = 0;
  if (positions.size() == 0) return;
  if (positions.size() == 1) {
    weights[feature_len] = positions[0];
    return;
  }
  
  int m = model_key_ptrs.size();  // number of samples
  int n = feature_len + 1;        // number of features
  std::vector<double> a(m * n, 0);
  std::vector<double> b(std::max(m, n), 0);

  for (int sample_i = 0; sample_i < m; ++sample_i) {
    for (size_t i = 0; i < n - 1; i++) {
      a[sample_i * n + i] = model_key_ptrs[sample_i][i];
    }
    a[sample_i * n + n - 1] = 1;
    b[sample_i] = positions[sample_i];
  }

  *cached_matrix_ptr = (double *) malloc(sizeof(double) * n * n * 2);
  std::vector<double> answers(n, 0);

  // Trigger entire training
  entire_training(a.data(), m,
                  b.data(), n,
                  cached_matrix_ptr,
                  answers.data());

  memcpy(weights, b.data(), n * sizeof(double));

  return;
}

inline size_t model_predict(double *weights, const double *model_key,
                            size_t feature_len) {
  if (feature_len == 1) {
    double res = weights[0] * model_key[0] + weights[1];
    return res > 0 ? res : 0;
  } else {
    double res = dot_product(weights, model_key, feature_len);
    res += weights[feature_len];
    return res > 0 ? res : 0;
  }
}

}  // namespace sindex

#endif  // SINDEX_MODEL_H
