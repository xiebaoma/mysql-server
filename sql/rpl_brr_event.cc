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

#include "sql/rpl_brr_event.h"

#include <cstring>
#include <ctime>

#include "my_byteorder.h"
#include "mysql/binlog/event/binlog_event.h"  // checksum_crc32, BINLOG_CHECKSUM_LEN
#include "sql/log_event.h"                     // server_id, LOG_EVENT_HEADER_LEN

using mysql::binlog::event::BINLOG_CHECKSUM_LEN;
using mysql::binlog::event::checksum_crc32;

using mysql::binlog::event::BRR_DDL_COMMIT_EVENT;
using mysql::binlog::event::BRR_DDL_PREPARE_EVENT;
using mysql::binlog::event::BRR_DDL_ROLLBACK_EVENT;
using mysql::binlog::event::Log_event_type;

bool opt_binlog_realtime_replication = false;

// ==========================================================================
//  Encoding helpers
// ==========================================================================

namespace {

/** Cursor for writing into a pre-allocated buffer with bounds checking. */
class Encode_cursor {
 public:
  Encode_cursor(unsigned char *buf, size_t buf_size)
      : m_start(buf), m_pos(buf), m_end(buf + buf_size) {}

  /// Returns the number of bytes written so far.
  size_t written() const { return m_pos - m_start; }

  bool write_u16(uint16_t v) {
    if (remaining() < 2) return false;
    int2store(m_pos, v);
    m_pos += 2;
    return true;
  }

  bool write_u32(uint32_t v) {
    if (remaining() < 4) return false;
    int4store(m_pos, v);
    m_pos += 4;
    return true;
  }

  bool write_u64(uint64_t v) {
    if (remaining() < 8) return false;
    int8store(m_pos, v);
    m_pos += 8;
    return true;
  }

  bool write_bytes(const void *src, size_t len) {
    if (remaining() < len) return false;
    memcpy(m_pos, src, len);
    m_pos += len;
    return true;
  }

  /// Write a length-prefixed string: 2-byte length + data.
  bool write_str_u16(const std::string &s) {
    if (s.size() > UINT16_MAX) return false;
    return write_u16(static_cast<uint16_t>(s.size())) &&
           write_bytes(s.data(), s.size());
  }

  /// Write a length-prefixed blob: 4-byte length + data.
  bool write_blob_u32(const std::string &s) {
    if (s.size() > UINT32_MAX) return false;
    return write_u32(static_cast<uint32_t>(s.size())) &&
           write_bytes(s.data(), s.size());
  }

  /// Write the fields common to all BRR events.
  bool write_common(const Brr_event_common &c) {
    return write_u16(c.event_version) && write_u64(c.ddl_id) &&
           write_u32(c.source_server_id) &&
           write_bytes(c.source_server_uuid, BRR_UUID_STRING_LENGTH) &&
           write_u64(static_cast<uint64_t>(c.gtid_gno));
  }

 private:
  size_t remaining() const { return m_end - m_pos; }

  unsigned char *const m_start;
  unsigned char *m_pos;
  const unsigned char *m_end;
};

}  // anonymous namespace

// ==========================================================================
//  Maximum encoding size helpers
// ==========================================================================

size_t max_encode_size_prepare(const Brr_ddl_prepare_event &ev) {
  return BRR_COMMON_PREFIX_SIZE +
         2 + ev.schema_name.size() +
         2 + ev.table_name.size() +
         4 + ev.query.size() +
         4 +  // ddl_type
         4 +  // ddl_algorithm
         4 +  // ddl_lock_type
         4 + ev.prepare_dependency_gtid_set.size() +
         2 + ev.session_variables.size();
}

size_t max_encode_size_commit(const Brr_ddl_commit_event &ev) {
  return BRR_COMMON_PREFIX_SIZE +
         4 + ev.commit_dependency_gtid_set.size() +
         1;  // source_result
}

size_t max_encode_size_rollback(const Brr_ddl_rollback_event &ev) {
  return BRR_COMMON_PREFIX_SIZE +
         4 +  // source_error_code
         2 + ev.source_error_message.size() +
         1;  // allow_fallback
}

// ==========================================================================
//  Encoding functions
// ==========================================================================

size_t encode_brr_prepare_event(const Brr_ddl_prepare_event &ev,
                                unsigned char *buf, size_t buf_size) {
  Encode_cursor c(buf, buf_size);
  if (!c.write_common(ev.common)) return 0;
  if (!c.write_str_u16(ev.schema_name)) return 0;
  if (!c.write_str_u16(ev.table_name)) return 0;
  if (!c.write_blob_u32(ev.query)) return 0;
  if (!c.write_u32(ev.ddl_type)) return 0;
  if (!c.write_u32(ev.ddl_algorithm)) return 0;
  if (!c.write_u32(ev.ddl_lock_type)) return 0;
  if (!c.write_blob_u32(ev.prepare_dependency_gtid_set)) return 0;
  if (!c.write_str_u16(ev.session_variables)) return 0;
  return c.written();
}

size_t encode_brr_commit_event(const Brr_ddl_commit_event &ev,
                               unsigned char *buf, size_t buf_size) {
  Encode_cursor c(buf, buf_size);
  if (!c.write_common(ev.common)) return 0;
  if (!c.write_blob_u32(ev.commit_dependency_gtid_set)) return 0;
  // Write source_result as a single byte.
  if (!c.write_bytes(&ev.source_result, 1)) return 0;
  return c.written();
}

size_t encode_brr_rollback_event(const Brr_ddl_rollback_event &ev,
                                 unsigned char *buf, size_t buf_size) {
  Encode_cursor c(buf, buf_size);
  if (!c.write_common(ev.common)) return 0;
  if (!c.write_u32(ev.source_error_code)) return 0;
  if (!c.write_str_u16(ev.source_error_message)) return 0;
  if (!c.write_bytes(&ev.allow_fallback, 1)) return 0;
  return c.written();
}

// ==========================================================================
//  Decoding helpers
// ==========================================================================

namespace {

/** Cursor for reading from a wire-format buffer with bounds checking. */
class Decode_cursor {
 public:
  Decode_cursor(const unsigned char *buf, size_t buf_len)
      : m_pos(buf), m_end(buf + buf_len) {}

  bool read_u16(uint16_t *v) {
    if (remaining() < 2) return false;
    *v = uint2korr(m_pos);
    m_pos += 2;
    return true;
  }

  bool read_u32(uint32_t *v) {
    if (remaining() < 4) return false;
    *v = uint4korr(m_pos);
    m_pos += 4;
    return true;
  }

  bool read_u64(uint64_t *v) {
    if (remaining() < 8) return false;
    *v = uint8korr(m_pos);
    m_pos += 8;
    return true;
  }

  bool read_bytes(void *dst, size_t len) {
    if (remaining() < len) return false;
    memcpy(dst, m_pos, len);
    m_pos += len;
    return true;
  }

  /// Read a length-prefixed string: 2-byte length + data.
  bool read_str_u16(std::string *s) {
    uint16_t len = 0;
    if (!read_u16(&len)) return false;
    if (remaining() < len) return false;
    s->assign(reinterpret_cast<const char *>(m_pos), len);
    m_pos += len;
    return true;
  }

  /// Read a length-prefixed blob: 4-byte length + data.
  bool read_blob_u32(std::string *s) {
    uint32_t len = 0;
    if (!read_u32(&len)) return false;
    if (remaining() < len) return false;
    s->assign(reinterpret_cast<const char *>(m_pos), len);
    m_pos += len;
    return true;
  }

  /// Read the fields common to all BRR events.
  bool read_common(Brr_event_common *c) {
    uint64_t gtid_gno_raw = 0;
    if (!read_u16(&c->event_version) || !read_u64(&c->ddl_id) ||
        !read_u32(&c->source_server_id) ||
        !read_bytes(c->source_server_uuid, BRR_UUID_STRING_LENGTH) ||
        !read_u64(&gtid_gno_raw)) {
      return false;
    }
    c->gtid_gno = static_cast<int64_t>(gtid_gno_raw);
    return true;
  }

  /// True when all bytes have been consumed.
  bool at_end() const { return m_pos == m_end; }

 private:
  size_t remaining() const { return m_end - m_pos; }

  const unsigned char *m_pos;
  const unsigned char *m_end;
};

}  // anonymous namespace

// ==========================================================================
//  Decoding function
// ==========================================================================

bool decode_brr_event(const unsigned char *buf, size_t buf_len,
                      Log_event_type type, Brr_event *out) {
  if (buf == nullptr || out == nullptr) return false;

  out->type = type;
  Decode_cursor c(buf, buf_len);

  switch (type) {
    case BRR_DDL_PREPARE_EVENT: {
      auto &ev = out->prepare;
      if (!c.read_common(&ev.common)) return false;
      if (ev.common.event_version != BRR_EVENT_VERSION) return false;
      if (!c.read_str_u16(&ev.schema_name)) return false;
      if (!c.read_str_u16(&ev.table_name)) return false;
      if (!c.read_blob_u32(&ev.query)) return false;
      if (!c.read_u32(&ev.ddl_type)) return false;
      if (!c.read_u32(&ev.ddl_algorithm)) return false;
      if (!c.read_u32(&ev.ddl_lock_type)) return false;
      if (!c.read_blob_u32(&ev.prepare_dependency_gtid_set)) return false;
      if (!c.read_str_u16(&ev.session_variables)) return false;
      return c.at_end();
    }

    case BRR_DDL_COMMIT_EVENT: {
      auto &ev = out->commit;
      if (!c.read_common(&ev.common)) return false;
      if (ev.common.event_version != BRR_EVENT_VERSION) return false;
      if (!c.read_blob_u32(&ev.commit_dependency_gtid_set)) return false;
      if (!c.read_bytes(&ev.source_result, 1)) return false;
      return c.at_end();
    }

    case BRR_DDL_ROLLBACK_EVENT: {
      auto &ev = out->rollback;
      if (!c.read_common(&ev.common)) return false;
      if (ev.common.event_version != BRR_EVENT_VERSION) return false;
      if (!c.read_u32(&ev.source_error_code)) return false;
      if (!c.read_str_u16(&ev.source_error_message)) return false;
      if (!c.read_bytes(&ev.allow_fallback, 1)) return false;
      return c.at_end();
    }

    default:
      return false;
  }
}

// ==========================================================================
//  Full-event encoding
// ==========================================================================

size_t encode_full_brr_event(const Brr_event &ev, unsigned char *buf,
                             size_t buf_size, bool do_checksum) {
  if (buf_size < LOG_EVENT_HEADER_LEN) return 0;

  /* Encode body after the common header. */
  unsigned char *body_buf = buf + LOG_EVENT_HEADER_LEN;
  size_t body_size_limit = buf_size - LOG_EVENT_HEADER_LEN;
  if (do_checksum && body_size_limit > BINLOG_CHECKSUM_LEN)
    body_size_limit -= BINLOG_CHECKSUM_LEN;
  else if (do_checksum)
    return 0;  // buffer too small for checksum

  size_t body_size = 0;
  switch (ev.type) {
    case BRR_DDL_PREPARE_EVENT:
      body_size = encode_brr_prepare_event(ev.prepare, body_buf, body_size_limit);
      break;
    case BRR_DDL_COMMIT_EVENT:
      body_size = encode_brr_commit_event(ev.commit, body_buf, body_size_limit);
      break;
    case BRR_DDL_ROLLBACK_EVENT:
      body_size = encode_brr_rollback_event(ev.rollback, body_buf, body_size_limit);
      break;
    default:
      return 0;
  }
  if (body_size == 0) return 0;

  size_t event_len = LOG_EVENT_HEADER_LEN + body_size;
  if (do_checksum) event_len += BINLOG_CHECKSUM_LEN;

  /* Write the 19-byte common header. */
  int4store(buf, 0);  // timestamp (0 = unused for BRR events)
  buf[EVENT_TYPE_OFFSET] = static_cast<unsigned char>(ev.type);
  int4store(buf + SERVER_ID_OFFSET, server_id);
  int4store(buf + EVENT_LEN_OFFSET, static_cast<uint32_t>(event_len));
  int4store(buf + LOG_POS_OFFSET, 0);  // log_pos = 0 (non-binlog event)
  int2store(buf + FLAGS_OFFSET, 0);

  /* Append checksum if requested. */
  if (do_checksum) {
    uint32_t crc = checksum_crc32(0L, nullptr, 0);
    crc = checksum_crc32(crc, buf, event_len - BINLOG_CHECKSUM_LEN);
    int4store(buf + event_len - BINLOG_CHECKSUM_LEN, crc);
  }

  return event_len;
}
