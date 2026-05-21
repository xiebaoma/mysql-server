#include "sql/threadpool/thread_pool.h"

#include <new>
#include <unistd.h>

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
  if (sd == nullptr) return;

  // Transition to CLOSING so the worker skips processing.
  // Try the states the connection is likely to be in:
  //   CS_QUEUED — waiting in queue, not yet picked up
  //   CS_ACTIVE — currently being processed by a worker
  //   CS_IDLE   — idle, waiting for a socket event
  // The worker's CAS from ACTIVE→IDLE detects CLOSING and forces a close.
  // TODO(Phase 2): CS_WAIT_IO will be added once the listener is in place.
  for (int expected : {CS_QUEUED, CS_ACTIVE, CS_IDLE}) {
    if (sd->state.compare_exchange_strong(expected, CS_CLOSING,
                                          std::memory_order_acq_rel,
                                          std::memory_order_relaxed))
      return;
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

  return false;
}

void Thread_pool::destroy() {
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
    group.m_high_priority_queue.signal_all();
    group.m_low_priority_queue.signal_all();
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
