/**
 * bg3mse - the identity table: lookup
 */

#include "identity.h"

#include <string.h>

uint64_t identity_stem_hash(const char *stem, size_t len) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < len; i++) h = (h ^ (uint8_t)stem[i]) * 0x100000001b3ull;
    return h;
}

bool identity_lookup(const void *container, size_t size, const IdentityEntry **first, uint32_t *count) {
    uint8_t key[16];
    identity_hash(container, size, key);
    *first = NULL;
    *count = 0;
    /* lower bound */
    uint32_t lo = 0, hi = k_identity_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (memcmp(k_identity[mid].hash, key, 16) < 0) lo = mid + 1;
        else hi = mid;
    }
    if (lo == k_identity_count || memcmp(k_identity[lo].hash, key, 16) != 0) return false;
    *first = &k_identity[lo];
    while (lo + *count < k_identity_count && memcmp(k_identity[lo + *count].hash, key, 16) == 0) (*count)++;
    return true;
}
