#ifndef THREAD_POOL_INCLUDED
#define THREAD_POOL_INCLUDED

#include <atomic>

#include "my_thread.h"
#include "sql/conn_handler/connection_handler.h"
#include "sql/threadpool/thread_group.h"

class Thread_pool {
 public:
  static Thread_pool *s_instance;

  // System variables — defined in thread_pool.cc
  static uint s_pool_size;
  static uint s_max_threads;
  static ulong s_stall_limit;
  static ulong s_idle_timeout;
  static uint s_oversubscribe_par;
  static uint s_high_prio_tickets;
  static ulong s_prio_kickup_timer;

  // thread_pool_high_prio_mode enum values
  enum High_prio_mode { HIGH_PRIO_MODE_TRANSACTIONS = 0, HIGH_PRIO_MODE_STATEMENTS, HIGH_PRIO_MODE_NONE };
  static uint s_high_prio_mode;

  Thread_group *m_groups{nullptr};
  uint m_group_count{0};

  // Monotonically increasing sequence for round-robin group assignment.
  std::atomic<ulonglong> m_accepted_connection_seq{0};

  my_thread_handle m_timer_thread;
  std::atomic<bool> m_timer_running{false};

  bool init();
  void destroy();
};

// Built-in Connection_handler subclass
class Thread_pool_connection_handler : public Connection_handler {
 public:
  Thread_pool_connection_handler() = default;
  ~Thread_pool_connection_handler() override = default;

 protected:
  bool add_connection(Channel_info *channel_info) override;
  uint get_max_threads() const override;
};

#endif  // THREAD_POOL_INCLUDED
