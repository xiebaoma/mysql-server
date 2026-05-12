/* Copyright (c) 2026, Oracle and/or its affiliates.

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
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef RPL_BRR_SOURCE_H
#define RPL_BRR_SOURCE_H

#include <cstddef>

struct Brr_event;

/**
  Enqueue a BRR event to all currently connected dump threads that negotiated
  BRR capability.

  The source DDL hook calls this after constructing a PREPARE/COMMIT/ROLLBACK
  event. The return value is the number of sender queues that accepted the
  event; zero means there is no negotiated recipient or all queues rejected it.
*/
size_t brr_source_enqueue_event(const Brr_event &event);

/** True if at least one dump thread is registered for BRR delivery. */
bool brr_source_has_registered_sender();

/** Number of dump threads currently registered for BRR delivery. */
size_t brr_source_registered_sender_count();

#endif  // RPL_BRR_SOURCE_H
