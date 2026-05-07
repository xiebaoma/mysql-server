#include "usr0sess.h"
#include "trx0trx.h"

/** Opens a session.
 @return own: session object */
sess_t *sess_open(void) {
  sess_t *sess;

  sess = static_cast<sess_t *>(
      ut::zalloc_withkey(UT_NEW_THIS_FILE_PSI_KEY, sizeof(*sess)));

  sess->state = SESS_ACTIVE;

  sess->trx = trx_allocate_for_background();
  sess->trx->sess = sess;

  return (sess);
}

/** Closes a session, freeing the memory occupied by it. */
void sess_close(sess_t *sess) /*!< in, own: session object */
{
  trx_free_for_background(sess->trx);
  ut::free(sess);
}
