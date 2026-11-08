#include "gnb_utc_hub_exec.h"

#include <string.h>

void gnb_utc_hub_exec_init(gnb_utc_hub_exec_t *exec) {
    if (!exec) return;
    memset(exec, 0, sizeof(*exec));
}

static int find_policy(const gnb_utc_policy_t *policies, size_t count,
                       uint64_t peer_id) {
    size_t i;
    for (i = 0; i < count; ++i) {
        if (policies[i].peer_id == peer_id) return (int)i;
    }
    return -1;
}

int gnb_utc_hub_exec_apply(gnb_utc_table_t *table, gnb_utc_hub_exec_t *exec,
                           uint64_t config_gen, const gnb_utc_policy_t *updates,
                           const uint8_t *blocked, size_t count) {
    gnb_utc_policy_t merged[GNB_UTC_HUB_EXEC_MAX];
    gnb_utc_policy_t previous[GNB_UTC_HUB_EXEC_MAX];
    size_t merged_count = 0;
    size_t previous_count = 0;
    size_t i;
    int result;

    if (!table || !exec || !config_gen || count > GNB_UTC_HUB_EXEC_MAX ||
        (count && (!updates || !blocked)))
        return GNB_UTC_INVALID;
    result = gnb_utc_snapshot(table, merged, GNB_UTC_HUB_EXEC_MAX,
                              &merged_count);
    if (result != GNB_UTC_OK) return result;
    if (merged_count)
        memcpy(previous, merged, merged_count * sizeof(merged[0]));
    previous_count = merged_count;
    for (i = 0; i < count; ++i) {
        int index;
        if (blocked[i]) {
            index = find_policy(merged, merged_count, updates[i].peer_id);
            if (index >= 0) {
                merged[index] = merged[--merged_count];
            }
            continue;
        }
        index = find_policy(merged, merged_count, updates[i].peer_id);
        if (index < 0) {
            if (merged_count == GNB_UTC_HUB_EXEC_MAX) return GNB_UTC_CAPACITY;
            index = (int)merged_count++;
        }
        merged[index] = updates[i];
    }
    result = gnb_utc_apply(table, config_gen, merged, merged_count);
    if (result != GNB_UTC_OK) return result;
    if (previous_count)
        memcpy(exec->previous, previous, previous_count * sizeof(previous[0]));
    exec->previous_count = previous_count;
    exec->previous_valid = 1;
    for (i = 0; i < count; ++i) {
        if (blocked[i]) {
            result = gnb_utc_block_peer(table, updates[i].peer_id);
            if (result != GNB_UTC_OK && result != GNB_UTC_CONFLICT) return result;
        }
    }
    return GNB_UTC_OK;
}

int gnb_utc_hub_exec_rollback(gnb_utc_table_t *table, gnb_utc_hub_exec_t *exec,
                              uint64_t config_gen) {
    int result;
    if (!table || !exec || !config_gen || !exec->previous_valid)
        return GNB_UTC_INVALID;
    result = gnb_utc_apply(table, config_gen, exec->previous,
                           exec->previous_count);
    if (result != GNB_UTC_OK) return result;
    exec->previous_valid = 0;
    return GNB_UTC_OK;
}
