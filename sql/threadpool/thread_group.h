#ifndef THREAD_GROUP_INCLUDED
#define THREAD_GROUP_INCLUDED

#include <atomic>

#include "mysql/psi/mysql_cond.h"
#include "mysql/psi/mysql_mutex.h"
#include "my_thread.h"

#include "sql/threadpool/connection_queue.h"
#include "sql/threadpool/threadpool_common.h"
#include "sql/threadpool/threadpool_listener.h"

class Thread_group {
 public:
  static constexpr int MAX_THREADS_PER_GROUP = 4096;

  uint m_group_id{0};

  // -- queues --
  // High priority: connections with active transactions
  // Low priority: new connections, idle connections reawakening
  Connection_queue m_high_priority_queue;
  Connection_queue m_low_priority_queue;

  // -- thread counts --
  int m_active_thread_count{0};
  std::atomic<int> m_worker_thread_count{0};
  int m_listener_thread_count{0};

  // -- I/O waiting count --
  std::atomic<int> m_io_waiting_count{0};

  // -- stall detection --
  std::atomic<bool> m_stalled{false};
  std::atomic<ulonglong> m_last_activity_time{0};

  // -- shutdown --
  std::atomic<bool> m_shutdown{false};

  // -- listener --
  Threadpool_listener m_listener;
  my_thread_handle m_listener_handle;
  std::atomic<bool> m_listener_running{false};

  // -- synchronization --
  mysql_mutex_t m_mutex;
  mysql_cond_t m_cond_worker;
  mysql_cond_t m_cond_listener;

  // -- connection list --
  // Doubly-linked list of Scheduler_data for all connections in this group.
  // Protected by m_mutex (multiple workers may add/remove concurrently).
  Scheduler_data *m_connections_head{nullptr};
  Scheduler_data *m_connections_tail{nullptr};

  bool init(uint group_id);
  void destroy();

  // Enqueue a connection event.
  // @retval true on success, false on OOM.
  bool enqueue_connection(Connection_event event);

  // Dequeue the next connection event. Blocks with timeout.
  // @param event  [out] The dequeued event
  // @param timeout_ms  Timeout in ms, < 0 for indefinite, 0 for non-blocking
  // @retval true  event was dequeued
  // @retval false timeout or shutdown
  bool dequeue_connection(Connection_event *event, int timeout_ms);

  // Wake an existing sleeping worker or create a new one.
  void wake_or_create_thread();

  // Main loop for a worker thread.
  void worker_main();

  // Listener thread lifecycle.
  bool create_listener();
  void listener_main();

  // Register / remove / rearm connection fd with the listener.
  bool register_connection_fd(THD *thd);
  bool remove_connection_fd(THD *thd);
  bool rearm_connection_fd(THD *thd);

  // Shutdown: enqueue all IDLE connections so workers can drain them.
  void drain_idle_connections();

  // Listener inline cleanup: remove fd, store globals, and tear down a
  // CS_CLOSING connection.  Caller must have already removed sd from the
  // connection list.
  void cleanup_closing_connection(Scheduler_data *sd, bool server_shutdown);
  // Scan the connection list and clean up all CS_CLOSING connections.
  void drain_closing_connections();

  // Add a Scheduler_data to the group's connection list.
  void add_connection_to_list(Scheduler_data *sd);
  // Remove a Scheduler_data from the group's connection list.
  void remove_connection_from_list(Scheduler_data *sd);

  // Progress counters for stall detection.
  std::atomic<ulonglong> m_dequeue_count{0};
  std::atomic<ulonglong> m_last_dequeue_count{0};

  // Last time a worker thread was created (microseconds, for throttling).
  ulonglong m_last_thread_creation_time{0};

  // Check if this group is stalled.
  bool check_stall();

 private:
  // Helper: create a pool worker thread using the OS.
  static void *worker_main_cdecl(void *arg);
  // Helper: create a listener thread using the OS.
  static void *listener_main_cdecl(void *arg);

  // Helper: full THD teardown.  Caller must have called thd_store_globals(thd)
  // and removed the connection from the group list beforehand.
  void cleanup_thd_connection(THD *thd, Scheduler_data *sd,
                              bool server_shutdown);

  // Clean up a connection event, releasing the associated resources.
  // For NEW_CONNECTION: destroys Channel_info and decrements connection count.
  // For READY_CONNECTION: closes the THD and decrements connection count.
  void cleanup_event(Connection_event event);
};

#endif  // THREAD_GROUP_INCLUDED
