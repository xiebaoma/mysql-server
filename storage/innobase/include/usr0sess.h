#pragma once

#include "data0data.h"
#include "que0types.h"
#include "rem0rec.h"
#include "srv0srv.h"
#include "trx0types.h"
#include "univ.i"
#include "usr0types.h"
#include "ut0byte.h"

/** Opens a session.*/
sess_t *sess_open(void);
/** Closes a session, freeing the memory occupied by it. */
void sess_close(sess_t *sess);

/* The session handle. This data structure is only used by purge and is
not really necessary. We should get rid of it. */
struct sess_t {
  ulint state; /*!< state of the session */
  trx_t *trx;  /*!< transaction object permanently
               assigned for the session: the
               transaction instance designated by the
               trx id changes, but the memory
               structure is preserved */
};

/* Session states */
constexpr uint32_t SESS_ACTIVE = 1;
/** session contains an error message which has not yet been communicated to theclient */
constexpr uint32_t SESS_ERROR = 2;
