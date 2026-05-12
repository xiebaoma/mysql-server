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

/**
  BRR worker thread entry point.

  Consumes BRR events from the in-memory Brr_queue, pre-executes DDL
  on the replica in parallel with the source, and commits or rolls back
  based on the source's result.

  @param arg  Pointer to Master_info for this channel.
  @return nullptr.
*/
extern "C" void *handle_slave_brr(void *arg);

#endif  // RPL_BRR_WORKER_H
