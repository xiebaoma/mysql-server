#include "sql/threadpool/thread_pool.h"

#include <algorithm>
#include <new>
#include <unistd.h>

#include "include/my_systime.h"
#include "include/mysql/thread_pool_priv.h"
#include "sql/threadpool/threadpool_common.h"

// Static variable definitions
Thread_pool *Thread_pool::s_instance = nullptr;
uint Thread_pool::s_pool_size = 0;
uint Thread_pool::s_max_threads = 100000;
ulong Thread_pool::s_stall_limit = 500;
ulong Thread_pool::s_idle_timeout = 60;
uint Thread_pool::s_oversubscribe_par = 3;
uint Thread_pool::s_high_prio_mode =
    Thread_pool::HIGH_PRIO_MODE_TRANSACTIONS;
uint Thread_pool::s_high_prio_tickets = UINT32_MAX;
ulong Thread_pool::s_prio_kickup_timer = 1000;

THD_event_functions Thread_pool::s_event_functions = {
    Thread_pool::thd_wait_begin_cb,
    Thread_pool::thd_wait_end_cb,
    Thread_pool::post_kill_notification_cb};

void Thread_pool::thd_wait_begin_cb(THD *thd, int wait_type [[maybe_unused]]) {
  if (s_instance == nullptr) return;
  Scheduler_data *sd =
      static_cast<Scheduler_data *>(thd_get_scheduler_data(thd));
  if (sd == nullptr || sd->group == nullptr) return;

  sd->group->m_io_waiting_count.fetch_add(1, std::memory_order_relaxed);
}

void Thread_pool::thd_wait_end_cb(THD *thd) {
  if (s_instance == nullptr) return;
  Scheduler_data *sd =
      static_cast<Scheduler_data *>(thd_get_scheduler_data(thd));
  if (sd == nullptr || sd->group == nullptr) return;

  int old = sd->group->m_io_waiting_count.fetch_sub(1, std::memory_order_relaxed);
  if (old <= 0) {
    // Underflow guard: a spurious thd_wait_end without matching
    // thd_wait_begin would drive the counter negative.
    sd->group->m_io_waiting_count.store(0, std::memory_order_relaxed);
  }
}

void Thread_pool::post_kill_notification_cb(THD *thd) {
  if (s_instance == nullptr) return;
  Scheduler_data *sd =
      static_cast<Scheduler_data *>(thd_get_scheduler_data(thd));
  if (sd == nullptr || sd->group == nullptr) return;

  // Try CS_QUEUED first: the worker's CS_CLOSING check at the top of
  // READY_CONNECTION will handle cleanup.
  {
    int expected = CS_QUEUED;
    if (sd->state.compare_exchange_strong(expected, CS_CLOSING,
                                          std::memory_order_acq_rel,
                                          std::memory_order_relaxed))
      return;
  }

  // Try CS_ACTIVE: the worker's CAS from ACTIVE→IDLE detects CLOSING
  // and forces close.
  {
    int expected = CS_ACTIVE;
    if (sd->state.compare_exchange_strong(expected, CS_CLOSING,
                                          std::memory_order_acq_rel,
                                          std::memory_order_relaxed))
      return;
  }

  // Try CS_IDLE: the connection is waiting for a socket event.
  // EPOLLONESHOT has already disarmed the fd, so no need to call
  // remove_connection_fd.  Wake the listener so it detects CS_CLOSING
  // and performs inline cleanup — no worker thread participates,
  // closing the UAF window between epoll_wait and event processing.
  {
    int expected = CS_IDLE;
    if (sd->state.compare_exchange_strong(expected, CS_CLOSING,
                                          std::memory_order_acq_rel,
                                          std::memory_order_relaxed)) {
      sd->group->m_listener.wake();
    }
  }
}

bool Thread_pool::init() {
  m_group_count = s_pool_size;
  if (m_group_count == 0) {
    long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
    m_group_count = (ncpus > 0) ? static_cast<uint>(ncpus) : 4;
  }

  m_groups = new (std::nothrow) Thread_group[m_group_count];
  if (m_groups == nullptr) return true;

  for (uint i = 0; i < m_group_count; i++) {
    if (m_groups[i].init(i)) {
      destroy();
      return true;
    }
  }

  s_instance = this;

  // Register thread pool event callbacks with the connection handler manager.
  Connection_handler_manager::event_functions = &s_event_functions;

  // Start the timer thread for periodic stall detection.
  m_timer_running.store(true, std::memory_order_release);
  if (my_thread_create(&m_timer_thread, get_connection_attrib(),
                       timer_main_cdecl, this) != 0) {
    m_timer_running.store(false, std::memory_order_release);
    destroy();
    return true;
  }

  return false;
}

void Thread_pool::destroy() {
  // Stop the timer thread first since it accesses groups.
  if (m_timer_running.load(std::memory_order_relaxed)) {
    m_timer_running.store(false, std::memory_order_release);
    my_thread_join(&m_timer_thread, nullptr);
  }

  if (m_groups != nullptr) {
    for (uint i = 0; i < m_group_count; i++) {
      m_groups[i].destroy();
    }
    delete[] m_groups;
    m_groups = nullptr;
  }
  Connection_handler_manager::event_functions = nullptr;
  s_instance = nullptr;
}

void Thread_pool::prepare_shutdown() {
  if (s_instance == nullptr) return;
  for (uint i = 0; i < s_instance->m_group_count; i++) {
    Thread_group &group = s_instance->m_groups[i];
    group.m_shutdown.store(true, std::memory_order_relaxed);
    // Clean up IDLE connections first (inline, no worker needed),
    // then wake workers to drain any remaining queued events.
    group.drain_idle_connections();
    group.m_high_priority_queue.signal_all();
    group.m_low_priority_queue.signal_all();
  }
}

void *Thread_pool::timer_main_cdecl(void *arg) {
  Thread_pool *pool = static_cast<Thread_pool *>(arg);
  if (my_thread_init()) return nullptr;
  pool->timer_main();
  my_thread_end();
  return nullptr;
}

void Thread_pool::timer_main() {
  ulong interval_ms = s_stall_limit;
  if (interval_ms < 10) interval_ms = 10;

  while (m_timer_running.load(std::memory_order_relaxed)) {
    // Sleep in 200 ms chunks so shutdown is detected promptly.
    // Measure actual elapsed time, since my_sleep may be interrupted
    // early by signals.
    ulonglong start = my_micro_time();
    ulonglong deadline = start + interval_ms * 1000ULL;
    while (m_timer_running.load(std::memory_order_relaxed)) {
      ulonglong now = my_micro_time();
      if (now >= deadline) break;
      ulonglong remaining_us = deadline - now;
      ulonglong chunk_us = std::min(remaining_us, 200000ULL);
      my_sleep(chunk_us);
    }

    if (!m_timer_running.load(std::memory_order_relaxed)) break;

    for (uint i = 0; i < m_group_count; i++) {
      Thread_group &group = m_groups[i];
      if (group.m_shutdown.load(std::memory_order_relaxed)) continue;

      if (group.check_stall()) {
        bool was_stalled =
            group.m_stalled.exchange(true, std::memory_order_acq_rel);
        if (!was_stalled) {
          // Create at most one extra worker per stall episode from the
          // timer.  If one worker doesn't resolve the stall, adding more
          // is unlikely to help (the bottleneck is elsewhere).  Additional
          // workers can still arrive via the add_connection path from the
          // acceptor thread.
          group.wake_or_create_thread();
        }
      } else {
        group.m_stalled.store(false, std::memory_order_release);
      }
    }
  }
}

/**
  add_connection() is called from the acceptor thread.
  It must be lightweight:
    - Pick a group via round-robin.
    - Enqueue Channel_info into the group's low-priority queue.
    - Wake or create a worker in that group.
  The worker thread handles THD creation, auth, do_command, and cleanup.
*/
bool Thread_pool_connection_handler::add_connection(
    Channel_info *channel_info) {
  Thread_pool *pool = Thread_pool::s_instance;
  if (pool == nullptr) return true;

  // Round-robin group assignment
  ulonglong seq = pool->m_accepted_connection_seq.fetch_add(
      1, std::memory_order_relaxed);
  uint group_id = static_cast<uint>(seq % pool->m_group_count);
  Thread_group &group = pool->m_groups[group_id];

  // Build a NEW_CONNECTION event and enqueue it.
  Connection_event event;
  event.type = Connection_event_type::NEW_CONNECTION;
  event.data.channel_info = channel_info;

  if (!group.enqueue_connection(event)) {
    // OOM: send error, close channel, and decrement the connection count
    // that was incremented in process_new_connection().  The caller will
    // inc_aborted_connects() and delete channel_info.
    channel_info->send_error_and_close_channel(ER_OUT_OF_RESOURCES, 0, true);
    dec_connection_count();
    return true;
  }
  group.wake_or_create_thread();

  return false;
}

uint Thread_pool_connection_handler::get_max_threads() const {
  return Thread_pool::s_max_threads;
}
