/* Copyright (c) 2025, Oracle and/or its affiliates.

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

#ifndef RPL_BRR_QUEUE_H
#define RPL_BRR_QUEUE_H

#include <mutex>
#include <queue>

#include "sql/rpl_brr_event.h"  // Brr_event

/**
 * In-memory queue for BRR events on the replica side.
 *
 * The IO thread enqueues decoded BRR events; the BRR worker dequeues
 * them for pre-execution.  Not persistent — after a crash the queue is
 * empty and the replica falls back to the original relay-log DDL.
 */
class Brr_queue {
 public:
  /// Maximum number of events allowed in the queue.  When the limit is
  /// hit the IO thread stops enqueueing and falls back.
  static constexpr size_t MAX_QUEUE_SIZE = 64;

  Brr_queue() = default;

  /** Append a decoded BRR event.  Returns false if the queue is full. */
  bool enqueue(Brr_event &&ev) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_queue.size() >= MAX_QUEUE_SIZE) {
      ++m_rejected_count;
      return false;
    }
    m_queue.push(std::move(ev));
    return true;
  }

  /** Remove and return the oldest event.  Returns false when empty. */
  bool dequeue(Brr_event *out) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_queue.empty()) return false;
    *out = std::move(m_queue.front());
    m_queue.pop();
    return true;
  }

  /** Discard all pending events. */
  void clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    while (!m_queue.empty()) m_queue.pop();
  }

  bool is_empty() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.empty();
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.size();
  }

  /// Number of events rejected due to queue-full.
  uint64_t rejected_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_rejected_count;
  }

 private:
  mutable std::mutex m_mutex;
  std::queue<Brr_event> m_queue;
  uint64_t m_rejected_count{0};
};

#endif  // RPL_BRR_QUEUE_H
