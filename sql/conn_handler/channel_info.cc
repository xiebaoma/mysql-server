#include "sql/conn_handler/channel_info.h"

#include <stddef.h>
#include <stdio.h>
#include <new>

#include "my_dbug.h"
#include "my_stacktrace.h"  // my_safe_snprintf
#include "mysql_com.h"
#include "sql/derror.h"  // ER_DEFAULT
#include "sql/protocol_classic.h"
#include "sql/sql_class.h"  // THD
#include "violite.h"

THD *Channel_info::create_thd() {
  DBUG_EXECUTE_IF("simulate_resource_failure", return nullptr;);

  Vio *vio_tmp = create_and_init_vio();
  if (vio_tmp == nullptr) return nullptr;

  THD *thd = new (std::nothrow) THD;
  if (thd == nullptr) {
    vio_delete(vio_tmp);
    return nullptr;
  }

  thd->get_protocol_classic()->init_net(vio_tmp);

  return thd;
}

void Channel_info::send_error_and_close_channel(uint errorcode, int error,
                                                bool senderror) {
  assert(errorcode != 0);
  if (!errorcode) return;

  char error_message_buff[MYSQL_ERRMSG_SIZE];

  if (senderror) {
    NET net_tmp;
    Vio *vio_tmp = create_and_init_vio();

    if (vio_tmp && !my_net_init(&net_tmp, vio_tmp)) {
      if (error)
        snprintf(error_message_buff, sizeof(error_message_buff),
                 ER_DEFAULT_NONCONST(errorcode), error);
      net_send_error(
          &net_tmp, errorcode,
          error ? error_message_buff : ER_DEFAULT_NONCONST(errorcode));
      net_end(&net_tmp);
    }
    if (vio_tmp != nullptr) {
      vio_tmp->inactive = true;  // channel is already closed.
      vio_delete(vio_tmp);
    }
  } else  // fatal error like out of memory.
  {
    if (error)
      my_safe_snprintf(error_message_buff, sizeof(error_message_buff),
                       ER_DEFAULT_NONCONST(errorcode), error);
    my_safe_printf_stderr(
        "[Warning] %s\n",
        error ? error_message_buff : ER_DEFAULT_NONCONST(errorcode));
  }
}
