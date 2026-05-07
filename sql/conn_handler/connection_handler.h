#pragma once

#include "my_inttypes.h"

class THD;
class Channel_info;
class Connection_handler_manager;

/**
  This abstract base class represents how connections are processed,
  most importantly how they map to OS threads.
*/
class Connection_handler {
 protected:
  friend class Connection_handler_manager;

  Connection_handler() = default;
  virtual ~Connection_handler() = default;

  /**
    Add a connection.

    @param channel_info     Pointer to the Channel_info object.

    @note If this function is successful (returns false), the ownership of
    channel_info is transferred. Otherwise the caller is responsible for
    its destruction.

    @return true if processing of the new connection failed, false otherwise.
  */
  virtual bool add_connection(Channel_info *channel_info) = 0;

  /**
    @return Maximum number of threads that can be created
            by this connection handler.
  */
  virtual uint get_max_threads() const = 0;
};
