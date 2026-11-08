#ifndef GNB_UTC_HUB_EXEC_H
#define GNB_UTC_HUB_EXEC_H

#include "gnb_utc.h"

/* UTC Hub per-peer execution on top of the local GNB policy table.
 *
 * A Hub delta names only the affected peer(s): export, direct or block. The
 * executor merges that delta into the managed snapshot so every other peer
 * keeps its current policy (third-peer isolation), applies the merged snapshot
 * as one atomic generation, and keeps the pre-apply snapshot for one explicit
 * rollback. Omitted or blocked peers stay blocked until a newer pair_gen
 * policy explicitly revives them.
 */

#define GNB_UTC_HUB_EXEC_MAX GNB_UTC_MAX_PEERS

typedef struct {
    gnb_utc_policy_t previous[GNB_UTC_HUB_EXEC_MAX];
    size_t previous_count;
    int previous_valid;
} gnb_utc_hub_exec_t;

void gnb_utc_hub_exec_init(gnb_utc_hub_exec_t *exec);
/* Apply a per-peer delta at a strictly newer config_gen. updates[i] replaces or
 * adds peer updates[i].peer_id; blocked[i] blocks that peer instead. Returns the
 * gnb_utc_result_t of the atomic table apply. */
int gnb_utc_hub_exec_apply(gnb_utc_table_t *table, gnb_utc_hub_exec_t *exec,
                           uint64_t config_gen, const gnb_utc_policy_t *updates,
                           const uint8_t *blocked, size_t count);
/* Re-apply the snapshot taken before the last successful apply. Blocked peers
 * remain blocked; reviving one requires a newer pair_gen policy. */
int gnb_utc_hub_exec_rollback(gnb_utc_table_t *table, gnb_utc_hub_exec_t *exec,
                              uint64_t config_gen);

#endif
