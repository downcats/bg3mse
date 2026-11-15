/**
 * bg3mse - the .lsf rewrite of the twin path
 *
 * Two passes over the mod's tree matched against the Mac material's: the
 * first decides, per mod node and attribute, kept or dropped (and the value
 * to write), and lists the Mac subtrees and attributes to add; the second
 * emits the output tree, nodes in the mod's order with additions after the
 * matched parent's own children, attributes in the mod's storage order with
 * additions at the end.
 */

#include "lsf_relayout.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lsf_write.h"

typedef struct {
    const LsfDoc *mod, *mac;
    int32_t *mod_match;        /* per mod node: Mac node, -1 unmatched (dropped) */
    int32_t *mac_match;        /* per Mac node: mod node, -1 unmatched; -2 unmatched and added */
    const uint8_t **mod_value; /* per mod attribute: value to write, NULL dropped */
    uint8_t *mac_attr_added;   /* per Mac attribute: added to the matched mod node */
    LsfNames names;
    LsfOutNode *nodes;
    uint32_t node_count;
    LsfOutAttr *attrs;
    uint32_t attr_count;
    int32_t *mod_out;          /* per mod node: output index */
    int32_t *mac_out;          /* per Mac node: output index when added */
    LsfRelayoutStats stats;
    LsfRelayoutTrace trace;
    void *user;
    char *err;
    size_t err_len;
} Ctx;

static void trace(Ctx *c, const char *kind, const char *node, const char *attr) {
    if (c->trace) c->trace(c->user, kind, node, attr);
}

static bool refuse(Ctx *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static bool refuse(Ctx *c, const char *fmt, ...) {
    if (c->err && c->err_len) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(c->err, c->err_len, fmt, ap);
        va_end(ap);
    }
    return false;
}

static bool is_vk(const char *name) {
    return strcmp(name, "VkDescriptorSet") == 0 || strcmp(name, "VkBindingIndex") == 0;
}

static bool is_numeric_group(const char *name) {
    return strcmp(name, "ScalarParameters") == 0 || strcmp(name, "Vector2Parameters") == 0 ||
           strcmp(name, "Vector3Parameters") == 0 || strcmp(name, "VectorParameters") == 0;
}

/* Structural or value: see the header. */
static bool is_structural(const char *attr, bool is_root, bool under_desc, bool numeric_param) {
    if (is_vk(attr)) return false;
    if (is_root) return strcmp(attr, "MetalReady") == 0;
    if (under_desc) return true;
    if (strcmp(attr, "DxVsIndex") == 0 || strcmp(attr, "DxPsIndex") == 0) return true;
    if (numeric_param && strncmp(attr, "Static", 6) == 0) return true;
    return false;
}

static const char *uniform_name(const LsfDoc *d, uint32_t node, char *buf, size_t len) {
    const LsfAttr *a = lsf_attr(d, node, "UniformName");
    if (!a || !lsf_attr_text(a, buf, len)) return NULL;
    return buf;
}

static const char *path_of(const LsfDoc *d, uint32_t node, char *buf, size_t len) {
    char tmp[512] = "";
    for (int32_t n = (int32_t)node; n >= 0; n = d->nodes[n].parent) {
        char next[512];
        snprintf(next, sizeof next, "%s%s%s", d->nodes[n].name, tmp[0] ? "/" : "", tmp);
        memcpy(tmp, next, sizeof tmp);
    }
    snprintf(buf, len, "%s", tmp);
    return buf;
}

static uint32_t children_of(const LsfDoc *d, int32_t node, uint32_t *out) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < d->node_count; i++)
        if (d->nodes[i].parent == node) out[n++] = i;
    return n;
}

/* ---- pass 1: match ---- */

static bool match_attrs(Ctx *c, uint32_t mod_node, uint32_t mac_node, bool is_root, bool under_desc, bool numeric_param) {
    const LsfNode *mn = &c->mod->nodes[mod_node], *cn = &c->mac->nodes[mac_node];
    char path[512];
    for (uint32_t i = 0; i < mn->attr_count; i++) {
        const LsfAttr *a = &c->mod->attrs[mn->attr_begin + i];
        if (!is_structural(a->name, is_root, under_desc, numeric_param)) continue;
        const LsfAttr *b = lsf_attr(c->mac, mac_node, a->name);
        if (!b) {
            c->mod_value[mn->attr_begin + i] = NULL;
            c->stats.dropped_attrs++;
            trace(c, "dropped", mn->name, a->name);
            continue;
        }
        if (b->type != a->type || b->size != a->size)
            return refuse(c, "%s.%s is type %u of %u bytes, and %u of %u on Mac", path_of(c->mod, mod_node, path, sizeof path), a->name,
                          a->type, a->size, b->type, b->size);
        c->mod_value[mn->attr_begin + i] = b->value;
        c->stats.edited++;
        if (memcmp(a->value, b->value, a->size) != 0) { c->stats.changed++; trace(c, "changed", mn->name, a->name); }
    }
    for (uint32_t i = 0; i < cn->attr_count; i++) {
        const LsfAttr *b = &c->mac->attrs[cn->attr_begin + i];
        if (!is_structural(b->name, is_root, under_desc, numeric_param)) continue;
        if (lsf_attr(c->mod, mod_node, b->name)) continue;
        c->mac_attr_added[cn->attr_begin + i] = 1;
        c->stats.added_attrs++;
        trace(c, "added", cn->name, b->name);
    }
    return true;
}

static void count_subtree(Ctx *c, uint32_t mac_node) {
    c->stats.added_nodes++;
    c->stats.added_attrs += c->mac->nodes[mac_node].attr_count;
    trace(c, "added-node", c->mac->nodes[mac_node].name, NULL);
    for (uint32_t k = 0; k < c->mac->nodes[mac_node].attr_count; k++)
        trace(c, "added", c->mac->nodes[mac_node].name, c->mac->attrs[c->mac->nodes[mac_node].attr_begin + k].name);
    for (uint32_t i = 0; i < c->mac->node_count; i++)
        if (c->mac->nodes[i].parent == (int32_t)mac_node) count_subtree(c, i);
}

static bool match_node(Ctx *c, uint32_t mod_node, uint32_t mac_node, bool is_root, bool under_desc, bool numeric_param) {
    c->mod_match[mod_node] = (int32_t)mac_node;
    c->mac_match[mac_node] = (int32_t)mod_node;
    if (!match_attrs(c, mod_node, mac_node, is_root, under_desc, numeric_param)) return false;
    uint32_t *mk = malloc(c->mod->node_count * sizeof *mk), *ck = malloc(c->mac->node_count * sizeof *ck);
    if (!mk || !ck) { free(mk); free(ck); return refuse(c, "out of memory"); }
    uint32_t mkn = children_of(c->mod, (int32_t)mod_node, mk), ckn = children_of(c->mac, (int32_t)mac_node, ck);
    bool ok = true;
    char path[512], ua[128], ub[128];
    for (uint32_t i = 0; i < mkn && ok; i++) {
        uint32_t m = mk[i];
        const char *name = c->mod->nodes[m].name;
        bool param = is_root && strcmp(name, "ShaderDescriptions") != 0;
        const char *un = param ? uniform_name(c->mod, m, ua, sizeof ua) : NULL;
        if (param && !un) { ok = refuse(c, "%s has no UniformName", path_of(c->mod, m, path, sizeof path)); break; }
        int32_t match = -1;
        for (uint32_t j = 0; j < ckn && match < 0; j++) {
            if (c->mac_match[ck[j]] != -1 || strcmp(c->mac->nodes[ck[j]].name, name) != 0) continue;
            if (param) {
                const char *vn = uniform_name(c->mac, ck[j], ub, sizeof ub);
                if (!vn || strcmp(vn, un) != 0) continue;
            }
            match = (int32_t)ck[j];
        }
        if (match < 0) {
            if (is_root) { ok = refuse(c, "%s %s is not in the Mac material", path_of(c->mod, m, path, sizeof path), un ? un : ""); break; }
            c->stats.dropped_nodes++;
            trace(c, "dropped-node", name, NULL);
            continue;
        }
        bool desc = under_desc || strncmp(name, "ShaderDesc_", 11) == 0;
        ok = match_node(c, m, (uint32_t)match, false, desc, param && is_numeric_group(name));
    }
    for (uint32_t j = 0; j < ckn && ok; j++) {
        if (c->mac_match[ck[j]] != -1) continue;
        if (is_root) {
            const char *vn = uniform_name(c->mac, ck[j], ub, sizeof ub);
            ok = refuse(c, "the Mac material has %s %s, which the mod's lacks", path_of(c->mac, ck[j], path, sizeof path), vn ? vn : "");
            break;
        }
        c->mac_match[ck[j]] = -2;
        count_subtree(c, ck[j]);
    }
    free(mk);
    free(ck);
    return ok;
}

/* ---- pass 2: emit ---- */

static bool emit_mac_subtree(Ctx *c, uint32_t mac_node, int32_t parent_out) {
    uint32_t ref;
    if (!lsf_names_ref(&c->names, c->mac->nodes[mac_node].name, &ref, c->err, c->err_len)) return false;
    uint32_t me = c->node_count++;
    c->nodes[me] = (LsfOutNode){ ref, parent_out };
    c->mac_out[mac_node] = (int32_t)me;
    const LsfNode *n = &c->mac->nodes[mac_node];
    for (uint32_t k = 0; k < n->attr_count; k++) {
        const LsfAttr *a = &c->mac->attrs[n->attr_begin + k];
        if (!lsf_names_ref(&c->names, a->name, &ref, c->err, c->err_len)) return false;
        c->attrs[c->attr_count++] = (LsfOutAttr){ me, ref, a->type, a->value, a->size };
    }
    for (uint32_t i = 0; i < c->mac->node_count; i++)
        if (c->mac->nodes[i].parent == (int32_t)mac_node && !emit_mac_subtree(c, i, (int32_t)me)) return false;
    return true;
}

static bool emit_node(Ctx *c, uint32_t mod_node, int32_t parent_out) {
    uint32_t me = c->node_count++;
    c->nodes[me] = (LsfOutNode){ c->mod->nodes[mod_node].name_ref, parent_out };
    c->mod_out[mod_node] = (int32_t)me;
    int32_t mac_node = c->mod_match[mod_node];
    for (uint32_t i = 0; i < c->mod->node_count; i++)
        if (c->mod->nodes[i].parent == (int32_t)mod_node && c->mod_match[i] >= 0 && !emit_node(c, i, (int32_t)me)) return false;
    for (uint32_t i = 0; i < c->mac->node_count; i++)
        if (c->mac->nodes[i].parent == mac_node && c->mac_match[i] == -2 && !emit_mac_subtree(c, i, (int32_t)me)) return false;
    return true;
}

static int cmp_index(const void *x, const void *y) {
    uint32_t a = (*(const LsfAttr *const *)x)->index, b = (*(const LsfAttr *const *)y)->index;
    return a < b ? -1 : a > b;
}

bool lsf_relayout(const LsfDoc *mod, const LsfDoc *mac, uint8_t **out, size_t *out_size,
                  LsfRelayoutStats *stats, char *err, size_t err_len) {
    return lsf_relayout_traced(mod, mac, out, out_size, stats, NULL, NULL, err, err_len);
}

bool lsf_relayout_traced(const LsfDoc *mod, const LsfDoc *mac, uint8_t **out, size_t *out_size,
                         LsfRelayoutStats *stats, LsfRelayoutTrace tr, void *user, char *err, size_t err_len) {
    *out = NULL;
    *out_size = 0;
    Ctx c;
    memset(&c, 0, sizeof c);
    c.mod = mod; c.mac = mac; c.err = err; c.err_len = err_len; c.trace = tr; c.user = user;
    if (!mod || !mac || !mod->node_count || !mac->node_count) return refuse(&c, "empty document");
    uint32_t mn = mod->node_count, cn = mac->node_count, ma = mod->attr_count ? mod->attr_count : 1, ca = mac->attr_count ? mac->attr_count : 1;
    c.mod_match = malloc(mn * sizeof *c.mod_match);
    c.mac_match = malloc(cn * sizeof *c.mac_match);
    c.mod_value = calloc(ma, sizeof *c.mod_value);
    c.mac_attr_added = calloc(ca, 1);
    c.mod_out = malloc(mn * sizeof *c.mod_out);
    c.mac_out = malloc(cn * sizeof *c.mac_out);
    bool ok = c.mod_match && c.mac_match && c.mod_value && c.mac_attr_added && c.mod_out && c.mac_out;
    if (!ok) refuse(&c, "out of memory");
    for (uint32_t i = 0; ok && i < mn; i++) { c.mod_match[i] = -1; c.mod_out[i] = -1; }
    for (uint32_t i = 0; ok && i < cn; i++) { c.mac_match[i] = -1; c.mac_out[i] = -1; }
    for (uint32_t i = 0; ok && i < mod->attr_count; i++) c.mod_value[i] = mod->attrs[i].value;

    uint32_t mod_root = 0, mac_root = 0, mod_roots = 0, mac_roots = 0;
    for (uint32_t i = 0; ok && i < mn; i++) if (mod->nodes[i].parent < 0) { mod_root = i; mod_roots++; }
    for (uint32_t i = 0; ok && i < cn; i++) if (mac->nodes[i].parent < 0) { mac_root = i; mac_roots++; }
    if (ok && (mod_roots != 1 || mac_roots != 1)) ok = refuse(&c, "a material .lsf has one root node, this one has %u", mod_roots != 1 ? mod_roots : mac_roots);
    if (ok && strcmp(mod->nodes[mod_root].name, mac->nodes[mac_root].name) != 0)
        ok = refuse(&c, "root is %s, the Mac material's %s", mod->nodes[mod_root].name, mac->nodes[mac_root].name);
    if (ok) ok = match_node(&c, mod_root, mac_root, true, false, false);

    bool names_ok = false;
    if (ok) {
        ok = names_ok = lsf_names_init(&c.names, mod, err, err_len);
        c.nodes = calloc(mn + c.stats.added_nodes, sizeof *c.nodes);
        c.attrs = calloc(mod->attr_count + c.stats.added_attrs + 1, sizeof *c.attrs);
        if (!c.nodes || !c.attrs) ok = refuse(&c, "out of memory");
    }
    if (ok) ok = emit_node(&c, mod_root, -1);
    const LsfAttr **order = NULL;
    if (ok) {
        /* the mod's kept attributes in storage order, then the attributes added to matched nodes */
        order = calloc(ma, sizeof *order);
        if (!order) ok = refuse(&c, "out of memory");
    }
    if (ok) {
        uint32_t n = 0;
        for (uint32_t i = 0; i < mn; i++) {
            if (c.mod_match[i] < 0) continue;
            for (uint32_t k = 0; k < mod->nodes[i].attr_count; k++) {
                uint32_t ai = mod->nodes[i].attr_begin + k;
                if (c.mod_value[ai]) order[n++] = &mod->attrs[ai];
            }
        }
        qsort(order, n, sizeof *order, cmp_index);
        for (uint32_t k = 0; k < n; k++) {
            const LsfAttr *a = order[k];
            uint32_t ai = (uint32_t)(a - mod->attrs), node = 0;
            while (!(ai >= mod->nodes[node].attr_begin && ai < mod->nodes[node].attr_begin + mod->nodes[node].attr_count)) node++;
            c.attrs[c.attr_count++] = (LsfOutAttr){ (uint32_t)c.mod_out[node], a->name_ref, a->type, c.mod_value[ai], a->size };
        }
        for (uint32_t i = 0; ok && i < cn; i++) {
            if (c.mac_match[i] < 0) continue;
            for (uint32_t k = 0; ok && k < mac->nodes[i].attr_count; k++) {
                uint32_t ai = mac->nodes[i].attr_begin + k;
                if (!c.mac_attr_added[ai]) continue;
                uint32_t ref;
                ok = lsf_names_ref(&c.names, mac->attrs[ai].name, &ref, err, err_len);
                if (ok) c.attrs[c.attr_count++] = (LsfOutAttr){ (uint32_t)c.mod_out[c.mac_match[i]], ref, mac->attrs[ai].type, mac->attrs[ai].value, mac->attrs[ai].size };
            }
        }
    }
    if (ok) ok = lsf_write(mod, &c.names, c.nodes, c.node_count, c.attrs, c.attr_count, out, out_size, err, err_len);
    if (stats) *stats = c.stats;
    free(order);
    free(c.nodes);
    free(c.attrs);
    if (names_ok) lsf_names_free(&c.names);
    free(c.mod_match);
    free(c.mac_match);
    free(c.mod_value);
    free(c.mac_attr_added);
    free(c.mod_out);
    free(c.mac_out);
    return ok;
}
