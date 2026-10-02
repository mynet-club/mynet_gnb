/* gnb_proxy.h — proxy.conf：route.conf 的延续 + 热读取（双缓冲）
 *
 * 设计见 mynet_ctl/docs/knowledge-base/spec/proxy-conf-design.md
 *
 * 语义：
 *   route.conf 未命中 且 proxy on 时，按 group 的 target 匹配，
 *   从该组 exit 选一个**可用**的节点作为下一跳。
 *   等同动态补 route.conf：exit|dst|255.255.255.255。
 *
 * 内存：两槽各约 8KB 静态；连接表/分片缓存另计，按 node.conf memory 分档。
 */
#ifndef GNB_PROXY_CONF_H
#define GNB_PROXY_CONF_H

#include "gnb.h"

#define GNB_PROXY_CFG_MAX        8192
#define GNB_PROXY_MAX_GROUPS     16
#define GNB_PROXY_MAX_TARGETS    32   /* per group */
#define GNB_PROXY_MAX_EXITS      8    /* per group */

/* 连接表容量上限（实际生效条数见 proxy_flow_limit，按 memory 分档） */
#define GNB_PROXY_FLOW_MAX       2048
/* 分片缓存：仅首片有端口，后续片按 (src,dst,proto,ip_id) 关联出口 */
#define GNB_PROXY_FRAG_MAX       256
#define GNB_PROXY_FRAG_TTL_SEC   30

/* 出口 PONG 有效期。node_worker 用 GNB_NODE_UPDATE_INTERVAL_SEC（75s）清位，
 * 这里再按设计默认 60s 收紧一次，避免刚被清位前的窗口误判可用。 */
#define GNB_PROXY_PONG_TTL_SEC   60

typedef struct {
    uint32_t prefix;   /* network order */
    uint32_t mask;
} gnb_proxy_target_t;

typedef struct {
    gnb_uuid_t uuid64;
    uint16_t   weight;
    uint16_t   order;
} gnb_proxy_exit_t;

typedef struct {
    char name[32];
    int  is_default;
    int  n_targets;
    int  n_exits;
    gnb_proxy_target_t targets[GNB_PROXY_MAX_TARGETS];
    gnb_proxy_exit_t   exits[GNB_PROXY_MAX_EXITS];
} gnb_proxy_group_t;

typedef struct {
    uint64_t gen;
    int      enabled;       /* proxy on|off */
    int      ingress_open;  /* ingress_open on|off */
    int      role;          /* GNB_PROXY_ROLE_* */
    int      group_count;
    gnb_proxy_group_t groups[GNB_PROXY_MAX_GROUPS];
} gnb_proxy_cfg_t;

#define GNB_PROXY_ROLE_NONE   0
#define GNB_PROXY_ROLE_CLIENT 1
#define GNB_PROXY_ROLE_SERVER 2
#define GNB_PROXY_ROLE_BOTH   3

/* 读侧：取当前 active 指针（无锁；内容请走 gnb_proxy_enabled / pick_*） */
gnb_proxy_cfg_t *gnb_proxy_cfg_active(void);

/* 加载/热更新：parse 失败不切换 active */
int  gnb_proxy_cfg_load(gnb_core_t *gnb_core, const char *conf_dir);
/* 文件 mtime 变化则热载；可在准出路径定期调（内部 1s 节流） */
void gnb_proxy_cfg_reload_if_changed(gnb_core_t *gnb_core);

/* 准出选路：目的在 route.conf 未命中时调用。
 * 返回可用出口节点或 NULL。锁内只读配置快照，节点/PONG 在锁外查。 */
gnb_node_t *gnb_proxy_pick_exit(gnb_core_t *gnb_core, uint32_t dst_ip_hostorder);
gnb_node_t *gnb_proxy_pick_flow(gnb_core_t *gnb_core, uint32_t src_ip,
                                uint32_t dst_ip, uint8_t proto,
                                uint16_t src_port, uint16_t dst_port,
                                uint16_t ip_id, int is_fragment);

/* proxy on|off（读锁内） */
int gnb_proxy_enabled(void);
/* client/both 才做准出选路；server 只放行，不选 exit */
int gnb_proxy_is_client(void);
/* 准入侧是否允许目的未登记的流量进入 tun。 */
int gnb_proxy_ingress_open(void);

#endif
