#pragma once

#include <sys/types.h>

#include "my_inttypes.h"
#include "my_systime.h"  // my_micro_time
#include "violite.h"

class THD;

typedef Vio Vio;

/**
  This abstract base class represents connection channel information
  about a new connection. Its subclasses encapsulate differences
  between different connection channel types.

  Currently we support local and TCP/IP sockets (all platforms),
  named pipes and shared memory (Windows only).
*/
class Channel_info {
  ulonglong prior_thr_create_utime;

 protected:
  /**
    Create and initialize a Vio object.
  */
  virtual Vio *create_and_init_vio() const = 0;

  Channel_info() : prior_thr_create_utime(0) {}

 public:
  virtual ~Channel_info() = default;

  /**
    Instantiate and initialize THD object and vio.
  */
  virtual THD *create_thd();

  /**
    Send error back to the client and close the channel.
  */
  virtual void send_error_and_close_channel(uint errorcode, int error,
                                            bool senderror);

  ulonglong get_prior_thr_create_utime() const {
    return prior_thr_create_utime;
  }

  void set_prior_thr_create_utime() {
    prior_thr_create_utime = my_micro_time();
  }

  virtual bool is_admin_connection() const { return false; }
};
