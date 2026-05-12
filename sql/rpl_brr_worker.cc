/* Copyright (c) 2026, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "sql/rpl_brr_worker.h"

#include <stdio.h>

#include "mysql/thread_type.h"
#include "sql/log.h"                // sql_print_*
#include "sql/mysqld.h"             // opt_binlog_realtime_replication
#include "sql/mysqld_thd_manager.h"  // Global_THD_manager
#include "sql/rpl_brr_event.h"
#include "sql/rpl_brr_queue.h"
#include "sql/rpl_mi.h"
#include "sql/rpl_replica.h"       // init_replica_thread, SLAVE_THD_BRR
#include "sql/rpl_rli.h"
#include "sql/sql_class.h"         // THD

/**
  Process a single BRR event dequeued from the queue.

  @param thd   The BRR worker THD.
  @param mi    Master_info for this channel.
  @param ev    The dequeued BRR event.

  TODO Weeks 7-8: Implement full PREPARE/COMMIT/ROLLBACK DDL processing.
*/
static void process_brr_event(THD *, Master_info *mi, const Brr_event &ev) {
  switch (ev.type) {
    case mysql::binlog::event::BRR_DDL_PREPARE_EVENT:
      sql_print_information(
          "[BRR] Channel '%s': Received PREPARE event, ddl_id=%llu, "
          "table=%s.%s",
          mi->get_channel(),
          static_cast<unsigned long long>(ev.prepare.common.ddl_id),
          ev.prepare.schema_name.c_str(), ev.prepare.table_name.c_str());
      break;

    case mysql::binlog::event::BRR_DDL_COMMIT_EVENT:
      sql_print_information(
          "[BRR] Channel '%s': Received COMMIT event, ddl_id=%llu",
          mi->get_channel(),
          static_cast<unsigned long long>(ev.commit.common.ddl_id));
      break;

    case mysql::binlog::event::BRR_DDL_ROLLBACK_EVENT:
      sql_print_information(
          "[BRR] Channel '%s': Received ROLLBACK event, ddl_id=%llu",
          mi->get_channel(),
          static_cast<unsigned long long>(ev.rollback.common.ddl_id));
      break;

    default:
      break;
  }
}

/**
  Clean up any in-flight BRR DDL state, releasing GTID ownership if held.

  Must be called before the BRR worker exits to ensure the SQL worker
  is not blocked on a GTID that will never be released.

  @param thd  The BRR worker THD.
  @param rli  Relay_log_info for this channel.
*/
static void cleanup_in_flight_brr_ddl(THD *, Relay_log_info *rli) {
  // TODO Weeks 7-8: release GTID ownership, clean up intermediate objects
  rli->m_brr_queue.clear();
}

extern "C" void *handle_slave_brr(void *arg) {
  THD *thd{nullptr};
  bool thd_added{false};
  bool init_failed{false};
  Master_info *mi = static_cast<Master_info *>(arg);
  Relay_log_info *rli = mi->rli;

  my_thread_init();

  {
    DBUG_TRACE;

    mysql_mutex_lock(&rli->run_lock);

    rli->m_brr_worker_run_id++;

    thd = new THD;
    THD_CHECK_SENTRY(thd);
    thd->thread_stack = reinterpret_cast<char *>(&thd);

    /* Bind the PSI thread instrumentation to the THD. */
#ifdef HAVE_PSI_THREAD_INTERFACE
    {
      struct PSI_thread *psi = PSI_THREAD_CALL(get_thread)();
      thd_set_psi(thd, psi);
    }
#endif
    mysql_thread_set_psi_THD(thd);

    /* The BRR worker is a slave applier context — downstream code in
       e.g. DDL execution will access rli_slave for relay-log info. */
    thd->rli_slave = rli;

    /*
      Per Rpl_info contract (sql/rpl_info.h): writes to *_info_thd
      require holding both run_lock and info_thd_lock.  We already hold
      run_lock here.
    */
    mysql_mutex_lock(&rli->info_thd_lock);
    rli->m_brr_info_thd = thd;
    mysql_mutex_unlock(&rli->info_thd_lock);
    rli->m_brr_worker_running = 1;
    rli->m_brr_worker_abort.store(false);

    if (init_replica_thread(thd, SLAVE_THD_BRR)) {
      sql_print_error(
          "[BRR] Channel '%s': Failed to initialize BRR worker thread",
          mi->get_channel());
      init_failed = true;
    } else {
      Global_THD_manager::get_instance()->add_thd(thd);
      thd_added = true;
    }

    mysql_cond_broadcast(&rli->m_brr_start_cond);
    mysql_mutex_unlock(&rli->run_lock);

    if (!init_failed) {
      /* Reset the queue's aborted flag for a fresh start. */
      rli->m_brr_queue.reset_aborted();

      sql_print_information("[BRR] Channel '%s': BRR worker started",
                            mi->get_channel());

      /*
        Main loop: block on the BRR queue, process events one at a time.
        The loop exits when the BRR worker is aborted by the terminate path
        or the queue is aborted (IO disconnect).
      */
      while (!rli->m_brr_worker_abort.load()) {
        Brr_event ev;
        if (!rli->m_brr_queue.dequeue_blocking(&ev)) {
          if (!rli->m_brr_worker_abort.load())
            sql_print_warning(
                "[BRR] Channel '%s': BRR queue aborted, worker exiting",
                mi->get_channel());
          break;
        }

        if (rli->m_brr_worker_abort.load()) break;
        process_brr_event(thd, mi, ev);
      }

      /*
        Clean up any in-flight BRR DDL state.  Must happen within the
        THD lifecycle so GTID ownership can be released properly.
      */
      cleanup_in_flight_brr_ddl(thd, rli);
    }
  }

  sql_print_information("[BRR] Channel '%s': BRR worker stopping",
                        mi->get_channel());

  /*
    Unified cleanup path for both successful runs and init-failure runs.

    Hold run_lock while flipping m_brr_worker_running to 0 and clearing
    m_brr_info_thd so that terminate_slave_thread observes a consistent
    (running, info_thd) pair: once it acquires run_lock and sees
    running==0 it returns without touching the (now-deleted) THD; once
    it sees running==1 the THD is guaranteed to still exist because the
    worker can only delete it under run_lock.
  */
  mysql_mutex_lock(&rli->run_lock);

  rli->m_brr_worker_running = 0;

  mysql_mutex_lock(&rli->info_thd_lock);
  rli->m_brr_info_thd = nullptr;
  mysql_mutex_unlock(&rli->info_thd_lock);

  thd->release_resources();
  THD_CHECK_SENTRY(thd);
  if (thd_added) Global_THD_manager::get_instance()->remove_thd(thd);

  mysql_thread_set_psi_THD(nullptr);
  delete thd;

  mysql_cond_broadcast(&rli->m_brr_stop_cond);
  mysql_mutex_unlock(&rli->run_lock);

  my_thread_end();
  return nullptr;
}
