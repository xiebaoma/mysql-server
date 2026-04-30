#pragma once

#include "sql/conn_handler/channel_info.h"
#include "sql/conn_handler/connection_handler_manager.h"

/**
  This class presents a generic interface to initialize and run
  a connection event loop for different types of listeners and
  a callback functor to call on the connection event from the
  listener that listens for connection. Listener type should
  be a class providing methods setup_listener, listen_for_
  connection_event and close_listener. The Connection event
  callback functor object would on receiving connection event
  from the client to process the connection.
*/
template <typename Listener>
class Connection_acceptor {
  Listener *m_listener;

 public:
  Connection_acceptor(Listener *listener) : m_listener(listener) {}

  ~Connection_acceptor() { delete m_listener; }

  /**
    Initialize a connection acceptor.
  */
  bool init_connection_acceptor() { return m_listener->setup_listener(); }

  /**
    Connection acceptor loop to accept connections from clients.
  */
  void connection_event_loop() {
    Connection_handler_manager *mgr =
        Connection_handler_manager::get_instance();
    while (!connection_events_loop_aborted()) {
      Channel_info *channel_info = m_listener->listen_for_connection_event();
      if (channel_info != nullptr) mgr->process_new_connection(channel_info);
    }
  }

  /**
     Spawn admin connection handler to accept admin connections from clients if
     create-admin-listener-thread is specified by user on commandline.
  */
  bool check_and_spawn_admin_connection_handler_thread() const {
    return m_listener->check_and_spawn_admin_connection_handler_thread();
  }

  /**
    Close the listener.
  */
  void close_listener() { m_listener->close_listener(); }
};
