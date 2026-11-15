#pragma once

#include <cstdint>
#include <cstring>
#include <iterator>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <vector>

// Test data shared by the test executables.

// A small arm64 Mach-O file: __TEXT with __text and __stubs, __DATA_CONST with a
// vtable in its __const section, and a symbol table with two imports behind
// the stubs.
struct FakeBinary
{
    static constexpr uint64_t Base = 0x100000000, Text = Base + 0x1000, Stubs = Base + 0x2000, Data = Base + 0x4000;
    static constexpr size_t SymbolTable = 0x5000, MaxSymbols = 16;
    std::vector<uint8_t> File = std::vector<uint8_t>(0x6000, 0);

    // Sect: 1 = __text, 2 = __stubs, 3 = __const.
    struct Sym { char const* Name; uint64_t Value; uint8_t Type, Sect; };

    template <class T> T* At(size_t offset) { return reinterpret_cast<T*>(File.data() + offset); }
    void Word(uint64_t va, uint32_t w) { memcpy(File.data() + (va - Base), &w, 4); }
    void Pointer(uint64_t va, uint64_t p) { memcpy(File.data() + 0x4000 + (va - Data), &p, 8); }
    // The i-th symbol: the six below, then `extra`.
    nlist_64* Symbol(size_t i) { return At<nlist_64>(SymbolTable + i * sizeof(nlist_64)); }

    explicit FakeBinary(bool chainedFixups = false, std::vector<Sym> const& extra = {})
    {
        auto mh = At<mach_header_64>(0);
        mh->magic = MH_MAGIC_64;
        mh->cputype = CPU_TYPE_ARM64;
        mh->filetype = MH_EXECUTE;
        size_t at = sizeof(*mh);

        auto text = At<segment_command_64>(at);
        text->cmd = LC_SEGMENT_64;
        text->cmdsize = sizeof(*text) + 2 * sizeof(section_64);
        strcpy(text->segname, "__TEXT");
        text->vmaddr = Base;
        text->vmsize = text->filesize = 0x4000;
        text->nsects = 2;
        auto sect = reinterpret_cast<section_64*>(text + 1);
        strcpy(sect[0].sectname, "__text");
        strcpy(sect[0].segname, "__TEXT");
        sect[0].addr = Text;
        sect[0].size = 0x100;
        sect[0].flags = S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS;
        strcpy(sect[1].sectname, "__stubs");
        strcpy(sect[1].segname, "__TEXT");
        sect[1].addr = Stubs;
        sect[1].size = 24;
        sect[1].flags = S_SYMBOL_STUBS | S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS;
        sect[1].reserved1 = 0;
        sect[1].reserved2 = 12;
        at += text->cmdsize;

        auto data = At<segment_command_64>(at);
        data->cmd = LC_SEGMENT_64;
        data->cmdsize = sizeof(*data) + sizeof(section_64);
        strcpy(data->segname, "__DATA_CONST");
        data->vmaddr = Data;
        data->vmsize = 0x1000;
        data->fileoff = 0x4000;
        data->filesize = 0x800;
        data->nsects = 1;
        auto constants = reinterpret_cast<section_64*>(data + 1);
        strcpy(constants->sectname, "__const");
        strcpy(constants->segname, "__DATA_CONST");
        constants->addr = Data;
        constants->size = 0x800;
        constants->offset = 0x4000;
        at += data->cmdsize;

        // Symbols: four defined, two imported; the stubs use the imports.
        std::vector<Sym> syms = {
            { "_first", Text, N_SECT | N_EXT, 1 },
            { "_second", Text + 0x40, N_SECT | N_EXT, 1 },
            { "_vtable", Data, N_SECT, 3 },
            { "_after", Data + 0x30, N_SECT, 3 },
            { "_longjmp", 0, N_UNDF | N_EXT, 0 },
            { "_setjmp", 0, N_UNDF | N_EXT, 0 },
        };
        syms.insert(syms.end(), extra.begin(), extra.end());
        if (syms.size() > MaxSymbols) syms.resize(MaxSymbols);
        constexpr size_t kSymOff = SymbolTable, kStrOff = 0x5100, kIndOff = 0x5800;
        uint32_t strx = 1;
        for (size_t i = 0; i < std::size(syms); i++) {
            auto n = Symbol(i);
            n->n_un.n_strx = strx;
            n->n_type = syms[i].Type;
            n->n_sect = syms[i].Sect;
            n->n_value = syms[i].Value;
            strcpy(At<char>(kStrOff + strx), syms[i].Name);
            strx += (uint32_t)strlen(syms[i].Name) + 1;
        }
        *At<uint32_t>(kIndOff) = 5;         // stub 0: _setjmp
        *At<uint32_t>(kIndOff + 4) = 4;     // stub 1: _longjmp

        auto symtab = At<symtab_command>(at);
        symtab->cmd = LC_SYMTAB;
        symtab->cmdsize = sizeof(*symtab);
        symtab->symoff = kSymOff;
        symtab->nsyms = (uint32_t)std::size(syms);
        symtab->stroff = kStrOff;
        symtab->strsize = strx;
        at += sizeof(*symtab);

        auto dysymtab = At<dysymtab_command>(at);
        dysymtab->cmd = LC_DYSYMTAB;
        dysymtab->cmdsize = sizeof(*dysymtab);
        dysymtab->indirectsymoff = kIndOff;
        dysymtab->nindirectsyms = 2;
        at += sizeof(*dysymtab);

        mh->ncmds = 4;
        if (chainedFixups) {
            // Pointers on disk are then fixup chains, not addresses.
            auto fixups = At<linkedit_data_command>(at);
            fixups->cmd = LC_DYLD_CHAINED_FIXUPS;
            fixups->cmdsize = sizeof(*fixups);
            at += sizeof(*fixups);
            mh->ncmds++;
        }
        mh->sizeofcmds = (uint32_t)(at - sizeof(*mh));

        // _first: ldr x8,[x19,#0x2000]; add x0,x8,#0x1f0; bl _longjmp; ldr x8,[x19,#0x2000]; add x0,x8,#0x10; ret
        Word(Text + 0x00, 0xf9500268);
        Word(Text + 0x04, 0x9107c100);
        Word(Text + 0x08, 0x94000000 | (uint32_t)((Stubs + 12 - (Text + 8)) / 4));
        Word(Text + 0x0c, 0xf9500268);
        Word(Text + 0x10, 0x91004100);
        Word(Text + 0x14, 0xd65f03c0);
        // _second: bl _setjmp; bl _first; ret
        Word(Text + 0x40, 0x94000000 | (uint32_t)((Stubs - (Text + 0x40)) / 4));
        Word(Text + 0x44, 0x94000000 | (uint32_t)((uint64_t)(Text - (Text + 0x44)) / 4 & 0x03ffffff));
        Word(Text + 0x48, 0xd65f03c0);
        // The vtable: offset to top, RTTI, then _first, _second, _first.
        Pointer(Data + 0x10, Text);
        Pointer(Data + 0x18, Text + 0x40);
        Pointer(Data + 0x20, Text);
    }
};
