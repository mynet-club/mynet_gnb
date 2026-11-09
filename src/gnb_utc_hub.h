#ifndef GNB_UTC_HUB_H
#define GNB_UTC_HUB_H

#include <stddef.h>
#include <stdint.h>

#define GNB_UTC_HUB_TEXT_MAX 131072U
#define GNB_UTC_HUB_TOKEN_MAX 64U
#define GNB_UTC_HUB_MAX_PEERS 128U
#define GNB_UTC_HUB_CAP_EPOCH 0x1U
#define GNB_UTC_HUB_CAP_EXPLICIT_ACTIVATE 0x2U
#define GNB_UTC_HUB_CAP_MASK 0x3U

/* Peer projection modes. 0/1 are the historical direct/A-S values; 2 is A-D,
 * which exports to the local utcd client's per-pair loopback listener. */
#define GNB_UTC_HUB_MODE_DIRECT 0
#define GNB_UTC_HUB_MODE_AS 1
#define GNB_UTC_HUB_MODE_AD 2

typedef struct {
    uint64_t peer_id, pair_id, pair_gen, lease_epoch, placement_epoch, expires_at;
    uint8_t mode, family, address[16];
    uint16_t port;
    uint32_t scope_id;
    char manifest[65];
} gnb_utc_hub_peer_t;

typedef struct {
    char scope[GNB_UTC_HUB_TOKEN_MAX], instance[GNB_UTC_HUB_TOKEN_MAX], boot_id[33];
    uint64_t local_id, config_gen, process_id;
    uint32_t capabilities;
    size_t count;
    gnb_utc_hub_peer_t peers[GNB_UTC_HUB_MAX_PEERS];
} gnb_utc_hub_projection_t;

/* Parse exact canonical utc-v2 text. Content is copied; the caller's buffer is
 * not modified. Rejects v1, duplicate/unsorted peers, invalid epochs/endpoints
 * and any non-canonical spacing or trailing data. */
int gnb_utc_hub_parse(const char *text, size_t length, gnb_utc_hub_projection_t *output,
                      char *error, size_t error_size);
/* Emit canonical utc-v2 text. Peers are sorted by peer_id. */
int gnb_utc_hub_format(const gnb_utc_hub_projection_t *projection, char *output,
                       size_t capacity, size_t *output_length);

#endif
