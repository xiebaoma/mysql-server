/* Copyright (c) 2025, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef RPL_BRR_EVENT_H
#define RPL_BRR_EVENT_H

#include <cstdint>
#include <string>

#include "mysql/binlog/event/binlog_event.h"

/// Global switch for Binlog Realtime Replication.
/// When false (default), neither source nor replica will use BRR.
/// TODO: replace with a proper sys_var in the observability phase.
extern bool opt_binlog_realtime_replication;

/// BRR event protocol version.
constexpr uint16_t BRR_EVENT_VERSION = 1;

/// Length of a UUID string in the BRR wire format
/// (8-4-4-4-12 hex digits + 4 dashes = 36 chars).
constexpr size_t BRR_UUID_STRING_LENGTH = 36;

/// BRR common prefix wire size (bytes):
///   event_version(2) + ddl_id(8) + source_server_id(4)
///   + source_server_uuid(36) + gtid_gno(8)
constexpr size_t BRR_COMMON_PREFIX_SIZE = 2 + 8 + 4 + BRR_UUID_STRING_LENGTH + 8;

/**
 * Common fields shared by all three BRR event types.
 */
struct Brr_event_common {
  uint16_t event_version{BRR_EVENT_VERSION};
  uint64_t ddl_id{0};
  uint32_t source_server_id{0};
  char source_server_uuid[BRR_UUID_STRING_LENGTH]{0};
  int64_t gtid_gno{0};
};

/**
 * BRR DDL PREPARE event.
 *
 * Sent by the source at the start of a DDL (after MDL X-lock, before the
 * long-running body phase). Carries the full DDL context so the replica
 * BRR worker can pre-execute the same DDL in parallel.
 */
struct Brr_ddl_prepare_event {
  Brr_event_common common;
  std::string schema_name;
  std::string table_name;
  std::string query;
  uint32_t ddl_type{0};
  uint32_t ddl_algorithm{0};
  uint32_t ddl_lock_type{0};
  std::string prepare_dependency_gtid_set;
  std::string session_variables;
};

/**
 * BRR DDL COMMIT event.
 *
 * Sent by the source after the DDL body succeeds, carrying the commit
 * dependency GTID set. The replica BRR worker uses this to wait for
 * dependent transactions, then commits the pre-executed DDL.
 */
struct Brr_ddl_commit_event {
  Brr_event_common common;
  std::string commit_dependency_gtid_set;
  uint8_t source_result{1};  // 1 = success
};

/**
 * BRR DDL ROLLBACK event.
 *
 * Sent by the source when a DDL that already had its PREPARE sent
 * subsequently fails. The replica BRR worker rolls back / cleans up
 * the pre-executed DDL and releases GTID ownership so the original
 * SQL worker can fall back to applying the relay-log DDL.
 */
struct Brr_ddl_rollback_event {
  Brr_event_common common;
  uint32_t source_error_code{0};
  std::string source_error_message;
  uint8_t allow_fallback{1};  // 1 = replica may fallback to original DDL
};

/**
 * Variant container for any BRR event.
 *
 * Only one of the three event-specific members is valid, determined by
 * the `type` field.
 */
struct Brr_event {
  mysql::binlog::event::Log_event_type type{
      mysql::binlog::event::UNKNOWN_EVENT};
  Brr_ddl_prepare_event prepare;
  Brr_ddl_commit_event commit;
  Brr_ddl_rollback_event rollback;
};

// ==========================================================================
//  Encoding  (source side)
// ==========================================================================

/**
 * Encode a BRR DDL PREPARE event body into a wire-format buffer.
 *
 * @param ev        Event to encode.
 * @param buf       Destination buffer.
 * @param buf_size  Size of the destination buffer.
 * @return Number of bytes written to buf, or 0 on error (buffer too small).
 */
size_t encode_brr_prepare_event(const Brr_ddl_prepare_event &ev,
                                unsigned char *buf, size_t buf_size);

/**
 * Encode a BRR DDL COMMIT event body into a wire-format buffer.
 *
 * @param ev        Event to encode.
 * @param buf       Destination buffer.
 * @param buf_size  Size of the destination buffer.
 * @return Number of bytes written to buf, or 0 on error (buffer too small).
 */
size_t encode_brr_commit_event(const Brr_ddl_commit_event &ev,
                               unsigned char *buf, size_t buf_size);

/**
 * Encode a BRR DDL ROLLBACK event body into a wire-format buffer.
 *
 * @param ev        Event to encode.
 * @param buf       Destination buffer.
 * @param buf_size  Size of the destination buffer.
 * @return Number of bytes written to buf, or 0 on error (buffer too small).
 */
size_t encode_brr_rollback_event(const Brr_ddl_rollback_event &ev,
                                 unsigned char *buf, size_t buf_size);

// ==========================================================================
//  Maximum encoding sizes  (for buffer pre-allocation)
// ==========================================================================

size_t max_encode_size_prepare(const Brr_ddl_prepare_event &ev);
size_t max_encode_size_commit(const Brr_ddl_commit_event &ev);
size_t max_encode_size_rollback(const Brr_ddl_rollback_event &ev);

// ==========================================================================
//  Decoding  (replica side)
// ==========================================================================

/**
 * Decode a BRR event body from a wire-format buffer.
 *
 * @param buf       Source buffer (body only, after the 19-byte common header).
 * @param buf_len   Number of bytes available in buf.
 * @param type      The event type (must be one of the three BRR types).
 * @param[out] out  Decoded event.
 * @return true on success, false on format error.
 */
bool decode_brr_event(const unsigned char *buf, size_t buf_len,
                      mysql::binlog::event::Log_event_type type, Brr_event *out);

#endif  // RPL_BRR_EVENT_H
