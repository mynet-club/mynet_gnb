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
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>
static unsigned event_count, stop_at;
static int crash;
int gnb_utc_test_io_event(const char *name, const char *phase) {
    (void)phase;
    /* Best-effort files are outside the managed persistence crash window. */
    if (!strcmp(name, "utc.status") || !strcmp(name, "utc.identity")) return 0;
    if (++event_count != stop_at) return 0;
    if (crash) _exit(73);
    errno = EIO; return 1;
}
static void write_text(const char *dir, const char *name, const char *text) {
    char file[1200]; FILE *f; snprintf(file, sizeof(file), "%s/%s", dir, name);
    f = fopen(file, "w"); assert(f); assert(fputs(text, f) >= 0); assert(!fclose(f)); assert(!chmod(file, 0600));
}
static void config(const char *dir, unsigned gen) {
    char text[2048]; unsigned long long expires = (unsigned long long)time(NULL) + 1000;
    snprintf(text, sizeof(text), "utc-v1 static crash 42 %u\npeer 7 8 %u 1 %llu 4 7f000001000000000000000000000000 9000 0 %064u\n%s",
        gen, gen, expires, 1u, gen == 2 ? "peer 9 10 1 1 4000000000 4 7f000001000000000000000000000000 9001 0 0000000000000000000000000000000000000000000000000000000000000002\n" : "");
    write_text(dir, "utc.conf", text);
}
static gnb_utc_action_t route(gnb_utc_service_t *s, uint64_t peer) {
    gnb_utc_endpoint_t e; return gnb_utc_route(&s->table, peer, time(NULL), 1, &e);
}
static void cleanup(const char *dir) {
    struct dirent *entry; char file[1200]; DIR *d = opendir(dir); assert(d);
    while ((entry = readdir(d))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        snprintf(file, sizeof(file), "%s/%s", dir, entry->d_name); assert(!unlink(file));
    }
    closedir(d); assert(!rmdir(dir));
}
static void unchanged_pair_survives_io_error(void) {
    char dir[] = "/tmp/gnb-utc-pair-isolation-XXXXXX", text[2048];
    gnb_utc_service_t *s;
    assert(mkdtemp(dir)); stop_at = event_count = 0;
    config(dir, 2); s = gnb_utc_service_open(dir, 42, 3); assert(s);
    assert(route(s, 7) == GNB_UTC_FORWARD && route(s, 9) == GNB_UTC_FORWARD);
    snprintf(text, sizeof(text), "utc-v1 static crash 42 3\npeer 7 8 3 1 4000000000 4 7f000001000000000000000000000000 9002 0 %064u\npeer 9 10 1 1 4000000000 4 7f000001000000000000000000000000 9001 0 %064u\n", 1u, 2u);
    write_text(dir, "utc.conf", text);
    event_count = 0; stop_at = 13; crash = 0; s->last_poll = 0;
    assert(gnb_utc_service_poll(s) < 0 && s->fault);
    assert(s->table.config_gen == 2);
    assert(route(s, 7) == GNB_UTC_BLOCK); /* changed pair */
    assert(route(s, 9) == GNB_UTC_FORWARD); /* unchanged UTC pair */
    assert(route(s, 100) == GNB_UTC_NATIVE); /* never managed */
    stop_at = 0; gnb_utc_service_close(s); cleanup(dir);
}
int main(void) {
    unsigned initial, mode, step, replica, cases = 0;
    for (initial = 0; initial < 2; ++initial)
    for (mode = 0; mode < 2; ++mode) for (step = 1; step <= 24; ++step)
    for (replica = 0; replica < 3; ++replica) {
        char dir[] = "/tmp/gnb-utc-crash-XXXXXX", name[32], file[1200]; int status;
        pid_t child; gnb_utc_service_t *s;
        assert(mkdtemp(dir)); event_count = stop_at = 0;
        if (!initial) config(dir, 1);
        s = gnb_utc_service_open(dir, 42, 3); assert(s);
        assert(route(s, 7) == (initial ? GNB_UTC_NATIVE : GNB_UTC_FORWARD));
        gnb_utc_service_close(s);
        child = fork(); assert(child >= 0);
        if (!child) {
            event_count = stop_at = 0; s = gnb_utc_service_open(dir, 42, 3); assert(s);
            config(dir, 2); event_count = 0; stop_at = step; crash = mode;
            s->last_poll = 0;
            assert(gnb_utc_service_poll(s) < 0);
            assert(s->fault); assert(route(s, 7) == (initial ? GNB_UTC_NATIVE : GNB_UTC_BLOCK));
            assert(route(s, 9) == GNB_UTC_NATIVE); /* never published */
            gnb_utc_service_close(s); _exit(74);
        }
        assert(waitpid(child, &status, 0) == child && WIFEXITED(status));
        assert(WEXITSTATUS(status) == (mode ? 73 : 74));
        /* A further single-replica corruption after the interrupted update. */
        snprintf(name, sizeof(name), "utc.state.%u", replica);
        snprintf(file, sizeof(file), "%s/%s", dir, name);
        if (!access(file, F_OK)) write_text(dir, name, "corrupt\n");
        snprintf(file, sizeof(file), "%s/utc.conf", dir); assert(!unlink(file));
        event_count = stop_at = 0; s = gnb_utc_service_open(dir, 42, 3); assert(s);
        assert(route(s, 7) == GNB_UTC_BLOCK || (initial && step <= 4 && route(s, 7) == GNB_UTC_NATIVE));
        assert(route(s, 100) == GNB_UTC_NATIVE);
        assert(route(s, 9) == GNB_UTC_NATIVE || route(s, 9) == GNB_UTC_BLOCK);
        /* Once all three replicas passed fsync, removing one cannot lose gen2. */
        if (step == 24) { assert(s->table.config_gen == 2); assert(route(s, 9) == GNB_UTC_BLOCK); }
        gnb_utc_service_close(s); cleanup(dir); ++cases;
    }
    unchanged_pair_survives_io_error();
    printf("UTC initial/update persistence error/crash x 24 I/O boundaries x single-replica corruption: %u cases PASS\n", cases);
    return 0;
}
