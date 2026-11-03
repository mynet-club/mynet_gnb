#include "gnb_utc.h"
#include <string.h>

static int endpoint_equal(const gnb_utc_endpoint_t *a, const gnb_utc_endpoint_t *b) {
    return a->family == b->family && a->port == b->port &&
        a->scope_id == b->scope_id && !memcmp(a->address, b->address, 16);
}
static int policy_equal(const gnb_utc_policy_t *a, const gnb_utc_policy_t *b) {
    return a->peer_id == b->peer_id && a->pair_id == b->pair_id &&
        a->pair_gen == b->pair_gen && a->mode == b->mode &&
        a->expires_at == b->expires_at &&
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
    if (p->endpoint.family == 6)
        return memcmp(p->endpoint.address, zero, 16) && p->endpoint.address[0] != 255;
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
        if ((p[i].pair_gen == t->entries[j].policy.pair_gen || gen == t->config_gen)
            && !policy_equal(p + i, &t->entries[j].policy)) {
            result = GNB_UTC_CONFLICT; goto done;
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
        if (t->entries[j].policy.pair_gen != p[i].pair_gen ||
            !t->entries[j].policy.peer_id) t->entries[j].policy = p[i];
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
