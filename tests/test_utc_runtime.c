#include "gnb.h"
#include "gnb_utc.h"
#include "gnb_index_frame_type.h"
#include "gnb_pingpong_frame_type.h"
#include "gnb_uf_node_frame_type.h"
#include "gnb_ur1_frame_type.h"
#include "gnb_route_frame.h"
#include <assert.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
static void frame_lengths(void) {
    struct { unsigned type, subtype; size_t body; } cases[] = {
        {GNB_PAYLOAD_TYPE_IPFRAME, 0, sizeof(gnb_route_frame_head_t)+20},
        {GNB_PAYLOAD_TYPE_IPFRAME, GNB_PAYLOAD_SUB_TYPE_IPFRAME_RELAY, sizeof(gnb_route_frame_head_t)+20+sizeof(gnb_uuid_t)},
        {GNB_PAYLOAD_TYPE_IPFRAME, GNB_PAYLOAD_SUB_TYPE_IPFRAME_UNIFIED, sizeof(gnb_route_frame_head_t)+20+sizeof(gnb_unified_forwarding_frame_foot_t)},
        {GNB_PAYLOAD_TYPE_NODE, PAYLOAD_SUB_TYPE_PING, sizeof(node_ping_frame_t)},
        {GNB_PAYLOAD_TYPE_NODE, PAYLOAD_SUB_TYPE_LAN_PING, sizeof(node_ping_frame_t)},
        {GNB_PAYLOAD_TYPE_NODE, PAYLOAD_SUB_TYPE_PONG, sizeof(node_pong_frame_t)},
        {GNB_PAYLOAD_TYPE_NODE, PAYLOAD_SUB_TYPE_PONG2, sizeof(node_pong_frame_t)},
        {GNB_PAYLOAD_TYPE_NODE, PAYLOAD_SUB_TYPE_NODE_UNIFIED_NOTIFY, sizeof(uf_node_notify_frame_t)},
        {GNB_PAYLOAD_TYPE_INDEX, PAYLOAD_SUB_TYPE_POST_ADDR, sizeof(post_addr_frame_t)},
        {GNB_PAYLOAD_TYPE_INDEX, PAYLOAD_SUB_TYPE_REQUEST_ADDR, sizeof(request_addr_frame_t)},
        {GNB_PAYLOAD_TYPE_INDEX, PAYLOAD_SUB_TYPE_ECHO_ADDR, sizeof(echo_addr_frame_t)},
        {GNB_PAYLOAD_TYPE_INDEX, PAYLOAD_SUB_TYPE_PUSH_ADDR, sizeof(push_addr_frame_t)},
        {GNB_PAYLOAD_TYPE_INDEX, PAYLOAD_SUB_TYPE_DETECT_ADDR, sizeof(detect_addr_frame_t)},
        {GNB_PAYLOAD_TYPE_UR1, 0, sizeof(gnb_ur1_frame_head_t) + 4}
    };
    unsigned char b[4096]; size_t i, n; uint16_t encoded;
    assert(!gnb_utc_frame_valid(NULL, 0));
    for (i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        memset(b, 0, sizeof(b)); b[2] = cases[i].type; b[3] = cases[i].subtype;
        for (n = 0; n < cases[i].body + 4; ++n) {
            encoded = htons(n); memcpy(b, &encoded, 2);
            assert(!gnb_utc_frame_valid(b, n));
        }
        encoded = htons(n); memcpy(b, &encoded, 2);
        assert(gnb_utc_frame_valid(b, n));
        assert(!gnb_utc_frame_valid(b, n+1));
    }
}
int main(void) {
    frame_lengths();
    gnb_core_t core;
    gnb_conf_t conf;
    gnb_utc_table_t table;
    gnb_utc_policy_t p;
    struct sockaddr_in address;
    struct timeval timeout = {0, 100000};
    struct timespec tick;
    socklen_t length = sizeof(address);
    char received[16];
    const char packet[] = "opaque";
    int receiver = socket(AF_INET, SOCK_DGRAM, 0);
    assert(receiver >= 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET; address.sin_addr.s_addr = inet_addr("127.0.0.1");
    assert(!bind(receiver, (struct sockaddr *)&address, sizeof(address)));
    assert(!getsockname(receiver, (struct sockaddr *)&address, &length));
    assert(!setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    memset(&core, 0, sizeof(core)); memset(&conf, 0, sizeof(conf));
    core.conf = &conf;
    assert(gnb_utc_send(&core, 7, packet, sizeof(packet)) == 0);
    assert(!gnb_utc_init(&table)); core.utc_policy = &table;
    conf.udp_socket_type = GNB_ADDR_TYPE_IPV4; conf.udp4_socket_num = 1;
    core.udp_ipv4_sockets[0] = socket(AF_INET, SOCK_DGRAM, 0);
    assert(core.udp_ipv4_sockets[0] >= 0);
    memset(&p, 0, sizeof(p)); p.peer_id = 7; p.pair_id = 8; p.pair_gen = 1;
    p.mode = GNB_UTC_EXPORT; p.expires_at = time(NULL) + 100;
    assert(!clock_gettime(CLOCK_MONOTONIC, &tick)); p.deadline_tick = tick.tv_sec + 100;
    p.endpoint.family = 4; p.endpoint.port = ntohs(address.sin_port);
    memcpy(p.endpoint.address, &address.sin_addr, 4);
    assert(!gnb_utc_apply(&table, 1, &p, 1));
    {
        gnb_sockaddress_t source;
        memset(&source, 0, sizeof(source)); source.addr_type = AF_INET;
        source.addr.in = address;
        assert(gnb_utc_action(&core, 7) == GNB_UTC_FORWARD);
        assert(gnb_utc_receive(&core, 7, &source));
        source.addr.in.sin_port = htons(ntohs(address.sin_port) + 1);
        assert(!gnb_utc_receive(&core, 7, &source));
        assert(gnb_utc_receive(&core, 9, &source));
    }
    assert(gnb_utc_send(&core, 7, packet, sizeof(packet)) == 1);
    assert(recv(receiver, received, sizeof(received), 0) == sizeof(packet));
    assert(!memcmp(received, packet, sizeof(packet)));
    assert(gnb_utc_send(&core, 9, packet, sizeof(packet)) == 0);
    /* Unusable export family and send errors are handled, never native. */
    conf.udp_socket_type = GNB_ADDR_TYPE_IPV6;
    assert(gnb_utc_send(&core, 7, packet, sizeof(packet)) == 1);
    assert(recv(receiver, received, sizeof(received), 0) < 0);
    conf.udp_socket_type = GNB_ADDR_TYPE_IPV4;
    close(core.udp_ipv4_sockets[0]); core.udp_ipv4_sockets[0] = -1;
    assert(gnb_utc_send(&core, 7, packet, sizeof(packet)) == 1);
    {
        struct sockaddr_in6 address6; gnb_sockaddress_t source6;
        int receiver6 = socket(AF_INET6, SOCK_DGRAM, 0);
        assert(receiver6 >= 0);
        memset(&address6, 0, sizeof(address6)); address6.sin6_family = AF_INET6;
        assert(inet_pton(AF_INET6, "::1", &address6.sin6_addr) == 1);
        assert(!bind(receiver6, (struct sockaddr *)&address6, sizeof(address6)));
        length = sizeof(address6);
        assert(!getsockname(receiver6, (struct sockaddr *)&address6, &length));
        assert(!setsockopt(receiver6, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
        conf.udp_socket_type = GNB_ADDR_TYPE_IPV6; conf.udp6_socket_num = 1;
        core.udp_ipv6_sockets[0] = socket(AF_INET6, SOCK_DGRAM, 0);
        assert(core.udp_ipv6_sockets[0] >= 0);
        p.pair_gen = 2; p.endpoint.family = 6; p.endpoint.port = ntohs(address6.sin6_port);
        memset(p.endpoint.address, 0, 16); p.endpoint.address[15] = 1;
        assert(!gnb_utc_apply(&table, 2, &p, 1));
        assert(gnb_utc_send(&core, 7, packet, sizeof(packet)) == 1);
        assert(recv(receiver6, received, sizeof(received), 0) == sizeof(packet));
        assert(!memcmp(received, packet, sizeof(packet)));
        memset(&source6, 0, sizeof(source6)); source6.addr_type = AF_INET6; source6.addr.in6 = address6;
        assert(gnb_utc_receive(&core, 7, &source6));
        ++source6.addr.in6.sin6_scope_id; assert(!gnb_utc_receive(&core, 7, &source6));
        close(core.udp_ipv6_sockets[0]); close(receiver6);
    }
    assert(!gnb_utc_apply(&table, 3, NULL, 0));
    assert(gnb_utc_send(&core, 7, packet, sizeof(packet)) == 1);
    assert(table.tx_packets == 2);
    assert(table.tx_errors == 2);
    assert(table.tx_blocked == 1);
    assert(table.rx_dropped == 2);
    close(receiver); gnb_utc_destroy(&table);
    puts("UTC runtime UDP export, native isolation, missing family and send error: PASS");
    return 0;
}
