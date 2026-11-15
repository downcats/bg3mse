#pragma once

#include <cstddef>
#include <cstdint>

// What Norbyte's UI code does with NoesisGUI's objects and tables, compiled
// as his sources are (the SDK's inline code and his declarations in
// GameDefinitions/UI.h), behind plain functions: the objects are the bytes
// the test gives, the tables are made by his code and read back by the test
// as the game's code reads them (Layout/GameNoesis.h).

namespace bg3se::mac::test {

// The private members his code reaches through the SDK's inline accessors.
uint32_t NorbyteTypeSymbol(void const* type);
uint32_t NorbyteDependencyPropertySymbol(void const* property);
void const* NorbyteDependencyPropertyType(void const* property);
void const* NorbyteDependencyPropertyOwner(void const* property);
void const* NorbyteDependencyPropertyManager(void const* property);
bool NorbyteDependencyPropertyReadOnly(void const* property);
uint32_t NorbyteRoutedEventSymbol(void const* event);
void const* NorbyteRoutedEventOwner(void const* event);
int32_t NorbyteReferenceCount(void const* object);
int32_t NorbyteAddReference(void const* object);
// The sizes of his types and of the SDK's as he compiles them.
size_t NorbyteSize(char const* type);

// The symbol table and the type registry as his SymbolManagerInternals and
// ReflectionInternals hold them: a new, empty one; one more entry, as his
// AddString and RegisterType add it; the bytes; the end.
void* NorbyteSymbolsMake();
void NorbyteSymbolsAdd(void* symbols, char const* text);
void NorbyteSymbolsFree(void* symbols);
uint32_t NorbyteSymbolsFind(void const* symbols, char const* text);
void* NorbyteTypesMake();
void NorbyteTypesAdd(void* types, uint32_t symbol, void* type);
void NorbyteTypesFree(void* types);
// A DependencyObject's map of stored values and a UIElement's map of
// handlers, as the SDK's inline HashMap fills them.
void* NorbyteValuesMake();
void NorbyteValuesAdd(void* values, void const* property, void* value);
void* NorbyteValuesFind(void const* values, void const* property);
void NorbyteValuesFree(void* values);
void* NorbyteHandlersMake();
void NorbyteHandlersAdd(void* handlers, void const* event);
void NorbyteHandlersFree(void* handlers);
// The bytes of a Noesis::String holding `text`, into `out` (kNoesisStringBytes).
void NorbyteStringBytes(char const* text, void* out);
void NorbyteStringFree(void* string);

}
