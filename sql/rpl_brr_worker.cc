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
#include <string.h>

#include "mysql/gtid/tsid.h"
#include "mysql/gtid/uuid.h"
#include "mysql/thread_type.h"
#include "sql/log.h"                 // sql_print_*
#include "sql/log_event.h"           // Parser_state
#include "sql/sql_parse.h"          // dispatch_sql_command
#include "sql/tztime.h"             // my_tz_find
#include "sql/mysqld.h"              // opt_binlog_realtime_replication, server_id
#include "sql/mysqld_thd_manager.h"  // Global_THD_manager
#include "sql/rpl_brr_event.h"
#include "sql/rpl_brr_queue.h"
#include "sql/rpl_gtid.h"            // Gtid, Gtid_specification, Gtid_state, set_gtid_next
#include "sql/rpl_mi.h"
#include "sql/rpl_replica.h"         // init_replica_thread, SLAVE_THD_BRR
#include "sql/rpl_rli.h"
#include "sql/sql_class.h"           // THD
#include "sql/sql_lex.h"             // LEX

// ==========================================================================
//  Forward declarations of static helpers
// ==========================================================================

static void process_brr_prepare(THD *thd, Master_info *mi, Relay_log_info *rli,
                                const Brr_event &ev,
                                Brr_inflight_ddl *inflight);
static bool validate_brr_prepare(const Brr_ddl_prepare_event &ev);
static Brr_gtid_status own_brr_gtid(THD *thd, const Brr_ddl_prepare_event &ev,
                                    Gtid *out_gtid);
static bool wait_brr_prepare_dep(THD *thd, Relay_log_info *rli,
                                 const Brr_ddl_prepare_event &ev);
static void process_brr_commit(THD *thd, Master_info *mi, Relay_log_info *rli,
                               const Brr_event &ev,
                               Brr_inflight_ddl *inflight);
static void process_brr_rollback(THD *thd, Master_info *mi, Relay_log_info *rli,
                                 const Brr_event &ev,
                                 Brr_inflight_ddl *inflight);


// ==========================================================================
//  Validation helpers (mirrors source-side Brr_source_ddl_context logic)
// ==========================================================================

static bool is_system_schema_name(const char *db) {
  if (db == nullptr) return true;
  return is_infoschema_db(db) || is_perfschema_db(db) ||
         !my_strcasecmp(system_charset_info, MYSQL_SCHEMA_NAME.str, db) ||
         !my_strcasecmp(system_charset_info, "sys", db);
}

/**
   Replica-side validation of a BRR PREPARE event.

   Checks fields that the replica can verify without opening the target table:
   - event_version matches
   - ddl_id is non-zero
   - source_server_uuid is not empty
   - gtid_gno is positive
   - schema_name is not a system schema
   - schema_name and table_name are non-empty

   Deeper structural checks (e.g. whether the engine is InnoDB, whether the
   table is partitioned, whether the index is supported) can only be done
   after opening the table and are therefore deferred to the DDL execution
   thread — if the DDL is not locally applicable the execution will fail and
   the worker will fallback.

   @return true if the event passes basic validation.
*/
static bool validate_brr_prepare(const Brr_ddl_prepare_event &ev) {
  if (ev.common.event_version != BRR_EVENT_VERSION) {
    sql_print_warning("[BRR] Validate failed: event_version=%u, expected=%u",
                      ev.common.event_version, BRR_EVENT_VERSION);
    return false;
  }

  if (ev.common.ddl_id == 0) {
    sql_print_warning("[BRR] Validate failed: ddl_id is zero");
    return false;
  }

  if (ev.common.source_server_uuid[0] == '\0') {
    sql_print_warning("[BRR] Validate failed: source_server_uuid is empty");
    return false;
  }

  if (ev.common.gtid_gno <= 0) {
    sql_print_warning("[BRR] Validate failed: gtid_gno=%lld",
                      static_cast<long long>(ev.common.gtid_gno));
    return false;
  }

  if (ev.schema_name.empty()) {
    sql_print_warning("[BRR] Validate failed: schema_name is empty");
    return false;
  }

  if (is_system_schema_name(ev.schema_name.c_str())) {
    sql_print_warning("[BRR] Validate failed: schema '%s' is a system schema",
                      ev.schema_name.c_str());
    return false;
  }

  if (ev.table_name.empty()) {
    sql_print_warning("[BRR] Validate failed: table_name is empty");
    return false;
  }

  if (ev.query.empty()) {
    sql_print_warning("[BRR] Validate failed: query is empty");
    return false;
  }

  return true;
}

// ==========================================================================
//  GTID validation
// ==========================================================================

/**
   Validate the GTID from a BRR PREPARE event.

   Converts source_server_uuid to a sidno, checks that the GTID is not
   already in gtid_executed, but does NOT acquire ownership on the BRR
   worker THD.  Ownership will be acquired later on the DDL execution
   thread's THD (via set_gtid_next) so that GTID ownership follows the
   THD that actually executes and commits the DDL.

   @param thd      The BRR worker THD (ownership is NOT acquired on it).
   @param ev       The PREPARE event.
   @param[out] out_gtid  The validated GTID (only valid on VALID return).
   @return VALID           — GTID is valid and not yet executed.
           ALREADY_EXECUTED — GTID is already in gtid_executed (should skip).
           ERROR           — UUID parse / sidno mapping failed (should fallback).
*/
static Brr_gtid_status own_brr_gtid(THD *, const Brr_ddl_prepare_event &ev,
                                    Gtid *out_gtid) {
  out_gtid->clear();

  mysql::gtid::Uuid uuid;
  if (uuid.parse(ev.common.source_server_uuid, BRR_UUID_STRING_LENGTH) != 0) {
    sql_print_warning("[BRR] GTID validate failed: cannot parse source uuid '%s'",
                      ev.common.source_server_uuid);
    return Brr_gtid_status::ERROR;
  }

  mysql::gtid::Tsid tsid(uuid);
  rpl_sidno sidno = get_sidno_from_global_tsid_map(tsid);
  if (sidno <= 0) {
    sql_print_warning("[BRR] GTID validate failed: cannot map uuid to sidno");
    return Brr_gtid_status::ERROR;
  }

  rpl_gno gno = static_cast<rpl_gno>(ev.common.gtid_gno);

  // Check if the GTID has already been executed (e.g. by the SQL worker).
  Gtid gtid;
  gtid.set(sidno, gno);
  global_tsid_lock->rdlock();
  bool already_executed = gtid_state->is_executed(gtid);
  global_tsid_lock->unlock();

  if (already_executed) {
    sql_print_information(
        "[BRR] GTID %u:%lld already executed, will skip DDL", sidno,
        static_cast<long long>(gno));
    return Brr_gtid_status::ALREADY_EXECUTED;
  }

  out_gtid->set(sidno, gno);
  sql_print_information("[BRR] GTID %u:%lld is valid and not yet executed",
                        sidno, static_cast<long long>(gno));
  return Brr_gtid_status::VALID;
}

// ==========================================================================
//  Dependency waiting
// ==========================================================================

/**
   Wait for the prepare dependency GTID set to be fully executed on this
   replica.  This ensures the replica's state is at least as up-to-date as
   the source was when it sent the PREPARE event.

   @return true on success (dependencies satisfied), false on timeout/error.
*/
static bool wait_brr_prepare_dep(THD *thd, Relay_log_info *rli,
                                 const Brr_ddl_prepare_event &ev) {
  if (ev.prepare_dependency_gtid_set.empty()) {
    // No dependencies to wait for — common in phase 1 (no dependency capture)
    return true;
  }

  sql_print_information("[BRR] Waiting for prepare dependency GTID set: %s",
                        ev.prepare_dependency_gtid_set.c_str());

  /*
    TODO: make timeout configurable via sys_var.
    For now use the relay-log-info default timeout, which is large enough
    for normal replication scenarios.
  */
  double timeout = 0;  // 0 = no timeout (wait indefinitely)

  int ret = const_cast<Relay_log_info *>(rli)->wait_for_gtid_set(
      thd, ev.prepare_dependency_gtid_set.c_str(), timeout, false);

  if (ret != 0) {
    sql_print_warning(
        "[BRR] Prepare dependency wait failed (ret=%d), falling back", ret);
    return false;
  }

  return true;
}

// ==========================================================================
//  Session variable application
// ==========================================================================

/**
   Format: null-separated key=value pairs, e.g.:
   "sql_mode=123\0character_set_client=utf8mb4\0..."

   Each pair is parsed, the variable name is looked up, and the value is
   applied directly to thd->variables.
*/
bool apply_session_vars_from_event(THD *thd, const Brr_ddl_prepare_event &ev) {
  if (ev.session_variables.empty()) return true;

  const std::string &data = ev.session_variables;
  size_t pos = 0;

  while (pos < data.size()) {
    // Find the end of this key=value entry
    size_t end = pos;
    while (end < data.size() && data[end] != '\0') ++end;
    if (end == pos) break;  // empty entry, skip

    std::string entry(data, pos, end - pos);
    pos = end + 1;  // skip null terminator

    size_t eq = entry.find('=');
    if (eq == std::string::npos) continue;

    std::string var_name = entry.substr(0, eq);
    std::string var_value = entry.substr(eq + 1);

    if (var_name == "sql_mode") {
      thd->variables.sql_mode = static_cast<sql_mode_t>(std::stoull(var_value));
    } else if (var_name == "character_set_client") {
      if (auto *cs = get_charset_by_csname(var_value.c_str(), MY_CS_PRIMARY,
                                           MYF(0)))
        thd->variables.character_set_client = cs;
    } else if (var_name == "collation_connection") {
      if (auto *cs = get_charset_by_name(var_value.c_str(), MYF(0)))
        thd->variables.collation_connection = cs;
    } else if (var_name == "collation_server") {
      if (auto *cs = get_charset_by_name(var_value.c_str(), MYF(0)))
        thd->variables.collation_server = cs;
    } else if (var_name == "time_zone") {
      String tmp(var_value.c_str(), var_value.size(), &my_charset_bin);
      thd->variables.time_zone = my_tz_find(thd, &tmp);
    } else if (var_name == "foreign_key_checks") {
      // OPTION_NO_FOREIGN_KEY_CHECKS set = FK disabled (0)
      if (var_value == "0" || var_value == "OFF")
        thd->variables.option_bits |= OPTION_NO_FOREIGN_KEY_CHECKS;
      else
        thd->variables.option_bits &= ~OPTION_NO_FOREIGN_KEY_CHECKS;
    } else if (var_name == "unique_checks") {
      // OPTION_RELAXED_UNIQUE_CHECKS set = unique_checks disabled (0)
      if (var_value == "0" || var_value == "OFF")
        thd->variables.option_bits |= OPTION_RELAXED_UNIQUE_CHECKS;
      else
        thd->variables.option_bits &= ~OPTION_RELAXED_UNIQUE_CHECKS;
    } else if (var_name == "explicit_defaults_for_timestamp") {
      // Applied indirectly — this is usually set via command-line or my.cnf
      // and is read-only at session level. Ignore for now.
    }
  }

  thd->update_charset();
  return true;
}

// ==========================================================================
//  DDL execution thread
// ==========================================================================

extern "C" void *handle_brr_ddl_exec(void *arg) {
  Brr_inflight_ddl *inflight = static_cast<Brr_inflight_ddl *>(arg);
  Brr_ddl_exec_ctx *ctx = &inflight->exec_ctx;
  Master_info *mi = ctx->mi;
  Relay_log_info *rli = mi->rli;

  my_thread_init();

  {
    THD *thd = new THD;
    THD_CHECK_SENTRY(thd);
    thd->thread_stack = reinterpret_cast<char *>(&thd);

#ifdef HAVE_PSI_THREAD_INTERFACE
    {
      struct PSI_thread *psi = PSI_THREAD_CALL(get_thread)();
      thd_set_psi(thd, psi);
    }
#endif
    mysql_thread_set_psi_THD(thd);

    thd->rli_slave = rli;
    thd->system_thread = SYSTEM_THREAD_SLAVE_BRR;
    thd->slave_thread = true;
    thd->server_id = inflight->prepare_ev.common.source_server_id;
    thd->unmasked_server_id = inflight->prepare_ev.common.source_server_id;

    // Apply slave thread options (disables binlog, sets BIG_SELECTS, etc.)
    set_slave_thread_options(thd);

    // Acquire GTID ownership on the DDL THD.  The BRR worker validated
    // the GTID but did not own it — ownership must be on the THD that
    // actually executes and commits the DDL so that the normal commit path
    // adds it to gtid_executed.
    bool ddl_skip = false;
    {
      Gtid_specification spec;
      spec.set(inflight->owned_gtid.sidno, inflight->owned_gtid.gno);
      global_tsid_lock->rdlock();
      if (set_gtid_next(thd, spec)) {
        global_tsid_lock->unlock();
        ctx->ddl_error = 1;
        sql_print_error(
            "[BRR] Channel '%s': DDL thread failed to acquire GTID ownership",
            mi->get_channel());
        ddl_skip = true;
      } else if (thd->owned_gtid.sidno == 0) {
        // GTID was already executed between validation and now — skip DDL.
        global_tsid_lock->unlock();
        sql_print_information(
            "[BRR] Channel '%s': GTID executed since validation, skipping DDL",
            mi->get_channel());
        ddl_skip = true;
      } else {
        global_tsid_lock->unlock();
      }
    }

    // Register with the global THD manager — always paired with remove_thd
    // at the end of the function.
    Global_THD_manager::get_instance()->add_thd(thd);

    if (ddl_skip) {
      // Signal the BRR worker to avoid deadlock (ddl_paused not set).
      mysql_mutex_lock(&ctx->mutex);
      ctx->ddl_paused = true;
      mysql_cond_signal(&ctx->cond_ddl_paused);
      mysql_mutex_unlock(&ctx->mutex);
    } else {
      // Connect the DDL execution context so mysql_inplace_alter_table
      // knows to pause before commit.
      thd->m_brr_ddl_exec_ctx = ctx;

      // Apply session variables from the PREPARE event.
      apply_session_vars_from_event(thd, inflight->prepare_ev);

      // Set the database context.
      if (!inflight->prepare_ev.schema_name.empty()) {
        thd->set_db(LEX_CSTRING{inflight->prepare_ev.schema_name.c_str(),
                                inflight->prepare_ev.schema_name.size()});
      }

      thd->set_query(inflight->prepare_ev.query.c_str(),
                     inflight->prepare_ev.query.size());

      sql_print_information("[BRR] Channel '%s': DDL exec thread starting, ddl_id=%llu, "
                            "query='%s'",
                            mi->get_channel(),
                            static_cast<unsigned long long>(
                                inflight->prepare_ev.common.ddl_id),
                            inflight->prepare_ev.query.c_str());

      Parser_state parser_state;
      ctx->ddl_error = 0;

      if (parser_state.init(thd, thd->query().str, thd->query().length)) {
        ctx->ddl_error = 1;
        sql_print_error("[BRR] Channel '%s': Parser_state init failed for DDL",
                        mi->get_channel());
      } else {
        parser_state.m_input.m_has_digest = true;
        thd->m_digest = &thd->m_digest_state;
        thd->m_statement_psi = MYSQL_START_STATEMENT(
            &thd->m_statement_state, 0, thd->db().str, thd->db().length,
            thd->charset(), nullptr);

        dispatch_sql_command(thd, &parser_state);

        if (thd->is_error()) {
          ctx->ddl_error = thd->get_stmt_da()->mysql_errno();
          sql_print_error(
              "[BRR] Channel '%s': DDL execution failed, error=%d: %s",
              mi->get_channel(), ctx->ddl_error,
              thd->get_stmt_da()->message_text());
        }
      }

      // If the DDL thread didn't reach the pause point (e.g., early error), we
      // still need to signal the BRR worker to avoid deadlock.
      if (!ctx->ddl_paused) {
        ctx->ddl_error = ctx->ddl_error ? ctx->ddl_error : 1;
        mysql_mutex_lock(&ctx->mutex);
        ctx->ddl_paused = true;
        mysql_cond_signal(&ctx->cond_ddl_paused);
        mysql_mutex_unlock(&ctx->mutex);
      }

      // Release GTID ownership if still held (early error, rollback paths).
      // On the successful commit path, ownership was released by the normal
      // transaction commit inside dispatch_sql_command.
      if (!thd->owned_gtid_is_empty()) {
        global_tsid_lock->rdlock();
        gtid_state->update_on_rollback(thd);
        global_tsid_lock->unlock();
      }
    }

    ctx->ddl_done = true;

    thd->m_brr_ddl_exec_ctx = nullptr;
    thd->release_resources();
    THD_CHECK_SENTRY(thd);
    Global_THD_manager::get_instance()->remove_thd(thd);
    mysql_thread_set_psi_THD(nullptr);
    delete thd;
  }

  my_thread_end();
  return nullptr;
}

// ==========================================================================
//  PREPARE processing (stages 1-5)
// ==========================================================================

static void process_brr_prepare(THD *thd, Master_info *mi, Relay_log_info *rli,
                                const Brr_event &ev,
                                Brr_inflight_ddl *inflight) {
  const Brr_ddl_prepare_event &pev = ev.prepare;

  inflight->state = Brr_replica_state::RPL_PREPARE_RECEIVED;
  sql_print_information(
      "[BRR] Channel '%s': [%s] PREPARE event dequeued, ddl_id=%llu, "
      "table=%s.%s",
      mi->get_channel(), brr_replica_state_name(inflight->state),
      static_cast<unsigned long long>(pev.common.ddl_id),
      pev.schema_name.c_str(), pev.table_name.c_str());

  // --- Stage 2: Validate ---
  inflight->state = Brr_replica_state::RPL_VALIDATE;
  if (!validate_brr_prepare(pev)) {
    inflight->fallback_reason = "validation_failed";
    sql_print_warning("[BRR] Channel '%s': [%s] Validation failed, fallback",
                      mi->get_channel(), brr_replica_state_name(inflight->state));
    inflight->state = Brr_replica_state::RPL_FALLBACK;
    return;
  }

  // Save the prepare event for later stages
  inflight->prepare_ev = pev;

  // --- Stage 3: Validate GTID ---
  inflight->state = Brr_replica_state::RPL_GTID_OWNING;
  {
    Gtid gtid;
    Brr_gtid_status status = own_brr_gtid(thd, pev, &gtid);
    switch (status) {
      case Brr_gtid_status::VALID:
        inflight->owned_gtid = gtid;
        // Ownership is NOT held here — the DDL thread acquires on its own THD.
        inflight->gtid_owned = false;
        break;
      case Brr_gtid_status::ALREADY_EXECUTED:
        sql_print_information(
            "[BRR] Channel '%s': [%s] GTID already executed, skipping",
            mi->get_channel(), brr_replica_state_name(inflight->state));
        inflight->state = Brr_replica_state::RPL_COMMITTED;
        return;
      case Brr_gtid_status::ERROR:
        inflight->fallback_reason = "gtid_validate_failed";
        sql_print_warning("[BRR] Channel '%s': [%s] GTID validation failed",
                          mi->get_channel(),
                          brr_replica_state_name(inflight->state));
        inflight->state = Brr_replica_state::RPL_FALLBACK;
        return;
    }
  }

  // --- Stage 4: Wait for prepare dependency ---
  inflight->state = Brr_replica_state::RPL_WAIT_PREPARE_DEP;
  if (!wait_brr_prepare_dep(thd, rli, pev)) {
    inflight->fallback_reason = "prepare_dependency_timeout";
    sql_print_warning("[BRR] Channel '%s': [%s] Prepare dependency wait failed",
                      mi->get_channel(), brr_replica_state_name(inflight->state));
    inflight->state = Brr_replica_state::RPL_FALLBACK;
    return;
  }

  // --- Stage 5: Execute DDL body on a separate thread ---
  inflight->state = Brr_replica_state::RPL_EXECUTING;

  inflight->exec_ctx.init();
  inflight->exec_ctx.mi = mi;

  my_thread_attr_t attr;
  my_thread_attr_init(&attr);
  my_thread_attr_setdetachstate(&attr, MY_THREAD_CREATE_JOINABLE);

  my_thread_handle thd_handle;
  // Note: use PSI key 0 for the temporary DDL thread (no instrumentation).
  // The BRR worker already has its own PSI registration via rpl_replica.cc.
  if (mysql_thread_create(0, &thd_handle, &attr,
                          handle_brr_ddl_exec, inflight)) {
    inflight->exec_ctx.destroy();
    inflight->fallback_reason = "ddl_thread_create_failed";
    sql_print_error(
        "[BRR] Channel '%s': [%s] Failed to create DDL execution thread",
        mi->get_channel(), brr_replica_state_name(inflight->state));
    inflight->state = Brr_replica_state::RPL_FALLBACK;
    my_thread_attr_destroy(&attr);
    return;
  }
  my_thread_attr_destroy(&attr);

  // Save the handle for later join (COMMIT/ROLLBACK handlers)
  inflight->exec_ctx.ddl_thread = thd_handle;

  // Wait for the DDL thread to reach the pause point (after ha_inplace_alter_table).
  // wait_paused() snapshots ddl_error under the mutex to avoid data races.
  int ddl_err = inflight->exec_ctx.wait_paused();

  if (ddl_err != 0) {
    // DDL failed (either before reaching the pause point, or GTID acquire
    // failed on the DDL thread) — fallback.
    sql_print_error(
        "[BRR] Channel '%s': [%s] DDL execution failed (error=%d)",
        mi->get_channel(), brr_replica_state_name(inflight->state),
        ddl_err);
    inflight->fallback_reason = "ddl_body_failed";
    inflight->state = Brr_replica_state::RPL_FALLBACK;
    // Wake the DDL thread to clean up (with rollback decision)
    inflight->exec_ctx.signal_decision(false /* rollback */);
    my_thread_join(&inflight->exec_ctx.ddl_thread, nullptr);
    inflight->exec_ctx.destroy();
    return;
  }

  // --- Stage 6: Wait for source result (COMMIT or ROLLBACK) ---
  inflight->state = Brr_replica_state::RPL_WAIT_SOURCE_RESULT;
  sql_print_information("[BRR] Channel '%s': [%s] DDL body paused, waiting for "
                        "COMMIT/ROLLBACK from source",
                        mi->get_channel(),
                        brr_replica_state_name(inflight->state));

}

// ==========================================================================
//  COMMIT / ROLLBACK processing
// ==========================================================================

static void process_brr_commit(THD *, Master_info *mi, Relay_log_info *rli,
                               const Brr_event &ev,
                               Brr_inflight_ddl *inflight) {
  inflight->state = Brr_replica_state::RPL_COMMIT_RECEIVED;
  sql_print_information(
      "[BRR] Channel '%s': [%s] COMMIT event received, ddl_id=%llu",
      mi->get_channel(), brr_replica_state_name(inflight->state),
      static_cast<unsigned long long>(ev.commit.common.ddl_id));

  // TODO: Wait for commit dependency GTID set before signalling commit

  // Signal the DDL thread to proceed with commit
  inflight->exec_ctx.signal_decision(true /* commit */);

  // Wait for the DDL thread to finish
  my_thread_join(&inflight->exec_ctx.ddl_thread, nullptr);

  // Save error before destroy() (cosmetic: destroy() frees mutex/cond)
  int ddl_error = inflight->exec_ctx.ddl_error;
  inflight->exec_ctx.destroy();

  if (ddl_error != 0) {
    sql_print_error(
        "[BRR] Channel '%s': DDL commit phase failed (error=%d), aborting",
        mi->get_channel(), ddl_error);
    inflight->state = Brr_replica_state::RPL_ABORTED;
    return;
  }

  // Verify the GTID made it into gtid_executed.  The DDL thread acquired
  // ownership via set_gtid_next and the normal commit path should have
  // called update_gtids_impl_own_gtid.  If the GTID is NOT executed here,
  // the SQL worker will not auto-skip and will re-execute the DDL.
  {
    Gtid gtid;
    gtid.set(inflight->owned_gtid.sidno, inflight->owned_gtid.gno);
    global_tsid_lock->rdlock();
    bool is_executed = gtid_state->is_executed(gtid);
    global_tsid_lock->unlock();

    if (!is_executed) {
      sql_print_error(
          "[BRR] Channel '%s': GTID %u:%lld is NOT in gtid_executed after "
          "DDL commit.  The SQL worker will re-execute the DDL if it "
          "proceeds.  Aborting BRR worker.",
          mi->get_channel(), inflight->owned_gtid.sidno,
          static_cast<long long>(inflight->owned_gtid.gno));
      inflight->state = Brr_replica_state::RPL_ABORTED;
      inflight->fallback_reason = "gtid_not_in_executed_after_commit";
      // Signal the main loop to stop — ABORTED is not recoverable.
      rli->m_brr_worker_abort.store(true);
      rli->m_brr_queue.abort();
      return;
    }
  }

  inflight->gtid_owned = false;
  inflight->state = Brr_replica_state::RPL_COMMITTED;
  sql_print_information("[BRR] Channel '%s': DDL committed successfully, "
                        "GTID %u:%lld in gtid_executed",
                        mi->get_channel(), inflight->owned_gtid.sidno,
                        static_cast<long long>(inflight->owned_gtid.gno));
}

static void process_brr_rollback(THD *thd, Master_info *mi, Relay_log_info *,
                                 const Brr_event &ev,
                                 Brr_inflight_ddl *inflight) {
  inflight->state = Brr_replica_state::RPL_ROLLBACK_RECEIVED;
  sql_print_information(
      "[BRR] Channel '%s': [%s] ROLLBACK event received, ddl_id=%llu, "
      "source_error=%u",
      mi->get_channel(), brr_replica_state_name(inflight->state),
      static_cast<unsigned long long>(ev.rollback.common.ddl_id),
      ev.rollback.source_error_code);

  // Signal the DDL thread to rollback
  inflight->exec_ctx.signal_decision(false /* rollback */);

  // Wait for the DDL thread to finish.
  // The DDL thread releases GTID ownership via its own cleanup path
  // (ha_commit_inplace_alter_table(false) + owned_gtid cleanup).
  my_thread_join(&inflight->exec_ctx.ddl_thread, nullptr);
  inflight->exec_ctx.destroy();

  inflight->gtid_owned = false;
  inflight->fallback_reason = "source_rolled_back";
  inflight->state = Brr_replica_state::RPL_FALLBACK;

  sql_print_information("[BRR] Channel '%s': DDL rolled back, falling back to "
                        "original relay-log DDL",
                        mi->get_channel());
}

// ==========================================================================
//  Cleanup
// ==========================================================================

/**
   Clean up any in-flight BRR DDL state, releasing GTID ownership if held.

   Must be called before the BRR worker exits to ensure the SQL worker
   is not blocked on a GTID that will never be released.
*/
static void cleanup_in_flight_brr_ddl(THD *thd, Relay_log_info *rli,
                                      Brr_inflight_ddl *inflight) {
  if (inflight == nullptr) {
    rli->m_brr_queue.clear();
    return;
  }

  if (inflight->is_active()) {
    // Signal the DDL thread to rollback if it's still waiting.
    // The DDL thread handles its own GTID ownership cleanup.
    if (inflight->state == Brr_replica_state::RPL_WAIT_SOURCE_RESULT) {
      inflight->exec_ctx.signal_decision(false /* rollback */);
      my_thread_join(&inflight->exec_ctx.ddl_thread, nullptr);
      inflight->exec_ctx.destroy();
    }
  }

  inflight->state = Brr_replica_state::RPL_FALLBACK;
  inflight->fallback_reason = "worker_shutdown";

  rli->m_brr_queue.clear();
}

// ==========================================================================
//  BRR worker main loop
// ==========================================================================

extern "C" void *handle_slave_brr(void *arg) {
  THD *thd{nullptr};
  bool thd_added{false};
  bool init_failed{false};
  Master_info *mi = static_cast<Master_info *>(arg);
  Relay_log_info *rli = mi->rli;

  // In-flight DDL state — only one at a time in phase 1.
  Brr_inflight_ddl inflight_storage;
  Brr_inflight_ddl *inflight = nullptr;

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
        Main loop: dequeue BRR events and dispatch based on current state.

        State machine:
          - No in-flight DDL: expect PREPARE event
          - In-flight DDL (RPL_WAIT_SOURCE_RESULT): expect COMMIT or ROLLBACK
          - FALLBACK / COMMITTED: reset inflight, expect next PREPARE
      */
      while (!rli->m_brr_worker_abort.load()) {
        // Handle terminal states from the previous iteration.
        if (inflight != nullptr) {
          if (inflight->state == Brr_replica_state::RPL_ABORTED) {
            // Unrecoverable — stop the BRR worker.
            sql_print_error(
                "[BRR] Channel '%s': DDL %llu ABORTED, BRR worker stopping",
                mi->get_channel(),
                static_cast<unsigned long long>(
                    inflight->prepare_ev.common.ddl_id));
            rli->m_brr_worker_abort.store(true);
            rli->m_brr_queue.abort();
            break;
          }
          if (inflight->state == Brr_replica_state::RPL_COMMITTED ||
              inflight->state == Brr_replica_state::RPL_FALLBACK) {
            sql_print_information(
                "[BRR] Channel '%s': DDL %llu completed (%s)%s%s",
                mi->get_channel(),
                static_cast<unsigned long long>(
                    inflight->prepare_ev.common.ddl_id),
                brr_replica_state_name(inflight->state),
                inflight->fallback_reason != nullptr ? ", reason: " : "",
                inflight->fallback_reason != nullptr
                    ? inflight->fallback_reason
                    : "");
            inflight = nullptr;
          }
        }

        Brr_event ev;
        if (!rli->m_brr_queue.dequeue_blocking(&ev)) {
          if (!rli->m_brr_worker_abort.load())
            sql_print_warning(
                "[BRR] Channel '%s': BRR queue aborted, worker exiting",
                mi->get_channel());
          break;
        }

        if (rli->m_brr_worker_abort.load()) break;

        if (inflight == nullptr) {
          // Expect PREPARE event
          if (ev.type != mysql::binlog::event::BRR_DDL_PREPARE_EVENT) {
            sql_print_warning(
                "[BRR] Channel '%s': Unexpected event type %d while idle "
                "(expected PREPARE), ignoring",
                mi->get_channel(), static_cast<int>(ev.type));
            continue;
          }

          inflight_storage = Brr_inflight_ddl{};
          inflight = &inflight_storage;
          inflight->state = Brr_replica_state::RPL_INIT;
          process_brr_prepare(thd, mi, rli, ev, inflight);

          if (inflight->state == Brr_replica_state::RPL_FALLBACK ||
              inflight->state == Brr_replica_state::RPL_COMMITTED) {
            // Validation failed or GTID already executed — reset for next event
            inflight = nullptr;
          }
          // Otherwise inflight remains set, worker is in RPL_WAIT_SOURCE_RESULT
        } else {
          // In RPL_WAIT_SOURCE_RESULT — expect COMMIT or ROLLBACK
          switch (ev.type) {
            case mysql::binlog::event::BRR_DDL_COMMIT_EVENT:
              process_brr_commit(thd, mi, rli, ev, inflight);
              break;
            case mysql::binlog::event::BRR_DDL_ROLLBACK_EVENT:
              process_brr_rollback(thd, mi, rli, ev, inflight);
              break;
            default:
              sql_print_warning(
                  "[BRR] Channel '%s': Unexpected event type %d while waiting "
                  "for source result, ignoring",
                  mi->get_channel(), static_cast<int>(ev.type));
              break;
          }
        }
      }

      /*
        Clean up any in-flight BRR DDL state.  Must happen within the
        THD lifecycle so GTID ownership can be released properly.
      */
      cleanup_in_flight_brr_ddl(thd, rli, inflight);
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
