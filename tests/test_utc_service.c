#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#include "gnb_utc_service.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
static char directory[1024];
static void name(const char *suffix, char file[1200]) { snprintf(file, 1200, "%s/%s", directory, suffix); }
static void write_text(const char *suffix, const char *text) {
    char file[1200]; FILE *f; name(suffix, file); f = fopen(file, "w"); assert(f);
    assert(fputs(text, f) >= 0); assert(!fclose(f)); assert(!chmod(file, 0600));
}
static void conf(unsigned gen, unsigned pairgen, int mode) {
    char text[2048];
    snprintf(text, sizeof(text), "utc-v1 static test 42 %u\npeer 7 8 %u %d %llu %d %s %d 0 %s\n",
        gen, pairgen, mode, mode ? (unsigned long long)time(NULL)+1000 : 0,
        mode ? 4 : 0, mode ? "7f000001000000000000000000000000" : "00000000000000000000000000000000",
        mode ? 9000 : 0, "0000000000000000000000000000000000000000000000000000000000000001");
    write_text("utc.conf", text);
}
static void remove_file(const char *suffix) { char file[1200]; name(suffix, file); assert(!unlink(file)); }
static gnb_utc_action_t action(gnb_utc_service_t *s, uint64_t peer) {
    gnb_utc_endpoint_t e; return gnb_utc_route(&s->table, peer, time(NULL), 1, &e);
}
static int poll(gnb_utc_service_t *s) { s->last_poll = 0; return gnb_utc_service_poll(s); }
int main(void) {
    char template[] = "/tmp/gnb-utc-service-XXXXXX", boot[33]; gnb_utc_service_t *s;
    char file[1200]; int i;
    assert(mkdtemp(template)); strcpy(directory, template);
    assert(gnb_utc_service_has_state(directory) == 0);
    assert(!gnb_utc_service_open(directory, 42, 1 | GNB_UTC_REQUIRED));
    s = gnb_utc_service_open(directory, 42, 1); assert(s); assert(!s->managed);
    assert(s->process_id == (uint64_t)getpid());
    {
        gnb_utc_service_t *other = gnb_utc_service_open(directory, 42, 1);
        char first[512]; FILE *status_file;
        assert(other); /* both were native before the first projection */
        conf(1, 1, 1); assert(!poll(s)); assert(s->managed);
        assert(poll(other) < 0); assert(!strcmp(other->reason, "WRITER_CONFLICT"));
        name("utc.status", file); status_file = fopen(file, "r"); assert(status_file);
        assert(fgets(first, sizeof(first), status_file)); fclose(status_file);
        assert(strstr(first, s->boot_id) && !strstr(first, other->boot_id));
        gnb_utc_service_close(other);
    }
    assert(!gnb_utc_service_open(directory, 42, 1)); /* exclusive managed writer */
    assert(action(s, 7) == GNB_UTC_FORWARD); assert(action(s, 9) == GNB_UTC_NATIVE);
    assert(gnb_utc_service_has_state(directory) == 1);
    strcpy(boot, s->boot_id);
    write_text("utc.conf", "utc-v1 static test 42 2\nunknown key\n");
    assert(poll(s) < 0); assert(action(s, 7) == GNB_UTC_FORWARD);
    remove_file("utc.conf"); assert(poll(s) < 0); assert(action(s, 7) == GNB_UTC_BLOCK);
    conf(1, 1, 1); assert(!poll(s)); assert(action(s, 7) == GNB_UTC_BLOCK);
    conf(2, 2, 1); assert(!poll(s)); assert(action(s, 7) == GNB_UTC_FORWARD);
    write_text("utc.conf", "utc-v1 static test 42 3\npeer 7 8 3 1 4000000000 6 00000000000000000000000000000001 9000 0 0000000000000000000000000000000000000000000000000000000000000001\n");
    assert(poll(s) < 0); assert(s->table.config_gen == 2);
    assert(action(s, 7) == GNB_UTC_FORWARD); /* unsupported family is not APPLIED */
    conf(2, 2, 1);
    gnb_utc_service_close(s);
    assert(!gnb_utc_service_open(directory, 42, 1 | GNB_UTC_UR_SERVER));
    write_text("utc.state.1", "corrupted\n");
    s = gnb_utc_service_open(directory, 42, 1 | GNB_UTC_REQUIRED); assert(s); assert(strcmp(boot, s->boot_id));
    assert(action(s, 7) == GNB_UTC_BLOCK); assert(action(s, 9) == GNB_UTC_NATIVE);
    conf(3, 3, 1); assert(!poll(s)); assert(action(s, 7) == GNB_UTC_FORWARD);
    conf(2, 2, 0); assert(poll(s) < 0); assert(action(s, 7) == GNB_UTC_FORWARD);
    conf(4, 4, 0); assert(!poll(s)); assert(action(s, 7) == GNB_UTC_NATIVE);
    gnb_utc_service_close(s);
    s = gnb_utc_service_open(directory, 42, 1 | GNB_UTC_UR_SERVER); assert(s); assert(action(s, 7) == GNB_UTC_NATIVE);
    conf(5, 5, 1); assert(poll(s) < 0); assert(s->table.config_gen == 4);
    gnb_utc_service_close(s);
    conf(4, 4, 0);
    s = gnb_utc_service_open(directory, 42, 1); assert(s); assert(action(s, 7) == GNB_UTC_NATIVE);
    /* Force replica 1 rename failure after replica 0 was made durable. Never
     * publish the candidate or accept more updates until restart recovery. */
    remove_file("utc.state.1"); name("utc.state.1", file); assert(!mkdir(file, 0700));
    conf(5, 5, 1); assert(poll(s) < 0); assert(s->fault);
    assert(action(s, 7) == GNB_UTC_NATIVE); /* previous explicit release */
    conf(6, 6, 1); assert(poll(s) < 0); assert(s->table.config_gen == 4);
    gnb_utc_service_close(s);
    assert(!rmdir(file));
    /* Newest pre-activation state survives; disk state alone cannot activate. */
    remove_file("utc.conf");
    s = gnb_utc_service_open(directory, 42, 1); assert(s); assert(s->table.config_gen == 5);
    assert(action(s, 7) == GNB_UTC_BLOCK); assert(action(s, 9) == GNB_UTC_NATIVE);
    conf(4, 4, 0); assert(poll(s) < 0); assert(action(s, 7) == GNB_UTC_BLOCK);
    conf(6, 6, 1); assert(!poll(s)); assert(action(s, 7) == GNB_UTC_FORWARD);
    gnb_utc_service_close(s);
    for (i = 0; i < 3; ++i) { snprintf(file, sizeof(file), "utc.state.%d", i); write_text(file, "bad\n"); }
    write_text("utc.managed", "corrupt\n");
    assert(!gnb_utc_service_open(directory, 42, 1)); /* no trusted state remains */
    for (i = 0; i < 3; ++i) { snprintf(file, sizeof(file), "utc.state.%d", i); remove_file(file); }
    remove_file("utc.managed"); remove_file("utc.conf"); remove_file("utc.status"); remove_file("utc.lock");
    assert(!rmdir(directory));
    puts("UTC config/restart/missing-file/single-replica corruption/release/disaster: PASS");
    return 0;
}
