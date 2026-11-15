/**
 * bg3mse - LSF writer: a node tree back to bytes
 *
 * Writes LSOF v7 in the long node layout with uncompressed sections, which is
 * what every material .lsf of the game and of mods is. The string table starts as a source document's, copied unchanged, and
 * grows by the names the output needs that the source lacks, appended to its
 * first bucket. A reference is a bucket and an index, and a reader resolves it
 * by those alone: LSLib places names by .NET's GetHashCode, which is seeded
 * per process, and the files it writes load in the game, so the engine does
 * not look a name up by Larian's hash. (The final confirmation is the first
 * twin that renders in the game.)
 *
 * The output is described outright: nodes in output order with parents by
 * output index, attributes in storage order with their node by output index.
 * A node's attribute chain is its attributes in the order given, so giving a
 * source document's attributes in their original storage order (LsfAttr.index)
 * reproduces its bytes.
 */

#ifndef BG3MSE_SHADERS_LSF_WRITE_H
#define BG3MSE_SHADERS_LSF_WRITE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lsf.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A string table under construction: the source's, plus appended names. */
typedef struct {
    uint8_t *bytes;          /* the section as it will be written */
    uint32_t size;
    uint32_t bucket0_count;  /* names in the first bucket, where appended names go */
    uint32_t bucket0_end;    /* byte offset just past the first bucket */
} LsfNames;

bool lsf_names_init(LsfNames *t, const LsfDoc *source, char *err, size_t err_len);
/** The reference of name, appending it when the table lacks it. */
bool lsf_names_ref(LsfNames *t, const char *name, uint32_t *ref, char *err, size_t err_len);
void lsf_names_free(LsfNames *t);

typedef struct {
    uint32_t name_ref;
    int32_t parent;          /* output index, or -1 */
} LsfOutNode;

typedef struct {
    uint32_t node;           /* output index */
    uint32_t name_ref;
    uint32_t type;
    const uint8_t *value;
    uint32_t size;
} LsfOutAttr;

/**
 * Write the tree. header_from supplies the file header's magic and engine
 * version and may be any document lsf_parse reads; what is written is v7 in
 * the long layout. *out is malloc'd.
 */
bool lsf_write(const LsfDoc *header_from, const LsfNames *names, const LsfOutNode *nodes, uint32_t node_count,
               const LsfOutAttr *attrs, uint32_t attr_count, uint8_t **out, size_t *out_size, char *err, size_t err_len);

/** Write a parsed document back unchanged: nodes in order, attributes in storage order. */
bool lsf_write_doc(const LsfDoc *doc, uint8_t **out, size_t *out_size, char *err, size_t err_len);

#ifdef __cplusplus
}
#endif

#endif /* BG3MSE_SHADERS_LSF_WRITE_H */
