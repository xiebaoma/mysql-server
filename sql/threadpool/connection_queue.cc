#include "sql/threadpool/connection_queue.h"

#include <new>
#include <errno.h>

#include "include/my_systime.h"

bool Connection_queue::init() {
  mysql_mutex_init(0, &m_mutex, MY_MUTEX_INIT_FAST);
  mysql_cond_init(0, &m_cond);
  m_head = nullptr;
  m_tail = nullptr;
  m_length = 0;
  m_shutdown = false;
  return false;
}

void Connection_queue::destroy() {
  mysql_mutex_lock(&m_mutex);
  m_shutdown = true;
  mysql_cond_broadcast(&m_cond);  // wake blocked dequeuers

  // Clean up remaining elements while holding the lock,
  // so no new enqueue can sneak in during the gap.
  Element *elem = m_head;
  while (elem != nullptr) {
    Element *next = elem->next;
    delete elem;
    elem = next;
  }
  m_head = nullptr;
  m_tail = nullptr;
  m_length = 0;
  mysql_mutex_unlock(&m_mutex);

  mysql_cond_destroy(&m_cond);
  mysql_mutex_destroy(&m_mutex);
}

bool Connection_queue::enqueue(Connection_event event) {
  Element *elem = new (std::nothrow) Element;
  if (elem == nullptr) return false;

  elem->event = event;
  elem->next = nullptr;

  mysql_mutex_lock(&m_mutex);
  if (m_tail != nullptr) {
    m_tail->next = elem;
  } else {
    m_head = elem;
  }
  m_tail = elem;
  m_length.fetch_add(1, std::memory_order_release);
  mysql_cond_signal(&m_cond);
  mysql_mutex_unlock(&m_mutex);
  return true;
}

bool Connection_queue::dequeue(Connection_event *event, int timeout_ms) {
  mysql_mutex_lock(&m_mutex);

  while (m_head == nullptr && !m_shutdown) {
    if (timeout_ms == 0) {
      mysql_mutex_unlock(&m_mutex);
      return false;
    }
    if (timeout_ms < 0) {
      mysql_cond_wait(&m_cond, &m_mutex);
    } else {
      struct timespec ts;
      set_timespec_nsec(&ts,
                        static_cast<ulonglong>(timeout_ms) * 1000000ULL);
      int ret = mysql_cond_timedwait(&m_cond, &m_mutex, &ts);
      if (ret == ETIMEDOUT || ret == ETIME) {
        mysql_mutex_unlock(&m_mutex);
        return false;
      }
    }
  }

  if (m_shutdown || m_head == nullptr) {
    mysql_mutex_unlock(&m_mutex);
    return false;
  }

  Element *elem = m_head;
  *event = elem->event;
  m_head = elem->next;
  if (m_head == nullptr) m_tail = nullptr;
  m_length.fetch_sub(1, std::memory_order_release);

  mysql_mutex_unlock(&m_mutex);
  delete elem;
  return true;
}

void Connection_queue::signal_all() {
  mysql_mutex_lock(&m_mutex);
  m_shutdown = true;
  mysql_cond_broadcast(&m_cond);
  mysql_mutex_unlock(&m_mutex);
}
