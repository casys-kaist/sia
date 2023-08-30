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

#include "sindex_group.h"
#include <climits>
#include <sstream>

#if !defined(SINDEX_GROUP_IMPL_H)
#define SINDEX_GROUP_IMPL_H

namespace sindex {

template <class key_t, class val_t, bool seq, size_t max_model_n>
Group<key_t, val_t, seq, max_model_n>::Group() {}

template <class key_t, class val_t, bool seq, size_t max_model_n>
Group<key_t, val_t, seq, max_model_n>::~Group() {}

template <class key_t, class val_t, bool seq, size_t max_model_n>
void Group<key_t, val_t, seq, max_model_n>::init(
    const typename std::vector<key_t>::const_iterator &keys_begin,
    const typename std::vector<val_t>::const_iterator &vals_begin,
    uint32_t array_size) {
  init(keys_begin, vals_begin, 1, array_size);
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
void Group<key_t, val_t, seq, max_model_n>::init(
    const typename std::vector<key_t>::const_iterator &keys_begin,
    const typename std::vector<val_t>::const_iterator &vals_begin,
    uint32_t model_n, uint32_t array_size) {
  assert(array_size > 0);
  this->pivot = *keys_begin;
  this->array_size = array_size;
  this->capacity = array_size * seq_insert_reserve_factor;
  this->model_n = model_n;
  data = new record_t[this->capacity]();
  buffer = new buffer_t();

  for (size_t rec_i = 0; rec_i < array_size; rec_i++) {
    data[rec_i].first = *(keys_begin + rec_i);
    data[rec_i].second = wrapped_val_t(*(vals_begin + rec_i));
  }

  for (size_t rec_i = 1; rec_i < array_size; rec_i++) {
    assert(data[rec_i].first >= data[rec_i - 1].first);
  }

  init_models(model_n);
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline result_t Group<key_t, val_t, seq, max_model_n>::get(const key_t &key,
                                                           val_t &val) {
  #ifdef LATENCY_BREAKDOWN
  bool res;
  if (get_from_array(key, val)) {
    return result_t::ok;
  }

  struct timespec begin_t, end_t;
  clock_gettime(CLOCK_MONOTONIC, &begin_t);
  res = get_from_buffer(key, val, buffer);
  clock_gettime(CLOCK_MONOTONIC, &end_t);
  lt.buffer_search_sum += GET_INTERVAL(begin_t, end_t);
  lt.buffer_search_count += 1;

  if (res) {
    return result_t::ok;
  }
  if (buffer_temp && get_from_buffer(key, val, buffer_temp)) {
    return result_t::ok;
  }
  return result_t::failed;
  
  #else

  if (get_from_array(key, val)) {
    return result_t::ok;
  }
  if (get_from_buffer(key, val, buffer)) {
    return result_t::ok;
  }
  if (buffer_temp && get_from_buffer(key, val, buffer_temp)) {
    return result_t::ok;
  }
  return result_t::failed;
  #endif
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline result_t Group<key_t, val_t, seq, max_model_n>::put(
    const key_t &key, const val_t &val, const uint32_t worker_id) {
  result_t res;
  res = update_to_array(key, val, worker_id);
  if (res == result_t::ok || res == result_t::retry) {
    return res;
  }

  if (likely(buffer_temp == nullptr)) {
    if (buf_frozen) {
      return result_t::retry;
    }
    insert_to_buffer(key, val, buffer);
    return result_t::ok;
  } else {
    if (update_to_buffer(key, val, buffer)) {
      return result_t::ok;
    }
    insert_to_buffer(key, val, buffer_temp);
    return result_t::ok;
  }
  COUT_N_EXIT("put should not fail!");
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline result_t Group<key_t, val_t, seq, max_model_n>::insert(
    const key_t &key, const val_t &val, const uint32_t worker_id) {
  result_t res;

  if (likely(buffer_temp == nullptr)) {
    if (buf_frozen) {
      return result_t::retry;
    }
    insert_to_buffer(key, val, buffer);
    return result_t::ok;
  } else {
    insert_to_buffer(key, val, buffer_temp);
    return result_t::ok;
  }
  COUT_N_EXIT("UNREACHABLE");
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline result_t Group<key_t, val_t, seq, max_model_n>::remove(
    const key_t &key) {
  if (remove_from_array(key)) {
    return result_t::ok;
  }
  if (remove_from_buffer(key, buffer)) {
    return result_t::ok;
  }
  if (buffer_temp && remove_from_buffer(key, buffer_temp)) {
    return result_t::ok;
  }
  return result_t::failed;
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline size_t Group<key_t, val_t, seq, max_model_n>::scan(
    const key_t &begin, const size_t n,
    std::vector<std::pair<key_t, val_t>> &result) {
  return buffer_temp ? scan_3_way(begin, n, key_t::max(), result)
                     : scan_2_way(begin, n, key_t::max(), result);
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline size_t Group<key_t, val_t, seq, max_model_n>::range_scan(
    const key_t &begin, const key_t &end,
    std::vector<std::pair<key_t, val_t>> &result) {
  size_t old_size = result.size();
  if (buffer_temp) {
    scan_3_way(begin, std::numeric_limits<size_t>::max(), end, result);
  } else {
    scan_2_way(begin, std::numeric_limits<size_t>::max(), end, result);
  }
  return result.size() - old_size;
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
float Group<key_t, val_t, seq, max_model_n>::mean_error_est() const {
  // we did not disable seq op here so array_size can be changed.
  // however, we only need an estimated error
  uint32_t array_size = this->array_size;
  size_t model_data_size = array_size - pos_last_pivot;
  std::vector<float> model_keys(model_data_size * feature_len);
  std::vector<float *> model_key_ptrs(model_data_size);
  std::vector<size_t> positions(model_data_size);
  for (size_t rec_i = 0; rec_i < model_data_size; rec_i++) {
    data[pos_last_pivot + rec_i].first.get_model_key(
        prefix_len, feature_len, model_keys.data() + feature_len * rec_i);
    model_key_ptrs[rec_i] = model_keys.data() + feature_len * rec_i;
    positions[rec_i] = pos_last_pivot + rec_i;
  }
  float error_last_model_now =
      get_error_bound(model_n - 1, model_key_ptrs, positions);

  if (model_n == 1) {
    return error_last_model_now;
  } else {
    // est previous last model error
    size_t model_data_size_prev_est = pos_last_pivot / (model_n - 1);
    if (model_data_size_prev_est > model_data_size) {
      model_data_size_prev_est = model_data_size;
    }
    model_key_ptrs.resize(model_data_size_prev_est);
    positions.resize(model_data_size_prev_est);
    float error_last_model_prev =
        get_error_bound(model_n - 1, model_key_ptrs, positions);

    return (error_last_model_now - error_last_model_prev) / model_n;
  }
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
float Group<key_t, val_t, seq, max_model_n>::get_mean_error() const {
  floatmean_err = 0;
  int err_max = 0, err_min = 0;
  for (size_t m_i = 0; m_i < model_n; ++m_i) {
    get_model_error(m_i, err_max, err_min);
    mean_err += (err_max < err_min ? INT_MAX : err_max - err_min + 1);
  }
  return mean_err / model_n;
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
Group<key_t, val_t, seq, max_model_n>
    *Group<key_t, val_t, seq, max_model_n>::compact_phase_1() {
  if (seq) {  // disable seq seq
    disable_seq_insert_opt();
  }

  buf_frozen = true;
  memory_fence();
  rcu_barrier();
  buffer_temp = new buffer_t();

  // now merge sort into a new array and train models
  Group *new_group = new Group();

  new_group->pivot = pivot;
  std::vector<std::pair<key_t, size_t>> delta_buffer_vector = merge_refs(new_group->data, new_group->array_size, new_group->capacity);
  if (seq) {  // mark capacity as negative to let seq insert not insert to buf
    new_group->disable_seq_insert_opt();
  }

  // Copy cached matrix
  std::copy(std::begin(cached_r), std::end(cached_r), std::begin(new_group->cached_r));

  new_group->feature_len = feature_len;
  if (!new_group->incremental_training(model_n, delta_buffer_vector)) {
    //new_group->incremental_training(model_n, delta_buffer_vector);
    new_group->init_models(model_n);
  }
  new_group->buffer = buffer_temp;
  new_group->next = next;

  return new_group;
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline void Group<key_t, val_t, seq, max_model_n>::compact_phase_2() {
  for (size_t rec_i = 0; rec_i < array_size; ++rec_i) {
    data[rec_i].second.replace_pointer();
  }

  if (seq) {
    // enable seq seq, because now all threads only see new node
    enable_seq_insert_opt();
  }
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
void Group<key_t, val_t, seq, max_model_n>::free_data() {
  delete[] data;
  data = nullptr;
}
template <class key_t, class val_t, bool seq, size_t max_model_n>
void Group<key_t, val_t, seq, max_model_n>::free_buffer() {
  delete buffer;
  buffer = nullptr;
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline size_t Group<key_t, val_t, seq, max_model_n>::locate_model(
    const key_t &key) {
  assert(model_n >= 1);

  int model_i = 0;
  while (model_i < model_n - 1 &&
         !key_less_than((uint8_t *)&key + prefix_len,
                        get_model_pivot(model_i + 1), feature_len)) {
    model_i++;
  }
  return model_i;
}

// semantics: atomically read the value
// only when the key exists and the record (record_t) is not logical removed,
// return true on success
template <class key_t, class val_t, bool seq, size_t max_model_n>
inline bool Group<key_t, val_t, seq, max_model_n>::get_from_array(
    const key_t &key, val_t &val) {
  size_t pos = get_pos_from_array(key);
  return pos != array_size &&  // position is valid (not out-of-range)
         data[pos].first ==
             key &&  // key matches, must use full key comparison here
         data[pos].second.read(val);  // value is not removed
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline result_t Group<key_t, val_t, seq, max_model_n>::update_to_array(
    const key_t &key, const val_t &val, const uint32_t worker_id) {
  if (seq) {
    seq_lock();
    size_t pos = get_pos_from_array(key);
    if (pos != array_size) {  // position is valid (not out-of-range)
      seq_unlock();
      return (/* key matches */ data[pos].first == key &&
              /* record updated */ data[pos].second.update(val))
                 ? result_t::ok
                 : result_t::failed;
    } else {                      // might append
      if (buffer->size() == 0) {  // buf is empty
        if (capacity < 0) {
          seq_unlock();
          return result_t::retry;
        }

        if ((int32_t)array_size == capacity) {
          record_t *prev_data = nullptr;
          capacity = array_size * seq_insert_reserve_factor;
          record_t *new_data = new record_t[capacity]();
          memcpy(new_data, data, array_size * sizeof(record_t));
          prev_data = data;
          data = new_data;

          data[pos].first = key;
          data[pos].second = wrapped_val_t(val);
          array_size++;
          seq_unlock();

          rcu_barrier(worker_id);
          memory_fence();
          delete[] prev_data;
          prev_data = nullptr;
          return result_t::ok;
        } else {
          data[pos].first = key;
          data[pos].second = wrapped_val_t(val);
          array_size++;
          seq_unlock();
          return result_t::ok;
        }
      } else {
        seq_unlock();
        return result_t::failed;
      }
    }
  } else {  // no seq
    size_t pos = get_pos_from_array(key);
    return pos != array_size && data[pos].first == key &&
                   data[pos].second.update(val)
               ? result_t::ok
               : result_t::failed;
  }
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline bool Group<key_t, val_t, seq, max_model_n>::remove_from_array(
    const key_t &key) {
  size_t pos = get_pos_from_array(key);
  return pos != array_size &&        // position is valid (not out-of-range)
         data[pos].first == key &&   // key matches
         data[pos].second.remove();  // value is not removed and is updated
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline size_t Group<key_t, val_t, seq, max_model_n>::get_pos_from_array(
    const key_t &key) {
  #ifdef LATENCY_BREAKDOWN
  struct timespec begin_t, end_t;
  clock_gettime(CLOCK_MONOTONIC, &begin_t);

  size_t model_i = locate_model(key);
  size_t pos;
  int error_min, error_max;
  predict_last(model_i, key, pos, error_min, error_max);

  clock_gettime(CLOCK_MONOTONIC, &end_t);
  lt.inference_sum += GET_INTERVAL(begin_t, end_t);
  lt.inference_count += 1;

  clock_gettime(CLOCK_MONOTONIC, &begin_t);
  size_t search_begin = unlikely((error_min < 0 && (error_min + pos) > pos))
                            ? 0
                            : pos + error_min;
  size_t search_end = pos + error_max + 1;
  if (error_min > error_max) {
    search_begin = 0;
    search_end = array_size;
  }
  size_t res = binary_search_key(key, pos, search_begin, search_end);
  clock_gettime(CLOCK_MONOTONIC, &end_t);
  lt.linear_search_sum += GET_INTERVAL(begin_t, end_t);
  lt.linear_search_count += 1;

  return res;

  #else

  size_t model_i = locate_model(key);
  size_t pos;
  int error_min, error_max;
  predict_last(model_i, key, pos, error_min, error_max);
  size_t search_begin = unlikely((error_min < 0 && (error_min + pos) > pos))
                            ? 0
                            : pos + error_min;
  size_t search_end = pos + error_max + 1;
  if (error_min > error_max) {
    search_begin = 0;
    search_end = array_size;
  }

  return binary_search_key(key, pos, search_begin, search_end);
  #endif
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline size_t Group<key_t, val_t, seq, max_model_n>::binary_search_key(
    const key_t &key, size_t pos, size_t search_begin, size_t search_end) {
  // search within the range
  if (unlikely(search_begin > array_size)) {
    search_begin = array_size;
  }
  if (unlikely(search_end > array_size)) {
    search_end = array_size;
  }
  size_t mid = pos >= search_begin && pos < search_end
                   ? pos
                   : (search_begin + search_end) / 2;
  while (search_end != search_begin) {
    if (data[mid].first.less_than(key, prefix_len, feature_len)) {
      search_begin = mid + 1;
    } else {
      search_end = mid;
    }
    mid = (search_begin + search_end) / 2;
  }
  assert(search_begin == search_end);
  assert(search_begin == mid);

  return mid;
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline size_t Group<key_t, val_t, seq, max_model_n>::exponential_search_key(
    const key_t &key, size_t pos) const {
  return exponential_search_key(data, array_size, key, pos);
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline size_t Group<key_t, val_t, seq, max_model_n>::exponential_search_key(
    record_t *const data, uint32_t array_size, const key_t &key,
    size_t pos) const {
  if (array_size == 0) return 0;
  pos = (pos >= array_size ? (array_size - 1) : pos);
  assert(pos < array_size);

  int begin_i = 0, end_i = array_size;
  size_t step = 1;

  if (!key.less_than(data[pos].first, prefix_len, feature_len)) {
    begin_i = pos;
    end_i = begin_i + step;
    while (end_i < (int)array_size &&
           !key.less_than(data[end_i].first, prefix_len, feature_len)) {
      step *= 2;
      begin_i = end_i;
      end_i = begin_i + step;
    }
    if (end_i >= (int)array_size) {
      end_i = array_size - 1;
    }
  } else {
    end_i = pos;
    begin_i = end_i - step;
    while (begin_i >= 0 &&
           key.less_than(data[begin_i].first, prefix_len, feature_len)) {
      step *= 2;
      end_i = begin_i;
      begin_i = end_i - step;
    }
    if (begin_i < 0) {
      begin_i = 0;
    }
  }

  assert(begin_i >= 0);
  assert(end_i < (int)array_size);
  assert(begin_i <= end_i);

  // the real range is [begin_i, end_i], both inclusive.
  // we add 1 to end_i in order to find the insert position when the given key
  // is not exist
  end_i++;
  // find the largest position whose key equal to the given key
  while (end_i > begin_i) {
    // here the +1 term is used to avoid the infinte loop
    // where (end_i = begin_i + 1 && mid = begin_i && data[mid].first <= key)
    int mid = (begin_i + end_i) >> 1;
    if (data[mid].first.less_than(key, prefix_len, feature_len)) {
      begin_i = mid + 1;
    } else {
      // we should assign end_i with mid (not mid+1) in case infinte loop
      end_i = mid;
    }
  }

  assert(end_i == begin_i);
  assert(data[end_i].first == key || end_i == 0 || end_i == (int)array_size ||
         (data[end_i - 1].first < key && data[end_i].first > key));

  return end_i;
}

// semantics: atomically read the value
// only when the key exists and the record (record_t) is not logical removed,
// return true on success
template <class key_t, class val_t, bool seq, size_t max_model_n>
inline bool Group<key_t, val_t, seq, max_model_n>::get_from_buffer(
    const key_t &key, val_t &val, buffer_t *buffer) {
  return buffer->get(key, val);
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline bool Group<key_t, val_t, seq, max_model_n>::update_to_buffer(
    const key_t &key, const val_t &val, buffer_t *buffer) {
  return buffer->update(key, val);
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline void Group<key_t, val_t, seq, max_model_n>::insert_to_buffer(
    const key_t &key, const val_t &val, buffer_t *buffer) {
  buffer->insert(key, val);
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline bool Group<key_t, val_t, seq, max_model_n>::remove_from_buffer(
    const key_t &key, buffer_t *buffer) {
  return buffer->remove(key);
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline bool Group<key_t, val_t, seq, max_model_n>::incremental_training(
  uint32_t model_n,
  std::vector<std::pair<key_t, size_t>> delta_vector
) {
  uint8_t before_feature_len = feature_len;
  init_feature_length();
  uint8_t after_feature_len = feature_len;
  if (before_feature_len != after_feature_len) return false;
  return incremental_training(model_n, delta_vector, prefix_len, feature_len);
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline bool Group<key_t, val_t, seq, max_model_n>::incremental_training(
  uint32_t model_n,
  std::vector<std::pair<key_t, size_t>> delta_vector,
  size_t p_len,
  size_t f_len
) {
  assert(model_n == 1);
  this->model_n = model_n;
  prefix_len = p_len;
  feature_len = f_len;

  size_t delta_size = delta_vector.size();
  std::vector<float> delta_model_keys(delta_size * feature_len);
  std::vector<std::pair<float *, size_t>> delta_key_vector(delta_size);

  for (size_t rec_i = 0; rec_i < delta_size; rec_i++) {
    delta_vector[rec_i].first.get_model_key(
      prefix_len, feature_len, delta_model_keys.data() + rec_i * feature_len);
    delta_key_vector[rec_i] = std::make_pair(delta_model_keys.data() + rec_i * feature_len, delta_vector[rec_i].second);
  }

  size_t inserted_size = array_size;
  std::vector<float> inserted_model_keys(inserted_size * feature_len);
  std::vector<std::pair<float, size_t>> inserted_key_vector(inserted_size);

  for (size_t rec_i = 0; rec_i < inserted_size; rec_i++) {
    data[rec_i].first.get_model_key(
      prefix_len, feature_len, inserted_model_keys.data() + rec_i * feature_len);
    inserted_key_vector[rec_i] = std::make_pair(inserted_model_keys.data() + rec_i * feature_len, rec_i);
  }

  set_model_pivot(0, data[0].first);
  incremental_model_prepare(delta_key_vector, inserted_key_vector, get_model(0), feature_len, &cached_r[0]);
  pos_last_pivot = 0;
  return true;
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
void Group<key_t, val_t, seq, max_model_n>::init_models(uint32_t model_n) {
  // not need to init prefix and models every time init_models
  init_feature_length();
  init_models(model_n, prefix_len, feature_len);
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
void Group<key_t, val_t, seq, max_model_n>::init_models(uint32_t model_n,
                                                        size_t p_len,
                                                        size_t f_len) {
  assert(model_n == 1);
  this->model_n = model_n;
  prefix_len = p_len;
  feature_len = f_len;

  size_t records_per_model = array_size / model_n;
  size_t trailing_n = array_size - records_per_model * model_n;
  size_t begin = 0;
  for (size_t model_i = 0; model_i < model_n; ++model_i) {
    size_t end = begin + records_per_model;
    if (trailing_n > 0) {
      end++;
      trailing_n--;
    }
    assert(end <= array_size);
    assert((model_i == model_n - 1 && end == array_size) ||
           model_i < model_n - 1);

    set_model_pivot(model_i, data[begin].first);
    train_model(model_i, begin, end);
    begin = end;
    if (model_i == model_n - 1) pos_last_pivot = begin;
  }
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
void Group<key_t, val_t, seq, max_model_n>::init_feature_length() {
  const size_t key_size = sizeof(key_t);
  if (array_size < 2) {
    prefix_len = key_size;
    return;
  }

  prefix_len = common_prefix_length(0, (uint8_t *)&data[0].first, key_size,
                                    (uint8_t *)&data[1].first, key_size);
  size_t max_adjacent_prefix = prefix_len;

  for (size_t k_i = 2; k_i < array_size; ++k_i) {
    prefix_len =
        common_prefix_length(0, (uint8_t *)&data[k_i - 1].first, prefix_len,
                             (uint8_t *)&data[k_i].first, key_size);
    size_t adjacent_prefix =
        common_prefix_length(prefix_len, (uint8_t *)&data[k_i - 1].first,
                             key_size, (uint8_t *)&data[k_i].first, key_size);
    assert(adjacent_prefix <= sizeof(key_t) - prefix_len);
    if (adjacent_prefix < sizeof(key_t) - prefix_len) {
      max_adjacent_prefix =
          std::max(max_adjacent_prefix, prefix_len + adjacent_prefix);
    }
  }
  feature_len = max_adjacent_prefix - prefix_len + 1;
  assert(prefix_len <= sizeof(key_t));
  assert(feature_len <= sizeof(key_t));
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline float Group<key_t, val_t, seq, max_model_n>::train_model(size_t model_i,
                                                                 size_t begin,
                                                                 size_t end) {
  assert(end >= begin);
  assert(array_size >= end);

  size_t model_data_size = end - begin;
  std::vector<float> model_keys(model_data_size * feature_len);
  std::vector<float*> model_key_ptrs(model_data_size);
  std::vector<size_t> positions(model_data_size);

  for (size_t rec_i = 0; rec_i < model_data_size; rec_i++) {
    data[begin + rec_i].first.get_model_key(
        prefix_len, feature_len, model_keys.data() + rec_i * feature_len);
    model_key_ptrs[rec_i] = model_keys.data() + rec_i * feature_len;
    positions[rec_i] = begin + rec_i;
  }

  prepare_last(model_i, model_key_ptrs, positions);
  int err_max = 0, err_min = 0;
  get_model_error(model_i, err_max, err_min);
  return err_max < err_min ? INT_MAX : err_max - err_min;
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline void Group<key_t, val_t, seq, max_model_n>::seq_lock() {
  while (true) {
    uint8_t expected = 0;
    uint8_t desired = 1;
    if (likely(cmpxchgb((uint8_t *)&lock, expected, desired) == expected)) {
      return;
    }
  }
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline void Group<key_t, val_t, seq, max_model_n>::seq_unlock() {
  asm volatile("" : : : "memory");
  lock = 0;
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline void Group<key_t, val_t, seq, max_model_n>::enable_seq_insert_opt() {
  seq_lock();
  capacity = -capacity;
  INVARIANT(capacity > 0);
  seq_unlock();
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline void Group<key_t, val_t, seq, max_model_n>::disable_seq_insert_opt() {
  seq_lock();
  capacity = -capacity;
  INVARIANT(capacity < 0);
  seq_unlock();
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline std::vector<std::pair<key_t, size_t>> Group<key_t, val_t, seq, max_model_n>::merge_refs(
    record_t *&new_data, uint32_t &new_array_size,
    int32_t &new_capacity) const {
  size_t est_size = array_size + buffer->size();
  new_capacity = est_size * seq_insert_reserve_factor;
  new_data = new record_t[new_capacity]();
  return merge_refs_internal(new_data, new_array_size);
  assert((int32_t)new_array_size <= new_capacity);
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline void Group<key_t, val_t, seq, max_model_n>::merge_refs_n_split(
    record_t *&new_data_1, uint32_t &new_array_size_1, int32_t &new_capacity_1,
    record_t *&new_data_2, uint32_t &new_array_size_2, int32_t &new_capacity_2,
    const key_t &key) const {
  uint32_t intermediate_size;
  uint32_t est_size = array_size + buffer->size();

  new_capacity_1 = (est_size / 2) * seq_insert_reserve_factor;
  new_capacity_1 =
      (int32_t)est_size > new_capacity_1 ? est_size : new_capacity_1;

  record_t *intermediate = new record_t[new_capacity_1]();
  merge_refs_internal(intermediate, intermediate_size);

  uint32_t split_pos = exponential_search_key(intermediate, intermediate_size,
                                              key, intermediate_size / 2);
  assert(split_pos != intermediate_size &&
         intermediate[split_pos].first >= key);

  new_array_size_1 = split_pos;
  new_data_1 = intermediate;

  new_array_size_2 = intermediate_size - split_pos;
  new_capacity_2 = new_array_size_2 * seq_insert_reserve_factor;
  new_data_2 = new record_t[new_capacity_2]();
  memcpy(new_data_2, intermediate + split_pos,
         new_array_size_2 * sizeof(record_t));

  assert((int32_t)new_array_size_1 <= new_capacity_1);
  assert((int32_t)new_array_size_2 <= new_capacity_2);
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline void Group<key_t, val_t, seq, max_model_n>::merge_refs_with(
    const Group &next_group, record_t *&new_data, uint32_t &new_array_size,
    int32_t &new_capacity) const {
  size_t est_size = array_size + buffer->size() + next_group.array_size +
                    next_group.buffer->size();
  new_capacity = est_size * seq_insert_reserve_factor;
  new_data = new record_t[new_capacity]();

  uint32_t real_size_1, real_size_2;
  merge_refs_internal(new_data, real_size_1);
  next_group.merge_refs_internal(new_data + real_size_1, real_size_2);

  new_array_size = real_size_1 + real_size_2;

  assert((int32_t)new_array_size <= new_capacity);
}

// no workers should insert into buffer (frozen) now, so no lock needed
template <class key_t, class val_t, bool seq, size_t max_model_n>
inline std::vector<std::pair<key_t, size_t>> Group<key_t, val_t, seq, max_model_n>::merge_refs_internal(
    record_t *new_data, uint32_t &new_array_size) const {
  std::vector<std::pair<key_t, size_t>> result_vector;    
  size_t count = 0;

  auto buffer_source = typename buffer_t::RefSource(buffer);
  auto array_source = ArrayRefSource(data, array_size);
  array_source.advance_to_next_valid();
  buffer_source.advance_to_next_valid();

  while (array_source.has_next && buffer_source.has_next) {
    const key_t &base_key = array_source.get_key();
    wrapped_val_t &base_val = array_source.get_val();
    const key_t &buf_key = buffer_source.get_key();
    wrapped_val_t &buf_val = buffer_source.get_val();

    assert(base_key != buf_key);  // since update are inplaced

    if (base_key < buf_key) {
      new_data[count].first = base_key;
      new_data[count].second = wrapped_val_t(&base_val);
      assert(new_data[count].second.val.ptr->val.val == base_val.val.val);
      array_source.advance_to_next_valid();
    } else {
      new_data[count].first = buf_key;
      new_data[count].second = wrapped_val_t(&buf_val);
      assert(new_data[count].second.val.ptr->val.val == buf_val.val.val);
      result_vector.push_back(std::make_pair(buf_key, count));
      buffer_source.advance_to_next_valid();
    }
    count++;
  }

  while (array_source.has_next) {
    const key_t &base_key = array_source.get_key();
    wrapped_val_t &base_val = array_source.get_val();

    new_data[count].first = base_key;
    new_data[count].second = wrapped_val_t(&base_val);
    assert(new_data[count].second.val.ptr->val.val == base_val.val.val);

    array_source.advance_to_next_valid();
    count++;
  }

  while (buffer_source.has_next) {
    const key_t &buf_key = buffer_source.get_key();
    wrapped_val_t &buf_val = buffer_source.get_val();

    new_data[count].first = buf_key;
    new_data[count].second = wrapped_val_t(&buf_val);
    assert(new_data[count].second.val.ptr->val.val == buf_val.val.val);

    result_vector.push_back(std::make_pair(buf_key, count));
    buffer_source.advance_to_next_valid();
    count++;
  }

  for (size_t rec_i = 0; rec_i < (count == 0 ? 0 : count - 1); rec_i++) {
    assert(new_data[rec_i].first < new_data[rec_i + 1].first);
    assert(new_data[rec_i].second.status == new_data[rec_i + 1].second.status);
    assert(new_data[rec_i].second.status == 0x4000000000000000);
  }

  new_array_size = count;
  // assert(count > 0);
  return result_vector;
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline size_t Group<key_t, val_t, seq, max_model_n>::scan_2_way(
    const key_t &begin, const size_t n, const key_t &end,
    std::vector<std::pair<key_t, val_t>> &result) {

  #ifdef LATENCY_BREAKDOWN
  struct timespec begin_t, end_t;

  size_t remaining = n;
  bool out_of_range = false;
  
  uint32_t base_i = get_pos_from_array(begin);

  clock_gettime(CLOCK_MONOTONIC, &begin_t);
  ArrayDataSource array_source(data, array_size, base_i);
  typename buffer_t::DataSource buffer_source(begin, buffer);

  // first read a not-removed value from array and buffer, to avoid floatread
  // during merge
  array_source.advance_to_next_valid();
  buffer_source.advance_to_next_valid();

  while (array_source.has_next && buffer_source.has_next && remaining &&
         !out_of_range) {
    // we are sure that these key has not-removed (pre-read) value
    const key_t &base_key = array_source.get_key();
    const val_t &base_val = array_source.get_val();
    const key_t &buf_key = buffer_source.get_key();
    const val_t &buf_val = buffer_source.get_val();

    assert(base_key != buf_key);  // since update are inplaced

    if (base_key < buf_key) {
      if (base_key >= end) {
        out_of_range = true;
        break;
      }
      result.push_back(std::pair<key_t, val_t>(base_key, base_val));
      array_source.advance_to_next_valid();
    } else {
      if (buf_key >= end) {
        out_of_range = true;
        break;
      }
      result.push_back(std::pair<key_t, val_t>(buf_key, buf_val));
      buffer_source.advance_to_next_valid();
    }

    remaining--;
  }

  while (array_source.has_next && remaining && !out_of_range) {
    const key_t &base_key = array_source.get_key();
    const val_t &base_val = array_source.get_val();
    if (base_key >= end) {
      out_of_range = true;
      break;
    }
    result.push_back(std::pair<key_t, val_t>(base_key, base_val));
    array_source.advance_to_next_valid();
    remaining--;
  }

  while (buffer_source.has_next && remaining && !out_of_range) {
    const key_t &buf_key = buffer_source.get_key();
    const val_t &buf_val = buffer_source.get_val();
    if (buf_key >= end) {
      out_of_range = true;
      break;
    }
    result.push_back(std::pair<key_t, val_t>(buf_key, buf_val));
    buffer_source.advance_to_next_valid();
    remaining--;
  }
  clock_gettime(CLOCK_MONOTONIC, &end_t);
  lt.range_search_sum += GET_INTERVAL(begin_t, end_t);
  lt.range_search_count += 1;

  return n - remaining;

  #else

  size_t remaining = n;
  bool out_of_range = false;
  uint32_t base_i = get_pos_from_array(begin);
  ArrayDataSource array_source(data, array_size, base_i);
  typename buffer_t::DataSource buffer_source(begin, buffer);

  // first read a not-removed value from array and buffer, to avoid floatread
  // during merge
  array_source.advance_to_next_valid();
  buffer_source.advance_to_next_valid();

  while (array_source.has_next && buffer_source.has_next && remaining &&
         !out_of_range) {
    // we are sure that these key has not-removed (pre-read) value
    const key_t &base_key = array_source.get_key();
    const val_t &base_val = array_source.get_val();
    const key_t &buf_key = buffer_source.get_key();
    const val_t &buf_val = buffer_source.get_val();

    assert(base_key != buf_key);  // since update are inplaced

    if (base_key < buf_key) {
      if (base_key >= end) {
        out_of_range = true;
        break;
      }
      result.push_back(std::pair<key_t, val_t>(base_key, base_val));
      array_source.advance_to_next_valid();
    } else {
      if (buf_key >= end) {
        out_of_range = true;
        break;
      }
      result.push_back(std::pair<key_t, val_t>(buf_key, buf_val));
      buffer_source.advance_to_next_valid();
    }

    remaining--;
  }

  while (array_source.has_next && remaining && !out_of_range) {
    const key_t &base_key = array_source.get_key();
    const val_t &base_val = array_source.get_val();
    if (base_key >= end) {
      out_of_range = true;
      break;
    }
    result.push_back(std::pair<key_t, val_t>(base_key, base_val));
    array_source.advance_to_next_valid();
    remaining--;
  }

  while (buffer_source.has_next && remaining && !out_of_range) {
    const key_t &buf_key = buffer_source.get_key();
    const val_t &buf_val = buffer_source.get_val();
    if (buf_key >= end) {
      out_of_range = true;
      break;
    }
    result.push_back(std::pair<key_t, val_t>(buf_key, buf_val));
    buffer_source.advance_to_next_valid();
    remaining--;
  }

  return n - remaining;
  #endif
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline size_t Group<key_t, val_t, seq, max_model_n>::scan_3_way(
    const key_t &begin, const size_t n, const key_t &end,
    std::vector<std::pair<key_t, val_t>> &result) {
  size_t remaining = n;
  bool out_of_range = false;
  uint32_t base_i = get_pos_from_array(begin);
  ArrayDataSource array_source(data, array_size, base_i);
  typename buffer_t::DataSource buffer_source(begin, buffer);
  typename buffer_t::DataSource temp_buffer_source(begin, buffer_temp);

  // first read a not-removed value from array and buffer, to avoid float read
  // during merge
  array_source.advance_to_next_valid();
  buffer_source.advance_to_next_valid();
  temp_buffer_source.advance_to_next_valid();

  // 3-way
  while (array_source.has_next && buffer_source.has_next &&
         temp_buffer_source.has_next && remaining && !out_of_range) {
    // we are sure that these key has not-removed (pre-read) value
    const key_t &base_key = array_source.get_key();
    const val_t &base_val = array_source.get_val();
    const key_t &buf_key = buffer_source.get_key();
    const val_t &buf_val = buffer_source.get_val();
    const key_t &tmp_buf_key = temp_buffer_source.get_key();
    const val_t &tmp_buf_val = temp_buffer_source.get_val();

    assert(base_key != buf_key);      // since update are inplaced
    assert(base_key != tmp_buf_key);  // and removed values are skipped

    if (base_key < buf_key && base_key < tmp_buf_key) {
      if (base_key >= end) {
        out_of_range = true;
        break;
      }
      result.push_back(std::pair<key_t, val_t>(base_key, base_val));
      array_source.advance_to_next_valid();
    } else if (buf_key < base_key && buf_key < tmp_buf_key) {
      if (buf_key >= end) {
        out_of_range = true;
        break;
      }
      result.push_back(std::pair<key_t, val_t>(buf_key, buf_val));
      buffer_source.advance_to_next_valid();
    } else {
      if (tmp_buf_key >= end) {
        out_of_range = true;
        break;
      }
      result.push_back(std::pair<key_t, val_t>(tmp_buf_key, tmp_buf_val));
      temp_buffer_source.advance_to_next_valid();
    }

    remaining--;
  }

  // 2-way trailings
  while (array_source.has_next && buffer_source.has_next && remaining &&
         !out_of_range) {
    // we are sure that these key has not-removed (pre-read) value
    const key_t &base_key = array_source.get_key();
    const val_t &base_val = array_source.get_val();
    const key_t &buf_key = buffer_source.get_key();
    const val_t &buf_val = buffer_source.get_val();

    assert(base_key != buf_key);  // since update are inplaced

    if (base_key < buf_key) {
      if (base_key >= end) {
        out_of_range = true;
        break;
      }
      result.push_back(std::pair<key_t, val_t>(base_key, base_val));
      array_source.advance_to_next_valid();
    } else {
      if (buf_key >= end) {
        out_of_range = true;
        break;
      }
      result.push_back(std::pair<key_t, val_t>(buf_key, buf_val));
      buffer_source.advance_to_next_valid();
    }

    remaining--;
  }

  while (buffer_source.has_next && temp_buffer_source.has_next && remaining &&
         !out_of_range) {
    // we are sure that these key has not-removed (pre-read) value
    const key_t &buf_key = buffer_source.get_key();
    const val_t &buf_val = buffer_source.get_val();
    const key_t &tmp_buf_key = temp_buffer_source.get_key();
    const val_t &tmp_buf_val = temp_buffer_source.get_val();

    assert(buf_key != tmp_buf_key);  // and removed values are skipped

    if (buf_key < tmp_buf_key) {
      if (buf_key >= end) {
        out_of_range = true;
        break;
      }
      result.push_back(std::pair<key_t, val_t>(buf_key, buf_val));
      buffer_source.advance_to_next_valid();
    } else {
      if (tmp_buf_key >= end) {
        out_of_range = true;
        break;
      }
      result.push_back(std::pair<key_t, val_t>(tmp_buf_key, tmp_buf_val));
      temp_buffer_source.advance_to_next_valid();
    }

    remaining--;
  }

  while (array_source.has_next && temp_buffer_source.has_next && remaining &&
         !out_of_range) {
    // we are sure that these key has not-removed (pre-read) value
    const key_t &base_key = array_source.get_key();
    const val_t &base_val = array_source.get_val();
    const key_t &tmp_buf_key = temp_buffer_source.get_key();
    const val_t &tmp_buf_val = temp_buffer_source.get_val();

    assert(base_key != tmp_buf_key);  // and removed values are skipped

    if (base_key < tmp_buf_key) {
      if (base_key >= end) {
        out_of_range = true;
        break;
      }
      result.push_back(std::pair<key_t, val_t>(base_key, base_val));
      array_source.advance_to_next_valid();
    } else {
      if (tmp_buf_key >= end) {
        out_of_range = true;
        break;
      }
      result.push_back(std::pair<key_t, val_t>(tmp_buf_key, tmp_buf_val));
      temp_buffer_source.advance_to_next_valid();
    }

    remaining--;
  }

  // 1-way trailings
  while (array_source.has_next && remaining && !out_of_range) {
    const key_t &base_key = array_source.get_key();
    const val_t &base_val = array_source.get_val();
    if (base_key >= end) {
      out_of_range = true;
      break;
    }
    result.push_back(std::pair<key_t, val_t>(base_key, base_val));
    array_source.advance_to_next_valid();
    remaining--;
  }

  while (buffer_source.has_next && remaining && !out_of_range) {
    const key_t &buf_key = buffer_source.get_key();
    const val_t &buf_val = buffer_source.get_val();
    if (buf_key >= end) {
      out_of_range = true;
      break;
    }
    result.push_back(std::pair<key_t, val_t>(buf_key, buf_val));
    buffer_source.advance_to_next_valid();
    remaining--;
  }

  while (temp_buffer_source.has_next && remaining && !out_of_range) {
    const key_t &tmp_buf_key = temp_buffer_source.get_key();
    const val_t &tmp_buf_val = temp_buffer_source.get_val();
    if (tmp_buf_key >= end) {
      out_of_range = true;
      break;
    }
    result.push_back(std::pair<key_t, val_t>(tmp_buf_key, tmp_buf_val));
    temp_buffer_source.advance_to_next_valid();
    remaining--;
  }

  return n - remaining;
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline float *Group<key_t, val_t, seq, max_model_n>::get_model(
    size_t model_i) const {
  assert(model_i < model_n);
  size_t aligned_f_len = (feature_len + 7) & (~7);
  size_t pivots_size = (model_n - 1) * aligned_f_len;
  return (float *)(model_info.data() + pivots_size) +  // skip pivots
         ((size_t)feature_len + 2) * model_i;
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline const uint8_t *Group<key_t, val_t, seq, max_model_n>::get_model_pivot(
    size_t model_i) const {
  assert(model_i < model_n);
  size_t aligned_f_len = (feature_len + 7) & (~7);
  return model_i == 0 ? (uint8_t *)&pivot + prefix_len
                      : model_info.data() + aligned_f_len * (model_i - 1);
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline void Group<key_t, val_t, seq, max_model_n>::set_model_pivot(
    size_t model_i, const key_t &key) {
  assert(model_i < model_n);
  if (model_i == 0) return;
  size_t aligned_f_len = (feature_len + 7) & (~7);
  memcpy(model_info.data() + aligned_f_len * (model_i - 1),
         (uint8_t *)&key + prefix_len, feature_len);
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline void Group<key_t, val_t, seq, max_model_n>::get_model_error(
    size_t model_i, int &error_max, int &error_min) const {
  int32_t *error_info =
      (int32_t *)(get_model(model_i) + (size_t)feature_len + 1);
  error_max = error_info[0];
  error_min = error_info[1];
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline void Group<key_t, val_t, seq, max_model_n>::set_model_error(
    size_t model_i, int error_max, int error_min) {
  int32_t *error_info =
      (int32_t *)(get_model(model_i) + (size_t)feature_len + 1);
  error_info[0] = error_max;
  error_info[1] = error_min;
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline void Group<key_t, val_t, seq, max_model_n>::prepare_last(
    size_t model_i, const std::vector<float*> &model_key_ptrs,
    const std::vector<size_t> &positions) {
  model_prepare(model_key_ptrs, positions, get_model(model_i), feature_len, &cached_r[model_i]);

  // calculate error info
  int error_max = INT_MIN, error_min = INT_MAX;
  for (size_t key_i = 0; key_i < model_key_ptrs.size(); ++key_i) {
    long long int pos_actual = positions[key_i];
    long long int pos_pred = predict(model_i, model_key_ptrs[key_i]);
    long long int error = pos_actual - pos_pred;
    // when int error is overflowed, set max<min, so user can know
    if (error > INT_MAX || error < INT_MIN) {
      error_max = -1;
      error_min = 1;
      return;
    }
    if (error > error_max) error_max = error;
    if (error < error_min) error_min = error;
  }
  set_model_error(model_i, error_max, error_min);
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline size_t Group<key_t, val_t, seq, max_model_n>::get_error_bound(
    size_t model_i, const std::vector<float *> &model_key_ptrs,
    const std::vector<size_t> &positions) const {
  long long int max = 0;
  for (size_t key_i = 0; key_i < model_key_ptrs.size(); ++key_i) {
    long long int pos_actual = positions[key_i];
    long long int pos_pred = predict(model_i, model_key_ptrs[key_i]);
    long long int error = std::abs(pos_actual - pos_pred);
    if (error > INT_MAX) return INT_MAX;
    if (error > max) max = error;
  }
  return max;
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline void Group<key_t, val_t, seq, max_model_n>::predict_last(
    size_t model_i, const key_t &key, size_t &pos, int &error_min,
    int &error_max) const {
  pos = predict(model_i, key);
  get_model_error(model_i, error_max, error_min);
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline size_t Group<key_t, val_t, seq, max_model_n>::predict(
    size_t model_i, const key_t &key) const {
  float model_key[feature_len];
  key.get_model_key(prefix_len, feature_len, model_key);
  return model_predict(get_model(model_i), model_key, feature_len);
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
inline size_t Group<key_t, val_t, seq, max_model_n>::predict(
    size_t model_i, const float *model_key) const {
  return model_predict(get_model(model_i), model_key, feature_len);
}

// TODO unify compare interface
template <class key_t, class val_t, bool seq, size_t max_model_n>
inline bool Group<key_t, val_t, seq, max_model_n>::key_less_than(
    const uint8_t *k1, const uint8_t *k2, size_t len) const {
  return memcmp(k1, k2, len) < 0;
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
Group<key_t, val_t, seq, max_model_n>::ArrayDataSource::ArrayDataSource(
    record_t *data, uint32_t array_size, uint32_t pos)
    : array_size(array_size), pos(pos), data(data) {}

template <class key_t, class val_t, bool seq, size_t max_model_n>
void Group<key_t, val_t, seq,
           max_model_n>::ArrayDataSource::advance_to_next_valid() {
  while (pos < array_size) {
    if (data[pos].second.read(next_val)) {
      next_key = data[pos].first;
      has_next = true;
      pos++;
      return;
    }
    pos++;
  }
  has_next = false;
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
const key_t &Group<key_t, val_t, seq, max_model_n>::ArrayDataSource::get_key() {
  return next_key;
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
const val_t &Group<key_t, val_t, seq, max_model_n>::ArrayDataSource::get_val() {
  return next_val;
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
Group<key_t, val_t, seq, max_model_n>::ArrayRefSource::ArrayRefSource(
    record_t *data, uint32_t array_size)
    : array_size(array_size), pos(0), data(data) {}

template <class key_t, class val_t, bool seq, size_t max_model_n>
void Group<key_t, val_t, seq,
           max_model_n>::ArrayRefSource::advance_to_next_valid() {
  while (pos < array_size) {
    val_t temp_val;
    if (data[pos].second.read(temp_val)) {
      next_val_ptr = &data[pos].second;
      next_key = data[pos].first;
      has_next = true;
      pos++;
      return;
    }
    pos++;
  }
  has_next = false;
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
const key_t &Group<key_t, val_t, seq, max_model_n>::ArrayRefSource::get_key() {
  return next_key;
}

template <class key_t, class val_t, bool seq, size_t max_model_n>
typename Group<key_t, val_t, seq, max_model_n>::atomic_val_t &
Group<key_t, val_t, seq, max_model_n>::ArrayRefSource::get_val() {
  return *next_val_ptr;
}

}  // namespace sindex

#endif  // SINDEX_GROUP_IMPL_H
