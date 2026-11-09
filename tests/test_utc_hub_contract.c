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
        char *hex = strstr(text, "abababab");
        size_t i;
        assert(hex);
        for (i = 0; i < 8; ++i) hex[i] = (char)('A' + (i % 2));
        assert(gnb_utc_hub_parse(text, length, &parsed, error, sizeof(error)) != 0);
    }
    {
        char *hex = strstr(text, "cdcdcdcd");
        assert(hex);
        hex[0] = 'C';
        assert(gnb_utc_hub_parse(text, length, &parsed, error, sizeof(error)) != 0);
    }
    {
        char *hex = strstr(text, "00000000000000000000ffff");
        assert(hex);
        hex[20] = 'F';
        assert(gnb_utc_hub_parse(text, length, &parsed, error, sizeof(error)) != 0);
    }
    {
        sample_projection(&projection);
        projection.peers[0].family = 6;
        assert(gnb_utc_hub_format(&projection, text, sizeof(text), &length) != 0);
    }
    {
        sample_projection(&projection);
        memset(projection.peers[0].address, 0, 16);
        assert(gnb_utc_hub_format(&projection, text, sizeof(text), &length) != 0);
    }
    {
        sample_projection(&projection);
        projection.peers[0].address[12] = 224;
        assert(gnb_utc_hub_format(&projection, text, sizeof(text), &length) != 0);
    }
    {
        sample_projection(&projection);
        projection.peers[0].family = 6;
        memset(projection.peers[0].address, 0, 16);
        projection.peers[0].address[0] = 0xff;
        projection.peers[0].address[1] = 0x02;
        projection.peers[0].address[15] = 1;
        assert(gnb_utc_hub_format(&projection, text, sizeof(text), &length) != 0);
    }
    {
        sample_projection(&projection);
        projection.peers[0].family = 6;
        memset(projection.peers[0].address, 0, 16);
        projection.peers[0].address[0] = 0x20;
        projection.peers[0].address[1] = 0x01;
        projection.peers[0].address[15] = 7;
        assert(gnb_utc_hub_format(&projection, text, sizeof(text), &length) == 0);
        assert(gnb_utc_hub_parse(text, length, &parsed, error, sizeof(error)) == 0);
    }
    puts("PASS utc-v2 rejects uppercase hex, mapped, unspecified and multicast addresses");

    {
        const char *fixture = getenv("HUB_V2_FIXTURE");
        if (!fixture || !*fixture) {
            static const char fallback[] =
                "../../mynet_utc/tests/fixtures/hub_v2/projection.txt";
            FILE *probe = fopen(fallback, "rb");
            if (probe) {
                fclose(probe);
                fixture = fallback;
            }
        }
        if (!fixture || !*fixture) {
            fprintf(stderr, "HUB_V2_FIXTURE is required; the cross-language "
                            "golden contract must not be skipped\n");
            return 1;
        }
        {
            FILE *file = fopen(fixture, "rb");
            char golden[4096];
            size_t golden_length;
            if (!file) {
                fprintf(stderr, "cannot open HUB_V2_FIXTURE: %s\n", fixture);
                return 1;
            }
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
    {
        const char *fixture = getenv("HUB_V2_NEGATIVES");
        if (!fixture || !*fixture) {
            static const char fallback[] =
                "../../mynet_utc/tests/fixtures/hub_v2/negatives.txt";
            FILE *probe = fopen(fallback, "rb");
            if (probe) {
                fclose(probe);
                fixture = fallback;
            }
        }
        if (!fixture || !*fixture) {
            fprintf(stderr, "HUB_V2_NEGATIVES is required; the cross-language "
                            "negative contract must not be skipped\n");
            return 1;
        }
        {
            FILE *file = fopen(fixture, "rb");
            char *cases;
            long size;
            size_t offset = 0;
            unsigned int index;
            if (!file) {
                fprintf(stderr, "cannot open HUB_V2_NEGATIVES: %s\n", fixture);
                return 1;
            }
            assert(fseek(file, 0, SEEK_END) == 0);
            size = ftell(file);
            assert(size > 0 && size < 65536);
            assert(fseek(file, 0, SEEK_SET) == 0);
            cases = malloc((size_t)size);
            assert(cases);
            assert(fread(cases, 1, (size_t)size, file) == (size_t)size);
            assert(!fclose(file));
            for (index = 0; offset < (size_t)size; ++index) {
                size_t end = offset;
                size_t case_length;
                while (end < (size_t)size) {
                    if (cases[end] == '\n' && end + 1U < (size_t)size &&
                        cases[end + 1U] == '\n')
                        break;
                    ++end;
                }
                case_length = end < (size_t)size ? end - offset + 1U
                                                : end - offset;
                if (gnb_utc_hub_parse(cases + offset, case_length, &parsed,
                                      error, sizeof(error)) == 0) {
                    fprintf(stderr, "negative fixture case %u was accepted\n",
                            index);
                    assert(0);
                }
                offset = end + 2U;
            }
            free(cases);
            assert(index >= 13);
            puts("PASS utc-v2 cross-language negative fixture");
        }
    }
    return 0;
}
