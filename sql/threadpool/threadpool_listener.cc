#include "sql/threadpool/threadpool_listener.h"

#include <errno.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/epoll.h>
#include <sys/eventfd.h>
#endif

#include "include/mysql/thread_pool_priv.h"  // thd_get_fd

#ifdef __linux__

bool Threadpool_listener::init() {
  m_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (m_epoll_fd < 0) return true;

  m_wakeup_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (m_wakeup_fd < 0) {
    close(m_epoll_fd);
    m_epoll_fd = -1;
    return true;
  }

  struct epoll_event ev;
  ev.events = EPOLLIN;
  ev.data.ptr = this;  // distinguishes wakeup events from socket events
  if (epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, m_wakeup_fd, &ev) < 0) {
    close(m_wakeup_fd);
    close(m_epoll_fd);
    m_wakeup_fd = -1;
    m_epoll_fd = -1;
    return true;
  }

  return false;
}

void Threadpool_listener::destroy() {
  if (m_wakeup_fd >= 0) {
    close(m_wakeup_fd);
    m_wakeup_fd = -1;
  }
  if (m_epoll_fd >= 0) {
    close(m_epoll_fd);
    m_epoll_fd = -1;
  }
}

bool Threadpool_listener::register_fd(THD *thd) {
  my_socket s = thd_get_fd(thd);
  int fd = static_cast<int>(s);

  Scheduler_data *sd =
      static_cast<Scheduler_data *>(thd_get_scheduler_data(thd));

  struct epoll_event ev;
  ev.events = EPOLLIN | EPOLLONESHOT;
  ev.data.ptr = sd;

  return epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, fd, &ev) != 0;
}

bool Threadpool_listener::remove_fd(THD *thd) {
  my_socket s = thd_get_fd(thd);
  int fd = static_cast<int>(s);

  // EPOLL_CTL_DEL may fail with ENOENT if the fd was already removed
  // (e.g. by EPOLLONESHOT after being dequeued).  That is benign.
  if (epoll_ctl(m_epoll_fd, EPOLL_CTL_DEL, fd, nullptr) != 0 &&
      errno != ENOENT)
    return true;

  return false;
}

bool Threadpool_listener::rearm_fd(THD *thd) {
  my_socket s = thd_get_fd(thd);
  int fd = static_cast<int>(s);

  Scheduler_data *sd =
      static_cast<Scheduler_data *>(thd_get_scheduler_data(thd));

  struct epoll_event ev;
  ev.events = EPOLLIN | EPOLLONESHOT;
  ev.data.ptr = sd;

  return epoll_ctl(m_epoll_fd, EPOLL_CTL_MOD, fd, &ev) != 0;
}

void Threadpool_listener::wake() {
  uint64_t val = 1;
  ssize_t n = write(m_wakeup_fd, &val, sizeof(val));
  (void)n;
}

void Threadpool_listener::ack_wakeup() {
  uint64_t val;
  ssize_t n = read(m_wakeup_fd, &val, sizeof(val));
  (void)n;
}

int Threadpool_listener::wait(int timeout_ms) {
  struct epoll_event raw_events[MAX_EVENTS];
  m_num_events = epoll_wait(m_epoll_fd, raw_events, MAX_EVENTS, timeout_ms);
  if (m_num_events <= 0) return m_num_events;

  // Copy into opaque storage so callers don't need the epoll_event type.
  for (int i = 0; i < m_num_events; i++) {
    m_event_ptrs[i] = raw_events[i].data.ptr;
    m_event_flags[i] = raw_events[i].events;
  }
  return m_num_events;
}

bool Threadpool_listener::is_wakeup_event(int index) const {
  return m_event_ptrs[index] == this;
}

Scheduler_data *Threadpool_listener::get_event_sd(int index) const {
  return static_cast<Scheduler_data *>(m_event_ptrs[index]);
}

#else  // !__linux__ — poll() fallback stub

bool Threadpool_listener::init() {
  // poll() fallback not yet implemented — return success.
  // The listener thread will spin with a short sleep.
  return false;
}

void Threadpool_listener::destroy() {}

bool Threadpool_listener::register_fd(THD *thd [[maybe_unused]]) {
  // Stub: no-op, always report success so callers proceed normally.
  return false;
}

bool Threadpool_listener::remove_fd(THD *thd [[maybe_unused]]) {
  return false;
}

bool Threadpool_listener::rearm_fd(THD *thd [[maybe_unused]]) {
  return false;
}

void Threadpool_listener::wake() {}
void Threadpool_listener::ack_wakeup() {}

int Threadpool_listener::wait(int timeout_ms) {
  usleep(static_cast<unsigned>(timeout_ms) * 1000);
  m_num_events = 0;
  return 0;
}

bool Threadpool_listener::is_wakeup_event(int index [[maybe_unused]]) const {
  return false;
}

Scheduler_data *Threadpool_listener::get_event_sd(int index [[maybe_unused]]) const {
  return nullptr;
}

#endif  // __linux__
