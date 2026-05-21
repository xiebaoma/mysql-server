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

  return false;
}

void Thread_group::destroy() {
  m_shutdown = true;

  // Wake all waiters so they can see shutdown and exit.
  m_high_priority_queue.signal_all();
  m_low_priority_queue.signal_all();
  mysql_cond_broadcast(&m_cond_worker);
  mysql_cond_broadcast(&m_cond_listener);

  // Drain any events still in the queues and clean up their resources.
  // After signal_all(), blocked workers will wake and exit; events that
  // remain in the queues were never dequeued and must be cleaned up here.
  Connection_event event;
  while (m_high_priority_queue.dequeue(&event, 0)) {
    cleanup_event(event);
  }
  while (m_low_priority_queue.dequeue(&event, 0)) {
    cleanup_event(event);
  }

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
  // The queue's internal cond_signal wakes one waiter — no need to
  // signal m_cond_worker here (workers block on the queue's condvar).
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
  if (m_shutdown) return;

  mysql_mutex_lock(&m_mutex);
  // Create a new worker if we are under the per-group oversubscription limit
  // and under the global max thread cap.  The oversubscribe parameter controls
  // how many concurrent workers a group may have (default 3, phase 1).
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

void *Thread_group::worker_main_cdecl(void *arg) {
  Thread_group *group = static_cast<Thread_group *>(arg);

  if (my_thread_init()) {
    // Thread initialization failed.
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

  while (!m_shutdown) {
    Connection_event event;

    // Wait for a connection with idle timeout.
    // -1 means block indefinitely, but we use a finite timeout to check
    // idle_timeout and shutdown periodically.  The actual idle exit is
    // handled by the timer thread in later phases.
    int timeout_ms = static_cast<int>(idle_timeout_sec) * 1000;

    if (!dequeue_connection(&event, timeout_ms)) {
      // Timeout or shutdown — loop back to check m_shutdown.
      continue;
    }

    if (m_shutdown) {
      // Dequeued an event just as shutdown was signaled.
      // Clean it up before exiting so no resources leak.
      cleanup_event(event);
      break;
    }

    m_last_activity_time.store(my_micro_time(), std::memory_order_relaxed);

    if (event.type == Connection_event_type::NEW_CONNECTION) {
      // --- New connection: create THD, authenticate, execute ---
      Channel_info *channel_info = event.data.channel_info;

      THD *thd = create_thd(channel_info);
      if (thd == nullptr) {
        destroy_channel_info(channel_info);
        dec_connection_count();
        increment_aborted_connects();
        continue;
      }

      // Allocate and attach scheduler data.  On OOM, clean up and abort
      // this connection (the THD was never added to Global_THD_manager).
      Scheduler_data *sd = new (std::nothrow) Scheduler_data();
      if (sd == nullptr) {
        // Vio was already transferred to THD by create_thd(), so
        // Channel_info is just a shell — delete it directly.
        delete channel_info;
        dec_connection_count();
        increment_aborted_connects();
        delete thd;
        continue;
      }
      thd_set_scheduler_data(thd, sd);

      thd_store_globals(thd);

      if (thd_prepare_connection(thd)) {
        // Authentication or init_connect failure.
        increment_aborted_connects();
      } else {
        sd->group = this;
        sd->state.store(CS_ACTIVE, std::memory_order_release);
        add_connection_to_list(sd);

        do_command(thd);

        remove_connection_from_list(sd);
        sd->state.store(CS_IDLE, std::memory_order_release);
        end_connection(thd);
      }

      close_connection(thd, 0, false, false);
      reset_thread_globals(thd);
      dec_connection_count();
      delete sd;
      delete thd;
    } else {
      // --- Ready connection: THD already exists, socket has data ---
      THD *thd = event.data.thd;

      if (thd_connection_alive(thd)) {
        thd_store_globals(thd);
        Scheduler_data *sd = static_cast<Scheduler_data *>(
            thd_get_scheduler_data(thd));
        if (sd != nullptr) {
          sd->state.store(CS_ACTIVE, std::memory_order_release);
        }

        bool close = do_command(thd);

        if (sd != nullptr) {
          sd->state.store(CS_IDLE, std::memory_order_release);
        }

        if (close) {
          end_connection(thd);
          close_connection(thd, 0, false, false);
          if (sd != nullptr) remove_connection_from_list(sd);
          reset_thread_globals(thd);
          dec_connection_count();
          delete sd;
          delete thd;
        }
        // If not closed, the connection goes back to IDLE (listener picks it
        // up in later phases). For now, the connection is done.
      } else {
        // Connection was killed/dropped while in queue.
        end_connection(thd);
        close_connection(thd, 0, false, false);
        Scheduler_data *sd = static_cast<Scheduler_data *>(
            thd_get_scheduler_data(thd));
        if (sd != nullptr) remove_connection_from_list(sd);
        reset_thread_globals(thd);
        dec_connection_count();
        delete sd;
        delete thd;
      }
    }
  }
}

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
      if (sd != nullptr) remove_connection_from_list(sd);
      if (thd_connection_alive(thd)) {
        end_connection(thd);
        close_connection(thd, 0, true, false);
      }
      reset_thread_globals(thd);
      dec_connection_count();
      delete sd;
      delete thd;
    }
  }
}
