#ifndef CONNECTION_QUEUE_INCLUDED
#define CONNECTION_QUEUE_INCLUDED

#include <atomic>

#include "mysql/psi/mysql_cond.h"
#include "mysql/psi/mysql_mutex.h"

#include "sql/threadpool/threadpool_common.h"

/**
  Thread-safe FIFO queue for connection events.
  All operations are protected by an internal mutex.
  dequeue() blocks on a condition variable when the queue is empty.
*/
class Connection_queue {
 public:
  bool init();
  void destroy();

  /** Add an event to the tail of the queue. Never blocks. */
  void enqueue(Connection_event event);

  /**
    Remove an event from the head of the queue.
    @param event  [out] The dequeued event (only valid if returning true)
    @param timeout_ms  < 0: block indefinitely
                       = 0: non-blocking (return immediately if empty)
                       > 0: wait up to timeout_ms milliseconds
    @retval true  An event was dequeued
    @retval false Queue is empty (non-blocking or timeout)
  */
  bool dequeue(Connection_event *event, int timeout_ms);

  /** Return the current queue length (snapshot, may change immediately). */
  int length() const { return m_length.load(std::memory_order_relaxed); }

  /** Return true if the queue is empty (snapshot). */
  bool is_empty() const { return m_length.load(std::memory_order_relaxed) == 0; }

  /** Wake all waiters blocked in dequeue(). Used during shutdown. */
  void signal_all();

 private:
  struct Element {
    Connection_event event;
    Element *next;
  };

  mysql_mutex_t m_mutex;
  mysql_cond_t m_cond;
  Element *m_head{nullptr};
  Element *m_tail{nullptr};
  std::atomic<int> m_length{0};
  bool m_shutdown{false};
};

#endif  // CONNECTION_QUEUE_INCLUDED
