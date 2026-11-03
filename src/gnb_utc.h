/* UTC local path policy. No VPN wire changes, transport or authorization issuer.
 * Internal table API; the versioned local execution format is utc-conf-v1. */
#ifndef GNB_UTC_H
#define GNB_UTC_H
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#define GNB_UTC_MAX_PEERS 128
#define GNB_UTC_DIGEST_SIZE 32

typedef enum { GNB_UTC_DIRECT = 0, GNB_UTC_EXPORT = 1 } gnb_utc_mode_t;
typedef enum { GNB_UTC_NATIVE = 0, GNB_UTC_FORWARD = 1, GNB_UTC_BLOCK = 2 } gnb_utc_action_t;
typedef enum {
    GNB_UTC_OK = 0, GNB_UTC_INVALID = -1, GNB_UTC_REWIND = -2,
    GNB_UTC_CONFLICT = -3, GNB_UTC_CAPACITY = -4,
    GNB_UTC_UNSUPPORTED = -5, GNB_UTC_SCOPE = -6
} gnb_utc_result_t;
/* Host-order family (4/6), port and scope_id; address is network-order bytes.
 * IPv4 uses the first four bytes and requires the remaining bytes to be zero. */
typedef struct {
    uint8_t family;
    uint8_t address[16];
    uint16_t port;
    uint32_t scope_id;
} gnb_utc_endpoint_t;
typedef struct {
    uint64_t peer_id, pair_id, pair_gen;
    uint8_t digest[GNB_UTC_DIGEST_SIZE];
    gnb_utc_mode_t mode;
    uint64_t expires_at;       /* trusted wall-clock seconds */
    uint64_t deadline_tick;    /* process-local monotonic seconds */
    gnb_utc_endpoint_t endpoint;
} gnb_utc_policy_t;
typedef struct {
    gnb_utc_policy_t policy;
    int blocked;              /* irreversible until an explicit newer policy */
} gnb_utc_entry_t;
/* One table per node instance/scope. Never share across cores or instances.
 * Owner must stop all users before destroy. Do not copy a live table. */
typedef struct {
    pthread_mutex_t lock;
    uint64_t config_gen;
    uint64_t tx_packets, tx_blocked, tx_errors, rx_dropped; /* current boot diagnostics */
    size_t count;
    size_t snapshot_count;
    uint64_t snapshot_peers[GNB_UTC_MAX_PEERS];
    gnb_utc_entry_t entries[GNB_UTC_MAX_PEERS];
} gnb_utc_table_t;
int gnb_utc_init(gnb_utc_table_t *table);
void gnb_utc_destroy(gnb_utc_table_t *table);
/* Complete snapshot; omission retains managed history but blocks that peer.
 * Input is an already authenticated, validated and durably committed local
 * projection. This function is NOT a parser, persistence layer or lease check.
 * Equal generations are idempotent only for identical policy fields. */
int gnb_utc_apply(gnb_utc_table_t *table, uint64_t config_gen,
                  const gnb_utc_policy_t *policies, size_t count);
/* Caller provides trusted time and a process monotonic clock. Expiry latches.
 * Invalid arguments fail closed. Output endpoint is valid only for FORWARD. */
gnb_utc_action_t gnb_utc_route(gnb_utc_table_t *table, uint64_t peer_id,
                              uint64_t now, uint64_t tick,
                              gnb_utc_endpoint_t *endpoint);
/* Source filtering supplements, never replaces, native GNB authentication. */
int gnb_utc_accept(gnb_utc_table_t *table, uint64_t peer_id,
                   const gnb_utc_endpoint_t *source, uint64_t now, uint64_t tick);
#endif
