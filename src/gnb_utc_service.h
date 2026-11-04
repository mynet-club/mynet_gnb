#ifndef GNB_UTC_SERVICE_H
#define GNB_UTC_SERVICE_H
#include "gnb_utc.h"
#define GNB_UTC_TEXT_MAX 131072
#define GNB_UTC_TOKEN_MAX 64
#define GNB_UTC_DIR_MAX 4096
#define GNB_UTC_PATH_MAX 4352
#define GNB_UTC_UR_SERVER 0x100u /* incompatible opaque UR forwarding */
#define GNB_UTC_REQUIRED 0x200u /* launcher knows this instance was managed */
/* The manager is single-writer; only its policy table is shared with workers. */
typedef struct {
    gnb_utc_table_t table;
    char directory[GNB_UTC_DIR_MAX], scope[GNB_UTC_TOKEN_MAX], instance[GNB_UTC_TOKEN_MAX];
    char boot_id[33], config_hash[129], reason[32];
    uint64_t local_id, last_poll, process_id;
    int managed, fault, lock_fd;
    unsigned address_families; /* bit 0 IPv4, bit 1 IPv6 */
    int ur_server;
} gnb_utc_service_t;
/* Returns an owned object, including for a never-managed directory. NULL means
 * startup cannot establish safe managed state; caller must refuse startup. */
gnb_utc_service_t *gnb_utc_service_open(const char *directory, uint64_t local_id, unsigned address_families);
/* Presence only: 0 none, 1 known UTC artifacts, -1 cannot establish absence. */
int gnb_utc_service_has_state(const char *directory);
void gnb_utc_service_close(gnb_utc_service_t *service);
/* Single primary UDP worker only; at most once/second. Returns 0 for success,
 * negative for rejected update/degraded storage, while expiry stays enforced. */
int gnb_utc_service_poll(gnb_utc_service_t *service);
#endif
