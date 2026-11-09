#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#include "gnb_utc_service.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

static char directory[1024];
static char manifest[65];
static void name(const char *suffix, char file[1200]) { snprintf(file, 1200, "%s/%s", directory, suffix); }
static void write_text(const char *suffix, const char *text) {
    char file[1200]; FILE *f; name(suffix, file); f = fopen(file, "w"); assert(f);
    assert(fputs(text, f) >= 0); assert(!fclose(f)); assert(!chmod(file, 0600));
}
static int poll(gnb_utc_service_t *s) { s->last_poll = 0; return gnb_utc_service_poll(s); }
static gnb_utc_action_t route(gnb_utc_service_t *s, uint64_t peer, gnb_utc_endpoint_t *endpoint) {
    return gnb_utc_route(&s->table, peer, time(NULL), 1, endpoint);
}
static void v1_text(char *out, size_t capacity, uint64_t gen, unsigned mode, unsigned port) {
    int n = snprintf(out, capacity,
        "utc-v1 hub.scope hub-instance 42 %" PRIu64 "\npeer 7 8 1 %u %" PRIu64
        " 4 7f000001000000000000000000000000 %u 0 %s\n",
        gen, mode, mode ? (uint64_t)time(NULL) + 1000 : (uint64_t)0, port, manifest);
    assert(n > 0 && (size_t)n < capacity);
}
static void peer_row(char *out, size_t capacity, uint64_t peer_id, uint64_t pair_id,
                     uint64_t pair_gen, unsigned mode, uint64_t lease_epoch,
                     uint64_t placement_epoch, uint64_t expires_at, unsigned family,
                     const char *address, unsigned port, unsigned scope_id) {
    int n = snprintf(out, capacity,
        "peer %" PRIu64 " %" PRIu64 " %" PRIu64 " %u %" PRIu64 " %" PRIu64 " %" PRIu64
        " %u %s %u %u %s\n",
        peer_id, pair_id, pair_gen, mode, lease_epoch, placement_epoch, expires_at,
        family, address, port, scope_id, manifest);
    assert(n > 0 && (size_t)n < capacity);
}
static size_t v2_text(char *out, size_t capacity, const char *scope, const char *instance,
                      uint64_t local_id, uint64_t config_gen, const char *boot,
                      uint64_t process_id, unsigned capabilities, const char *peers) {
    int n = snprintf(out, capacity, "utc-v2 %s %s %" PRIu64 " %" PRIu64 " %s %" PRIu64 " %u\n%s",
        scope, instance, local_id, config_gen, boot, process_id, capabilities, peers);
    assert(n > 0 && (size_t)n < capacity);
    return (size_t)n;
}
static void status_gen(gnb_utc_service_t *s, uint64_t gen) {
    char file[1200], line[1024], *save, *word; FILE *f;
    name("utc.status", file); f = fopen(file, "r"); assert(f);
    assert(fgets(line, sizeof(line), f)); fclose(f);
    word = strtok_r(line, " \t\r\n", &save); assert(word && !strcmp(word, "status-v1"));
    word = strtok_r(NULL, " \t\r\n", &save); assert(word && !strcmp(word, "hub.scope"));
    word = strtok_r(NULL, " \t\r\n", &save); assert(word && !strcmp(word, "hub-instance"));
    word = strtok_r(NULL, " \t\r\n", &save); assert(word && strtoull(word, NULL, 10) == 42);
    word = strtok_r(NULL, " \t\r\n", &save); assert(word && !strcmp(word, s->boot_id));
    word = strtok_r(NULL, " \t\r\n", &save); assert(word && strtoull(word, NULL, 10) == gen);
}
static void status_contains(const char *needle) {
    char file[1200], text[16384]; FILE *f; size_t n;
    name("utc.status", file); f = fopen(file, "r"); assert(f);
    n = fread(text, 1, sizeof(text) - 1, f); assert(!ferror(f)); fclose(f); text[n] = 0;
    assert(strstr(text, needle));
}
static void cleanup(const char *dir) {
    struct dirent *entry; char file[1200]; DIR *d = opendir(dir); assert(d);
    while ((entry = readdir(d))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        snprintf(file, sizeof(file), "%s/%s", dir, entry->d_name); assert(!unlink(file));
    }
    closedir(d); assert(!rmdir(dir));
}
/* A rejected projection must leave generation, entries and routing untouched. */
static void rejected(gnb_utc_service_t *s, const char *text, const char *reason, unsigned port) {
    gnb_utc_endpoint_t endpoint;
    uint64_t gen = s->table.config_gen; size_t count = s->table.count;
    write_text("utc.conf", text);
    assert(poll(s) < 0);
    assert(!strcmp(s->reason, reason));
    assert(s->table.config_gen == gen && s->table.count == count);
    assert(route(s, 7, &endpoint) == GNB_UTC_FORWARD);
    assert(endpoint.port == port);
}
int main(void) {
    char template[] = "/tmp/gnb-utc-hub-service-XXXXXX";
    char text[8192], rows[4096], row[512];
    char good[8192], peer9[1024];
    char boot[33];
    gnb_utc_service_t *s; gnb_utc_endpoint_t endpoint;
    uint64_t expires = (uint64_t)time(NULL) + 1000;
    char address4[] = "00000000000000000000ffffcb007107";
    char address4_alt[] = "00000000000000000000ffffcb007108";
    char address6[] = "20010db8000000000000000000000001";
    char address0[] = "00000000000000000000000000000000";
    static const uint8_t zero[12] = {0};

    memset(manifest, 'a', 64); manifest[64] = 0;
    assert(mkdtemp(template)); strcpy(directory, template);
    s = gnb_utc_service_open(directory, 42, 1); assert(s); assert(!s->managed);
    strcpy(boot, s->boot_id);
    status_contains(boot); /* never-enabled status exposes boot/PID for v2 binding */

    /* utc-v1 keeps its path: apply an export, then verify a broken v2 cannot
     * replace the live table. */
    v1_text(text, sizeof(text), 1, 1, 9000); write_text("utc.conf", text);
    assert(!poll(s)); assert(s->managed && s->table.config_gen == 1);
    assert(route(s, 7, &endpoint) == GNB_UTC_FORWARD);
    assert(endpoint.port == 9000 && endpoint.address[0] == 127 && endpoint.address[3] == 1);
    status_gen(s, 1); status_contains("policy 7 8 1 1 ");

    peer_row(row, sizeof(row), 7, 8, 2, 1, 2, 2, expires, 4, address4, 9555, 0);
    v2_text(text, sizeof(text), "hub.scope", "hub-instance", 42, 2, "00000000000000000000000000000000",
            s->process_id, 3, row);
    rejected(s, text, "CONFIG_INVALID", 9000);

    peer_row(row, sizeof(row), 7, 8, 2, 1, 2, 2, expires, 4, address4, 9555, 0);
    v2_text(text, sizeof(text), "hub.scope", "hub-instance", 42, 2, boot, s->process_id + 1, 3, row);
    rejected(s, text, "CONFIG_INVALID", 9000);

    peer_row(row, sizeof(row), 7, 8, 2, 1, 2, 2, expires, 4, address4, 9555, 0);
    v2_text(text, sizeof(text), "hub.scope", "hub-instance", 43, 2, boot, s->process_id, 3, row);
    rejected(s, text, "CONFIG_INVALID", 9000);

    v2_text(text, sizeof(text), "other.scope", "hub-instance", 42, 2, boot, s->process_id, 3, row);
    rejected(s, text, "SCOPE_MISMATCH", 9000);

    /* Valid projection: IPv4 endpoint is ::ffff-mapped on the wire and must be
     * stored as the local first-four-bytes form; direct peer stays native. */
    peer_row(row, sizeof(row), 7, 8, 2, 1, 2, 2, expires, 4, address4, 9555, 0);
    peer_row(peer9, sizeof(peer9), 9, 10, 1, 0, 1, 1, 0, 0, address0, 0, 0);
    snprintf(rows, sizeof(rows), "%s%s", row, peer9);
    v2_text(good, sizeof(good), "hub.scope", "hub-instance", 42, 2, boot, s->process_id, 3, rows);
    strcpy(text, good); write_text("utc.conf", text);
    assert(!poll(s)); assert(s->table.config_gen == 2);
    assert(route(s, 7, &endpoint) == GNB_UTC_FORWARD);
    assert(endpoint.family == 4 && endpoint.port == 9555 && endpoint.address[0] == 203 &&
           endpoint.address[1] == 0 && endpoint.address[2] == 113 && endpoint.address[3] == 7);
    assert(!memcmp(endpoint.address + 4, zero, 12));
    assert(route(s, 9, &endpoint) == GNB_UTC_NATIVE);
    status_gen(s, 2); status_contains("policy 7 8 2 1 "); status_contains("policy 9 10 1 0 ");

    /* Non-canonical or invalid content is rejected by the strict parser. */
    peer_row(row, sizeof(row), 7, 8, 3, 1, 3, 3, expires, 4, address4_alt, 9666, 0);
    peer_row(peer9, sizeof(peer9), 9, 10, 1, 0, 1, 1, 0, 0, address0, 0, 0);
    snprintf(rows, sizeof(rows), "%s%s", peer9, row); /* unsorted */
    v2_text(text, sizeof(text), "hub.scope", "hub-instance", 42, 3, boot, s->process_id, 3, rows);
    rejected(s, text, "CONFIG_INVALID", 9555);

    peer_row(row, sizeof(row), 7, 8, 3, 1, 3, 3, expires, 4, address4_alt, 9666, 0);
    peer_row(peer9, sizeof(peer9), 11, 8, 1, 0, 1, 1, 0, 0, address0, 0, 0); /* duplicate pair */
    snprintf(rows, sizeof(rows), "%s%s", row, peer9);
    v2_text(text, sizeof(text), "hub.scope", "hub-instance", 42, 3, boot, s->process_id, 3, rows);
    rejected(s, text, "CONFIG_INVALID", 9555);

    peer_row(row, sizeof(row), 7, 8, 3, 1, 3, 3, expires, 4, "00000000000000000000ffff00000000", 9666, 0);
    v2_text(text, sizeof(text), "hub.scope", "hub-instance", 42, 3, boot, s->process_id, 3, row);
    rejected(s, text, "CONFIG_INVALID", 9555);
    peer_row(row, sizeof(row), 7, 8, 3, 1, 3, 3, expires, 4, "00000000000000000000ffffe0000001", 9666, 0);
    v2_text(text, sizeof(text), "hub.scope", "hub-instance", 42, 3, boot, s->process_id, 3, row);
    rejected(s, text, "CONFIG_INVALID", 9555);

    peer_row(row, sizeof(row), 7, 8, 3, 1, 0, 0, expires, 4, address4_alt, 9666, 0);
    v2_text(text, sizeof(text), "hub.scope", "hub-instance", 42, 3, boot, s->process_id, 3, row);
    rejected(s, text, "CONFIG_INVALID", 9555);
    peer_row(row, sizeof(row), 7, 8, 3, 1, 3, 0, expires, 4, address4_alt, 9666, 0);
    v2_text(text, sizeof(text), "hub.scope", "hub-instance", 42, 3, boot, s->process_id, 3, row);
    rejected(s, text, "CONFIG_INVALID", 9555);
    peer_row(row, sizeof(row), 7, 8, 3, 1, 3, 3, expires, 4, address4_alt, 9666, 0);
    v2_text(text, sizeof(text), "hub.scope", "hub-instance", 42, 0, boot, s->process_id, 3, row);
    rejected(s, text, "CONFIG_INVALID", 9555);
    peer_row(row, sizeof(row), 7, 8, 3, 1, 3, 3, 0, 4, address4_alt, 9666, 0);
    v2_text(text, sizeof(text), "hub.scope", "hub-instance", 42, 3, boot, s->process_id, 3, row);
    rejected(s, text, "CONFIG_INVALID", 9555);
    peer_row(row, sizeof(row), 42, 8, 3, 1, 3, 3, expires, 4, address4_alt, 9666, 0);
    v2_text(text, sizeof(text), "hub.scope", "hub-instance", 42, 3, boot, s->process_id, 3, row);
    rejected(s, text, "CONFIG_INVALID", 9555);
    peer_row(row, sizeof(row), 7, 8, 3, 1, 3, 3, expires, 4, address4_alt, 9666, 0);
    v2_text(text, sizeof(text), "hub.scope", "hub-instance", 42, 3, boot, s->process_id, 0, row);
    rejected(s, text, "CONFIG_INVALID", 9555);

    peer_row(row, sizeof(row), 7, 8, 3, 1, 3, 3, expires, 4, address4_alt, 9666, 0);
    v2_text(text, sizeof(text), "hub.scope", "hub-instance", 42, 3, boot, s->process_id, 3, row);
    text[strlen(text) - 1] = 0; /* truncated: canonical text always ends with \n */
    rejected(s, text, "CONFIG_INVALID", 9555);

    /* Generation rewind and same-generation content change are rejected. */
    v2_text(text, sizeof(text), "hub.scope", "hub-instance", 42, 1, boot, s->process_id, 3, row);
    write_text("utc.conf", text); assert(poll(s) < 0);
    assert(!strcmp(s->reason, "VERSION_REWIND") && s->table.config_gen == 2);
    assert(route(s, 7, &endpoint) == GNB_UTC_FORWARD && endpoint.port == 9555);
    v2_text(text, sizeof(text), "hub.scope", "hub-instance", 42, 2, boot, s->process_id, 3, row);
    write_text("utc.conf", text); assert(poll(s) < 0);
    assert(!strcmp(s->reason, "GENERATION_CONFLICT") && s->table.config_gen == 2);
    assert(route(s, 7, &endpoint) == GNB_UTC_FORWARD && endpoint.port == 9555);
    status_gen(s, 2); status_contains("policy 7 8 2 1 ");

    /* Dropping an export peer blocks it (v1 omission semantics); a newer
     * pair_gen revives it. */
    peer_row(peer9, sizeof(peer9), 9, 10, 1, 0, 1, 1, 0, 0, address0, 0, 0);
    v2_text(text, sizeof(text), "hub.scope", "hub-instance", 42, 3, boot, s->process_id, 3, peer9);
    write_text("utc.conf", text); assert(!poll(s)); assert(s->table.config_gen == 3);
    assert(route(s, 7, &endpoint) == GNB_UTC_BLOCK);
    assert(route(s, 9, &endpoint) == GNB_UTC_NATIVE);
    peer_row(row, sizeof(row), 7, 8, 3, 1, 3, 3, expires, 4, address4_alt, 9666, 0);
    snprintf(rows, sizeof(rows), "%s%s", row, peer9);
    v2_text(text, sizeof(text), "hub.scope", "hub-instance", 42, 4, boot, s->process_id, 3, rows);
    write_text("utc.conf", text); assert(!poll(s)); assert(s->table.config_gen == 4);
    assert(route(s, 7, &endpoint) == GNB_UTC_FORWARD);
    assert(endpoint.port == 9666 && endpoint.address[3] == 8);
    status_gen(s, 4);
    gnb_utc_service_close(s);
    cleanup(directory);

    /* Export peers need a locally enabled family; a v2 cannot bypass it. */
    assert(mkdtemp(template)); strcpy(directory, template);
    s = gnb_utc_service_open(directory, 42, 1); assert(s);
    strcpy(boot, s->boot_id);
    peer_row(row, sizeof(row), 7, 8, 1, 1, 1, 1, expires, 6, address6, 9443, 0);
    v2_text(text, sizeof(text), "hub.scope", "hub-instance", 42, 1, boot, s->process_id, 3, row);
    write_text("utc.conf", text); assert(poll(s) < 0);
    assert(!strcmp(s->reason, "UNSUPPORTED") && !s->managed && s->table.config_gen == 0);
    gnb_utc_service_close(s);
    cleanup(directory);

    assert(mkdtemp(template)); strcpy(directory, template);
    s = gnb_utc_service_open(directory, 42, 1 | GNB_UTC_UR_SERVER); assert(s);
    strcpy(boot, s->boot_id);
    peer_row(row, sizeof(row), 7, 8, 1, 1, 1, 1, expires, 4, address4, 9443, 0);
    v2_text(text, sizeof(text), "hub.scope", "hub-instance", 42, 1, boot, s->process_id, 3, row);
    write_text("utc.conf", text); assert(poll(s) < 0);
    assert(!strcmp(s->reason, "UNSUPPORTED") && s->table.config_gen == 0);
    gnb_utc_service_close(s);
    cleanup(directory);
    puts("UTC utc-v1 continuity and utc-v2 apply/reject/fail-closed: PASS");
    return 0;
}
