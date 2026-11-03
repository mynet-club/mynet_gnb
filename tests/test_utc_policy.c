#include "gnb_utc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static gnb_utc_policy_t policy(uint64_t peer) {
    gnb_utc_policy_t p;
    memset(&p, 0, sizeof(p));
    p.peer_id = peer; p.pair_id = peer + 100; p.pair_gen = 1;
    p.mode = GNB_UTC_EXPORT; p.expires_at = 1000; p.deadline_tick = 500;
    p.endpoint.family = 4; p.endpoint.port = 9000;
    p.endpoint.address[0] = 127; p.endpoint.address[3] = 1;
    return p;
}
static void lifecycle(void) {
    gnb_utc_table_t t, other;
    gnb_utc_policy_t p[2] = {policy(1), policy(2)};
    gnb_utc_endpoint_t e, bad = p[0].endpoint;
    assert(!gnb_utc_init(&t)); assert(!gnb_utc_init(&other));
    assert(gnb_utc_route(&t, 1, 1, 1, &e) == GNB_UTC_NATIVE);
    assert(!gnb_utc_apply(&t, 1, p, 2));
    assert(!gnb_utc_apply(&t, 1, p, 2));
    assert(gnb_utc_route(&other, 1, 1, 1, &e) == GNB_UTC_NATIVE);
    assert(gnb_utc_route(&t, 1, 1, 1, &e) == GNB_UTC_FORWARD);
    assert(gnb_utc_accept(&t, 1, &e, 1, 1));
    ++bad.port; assert(!gnb_utc_accept(&t, 1, &bad, 1, 1));
    assert(gnb_utc_route(&t, 3, 1, 1, &e) == GNB_UTC_NATIVE);
    assert(gnb_utc_apply(&t, 1, p, 1) == GNB_UTC_CONFLICT);
    p[0].endpoint.port++;
    assert(gnb_utc_apply(&t, 2, p, 2) == GNB_UTC_CONFLICT);
    p[0].endpoint.port--;
    assert(!gnb_utc_apply(&t, 2, p, 1));
    assert(gnb_utc_route(&t, 2, 1, 1, &e) == GNB_UTC_BLOCK);
    assert(gnb_utc_apply(&t, 2, p, 2) == GNB_UTC_CONFLICT);
    assert(!gnb_utc_apply(&t, 3, p, 2));
    assert(gnb_utc_route(&t, 2, 1, 1, &e) == GNB_UTC_BLOCK);
    assert(gnb_utc_route(&t, 1, 1, 500, &e) == GNB_UTC_BLOCK);
    assert(gnb_utc_route(&t, 1, 1, 1, &e) == GNB_UTC_BLOCK);
    p[0].pair_gen++; p[1].pair_gen++;
    assert(!gnb_utc_apply(&t, 4, p, 2));
    assert(gnb_utc_route(&t, 1, 999, 499, &e) == GNB_UTC_FORWARD);
    assert(gnb_utc_route(&t, 1, 1000, 499, &e) == GNB_UTC_BLOCK);
    assert(!gnb_utc_accept(&t, 1, &p[0].endpoint, 1, 1));
    p[0].mode = GNB_UTC_DIRECT; p[0].expires_at = p[0].deadline_tick = 0;
    memset(&p[0].endpoint, 0, sizeof(p[0].endpoint)); p[0].pair_gen++;
    assert(!gnb_utc_apply(&t, 5, p, 2));
    assert(gnb_utc_route(&t, 1, 1, 1, &e) == GNB_UTC_NATIVE);
    assert(gnb_utc_apply(&t, 4, p, 2) == GNB_UTC_REWIND);
    gnb_utc_destroy(&other); gnb_utc_destroy(&t);
}
static void atomicity_and_capacity(void) {
    gnb_utc_table_t t;
    gnb_utc_policy_t p[GNB_UTC_MAX_PEERS], extra = policy(500);
    gnb_utc_endpoint_t e;
    size_t i;
    assert(!gnb_utc_init(&t));
    for (i = 0; i < GNB_UTC_MAX_PEERS; ++i) p[i] = policy(i+1);
    assert(!gnb_utc_apply(&t, 1, p, GNB_UTC_MAX_PEERS));
    assert(gnb_utc_apply(&t, 2, &extra, 1) == GNB_UTC_CAPACITY);
    assert(gnb_utc_route(&t, 1, 1, 1, &e) == GNB_UTC_FORWARD);
    p[0].pair_gen++; p[0].endpoint.port++;
    p[1].endpoint.family = 9;
    assert(gnb_utc_apply(&t, 2, p, 2) == GNB_UTC_INVALID);
    assert(gnb_utc_route(&t, 1, 1, 1, &e) == GNB_UTC_FORWARD && e.port == 9000);
    p[1] = p[0];
    assert(gnb_utc_apply(&t, 2, p, 2) == GNB_UTC_CONFLICT);
    assert(!gnb_utc_apply(&t, 2, NULL, 0));
    assert(gnb_utc_route(&t, 1, 1, 1, &e) == GNB_UTC_BLOCK);
    assert(!gnb_utc_apply(&t, 2, NULL, 0));
    assert(gnb_utc_apply(&t, 2, p, 1) == GNB_UTC_CONFLICT);
    gnb_utc_destroy(&t);
}
static void ipv6(void) {
    gnb_utc_table_t t; gnb_utc_policy_t p = policy(1); gnb_utc_endpoint_t e;
    assert(!gnb_utc_init(&t));
    memset(p.endpoint.address, 0, 16); p.endpoint.address[15] = 1;
    p.endpoint.family = 6; p.endpoint.scope_id = 7;
    assert(!gnb_utc_apply(&t, 1, &p, 1));
    assert(gnb_utc_accept(&t, 1, &p.endpoint, 1, 1));
    e = p.endpoint; e.scope_id++;
    assert(!gnb_utc_accept(&t, 1, &e, 1, 1));
    assert(gnb_utc_route(NULL, 1, 1, 1, &e) == GNB_UTC_BLOCK);
    assert(!gnb_utc_accept(&t, 1, NULL, 1, 1));
    gnb_utc_destroy(&t);
}
static void *reader(void *arg) {
    gnb_utc_table_t *t = arg;
    gnb_utc_endpoint_t e;
    unsigned i;
    for (i = 0; i < 10000; ++i) {
        assert(gnb_utc_route(t, 1, 1, 1, &e) == GNB_UTC_FORWARD);
        assert((e.port == 9000 && e.address[3] == 1) ||
               (e.port == 9001 && e.address[3] == 2));
    }
    return NULL;
}
static void concurrent_snapshots(void) {
    gnb_utc_table_t t; gnb_utc_policy_t p = policy(1);
    pthread_t threads[3]; unsigned i;
    assert(!gnb_utc_init(&t)); assert(!gnb_utc_apply(&t, 1, &p, 1));
    for (i = 0; i < 3; ++i) assert(!pthread_create(&threads[i], NULL, reader, &t));
    for (i = 2; i < 2000; ++i) {
        p.pair_gen = i;
        p.endpoint.port = 9000 + (i % 2); p.endpoint.address[3] = 1 + (i % 2);
        assert(!gnb_utc_apply(&t, i, &p, 1));
    }
    for (i = 0; i < 3; ++i) assert(!pthread_join(threads[i], NULL));
    gnb_utc_destroy(&t);
}
int main(void) {
    lifecycle(); atomicity_and_capacity(); ipv6(); concurrent_snapshots();
    puts("UTC policy lifecycle, isolation, atomicity, capacity and IPv6: PASS");
    return 0;
}
