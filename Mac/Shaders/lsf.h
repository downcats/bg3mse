/**
 * bg3mse - Larian LSF (binary resource) reader
 *
 * Reads LSOF v6/v7 files: 16-byte header, 48-byte section table, then the
 * string, node, attribute, value and key sections, each optionally zlib or
 * LZ4 (raw block) compressed. Both node layouts are supported: the long one
 * (16-byte nodes and attributes, attributes chained) and the short one
 * (12-byte entries, attributes listed in node order).
 *
 * Input is untrusted mod data: every index and length is checked.
 */

#ifndef BG3MSE_SHADERS_LSF_H
#define BG3MSE_SHADERS_LSF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    LSF_T_BYTE = 1, LSF_T_SHORT = 2, LSF_T_USHORT = 3, LSF_T_INT = 4,
    LSF_T_UINT = 5, LSF_T_FLOAT = 6, LSF_T_DOUBLE = 7, LSF_T_BOOL = 19,
    LSF_T_STRING = 20, LSF_T_PATH = 21, LSF_T_FIXED = 22, LSF_T_LSSTRING = 23,
    LSF_T_ULONG = 24, LSF_T_LONG = 26, LSF_T_INT8 = 27, LSF_T_INT64 = 32
};

typedef struct {
    const char *name;
    uint32_t type;
    const uint8_t *value;
    uint32_t size;
    uint32_t name_ref;       /* the string table reference, (bucket << 16) | index, for lsf_write */
    uint32_t index;          /* position in the file's attribute table: the storage order, for lsf_write */
} LsfAttr;

typedef struct {
    const char *name;
    int32_t parent;          /* -1 for roots */
    uint32_t attr_begin;     /* index into LsfDoc.attrs */
    uint32_t attr_count;
    uint32_t name_ref;       /* the string table reference, for lsf_write */
} LsfNode;

typedef struct {
    uint32_t version;
    uint32_t format;         /* the header's format word: 1 is the long node layout */
    uint8_t header[16 + 48]; /* the file header and section table, as read */
    uint32_t strings_size;   /* size of sections[0], the string table, kept whole by lsf_write */
    LsfNode *nodes;
    uint32_t node_count;
    LsfAttr *attrs;
    uint32_t attr_count;
    /* owned storage */
    char *names;
    uint8_t *sections[5];
} LsfDoc;

bool lsf_parse(const void *data, size_t size, LsfDoc *out, char *err, size_t err_len);
void lsf_free(LsfDoc *doc);

/** Attribute of a node by name, or NULL. */
const LsfAttr *lsf_attr(const LsfDoc *doc, uint32_t node, const char *name);

/** Integer value of any integer-typed attribute. */
bool lsf_attr_int(const LsfAttr *a, int64_t *out);

/** Copy a text attribute into buf (always NUL-terminated). */
bool lsf_attr_text(const LsfAttr *a, char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif /* BG3MSE_SHADERS_LSF_H */
