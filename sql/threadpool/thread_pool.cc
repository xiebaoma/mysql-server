#include "sql/threadpool/thread_pool.h"

#include <new>
#include <unistd.h>

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
  s_instance = nullptr;
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

  group.enqueue_connection(event);
  group.wake_or_create_thread();

  return false;
}

uint Thread_pool_connection_handler::get_max_threads() const {
  return Thread_pool::s_max_threads;
}
