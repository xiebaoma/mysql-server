#ifndef THREADPOOL_LISTENER_INCLUDED
#define THREADPOOL_LISTENER_INCLUDED

/**
  Abstract listener for monitoring connection fd readiness.

  On Linux, uses epoll with EPOLLONESHOT so each ready fd is reported
  exactly once — the worker must rearm it after processing before the
  listener will report it again.  This prevents the same fd from being
  dispatched to multiple workers concurrently.

  A wakeup event (eventfd on Linux) is registered in the epoll set so
  the listener can be woken from epoll_wait during shutdown.
*/

class THD;

class Threadpool_listener {
 public:
  bool init();
  void destroy();

  /** Register a THD's socket fd for EPOLLIN | EPOLLONESHOT monitoring. */
  bool register_fd(THD *thd);
  /** Remove a THD's socket fd from monitoring. */
  bool remove_fd(THD *thd);
  /** Rearm after worker finishes processing so listener sees future data. */
  bool rearm_fd(THD *thd);

  /** Wake the listener thread from epoll_wait (shutdown). */
  void wake();
  /** Drain the wakeup notification after being woken. */
  void ack_wakeup();

  /**
    Wait for socket events with a timeout.  Ready events are stored
    internally; access them via is_wakeup_event() / get_event_thd().
    @param timeout_ms  Timeout in milliseconds.
    @return Number of ready fds (including wakeups), or -1 on error.
  */
  int wait(int timeout_ms);

  /** Returns true if event i is the internal wakeup (not a socket). */
  bool is_wakeup_event(int index) const;
  /** Returns the THD* associated with event i. */
  THD *get_event_thd(int index) const;

  bool is_initialized() const { return m_epoll_fd >= 0; }

  static constexpr int MAX_EVENTS = 256;
  static constexpr int DEFAULT_TIMEOUT_MS = 200;

 private:
  int m_epoll_fd{-1};
  int m_wakeup_fd{-1};  // eventfd (Linux) or pipe read end (fallback)

  // epoll result storage — opaque to callers
  int m_num_events{0};
  void *m_event_ptrs[MAX_EVENTS]{};
  unsigned m_event_flags[MAX_EVENTS]{};
};
#endif  // THREADPOOL_LISTENER_INCLUDED
