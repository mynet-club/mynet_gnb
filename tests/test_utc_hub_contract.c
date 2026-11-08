#include "gnb_utc_hub.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void sample_peer(gnb_utc_hub_peer_t *peer, uint64_t peer_id, uint64_t pair_id) {
    memset(peer, 0, sizeof(*peer));
    peer->peer_id = peer_id;
    peer->pair_id = pair_id;
    peer->pair_gen = 2;
    peer->mode = 1;
    peer->lease_epoch = 7;
    peer->placement_epoch = 3;
    peer->expires_at = 200;
    peer->family = 4;
    peer->address[10] = 0xff;
    peer->address[11] = 0xff;
    peer->address[12] = 203;
    peer->address[13] = 0;
    peer->address[14] = 113;
    peer->address[15] = 7;
    peer->port = 9443;
    strcpy(peer->manifest,
           "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd");
}

static void sample_projection(gnb_utc_hub_projection_t *projection) {
    memset(projection, 0, sizeof(*projection));
    strcpy(projection->scope, "scope.test");
    strcpy(projection->instance, "instance-a");
    strcpy(projection->boot_id, "abababababababababababababababab");
    projection->local_id = 10;
    projection->config_gen = 5;
    projection->process_id = 123;
    projection->capabilities = GNB_UTC_HUB_CAP_EPOCH | GNB_UTC_HUB_CAP_EXPLICIT_ACTIVATE;
    projection->count = 1;
    sample_peer(&projection->peers[0], 20, 30);
}

int main(void) {
    gnb_utc_hub_projection_t projection;
    gnb_utc_hub_projection_t parsed;
    char text[4096], changed[4096], error[128];
    size_t length = 0;

    sample_projection(&projection);
    assert(gnb_utc_hub_format(&projection, text, sizeof(text), &length) == 0);
    assert(strncmp(text, "utc-v2 scope.test instance-a 10 5 ",
                   strlen("utc-v2 scope.test instance-a 10 5 ")) == 0);
    assert(gnb_utc_hub_parse(text, length, &parsed, error, sizeof(error)) == 0);
    assert(parsed.count == 1 && parsed.peers[0].lease_epoch == 7);
    assert(parsed.peers[0].placement_epoch == 3 && parsed.peers[0].port == 9443);
    assert(parsed.capabilities == 3);
    puts("PASS utc-v2 projection canonical format and parse");

    projection.count = 2;
    sample_peer(&projection.peers[0], 30, 40);
    sample_peer(&projection.peers[1], 20, 50);
    assert(gnb_utc_hub_format(&projection, text, sizeof(text), &length) == 0);
    assert(strstr(text, "peer 20 50") < strstr(text, "peer 30 40"));
    assert(gnb_utc_hub_parse(text, length, &parsed, error, sizeof(error)) == 0);
    assert(parsed.peers[0].peer_id == 20 && parsed.peers[1].peer_id == 30);
    puts("PASS utc-v2 peer ordering is canonical");

    sample_projection(&projection);
    projection.peers[0].mode = 0;
    assert(gnb_utc_hub_format(&projection, text, sizeof(text), &length) != 0);
    sample_projection(&projection);
    projection.capabilities = 4;
    assert(gnb_utc_hub_format(&projection, text, sizeof(text), &length) != 0);
    sample_projection(&projection);
    projection.peers[0].peer_id = projection.local_id;
    assert(gnb_utc_hub_format(&projection, text, sizeof(text), &length) != 0);
    sample_projection(&projection);
    projection.count = 2;
    projection.peers[1] = projection.peers[0];
    projection.peers[1].peer_id = 21;
    assert(gnb_utc_hub_format(&projection, text, sizeof(text), &length) != 0);

    sample_projection(&projection);
    assert(gnb_utc_hub_format(&projection, text, sizeof(text), &length) == 0);
    memcpy(changed, text, length);
    changed[0] = 'x';
    assert(gnb_utc_hub_parse(changed, length, &parsed, error, sizeof(error)) != 0);
    memcpy(changed, text, length);
    changed[5] = 'v';
    changed[6] = '1';
    assert(gnb_utc_hub_parse(changed, length, &parsed, error, sizeof(error)) != 0);
    memcpy(changed, text, length);
    changed[10] = ' ';
    assert(gnb_utc_hub_parse(changed, length, &parsed, error, sizeof(error)) != 0);
    assert(gnb_utc_hub_parse(text, length - 1U, &parsed, error, sizeof(error)) != 0);
    puts("PASS utc-v2 rejects invalid capacity, identity and non-canonical text");

    {
        const char *fixture = getenv("HUB_V2_FIXTURE");
        if (fixture && *fixture) {
            FILE *file = fopen(fixture, "rb");
            char golden[4096];
            size_t golden_length;
            assert(file);
            golden_length = fread(golden, 1, sizeof(golden), file);
            assert(!ferror(file) && !fclose(file));
            assert(golden_length > 0 && golden_length < sizeof(golden));
            assert(gnb_utc_hub_parse(golden, golden_length, &parsed, error,
                                     sizeof(error)) == 0);
            assert(gnb_utc_hub_format(&parsed, text, sizeof(text), &length) == 0);
            assert(length == golden_length);
            assert(memcmp(text, golden, length) == 0);
            puts("PASS utc-v2 cross-language golden fixture");
        }
    }
    return 0;
}
