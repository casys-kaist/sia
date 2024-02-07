#include "alex.h"

struct BGParam {
  uint32_t thread_id;
  alex_t *table;
};
typedef BGParam bg_param_t;

bool foreground_finished = false;

void *run_bg(void *param) {
  //alex::rcu_init();
  bg_param_t *bgparam = (bg_param_t *) param;
  uint64_t thread_id = pthread_self();
  alex_t* index = (alex_t *) bgparam->table;
  std::unique_lock<std::mutex> lk(alex::cvm);
  //ready_bg_threads++;
  alex::cv.wait(lk, []{
    return !(alex::pending_modification_jobs_.empty());
  });

  while (true) {
    while (!(alex::pending_modification_jobs_.empty())) {
      auto pair = alex::pending_modification_jobs_.front();
      alex::pending_modification_jobs_.pop();
      lk.unlock();
      if (pair.second) { //expansion
        index->expand_handler(pair.first);
      } else { //modification
        index->insert_fail_handler(pair.first);
      }
      lk.lock();
    }

    if (foreground_finished) {
        break;
    } else {
      alex::cv.wait(lk, []{
        return !(alex::pending_modification_jobs_.empty()) || (foreground_finished);
      });
    }
  }

  lk.unlock();
  
  pthread_exit(nullptr);
}