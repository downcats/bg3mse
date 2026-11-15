/**
 * bg3mse - the identity table: is a mod's Vulkan shader container a vanilla one?
 *
 * One entry per vanilla Windows container of a game build: the first 16
 * bytes of the container's hash (SpirvIdentity.h: its name, reflection table
 * and stages, a SPIR-V module counting by what it does rather than by its
 * bytes), and the FNV-1a 64 of the pass stem (the container's pak path
 * without `_Vulkan.bshd`). The Mac Materials.pak stubs have the same stems,
 * so a hit names the vanilla pass whose Mac functions the container's shader
 * is: a vanilla container as shipped, or the same shader compiled again.
 *
 * The table itself, for one game build, is identity_data.c, which
 * `bg3mse-shaders identity` writes from a Windows Materials.pak.
 */

#ifndef BG3MSE_SHADERS_IDENTITY_H
#define BG3MSE_SHADERS_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t hash[16];       /* the container's hash, truncated */
    uint64_t stem;          /* identity_stem_hash of the vanilla pass stem */
} IdentityEntry;

extern const char k_identity_build[];
extern const IdentityEntry k_identity[];      /* sorted by hash */
extern const uint32_t k_identity_count;

/** FNV-1a 64 of a pass stem, "Public/Shared/Assets/Materials/Base/Base_ST_FOR". */
uint64_t identity_stem_hash(const char *stem, size_t len);

/**
 * The vanilla passes a container is the same shader as: *first points at the first entry with its hash and
 * *count says how many follow with the same hash (vanilla ships a few containers under two materials).
 * False, with *count 0, when it is not a vanilla container.
 */
bool identity_lookup(const void *container, size_t size, const IdentityEntry **first, uint32_t *count);

/** The lookup's key for a container, for callers that keep it. */
void identity_hash(const void *container, size_t size, uint8_t out[16]);

#ifdef __cplusplus
}
#endif

#endif /* BG3MSE_SHADERS_IDENTITY_H */
