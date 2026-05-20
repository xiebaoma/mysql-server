#include "sql/threadpool/thread_pool.h"

#include <new>
#include <unistd.h>

#include "include/mysql/thread_pool_priv.h"
#include "sql/conn_handler/channel_info.h"
#include "sql/sql_class.h"  // THD (complete type for delete)

// Static variable definitions
Thread_pool *Thread_pool::s_instance = nullptr;
uint Thread_pool::s_pool_size = 0;
uint Thread_pool::s_max_threads = 100000;
ulong Thread_pool::s_stall_limit = 500;
ulong Thread_pool::s_idle_timeout = 60;
uint Thread_pool::s_oversubscribe_par = 3;

bool Thread_pool::init() {
  m_group_count = s_pool_size;
  if (m_group_count == 0) {
    long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
    m_group_count = (ncpus > 0) ? static_cast<uint>(ncpus) : 4;
  }

  m_groups = new (std::nothrow) Thread_group[m_group_count];
  if (m_groups == nullptr) return true;

  for (uint i = 0; i < m_group_count; i++) {
    if (m_groups[i].init(i)) {
      destroy();
      return true;
    }
  }

  s_instance = this;
  return false;
}

void Thread_pool::destroy() {
  if (m_groups != nullptr) {
    for (uint i = 0; i < m_group_count; i++) {
      m_groups[i].destroy();
    }
    delete[] m_groups;
    m_groups = nullptr;
  }
  s_instance = nullptr;
}

bool Thread_pool_connection_handler::add_connection(
    Channel_info *channel_info) {
  THD *thd = create_thd(channel_info);
  if (thd == nullptr) return true;

  thd_store_globals(thd);
  bool error = thd_prepare_connection(thd);
  if (!error) {
    do_command(thd);
  }
  end_connection(thd);
  close_connection(thd, 0, false, false);
  reset_thread_globals(thd);
  dec_connection_count();
  delete thd;

  return error;
}

uint Thread_pool_connection_handler::get_max_threads() const {
  return Thread_pool::s_max_threads;
}
