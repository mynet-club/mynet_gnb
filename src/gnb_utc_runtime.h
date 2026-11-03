#ifndef GNB_UTC_RUNTIME_H
#define GNB_UTC_RUNTIME_H
#include "gnb.h"
#include "gnb_utc.h"
/* Return 0 only for native routing; 1 means handled, including block/send error.
 * The table must be attached before workers start and live until workers stop.
 * Runtime attachment is owned by the versioned local configuration service. */
int gnb_utc_send(gnb_core_t *core, uint64_t peer, const void *bytes, size_t size);
gnb_utc_action_t gnb_utc_action(gnb_core_t *core, uint64_t peer);
int gnb_utc_frame_valid(const void *bytes, size_t size);
int gnb_utc_receive(gnb_core_t *core, uint64_t peer, const gnb_sockaddress_t *source);
#endif
