#include <stdafx.h>
#include <GameDefinitions/Base/Base.h>
#include <GameDefinitions/Symbols.h>
#include <GameDefinitions/UI.h>

#include "NoesisNorbyte.h"

BEGIN_NS(mac::test)

using Symbols = Noesis::SymbolManagerInternals;
using Types = Noesis::ReflectionInternals;
// The maps' types as DependencyObject.h and UIElement.h declare the members
// (DependencyObject::Values, UIElement::RoutedEventMap, both private).
using Values = Noesis::HashMap<const Noesis::DependencyProperty*, Noesis::StoredValue*>;
using Handlers = Noesis::HashMap<const Noesis::RoutedEvent*, Noesis::RoutedEventHandler>;

uint32_t NorbyteTypeSymbol(void const* type)
{
    return (uint32_t)static_cast<Noesis::Type const*>(type)->GetTypeId();
}

uint32_t NorbyteDependencyPropertySymbol(void const* property)
{
    return (uint32_t)static_cast<Noesis::DependencyProperty const*>(property)->GetName();
}

void const* NorbyteDependencyPropertyType(void const* property)
{
    return static_cast<Noesis::DependencyProperty const*>(property)->GetType();
}

void const* NorbyteDependencyPropertyOwner(void const* property)
{
    return static_cast<Noesis::DependencyProperty const*>(property)->GetOwnerType();
}

void const* NorbyteDependencyPropertyManager(void const* property)
{
    return static_cast<Noesis::DependencyProperty const*>(property)->GetValueManager();
}

bool NorbyteDependencyPropertyReadOnly(void const* property)
{
    return static_cast<Noesis::DependencyProperty const*>(property)->IsReadOnly();
}

uint32_t NorbyteRoutedEventSymbol(void const* event)
{
    return (uint32_t)static_cast<Noesis::RoutedEvent const*>(event)->GetName();
}

void const* NorbyteRoutedEventOwner(void const* event)
{
    return static_cast<Noesis::RoutedEvent const*>(event)->GetOwnerType();
}

int32_t NorbyteReferenceCount(void const* object)
{
    return static_cast<Noesis::BaseRefCounted const*>(object)->GetNumReferences();
}

int32_t NorbyteAddReference(void const* object)
{
    // The SDK's inline AddReference is virtual in his headers; the count it
    // changes is the member GetNumReferences reads.
    return static_cast<Noesis::BaseRefCounted const*>(object)->BaseRefCounted::AddReference();
}

size_t NorbyteSize(char const* type)
{
    STDString name(type);
    if (name == "Symbols") return sizeof(Symbols);
    if (name == "Symbols.Keys") return offsetof(Symbols, Keys);
    if (name == "Types") return sizeof(Types);
    if (name == "Types.NameToType") return offsetof(Types, NameToType);
    if (name == "Values") return sizeof(Values);
    if (name == "Handlers") return sizeof(Handlers);
    if (name == "Delegate") return sizeof(Noesis::RoutedEventHandler);
    if (name == "String") return sizeof(Noesis::String);
    if (name == "StoredValue") return sizeof(Noesis::StoredValue);
    return 0;
}

void* NorbyteSymbolsMake()
{
    return new Symbols();
}

// His SymbolManager::AddString (Builtins.inl) without the lock: the same
// lookup, append and insert.
void NorbyteSymbolsAdd(void* symbols, char const* text)
{
    auto sym = static_cast<Symbols*>(symbols);
    if (sym->Keys.Find(text) != sym->Keys.End()) return;
    auto idx = sym->Strings.Size();
    auto keyCopy = _strdup(text);
    sym->Strings.PushBack(keyCopy);
    sym->Keys.Insert(keyCopy, idx);
}

uint32_t NorbyteSymbolsFind(void const* symbols, char const* text)
{
    auto sym = static_cast<Symbols const*>(symbols);
    auto it = sym->Keys.Find(text);
    return it != sym->Keys.End() ? it->value : 0xffffffffu;
}

void NorbyteSymbolsFree(void* symbols)
{
    auto sym = static_cast<Symbols*>(symbols);
    for (auto text : sym->Strings) free(const_cast<char*>(text));
    delete sym;
}

void* NorbyteTypesMake()
{
    return new Types();
}

// His Reflection::RegisterType (Builtins.inl) without the lock.
void NorbyteTypesAdd(void* types, uint32_t symbol, void* type)
{
    auto reflection = static_cast<Types*>(types);
    reflection->NameToType.Insert(symbol, static_cast<Noesis::Type*>(type));
    reflection->Names.PushBack(Noesis::Symbol(symbol));
}

void NorbyteTypesFree(void* types)
{
    delete static_cast<Types*>(types);
}

void* NorbyteValuesMake()
{
    return new Values();
}

void NorbyteValuesAdd(void* values, void const* property, void* value)
{
    static_cast<Values*>(values)->Insert(static_cast<Noesis::DependencyProperty const*>(property),
        static_cast<Noesis::StoredValue*>(value));
}

void* NorbyteValuesFind(void const* values, void const* property)
{
    auto map = static_cast<Values const*>(values);
    auto it = map->Find(static_cast<Noesis::DependencyProperty const*>(property));
    return it != map->End() ? it->value : nullptr;
}

void NorbyteValuesFree(void* values)
{
    delete static_cast<Values*>(values);
}

void* NorbyteHandlersMake()
{
    return new Handlers();
}

void NorbyteHandlersAdd(void* handlers, void const* event)
{
    static_cast<Handlers*>(handlers)->Insert(static_cast<Noesis::RoutedEvent const*>(event), Noesis::RoutedEventHandler());
}

void NorbyteHandlersFree(void* handlers)
{
    delete static_cast<Handlers*>(handlers);
}

void NorbyteStringBytes(char const* text, void* out)
{
    new (out) Noesis::String(text);
}

void NorbyteStringFree(void* string)
{
    using NoesisString = Noesis::String;
    static_cast<NoesisString*>(string)->~NoesisString();
}

END_NS()
