#ifndef THREAD_GROUP_INCLUDED
#define THREAD_GROUP_INCLUDED

#include <atomic>

#include "mysql/psi/mysql_cond.h"
#include "mysql/psi/mysql_mutex.h"

class Thread_group {
 public:
  uint m_group_id;

  mysql_mutex_t m_mutex;
  mysql_cond_t m_cond_worker;
  mysql_cond_t m_cond_listener;

  int m_active_thread_count;
  int m_worker_thread_count;
  int m_listener_thread_count;
  int m_io_waiting_count;

  std::atomic<bool> m_stalled;
  ulonglong m_last_activity_time;
  bool m_shutdown;

  bool init(uint group_id);
  void destroy();
};

#endif  // THREAD_GROUP_INCLUDED
