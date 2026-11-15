/**
 * bg3mse - the .lsf rewrite of the twin path
 *
 * The served .lsf parses to what the vanilla Mac .lsf parses to, with the
 * mod's values. Structural fields come from the Mac material, value fields
 * from the mod's:
 *
 *   structural: every attribute under a ShaderDesc_<pass> node and its
 *               children, except the Vulkan set and binding (sizes, both CB
 *               bindings' Dx indices, the field offsets, EngineParamFlags);
 *               every DxVsIndex and DxPsIndex (textures, samplers, virtual
 *               texture parts); every Static<pass> offset on a numeric
 *               parameter; MetalReady on the root
 *   value:      everything else: parameter defaults, texture GUIDs, the
 *               material flags, names, editor metadata
 *
 * Structure follows the Mac material outright: a structural attribute or a
 * node below a parameter or a pass description that only the mod has is
 * dropped (StaticBake, a Windows-only field offset), one that only the Mac
 * material has is added with the Mac value (a field offset, a virtual texture
 * cache). The parameter sets have to agree: a parameter group or parameter
 * on one side only, matched by group and UniformName, refuses, as does a type
 * or size mismatch of a structural attribute. With identical shaders the
 * parameter sets are identical, so a difference is an edited .lsf or a Mac
 * shader that is not the Windows one (Spectre_Skin_VT).
 */

#ifndef BG3MSE_SHADERS_LSF_RELAYOUT_H
#define BG3MSE_SHADERS_LSF_RELAYOUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lsf.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t edited;         /* attribute values taken from the Mac material in place of the mod's */
    uint32_t changed;        /* of those, different from the mod's */
    uint32_t dropped_nodes;
    uint32_t dropped_attrs;
    uint32_t added_nodes;
    uint32_t added_attrs;
} LsfRelayoutStats;

/** Rewrite mod with mac's structure. *out is malloc'd. False with err naming the first difference that refuses. */
bool lsf_relayout(const LsfDoc *mod, const LsfDoc *mac, uint8_t **out, size_t *out_size,
                  LsfRelayoutStats *stats, char *err, size_t err_len);

/**
 * Per-field trace of a rewrite, for a caller that tallies the differences: kind is "changed" (a structural value taken from Mac that
 * differs from the mod's), "added" (a Mac-only structural attribute, or an attribute of an added node, node then
 * the node's name), "dropped" (a mod-only structural attribute, or "dropped-node" with attr NULL). node is the
 * node's name, attr the attribute's.
 */
typedef void (*LsfRelayoutTrace)(void *user, const char *kind, const char *node, const char *attr);
bool lsf_relayout_traced(const LsfDoc *mod, const LsfDoc *mac, uint8_t **out, size_t *out_size,
                         LsfRelayoutStats *stats, LsfRelayoutTrace trace, void *user, char *err, size_t err_len);

#ifdef __cplusplus
}
#endif

#endif /* BG3MSE_SHADERS_LSF_RELAYOUT_H */
