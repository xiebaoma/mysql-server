#include "sql/threadpool/thread_group.h"

bool Thread_group::init(uint group_id) {
  m_group_id = group_id;
  m_active_thread_count = 0;
  m_worker_thread_count = 0;
  m_listener_thread_count = 0;
  m_io_waiting_count = 0;
  m_stalled = false;
  m_last_activity_time = 0;
  m_shutdown = false;

  mysql_mutex_init(0, &m_mutex, MY_MUTEX_INIT_FAST);
  mysql_cond_init(0, &m_cond_worker);
  mysql_cond_init(0, &m_cond_listener);

  return false;
}

void Thread_group::destroy() {
  mysql_mutex_destroy(&m_mutex);
  mysql_cond_destroy(&m_cond_worker);
  mysql_cond_destroy(&m_cond_listener);
}
