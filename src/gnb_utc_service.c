#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
/* Local execution projection, not a lease issuer. POSIX reference persistence
 * protocol: checksummed intent + three full-state replicas, durable before activate.
 * Windows activation deliberately fails until equivalent durable I/O exists. */
#include "gnb_utc_service.h"
#include "gnb_utc_hub.h"
#include "gnb_utc_hub_exec.h"
#include "ed25519/sha512.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif
#ifndef _WIN32
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#endif


int gnb_utc_service_has_state(const char *directory) {
    const char *names[] = {"utc.conf", "utc.managed", "utc.state.0", "utc.state.1", "utc.state.2"};
    size_t i; char file[GNB_UTC_PATH_MAX]; int n;
    if (!directory) return -1;
    for (i = 0; i < sizeof(names)/sizeof(names[0]); ++i) {
        n = snprintf(file, sizeof(file), "%s/%s", directory, names[i]);
        if (n < 0 || n >= (int)sizeof(file)) return -1;
#ifdef _WIN32
        if (GetFileAttributesA(file) != INVALID_FILE_ATTRIBUTES) return 1;
        if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) return -1;
#else
        struct stat st;
        if (!lstat(file, &st)) return 1;
        if (errno != ENOENT) return -1;
#endif
    }
    return 0;
}
static uint64_t ticks(void) {
#ifdef _WIN32
    return 0;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
    return (uint64_t)ts.tv_sec;
#endif
}
static int token(const char *s) {
    size_t n = 0;
    for (; *s; ++s, ++n)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
              (*s >= '0' && *s <= '9') || *s == '_' || *s == '-' || *s == '.')) return 0;
    return n && n < GNB_UTC_TOKEN_MAX;
}
static int number(const char *s, uint64_t *v) {
    char *end;
    if (!*s) return -1;
    for (end = (char *)s; *end; ++end) if (*end < '0' || *end > '9') return -1;
    errno = 0; *v = strtoull(s, &end, 10);
    return errno || *end ? -1 : 0;
}
static void hash(const char *s, char out[129]) {
    unsigned char digest[64]; size_t i;
    sha512((const unsigned char *)s, strlen(s), digest);
    for (i = 0; i < 64; ++i) snprintf(out + i * 2, 3, "%02x", digest[i]);
}
static int unhex(const char *s, unsigned char *out, size_t n) {
    size_t i; unsigned value;
    if (strlen(s) != n * 2) return -1;
    for (i = 0; i < n; ++i) {
        char b[3] = {s[2*i], s[2*i+1], 0};
        if (strspn(b, "0123456789abcdef") != 2 || sscanf(b, "%x", &value) != 1) return -1;
        out[i] = (unsigned char)value;
    }
    return 0;
}
static void hex(const unsigned char *in, size_t n, char *out) {
    size_t i;
    for (i = 0; i < n; ++i) snprintf(out + i*2, 3, "%02x", in[i]);
}
static int append(char *b, size_t *used, const char *fmt, ...) {
    int n; va_list ap;
    va_start(ap, fmt); n = vsnprintf(b + *used, GNB_UTC_TEXT_MAX - *used, fmt, ap); va_end(ap);
    if (n < 0 || (size_t)n >= GNB_UTC_TEXT_MAX - *used) return -1;
    *used += (size_t)n; return 0;
}
#ifndef _WIN32
static int path(gnb_utc_service_t *s, const char *name, char out[GNB_UTC_PATH_MAX]) {
    int n = snprintf(out, GNB_UTC_PATH_MAX, "%s/%s", s->directory, name);
    return n < 0 || n >= GNB_UTC_PATH_MAX ? -1 : 0;
}
#endif
/* Strict files are private, regular, owned by our effective uid; symlinks and
 * partial reads are rejected. 1 means absent, -1 invalid, 0 a complete read. */
static int read_file(gnb_utc_service_t *s, const char *name, char *b) {
#ifdef _WIN32
    (void)s; (void)name; (void)b; return -1;
#else
    char file[GNB_UTC_PATH_MAX]; struct stat st; size_t used = 0; ssize_t n; int fd;
    if (path(s, name, file)) return -1;
    fd = open(file, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT ? 1 : -1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 077) || st.st_size <= 0 || st.st_size >= GNB_UTC_TEXT_MAX) { close(fd); return -1; }
    while ((n = read(fd, b + used, GNB_UTC_TEXT_MAX - 1 - used)) > 0) used += (size_t)n;
    close(fd);
    if (n < 0 || used != (size_t)st.st_size || memchr(b, 0, used)) return -1;
    b[used] = 0; return 0;
#endif
}
static int take_lock(gnb_utc_service_t *s) {
#ifdef _WIN32
    (void)s; return -1;
#else
    char file[GNB_UTC_PATH_MAX]; struct stat st;
    if (s->lock_fd >= 0) return 0;
    if (path(s, "utc.lock", file)) return -1;
    s->lock_fd = open(file, O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
    if (s->lock_fd < 0) return -1;
    if (fstat(s->lock_fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 077) || flock(s->lock_fd, LOCK_EX | LOCK_NB)) {
        close(s->lock_fd); s->lock_fd = -1; return -1;
    }
    return 0;
#endif
}
/* Fault seam is absent from production builds; no environment bypass. */
#ifdef GNB_UTC_TESTING
extern int gnb_utc_test_io_event(const char *name, const char *phase);
#define IO_EVENT(name, phase) gnb_utc_test_io_event(name, phase)
#else
#define IO_EVENT(name, phase) 0
#endif
static int write_file(gnb_utc_service_t *s, const char *name, const char *b) {
#ifdef _WIN32
    (void)s; (void)name; (void)b; return -1;
#else
    char file[GNB_UTC_PATH_MAX], temporary[GNB_UTC_PATH_MAX + 32]; size_t used = 0, length = strlen(b); ssize_t n;
    int fd, dirfd, result = -1;
    if (path(s, name, file) || snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", file) >= (int)sizeof(temporary)) return -1;
    if (IO_EVENT(name, "before-temp")) return -1;
    fd = mkstemp(temporary); if (fd < 0) return -1;
    if (IO_EVENT(name, "after-temp")) goto done;
    while (used < length) {
        n = write(fd, b + used, length - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto done;
        used += (size_t)n;
    }
    if (IO_EVENT(name, "after-write") || fsync(fd) || IO_EVENT(name, "after-fsync") ||
        rename(temporary, file) || IO_EVENT(name, "after-rename")) goto done;
    dirfd = open(s->directory, O_RDONLY);
    if (dirfd < 0) goto done;
    result = fsync(dirfd); close(dirfd);
    if (!result && IO_EVENT(name, "after-dirsync")) result = -1;
done:
    close(fd); unlink(temporary); return result;
#endif
}
/* Guidance file for the agent: the identity of the running service. It is not
 * durable state, is never read back as authority, and a failed write must not
 * affect routing or the persisted table (same treatment as status). */
static void update_identity(gnb_utc_service_t *s, char *scratch) {
    char expected[160]; int n;
    n = snprintf(expected, sizeof(expected), "identity-v1 %"PRIu64" %s %"PRIu64"\n",
                 s->local_id, s->boot_id, s->process_id);
    if (n < 0 || (size_t)n >= sizeof(expected)) return;
    if (read_file(s, "utc.identity", scratch) == 0 && !strcmp(scratch, expected)) return;
    (void)write_file(s, "utc.identity", expected);
}
/* Space-separated fixed grammar, no comments, aliases or ignored trailing keys. */
static int split(char *line, char **v, int max) {
    int n = 0; char *save, *word = strtok_r(line, " \t\r", &save);
    while (word) { if (n == max) return -1; v[n++] = word; word = strtok_r(NULL, " \t\r", &save); }
    return n;
}
/* A record is canonical numeric/hex metadata; no arbitrary payload parsing.
 * state-v2 entries append the two lease watermarks; utc-v1 records and
 * state-v1 entries must not carry them. */
static int record(char **v, int n, gnb_utc_policy_t *p, uint64_t now, uint64_t tick,
                  int with_epochs) {
    uint64_t mode, family, port, scope;
    memset(p, 0, sizeof(*p));
    if (n != (with_epochs ? 13 : 11) || number(v[1], &p->peer_id) || number(v[2], &p->pair_id) ||
        number(v[3], &p->pair_gen) || number(v[4], &mode) || mode > 1 ||
        number(v[5], &p->expires_at) || number(v[6], &family) || family > 255 ||
        unhex(v[7], p->endpoint.address, 16) || number(v[8], &port) || port > 65535 ||
        number(v[9], &scope) || scope > UINT32_MAX || unhex(v[10], p->digest, 32) ||
        (with_epochs && (number(v[11], &p->lease_epoch) ||
                         number(v[12], &p->placement_epoch)))) return -1;
    p->mode = (gnb_utc_mode_t)mode; p->endpoint.family = (uint8_t)family;
    p->endpoint.port = (uint16_t)port; p->endpoint.scope_id = (uint32_t)scope;
    if (p->mode == GNB_UTC_EXPORT) {
        uint64_t remaining = p->expires_at > now ? p->expires_at - now : 0;
        if (!tick || remaining > UINT64_MAX - tick) return -1;
        p->deadline_tick = remaining ? tick + remaining : tick;
    }
    return 0;
}
static int emit_record_ex(char *b, size_t *used, const char *type,
                          const gnb_utc_policy_t *p, int with_epochs) {
    char address[33], digest[65]; hex(p->endpoint.address, 16, address); hex(p->digest, 32, digest);
    if (append(b, used, "%s %"PRIu64" %"PRIu64" %"PRIu64" %u %"PRIu64" %u %s %u %u %s",
        type, p->peer_id, p->pair_id, p->pair_gen, (unsigned)p->mode, p->expires_at,
        p->endpoint.family, address, p->endpoint.port, p->endpoint.scope_id, digest)) return -1;
    if (with_epochs && append(b, used, " %"PRIu64" %"PRIu64, p->lease_epoch, p->placement_epoch))
        return -1;
    return append(b, used, "\n");
}
static int emit_record(char *b, size_t *used, const char *type, const gnb_utc_policy_t *p) {
    return emit_record_ex(b, used, type, p, 0); /* status-v1 keeps its exact bytes */
}
/* utc-v2 writes IPv4 endpoints in the canonical ::ffff: layout; the local
 * policy table keeps the address in the first four bytes with zero padding. */
static int hub_peer_policy(const gnb_utc_hub_peer_t *peer, uint64_t now, uint64_t tick,
                           gnb_utc_policy_t *policy) {
    uint64_t remaining;
    memset(policy, 0, sizeof(*policy));
    if (!tick || unhex(peer->manifest, policy->digest, GNB_UTC_DIGEST_SIZE)) return -1;
    policy->peer_id = peer->peer_id;
    policy->pair_id = peer->pair_id;
    policy->pair_gen = peer->pair_gen;
    policy->lease_epoch = peer->lease_epoch;
    policy->placement_epoch = peer->placement_epoch;
    policy->mode = peer->mode == 1 ? GNB_UTC_EXPORT : GNB_UTC_DIRECT;
    policy->expires_at = peer->expires_at;
    policy->endpoint.family = peer->family;
    policy->endpoint.port = peer->port;
    policy->endpoint.scope_id = peer->scope_id;
    if (peer->family == 4) memcpy(policy->endpoint.address, peer->address + 12, 4);
    else memcpy(policy->endpoint.address, peer->address, sizeof(policy->endpoint.address));
    if (policy->mode == GNB_UTC_EXPORT) {
        remaining = peer->expires_at > now ? peer->expires_at - now : 0;
        if (remaining > UINT64_MAX - tick) return -1;
        policy->deadline_tick = remaining ? tick + remaining : tick;
    }
    return 0;
}
/* utc-v2 is the Hub projection of one whole node: every peer in the file is
 * applied and a peer dropped from it is blocked exactly like a v1 omission.
 * The projection must be bound to this running service (boot/PID) and this
 * instance/local node; any mismatch or apply failure leaves the live table
 * untouched because all work happens on the caller's candidate table. */
static int config_v2(gnb_utc_service_t *s, char *text, gnb_utc_table_t *next,
                     char scope[64], char instance[64]) {
    gnb_utc_hub_projection_t projection;
    gnb_utc_hub_exec_t exec;
    gnb_utc_policy_t current[GNB_UTC_MAX_PEERS], updates[GNB_UTC_MAX_PEERS];
    uint8_t blocked[GNB_UTC_MAX_PEERS];
    char error[64];
    size_t i, j, count, current_count = 0;
    uint64_t now = time(NULL), tick = ticks();
    if (gnb_utc_hub_parse(text, strlen(text), &projection, error, sizeof(error))) return -1;
    if (projection.local_id != s->local_id || strcmp(projection.boot_id, s->boot_id) ||
        projection.process_id != s->process_id) return -1;
    strcpy(scope, projection.scope); strcpy(instance, projection.instance);
    if (s->managed && (strcmp(s->scope, scope) || strcmp(s->instance, instance))) return GNB_UTC_SCOPE;
    for (i = 0; i < projection.count; ++i) {
        if (hub_peer_policy(&projection.peers[i], now, tick, updates + i)) return -1;
        if (updates[i].mode == GNB_UTC_EXPORT &&
            (s->ur_server || !(s->address_families & (updates[i].endpoint.family == 4 ? 1u : 2u))))
            return GNB_UTC_UNSUPPORTED;
        blocked[i] = 0;
    }
    count = projection.count;
    if (gnb_utc_snapshot(next, current, GNB_UTC_MAX_PEERS, &current_count)) return GNB_UTC_CAPACITY;
    for (j = 0; j < current_count; ++j) {
        for (i = 0; i < count; ++i) if (updates[i].peer_id == current[j].peer_id) break;
        if (i < count) continue;
        if (count == GNB_UTC_HUB_EXEC_MAX) return GNB_UTC_CAPACITY;
        updates[count] = current[j]; blocked[count] = 1; ++count;
    }
    gnb_utc_hub_exec_init(&exec);
    return gnb_utc_hub_exec_apply(next, &exec, projection.config_gen, updates, blocked, count);
}
/* Decode config into a fresh table; all syntax and policy checks precede commit.
 * utc-v1 keeps its original grammar and ordering rules byte for byte. */
static int config(gnb_utc_service_t *s, char *text, gnb_utc_table_t *next, char scope[64], char instance[64]) {
    char *line, *save, *v[12]; uint64_t local, gen; int n; size_t count = 0;
    gnb_utc_policy_t policies[GNB_UTC_MAX_PEERS];
    uint64_t now = time(NULL), tick = ticks();
    size_t version = strcspn(text, " \t\r\n");
    /* Version dispatch precedes tokenisation so v2 keeps the exact file bytes
     * its canonical round-trip check needs. */
    if (version == 6 && !memcmp(text, "utc-v2", 6)) return config_v2(s, text, next, scope, instance);
    line = strtok_r(text, "\n", &save); if (!line) return -1;
    n = split(line, v, 12);
    if (n != 5 || strcmp(v[0], "utc-v1") || !token(v[1]) || !token(v[2]) ||
        number(v[3], &local) || local != s->local_id || number(v[4], &gen) || !gen) return -1;
    strcpy(scope, v[1]); strcpy(instance, v[2]);
    if (s->managed && (strcmp(s->scope, scope) || strcmp(s->instance, instance))) return GNB_UTC_SCOPE;
    while ((line = strtok_r(NULL, "\n", &save))) {
        n = split(line, v, 12);
        if (count == GNB_UTC_MAX_PEERS || n < 1 || strcmp(v[0], "peer") ||
            record(v, n, policies + count, now, tick, 0) || policies[count].peer_id == local) return -1;
        if (policies[count].mode == GNB_UTC_EXPORT &&
            (s->ur_server || !(s->address_families & (policies[count].endpoint.family == 4 ? 1u : 2u)))) return GNB_UTC_UNSUPPORTED;
        ++count;
    }
    return gnb_utc_apply(next, gen, policies, count);
}
static void copy_table(gnb_utc_table_t *dst, gnb_utc_table_t *src) {
    pthread_mutex_lock(&src->lock);
    dst->config_gen = src->config_gen; dst->count = src->count;
    dst->snapshot_count = src->snapshot_count;
    memcpy(dst->snapshot_peers, src->snapshot_peers, sizeof(dst->snapshot_peers));
    memcpy(dst->entries, src->entries, sizeof(dst->entries));
    pthread_mutex_unlock(&src->lock);
}
static int encode(gnb_utc_service_t *s, gnb_utc_table_t *t, char *b) {
    size_t used = 0, i; char digest[129];
    if (append(b, &used, "state-v2 %s %s %"PRIu64" %"PRIu64"\n", s->scope, s->instance, s->local_id, t->config_gen)) return -1;
    for (i = 0; i < t->count; ++i) if (emit_record_ex(b, &used, "entry", &t->entries[i].policy, 1)) return -1;
    hash(b, digest); return append(b, &used, "sha512 %s\n", digest);
}
/* Recovery never reactivates an export from disk. New authorization projection
 * with a larger pair generation is needed. Released direct tombstones survive.
 * state-v1 remains readable with zero watermarks; state-v2 entries carry the
 * persisted epochs. A v2 entry without epochs (or a v1 entry with them) is
 * corruption, not an older format. */
static int decode(gnb_utc_service_t *s, char *b, gnb_utc_table_t *out, char scope[64], char instance[64]) {
    char expected[129], *footer = strstr(b, "sha512 ");
    char *line, *save, *v[14]; uint64_t local, gen; int n, version_2 = 0; size_t count = 0;
    gnb_utc_policy_t policies[GNB_UTC_MAX_PEERS];
    if (!footer || strlen(footer) != 136 || footer[135] != '\n') return -1;
    memcpy(expected, footer + 7, 128); expected[128] = 0; *footer = 0;
    { char actual[129]; hash(b, actual); if (strcmp(actual, expected)) return -1; }
    line = strtok_r(b, "\n", &save); if (!line || split(line, v, 14) != 5 || !token(v[1]) || !token(v[2]) ||
        number(v[3], &local) || local != s->local_id || number(v[4], &gen) || !gen) return -1;
    if (!strcmp(v[0], "state-v2")) version_2 = 1;
    else if (strcmp(v[0], "state-v1")) return -1;
    strcpy(scope, v[1]); strcpy(instance, v[2]);
    while ((line = strtok_r(NULL, "\n", &save))) {
        n = split(line, v, 14);
        if (count == GNB_UTC_MAX_PEERS || n < 1 || strcmp(v[0], "entry") ||
            record(v, n, policies + count, time(NULL), ticks(), version_2)) return -1;
        ++count;
    }
    if (gnb_utc_apply(out, gen, policies, count)) return -1;
    for (count = 0; count < out->count; ++count)
        if (out->entries[count].policy.mode == GNB_UTC_EXPORT) out->entries[count].blocked = 1;
    return 0;
}
static void block_exports(gnb_utc_table_t *t) {
    size_t i; pthread_mutex_lock(&t->lock);
    for (i = 0; i < t->count; ++i) if (t->entries[i].policy.mode == GNB_UTC_EXPORT) t->entries[i].blocked = 1;
    pthread_mutex_unlock(&t->lock);
}
static void block_changed_exports(gnb_utc_table_t *current, const gnb_utc_table_t *candidate) {
    size_t i, j;
    pthread_mutex_lock(&current->lock);
    for (i = 0; i < current->count; ++i) {
        if (current->entries[i].policy.mode != GNB_UTC_EXPORT) continue;
        for (j = 0; j < candidate->count; ++j)
            if (candidate->entries[j].policy.peer_id == current->entries[i].policy.peer_id) break;
        if (j == candidate->count ||
            candidate->entries[j].policy.pair_gen != current->entries[i].policy.pair_gen ||
            candidate->entries[j].blocked != current->entries[i].blocked)
            current->entries[i].blocked = 1;
    }
    pthread_mutex_unlock(&current->lock);
}
static int persist(gnb_utc_service_t *s, gnb_utc_table_t *t, char *buffer) {
    int i; char name[32];
    if (encode(s, t, buffer)) return -1;
    for (i = 0; i < 3; ++i) {
        snprintf(name, sizeof(name), "utc.state.%d", i);
        if (write_file(s, name, buffer)) return -1;
    }
    return 0;
}
static int status(gnb_utc_service_t *s, char *buffer) {
    size_t i, used = 0; uint64_t now = time(NULL), tick = ticks();
    if (append(buffer, &used, "status-v1 %s %s %"PRIu64" %s %"PRIu64" %"PRIu64" %s %"PRIu64"\n",
        s->scope[0] ? s->scope : "none", s->instance[0] ? s->instance : "none", s->local_id,
        s->boot_id, s->table.config_gen, now, s->reason, s->process_id)) return -1;
    { uint64_t sent, blocked, errors, dropped;
      pthread_mutex_lock(&s->table.lock);
      sent = s->table.tx_packets; blocked = s->table.tx_blocked;
      errors = s->table.tx_errors; dropped = s->table.rx_dropped;
      pthread_mutex_unlock(&s->table.lock);
      if (append(buffer, &used, "metrics %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64"\n", sent, blocked, errors, dropped)) return -1;
    }
    for (i = 0; i < s->table.count; ++i) {
        gnb_utc_endpoint_t endpoint; gnb_utc_policy_t *p = &s->table.entries[i].policy;
        gnb_utc_action_t action = gnb_utc_route(&s->table, p->peer_id, now, tick, &endpoint);
        char digest[65], local_digest[129]; size_t record_start = used;
        hex(p->digest, 32, digest);
        if (emit_record(buffer, &used, "policy", p)) return -1;
        hash(buffer + record_start, local_digest);
        const char *reason = action == GNB_UTC_NATIVE ? "EXPLICIT_DIRECT" : "POLICY_LOADED";
        if (action == GNB_UTC_BLOCK) {
            reason = "REQUIRES_NEW_GENERATION";
            if (now >= p->expires_at || tick >= p->deadline_tick) reason = "EXPIRED";
            else if (s->fault) reason = "STORAGE_FAILED";
            else if (!strcmp(s->reason, "CONFIG_MISSING")) reason = "CONFIG_MISSING";
        }
        if (append(buffer, &used, "peer %"PRIu64" %"PRIu64" %"PRIu64" %s %s %s %s\n", p->peer_id, p->pair_id, p->pair_gen,
            action == GNB_UTC_BLOCK ? "BLOCKED" : "APPLIED", reason, digest, local_digest)) return -1;
    }
    return write_file(s, "utc.status", buffer);
}

gnb_utc_service_t *gnb_utc_service_open(const char *directory, uint64_t local_id, unsigned address_families) {
    gnb_utc_service_t *s; char *buffer; int i, present = 0, recovered = 0;
    uint64_t best = 0; char selected_hash[129] = "";
    if (!directory || strlen(directory) >= GNB_UTC_DIR_MAX || !local_id || !(address_families & 3u) || (address_families & ~(3u | GNB_UTC_UR_SERVER | GNB_UTC_REQUIRED))) return NULL;
    s = calloc(1, sizeof(*s)); buffer = malloc(GNB_UTC_TEXT_MAX);
    if (!s || !buffer) { free(s); free(buffer); return NULL; }
    s->lock_fd = -1;
    if (gnb_utc_init(&s->table)) { free(s); free(buffer); return NULL; }
    strcpy(s->directory, directory); s->local_id = local_id; s->address_families = address_families & 3u; s->ur_server = !!(address_families & GNB_UTC_UR_SERVER); strcpy(s->reason, "NEVER_ENABLED");
#ifndef _WIN32
    s->process_id = (uint64_t)getpid();
    { char file[GNB_UTC_PATH_MAX]; struct stat st; int existing = 0;
      const char *names[] = {"utc.conf", "utc.managed", "utc.state.0", "utc.state.1", "utc.state.2"};
      for (i = 0; i < 5; ++i) {
          if (path(s, names[i], file)) goto failure;
          if (!lstat(file, &st) || errno != ENOENT) existing = 1;
      }
      /* utc.identity is authoritative for the agent, so only the instance
       * lock holder may stamp it. Cores that have not won the lock defer the
       * write to the first poll that holds it; the disclose-only status
       * bootstraps the agent until then. */
      s->identity_pending = !existing;
      if (existing && take_lock(s)) goto failure; }
    { unsigned char random[16]; int fd = open("/dev/urandom", O_RDONLY);
      if (fd < 0) goto failure;
      if (read(fd, random, sizeof(random)) != sizeof(random)) { close(fd); goto failure; }
      close(fd); hex(random, sizeof(random), s->boot_id); }
#endif
    for (i = 0; i < 4; ++i) {
        char name[32], scope[64], instance[64], digest[129]; int r; gnb_utc_table_t candidate;
        if (i == 3) strcpy(name, "utc.managed");
        else snprintf(name, sizeof(name), "utc.state.%d", i);
        r = read_file(s, name, buffer); if (r == 1) continue; ++present;
        if (r) continue;
        hash(buffer, digest);
        if (gnb_utc_init(&candidate)) goto failure;
        r = decode(s, buffer, &candidate, scope, instance);
        if (!r) {
            if (recovered && (strcmp(scope, s->scope) || strcmp(instance, s->instance))) { gnb_utc_destroy(&candidate); goto failure; }
            if (candidate.config_gen == best && recovered && strcmp(digest, selected_hash)) { gnb_utc_destroy(&candidate); goto failure; }
            if (!recovered || candidate.config_gen > best) {
                copy_table(&s->table, &candidate); best = candidate.config_gen;
                strcpy(s->scope, scope); strcpy(s->instance, instance); strcpy(selected_hash, digest); recovered = 1;
            }
        }
        gnb_utc_destroy(&candidate);
    }
    if ((present || (address_families & GNB_UTC_REQUIRED)) && !recovered) goto failure;
    if (s->ur_server) {
        size_t j;
        for (j = 0; j < s->table.count; ++j)
            if (s->table.entries[j].policy.mode == GNB_UTC_EXPORT) goto failure;
    }
    if (recovered) { s->managed = 1; strcpy(s->reason, "RECOVERED_BLOCKED"); }
#ifndef _WIN32
    if (s->lock_fd >= 0) update_identity(s, buffer);
#endif
    free(buffer);
    gnb_utc_service_poll(s); /* Invalid initial config must not become default direct. */
    if (!s->managed && strcmp(s->reason, "NEVER_ENABLED")) { gnb_utc_service_close(s); return NULL; }
    return s;
failure:
    free(buffer); gnb_utc_service_close(s); return NULL;
}
void gnb_utc_service_close(gnb_utc_service_t *s) {
    if (!s) return;
#ifndef _WIN32
    if (s->lock_fd >= 0) close(s->lock_fd);
#endif
    gnb_utc_destroy(&s->table); free(s);
}
int gnb_utc_service_poll(gnb_utc_service_t *s) {
    char *buffer, digest[129], scope[64], instance[64]; gnb_utc_table_t candidate;
    uint64_t tick = ticks(); int result = -1, r;
    if (!s || !tick) return -1;
    if (s->last_poll == tick) return 0;
    s->last_poll = tick; buffer = malloc(GNB_UTC_TEXT_MAX);
    if (!buffer) return -1;
    if (s->fault) { strcpy(s->reason, "RECOVERY_REQUIRED"); goto report; }
    r = read_file(s, "utc.conf", buffer);
    /* Two never-enabled cores can exist before the first file appears. Only
     * the winner of the instance lock may report managed state or prepare
     * activation; the never-enabled branch below writes disclose-only status. */
    if (r != 1 && take_lock(s)) {
        strcpy(s->reason, "WRITER_CONFLICT"); free(buffer); return -1;
    }
    if (r != 1 && s->identity_pending) {
        /* This core just became the authoritative writer: stamp the identity
         * the agent binds to, superseding any never-enabled peer's status. */
        char *scratch = malloc(GNB_UTC_TEXT_MAX);
        if (scratch) { update_identity(s, scratch); free(scratch); s->identity_pending = 0; }
    }
    if (r == 1) {
        if (s->managed) { block_exports(&s->table); strcpy(s->reason, "CONFIG_MISSING"); }
        else {
            /* Never-enabled disclose-only status: the agent must learn this
             * boot/PID before it can publish an identity-bound utc-v2. This
             * core is not an authoritative managed writer yet, so failure is
             * ignored and routing stays native. */
            status(s, buffer); free(buffer); return 0;
        }
        goto report;
    }
    if (r) { strcpy(s->reason, "CONFIG_INVALID"); goto report; }
    hash(buffer, digest);
    if (!strcmp(digest, s->config_hash)) { result = 0; goto report; }
    if (gnb_utc_init(&candidate)) { strcpy(s->reason, "RESOURCE_EXHAUSTED"); goto report; }
    copy_table(&candidate, &s->table);
    r = config(s, buffer, &candidate, scope, instance);
    if (r) {
        const char *reason = "CONFIG_INVALID";
        if (r == GNB_UTC_REWIND) reason = "VERSION_REWIND";
        else if (r == GNB_UTC_CONFLICT) reason = "GENERATION_CONFLICT";
        else if (r == GNB_UTC_CAPACITY) reason = "CAPACITY_EXCEEDED";
        else if (r == GNB_UTC_UNSUPPORTED) reason = "UNSUPPORTED";
        else if (r == GNB_UTC_SCOPE) reason = "SCOPE_MISMATCH";
        strcpy(s->reason, reason); gnb_utc_destroy(&candidate); goto report;
    }
    if (candidate.config_gen == s->table.config_gen) {
        strcpy(s->config_hash, digest); result = 0; gnb_utc_destroy(&candidate); goto report;
    }
    if (take_lock(s)) { strcpy(s->reason, "WRITER_CONFLICT"); gnb_utc_destroy(&candidate); goto report; }
    strcpy(s->scope, scope); strcpy(s->instance, instance);
    /* Sentinel is never removed, including after explicit direct. */
    if (encode(s, &candidate, buffer) || write_file(s, "utc.managed", buffer) || persist(s, &candidate, buffer)) {
        s->fault = 1; strcpy(s->reason, "STORAGE_FAILED");
        block_changed_exports(&s->table, &candidate); gnb_utc_destroy(&candidate); goto report;
    }
    /* Serialize publishing without doing disk I/O under the packet-path lock. */
    pthread_mutex_lock(&s->table.lock);
    { size_t i, j;
      for (i = 0; i < candidate.count; ++i)
          for (j = 0; j < s->table.count; ++j)
              if (candidate.entries[i].policy.peer_id == s->table.entries[j].policy.peer_id &&
                  candidate.entries[i].policy.pair_gen == s->table.entries[j].policy.pair_gen &&
                  s->table.entries[j].blocked) candidate.entries[i].blocked = 1;
    }
    s->table.config_gen = candidate.config_gen; s->table.count = candidate.count;
    s->table.snapshot_count = candidate.snapshot_count;
    memcpy(s->table.entries, candidate.entries, sizeof(candidate.entries));
    memcpy(s->table.snapshot_peers, candidate.snapshot_peers, sizeof(candidate.snapshot_peers));
    pthread_mutex_unlock(&s->table.lock);
    s->managed = 1; s->fault = 0; strcpy(s->config_hash, digest); strcpy(s->reason, "APPLIED"); result = 0;
    gnb_utc_destroy(&candidate);
report:
    if (status(s, buffer)) { strcpy(s->reason, "STATUS_FAILED"); result = -1; }
    free(buffer); return result;
}
