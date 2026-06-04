#ifndef GC_MANAGER_H
#define GC_MANAGER_H

#include "groupManager.h"

#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <functional>
#include <memory>

class gcManager {
  public:
    gcManager(GroupManager &ref);
    ~gcManager();

    void isGC();
    void fast_gc();

    void stop();
    void WaitIdle();

  public:
    GroupManager &gm_ref;

    enum class GCState {
        IDLE,        
        RUNNING,     
        SCHEDULED    
    };
  private:
    std::atomic<bool> _stop;
    std::atomic<GCState> _gc_state{GCState::IDLE};
    std::atomic<int> _gc_fast_group_number; 

    std::mutex _mtx_gc_fast;
    std::condition_variable _cv_gc_fast;
    std::unique_ptr<std::thread> gc_fast_ptr;
};

#endif