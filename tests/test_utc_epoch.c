#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#include "gnb_utc_service.h"
#include "ed25519/sha512.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

static const char manifest[65] =
    "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd";

static gnb_utc_policy_t policy(uint64_t peer, uint64_t pair, uint64_t gen,
                               uint64_t lease, uint64_t place,
                               uint64_t expires, uint16_t port) {
    gnb_utc_policy_t p;
    memset(&p, 0, sizeof(p));
    p.peer_id = peer; p.pair_id = pair; p.pair_gen = gen;
    p.lease_epoch = lease; p.placement_epoch = place;
    p.digest[0] = (uint8_t)peer;
    p.mode = GNB_UTC_EXPORT; p.expires_at = expires; p.deadline_tick = 100000;
    p.endpoint.family = 4; p.endpoint.port = port;
    p.endpoint.address[0] = 203; p.endpoint.address[3] = 7;
    return p;
}
static uint16_t routed_port(gnb_utc_table_t *t, uint64_t peer) {
    gnb_utc_endpoint_t e;
    assert(gnb_utc_route(t, peer, 10, 10, &e) == GNB_UTC_FORWARD);
    return e.port;
}
/* Normal RENEW keeps both epochs and extends expiry; advancing an epoch is
 * also a renewal. Identity changes stay conflicts and watermarks never rewind. */
static void renewal_apply(void) {
    gnb_utc_table_t t;
    gnb_utc_policy_t current, update;
    gnb_utc_policy_t snap[1]; size_t count = 0;
    assert(!gnb_utc_init(&t));
    current = policy(7, 8, 1, 1, 1, 1000, 9000);
    assert(!gnb_utc_apply(&t, 1, &current, 1));
    assert(routed_port(&t, 7) == 9000);

    update = current; update.endpoint.port = 9001;
    assert(gnb_utc_apply(&t, 1, &update, 1) == GNB_UTC_CONFLICT);

    /* Normal RENEW: epochs unchanged, only expiry extends. */
    update = current; update.expires_at = 2000;
    assert(!gnb_utc_apply(&t, 2, &update, 1));
    assert(routed_port(&t, 7) == 9000);
    assert(!gnb_utc_snapshot(&t, snap, 1, &count) && count == 1);
    assert(snap[0].lease_epoch == 1 && snap[0].placement_epoch == 1);
    assert(snap[0].expires_at == 2000 && snap[0].endpoint.port == 9000);

    /* Epoch advance with an extended expiry is also accepted. */
    update = current; update.lease_epoch = 2; update.expires_at = 2500;
    assert(!gnb_utc_apply(&t, 3, &update, 1));
    assert(t.entries[0].policy.lease_epoch == 2);

    /* Endpoint and digest changes are identity changes: an advancing epoch
     * does not permit them without a newer pair_gen. */
    update = current; update.lease_epoch = 3; update.expires_at = 3000;
    update.endpoint.port = 9001;
    assert(gnb_utc_apply(&t, 4, &update, 1) == GNB_UTC_CONFLICT);
    update = current; update.lease_epoch = 3; update.expires_at = 3000;
    update.digest[1] = 0x5a;
    assert(gnb_utc_apply(&t, 5, &update, 1) == GNB_UTC_CONFLICT);
    assert(routed_port(&t, 7) == 9000);

    /* Lease or placement epoch rewind. */
    update = policy(7, 8, 1, 1, 1, 3000, 9000);
    assert(gnb_utc_apply(&t, 6, &update, 1) == GNB_UTC_REWIND);
    update = policy(7, 8, 1, 3, 0, 3000, 9000);
    assert(gnb_utc_apply(&t, 7, &update, 1) == GNB_UTC_REWIND);

    /* Expiry rewind is a watermark regression too. */
    update = policy(7, 8, 1, 3, 1, 500, 9000);
    assert(gnb_utc_apply(&t, 8, &update, 1) == GNB_UTC_REWIND);
    assert(routed_port(&t, 7) == 9000);

    /* A placement-only advance with a non-decreasing expiry is a renewal. */
    update = policy(7, 8, 1, 2, 2, 3000, 9000);
    assert(!gnb_utc_apply(&t, 9, &update, 1));
    assert(t.entries[0].policy.placement_epoch == 2);

    /* A renewal updates the policy but never clears a latched block. */
    assert(!gnb_utc_block_peer(&t, 7));
    {
        gnb_utc_endpoint_t e;
        assert(gnb_utc_route(&t, 7, 10, 10, &e) == GNB_UTC_BLOCK);
    }
    update = policy(7, 8, 1, 3, 2, 3000, 9000);
    assert(!gnb_utc_apply(&t, 10, &update, 1));
    assert(t.entries[0].policy.lease_epoch == 3);
    assert(t.entries[0].policy.endpoint.port == 9000);
    assert(t.entries[0].blocked);
    {
        gnb_utc_endpoint_t e;
        assert(gnb_utc_route(&t, 7, 10, 10, &e) == GNB_UTC_BLOCK);
    }
    gnb_utc_destroy(&t);
    puts("PASS same pair_gen renewal vs conflict/rewind watermarks");
}

static void write_text(const char *dir, const char *name, const char *text) {
    char file[1200]; FILE *f;
    snprintf(file, sizeof(file), "%s/%s", dir, name);
    f = fopen(file, "w"); assert(f); assert(fputs(text, f) >= 0); assert(!fclose(f));
    assert(!chmod(file, 0600));
}
static void read_text(const char *dir, const char *name, char *out, size_t capacity) {
    char file[1200]; FILE *f; size_t n;
    snprintf(file, sizeof(file), "%s/%s", dir, name);
    f = fopen(file, "rb"); assert(f);
    n = fread(out, 1, capacity - 1, f); assert(!ferror(f)); assert(!fclose(f));
    out[n] = 0;
}
static size_t projection(char *out, size_t capacity, const char *boot, uint64_t pid,
                         uint64_t gen, uint64_t lease, uint64_t place,
                         uint64_t expires, unsigned port) {
    int n = snprintf(out, capacity,
        "utc-v2 epoch.scope epoch-instance 42 %"PRIu64" %s %"PRIu64" 3\n"
        "peer 7 8 1 1 %"PRIu64" %"PRIu64" %"PRIu64" 4 00000000000000000000ffffcb007107 %u 0 %s\n",
        gen, boot, pid, lease, place, expires, port, manifest);
    assert(n > 0 && (size_t)n < capacity);
    return (size_t)n;
}
static int poll(gnb_utc_service_t *s) { s->last_poll = 0; return gnb_utc_service_poll(s); }
static gnb_utc_entry_t *entry(gnb_utc_service_t *s, uint64_t peer) {
    size_t i;
    for (i = 0; i < s->table.count; ++i)
        if (s->table.entries[i].policy.peer_id == peer) return &s->table.entries[i];
    assert(0); return NULL;
}
static void identity_matches(const char *dir, gnb_utc_service_t *s) {
    char file[1200], text[256], expected[160]; struct stat st; FILE *f; size_t n;
    snprintf(file, sizeof(file), "%s/utc.identity", dir);
    assert(!lstat(file, &st) && S_ISREG(st.st_mode) && st.st_uid == geteuid());
    assert((st.st_mode & 0777) == 0600);
    f = fopen(file, "r"); assert(f);
    n = fread(text, 1, sizeof(text) - 1, f); assert(!ferror(f)); assert(!fclose(f));
    text[n] = 0;
    snprintf(expected, sizeof(expected), "identity-v1 42 %s %"PRIu64"\n",
             s->boot_id, s->process_id);
    assert(!strcmp(text, expected));
}
static void cleanup(const char *dir) {
    struct dirent *item; char file[1200]; DIR *d = opendir(dir); assert(d);
    while ((item = readdir(d))) {
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
        snprintf(file, sizeof(file), "%s/%s", dir, item->d_name); assert(!unlink(file));
    }
    closedir(d); assert(!rmdir(dir));
}

/* utc-v2 renewal through the service, state-v2 persistence, restart recovery. */
static void service_renewal_and_state(void) {
    char dir[] = "/tmp/gnb-utc-epoch-XXXXXX", text[4096], state[16384];
    char boot[33]; gnb_utc_service_t *s; gnb_utc_endpoint_t e;
    uint64_t now = (uint64_t)time(NULL), e1 = now + 1000, e2 = now + 2000;
    assert(mkdtemp(dir));
    { char file[1200]; snprintf(file, sizeof(file), "%s/utc.identity", dir);
      assert(access(file, F_OK) != 0); }
    s = gnb_utc_service_open(dir, 42, 1); assert(s); assert(!s->managed);
    identity_matches(dir, s);
    assert(gnb_utc_service_has_state(dir) == 0); /* identity is not managed state */
    strcpy(boot, s->boot_id);

    projection(text, sizeof(text), boot, s->process_id, 1, 1, 1, e1, 9555);
    write_text(dir, "utc.conf", text);
    assert(!poll(s)); assert(s->managed && s->table.config_gen == 1);
    assert(routed_port(&s->table, 7) == 9555);
    assert(entry(s, 7)->policy.lease_epoch == 1 && entry(s, 7)->policy.placement_epoch == 1);

    projection(text, sizeof(text), boot, s->process_id, 2, 2, 1, e2, 9555);
    write_text(dir, "utc.conf", text);
    assert(!poll(s)); assert(s->table.config_gen == 2);
    assert(routed_port(&s->table, 7) == 9555);
    assert(entry(s, 7)->policy.lease_epoch == 2);
    read_text(dir, "utc.state.0", state, sizeof(state));
    assert(!strncmp(state, "state-v2 ", 9));
    assert(strstr(state, " 9555 0 ") && strstr(state, " 2 1\n"));

    /* Normal RENEW: same epochs, only a later expiry. */
    projection(text, sizeof(text), boot, s->process_id, 3, 2, 1, e2 + 500, 9555);
    write_text(dir, "utc.conf", text);
    assert(!poll(s)); assert(s->table.config_gen == 3);
    assert(routed_port(&s->table, 7) == 9555);
    assert(entry(s, 7)->policy.lease_epoch == 2);

    /* Endpoint change, epoch rewind and expiry rewind are all rejected. */
    projection(text, sizeof(text), boot, s->process_id, 4, 3, 1, e2 + 1000, 9666);
    write_text(dir, "utc.conf", text);
    assert(poll(s) < 0 && !strcmp(s->reason, "GENERATION_CONFLICT"));
    projection(text, sizeof(text), boot, s->process_id, 5, 1, 1, e2 + 1500, 9555);
    write_text(dir, "utc.conf", text);
    assert(poll(s) < 0 && !strcmp(s->reason, "VERSION_REWIND"));
    assert(s->table.config_gen == 3 && routed_port(&s->table, 7) == 9555);
    projection(text, sizeof(text), boot, s->process_id, 6, 3, 1, e1 + 1, 9555);
    write_text(dir, "utc.conf", text);
    assert(poll(s) < 0 && !strcmp(s->reason, "VERSION_REWIND"));
    assert(routed_port(&s->table, 7) == 9555);

    /* Renewal at a newer config_gen applies and persists. */
    projection(text, sizeof(text), boot, s->process_id, 7, 3, 1, e2 + 2000, 9555);
    write_text(dir, "utc.conf", text);
    assert(!poll(s)); assert(s->table.config_gen == 7);
    assert(routed_port(&s->table, 7) == 9555);
    assert(entry(s, 7)->policy.lease_epoch == 3);
    gnb_utc_service_close(s);

    /* Restart: state-v2 recovers the watermarks, exports stay blocked, and the
     * identity follows the new boot/PID. */
    s = gnb_utc_service_open(dir, 42, 1 | GNB_UTC_REQUIRED); assert(s);
    assert(s->managed && s->table.config_gen == 7);
    assert(strcmp(boot, s->boot_id));
    identity_matches(dir, s);
    memset(&e, 0, sizeof(e));
    assert(gnb_utc_route(&s->table, 7, now + 1, 1, &e) == GNB_UTC_BLOCK);
    assert(entry(s, 7)->policy.lease_epoch == 3 && entry(s, 7)->policy.placement_epoch == 1);
    assert(entry(s, 7)->policy.pair_gen == 1);

    /* A renewal over recovered state advances the watermark but cannot revive
     * a blocked export without a newer pair_gen. */
    projection(text, sizeof(text), s->boot_id, s->process_id, 8, 4, 1, e2 + 3000, 9555);
    write_text(dir, "utc.conf", text);
    assert(!poll(s)); assert(s->table.config_gen == 8);
    assert(entry(s, 7)->policy.lease_epoch == 4 && entry(s, 7)->policy.endpoint.port == 9555);
    assert(entry(s, 7)->blocked);
    memset(&e, 0, sizeof(e));
    assert(gnb_utc_route(&s->table, 7, now + 1, 1, &e) == GNB_UTC_BLOCK);
    read_text(dir, "utc.state.2", state, sizeof(state));
    assert(!strncmp(state, "state-v2 ", 9) && strstr(state, " 4 1\n"));
    gnb_utc_service_close(s);
    cleanup(dir);
    puts("PASS utc-v2 renewal, state-v2 persistence and restart watermarks");
}

/* state-v1 bytes stay readable (epochs 0) and the next export is state-v2. */
static void state_v1_compatibility(void) {
    char dir[] = "/tmp/gnb-utc-epoch-v1-XXXXXX", text[1024], state[4096];
    char file[1200]; unsigned char digest[64]; size_t i, used;
    gnb_utc_service_t *s; gnb_utc_endpoint_t e;
    uint64_t now = (uint64_t)time(NULL);
    assert(mkdtemp(dir));
    used = (size_t)snprintf(text, sizeof(text),
        "state-v1 epoch.scope epoch-instance 42 2\n"
        "entry 7 8 1 1 %"PRIu64" 4 cb007107000000000000000000000000 9555 0 %s\n",
        now + 5000, manifest);
    memcpy(text + used, "sha512 ", 7);
    sha512((const unsigned char *)text, used, digest);
    for (i = 0; i < 64; ++i) snprintf(text + used + 7 + i * 2, 3, "%02x", digest[i]);
    text[used + 7 + 128] = '\n'; text[used + 7 + 129] = 0;
    snprintf(file, sizeof(file), "%s/utc.state.0", dir); write_text(dir, "utc.state.0", text);
    s = gnb_utc_service_open(dir, 42, 1); assert(s);
    assert(s->managed && s->table.config_gen == 2);
    assert(entry(s, 7)->policy.lease_epoch == 0 && entry(s, 7)->policy.placement_epoch == 0);
    memset(&e, 0, sizeof(e));
    assert(gnb_utc_route(&s->table, 7, now + 1, 1, &e) == GNB_UTC_BLOCK);

    projection(text, sizeof(text), s->boot_id, s->process_id, 3, 1, 1, now + 6000, 9555);
    write_text(dir, "utc.conf", text);
    assert(!poll(s)); assert(s->table.config_gen == 3);
    assert(entry(s, 7)->policy.lease_epoch == 1);
    for (i = 0; i < 3; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "utc.state.%u", (unsigned)i);
        read_text(dir, name, state, sizeof(state));
        assert(!strncmp(state, "state-v2 ", 9));
    }
    read_text(dir, "utc.managed", state, sizeof(state));
    assert(!strncmp(state, "state-v2 ", 9));
    gnb_utc_service_close(s);
    cleanup(dir);
    puts("PASS state-v1 decode with zero epochs and state-v2 upgrade");
}

/* Replica checksum corruption of a state-v2 set stays fail-closed. */
static void state_v2_corruption(void) {
    char dir[] = "/tmp/gnb-utc-epoch-bad-XXXXXX", text[4096], state[4096];
    gnb_utc_service_t *s;
    uint64_t now = (uint64_t)time(NULL);
    unsigned i;
    assert(mkdtemp(dir));
    s = gnb_utc_service_open(dir, 42, 1); assert(s);
    projection(text, sizeof(text), s->boot_id, s->process_id, 1, 1, 1, now + 1000, 9555);
    write_text(dir, "utc.conf", text);
    assert(!poll(s)); assert(s->managed);
    gnb_utc_service_close(s);

    read_text(dir, "utc.state.0", state, sizeof(state));
    assert(!strncmp(state, "state-v2 ", 9));
    { char *footer = strstr(state, "sha512 "); assert(footer);
      footer[7] = footer[7] == '0' ? '1' : '0'; }
    write_text(dir, "utc.state.0", state);
    s = gnb_utc_service_open(dir, 42, 1 | GNB_UTC_REQUIRED); assert(s); /* other replicas */
    assert(s->table.config_gen == 1);
    gnb_utc_service_close(s);

    /* state.0 was already corrupted above; corrupt the remaining replicas and
     * the managed record without flipping it back to a valid checksum. */
    for (i = 1; i < 3; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "utc.state.%u", i);
        read_text(dir, name, state, sizeof(state));
        { char *footer = strstr(state, "sha512 "); assert(footer && footer[7]);
          footer[7] = footer[7] == '0' ? '1' : '0'; }
        write_text(dir, name, state);
    }
    read_text(dir, "utc.managed", state, sizeof(state));
    { char *footer = strstr(state, "sha512 "); assert(footer);
      footer[7] = footer[7] == '0' ? '1' : '0'; }
    write_text(dir, "utc.managed", state);
    assert(!gnb_utc_service_open(dir, 42, 1)); /* no trusted replica remains */
    cleanup(dir);
    puts("PASS state-v2 replica corruption stays fail-closed");
}

/* Identity is rewritten on boot/PID change and repairs loose permissions. */
static void identity_lifecycle(void) {
    char dir[] = "/tmp/gnb-utc-identity-XXXXXX", file[1200];
    char first[33], second[33]; gnb_utc_service_t *s;
    assert(mkdtemp(dir));
    s = gnb_utc_service_open(dir, 42, 1); assert(s);
    identity_matches(dir, s);
    strcpy(first, s->boot_id);
    gnb_utc_service_close(s);

    s = gnb_utc_service_open(dir, 42, 1); assert(s);
    assert(strcmp(first, s->boot_id));
    identity_matches(dir, s);
    strcpy(second, s->boot_id);
    snprintf(file, sizeof(file), "%s/utc.identity", dir);
    assert(!chmod(file, 0644));
    gnb_utc_service_close(s);

    s = gnb_utc_service_open(dir, 42, 1); assert(s);
    assert(strcmp(second, s->boot_id));
    identity_matches(dir, s); /* 0600 repaired on rewrite */
    gnb_utc_service_close(s);
    cleanup(dir);
    puts("PASS utc.identity first write, boot refresh and 0600");
}

int main(void) {
    renewal_apply();
    service_renewal_and_state();
    state_v1_compatibility();
    state_v2_corruption();
    identity_lifecycle();
    return 0;
}
