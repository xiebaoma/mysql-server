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

#ifndef RPL_BRR_WORKER_H
#define RPL_BRR_WORKER_H

#include "my_thread.h"             // my_thread_handle
#include "sql/rpl_brr_event.h"  // Brr_ddl_prepare_event
#include "sql/rpl_gtid.h"       // Gtid

struct Master_info;
struct Relay_log_info;

// ==========================================================================
//  BRR replica state machine
// ==========================================================================

/// States of the replica BRR worker state machine.
enum class Brr_replica_state : int {
  RPL_INIT = 0,               // No in-flight BRR DDL
  RPL_PREPARE_RECEIVED,       // PREPARE event dequeued
  RPL_VALIDATE,               // Validating event fields & local whitelist
  RPL_GTID_OWNING,            // Acquiring GTID ownership
  RPL_WAIT_PREPARE_DEP,       // Waiting for prepare dependency GTID set
  RPL_EXECUTING,              // Executing DDL main body (on separate thread)
  RPL_WAIT_SOURCE_RESULT,     // DDL body done, waiting for COMMIT/ROLLBACK
  RPL_COMMIT_RECEIVED,        // COMMIT event received
  RPL_WAIT_COMMIT_DEP,        // Waiting for commit dependency GTID set
  RPL_COMMITTING,             // Committing DDL
  RPL_COMMITTED,              // DDL committed, GTID in gtid_executed
  RPL_ROLLBACK_RECEIVED,      // ROLLBACK event received
  RPL_ROLLING_BACK,           // Rolling back DDL
  RPL_FALLBACK,               // Releasing to original relay-log DDL path
  RPL_ABORTED                 // Unrecoverable — stop replication
};

/// Three-way result of GTID validation.
enum class Brr_gtid_status { VALID, ALREADY_EXECUTED, OWNED_BY_OTHER, ERROR };

inline const char *brr_replica_state_name(Brr_replica_state s) {
  switch (s) {
    case Brr_replica_state::RPL_INIT:               return "INIT";
    case Brr_replica_state::RPL_PREPARE_RECEIVED:   return "PREPARE_RECEIVED";
    case Brr_replica_state::RPL_VALIDATE:           return "VALIDATE";
    case Brr_replica_state::RPL_GTID_OWNING:        return "GTID_OWNING";
    case Brr_replica_state::RPL_WAIT_PREPARE_DEP:   return "WAIT_PREPARE_DEP";
    case Brr_replica_state::RPL_EXECUTING:          return "EXECUTING";
    case Brr_replica_state::RPL_WAIT_SOURCE_RESULT: return "WAIT_SOURCE_RESULT";
    case Brr_replica_state::RPL_COMMIT_RECEIVED:    return "COMMIT_RECEIVED";
    case Brr_replica_state::RPL_WAIT_COMMIT_DEP:    return "WAIT_COMMIT_DEP";
    case Brr_replica_state::RPL_COMMITTING:         return "COMMITTING";
    case Brr_replica_state::RPL_COMMITTED:          return "COMMITTED";
    case Brr_replica_state::RPL_ROLLBACK_RECEIVED:  return "ROLLBACK_RECEIVED";
    case Brr_replica_state::RPL_ROLLING_BACK:       return "ROLLING_BACK";
    case Brr_replica_state::RPL_FALLBACK:           return "FALLBACK";
    case Brr_replica_state::RPL_ABORTED:            return "ABORTED";
  }
  return "UNKNOWN";
}

// ==========================================================================
//  DDL execution thread synchronisation
// ==========================================================================

/**
   Shared state between the BRR worker and the DDL execution thread.

   The DDL thread pauses after ha_inplace_alter_table() and waits for the
   BRR worker to deliver a COMMIT/ROLLBACK decision from the source.
*/
struct Brr_ddl_exec_ctx {
  mysql_mutex_t mutex;
  mysql_cond_t cond_ddl_paused;   // DDL thread signals when paused
  mysql_cond_t cond_resume;       // BRR worker signals commit/rollback
  mysql_cond_t cond_thd_ready;    // DDL thread signals THD created
  mysql_cond_t cond_gtid_transferred;  // BRR worker signals GTID transferred

  bool ddl_paused{false};         // DDL thread reached pause point
  bool should_commit{false};      // true = commit, false = rollback
  bool decision_ready{false};     // BRR worker has set should_commit
  bool ddl_done{false};           // DDL thread completed execution
  int ddl_error{0};               // error code from DDL thread (0 = success)
  my_thread_handle ddl_thread;    // handle of the DDL execution thread
  const std::atomic<bool> *worker_abort{nullptr};  // -> m_brr_worker_abort

  // GTID transfer handshake
  bool thd_ready{false};          // DDL thread created THD, ready for transfer
  bool gtid_transferred{false};   // BRR worker has transferred GTID ownership
  THD *ddl_thd{nullptr};          // DDL thread's THD (set by DDL thread)

  Master_info *mi{nullptr};       // channel info

  void init() {
    mysql_mutex_init(0, &mutex, MY_MUTEX_INIT_FAST);
    mysql_cond_init(0, &cond_ddl_paused);
    mysql_cond_init(0, &cond_resume);
    mysql_cond_init(0, &cond_thd_ready);
    mysql_cond_init(0, &cond_gtid_transferred);
  }

  void destroy() {
    mysql_cond_destroy(&cond_gtid_transferred);
    mysql_cond_destroy(&cond_thd_ready);
    mysql_cond_destroy(&cond_resume);
    mysql_cond_destroy(&cond_ddl_paused);
    mysql_mutex_destroy(&mutex);
  }

  /// Called by DDL thread: block until BRR worker makes a decision.
  /// Returns true if should commit, false if should rollback.
  bool wait_for_decision() {
    mysql_mutex_lock(&mutex);
    ddl_paused = true;
    mysql_cond_signal(&cond_ddl_paused);
    while (!decision_ready)
      mysql_cond_wait(&cond_resume, &mutex);
    bool commit = should_commit;
    mysql_mutex_unlock(&mutex);
    return commit;
  }

  /// Called by BRR worker: signal the DDL thread to proceed.
  void signal_decision(bool commit) {
    mysql_mutex_lock(&mutex);
    should_commit = commit;
    decision_ready = true;
    mysql_cond_signal(&cond_resume);
    mysql_mutex_unlock(&mutex);
  }

  /// Called by BRR worker: wait for DDL thread to reach the pause point.
  /// Returns a snapshot of ddl_error taken under the mutex.
  /// Uses a timed wait (100 ms poll interval) so that STOP REPLICA / IO
  /// disconnect can interrupt the wait via m_brr_worker_abort.
  int wait_paused() {
    int err;
    struct timespec abstime;
    mysql_mutex_lock(&mutex);
    while (!ddl_paused) {
      // Check for abort (STOP REPLICA, IO disconnect, server shutdown)
      if (worker_abort != nullptr && worker_abort->load()) {
        mysql_mutex_unlock(&mutex);
        return 1;  // non-zero = aborted
      }
      set_timespec_nsec(&abstime, 100 * 1000000ULL);  // 100 ms
      mysql_cond_timedwait(&cond_ddl_paused, &mutex, &abstime);
    }
    err = ddl_error;
    mysql_mutex_unlock(&mutex);
    return err;
  }

  /// Read ddl_error under the mutex (avoids data race with DDL thread).
  int get_error() {
    mysql_mutex_lock(&mutex);
    int err = ddl_error;
    mysql_mutex_unlock(&mutex);
    return err;
  }

  /// Called by BRR worker: poll until the DDL thread is done.
  /// Prefer joining the thread directly over using this method.
  void wait_done() {
    mysql_mutex_lock(&mutex);
    while (!ddl_done) {
      mysql_mutex_unlock(&mutex);
      my_sleep(10000);  // 10 ms
      mysql_mutex_lock(&mutex);
    }
    mysql_mutex_unlock(&mutex);
  }
};

// ==========================================================================
//  In-flight BRR DDL state
// ==========================================================================

/**
   Tracks the state of a single in-flight BRR DDL on the replica.

   Only one in-flight DDL is allowed per channel in phase 1.
*/
struct Brr_inflight_ddl {
  Brr_replica_state state{Brr_replica_state::RPL_INIT};
  Brr_ddl_prepare_event prepare_ev;
  Gtid owned_gtid;
  bool gtid_owned{false};
  Brr_ddl_exec_ctx exec_ctx;
  const char *fallback_reason{nullptr};

  bool is_active() const {
    return state != Brr_replica_state::RPL_INIT &&
           state != Brr_replica_state::RPL_FALLBACK &&
           state != Brr_replica_state::RPL_COMMITTED &&
           state != Brr_replica_state::RPL_ABORTED;
  }
};

// ==========================================================================
//  Function declarations
// ==========================================================================

/**
   BRR worker thread entry point.

   Consumes BRR events from the in-memory Brr_queue, pre-executes DDL
   on the replica in parallel with the source, and commits or rolls back
   based on the source's result.

   @param arg  Pointer to Master_info for this channel.
   @return nullptr.
*/
extern "C" void *handle_slave_brr(void *arg);

/**
   DDL execution thread entry point.

   Runs the ALTER TABLE body on a separate THD, pauses before commit,
   and waits for the BRR worker to signal commit or rollback.

   @param arg  Pointer to Brr_ddl_exec_ctx.
   @return nullptr.
*/
extern "C" void *handle_brr_ddl_exec(void *arg);

/**
   Apply session variables from a BRR PREPARE event to a THD.

   The variables are encoded as null-separated key=value pairs, e.g.:
   "sql_mode=123\0character_set_client=utf8mb4\0..."

   @param thd   Target THD.
   @param ev    PREPARE event carrying the serialised session variables.
   @return true on success, false if parsing failed (non-fatal: vars are
           left at thread defaults).
*/
bool apply_session_vars_from_event(THD *thd, const Brr_ddl_prepare_event &ev);

#endif  // RPL_BRR_WORKER_H
