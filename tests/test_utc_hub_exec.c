#include "gnb_utc.h"
#include "gnb_utc_hub_exec.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static gnb_utc_policy_t export_policy(uint64_t peer_id, uint64_t pair_id,
                                      uint64_t pair_gen, uint16_t port,
                                      uint8_t host) {
    gnb_utc_policy_t policy;
    memset(&policy, 0, sizeof(policy));
    policy.peer_id = peer_id;
    policy.pair_id = pair_id;
    policy.pair_gen = pair_gen;
    policy.digest[0] = (uint8_t)peer_id;
    policy.mode = GNB_UTC_EXPORT;
    policy.expires_at = 1000;
    policy.deadline_tick = 1000;
    policy.endpoint.family = 4;
    policy.endpoint.address[0] = 203;
    policy.endpoint.address[1] = 0;
    policy.endpoint.address[2] = 113;
    policy.endpoint.address[3] = host;
    policy.endpoint.port = port;
    return policy;
}

static gnb_utc_policy_t direct_policy(uint64_t peer_id, uint64_t pair_id,
                                      uint64_t pair_gen) {
    gnb_utc_policy_t policy;
    memset(&policy, 0, sizeof(policy));
    policy.peer_id = peer_id;
    policy.pair_id = pair_id;
    policy.pair_gen = pair_gen;
    policy.digest[0] = (uint8_t)peer_id;
    policy.mode = GNB_UTC_DIRECT;
    return policy;
}

static void test_per_peer_delta(void) {
    gnb_utc_table_t table;
    gnb_utc_hub_exec_t exec;
    gnb_utc_policy_t initial[3];
    gnb_utc_policy_t update;
    gnb_utc_endpoint_t routed;
    uint8_t blocked = 0;
    assert(gnb_utc_init(&table) == GNB_UTC_OK);
    gnb_utc_hub_exec_init(&exec);
    initial[0] = export_policy(1, 10, 1, 9444, 7);
    initial[1] = export_policy(2, 20, 1, 9444, 8);
    initial[2] = direct_policy(3, 30, 1);
    assert(gnb_utc_apply(&table, 1, initial, 3) == GNB_UTC_OK);
    assert(gnb_utc_route(&table, 1, 10, 10, &routed) == GNB_UTC_FORWARD);
    assert(routed.port == 9444);
    assert(gnb_utc_route(&table, 3, 10, 10, &routed) == GNB_UTC_NATIVE);

    update = export_policy(2, 20, 2, 9555, 9);
    assert(gnb_utc_hub_exec_apply(&table, &exec, 2, &update, &blocked, 1) ==
           GNB_UTC_OK);
    assert(table.config_gen == 2);
    assert(gnb_utc_route(&table, 2, 10, 10, &routed) == GNB_UTC_FORWARD);
    assert(routed.port == 9555 && routed.address[3] == 9);
    assert(gnb_utc_route(&table, 1, 10, 10, &routed) == GNB_UTC_FORWARD);
    assert(routed.port == 9444 && routed.address[3] == 7);
    assert(gnb_utc_route(&table, 3, 10, 10, &routed) == GNB_UTC_NATIVE);

    blocked = 1;
    update = export_policy(2, 20, 2, 9555, 9);
    assert(gnb_utc_hub_exec_apply(&table, &exec, 3, &update, &blocked, 1) ==
           GNB_UTC_OK);
    assert(gnb_utc_route(&table, 2, 10, 10, &routed) == GNB_UTC_BLOCK);
    assert(gnb_utc_route(&table, 1, 10, 10, &routed) == GNB_UTC_FORWARD);
    assert(gnb_utc_route(&table, 3, 10, 10, &routed) == GNB_UTC_NATIVE);

    assert(gnb_utc_hub_exec_rollback(&table, &exec, 4) == GNB_UTC_OK);
    assert(table.config_gen == 4);
    assert(gnb_utc_route(&table, 2, 10, 10, &routed) == GNB_UTC_BLOCK);
    assert(gnb_utc_route(&table, 1, 10, 10, &routed) == GNB_UTC_FORWARD);
    assert(gnb_utc_route(&table, 3, 10, 10, &routed) == GNB_UTC_NATIVE);
    assert(gnb_utc_hub_exec_rollback(&table, &exec, 5) == GNB_UTC_INVALID);

    blocked = 0;
    update = export_policy(2, 20, 3, 9555, 9);
    assert(gnb_utc_hub_exec_apply(&table, &exec, 5, &update, &blocked, 1) ==
           GNB_UTC_OK);
    assert(gnb_utc_route(&table, 2, 10, 10, &routed) == GNB_UTC_FORWARD);
    assert(gnb_utc_route(&table, 1, 10, 10, &routed) == GNB_UTC_FORWARD);
    assert(gnb_utc_route(&table, 3, 10, 10, &routed) == GNB_UTC_NATIVE);
    assert(gnb_utc_hub_exec_apply(&table, &exec, 3, &update, &blocked, 1) ==
           GNB_UTC_REWIND);
    update = export_policy(2, 20, 4, 9666, 9);
    assert(gnb_utc_hub_exec_apply(&table, &exec, 5, &update, &blocked, 1) ==
           GNB_UTC_CONFLICT);
    gnb_utc_destroy(&table);
    puts("PASS per-peer delta isolates other peers, blocks latently and rolls back");
}

static void test_mode_switch(void) {
    gnb_utc_table_t table;
    gnb_utc_hub_exec_t exec;
    gnb_utc_policy_t initial[2];
    gnb_utc_policy_t update;
    gnb_utc_endpoint_t routed;
    uint8_t blocked = 0;
    assert(gnb_utc_init(&table) == GNB_UTC_OK);
    gnb_utc_hub_exec_init(&exec);
    initial[0] = export_policy(1, 10, 1, 9444, 7);
    initial[1] = export_policy(2, 20, 1, 9444, 8);
    assert(gnb_utc_apply(&table, 1, initial, 2) == GNB_UTC_OK);
    update = direct_policy(2, 20, 2);
    assert(gnb_utc_hub_exec_apply(&table, &exec, 2, &update, &blocked, 1) ==
           GNB_UTC_OK);
    assert(gnb_utc_route(&table, 2, 10, 10, &routed) == GNB_UTC_NATIVE);
    assert(gnb_utc_route(&table, 1, 10, 10, &routed) == GNB_UTC_FORWARD);
    assert(routed.port == 9444);
    update = export_policy(2, 20, 3, 9555, 9);
    assert(gnb_utc_hub_exec_apply(&table, &exec, 3, &update, &blocked, 1) ==
           GNB_UTC_OK);
    assert(gnb_utc_route(&table, 2, 10, 10, &routed) == GNB_UTC_FORWARD);
    assert(routed.port == 9555 && routed.address[3] == 9);
    assert(gnb_utc_route(&table, 1, 10, 10, &routed) == GNB_UTC_FORWARD);
    assert(gnb_utc_hub_exec_apply(&table, &exec, 0, &update, &blocked, 1) ==
           GNB_UTC_INVALID);
    assert(gnb_utc_hub_exec_apply(&table, &exec, 4, NULL, NULL, 1) ==
           GNB_UTC_INVALID);
    gnb_utc_destroy(&table);
    puts("PASS per-peer export/direct switch keeps other peers untouched");
}

int main(void) {
    test_per_peer_delta();
    test_mode_switch();
    return 0;
}
