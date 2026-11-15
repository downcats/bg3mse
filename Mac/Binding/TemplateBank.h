#pragma once

#include <cstdint>

namespace bg3se::mac::engine { class Resolution; }

// Which of ls::GlobalTemplateManager's banks is the current one.
//
// On Windows Norbyte reads the index from the executable's thread-local
// block (StaticSymbols::GetGlobalTemplateBank, GameDefinitions/GameHelpers.cpp).
// The macOS game works it out as ls::GlobalTemplateManager::GetTemplateRaw
// does: a global byte is the index, unless it is 2, and then the index is a
// byte of the calling thread, reached through its thread-local descriptor.
// Both addresses are engine table entries, taken from that function's code.

namespace bg3se::mac::binding {

// The index, or -1 when the two entries are not bound or the game holds an
// index below zero.
int CurrentTemplateBankIndex();

// Takes the two addresses from a resolution of the running game. Without
// both, CurrentTemplateBankIndex gives -1.
void BindTemplateBank(engine::Resolution const& resolution);

// A Mach-O thread-local descriptor: its first word is the function that,
// called with the descriptor, returns the address of the calling thread's
// variable.
struct ThreadLocalDescriptor
{
    void* (*Address)(ThreadLocalDescriptor*);
    uintptr_t Key;
    uintptr_t Offset;
};

// The two entries of the engine table BindTemplateBank takes.
constexpr char const* kTemplateBankEntryNames[] = { "g_GlobalTemplateBankType", "tls_CurrentBankType" };

// For tests: the global byte and the descriptor; nullptr unbinds.
void SetTemplateBankForTest(signed char const* bankType, ThreadLocalDescriptor* threadBankType);

// The value of the global byte that means "the calling thread's".
constexpr signed char kBankTypeOfThread = 2;

}
