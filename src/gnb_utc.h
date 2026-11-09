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
    /* utc-v2 watermarks; utc-v1 projections carry no epochs and use 0. */
    uint64_t lease_epoch, placement_epoch;
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
 * Equal generations are idempotent only for identical policy fields. A content
 * change at the same pair_gen is only a lease renewal: pair_id/mode/endpoint/
 * digest must be identical, and expiry plus both epochs must not decrease.
 * A normal RENEW keeps the epochs and only extends expiry; advancing an epoch
 * is also accepted. Any identity change needs a newer pair_gen. Otherwise the
 * update is rejected without touching the table. */
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
/* Snapshot every managed, non-blocked policy so a delta executor can merge a
 * per-peer update without disturbing other peers. Main thread only. */
int gnb_utc_snapshot(gnb_utc_table_t *table, gnb_utc_policy_t *out,
                     size_t capacity, size_t *count);
/* Mark one managed peer blocked regardless of mode. Only a newer pair_gen
 * policy revives it. */
int gnb_utc_block_peer(gnb_utc_table_t *table, uint64_t peer_id);
/* Integration entry points: callers provide the final peer and opaque bytes.
 * Only NATIVE permits the caller to continue its existing route selection. */
struct _gnb_core_t;
struct _gnb_conf_t;
/* Main thread only, before workers start/after they stop. Disk I/O is delegated
 * to the private service helper; lifecycle decisions stay in this module. */
void gnb_utc_require_managed(void);
int gnb_utc_index_allowed(void);
int gnb_utc_attach(struct _gnb_core_t *core, struct _gnb_conf_t *conf);
void gnb_utc_detach(struct _gnb_core_t *core, int remember_history);
struct _gnb_sockaddress_t;
int gnb_utc_send(struct _gnb_core_t *core, uint64_t peer, const void *bytes, size_t size);
gnb_utc_action_t gnb_utc_action(struct _gnb_core_t *core, uint64_t peer);
int gnb_utc_frame_valid(const void *bytes, size_t size);
int gnb_utc_receive(struct _gnb_core_t *core, uint64_t peer, const struct _gnb_sockaddress_t *source);
#endif
