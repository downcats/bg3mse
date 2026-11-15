/**
 * bg3mse - Mach-O symbol table lookup
 *
 * One pass over the nlist table hashes each defined symbol's name against a
 * small open-addressing set of the wanted names. The game's table has about
 * 770k symbols and 70 MB of names; it lives in __LINKEDIT, which dyld maps
 * with the rest of the image, so nothing is read from disk in-process.
 */

#include "macho_symbols.h"

#include <mach-o/dyld.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <libkern/OSByteOrder.h>
#include <stdlib.h>
#include <string.h>

/* Load commands the table is built from. */
typedef struct {
    const struct symtab_command *symtab;
    const struct dysymtab_command *dysymtab;
    const struct segment_command_64 *text;
    const struct segment_command_64 *linkedit;
    const struct uuid_command *uuid;
} MachoCommands;

static bool read_commands(const struct mach_header_64 *mh, size_t limit,
                          MachoCommands *cmds) {
    memset(cmds, 0, sizeof(*cmds));
    if (mh->magic != MH_MAGIC_64) return false;
    size_t end = sizeof(*mh) + (size_t)mh->sizeofcmds;
    if (limit && end > limit) return false;

    const uint8_t *p = (const uint8_t *)(mh + 1);
    const uint8_t *stop = (const uint8_t *)mh + end;
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)p;
        if (p + sizeof(*lc) > stop || lc->cmdsize < sizeof(*lc) ||
            p + lc->cmdsize > stop) {
            return false;
        }
        if (lc->cmd == LC_SYMTAB && lc->cmdsize >= sizeof(struct symtab_command)) {
            cmds->symtab = (const struct symtab_command *)p;
        } else if (lc->cmd == LC_DYSYMTAB && lc->cmdsize >= sizeof(struct dysymtab_command)) {
            cmds->dysymtab = (const struct dysymtab_command *)p;
        } else if (lc->cmd == LC_UUID && lc->cmdsize >= sizeof(struct uuid_command)) {
            cmds->uuid = (const struct uuid_command *)p;
        } else if (lc->cmd == LC_SEGMENT_64 &&
                   lc->cmdsize >= sizeof(struct segment_command_64)) {
            const struct segment_command_64 *seg = (const struct segment_command_64 *)p;
            if (strncmp(seg->segname, SEG_TEXT, sizeof(seg->segname)) == 0) {
                cmds->text = seg;
            } else if (strncmp(seg->segname, SEG_LINKEDIT, sizeof(seg->segname)) == 0) {
                cmds->linkedit = seg;
            }
        }
        p += lc->cmdsize;
    }
    return cmds->symtab && cmds->text;
}

/* base + file offset reaches the symbol and string tables. limit bounds the
 * tables by file offset (0 = trust the load commands). */
static bool fill_symtab(const struct mach_header_64 *mh, const MachoCommands *cmds,
                        const uint8_t *base, uint64_t limit, MachoSymtab *out) {
    const struct symtab_command *sc = cmds->symtab;
    uint64_t sym_end = (uint64_t)sc->symoff + (uint64_t)sc->nsyms * sizeof(struct nlist_64);
    uint64_t str_end = (uint64_t)sc->stroff + sc->strsize;
    if (limit && (sym_end > limit || str_end > limit)) return false;
    if (sc->strsize == 0) return false;

    memset(out, 0, sizeof(*out));
    out->header = mh;
    out->symbols = base + sc->symoff;
    out->nsyms = sc->nsyms;
    out->strings = (const char *)(base + sc->stroff);
    out->strsize = sc->strsize;
    out->text_vmaddr = cmds->text->vmaddr;
    if (cmds->uuid) memcpy(out->uuid, cmds->uuid->uuid, sizeof(out->uuid));

    const struct dysymtab_command *ds = cmds->dysymtab;
    if (ds && ds->nindirectsyms &&
        (!limit || (uint64_t)ds->indirectsymoff + (uint64_t)ds->nindirectsyms * 4 <= limit)) {
        out->indirect = (const uint32_t *)(base + ds->indirectsymoff);
        out->nindirect = ds->nindirectsyms;
    }
    return true;
}

bool macho_symtab_from_image(const void *header, intptr_t slide, MachoSymtab *out) {
    if (!header || !out) return false;
    const struct mach_header_64 *mh = header;
    MachoCommands cmds;
    if (!read_commands(mh, 0, &cmds) || !cmds.linkedit) return false;

    /* The tables sit in __LINKEDIT; its file offset maps to its vmaddr. */
    const struct segment_command_64 *le = cmds.linkedit;
    const struct symtab_command *sc = cmds.symtab;
    uint64_t le_end = le->fileoff + le->filesize;
    if (sc->symoff < le->fileoff || sc->stroff < le->fileoff ||
        (uint64_t)sc->stroff + sc->strsize > le_end ||
        (uint64_t)sc->symoff + (uint64_t)sc->nsyms * sizeof(struct nlist_64) > le_end) {
        return false;
    }
    const struct dysymtab_command *ds = cmds.dysymtab;
    if (ds && (ds->indirectsymoff < le->fileoff ||
               (uint64_t)ds->indirectsymoff + (uint64_t)ds->nindirectsyms * 4 > le_end)) {
        cmds.dysymtab = NULL;
    }
    const uint8_t *base = (const uint8_t *)(uintptr_t)(le->vmaddr + (uint64_t)slide - le->fileoff);
    return fill_symtab(mh, &cmds, base, 0, out);
}

bool macho_symtab_main_image(MachoSymtab *out) {
    uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; i++) {
        const struct mach_header *mh = _dyld_get_image_header(i);
        if (mh && mh->filetype == MH_EXECUTE) {
            return macho_symtab_from_image(mh, _dyld_get_image_vmaddr_slide(i), out);
        }
    }
    return false;
}

bool macho_symtab_from_file(const void *data, size_t size, MachoSymtab *out) {
    if (!data || !out || size < sizeof(struct mach_header_64)) return false;
    const uint8_t *slice = data;
    uint64_t slice_size = size;

    uint32_t magic = OSReadBigInt32(data, 0);
    if (magic == FAT_MAGIC || magic == FAT_MAGIC_64) {
        bool wide = magic == FAT_MAGIC_64;
        uint32_t narch = OSReadBigInt32(data, 4);
        size_t entry = wide ? sizeof(struct fat_arch_64) : sizeof(struct fat_arch);
        if (sizeof(struct fat_header) + (uint64_t)narch * entry > size) return false;
        slice = NULL;
        for (uint32_t i = 0; i < narch; i++) {
            const uint8_t *fa = (const uint8_t *)data + sizeof(struct fat_header) + i * entry;
            if ((cpu_type_t)OSReadBigInt32(fa, 0) != CPU_TYPE_ARM64) continue;
            uint64_t off = wide ? OSReadBigInt64(fa, 8) : OSReadBigInt32(fa, 8);
            uint64_t len = wide ? OSReadBigInt64(fa, 16) : OSReadBigInt32(fa, 12);
            if (off + len > size || len < sizeof(struct mach_header_64)) return false;
            slice = (const uint8_t *)data + off;
            slice_size = len;
            break;
        }
        if (!slice) return false;
    }

    const struct mach_header_64 *mh = (const struct mach_header_64 *)slice;
    if (mh->cputype != CPU_TYPE_ARM64) return false;
    MachoCommands cmds;
    if (!read_commands(mh, (size_t)slice_size, &cmds)) return false;
    return fill_symtab(mh, &cmds, slice, slice_size, out);
}

/* ------------------------------------------------------------------------ */
/* Batch resolve                                                            */
/* ------------------------------------------------------------------------ */

#define FNV_OFFSET 0xcbf29ce484222325ULL
#define FNV_PRIME 0x100000001b3ULL

static uint64_t hash_name(const char *s) {
    uint64_t h = FNV_OFFSET;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * FNV_PRIME;
    return h;
}

size_t macho_symtab_resolve(const MachoSymtab *st, const char *const *names,
                            size_t count, uint64_t *out_va,
                            uint8_t *out_status) {
    if (!st || !names || !out_va || count == 0) return 0;
    memset(out_va, 0, count * sizeof(*out_va));
    if (out_status) memset(out_status, MACHO_SYM_MISSING, count);

    size_t cap = 16;
    while (cap < count * 2) cap <<= 1;
    uint32_t *slots = calloc(cap, sizeof(*slots));      /* name index + 1 */
    uint64_t *hashes = calloc(count, sizeof(*hashes));
    uint32_t *canon = calloc(count, sizeof(*canon));    /* first identical name */
    uint8_t *status = calloc(count, sizeof(*status));
    if (!slots || !hashes || !canon || !status) {
        free(slots); free(hashes); free(canon); free(status);
        return 0;
    }

    for (size_t i = 0; i < count; i++) {
        canon[i] = (uint32_t)i;
        if (!names[i] || !names[i][0]) continue;
        hashes[i] = hash_name(names[i]);
        size_t s = hashes[i] & (cap - 1);
        for (;; s = (s + 1) & (cap - 1)) {
            if (!slots[s]) { slots[s] = (uint32_t)i + 1; break; }
            uint32_t j = slots[s] - 1;
            if (hashes[j] == hashes[i] && strcmp(names[j], names[i]) == 0) {
                canon[i] = j;
                break;
            }
        }
    }

    const struct nlist_64 *syms = st->symbols;
    for (uint32_t k = 0; k < st->nsyms; k++) {
        const struct nlist_64 *n = &syms[k];
        if ((n->n_type & N_STAB) || (n->n_type & N_TYPE) != N_SECT) continue;
        uint32_t strx = n->n_un.n_strx;
        if (strx == 0 || strx >= st->strsize) continue;

        const char *name = st->strings + strx;
        const char *end = st->strings + st->strsize;
        uint64_t h = FNV_OFFSET;
        const char *c = name;
        for (; c < end && *c; c++) h = (h ^ (uint8_t)*c) * FNV_PRIME;
        if (c == end) continue;  /* unterminated */

        for (size_t s = h & (cap - 1); slots[s]; s = (s + 1) & (cap - 1)) {
            uint32_t j = slots[s] - 1;
            if (hashes[j] != h || strcmp(names[j], name) != 0) continue;
            if (status[j] == MACHO_SYM_MISSING) {
                status[j] = MACHO_SYM_FOUND;
                out_va[j] = n->n_value;
            } else if (status[j] == MACHO_SYM_FOUND && out_va[j] != n->n_value) {
                status[j] = MACHO_SYM_AMBIGUOUS;
            }
            break;
        }
    }

    size_t resolved = 0;
    for (size_t i = 0; i < count; i++) {
        uint32_t j = canon[i];
        uint8_t st_i = status[j];
        out_va[i] = st_i == MACHO_SYM_FOUND ? out_va[j] : 0;
        if (out_status) out_status[i] = st_i;
        if (st_i == MACHO_SYM_FOUND) resolved++;
    }

    free(slots); free(hashes); free(canon); free(status);
    return resolved;
}

size_t macho_symtab_resolve_got(const MachoSymtab *st, const char *const *names,
                                size_t count, uint64_t *out_slot_va,
                                uint8_t *out_status) {
    if (!st || !names || !out_slot_va || count == 0) return 0;
    memset(out_slot_va, 0, count * sizeof(*out_slot_va));
    uint8_t *status = calloc(count, sizeof(*status));
    if (!status) return 0;

    const struct mach_header_64 *mh = st->header;
    const struct nlist_64 *syms = st->symbols;
    const uint8_t *p = (const uint8_t *)(mh + 1);
    for (uint32_t i = 0; st->indirect && i < mh->ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)p;
        p += lc->cmdsize;
        if (lc->cmd != LC_SEGMENT_64) continue;
        const struct segment_command_64 *seg = (const struct segment_command_64 *)lc;
        const struct section_64 *sect = (const struct section_64 *)(seg + 1);
        for (uint32_t j = 0; j < seg->nsects; j++, sect++) {
            if ((sect->flags & SECTION_TYPE) != S_NON_LAZY_SYMBOL_POINTERS) continue;
            uint64_t slots = sect->size / sizeof(uint64_t);
            for (uint64_t k = 0; k < slots; k++) {
                uint64_t ix = (uint64_t)sect->reserved1 + k;
                if (ix >= st->nindirect) break;
                uint32_t symidx = st->indirect[ix];
                if (symidx & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS)) continue;
                if (symidx >= st->nsyms) continue;
                uint32_t strx = syms[symidx].n_un.n_strx;
                if (strx == 0 || strx >= st->strsize) continue;
                const char *name = st->strings + strx;
                uint64_t slot = sect->addr + k * sizeof(uint64_t);
                for (size_t n = 0; n < count; n++) {
                    if (!names[n] || strncmp(names[n], name, st->strsize - strx) != 0) continue;
                    if (status[n] == MACHO_SYM_MISSING) {
                        status[n] = MACHO_SYM_FOUND;
                        out_slot_va[n] = slot;
                    } else if (status[n] == MACHO_SYM_FOUND && out_slot_va[n] != slot) {
                        status[n] = MACHO_SYM_AMBIGUOUS;
                    }
                }
            }
        }
    }

    size_t resolved = 0;
    for (size_t n = 0; n < count; n++) {
        if (status[n] != MACHO_SYM_FOUND) out_slot_va[n] = 0;
        else resolved++;
        if (out_status) out_status[n] = status[n];
    }
    free(status);
    return resolved;
}
