/* stubs.c — 测试替身：hash32 / 日志 / 时间
 * 只满足 gnb_proxy.c 的链接需求，不模拟完整 gnb。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#include "gnb.h"
#include "gnb_time.h"

/* ── 简易 uuid → gnb_node_t* 注册表 ───────────────────── */
#define STUB_MAX_NODES 32

typedef struct {
    gnb_uuid_t uuid64;
    gnb_node_t node;
    int used;
} stub_node_slot_t;

/* value 块要能容纳一个指针（GNB_HASH32_VALUE_PTR 读 data 起始的 void*） */
typedef struct {
    uint32_t size;
    unsigned char data[sizeof(void *)];
} stub_block_t;

typedef struct {
    stub_block_t key;
    stub_block_t val;
    gnb_kv32_t kv;
} stub_kv_slot_t;

static stub_node_slot_t g_nodes[STUB_MAX_NODES];
static stub_kv_slot_t   g_kvs[STUB_MAX_NODES];

void stub_nodes_reset(void) {
    memset(g_nodes, 0, sizeof(g_nodes));
    memset(g_kvs, 0, sizeof(g_kvs));
}

gnb_node_t *stub_node_add(gnb_uuid_t uuid64) {
    int i;
    for (i = 0; i < STUB_MAX_NODES; i++) {
        if (!g_nodes[i].used) {
            g_nodes[i].used = 1;
            g_nodes[i].uuid64 = uuid64;
            memset(&g_nodes[i].node, 0, sizeof(g_nodes[i].node));
            g_nodes[i].node.uuid64 = uuid64;
            return &g_nodes[i].node;
        }
    }
    return NULL;
}

void stub_node_set_pong(gnb_node_t *node, int on, uint64_t ts_sec) {
    if (NULL == node) {
        return;
    }
    if (on) {
        node->udp_addr_status |= GNB_NODE_STATUS_IPV4_PONG;
    } else {
        node->udp_addr_status &= (unsigned int)~(GNB_NODE_STATUS_IPV4_PONG | GNB_NODE_STATUS_IPV6_PONG);
    }
    node->addr4_update_ts_sec = ts_sec;
}

/* gnb_hash32_get：按键（uint64 uuid）查注册表 */
gnb_kv32_t *gnb_hash32_get(gnb_hash32_map_t *hash32_map, u_char *key, uint32_t key_len) {
    gnb_uuid_t uuid;
    int i;
    (void)hash32_map;
    if (key_len != sizeof(uint64_t) || NULL == key) {
        return NULL;
    }
    memcpy(&uuid, key, sizeof(uuid));
    for (i = 0; i < STUB_MAX_NODES; i++) {
        if (g_nodes[i].used && g_nodes[i].uuid64 == uuid) {
            void *p = &g_nodes[i].node;
            g_kvs[i].key.size = sizeof(uint64_t);
            memcpy(g_kvs[i].key.data, key, sizeof(uint64_t));
            g_kvs[i].val.size = 0;
            memcpy(g_kvs[i].val.data, &p, sizeof(void *));
            g_kvs[i].kv.key = (gnb_block32_t *)&g_kvs[i].key;
            g_kvs[i].kv.value = (gnb_block32_t *)&g_kvs[i].val;
            g_kvs[i].kv.nex = NULL;
            return &g_kvs[i].kv;
        }
    }
    return NULL;
}

gnb_hash32_map_t *gnb_hash32_create(gnb_heap_t *heap, uint32_t bucket_num) {
    (void)heap;
    (void)bucket_num;
    return (gnb_hash32_map_t *)0x1;
}

void gnb_hash32_release(gnb_hash32_map_t *hash32_map) {
    (void)hash32_map;
}

gnb_kv32_t *gnb_hash32_set(gnb_hash32_map_t *m, u_char *k, uint32_t kl, void *v, uint32_t vl) {
    (void)m; (void)k; (void)kl; (void)v; (void)vl;
    return NULL;
}

void gnb_logf(gnb_log_ctx_t *log, uint8_t log_type, uint8_t log_id, uint8_t level, const char *format, ...) {
    (void)log; (void)log_type; (void)log_id; (void)level; (void)format;
}

uint64_t gnb_timestamp_sec(void) {
    return (uint64_t)time(NULL);
}
