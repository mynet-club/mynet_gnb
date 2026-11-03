#include "gnb_utc_runtime.h"
#include <string.h>
#include <time.h>

static void count_tx(gnb_core_t *core, int blocked, int error) {
    pthread_mutex_lock(&core->utc_policy->lock);
    if (blocked) ++core->utc_policy->tx_blocked;
    else if (error) ++core->utc_policy->tx_errors;
    else ++core->utc_policy->tx_packets;
    pthread_mutex_unlock(&core->utc_policy->lock);
}
static int count_rx(gnb_core_t *core, int accepted) {
    if (!accepted) {
        pthread_mutex_lock(&core->utc_policy->lock);
        ++core->utc_policy->rx_dropped;
        pthread_mutex_unlock(&core->utc_policy->lock);
    }
    return accepted;
}
int gnb_utc_send(gnb_core_t *core, uint64_t peer, const void *bytes, size_t size) {
    gnb_utc_endpoint_t endpoint;
    gnb_utc_action_t action;
    uint64_t tick;
    int fd;
    ssize_t sent = -1;
    if (!core || !peer || !bytes || !size) return 1;
    if (!core->utc_policy) return 0;
#ifdef _WIN32
    tick = GetTickCount64() / 1000;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 1;
    tick = (uint64_t)ts.tv_sec;
#endif
    action = gnb_utc_route(core->utc_policy, peer, (uint64_t)time(NULL), tick, &endpoint);
    if (action == GNB_UTC_NATIVE) return 0;
    if (action == GNB_UTC_BLOCK || !core->conf) { count_tx(core, 1, 0); return 1; }
    /* Select by export family, never by a learned peer socket index. A failed
     * send is handled: callers must not fall back to a direct candidate. */
    if (endpoint.family == 4 && (core->conf->udp_socket_type & GNB_ADDR_TYPE_IPV4)
        && core->conf->udp4_socket_num > 0) {
        struct sockaddr_in address;
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET; address.sin_port = htons(endpoint.port);
        memcpy(&address.sin_addr, endpoint.address, 4);
        fd = core->udp_ipv4_sockets[0];
        sent = sendto(fd, bytes, size, 0, (struct sockaddr *)&address, sizeof(address));
    } else if (endpoint.family == 6 && (core->conf->udp_socket_type & GNB_ADDR_TYPE_IPV6)
               && core->conf->udp6_socket_num > 0) {
        struct sockaddr_in6 address;
        memset(&address, 0, sizeof(address));
        address.sin6_family = AF_INET6; address.sin6_port = htons(endpoint.port);
        address.sin6_scope_id = endpoint.scope_id;
        memcpy(&address.sin6_addr, endpoint.address, 16);
        fd = core->udp_ipv6_sockets[0];
        sent = sendto(fd, bytes, size, 0, (struct sockaddr *)&address, sizeof(address));
    }
    count_tx(core, 0, sent != (ssize_t)size);
    return 1;
}

int gnb_utc_receive(gnb_core_t *core, uint64_t peer, const gnb_sockaddress_t *source) {
    gnb_utc_endpoint_t endpoint;
    uint64_t tick;
    if (!core || !source) return 0;
    if (!core->utc_policy) return 1;
#ifdef _WIN32
    tick = GetTickCount64() / 1000;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
    tick = (uint64_t)ts.tv_sec;
#endif
    memset(&endpoint, 0, sizeof(endpoint));
    if (source->addr_type == AF_INET) {
        endpoint.family = 4; endpoint.port = ntohs(source->addr.in.sin_port);
        memcpy(endpoint.address, &source->addr.in.sin_addr, 4);
    } else if (source->addr_type == AF_INET6) {
        endpoint.family = 6; endpoint.port = ntohs(source->addr.in6.sin6_port);
        endpoint.scope_id = source->addr.in6.sin6_scope_id;
        memcpy(endpoint.address, &source->addr.in6.sin6_addr, 16);
    } else return count_rx(core, 0);
    return count_rx(core, gnb_utc_accept(core->utc_policy, peer, &endpoint, (uint64_t)time(NULL), tick));
}

gnb_utc_action_t gnb_utc_action(gnb_core_t *core, uint64_t peer) {
    gnb_utc_endpoint_t endpoint;
    uint64_t tick;
    if (!core) return GNB_UTC_BLOCK;
    if (!core->utc_policy) return GNB_UTC_NATIVE;
#ifdef _WIN32
    tick = GetTickCount64() / 1000;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return GNB_UTC_BLOCK;
    tick = ts.tv_sec;
#endif
    return gnb_utc_route(core->utc_policy, peer, time(NULL), tick, &endpoint);
}

/* Reject truncated control frames before a worker casts their public headers. */
#include "gnb_index_frame_type.h"
#include "gnb_pingpong_frame_type.h"
#include "gnb_uf_node_frame_type.h"
#include "gnb_ur1_frame_type.h"
#include "gnb_route_frame.h"
int gnb_utc_frame_valid(const void *bytes, size_t size) {
    const unsigned char *b = bytes;
    uint16_t encoded; size_t need = GNB_PAYLOAD16_HEAD_SIZE;
    if (!bytes || size < GNB_PAYLOAD16_HEAD_SIZE || size > GNB_MAX_PAYLOAD_SIZE) return 0;
    memcpy(&encoded, b, sizeof(encoded));
    if (ntohs(encoded) != size) return 0;
    if (b[2] == GNB_PAYLOAD_TYPE_NODE) {
        switch (b[3]) {
        case PAYLOAD_SUB_TYPE_PING: case PAYLOAD_SUB_TYPE_LAN_PING: need += sizeof(node_ping_frame_t); break;
        case PAYLOAD_SUB_TYPE_PONG: case PAYLOAD_SUB_TYPE_PONG2: need += sizeof(node_pong_frame_t); break;
        case PAYLOAD_SUB_TYPE_NODE_UNIFIED_NOTIFY: need += sizeof(uf_node_notify_frame_t); break;
        default: return 0;
        }
    } else if (b[2] == GNB_PAYLOAD_TYPE_INDEX) {
        switch (b[3]) {
        case PAYLOAD_SUB_TYPE_POST_ADDR: need += sizeof(post_addr_frame_t); break;
        case PAYLOAD_SUB_TYPE_REQUEST_ADDR: need += sizeof(request_addr_frame_t); break;
        case PAYLOAD_SUB_TYPE_ECHO_ADDR: need += sizeof(echo_addr_frame_t); break;
        case PAYLOAD_SUB_TYPE_PUSH_ADDR: need += sizeof(push_addr_frame_t); break;
        case PAYLOAD_SUB_TYPE_DETECT_ADDR: need += sizeof(detect_addr_frame_t); break;
        default: return 0;
        }
    } else if (b[2] == GNB_PAYLOAD_TYPE_IPFRAME) {
        need += sizeof(gnb_route_frame_head_t) + 20; /* native route's minimum IPv4 header */
        if (b[3] & GNB_PAYLOAD_SUB_TYPE_IPFRAME_RELAY) need += sizeof(gnb_uuid_t);
        if (b[3] & (GNB_PAYLOAD_SUB_TYPE_IPFRAME_UNIFIED | GNB_PAYLOAD_SUB_TYPE_IPFRAME_UNIFIED_MULTI_PATH))
            need += sizeof(gnb_unified_forwarding_frame_foot_t);
    } else if (b[2] == GNB_PAYLOAD_TYPE_UR1) {
        need += sizeof(gnb_ur1_frame_head_t) + 4; /* verifycode reads first/last 2 */
    } else if (b[2] == GNB_PAYLOAD_TYPE_UR0) {
        /* IPv4, port, passcode, followed by one complete GNB datagram. */
        uint16_t inner;
        need += 10 + GNB_PAYLOAD16_HEAD_SIZE;
        if (size < need) return 0;
        memcpy(&inner, b + GNB_PAYLOAD16_HEAD_SIZE + 10, sizeof(inner));
        if (ntohs(inner) != size - GNB_PAYLOAD16_HEAD_SIZE - 10) return 0;
    }
    return size >= need;
}
