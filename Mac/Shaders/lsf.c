/**
 * bg3mse - Larian LSF (binary resource) reader
 */

#include "lsf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <lz4.h>

#define LSF_MAGIC 0x464F534Cu        /* "LSOF" */
#define LSF_HEADER_SIZE 16
#define LSF_META_SIZE 48
#define LSF_DATA_AT (LSF_HEADER_SIZE + LSF_META_SIZE)
#define LSF_MAX_SECTION (64u << 20)
#define LZ4F_MAGIC 0x184D2204u

enum { SEC_STRINGS, SEC_NODES, SEC_ATTRS, SEC_VALUES, SEC_KEYS, SEC_COUNT };

static void set_err(char *err, size_t err_len, const char *msg) {
    if (err && err_len) snprintf(err, err_len, "%s", msg);
}

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

/* LZ4 frame: header, then blocks of {u32 size (high bit = stored), data}, end mark 0. */
static bool lz4_frame_decode(const uint8_t *src, size_t n, uint8_t *dst, size_t cap) {
    if (n < 7 || rd32(src) != LZ4F_MAGIC) return false;
    uint8_t flg = src[4];
    if ((flg >> 6) != 1) return false;
    size_t p = 6 + ((flg & 0x08) ? 8 : 0) + ((flg & 0x01) ? 4 : 0) + 1;
    bool block_checksum = (flg & 0x10) != 0;
    size_t out = 0;
    while (p + 4 <= n) {
        uint32_t bs = rd32(src + p);
        p += 4;
        if (bs == 0) return out == cap;
        uint32_t len = bs & 0x7FFFFFFFu;
        if (len > n - p) return false;
        if (bs & 0x80000000u) {
            if (len > cap - out) return false;
            memcpy(dst + out, src + p, len);
            out += len;
        } else {
            /* Blocks may reference earlier output (linked blocks), so decode with the
             * whole output so far as the dictionary. */
            int r = LZ4_decompress_safe_usingDict((const char *)src + p, (char *)dst + out,
                                                  (int)len, (int)(cap - out),
                                                  (const char *)dst, (int)out);
            if (r < 0) return false;
            out += (size_t)r;
        }
        p += len + (block_checksum ? 4 : 0);
    }
    return false;
}

static uint8_t *read_section(const uint8_t *d, size_t size, size_t *off,
                             uint32_t unc, uint32_t disk, uint32_t method,
                             char *err, size_t err_len) {
    if (unc > LSF_MAX_SECTION) { set_err(err, err_len, "section too large"); return NULL; }
    uint8_t *buf = malloc(unc ? unc : 1);
    if (!buf) { set_err(err, err_len, "out of memory"); return NULL; }
    if (disk == 0) {
        if (unc > size - *off) { free(buf); set_err(err, err_len, "section out of range"); return NULL; }
        memcpy(buf, d + *off, unc);
        *off += unc;
        return buf;
    }
    if (disk > size - *off) { free(buf); set_err(err, err_len, "section out of range"); return NULL; }
    const uint8_t *src = d + *off;
    bool ok = false;
    if (method == 1) {
        uLongf out = unc;
        ok = uncompress(buf, &out, src, disk) == Z_OK && out == unc;
    } else if (method == 2) {
        if (disk >= 4 && rd32(src) == LZ4F_MAGIC)
            ok = lz4_frame_decode(src, disk, buf, unc);
        else
            ok = LZ4_decompress_safe((const char *)src, (char *)buf, (int)disk, (int)unc) == (int)unc;
    } else {
        set_err(err, err_len, "unknown compression method");
        free(buf);
        return NULL;
    }
    if (!ok) { free(buf); set_err(err, err_len, "section failed to decompress"); return NULL; }
    *off += disk;
    return buf;
}

/* String table: u32 bucket count, per bucket u16 count, per string u16 length + bytes. */
typedef struct {
    uint32_t bucket_count;
    uint32_t *bucket_first;   /* index of the bucket's first string */
    uint16_t *bucket_len;
    const char **strings;
    uint32_t string_count;
} NameTable;

static bool build_names(const uint8_t *s, uint32_t n, NameTable *t, char **storage,
                        char *err, size_t err_len) {
    memset(t, 0, sizeof(*t));
    if (n < 4) { set_err(err, err_len, "string table too short"); return false; }
    uint32_t buckets = rd32(s);
    if (buckets > n / 2) { set_err(err, err_len, "bad bucket count"); return false; }
    /* First pass: count strings and bytes. */
    size_t p = 4, total = 0, bytes = 0;
    for (uint32_t b = 0; b < buckets; b++) {
        if (p + 2 > n) goto bad;
        uint16_t c = rd16(s + p); p += 2;
        for (uint16_t i = 0; i < c; i++) {
            if (p + 2 > n) goto bad;
            uint16_t len = rd16(s + p); p += 2;
            if (len > n - p) goto bad;
            p += len; bytes += (size_t)len + 1; total++;
        }
    }
    t->bucket_count = buckets;
    t->string_count = (uint32_t)total;
    t->bucket_first = calloc(buckets ? buckets : 1, sizeof(uint32_t));
    t->bucket_len = calloc(buckets ? buckets : 1, sizeof(uint16_t));
    t->strings = calloc(total ? total : 1, sizeof(char *));
    *storage = malloc(bytes ? bytes : 1);
    if (!t->bucket_first || !t->bucket_len || !t->strings || !*storage) {
        set_err(err, err_len, "out of memory");
        return false;
    }
    char *w = *storage;
    uint32_t k = 0;
    p = 4;
    for (uint32_t b = 0; b < buckets; b++) {
        uint16_t c = rd16(s + p); p += 2;
        t->bucket_first[b] = k;
        t->bucket_len[b] = c;
        for (uint16_t i = 0; i < c; i++) {
            uint16_t len = rd16(s + p); p += 2;
            memcpy(w, s + p, len);
            w[len] = '\0';
            t->strings[k++] = w;
            w += len + 1;
            p += len;
        }
    }
    return true;
bad:
    set_err(err, err_len, "string table out of range");
    return false;
}

static void free_names(NameTable *t) {
    free(t->bucket_first);
    free(t->bucket_len);
    free(t->strings);
}

static const char *lookup_name(const NameTable *t, uint32_t ref) {
    uint32_t b = ref >> 16, i = ref & 0xFFFF;
    if (b >= t->bucket_count || i >= t->bucket_len[b]) return NULL;
    return t->strings[t->bucket_first[b] + i];
}

bool lsf_parse(const void *data, size_t size, LsfDoc *out, char *err, size_t err_len) {
    const uint8_t *d = (const uint8_t *)data;
    if (!d || !out) { set_err(err, err_len, "null argument"); return false; }
    memset(out, 0, sizeof(*out));
    if (size < LSF_DATA_AT || rd32(d) != LSF_MAGIC) { set_err(err, err_len, "not an LSF file"); return false; }
    out->version = rd32(d + 4);
    memcpy(out->header, d, sizeof out->header);
    if (out->version < 6 || out->version > 7) {
        char msg[64];
        snprintf(msg, sizeof msg, "unsupported LSF version %u", out->version);
        set_err(err, err_len, msg);
        return false;
    }

    const uint8_t *m = d + LSF_HEADER_SIZE;
    uint32_t su = rd32(m + 0), sd = rd32(m + 4), ku = rd32(m + 8), kd = rd32(m + 12);
    uint32_t nu = rd32(m + 16), nd = rd32(m + 20), au = rd32(m + 24), ad = rd32(m + 28);
    uint32_t vu = rd32(m + 32), vd = rd32(m + 36);
    uint32_t method = m[40] & 0x0F;
    uint32_t format = rd32(m + 44);
    bool long_format = format == 1;
    out->format = format;
    out->strings_size = su;

    /* Sections are stored strings, nodes, attributes, values, keys. */
    const uint32_t unc[SEC_COUNT] = { su, nu, au, vu, ku };
    const uint32_t disk[SEC_COUNT] = { sd, nd, ad, vd, kd };
    size_t off = LSF_DATA_AT;
    for (int i = 0; i < SEC_COUNT; i++) {
        out->sections[i] = read_section(d, size, &off, unc[i], disk[i], method, err, err_len);
        if (!out->sections[i]) { lsf_free(out); return false; }
    }

    NameTable names;
    if (!build_names(out->sections[SEC_STRINGS], su, &names, &out->names, err, err_len)) {
        free_names(&names);
        lsf_free(out);
        return false;
    }

    const uint8_t *nb = out->sections[SEC_NODES];
    const uint8_t *ab = out->sections[SEC_ATTRS];
    const uint8_t *vb = out->sections[SEC_VALUES];
    uint32_t node_size = long_format ? 16 : 12;
    uint32_t attr_size = long_format ? 16 : 12;
    uint32_t node_count = nu / node_size;
    uint32_t attr_count = au / attr_size;

    out->nodes = calloc(node_count ? node_count : 1, sizeof(LsfNode));
    out->attrs = calloc(attr_count ? attr_count : 1, sizeof(LsfAttr));
    int32_t *owner = calloc(attr_count ? attr_count : 1, sizeof(int32_t));
    uint32_t *val_off = calloc(attr_count ? attr_count : 1, sizeof(uint32_t));
    LsfAttr *raw = calloc(attr_count ? attr_count : 1, sizeof(LsfAttr));
    if (!out->nodes || !out->attrs || !owner || !val_off || !raw) {
        set_err(err, err_len, "out of memory");
        goto fail;
    }
    out->node_count = node_count;

    for (uint32_t i = 0; i < node_count; i++) {
        const uint8_t *e = nb + (size_t)i * node_size;
        const char *nm = lookup_name(&names, rd32(e));
        int32_t parent = (int32_t)rd32(e + (long_format ? 4 : 8));
        if (!nm || parent < -1 || parent >= (int32_t)node_count) {
            set_err(err, err_len, "bad node entry");
            goto fail;
        }
        out->nodes[i].name = nm;
        out->nodes[i].parent = parent;
        out->nodes[i].name_ref = rd32(e);
    }

    /* Read every attribute, recording which node owns it. */
    for (uint32_t i = 0; i < attr_count; i++) owner[i] = -1;
    if (long_format) {
        for (uint32_t i = 0; i < attr_count; i++) {
            const uint8_t *e = ab + (size_t)i * attr_size;
            uint32_t tl = rd32(e + 4);
            raw[i].name = lookup_name(&names, rd32(e));
            raw[i].name_ref = rd32(e);
            raw[i].index = i;
            raw[i].type = tl & 0x3F;
            raw[i].size = tl >> 6;
            val_off[i] = rd32(e + 12);
        }
        /* Walk each node's chain from its first attribute. */
        for (uint32_t n = 0; n < node_count; n++) {
            int32_t a = (int32_t)rd32(nb + (size_t)n * node_size + 12);
            uint32_t guard = 0;
            while (a >= 0) {
                if ((uint32_t)a >= attr_count || owner[a] != -1 || ++guard > attr_count) {
                    set_err(err, err_len, "bad attribute chain");
                    goto fail;
                }
                owner[a] = (int32_t)n;
                a = (int32_t)rd32(ab + (size_t)a * attr_size + 8);
            }
        }
    } else {
        uint64_t voff = 0;
        for (uint32_t i = 0; i < attr_count; i++) {
            const uint8_t *e = ab + (size_t)i * attr_size;
            uint32_t tl = rd32(e + 4);
            int32_t ni = (int32_t)rd32(e + 8);
            raw[i].name = lookup_name(&names, rd32(e));
            raw[i].name_ref = rd32(e);
            raw[i].index = i;
            raw[i].type = tl & 0x3F;
            raw[i].size = tl >> 6;
            if (ni < 0 || ni >= (int32_t)node_count || voff > UINT32_MAX) {
                set_err(err, err_len, "bad attribute entry");
                goto fail;
            }
            owner[i] = ni;
            val_off[i] = (uint32_t)voff;
            voff += raw[i].size;
        }
    }
    for (uint32_t i = 0; i < attr_count; i++) {
        if (owner[i] < 0) continue;               /* unreferenced attribute */
        if (!raw[i].name || (uint64_t)val_off[i] + raw[i].size > vu) {
            set_err(err, err_len, "attribute value out of range");
            goto fail;
        }
        raw[i].value = vb + val_off[i];
    }

    /* Lay attributes out node by node, keeping each node's original order. */
    for (uint32_t i = 0; i < attr_count; i++)
        if (owner[i] >= 0) out->nodes[owner[i]].attr_count++;
    uint32_t pos = 0;
    for (uint32_t n = 0; n < node_count; n++) {
        out->nodes[n].attr_begin = pos;
        pos += out->nodes[n].attr_count;
        out->nodes[n].attr_count = 0;
    }
    if (long_format) {
        for (uint32_t n = 0; n < node_count; n++) {
            int32_t a = (int32_t)rd32(nb + (size_t)n * node_size + 12);
            while (a >= 0) {
                LsfNode *node = &out->nodes[n];
                out->attrs[node->attr_begin + node->attr_count++] = raw[a];
                a = (int32_t)rd32(ab + (size_t)a * attr_size + 8);
            }
        }
    } else {
        for (uint32_t i = 0; i < attr_count; i++) {
            LsfNode *node = &out->nodes[owner[i]];
            out->attrs[node->attr_begin + node->attr_count++] = raw[i];
        }
    }
    out->attr_count = pos;

    free(owner);
    free(val_off);
    free(raw);
    free_names(&names);
    return true;

fail:
    free(owner);
    free(val_off);
    free(raw);
    free_names(&names);
    lsf_free(out);
    return false;
}

void lsf_free(LsfDoc *doc) {
    if (!doc) return;
    free(doc->nodes);
    free(doc->attrs);
    free(doc->names);
    for (int i = 0; i < 5; i++) free(doc->sections[i]);
    memset(doc, 0, sizeof(*doc));
}

const LsfAttr *lsf_attr(const LsfDoc *doc, uint32_t node, const char *name) {
    if (!doc || node >= doc->node_count || !name) return NULL;
    const LsfNode *n = &doc->nodes[node];
    for (uint32_t i = 0; i < n->attr_count; i++) {
        const LsfAttr *a = &doc->attrs[n->attr_begin + i];
        if (strcmp(a->name, name) == 0) return a;
    }
    return NULL;
}

bool lsf_attr_int(const LsfAttr *a, int64_t *out) {
    if (!a || !out) return false;
    uint64_t u = 0;
    bool is_signed;
    switch (a->type) {
    case LSF_T_BYTE: case LSF_T_USHORT: case LSF_T_UINT: case LSF_T_BOOL: case LSF_T_ULONG:
        is_signed = false; break;
    case LSF_T_SHORT: case LSF_T_INT: case LSF_T_LONG: case LSF_T_INT8: case LSF_T_INT64:
        is_signed = true; break;
    default:
        return false;
    }
    if (a->size == 0 || a->size > 8 || (a->size & (a->size - 1))) return false;
    memcpy(&u, a->value, a->size);  /* little-endian */
    if (is_signed && a->size < 8) {
        uint64_t sign = 1ull << (a->size * 8 - 1);
        u = (u ^ sign) - sign;
    }
    *out = (int64_t)u;
    return true;
}

bool lsf_attr_text(const LsfAttr *a, char *buf, size_t buf_len) {
    if (!a || !buf || !buf_len) return false;
    switch (a->type) {
    case LSF_T_STRING: case LSF_T_PATH: case LSF_T_FIXED: case LSF_T_LSSTRING: break;
    default: return false;
    }
    size_t n = a->size;
    while (n && a->value[n - 1] == '\0') n--;
    if (n >= buf_len) n = buf_len - 1;
    memcpy(buf, a->value, n);
    buf[n] = '\0';
    return true;
}
