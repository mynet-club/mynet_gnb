/* test_proxy_conf.c — proxy.conf 解析 / 匹配 / order / 可用性 / 热更新 / flow
 *
 * 编译并链接真实的 src/gnb_proxy.c（非复制逻辑）。
 * 用法：make -C packages/1.6.8-dev/p1-route-proxy/tests
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <arpa/inet.h>

#include "gnb.h"
#include "gnb_proxy.h"

/* stubs.c */
void stub_nodes_reset(void);
gnb_node_t *stub_node_add(gnb_uuid_t uuid64);
void stub_node_set_pong(gnb_node_t *node, int on, uint64_t ts_sec);

static int g_pass, g_fail;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; printf("  ✓ %s\n", msg); } \
    else { g_fail++; printf("  ✗ %s (line %d)\n", msg, __LINE__); } \
} while (0)

static char g_confdir[256];

static void write_conf(const char *body) {
    char path[512];
    snprintf(path, sizeof(path), "%s/proxy.conf", g_confdir);
    FILE *f = fopen(path, "w");
    if (!f) {
        perror("fopen proxy.conf");
        exit(2);
    }
    fputs(body, f);
    fclose(f);
}

static void remove_conf(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/proxy.conf", g_confdir);
    unlink(path);
}

static gnb_core_t g_core;
static gnb_conf_t g_conf;

static void core_setup(void) {
    memset(&g_core, 0, sizeof(g_core));
    memset(&g_conf, 0, sizeof(g_conf));
    g_conf.memory = GNB_MEMORY_SCALE_TINY;
    snprintf(g_conf.conf_dir, sizeof(g_conf.conf_dir), "%s", g_confdir);
    g_core.conf = &g_conf;
    g_core.uuid_node_map = (gnb_hash32_map_t *)0x1;
    g_core.log = NULL;
}

static uint32_t ip4(const char *s) {
    struct in_addr a;
    inet_pton(AF_INET, s, &a);
    return ntohl(a.s_addr);
}

/* ── 解析 ─────────────────────────────────────────────── */
static void test_parse(void) {
    printf("▸ parse\n");
    gnb_proxy_cfg_t cfg;

    write_conf("role client\nproxy on\n[default]\nexit 1001 w=10\n");
    CHECK(gnb_proxy_cfg_load(&g_core, g_confdir) == 0, "合法 client 载入成功");
    CHECK(gnb_proxy_enabled() == 1, "proxy on 生效");
    CHECK(gnb_proxy_ingress_open() == 0, "ingress_open 缺省 off");

    write_conf("proxy on\n[default]\nexit 1001\n");
    CHECK(gnb_proxy_cfg_load(&g_core, g_confdir) != 0, "缺 role 整文件拒载");

    write_conf("role bogus\nproxy on\n");
    CHECK(gnb_proxy_cfg_load(&g_core, g_confdir) != 0, "非法 role 拒载");

    write_conf("role client\nproxy on\n[group us]\ntarget 0.0.0.0/0\nexit 1001\n");
    CHECK(gnb_proxy_cfg_load(&g_core, g_confdir) != 0, "target /0 拒载");

    write_conf("role client\nproxy on\n[group us]\ntarget 8.8.8.0/33\nexit 1001\n");
    CHECK(gnb_proxy_cfg_load(&g_core, g_confdir) != 0, "非法前缀长度拒载");

    write_conf("role server\nproxy on\ningress_open on\n[group us]\ntarget 8.8.8.0/24\nexit 1001\n");
    CHECK(gnb_proxy_cfg_load(&g_core, g_confdir) != 0, "server 带策略组拒载");

    write_conf("role server\nproxy on\ningress_open on\n");
    CHECK(gnb_proxy_cfg_load(&g_core, g_confdir) == 0, "纯 server 载入成功");
    CHECK(gnb_proxy_ingress_open() == 1, "ingress_open on 生效");
    CHECK(gnb_proxy_pick_exit(&g_core, ip4("8.8.8.8")) == NULL, "server 不做 client 选路");

    write_conf("role client\nproxy off\n[default]\nexit 1001\n");
    CHECK(gnb_proxy_cfg_load(&g_core, g_confdir) == 0, "proxy off 载入成功");
    CHECK(gnb_proxy_enabled() == 0, "proxy off 关闭准出");

    /* 键字边界：proxy_foo 不应被当成 proxy */
    write_conf("role client\nproxy_foo on\nproxy on\n[default]\nexit 1001\n");
    CHECK(gnb_proxy_cfg_load(&g_core, g_confdir) == 0, "proxy_foo 不干扰 proxy 键");

    /* 超限：17 个 group */
    {
        char buf[4096];
        int i, n;
        n = snprintf(buf, sizeof(buf), "role client\nproxy on\n[default]\nexit 1001\n");
        for (i = 0; i < 17; i++) {
            n += snprintf(buf + n, sizeof(buf) - (size_t)n, "[group g%d]\ntarget 10.%d.0.0/16\nexit 1002\n", i, i);
        }
        write_conf(buf);
        CHECK(gnb_proxy_cfg_load(&g_core, g_confdir) != 0, "group 超 16 拒载");
    }

    (void)cfg;
}

/* ── 热更新：删文件 / 语法错 ──────────────────────────── */
static void test_hot_reload(void) {
    printf("▸ hot-reload\n");

    write_conf("role client\nproxy on\n[default]\nexit 1001\n");
    CHECK(gnb_proxy_cfg_load(&g_core, g_confdir) == 0, "基线载入");
    CHECK(gnb_proxy_enabled() == 1, "基线 enabled");

    remove_conf();
    CHECK(gnb_proxy_cfg_load(&g_core, g_confdir) == 0, "删文件返回 0");
    CHECK(gnb_proxy_enabled() == 0, "删文件 → disabled（不保留旧配置）");

    write_conf("role client\nproxy on\n[default]\nexit 1001\n");
    CHECK(gnb_proxy_cfg_load(&g_core, g_confdir) == 0, "再载入");
    CHECK(gnb_proxy_enabled() == 1, "再载入 enabled");

    /* 语法错：保留上一版 */
    write_conf("role client\nproxy on\n[target bad\n");
    {
        int rc = gnb_proxy_cfg_load(&g_core, g_confdir);
        CHECK(rc != 0, "语法错返回非 0");
        CHECK(gnb_proxy_enabled() == 1, "语法错保留上一版 enabled");
    }
}

/* ── 匹配 / order / 可用性 ────────────────────────────── */
static void test_select(void) {
    printf("▸ select\n");
    stub_nodes_reset();

    gnb_node_t *n1001 = stub_node_add(1001);
    gnb_node_t *n1002 = stub_node_add(1002);
    gnb_node_t *n1003 = stub_node_add(1003);
    gnb_node_t *n2001 = stub_node_add(2001);

    n1001->tun_addr4.s_addr = htonl(0x0a000001);
    n1002->tun_addr4.s_addr = htonl(0x0a000002);
    n1003->tun_addr4.s_addr = htonl(0x0a000003);
    n2001->tun_addr4.s_addr = htonl(0x0a000011);
    stub_node_set_pong(n1001, 1, (uint64_t)time(NULL));
    stub_node_set_pong(n1002, 1, (uint64_t)time(NULL));
    stub_node_set_pong(n1003, 1, (uint64_t)time(NULL));
    stub_node_set_pong(n2001, 1, (uint64_t)time(NULL));

    /* 最长前缀 + default 兜底 */
    write_conf(
        "role client\nproxy on\n"
        "[default]\nexit 1001\n"
        "[group us]\ntarget 8.8.8.0/24\nexit 1002\n"
        "[group us2]\ntarget 8.8.0.0/16\nexit 1003\n"
    );
    CHECK(gnb_proxy_cfg_load(&g_core, g_confdir) == 0, "三组配置载入");

    {
        gnb_node_t *e = gnb_proxy_pick_exit(&g_core, ip4("8.8.8.8"));
        CHECK(e == n1002, "8.8.8.8 命中 /24 组 → 1002");
    }
    {
        gnb_node_t *e = gnb_proxy_pick_exit(&g_core, ip4("8.8.1.1"));
        CHECK(e == n1003, "8.8.1.1 命中 /16 组 → 1003");
    }
    {
        gnb_node_t *e = gnb_proxy_pick_exit(&g_core, ip4("1.2.3.4"));
        CHECK(e == n1001, "未命中 target → default → 1001");
    }

    /* 组内无出口 → DROP，不落 default */
    write_conf(
        "role client\nproxy on\n"
        "[default]\nexit 1001\n"
        "[group us]\ntarget 8.8.8.0/24\n"
    );
    CHECK(gnb_proxy_cfg_load(&g_core, g_confdir) == 0, "空出口组载入");
    CHECK(gnb_proxy_pick_exit(&g_core, ip4("8.8.8.8")) == NULL, "命中组无出口 → NULL（不落 default）");
    CHECK(gnb_proxy_pick_exit(&g_core, ip4("1.2.3.4")) == n1001, "未命中仍走 default");

    /* order 分层：order=0 是最低层；低层可用时不看高层；低层全挂才降层 */
    write_conf(
        "role client\nproxy on\n"
        "[default]\n"
        "exit 1001 w=10 order=0\n"
        "exit 1002 w=10 order=1\n"
        "exit 2001 w=10 order=2\n"
    );
    CHECK(gnb_proxy_cfg_load(&g_core, g_confdir) == 0, "三层 order 载入");
    {
        gnb_node_t *e = gnb_proxy_pick_exit(&g_core, ip4("1.2.3.4"));
        CHECK(e == n1001, "order=0 可用时只在第 0 层选");
    }
    stub_node_set_pong(n1001, 0, 0);
    {
        gnb_node_t *e = gnb_proxy_pick_exit(&g_core, ip4("1.2.3.4"));
        CHECK(e == n1002, "order=0 全不可用 → 降到 order=1");
    }
    stub_node_set_pong(n1002, 0, 0);
    {
        gnb_node_t *e = gnb_proxy_pick_exit(&g_core, ip4("1.2.3.4"));
        CHECK(e == n2001, "再降到 order=2");
    }
    stub_node_set_pong(n2001, 0, 0);
    CHECK(gnb_proxy_pick_exit(&g_core, ip4("1.2.3.4")) == NULL, "全不可用 → NULL");

    /* 无 tun 地址 / 无 PONG / 过期 PONG 均不可用 */
    stub_node_set_pong(n1001, 1, (uint64_t)time(NULL));
    n1001->tun_addr4.s_addr = 0;
    write_conf("role client\nproxy on\n[default]\nexit 1001\n");
    CHECK(gnb_proxy_cfg_load(&g_core, g_confdir) == 0, "单出口载入");
    CHECK(gnb_proxy_pick_exit(&g_core, ip4("1.2.3.4")) == NULL, "无 tun 地址不可用");

    n1001->tun_addr4.s_addr = htonl(0x0a000001);
    stub_node_set_pong(n1001, 0, 0);
    CHECK(gnb_proxy_pick_exit(&g_core, ip4("1.2.3.4")) == NULL, "无 PONG 不可用");

    stub_node_set_pong(n1001, 1, (uint64_t)time(NULL) - 120);
    CHECK(gnb_proxy_pick_exit(&g_core, ip4("1.2.3.4")) == NULL, "PONG 过期（>60s）不可用");

    /* 未注册 uuid */
    write_conf("role client\nproxy on\n[default]\nexit 9999\n");
    CHECK(gnb_proxy_cfg_load(&g_core, g_confdir) == 0, "未注册出口载入");
    CHECK(gnb_proxy_pick_exit(&g_core, ip4("1.2.3.4")) == NULL, "uuid 不在 map → NULL");
}

/* ── flow 粘性 / 分片 ─────────────────────────────────── */
static void test_flow(void) {
    printf("▸ flow\n");
    stub_nodes_reset();
    gnb_node_t *n1001 = stub_node_add(1001);
    gnb_node_t *n1002 = stub_node_add(1002);
    n1001->tun_addr4.s_addr = htonl(0x0a000001);
    n1002->tun_addr4.s_addr = htonl(0x0a000002);
    stub_node_set_pong(n1001, 1, (uint64_t)time(NULL));
    stub_node_set_pong(n1002, 1, (uint64_t)time(NULL));

    write_conf(
        "role client\nproxy on\n"
        "[default]\nexit 1001 w=50 order=0\nexit 1002 w=50 order=0\n"
    );
    CHECK(gnb_proxy_cfg_load(&g_core, g_confdir) == 0, "双出口载入");

    {
        uint32_t src = ip4("10.0.0.2"), dst = ip4("8.8.8.8");
        gnb_node_t *a = gnb_proxy_pick_flow(&g_core, src, dst, IPPROTO_TCP, 1234, 443, 0, 0);
        CHECK(a != NULL, "首包选出出口");
        {
            gnb_node_t *b = gnb_proxy_pick_flow(&g_core, src, dst, IPPROTO_TCP, 1234, 443, 0, 0);
            CHECK(b == a, "同五元组粘住同一出口");
        }
        {
            gnb_node_t *c = gnb_proxy_pick_flow(&g_core, src, dst, IPPROTO_TCP, 1235, 443, 0, 0);
            CHECK(c != NULL, "不同源端口是新连接");
        }
    }

    /* 分片：按 ip_id 关联，不看端口 */
    {
        uint32_t src = ip4("10.0.0.3"), dst = ip4("1.2.3.4");
        gnb_node_t *f0 = gnb_proxy_pick_flow(&g_core, src, dst, IPPROTO_UDP, 1111, 53, 77, 1);
        CHECK(f0 != NULL, "分片首片选出出口");
        gnb_node_t *f1 = gnb_proxy_pick_flow(&g_core, src, dst, IPPROTO_UDP, 0, 0, 77, 1);
        CHECK(f1 == f0, "同 ip_id 后续分片跟随出口");
        gnb_node_t *f2 = gnb_proxy_pick_flow(&g_core, src, dst, IPPROTO_UDP, 0, 0, 78, 1);
        CHECK(f2 != NULL, "不同 ip_id 是另一条分片流");
    }
}

/* ── client 不自动放宽准入 ────────────────────────────── */
static void test_ingress(void) {
    printf("▸ ingress\n");
    write_conf("role client\nproxy on\n[default]\nexit 1001\n");
    CHECK(gnb_proxy_cfg_load(&g_core, g_confdir) == 0, "client 载入");
    CHECK(gnb_proxy_ingress_open() == 0, "client 的 proxy on 不放宽准入");

    write_conf("role client\nproxy on\ningress_open on\n[default]\nexit 1001\n");
    CHECK(gnb_proxy_cfg_load(&g_core, g_confdir) == 0, "client 显式 ingress_open");
    CHECK(gnb_proxy_ingress_open() == 1, "显式 ingress_open on 生效");
}

int main(void) {
    snprintf(g_confdir, sizeof(g_confdir), "/tmp/gnb_proxy_test_%d", (int)getpid());
    mkdir(g_confdir, 0755);

    core_setup();

    test_parse();
    test_hot_reload();
    test_select();
    test_flow();
    test_ingress();

    {
        char path[512];
        snprintf(path, sizeof(path), "%s/proxy.conf", g_confdir);
        unlink(path);
        rmdir(g_confdir);
    }

    printf("\nproxy.conf 测试：%d 通过 / %d 失败\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
