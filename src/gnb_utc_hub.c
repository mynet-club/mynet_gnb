#include "gnb_utc_hub.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int set_error(char *error, size_t capacity, const char *message) {
    if (error && capacity) snprintf(error, capacity, "%s", message);
    return -1;
}

static int token(const char *value) {
    size_t length = strnlen(value, GNB_UTC_HUB_TOKEN_MAX);
    size_t i;
    if (!length || length == GNB_UTC_HUB_TOKEN_MAX) return 0;
    for (i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)value[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            return 0;
    }
    return 1;
}

static int number(const char *value, uint64_t *output) {
    char *end;
    unsigned long long parsed;
    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno || end == value || *end) return -1;
    *output = (uint64_t)parsed;
    return 0;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int decode_hex(const char *value, uint8_t *output, size_t bytes) {
    size_t i;
    if (strlen(value) != bytes * 2U) return -1;
    for (i = 0; i < bytes; ++i) {
        int high = hex_nibble(value[i * 2U]);
        int low = hex_nibble(value[i * 2U + 1U]);
        if (high < 0 || low < 0) return -1;
        output[i] = (uint8_t)((high << 4) | low);
    }
    return 0;
}

static int boot_id_valid(const char *boot_id) {
    uint8_t raw[16];
    return decode_hex(boot_id, raw, sizeof(raw)) == 0;
}

static int split(char *line, char *fields[], size_t capacity) {
    size_t count = 0;
    char *cursor = line;
    while (*cursor) {
        while (*cursor == ' ') ++cursor;
        if (!*cursor) break;
        if (count == capacity) return -1;
        fields[count++] = cursor;
        while (*cursor && *cursor != ' ') ++cursor;
        if (*cursor) *cursor++ = '\0';
    }
    return (int)count;
}

static int direct_address_zero(const uint8_t address[16]) {
    size_t i;
    for (i = 0; i < 16; ++i) if (address[i]) return 0;
    return 1;
}

static int peer_valid(const gnb_utc_hub_peer_t *peer, uint64_t local_id) {
    uint8_t manifest[32];
    size_t i;
    if (!peer->peer_id || peer->peer_id == local_id || !peer->pair_id ||
        !peer->pair_gen || !peer->lease_epoch || !peer->placement_epoch ||
        decode_hex(peer->manifest, manifest, sizeof(manifest)))
        return 0;
    if (peer->mode == 0)
        return peer->family == 0 && peer->port == 0 && peer->scope_id == 0 &&
               direct_address_zero(peer->address);
    if (peer->mode != 1 || !peer->expires_at || !peer->port ||
        (peer->family != 4 && peer->family != 6))
        return 0;
    if (peer->family == 4) {
        if (peer->scope_id) return 0;
        for (i = 0; i < 10; ++i) if (peer->address[i]) return 0;
        if (peer->address[10] != 0xff || peer->address[11] != 0xff) return 0;
    }
    return 1;
}

static int projection_valid(const gnb_utc_hub_projection_t *projection) {
    size_t i, j;
    if (!token(projection->scope) || !token(projection->instance) ||
        !projection->local_id || !projection->config_gen ||
        !boot_id_valid(projection->boot_id) || !projection->process_id ||
        !projection->capabilities || (projection->capabilities & ~GNB_UTC_HUB_CAP_MASK) ||
        projection->count > GNB_UTC_HUB_MAX_PEERS)
        return 0;
    for (i = 0; i < projection->count; ++i) {
        if (!peer_valid(&projection->peers[i], projection->local_id)) return 0;
        for (j = 0; j < i; ++j) {
            if (projection->peers[j].peer_id == projection->peers[i].peer_id ||
                projection->peers[j].pair_id == projection->peers[i].pair_id)
                return 0;
        }
    }
    return 1;
}

static int peer_compare(const void *left, const void *right) {
    const gnb_utc_hub_peer_t *a = left;
    const gnb_utc_hub_peer_t *b = right;
    return a->peer_id < b->peer_id ? -1 : a->peer_id != b->peer_id;
}

int gnb_utc_hub_format(const gnb_utc_hub_projection_t *projection, char *output,
                       size_t capacity, size_t *output_length) {
    gnb_utc_hub_peer_t *peers;
    size_t used, i, j;
    if (!projection || !output || !output_length || !projection_valid(projection)) return -1;
    peers = malloc(projection->count * sizeof(*peers));
    if (!peers && projection->count) return -1;
    if (projection->count) memcpy(peers, projection->peers, projection->count * sizeof(*peers));
    qsort(peers, projection->count, sizeof(*peers), peer_compare);
    used = (size_t)snprintf(output, capacity, "utc-v2 %s %s %llu %llu %s %llu %u\n",
        projection->scope, projection->instance, (unsigned long long)projection->local_id,
        (unsigned long long)projection->config_gen, projection->boot_id,
        (unsigned long long)projection->process_id, projection->capabilities);
    if (used >= capacity) { free(peers); return -1; }
    for (i = 0; i < projection->count; ++i) {
        int added;
        if (i && peers[i].peer_id == peers[i - 1].peer_id) { free(peers); return -1; }
        added = snprintf(output + used, capacity - used,
            "peer %llu %llu %llu %u %llu %llu %llu %u ",
            (unsigned long long)peers[i].peer_id, (unsigned long long)peers[i].pair_id,
            (unsigned long long)peers[i].pair_gen, peers[i].mode,
            (unsigned long long)peers[i].lease_epoch,
            (unsigned long long)peers[i].placement_epoch,
            (unsigned long long)peers[i].expires_at, peers[i].family);
        if (added < 0 || (size_t)added >= capacity - used) { free(peers); return -1; }
        used += (size_t)added;
        for (j = 0; j < 16; ++j) {
            added = snprintf(output + used, capacity - used, "%02x", peers[i].address[j]);
            if (added < 0 || (size_t)added >= capacity - used) { free(peers); return -1; }
            used += (size_t)added;
        }
        added = snprintf(output + used, capacity - used, " %u %u %s\n",
            peers[i].port, peers[i].scope_id, peers[i].manifest);
        if (added < 0 || (size_t)added >= capacity - used) { free(peers); return -1; }
        used += (size_t)added;
    }
    free(peers);
    *output_length = used;
    return 0;
}

int gnb_utc_hub_parse(const char *text, size_t length, gnb_utc_hub_projection_t *output,
                      char *error, size_t error_size) {
    gnb_utc_hub_projection_t parsed;
    char *copy, *line, *save, *fields[16];
    char canonical[GNB_UTC_HUB_TEXT_MAX];
    size_t canonical_length = 0;
    int count;
    if (!text || !length || length >= GNB_UTC_HUB_TEXT_MAX || !output)
        return set_error(error, error_size, "invalid Hub projection input");
    copy = malloc(length + 1U);
    if (!copy) return set_error(error, error_size, "Hub projection allocation failed");
    memcpy(copy, text, length);
    copy[length] = '\0';
    memset(&parsed, 0, sizeof(parsed));
    line = strtok_r(copy, "\n", &save);
    count = line ? split(line, fields, 16) : -1;
    if (count != 8 || strcmp(fields[0], "utc-v2") ||
        !token(fields[1]) || !token(fields[2]) ||
        number(fields[3], &parsed.local_id) || number(fields[4], &parsed.config_gen) ||
        strlen(fields[5]) != 32 || number(fields[6], &parsed.process_id))
        goto invalid;
    strcpy(parsed.scope, fields[1]);
    strcpy(parsed.instance, fields[2]);
    strcpy(parsed.boot_id, fields[5]);
    {
        uint64_t capabilities;
        if (number(fields[7], &capabilities) || capabilities > UINT32_MAX) goto invalid;
        parsed.capabilities = (uint32_t)capabilities;
    }
    while ((line = strtok_r(NULL, "\n", &save))) {
        gnb_utc_hub_peer_t *peer;
        uint64_t mode, family, port, scope_id;
        if (parsed.count == GNB_UTC_HUB_MAX_PEERS) goto invalid;
        count = split(line, fields, 16);
        if (count != 13 || strcmp(fields[0], "peer")) goto invalid;
        peer = &parsed.peers[parsed.count];
        if (number(fields[1], &peer->peer_id) || number(fields[2], &peer->pair_id) ||
            number(fields[3], &peer->pair_gen) || number(fields[4], &mode) ||
            number(fields[5], &peer->lease_epoch) ||
            number(fields[6], &peer->placement_epoch) ||
            number(fields[7], &peer->expires_at) || number(fields[8], &family) ||
            number(fields[10], &port) || number(fields[11], &scope_id) ||
            mode > UINT8_MAX || family > UINT8_MAX || port > UINT16_MAX ||
            scope_id > UINT32_MAX || decode_hex(fields[9], peer->address, 16))
            goto invalid;
        peer->mode = (uint8_t)mode;
        peer->family = (uint8_t)family;
        peer->port = (uint16_t)port;
        peer->scope_id = (uint32_t)scope_id;
        if (strlen(fields[12]) != 64) goto invalid;
        strcpy(peer->manifest, fields[12]);
        ++parsed.count;
    }
    if (gnb_utc_hub_format(&parsed, canonical, sizeof(canonical), &canonical_length) ||
        canonical_length != length || memcmp(canonical, text, length))
        goto invalid;
    *output = parsed;
    free(copy);
    return 0;
invalid:
    free(copy);
    return set_error(error, error_size, "invalid or non-canonical utc-v2 projection");
}
