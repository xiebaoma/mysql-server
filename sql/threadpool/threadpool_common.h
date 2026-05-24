#ifndef THREADPOOL_COMMON_INCLUDED
#define THREADPOOL_COMMON_INCLUDED

#include <atomic>

class THD;
class Channel_info;
class Thread_group;

// Connection states
enum Connection_state {
  CS_IDLE = 0,
  CS_QUEUED,
  CS_ACTIVE,
  CS_WAIT_IO,
  CS_CLOSING
};

// Event type placed in the thread group queue
enum class Connection_event_type { NEW_CONNECTION, READY_CONNECTION };

// Union-based queue event
struct Connection_event {
  Connection_event_type type;
  union {
    Channel_info *channel_info;
    THD *thd;
  } data;
};

// Per-THD scheduler data, stored via thd_set_scheduler_data()
struct Scheduler_data {
  THD *thd{nullptr};
  Thread_group *group{nullptr};
  std::atomic<int> state{CS_IDLE};
  uint high_prio_tickets{0};
  ulonglong queue_enter_time{0};

  // Intrusive linked list pointers for Thread_group::m_connections
  Scheduler_data *next_in_group{nullptr};
  Scheduler_data *prev_in_group{nullptr};
};

#endif  // THREADPOOL_COMMON_INCLUDED
