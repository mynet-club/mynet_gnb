/* gnb_proxy.c — proxy.conf 解析 + 双缓冲热读取 + 准出选路
 *
 * 设计：mynet_ctl/docs/knowledge-base/spec/proxy-conf-design.md
 * 风格：对齐 gnb（snake + GNB_* + GNB_LOGn）；跨平台走 proxy_* 锁宏。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <limits.h>
#include <time.h>

#ifdef __UNIX_LIKE_OS__
#include <arpa/inet.h>
#endif

#ifdef _WIN32
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifndef NAME_MAX
#define NAME_MAX 255
#endif

#ifdef _WIN32
#include <windows.h>
typedef CRITICAL_SECTION gnb_proxy_lock_t;
#define proxy_rdlock(x) EnterCriticalSection(x)
#define proxy_wrlock(x) EnterCriticalSection(x)
#define proxy_unlock(x) LeaveCriticalSection(x)
#define proxy_lock_init(x) InitializeCriticalSection(x)
#else
#include <pthread.h>
typedef pthread_rwlock_t gnb_proxy_lock_t;
#define proxy_rdlock(x) pthread_rwlock_rdlock(x)
#define proxy_wrlock(x) pthread_rwlock_wrlock(x)
#define proxy_unlock(x) pthread_rwlock_unlock(x)
#define proxy_lock_init(x) pthread_rwlock_init((x), NULL)
#endif

#include "gnb.h"
#include "gnb_time.h"
#include "gnb_proxy.h"

/* ── 双缓冲配置 ───────────────────────────────────────── */
static gnb_proxy_cfg_t  g_proxy_slot[2];
static gnb_proxy_cfg_t *volatile g_proxy_active = &g_proxy_slot[0];
static int   g_proxy_write_idx = 1;
static time_t g_proxy_mtime;
static off_t  g_proxy_size;
static uint64_t g_proxy_gen;

#ifdef _WIN32
static gnb_proxy_lock_t g_proxy_lock;
static gnb_proxy_lock_t g_proxy_flow_lock;
static int g_proxy_lock_ready;
static void proxy_locks_init(void) {
    if (!g_proxy_lock_ready) {
        proxy_lock_init(&g_proxy_lock);
        proxy_lock_init(&g_proxy_flow_lock);
        g_proxy_lock_ready = 1;
    }
}
#else
static gnb_proxy_lock_t g_proxy_lock = PTHREAD_RWLOCK_INITIALIZER;
static gnb_proxy_lock_t g_proxy_flow_lock = PTHREAD_RWLOCK_INITIALIZER;
static void proxy_locks_init(void) { /* 静态初始化，无需动作 */ }
#endif

/* ── 连接表 / 分片缓存 ─────────────────────────────────── */
typedef struct {
    uint32_t src, dst;
    uint16_t sport, dport;
    uint16_t ip_id;
    uint8_t  proto;
    uint8_t  used;
    time_t   last;
    gnb_uuid_t exit_uuid;
} gnb_proxy_flow_t;

typedef struct {
    uint32_t src, dst;
    uint16_t ip_id;
    uint8_t  proto;
    uint8_t  used;
    time_t   last;
    gnb_uuid_t exit_uuid;
} gnb_proxy_frag_t;

static gnb_proxy_flow_t g_proxy_flows[GNB_PROXY_FLOW_MAX];
static gnb_proxy_frag_t g_proxy_frags[GNB_PROXY_FRAG_MAX];

static int proxy_flow_limit(const gnb_core_t *core) {
    if (!core || !core->conf) return 512;
    if (core->conf->memory == GNB_MEMORY_SCALE_SMALL) return 1024;
    if (core->conf->memory == GNB_MEMORY_SCALE_LARGE || core->conf->memory == GNB_MEMORY_SCALE_HUGE) {
        return GNB_PROXY_FLOW_MAX;
    }
    return 512; /* tiny / 未配置 */
}

/* 线程安全的轻量伪随机（加权选路用；rand() 非线程安全） */
static uint32_t proxy_rand(uint32_t n) {
    static volatile uint32_t s = 1;
    uint32_t x = __sync_add_and_fetch(&s, 0x9e3779b9u);
    x ^= (uint32_t)time(NULL) * 2654435761u;
    x = x * 1664525u + 1013904223u;
    return n ? (x % n) : 0;
}

/* ── 读侧开关 ─────────────────────────────────────────── */
gnb_proxy_cfg_t *gnb_proxy_cfg_active(void) {
    return g_proxy_active;
}

int gnb_proxy_enabled(void) {
    int v;
    proxy_locks_init();
    proxy_rdlock(&g_proxy_lock);
    v = (NULL != g_proxy_active && g_proxy_active->enabled);
    proxy_unlock(&g_proxy_lock);
    return v;
}

int gnb_proxy_is_client(void) {
    int v;
    proxy_locks_init();
    proxy_rdlock(&g_proxy_lock);
    v = (NULL != g_proxy_active && g_proxy_active->enabled &&
         (g_proxy_active->role == GNB_PROXY_ROLE_CLIENT ||
          g_proxy_active->role == GNB_PROXY_ROLE_BOTH));
    proxy_unlock(&g_proxy_lock);
    return v;
}

int gnb_proxy_ingress_open(void) {
    int v;
    proxy_locks_init();
    proxy_rdlock(&g_proxy_lock);
    v = (NULL != g_proxy_active && g_proxy_active->ingress_open);
    proxy_unlock(&g_proxy_lock);
    return v;
}

/* ── 解析 ─────────────────────────────────────────────── */
/* 键字匹配：整词，避免 proxy/role/exit 误命中 proxy_foo / roleplay */
static int key_match(const char *line, const char *key) {
    size_t n = strlen(key);
    if (strncmp(line, key, n) != 0) {
        return 0;
    }
    return (line[n] == '\0' || line[n] == ' ' || line[n] == '\t');
}

static int parse_cidr(const char *s, uint32_t *prefix, uint32_t *mask) {
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%s", s);
    char *slash = strchr(tmp, '/');
    int plen = 32;
    if (slash) {
        *slash = 0;
        plen = atoi(slash + 1);
        if (plen < 0 || plen > 32) {
            return -1;
        }
    }
    struct in_addr a;
    if (inet_pton(AF_INET, tmp, &a) != 1) {
        return -1;
    }
    *prefix = a.s_addr;
    if (plen == 0) {
        *mask = 0;
    } else {
        *mask = htonl(~((uint32_t)0) << (32 - plen));
    }
    *prefix &= *mask;
    return 0;
}

static void trim(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (p != s) {
        memmove(s, p, strlen(p) + 1);
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r' || s[n-1] == '\n')) {
        s[--n] = 0;
    }
}

static int mask_plen(uint32_t mask) {
    int plen = 0;
    while (mask) {
        plen += mask & 1;
        mask >>= 1;
    }
    return plen;
}

static int parse_exit_tokens(const char *p, gnb_proxy_exit_t *out) {
    unsigned long long uuid = 0;
    int w = 10, order = 0;
    char *end = NULL;

    uuid = strtoull(p, &end, 10);
    if (end == p || 0 == uuid) {
        return -1;
    }
    p = end;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    while (*p) {
        if (0 == strncmp(p, "w=", 2)) {
            w = atoi(p + 2);
            while (*p && *p != ' ' && *p != '\t') p++;
        } else if (0 == strncmp(p, "order=", 6)) {
            order = atoi(p + 6);
            while (*p && *p != ' ' && *p != '\t') p++;
        } else if (*p >= '0' && *p <= '9') {
            /* 兼容 `exit <uuid> <weight>` */
            w = atoi(p);
            while (*p && *p != ' ' && *p != '\t') p++;
        } else {
            while (*p && *p != ' ' && *p != '\t') p++;
        }
        while (*p == ' ' || *p == '\t') p++;
    }
    if (w <= 0) {
        w = 10;
    }
    if (order < 0) {
        order = 0;
    }
    out->uuid64 = (gnb_uuid_t)uuid;
    out->weight = (uint16_t)w;
    out->order  = (uint16_t)order;
    return 0;
}

static int parse_proxy_conf_file(const char *path, gnb_proxy_cfg_t *cfg) {
    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled = 0;
    cfg->ingress_open = 0;
    cfg->role = GNB_PROXY_ROLE_NONE;
    cfg->group_count = 0;

    gnb_proxy_group_t *cur = NULL;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *h = strchr(line, '#');
        if (h) {
            *h = 0;
        }
        trim(line);
        if (!line[0]) {
            continue;
        }

        if (key_match(line, "proxy")) {
            char v[16] = {0};
            if (sscanf(line, "proxy %15s", v) == 1) {
                cfg->enabled = (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true"));
            }
            continue;
        }
        if (key_match(line, "ingress_open")) {
            char v[16] = {0};
            if (sscanf(line, "ingress_open %15s", v) != 1) {
                fclose(f);
                return -3;
            }
            if (!strcmp(v, "on")) {
                cfg->ingress_open = 1;
            } else if (!strcmp(v, "off")) {
                cfg->ingress_open = 0;
            } else {
                fclose(f);
                return -3;
            }
            continue;
        }
        if (key_match(line, "role")) {
            char v[16] = {0};
            if (sscanf(line, "role %15s", v) != 1) {
                fclose(f);
                return -3;
            }
            if (!strcmp(v, "client")) {
                cfg->role = GNB_PROXY_ROLE_CLIENT;
            } else if (!strcmp(v, "server")) {
                cfg->role = GNB_PROXY_ROLE_SERVER;
            } else if (!strcmp(v, "both")) {
                cfg->role = GNB_PROXY_ROLE_BOTH;
            } else {
                fclose(f);
                return -3;
            }
            continue;
        }
        if (line[0] == '[') {
            /* 手动找 ]：sscanf 只统计赋值项，缺闭合括号时仍会返回 1 */
            char *rb = strchr(line, ']');
            if (NULL == rb) {
                fclose(f);
                return -3;
            }
            *rb = 0;
            char name[32] = {0};
            snprintf(name, sizeof(name), "%s", line + 1);
            trim(name);
            /* 去掉可选 "group " 前缀 */
            char *nm = name;
            if (!strncmp(nm, "group ", 6)) {
                nm += 6;
            }
            if (cfg->group_count >= GNB_PROXY_MAX_GROUPS) {
                fclose(f);
                return -2;
            }
            cur = &cfg->groups[cfg->group_count++];
            memset(cur, 0, sizeof(*cur));
            snprintf(cur->name, sizeof(cur->name), "%s", nm);
            cur->is_default = !strcmp(nm, "default");
            continue;
        }
        if (!cur) {
            continue;
        }

        if (key_match(line, "target")) {
            char cidr[64] = {0};
            if (sscanf(line, "target %63s", cidr) != 1) {
                continue;
            }
            if (cur->n_targets >= GNB_PROXY_MAX_TARGETS) {
                fclose(f);
                return -2;
            }
            uint32_t pfx, msk;
            if (parse_cidr(cidr, &pfx, &msk) != 0 || msk == 0) {
                fclose(f);
                return -3;
            }
            cur->targets[cur->n_targets].prefix = pfx;
            cur->targets[cur->n_targets].mask = msk;
            cur->n_targets++;
            continue;
        }
        if (key_match(line, "exit")) {
            const char *p = line + 4;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (cur->n_exits >= GNB_PROXY_MAX_EXITS) {
                fclose(f);
                return -2;
            }
            if (parse_exit_tokens(p, &cur->exits[cur->n_exits]) == 0) {
                cur->n_exits++;
            }
            continue;
        }
        /* 兼容旧一行式：`<uuid> <weight>` */
        {
            gnb_proxy_exit_t e;
            if (parse_exit_tokens(line, &e) == 0 && e.uuid64 > 1000) {
                if (cur->n_exits < GNB_PROXY_MAX_EXITS) {
                    cur->exits[cur->n_exits++] = e;
                }
            }
        }
    }
    fclose(f);
    if (cfg->role == GNB_PROXY_ROLE_NONE) {
        return -3;
    }
    /* server 不承载 client 策略组；允许注释/空 default，不允许带出口的策略组 */
    if (cfg->role == GNB_PROXY_ROLE_SERVER && cfg->group_count > 0) {
        int i;
        for (i = 0; i < cfg->group_count; i++) {
            if (!cfg->groups[i].is_default || cfg->groups[i].n_exits > 0) {
                return -3;
            }
        }
    }
    return 0;
}

/* ── 加载 / 热更新 ─────────────────────────────────────── */
int gnb_proxy_cfg_load(gnb_core_t *gnb_core, const char *conf_dir) {
    char path[PATH_MAX + NAME_MAX];
    snprintf(path, sizeof(path), "%s/proxy.conf", conf_dir);

    proxy_locks_init();

    struct stat st;
    if (stat(path, &st) != 0) {
        /* 无文件：发布 disabled（删除后不保留旧配置） */
        proxy_wrlock(&g_proxy_lock);
        gnb_proxy_cfg_t *w = &g_proxy_slot[g_proxy_write_idx];
        memset(w, 0, sizeof(*w));
        w->gen = ++g_proxy_gen;
        w->enabled = 0;
        __sync_synchronize();
        g_proxy_active = w;
        g_proxy_write_idx ^= 1;
        g_proxy_mtime = 0;
        g_proxy_size = 0;
        proxy_unlock(&g_proxy_lock);
        return 0;
    }

    /* 解析到写槽；失败不切换 active（保留上一版） */
    proxy_wrlock(&g_proxy_lock);
    gnb_proxy_cfg_t *w = &g_proxy_slot[g_proxy_write_idx];
    int rc = parse_proxy_conf_file(path, w);
    if (rc != 0) {
        proxy_unlock(&g_proxy_lock);
        GNB_ERROR1(gnb_core->log, GNB_LOG_ID_PF,
            "proxy.conf parse failed (%d), keep previous config\n", rc);
        return rc;
    }
    w->gen = ++g_proxy_gen;
    __sync_synchronize();
    g_proxy_active = w;
    g_proxy_write_idx ^= 1;
    proxy_unlock(&g_proxy_lock);
    g_proxy_mtime = st.st_mtime;
    g_proxy_size = st.st_size;
    GNB_LOG2(gnb_core->log, GNB_LOG_ID_PF,
        "proxy.conf loaded gen=%llu enabled=%d ingress=%d role=%d groups=%d\n",
        (unsigned long long)w->gen, w->enabled, w->ingress_open, w->role, w->group_count);
    return 0;
}

void gnb_proxy_cfg_reload_if_changed(gnb_core_t *gnb_core) {
    static time_t last_check;
    time_t now = time(NULL);
    if (last_check == now) {
        return;
    }
    last_check = now;
    if (!gnb_core || !gnb_core->conf) {
        return;
    }
    char path[PATH_MAX + NAME_MAX];
    snprintf(path, sizeof(path), "%s/proxy.conf", gnb_core->conf->conf_dir);
    struct stat st;
    if (stat(path, &st) != 0) {
        if (g_proxy_mtime != 0) {
            gnb_proxy_cfg_load(gnb_core, gnb_core->conf->conf_dir);
        }
        return;
    }
    if (st.st_mtime == g_proxy_mtime && st.st_size == g_proxy_size) {
        return;
    }
    gnb_proxy_cfg_load(gnb_core, gnb_core->conf->conf_dir);
}

/* ── 出口可用性（锁外查） ─────────────────────────────── */
static gnb_node_t *proxy_node_lookup(gnb_core_t *gnb_core, gnb_uuid_t uuid64) {
    return (gnb_node_t *)GNB_HASH32_UINT64_GET_PTR(gnb_core->uuid_node_map, uuid64);
}

/* 可用 = 在 uuid_node_map + 有 tun 地址 + 近期 PONG。
 * PONG 位由 node_worker 按 GNB_NODE_UPDATE_INTERVAL_SEC 维护；
 * 这里再按 GNB_PROXY_PONG_TTL_SEC 校验时间戳，避免清位前窗口误判。 */
static int proxy_node_available(gnb_core_t *gnb_core, gnb_uuid_t uuid64, gnb_node_t **out_node) {
    gnb_node_t *node = proxy_node_lookup(gnb_core, uuid64);
    if (NULL == node) {
        return 0;
    }
    if (0 == node->tun_addr4.s_addr) {
        /* 有 node 无 tun 地址/路由 → 不可用 */
        return 0;
    }
    if (0 == ((GNB_NODE_STATUS_IPV4_PONG | GNB_NODE_STATUS_IPV6_PONG) & node->udp_addr_status)) {
        return 0;
    }
    uint64_t now = gnb_timestamp_sec();
    if (0 != node->addr4_update_ts_sec && (now - node->addr4_update_ts_sec) > (uint64_t)GNB_PROXY_PONG_TTL_SEC) {
        return 0;
    }
    if (out_node) {
        *out_node = node;
    }
    return 1;
}

/* ── 匹配（锁内快照） ─────────────────────────────────── */
static int match_target(const gnb_proxy_group_t *g, uint32_t dst_hostorder) {
    uint32_t d = htonl(dst_hostorder);
    int best = -1;
    int best_plen = -1;
    int i;
    for (i = 0; i < g->n_targets; i++) {
        uint32_t m = g->targets[i].mask;
        if ((d & m) != g->targets[i].prefix) {
            continue;
        }
        int plen = mask_plen(m);
        if (plen > best_plen) {
            best_plen = plen;
            best = i;
        }
    }
    return best;
}

/* 命中组：组间最长前缀，同前缀取书写序先者；无 target 命中落 default */
static const gnb_proxy_group_t *pick_group(const gnb_proxy_cfg_t *cfg, uint32_t dst_ip_hostorder, int *hit_plen_out) {
    const gnb_proxy_group_t *hit = NULL;
    int hit_plen = -1;
    int i;
    for (i = 0; i < cfg->group_count; i++) {
        const gnb_proxy_group_t *g = &cfg->groups[i];
        if (g->is_default) {
            continue;
        }
        int m = match_target(g, dst_ip_hostorder);
        if (m < 0) {
            continue;
        }
        int plen = mask_plen(g->targets[m].mask);
        if (plen > hit_plen) {
            hit_plen = plen;
            hit = g;
        }
    }
    if (!hit) {
        for (i = 0; i < cfg->group_count; i++) {
            if (cfg->groups[i].is_default) {
                hit = &cfg->groups[i];
                hit_plen = 0;
                break;
            }
        }
    }
    if (hit_plen_out) {
        *hit_plen_out = hit_plen;
    }
    return hit;
}

/* ── 准出选路 ─────────────────────────────────────────── */
gnb_node_t *gnb_proxy_pick_exit(gnb_core_t *gnb_core, uint32_t dst_ip_hostorder) {
    gnb_proxy_exit_t cands[GNB_PROXY_MAX_EXITS];
    int n_cands = 0;
    char group_name[32];
    uint64_t gen = 0;

    group_name[0] = 0;
    proxy_locks_init();

    /* 读锁内：只取配置快照，不查节点、不做 I/O */
    proxy_rdlock(&g_proxy_lock);
    gnb_proxy_cfg_t *cfg = g_proxy_active;
    if (NULL == cfg || 0 == cfg->enabled) {
        proxy_unlock(&g_proxy_lock);
        return NULL;
    }
    if (cfg->role != GNB_PROXY_ROLE_CLIENT && cfg->role != GNB_PROXY_ROLE_BOTH) {
        proxy_unlock(&g_proxy_lock);
        return NULL;
    }
    gen = cfg->gen;
    {
        const gnb_proxy_group_t *hit = pick_group(cfg, dst_ip_hostorder, NULL);
        if (NULL == hit || 0 == hit->n_exits) {
            proxy_unlock(&g_proxy_lock);
            return NULL;
        }
        snprintf(group_name, sizeof(group_name), "%s", hit->name);
        for (n_cands = 0; n_cands < hit->n_exits; n_cands++) {
            cands[n_cands] = hit->exits[n_cands];
        }
    }
    proxy_unlock(&g_proxy_lock);

    /* 锁外：按 order 升序降层；层内 weight 加权；只收可用节点 */
    {
        int orders[GNB_PROXY_MAX_EXITS];
        int n_orders = 0;
        int i, j, k;
        /* 收集唯一 order 并升序排序（0 是合法最低层） */
        for (i = 0; i < n_cands; i++) {
            int dup = 0;
            for (j = 0; j < n_orders; j++) {
                if (orders[j] == (int)cands[i].order) {
                    dup = 1;
                    break;
                }
            }
            if (!dup) {
                orders[n_orders++] = (int)cands[i].order;
            }
        }
        for (i = 0; i < n_orders; i++) {
            for (k = i + 1; k < n_orders; k++) {
                if (orders[k] < orders[i]) {
                    int t = orders[i];
                    orders[i] = orders[k];
                    orders[k] = t;
                }
            }
        }
        for (i = 0; i < n_orders; i++) {
            uint16_t want = (uint16_t)orders[i];
            int cand_idx[GNB_PROXY_MAX_EXITS];
            gnb_node_t *cand_node[GNB_PROXY_MAX_EXITS];
            int nc = 0;
            uint32_t total_w = 0;
            for (j = 0; j < n_cands; j++) {
                gnb_node_t *node = NULL;
                if (cands[j].order != want) {
                    continue;
                }
                if (!proxy_node_available(gnb_core, cands[j].uuid64, &node)) {
                    continue;
                }
                cand_idx[nc] = j;
                cand_node[nc] = node;
                total_w += (cands[j].weight > 0) ? cands[j].weight : 1;
                nc++;
            }
            if (nc > 0) {
                uint32_t r = proxy_rand(total_w ? total_w : 1);
                uint32_t acc = 0;
                int pick = 0;
                for (j = 0; j < nc; j++) {
                    acc += (cands[cand_idx[j]].weight > 0) ? cands[cand_idx[j]].weight : 1;
                    if (r < acc) {
                        pick = j;
                        break;
                    }
                }
                return cand_node[pick];
            }
        }
    }

    {
        /* 失败日志限速：每秒最多 1 条，避免 DROP 风暴刷屏 */
        static time_t last_fail_log;
        time_t now = time(NULL);
        if (last_fail_log != now) {
            char dst_ip_string[32];
            struct in_addr a;
            a.s_addr = htonl(dst_ip_hostorder);
            inet_ntop(AF_INET, &a, dst_ip_string, sizeof(dst_ip_string));
            GNB_LOG3(gnb_core->log, GNB_LOG_ID_PF,
                "proxy pick exit fail gen=%llu group=%s dst=%s cands=%d (no available exit)\n",
                (unsigned long long)gen, group_name, dst_ip_string, n_cands);
            last_fail_log = now;
        }
    }
    return NULL;
}

/* ── 连接表 / 分片 ─────────────────────────────────────── */
gnb_node_t *gnb_proxy_pick_flow(gnb_core_t *gnb_core, uint32_t src_ip,
                                uint32_t dst_ip, uint8_t proto,
                                uint16_t src_port, uint16_t dst_port,
                                uint16_t ip_id, int is_fragment) {
    time_t now = time(NULL);
    gnb_node_t *node = NULL;

    proxy_locks_init();
    proxy_wrlock(&g_proxy_flow_lock);

    if (is_fragment) {
        /* 分片：按 (src,dst,proto,ip_id) 关联出口，30s 过期 */
        int free_i = -1, old_i = 0;
        int i;
        for (i = 0; i < GNB_PROXY_FRAG_MAX; i++) {
            gnb_proxy_frag_t *f = &g_proxy_frags[i];
            if (!f->used) {
                if (free_i < 0) {
                    free_i = i;
                }
                continue;
            }
            if ((unsigned)(now - f->last) > GNB_PROXY_FRAG_TTL_SEC) {
                f->used = 0;
                if (free_i < 0) {
                    free_i = i;
                }
                continue;
            }
            if (f->src == src_ip && f->dst == dst_ip && f->proto == proto && f->ip_id == ip_id) {
                f->last = now;
                node = proxy_node_lookup(gnb_core, f->exit_uuid);
                proxy_unlock(&g_proxy_flow_lock);
                return node;
            }
            if (f->last < g_proxy_frags[old_i].last) {
                old_i = i;
            }
        }
        node = gnb_proxy_pick_exit(gnb_core, ntohl(dst_ip));
        if (node) {
            int idx = (free_i >= 0) ? free_i : old_i;
            g_proxy_frags[idx].src = src_ip;
            g_proxy_frags[idx].dst = dst_ip;
            g_proxy_frags[idx].ip_id = ip_id;
            g_proxy_frags[idx].proto = proto;
            g_proxy_frags[idx].used = 1;
            g_proxy_frags[idx].last = now;
            g_proxy_frags[idx].exit_uuid = node->uuid64;
        }
        proxy_unlock(&g_proxy_flow_lock);
        return node;
    }

    {
        int free_i = -1, old_i = 0;
        int i;
        int flow_limit = proxy_flow_limit(gnb_core);
        for (i = 0; i < flow_limit; i++) {
            gnb_proxy_flow_t *f = &g_proxy_flows[i];
            if (!f->used) {
                if (free_i < 0) {
                    free_i = i;
                }
                continue;
            }
            unsigned ttl = (proto == IPPROTO_TCP) ? 300 : ((proto == IPPROTO_ICMP) ? 60 : 180);
            if ((unsigned)(now - f->last) > ttl) {
                f->used = 0;
                if (free_i < 0) {
                    free_i = i;
                }
                continue;
            }
            if (f->src == src_ip && f->dst == dst_ip && f->proto == proto &&
                f->sport == src_port && f->dport == dst_port) {
                f->last = now;
                node = proxy_node_lookup(gnb_core, f->exit_uuid);
                proxy_unlock(&g_proxy_flow_lock);
                return node;
            }
            if (f->last < g_proxy_flows[old_i].last) {
                old_i = i;
            }
        }
        node = gnb_proxy_pick_exit(gnb_core, ntohl(dst_ip));
        if (node) {
            int idx = (free_i >= 0) ? free_i : old_i;
            g_proxy_flows[idx].src = src_ip;
            g_proxy_flows[idx].dst = dst_ip;
            g_proxy_flows[idx].sport = src_port;
            g_proxy_flows[idx].dport = dst_port;
            g_proxy_flows[idx].ip_id = ip_id;
            g_proxy_flows[idx].proto = proto;
            g_proxy_flows[idx].used = 1;
            g_proxy_flows[idx].last = now;
            g_proxy_flows[idx].exit_uuid = node->uuid64;
        }
        proxy_unlock(&g_proxy_flow_lock);
        return node;
    }
}
