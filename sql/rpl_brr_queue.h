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

#include <chrono>
#include <condition_variable>
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

  /**
   * Append a decoded BRR event.
   * Returns false when the queue is full, has been aborted, or is
   * disconnected (IO not yet reconnected).
   */
  bool enqueue(Brr_event &&ev) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_aborted || m_disconnected || m_queue.size() >= MAX_QUEUE_SIZE) {
      ++m_rejected_count;
      return false;
    }
    m_queue.push(std::move(ev));
    m_cond.notify_one();
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

  /**
   * Blocking dequeue with optional timeout.  Waits until an event is
   * available, the queue is aborted (STOP REPLICA / shutdown), the IO
   * thread has disconnected, or the timeout expires.
   *
   * @param out             Receives the dequeued event.
   * @param timeout_seconds Maximum wait time in seconds; 0 = no timeout.
   * @return true on success, false if the worker should wake (abort,
   *         disconnect, or timeout).  Caller checks m_brr_worker_abort
   *         and is_disconnected() to distinguish.
   */
  bool dequeue_blocking(Brr_event *out, double timeout_seconds = 0) {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (timeout_seconds > 0) {
      auto deadline = std::chrono::steady_clock::now() +
                      std::chrono::duration<double>(timeout_seconds);
      while (m_queue.empty() && !m_aborted && !m_disconnected) {
        if (m_cond.wait_until(lock, deadline) == std::cv_status::timeout)
          // Break out of the while-loop on timeout.  The deadline has
          // already passed — without the break wait_until would return
          // timeout immediately on every iteration, forming a busy loop.
          break;
      }
    } else {
      while (m_queue.empty() && !m_aborted && !m_disconnected)
        m_cond.wait(lock);
    }
    if (m_queue.empty()) return false;  // aborted, disconnected, or timeout
    *out = std::move(m_queue.front());
    m_queue.pop();
    return true;
  }

  /**
   * Discard all pending events and permanently abort the queue.
   * Called on STOP REPLICA / server shutdown.
   */
  void clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    while (!m_queue.empty()) m_queue.pop();
    m_aborted = true;
    m_cond.notify_all();
  }

  /**
   * Discard all queued events without aborting.
   * Called after IO disconnect to remove stale events from the old connection.
   */
  void drain() {
    std::lock_guard<std::mutex> lock(m_mutex);
    while (!m_queue.empty()) m_queue.pop();
  }

  /** Wake up any thread blocked on dequeue_blocking(). */
  void wakeup() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cond.notify_all();
  }

  /** Signal permanent shutdown — dequeue_blocking() will return false. */
  void abort() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_aborted = true;
    m_cond.notify_all();
  }

  /** Reset aborted flag (for restart). */
  void reset_aborted() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_aborted = false;
  }

  /**
   * Signal IO disconnect — wakes dequeue_blocking() so the BRR worker
   * can clean up in-flight DDL without permanently aborting the queue.
   * After cleanup the worker blocks again until either:
   *   - clear_disconnect() is called (reconnect) and new events arrive, or
   *   - abort() is called (STOP REPLICA).
   */
  void signal_disconnect() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_disconnected = true;
    m_cond.notify_all();
  }

  /**
   * Clear the disconnect signal.  Called by the BRR worker after it
   * finishes cleaning up in-flight DDL, and also by the IO thread after
   * a successful reconnect (idempotent).
   */
  void clear_disconnect() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_disconnected = false;
    m_cond.notify_all();
  }

  bool is_empty() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.empty();
  }

  bool is_disconnected() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_disconnected;
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
  std::condition_variable m_cond;
  std::queue<Brr_event> m_queue;
  uint64_t m_rejected_count{0};
  bool m_aborted{false};
  bool m_disconnected{false};
};

#endif  // RPL_BRR_QUEUE_H
