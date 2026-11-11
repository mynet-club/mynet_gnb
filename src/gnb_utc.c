#include "gnb_utc.h"
#include <string.h>

static int endpoint_equal(const gnb_utc_endpoint_t *a, const gnb_utc_endpoint_t *b) {
    return a->family == b->family && a->port == b->port &&
        a->scope_id == b->scope_id && !memcmp(a->address, b->address, 16);
}
static int policy_equal(const gnb_utc_policy_t *a, const gnb_utc_policy_t *b) {
    return a->peer_id == b->peer_id && a->pair_id == b->pair_id &&
        a->pair_gen == b->pair_gen && a->lease_epoch == b->lease_epoch &&
        a->placement_epoch == b->placement_epoch && a->mode == b->mode &&
        a->expires_at == b->expires_at &&
        !memcmp(a->digest, b->digest, GNB_UTC_DIGEST_SIZE) &&
        endpoint_equal(&a->endpoint, &b->endpoint);
}
/* Same-generation content changes are lease renewals: the authorization
 * watermarks and the expiry may only move forward. A normal RENEW keeps both
 * epochs and only extends expiry; advancing either epoch is also accepted. */
#define GNB_UTC_LEASE_RENEWABLE(c, p) \
    ((p)->expires_at >= (c)->expires_at && \
     (p)->lease_epoch >= (c)->lease_epoch && \
     (p)->placement_epoch >= (c)->placement_epoch)
/* A renewal may only move expiry and epochs. The authorization identity of the
 * pair stays byte-identical; an endpoint, digest or mode change needs a newer
 * pair_gen (ADR-0005). */
static int policy_identity_equal(const gnb_utc_policy_t *a, const gnb_utc_policy_t *b) {
    return a->peer_id == b->peer_id && a->pair_id == b->pair_id &&
        a->mode == b->mode &&
        !memcmp(a->digest, b->digest, GNB_UTC_DIGEST_SIZE) &&
        endpoint_equal(&a->endpoint, &b->endpoint);
}
static int valid(const gnb_utc_policy_t *p) {
    static const uint8_t zero[16] = {0};
    if (!p->peer_id || !p->pair_id || !p->pair_gen) return 0;
    if (p->mode == GNB_UTC_DIRECT)
        return !p->expires_at && !p->deadline_tick && !p->endpoint.family &&
            !p->endpoint.port && !p->endpoint.scope_id &&
            !memcmp(p->endpoint.address, zero, 16);
    if (p->mode != GNB_UTC_EXPORT || !p->expires_at || !p->deadline_tick ||
        !p->endpoint.port) return 0;
    if (p->endpoint.family == 4)
        return !p->endpoint.scope_id && memcmp(p->endpoint.address, zero, 4) &&
            !memcmp(p->endpoint.address + 4, zero, 12) &&
            p->endpoint.address[0] < 224;
    if (p->endpoint.family == 6) {
        /* Reject v4-mapped-as-v6 locally: family 6 is a real IPv6 destination,
         * not another spelling of IPv4. */
        static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0,
                                           0, 0, 0xff, 0xff};
        if (!memcmp(p->endpoint.address, mapped, sizeof(mapped))) return 0;
        return memcmp(p->endpoint.address, zero, 16) && p->endpoint.address[0] != 255;
    }
    return 0;
}
static size_t find(const gnb_utc_table_t *t, uint64_t peer) {
    size_t i;
    for (i = 0; i < t->count; ++i)
        if (t->entries[i].policy.peer_id == peer) break;
    return i;
}
int gnb_utc_init(gnb_utc_table_t *t) {
    if (!t) return GNB_UTC_INVALID;
    memset(t, 0, sizeof(*t));
    return pthread_mutex_init(&t->lock, NULL) ? GNB_UTC_INVALID : GNB_UTC_OK;
}
void gnb_utc_destroy(gnb_utc_table_t *t) {
    if (t) pthread_mutex_destroy(&t->lock);
}
int gnb_utc_snapshot(gnb_utc_table_t *t, gnb_utc_policy_t *out,
                     size_t capacity, size_t *count) {
    size_t i, used = 0;
    if (!t || !out || !count) return GNB_UTC_INVALID;
    pthread_mutex_lock(&t->lock);
    for (i = 0; i < t->count; ++i) {
        if (t->entries[i].blocked) continue;
        if (used == capacity) {
            pthread_mutex_unlock(&t->lock);
            return GNB_UTC_CAPACITY;
        }
        out[used++] = t->entries[i].policy;
    }
    pthread_mutex_unlock(&t->lock);
    *count = used;
    return GNB_UTC_OK;
}

int gnb_utc_block_peer(gnb_utc_table_t *t, uint64_t peer_id) {
    size_t i;
    if (!t || !peer_id) return GNB_UTC_INVALID;
    pthread_mutex_lock(&t->lock);
    for (i = 0; i < t->count; ++i) {
        if (t->entries[i].policy.peer_id == peer_id) {
            t->entries[i].blocked = 1;
            pthread_mutex_unlock(&t->lock);
            return GNB_UTC_OK;
        }
    }
    pthread_mutex_unlock(&t->lock);
    return GNB_UTC_CONFLICT;
}

int gnb_utc_apply(gnb_utc_table_t *t, uint64_t gen,
                  const gnb_utc_policy_t *p, size_t count) {
    size_t i, j, additions = 0;
    int result = GNB_UTC_OK;
    if (!t || !gen || count > GNB_UTC_MAX_PEERS || (count && !p))
        return GNB_UTC_INVALID;
    for (i = 0; i < count; ++i) {
        if (!valid(p + i)) return GNB_UTC_INVALID;
        for (j = 0; j < i; ++j)
            if (p[i].peer_id == p[j].peer_id || p[i].pair_id == p[j].pair_id)
                return GNB_UTC_CONFLICT;
    }
    pthread_mutex_lock(&t->lock);
    if (gen < t->config_gen) { result = GNB_UTC_REWIND; goto done; }
    /* Validate the entire transaction before mutating any peer. */
    for (i = 0; i < count; ++i) {
        for (j = 0; j < t->count; ++j) {
            if (t->entries[j].policy.pair_id == p[i].pair_id &&
                t->entries[j].policy.peer_id != p[i].peer_id) {
                result = GNB_UTC_CONFLICT; goto done;
            }
        }
        j = find(t, p[i].peer_id);
        if (j == t->count) {
            if (gen == t->config_gen) { result = GNB_UTC_CONFLICT; goto done; }
            ++additions;
            continue;
        }
        if (p[i].pair_id != t->entries[j].policy.pair_id) {
            result = GNB_UTC_CONFLICT; goto done;
        }
        if (p[i].pair_gen < t->entries[j].policy.pair_gen) {
            result = GNB_UTC_REWIND; goto done;
        }
        if (gen == t->config_gen && !policy_equal(p + i, &t->entries[j].policy)) {
            result = GNB_UTC_CONFLICT; goto done;
        }
        if (p[i].pair_gen == t->entries[j].policy.pair_gen &&
            !policy_equal(p + i, &t->entries[j].policy)) {
            /* utc-v1 and state-v1 records carry no epochs (lease 0); the
             * same-generation renewal path is a state-v2 feature and must not
             * relax their original conflict semantics (ADR-0005). */
            if (!t->entries[j].policy.lease_epoch ||
                !policy_identity_equal(p + i, &t->entries[j].policy)) {
                result = GNB_UTC_CONFLICT; goto done;
            }
            if (!GNB_UTC_LEASE_RENEWABLE(&t->entries[j].policy, p + i)) {
                /* Identity already matched, so only a watermark regression
                 * remains: expiry or an epoch moved backwards. */
                result = GNB_UTC_REWIND; goto done;
            }
        }
    }
    if (t->count + additions > GNB_UTC_MAX_PEERS) {
        result = GNB_UTC_CAPACITY; goto done;
    }
    if (gen == t->config_gen) {
        if (count != t->snapshot_count) { result = GNB_UTC_CONFLICT; goto done; }
        for (j = 0; j < t->snapshot_count; ++j) {
            for (i = 0; i < count; ++i)
                if (p[i].peer_id == t->snapshot_peers[j]) break;
            if (i == count) { result = GNB_UTC_CONFLICT; goto done; }
        }
        goto done;
    }
    for (j = 0; j < t->count; ++j) {
        for (i = 0; i < count; ++i)
            if (p[i].peer_id == t->entries[j].policy.peer_id) break;
        if (i == count && t->entries[j].policy.mode == GNB_UTC_EXPORT)
            t->entries[j].blocked = 1;
    }
    for (i = 0; i < count; ++i) {
        j = find(t, p[i].peer_id);
        if (j == t->count) {
            ++t->count;
            t->entries[j].blocked = 0;
        } else if (p[i].pair_gen > t->entries[j].policy.pair_gen) {
            t->entries[j].blocked = 0;
        }
        /* A renewal updates the policy but never clears a latched block; only
         * a newer pair_gen revives an export. */
        t->entries[j].policy = p[i];
    }
    t->snapshot_count = count;
    for (i = 0; i < count; ++i) t->snapshot_peers[i] = p[i].peer_id;
    t->config_gen = gen;
done:
    pthread_mutex_unlock(&t->lock);
    return result;
}
gnb_utc_action_t gnb_utc_route(gnb_utc_table_t *t, uint64_t peer,
                              uint64_t now, uint64_t tick,
                              gnb_utc_endpoint_t *endpoint) {
    size_t i;
    gnb_utc_action_t action = GNB_UTC_BLOCK;
    if (endpoint) memset(endpoint, 0, sizeof(*endpoint));
    if (!t || !peer || !endpoint) return action;
    pthread_mutex_lock(&t->lock);
    i = find(t, peer);
    if (i == t->count || t->entries[i].policy.mode == GNB_UTC_DIRECT) {
        action = GNB_UTC_NATIVE;
    } else {
        gnb_utc_entry_t *e = &t->entries[i];
        if (now >= e->policy.expires_at || tick >= e->policy.deadline_tick)
            e->blocked = 1;
        if (!e->blocked) {
            *endpoint = e->policy.endpoint;
            action = GNB_UTC_FORWARD;
        }
    }
    pthread_mutex_unlock(&t->lock);
    return action;
}
int gnb_utc_accept(gnb_utc_table_t *t, uint64_t peer,
                   const gnb_utc_endpoint_t *source, uint64_t now, uint64_t tick) {
    gnb_utc_endpoint_t endpoint;
    gnb_utc_action_t action;
    if (!source) return 0;
    action = gnb_utc_route(t, peer, now, tick, &endpoint);
    return action == GNB_UTC_NATIVE ||
        (action == GNB_UTC_FORWARD && endpoint_equal(source, &endpoint));
}

/* GNB socket/worker integration: routing policy stays in this module. */
#include "gnb.h"
#include <string.h>
#include <time.h>

static void count_tx(gnb_core_t *core, uint64_t peer, size_t size, int blocked,
                     int error) {
    size_t i;
    pthread_mutex_lock(&core->utc_policy->lock);
    if (blocked) {
        ++core->utc_policy->tx_blocked;
    } else if (error) {
        ++core->utc_policy->tx_errors;
    } else {
        ++core->utc_policy->tx_packets;
        for (i = 0; i < core->utc_policy->count; ++i) {
            gnb_utc_entry_t *entry = &core->utc_policy->entries[i];
            if (entry->policy.peer_id != peer) continue;
            if (size > UINT64_MAX - entry->tx_bytes)
                entry->tx_bytes = UINT64_MAX;
            else
                entry->tx_bytes += size;
            break;
        }
    }
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
    if (action == GNB_UTC_BLOCK || !core->conf) { count_tx(core, peer, size, 1, 0); return 1; }
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
    count_tx(core, peer, size, 0, sent != (ssize_t)size);
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


/* Process-start/main-thread assertion; never reset by argv reparsing. */
static int managed_required;
void gnb_utc_require_managed(void) { managed_required = 1; }
int gnb_utc_index_allowed(void) { return !managed_required; }
int gnb_utc_attach(gnb_core_t *core, gnb_conf_t *conf) {
    if (!core || !conf) return -1;
    if (managed_required && (!conf->conf_dir[0] || conf->lite_mode)) {
        fprintf(stderr, "UTC RECOVERY_REQUIRED: managed instance needs its configuration\n");
        return -1;
    }
#ifndef _WIN32
    if (conf->conf_dir[0]) {
        core->utc_service = gnb_utc_service_open(conf->conf_dir, conf->local_uuid,
            conf->udp_socket_type | ((conf->universal_relay0 || conf->universal_relay1) ? GNB_UTC_UR_SERVER : 0) |
            (managed_required ? GNB_UTC_REQUIRED : 0));
        if (!core->utc_service) {
            fprintf(stderr, "UTC RECOVERY_REQUIRED: refusing unsafe instance startup\n");
            return -1;
        }
        core->utc_policy = &core->utc_service->table;
    }
#else
    if (managed_required || (conf->conf_dir[0] && gnb_utc_service_has_state(conf->conf_dir) != 0)) {
        fprintf(stderr, "UTC UNSUPPORTED: managed state requires a capable driver\n");
        return -1;
    }
#endif
    return 0;
}
void gnb_utc_detach(gnb_core_t *core, int remember_history) {
    if (!core) return;
    if (remember_history && core->utc_service && core->utc_service->managed) gnb_utc_require_managed();
    gnb_utc_service_close(core->utc_service);
    core->utc_service = NULL;
    core->utc_policy = NULL;
}
