#include "sql/threadpool/thread_group.h"
#include "sql/threadpool/thread_pool.h"

#include <new>

#include "include/my_systime.h"
#include "include/mysql/thread_pool_priv.h"
#include "sql/mysqld_thd_manager.h"
#include "sql/sql_class.h"

bool Thread_group::init(uint group_id) {
  m_group_id = group_id;

  if (m_high_priority_queue.init() || m_low_priority_queue.init()) {
    return true;
  }

  mysql_mutex_init(0, &m_mutex, MY_MUTEX_INIT_FAST);
  mysql_cond_init(0, &m_cond_worker);
  mysql_cond_init(0, &m_cond_listener);

  if (m_listener.init()) {
    mysql_cond_destroy(&m_cond_listener);
    mysql_cond_destroy(&m_cond_worker);
    mysql_mutex_destroy(&m_mutex);
    m_high_priority_queue.destroy();
    m_low_priority_queue.destroy();
    return true;
  }

  return create_listener();
}

bool Thread_group::create_listener() {
  my_thread_attr_t *attr = get_connection_attrib();

  m_listener_running.store(true, std::memory_order_release);
  if (my_thread_create(&m_listener_handle, attr, listener_main_cdecl,
                       this) != 0) {
    m_listener_running.store(false, std::memory_order_release);
    return true;
  }
  mysql_mutex_lock(&m_mutex);
  m_listener_thread_count++;
  m_active_thread_count++;
  mysql_mutex_unlock(&m_mutex);
  return false;
}

void Thread_group::destroy() {
  m_shutdown.store(true, std::memory_order_relaxed);

  // Wake the listener so it can exit its epoll_wait.
  if (m_listener_running.load(std::memory_order_relaxed)) {
    m_listener.wake();
  }

  // Wake all workers blocked on queues.
  m_high_priority_queue.signal_all();
  m_low_priority_queue.signal_all();
  mysql_cond_broadcast(&m_cond_worker);
  mysql_cond_broadcast(&m_cond_listener);

  // Drain any remaining events from the queues.
  Connection_event event;
  while (m_high_priority_queue.dequeue(&event, 0)) {
    cleanup_event(event);
  }
  while (m_low_priority_queue.dequeue(&event, 0)) {
    cleanup_event(event);
  }

  // Join the listener thread before destroying its resources.
  if (m_listener_running.load(std::memory_order_relaxed)) {
    my_thread_join(&m_listener_handle, nullptr);
  }

  m_listener.destroy();

  m_high_priority_queue.destroy();
  m_low_priority_queue.destroy();

  mysql_mutex_destroy(&m_mutex);
  mysql_cond_destroy(&m_cond_worker);
  mysql_cond_destroy(&m_cond_listener);
}

bool Thread_group::enqueue_connection(Connection_event event) {
  // For now, all events go to the low priority queue.
  // High priority scheduling will be added in Phase 4.
  if (!m_low_priority_queue.enqueue(event)) return false;
  return true;
}

bool Thread_group::dequeue_connection(Connection_event *event, int timeout_ms) {
  // Try high priority queue first (non-blocking), then low.
  if (m_high_priority_queue.dequeue(event, 0)) {
    m_dequeue_count.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  // Wait on the low priority queue with the requested timeout.
  if (m_low_priority_queue.dequeue(event, timeout_ms)) {
    m_dequeue_count.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  return false;
}

void Thread_group::wake_or_create_thread() {
  if (m_shutdown.load(std::memory_order_relaxed)) return;

  mysql_mutex_lock(&m_mutex);
  if (m_worker_thread_count < static_cast<int>(Thread_pool::s_oversubscribe_par) &&
      m_active_thread_count < MAX_THREADS_PER_GROUP) {
    my_thread_handle handle;
    my_thread_attr_t *attr = get_connection_attrib();

    if (my_thread_create(&handle, attr, worker_main_cdecl, this) == 0) {
      m_worker_thread_count++;
      m_active_thread_count++;
      Global_THD_manager::get_instance()->inc_thread_created();
    }
  }
  mysql_mutex_unlock(&m_mutex);
}

// ===================== THD cleanup helper =====================

void Thread_group::cleanup_thd_connection(THD *thd, Scheduler_data *sd,
                                          bool server_shutdown) {
  end_connection(thd);
  close_connection(thd, 0, server_shutdown, false);
  thd->release_resources();
  Global_THD_manager::get_instance()->remove_thd(thd);
  reset_thread_globals(thd);
  dec_connection_count();
  delete sd;
  delete thd;
}

// ===================== Listener thread =====================

void *Thread_group::listener_main_cdecl(void *arg) {
  Thread_group *group = static_cast<Thread_group *>(arg);

  if (my_thread_init()) {
    mysql_mutex_lock(&group->m_mutex);
    group->m_listener_thread_count--;
    group->m_active_thread_count--;
    group->m_listener_running.store(false, std::memory_order_release);
    mysql_mutex_unlock(&group->m_mutex);
    return nullptr;
  }

  group->listener_main();

  my_thread_end();
  mysql_mutex_lock(&group->m_mutex);
  group->m_listener_thread_count--;
  group->m_active_thread_count--;
  group->m_listener_running.store(false, std::memory_order_release);
  mysql_mutex_unlock(&group->m_mutex);
  return nullptr;
}

void Thread_group::listener_main() {
  while (!m_shutdown.load(std::memory_order_relaxed)) {
    int nfds = m_listener.wait(Threadpool_listener::DEFAULT_TIMEOUT_MS);

    if (nfds < 0) {
      if (errno == EINTR) continue;
      break;
    }

    for (int i = 0; i < nfds; i++) {
      if (m_listener.is_wakeup_event(i)) {
        m_listener.ack_wakeup();
        continue;
      }

      THD *thd = m_listener.get_event_thd(i);
      Scheduler_data *sd =
          static_cast<Scheduler_data *>(thd_get_scheduler_data(thd));
      if (sd == nullptr) continue;

      int expected = CS_IDLE;
      if (!sd->state.compare_exchange_strong(expected, CS_QUEUED,
                                             std::memory_order_acq_rel,
                                             std::memory_order_relaxed))
        continue;

      Connection_event event;
      event.type = Connection_event_type::READY_CONNECTION;
      event.data.thd = thd;

      if (!enqueue_connection(event)) {
        // OOM: revert to IDLE and rearm so we don't lose the connection.
        sd->state.store(CS_IDLE, std::memory_order_release);
        m_listener.rearm_fd(thd);
        continue;
      }
      wake_or_create_thread();
    }
  }
}

// ===================== Worker thread =====================

void *Thread_group::worker_main_cdecl(void *arg) {
  Thread_group *group = static_cast<Thread_group *>(arg);

  if (my_thread_init()) {
    mysql_mutex_lock(&group->m_mutex);
    group->m_worker_thread_count--;
    group->m_active_thread_count--;
    mysql_mutex_unlock(&group->m_mutex);
    return nullptr;
  }

  group->worker_main();

  my_thread_end();
  mysql_mutex_lock(&group->m_mutex);
  group->m_worker_thread_count--;
  group->m_active_thread_count--;
  mysql_mutex_unlock(&group->m_mutex);
  return nullptr;
}

void Thread_group::worker_main() {
  const ulong idle_timeout_sec = Thread_pool::s_idle_timeout;

  while (true) {
    Connection_event event;

    int timeout_ms = static_cast<int>(idle_timeout_sec) * 1000;

    if (!dequeue_connection(&event, timeout_ms)) {
      if (m_shutdown.load(std::memory_order_relaxed)) return;
      continue;
    }

    if (m_shutdown.load(std::memory_order_relaxed)) {
      cleanup_event(event);
      continue;
    }

    m_last_activity_time.store(my_micro_time(), std::memory_order_relaxed);

    if (event.type == Connection_event_type::NEW_CONNECTION) {
      // ========== NEW_CONNECTION ==========
      Channel_info *channel_info = event.data.channel_info;

      THD *thd = create_thd(channel_info);
      if (thd == nullptr) {
        destroy_channel_info(channel_info);
        dec_connection_count();
        increment_aborted_connects();
        continue;
      }

      Scheduler_data *sd = new (std::nothrow) Scheduler_data();
      if (sd == nullptr) {
        delete channel_info;
        dec_connection_count();
        increment_aborted_connects();
        delete thd;
        continue;
      }
      thd_set_scheduler_data(thd, sd);
      sd->thd = thd;

      thd_store_globals(thd);
      Global_THD_manager::get_instance()->add_thd(thd);

      if (thd_prepare_connection(thd)) {
        increment_aborted_connects();
        cleanup_thd_connection(thd, sd, false);
        continue;
      }

      // Authentication succeeded.
      sd->group = this;
      add_connection_to_list(sd);

      if (thd_connection_has_data(thd)) {
        sd->state.store(CS_ACTIVE, std::memory_order_release);

        bool close = do_command(thd);

        int expected = CS_ACTIVE;
        sd->state.compare_exchange_strong(expected, CS_IDLE,
                                          std::memory_order_release,
                                          std::memory_order_relaxed);
        if (expected == CS_CLOSING) close = true;

        if (close || !thd_connection_alive(thd) ||
            m_shutdown.load(std::memory_order_relaxed)) {
          remove_connection_from_list(sd);
          cleanup_thd_connection(thd, sd, false);
        } else if (thd_connection_has_data(thd)) {
          sd->state.store(CS_QUEUED, std::memory_order_release);
          Connection_event reev;
          reev.type = Connection_event_type::READY_CONNECTION;
          reev.data.thd = thd;
          if (!enqueue_connection(reev)) {
            remove_connection_from_list(sd);
            cleanup_thd_connection(thd, sd, false);
          }
        } else {
          if (register_connection_fd(thd)) {
            remove_connection_from_list(sd);
            cleanup_thd_connection(thd, sd, false);
          } else if (sd->state.load(std::memory_order_acquire) != CS_IDLE) {
            // KILL arrived between state transition to CS_IDLE and the
            // fd being registered.  post_kill_notification_cb already
            // enqueued a READY_CONNECTION event for cleanup — just undo
            // our fd registration so the listener doesn't see a dangling THD.
            remove_connection_fd(thd);
          }
          // Connection stays alive under listener.
        }
      } else {
        if (m_shutdown.load(std::memory_order_relaxed)) {
          remove_connection_from_list(sd);
          cleanup_thd_connection(thd, sd, true);
        } else {
          if (register_connection_fd(thd)) {
            remove_connection_from_list(sd);
            cleanup_thd_connection(thd, sd, false);
          } else if (sd->state.load(std::memory_order_acquire) != CS_IDLE) {
            // KILL arrived between setting CS_IDLE and registering fd.
            remove_connection_fd(thd);
          }
        }
      }
    } else {
      // ========== READY_CONNECTION ==========
      THD *thd = event.data.thd;
      Scheduler_data *sd =
          static_cast<Scheduler_data *>(thd_get_scheduler_data(thd));

      if (sd == nullptr) {
        // Should not happen — every READY_CONNECTION must have scheduler data.
        // Clean up defensively.
        thd_store_globals(thd);
        close_connection(thd, 0, false, false);
        thd->release_resources();
        Global_THD_manager::get_instance()->remove_thd(thd);
        reset_thread_globals(thd);
        dec_connection_count();
        delete thd;
        continue;
      }

      // Check if the connection was killed while queued.
      if (sd->state.load(std::memory_order_acquire) == CS_CLOSING) {
        thd_store_globals(thd);
        remove_connection_from_list(sd);
        // KILL may have already called end_connection/close_connection.
        if (thd_connection_alive(thd)) {
          cleanup_thd_connection(thd, sd, false);
        } else {
          thd->release_resources();
          Global_THD_manager::get_instance()->remove_thd(thd);
          reset_thread_globals(thd);
          dec_connection_count();
          delete sd;
          delete thd;
        }
        continue;
      }

      if (!thd_connection_alive(thd)) {
        thd_store_globals(thd);
        remove_connection_from_list(sd);
        cleanup_thd_connection(thd, sd, false);
        continue;
      }

      // Connection is alive — process one command.
      thd_store_globals(thd);
      sd->state.store(CS_ACTIVE, std::memory_order_release);

      bool close = do_command(thd);

      int expected = CS_ACTIVE;
      sd->state.compare_exchange_strong(expected, CS_IDLE,
                                        std::memory_order_release,
                                        std::memory_order_relaxed);
      if (expected == CS_CLOSING) close = true;

      if (close || !thd_connection_alive(thd) ||
          m_shutdown.load(std::memory_order_relaxed)) {
        remove_connection_from_list(sd);
        cleanup_thd_connection(thd, sd, false);
      } else if (thd_connection_has_data(thd)) {
        sd->state.store(CS_QUEUED, std::memory_order_release);
        Connection_event reev;
        reev.type = Connection_event_type::READY_CONNECTION;
        reev.data.thd = thd;
        if (!enqueue_connection(reev)) {
          remove_connection_from_list(sd);
          cleanup_thd_connection(thd, sd, false);
        }
      } else {
        if (rearm_connection_fd(thd)) {
          remove_connection_from_list(sd);
          cleanup_thd_connection(thd, sd, false);
        } else if (sd->state.load(std::memory_order_acquire) != CS_IDLE) {
          // KILL arrived between CAS and rearm_fd.
          remove_connection_fd(thd);
        }
      }
    }
  }
}

// ===================== FD management helpers =====================

bool Thread_group::register_connection_fd(THD *thd) {
  return m_listener.register_fd(thd);
}

bool Thread_group::remove_connection_fd(THD *thd) {
  return m_listener.remove_fd(thd);
}

bool Thread_group::rearm_connection_fd(THD *thd) {
  return m_listener.rearm_fd(thd);
}

void Thread_group::drain_idle_connections() {
  mysql_mutex_lock(&m_mutex);
  while (m_connections_head != nullptr) {
    // Find the first CS_IDLE connection in the list.
    Scheduler_data *sd = m_connections_head;
    while (sd != nullptr) {
      int expected = CS_IDLE;
      if (sd->state.compare_exchange_strong(expected, CS_CLOSING,
                                            std::memory_order_acq_rel,
                                            std::memory_order_relaxed))
        break;
      sd = sd->next_in_group;
    }
    if (sd == nullptr) break;  // no more IDLE connections

    THD *thd = sd->thd;
    remove_connection_fd(thd);
    remove_connection_from_list(sd);
    mysql_mutex_unlock(&m_mutex);

    thd_store_globals(thd);
    cleanup_thd_connection(thd, sd, /*server_shutdown=*/true);

    mysql_mutex_lock(&m_mutex);
  }
  mysql_mutex_unlock(&m_mutex);
}

// ===================== Connection list helpers =====================

void Thread_group::add_connection_to_list(Scheduler_data *sd) {
  mysql_mutex_lock(&m_mutex);
  sd->prev_in_group = nullptr;
  sd->next_in_group = m_connections_head;
  if (m_connections_head != nullptr) {
    m_connections_head->prev_in_group = sd;
  } else {
    m_connections_tail = sd;
  }
  m_connections_head = sd;
  mysql_mutex_unlock(&m_mutex);
}

void Thread_group::remove_connection_from_list(Scheduler_data *sd) {
  mysql_mutex_lock(&m_mutex);
  if (sd->prev_in_group != nullptr) {
    sd->prev_in_group->next_in_group = sd->next_in_group;
  } else {
    m_connections_head = sd->next_in_group;
  }
  if (sd->next_in_group != nullptr) {
    sd->next_in_group->prev_in_group = sd->prev_in_group;
  } else {
    m_connections_tail = sd->prev_in_group;
  }
  sd->next_in_group = nullptr;
  sd->prev_in_group = nullptr;
  mysql_mutex_unlock(&m_mutex);
}

void Thread_group::cleanup_event(Connection_event event) {
  if (event.type == Connection_event_type::NEW_CONNECTION) {
    destroy_channel_info(event.data.channel_info);
    dec_connection_count();
  } else {
    THD *thd = event.data.thd;
    if (thd != nullptr) {
      thd_store_globals(thd);
      Scheduler_data *sd =
          static_cast<Scheduler_data *>(thd_get_scheduler_data(thd));
      remove_connection_fd(thd);
      if (sd != nullptr) remove_connection_from_list(sd);
      if (thd_connection_alive(thd)) {
        end_connection(thd);
        close_connection(thd, 0, true, false);
      }
      thd->release_resources();
      Global_THD_manager::get_instance()->remove_thd(thd);
      reset_thread_globals(thd);
      dec_connection_count();
      delete sd;
      delete thd;
    }
  }
}
