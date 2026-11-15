/**
 * bg3mse - Mach-O symbol table lookup
 *
 * The macOS game binary keeps its full symbol table, local symbols included,
 * so a game address is found by its exact mangled name instead of a number
 * baked in for one game build. A mangled function name encodes its parameter
 * types, so a changed signature fails to resolve rather than resolving to the
 * wrong ABI.
 *
 * The same code reads the symbol table of an image loaded in this process
 * (the game) or of a binary file mapped into memory (tests and tools).
 */

#ifndef BG3SE_MACHO_SYMBOLS_H
#define BG3SE_MACHO_SYMBOLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const void *header;        /* mach_header_64 of the image or file slice */
    const void *symbols;       /* nlist_64[nsyms] */
    uint32_t nsyms;
    const char *strings;
    uint32_t strsize;
    const uint32_t *indirect;  /* indirect symbol table, NULL when absent */
    uint32_t nindirect;
    uint64_t text_vmaddr;      /* unslid __TEXT vmaddr (0x100000000 for the game) */
    uint8_t uuid[16];          /* LC_UUID, all zero when absent */
} MachoSymtab;

typedef enum {
    MACHO_SYM_MISSING = 0,
    MACHO_SYM_FOUND = 1,
    MACHO_SYM_AMBIGUOUS = 2,   /* same name defined at two addresses */
} MachoSymStatus;

/** Symbol table of an image loaded by dyld (header and slide from dyld). */
bool macho_symtab_from_image(const void *header, intptr_t slide, MachoSymtab *out);

/** Symbol table of the main executable of this process. */
bool macho_symtab_main_image(MachoSymtab *out);

/** Symbol table of a binary file mapped at data. A fat file uses its arm64 slice. */
bool macho_symtab_from_file(const void *data, size_t size, MachoSymtab *out);

/**
 * Resolve many raw symbol names (as nm prints them, e.g. "__ZN2ls...E") in one
 * pass over the table. out_va[i] gets the unslid address, or 0 when the name
 * is missing or defined at more than one address. out_status may be NULL.
 * Returns the number of names resolved.
 */
size_t macho_symtab_resolve(const MachoSymtab *st, const char *const *names,
                            size_t count, uint64_t *out_va,
                            uint8_t *out_status);

/**
 * Like macho_symtab_resolve, but finds the non-lazy pointer (__got) slot that
 * holds each symbol's address, as code reaching an exported global from inside
 * the image loads it. A symbol with slots at two addresses is ambiguous.
 */
size_t macho_symtab_resolve_got(const MachoSymtab *st, const char *const *names,
                                size_t count, uint64_t *out_slot_va,
                                uint8_t *out_status);

#ifdef __cplusplus
}
#endif

#endif /* BG3SE_MACHO_SYMBOLS_H */
