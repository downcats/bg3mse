/**
 * bg3mse - LSF writer
 *
 * Layout written (LSOF v7, format 1, no compression):
 *   header[16]:   "LSOF", version, engine version (copied from the source)
 *   meta[48]:     strings, keys, nodes, attributes, values, each as (uncompressed, on-disk) sizes with
 *                 on-disk 0 meaning stored; byte 40 compression flags 0; u32 44 format 1
 *   strings:      u32 bucket count, per bucket u16 count then (u16 length, bytes) per name
 *   nodes:        16 bytes each: name ref, parent, next sibling, first attribute (-1: none)
 *   attributes:   16 bytes each: name ref, type | size << 6, next attribute (-1: none), value offset
 *   values:       the attribute values, in storage order
 */

#include "lsf_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_err(char *err, size_t err_len, const char *msg) {
    if (err && err_len) snprintf(err, err_len, "%s", msg);
}

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void wr16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }

bool lsf_names_init(LsfNames *t, const LsfDoc *source, char *err, size_t err_len) {
    memset(t, 0, sizeof *t);
    if (!source || !source->sections[0] || source->strings_size < 4) { set_err(err, err_len, "no string table"); return false; }
    const uint8_t *s = source->sections[0];
    uint32_t size = source->strings_size, buckets = rd32(s), off = 4;
    if (buckets == 0 || buckets > 0xffff) { set_err(err, err_len, "bad string table"); return false; }
    /* walk the table once to check it and find the end of the first bucket */
    for (uint32_t b = 0; b < buckets; b++) {
        if (off + 2 > size) { set_err(err, err_len, "bad string table"); return false; }
        uint32_t count = rd16(s + off);
        off += 2;
        for (uint32_t i = 0; i < count; i++) {
            if (off + 2 > size) { set_err(err, err_len, "bad string table"); return false; }
            uint32_t len = rd16(s + off);
            off += 2;
            if (off + len > size) { set_err(err, err_len, "bad string table"); return false; }
            off += len;
        }
        if (b == 0) { t->bucket0_count = count; t->bucket0_end = off; }
    }
    if (off != size) { set_err(err, err_len, "string table has trailing bytes"); return false; }
    t->bytes = malloc(size);
    if (!t->bytes) { set_err(err, err_len, "out of memory"); return false; }
    memcpy(t->bytes, s, size);
    t->size = size;
    return true;
}

bool lsf_names_ref(LsfNames *t, const char *name, uint32_t *ref, char *err, size_t err_len) {
    size_t len = strlen(name);
    if (len == 0 || len > 0xffff) { set_err(err, err_len, "bad name"); return false; }
    uint32_t buckets = rd32(t->bytes), off = 4;
    for (uint32_t b = 0; b < buckets; b++) {
        uint32_t count = rd16(t->bytes + off);
        off += 2;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t l = rd16(t->bytes + off);
            off += 2;
            if (l == len && memcmp(t->bytes + off, name, len) == 0) { *ref = (b << 16) | i; return true; }
            off += l;
        }
    }
    if (t->bucket0_count >= 0xffff) { set_err(err, err_len, "string bucket full"); return false; }
    uint8_t *grown = realloc(t->bytes, t->size + 2 + len);
    if (!grown) { set_err(err, err_len, "out of memory"); return false; }
    t->bytes = grown;
    memmove(t->bytes + t->bucket0_end + 2 + len, t->bytes + t->bucket0_end, t->size - t->bucket0_end);
    wr16(t->bytes + t->bucket0_end, (uint16_t)len);
    memcpy(t->bytes + t->bucket0_end + 2, name, len);
    t->bucket0_end += 2 + (uint32_t)len;
    t->size += 2 + (uint32_t)len;
    *ref = t->bucket0_count++;
    wr16(t->bytes + 4, (uint16_t)t->bucket0_count);
    return true;
}

void lsf_names_free(LsfNames *t) {
    free(t->bytes);
    memset(t, 0, sizeof *t);
}

bool lsf_write(const LsfDoc *header_from, const LsfNames *names, const LsfOutNode *nodes, uint32_t node_count,
               const LsfOutAttr *attrs, uint32_t attr_count, uint8_t **out, size_t *out_size, char *err, size_t err_len) {
    *out = NULL;
    *out_size = 0;
    if (!header_from || !names || !names->bytes) { set_err(err, err_len, "no document"); return false; }
    /* The source may be any document lsf_parse reads (v6 or v7, either node layout): what is written is v7 in the long layout. */
    if (header_from->version < 6 || header_from->version > 7) { set_err(err, err_len, "only an LSOF v6 or v7 document is written again"); return false; }
    if (node_count >= 0x7fffffffu || attr_count >= 0x7fffffffu) { set_err(err, err_len, "too many entries"); return false; }
    for (uint32_t i = 0; i < node_count; i++)
        if (nodes[i].parent < -1 || nodes[i].parent >= (int32_t)node_count) { set_err(err, err_len, "node parent out of range"); return false; }
    uint64_t values_size = 0;
    for (uint32_t a = 0; a < attr_count; a++) {
        if (attrs[a].node >= node_count) { set_err(err, err_len, "attribute node out of range"); return false; }
        if (attrs[a].type > 0x3f || attrs[a].size >= (1u << 26)) { set_err(err, err_len, "attribute type or size out of range"); return false; }
        values_size += attrs[a].size;
    }
    if (values_size > 0xffffffffu) { set_err(err, err_len, "values too large"); return false; }

    size_t nodes_size = (size_t)node_count * 16, attrs_size = (size_t)attr_count * 16;
    size_t total = 64 + names->size + nodes_size + attrs_size + (size_t)values_size;
    uint8_t *buf = calloc(total, 1);
    int32_t *last_child = malloc((node_count + 1) * sizeof *last_child);   /* per parent, roots at node_count */
    int32_t *last_attr = malloc((node_count ? node_count : 1) * sizeof *last_attr);
    if (!buf || !last_child || !last_attr) { free(buf); free(last_child); free(last_attr); set_err(err, err_len, "out of memory"); return false; }

    memcpy(buf, header_from->header, 16);
    buf[4] = 7; buf[5] = buf[6] = buf[7] = 0;      /* the version written, whatever the source's */
    uint8_t *m = buf + 16;
    wr32(m + 0, names->size); wr32(m + 4, 0);
    wr32(m + 8, 0); wr32(m + 12, 0);                      /* keys: none */
    wr32(m + 16, (uint32_t)nodes_size); wr32(m + 20, 0);
    wr32(m + 24, (uint32_t)attrs_size); wr32(m + 28, 0);
    wr32(m + 32, (uint32_t)values_size); wr32(m + 36, 0);
    m[40] = 0; m[41] = 0; m[42] = 0; m[43] = 0;
    wr32(m + 44, 1);

    uint8_t *strings = buf + 64, *nb = strings + names->size, *ab = nb + nodes_size, *vb = ab + attrs_size;
    memcpy(strings, names->bytes, names->size);

    for (uint32_t i = 0; i <= node_count; i++) last_child[i] = -1;
    for (uint32_t i = 0; i < node_count; i++) {
        uint8_t *e = nb + (size_t)i * 16;
        wr32(e + 0, nodes[i].name_ref);
        wr32(e + 4, nodes[i].parent < 0 ? 0xffffffffu : (uint32_t)nodes[i].parent);
        wr32(e + 8, 0xffffffffu);
        wr32(e + 12, 0xffffffffu);
        uint32_t slot = nodes[i].parent < 0 ? node_count : (uint32_t)nodes[i].parent;
        if (last_child[slot] >= 0) wr32(nb + (size_t)last_child[slot] * 16 + 8, i);
        last_child[slot] = (int32_t)i;
        last_attr[i] = -1;
    }
    uint32_t voff = 0;
    for (uint32_t a = 0; a < attr_count; a++) {
        uint8_t *e = ab + (size_t)a * 16;
        wr32(e + 0, attrs[a].name_ref);
        wr32(e + 4, attrs[a].type | (attrs[a].size << 6));
        wr32(e + 8, 0xffffffffu);
        wr32(e + 12, voff);
        if (attrs[a].size) memcpy(vb + voff, attrs[a].value, attrs[a].size);
        voff += attrs[a].size;
        uint32_t n = attrs[a].node;
        if (last_attr[n] < 0) wr32(nb + (size_t)n * 16 + 12, a);
        else wr32(ab + (size_t)last_attr[n] * 16 + 8, a);
        last_attr[n] = (int32_t)a;
    }
    free(last_child);
    free(last_attr);
    *out = buf;
    *out_size = total;
    return true;
}

static int cmp_index(const void *x, const void *y) {
    uint32_t a = (*(const LsfAttr *const *)x)->index, b = (*(const LsfAttr *const *)y)->index;
    return a < b ? -1 : a > b;
}

bool lsf_write_doc(const LsfDoc *doc, uint8_t **out, size_t *out_size, char *err, size_t err_len) {
    *out = NULL;
    *out_size = 0;
    if (!doc || !doc->nodes) { set_err(err, err_len, "no document"); return false; }
    LsfNames names;
    if (!lsf_names_init(&names, doc, err, err_len)) return false;
    LsfOutNode *on = calloc(doc->node_count ? doc->node_count : 1, sizeof *on);
    LsfOutAttr *oa = calloc(doc->attr_count ? doc->attr_count : 1, sizeof *oa);
    const LsfAttr **order = calloc(doc->attr_count ? doc->attr_count : 1, sizeof *order);
    bool ok = on && oa && order;
    if (!ok) set_err(err, err_len, "out of memory");
    for (uint32_t i = 0; ok && i < doc->node_count; i++) {
        on[i].name_ref = doc->nodes[i].name_ref;
        on[i].parent = doc->nodes[i].parent;
        for (uint32_t k = 0; k < doc->nodes[i].attr_count; k++) order[doc->nodes[i].attr_begin + k] = &doc->attrs[doc->nodes[i].attr_begin + k];
    }
    if (ok) {
        qsort(order, doc->attr_count, sizeof *order, cmp_index);
        for (uint32_t a = 0; a < doc->attr_count; a++) {
            const LsfAttr *s = order[a];
            uint32_t node = 0;
            while (node < doc->node_count && !(s >= &doc->attrs[doc->nodes[node].attr_begin] && s < &doc->attrs[doc->nodes[node].attr_begin + doc->nodes[node].attr_count])) node++;
            oa[a] = (LsfOutAttr){ node, s->name_ref, s->type, s->value, s->size };
        }
        ok = lsf_write(doc, &names, on, doc->node_count, oa, doc->attr_count, out, out_size, err, err_len);
    }
    free(on);
    free(oa);
    free(order);
    lsf_names_free(&names);
    return ok;
}
