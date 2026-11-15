#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include <Binding/CallAdapters.h>

// Four functions of the stats that Norbyte's code calls through StaticSymbols
// and that the macOS game has only inline in their callers: there is no code
// to call. Each has a function here, of Norbyte's prototype, that does what
// the inlined code does, on the game's own structures and through the game
// functions that code calls.
//
// Each was written from the code of the functions its engine table entry
// names as hosts (game build 4.1.1.7631656), and the entry records the shape
// of that code: StaticSymbols holds the function only while the entry
// confirms that the game still has no such function, that the hosts are
// unchanged, and that every game function it calls resolved with the
// argument passing stated here. Otherwise the member stays null, which
// Norbyte's callers test.
//
// The types are plain ones with the passing of Norbyte's, as in
// Binding/CallAdapters.h; the offsets are those of Mac/Layout, which
// Layout/DefinitionChecks.cpp ties to his declarations.

namespace bg3se::mac::binding {

namespace adapter {

// eoc__ParseStaticBoosts: stats::ParseStaticBoostsProc. Parses the text of a
// Boosts attribute and appends the GUID of each boost's prototype to
// `boosts` (an array of GUIDs). `filter` is an ls::Function<bool
// (FixedString const&)> that may refuse a boost by its name; one that holds
// nothing (its first word null, as the zeroed block Norbyte passes) refuses
// none.
bool ParseStaticBoosts(GameStringView& text, void* boosts, void* filter);

// stats__Object__SetPropertyString: stats::Object__SetPropertyStringProc.
// Sets the attribute `name` of a stats object from the text of its value, as
// a `data` line of a stats file does: by the type of the attribute, a
// position in one of the stats' pools, a condition, functors or roll
// conditions by text key, requirements, or the AI flags.
void StatsObjectSetPropertyString(void* object, uint32_t const* name, char const* value);

// eoc__PassivePrototype__Init: stats::PassivePrototype__InitProc. Fills a
// passive's prototype from its stats object: the prototype is cleaned, and
// then every member is set from the object's attributes, except that the
// bits of the Properties attribute are added to the ones it has.
void PassivePrototypeInit(void* prototype, void* object);

// eoc__InterruptPrototype__Init: stats::InterruptPrototype__InitProc. Fills
// an interrupt's prototype from its stats object. A functor list of the
// prototype is replaced only when the object has functors for it.
void InterruptPrototypeInit(void* prototype, void* object);

// --- the game functions, as the functions above call them --------------------

// (anonymous namespace)::ParseBoosts: the text and its length apart, and the
// function object called for each boost.
using ParseBoosts = bool(char const* text, uint32_t length, void* function);
// The three methods of the function object eoc::ParseStaticBoosts hands to
// ParseBoosts. Call is the game's to call; MoveDestroy is what the caller
// ends the object with.
using BoostCall = void(void* methods, void const* text, void const* parameters, void const* name, void const* arguments);
using BoostCopy = void*(void* methods, void const* from, void* to);
using BoostMoveDestroy = void*(void* methods, void* from, void* to);


// What ls::Split<2> returns: the text before the first delimiter, and the rest.
struct GameSplit
{
    GameStringView First;
    GameStringView Rest;
};

// CNamedElementManager<CRPGStats_Modifier>::GetHandleByName and GetByHandle:
// a list of modifiers looks an attribute up by its name, and gives the modifier.
using ModifierHandleByName = int32_t(void const* list, uint32_t const* name);
using ModifierByHandle = void*(void const* list, int32_t handle);
// CRPGStats_Modifier::GetPosition: here its first argument is the index of
// the modifier's list of values, not a modifier.
using ModifierGetPosition = int32_t(int32_t valueList, char const* text);
// RPGStats::AddCondition.
using StatsAddCondition = int32_t(void* stats, void const* text);
// ParseRequirements(ls::STDString const&, ls::DynamicArray<Requirement>&).
using StatsParseRequirements = void(void const* text, void* requirements);
// ls::STDString: from a C string, from a view (which arrives as its two
// members), trimmed, ended.
using StringFromText = void*(void* string, char const* text);
using StringFromView = void*(void* string, char const* text, uint32_t length);
using StringTrim = void(void* string);
using StringEnd = void*(void* string);
// eoc::TextKeyParsingHelpers::SplitStringPerTextKeyWithMerge and ...NoMerge:
// the map of text key to text is built at the first argument.
using SplitPerTextKey = void(void* map, void const* text, uint32_t const* defaultKey);
// ls::HashMap<ls::FixedString, ls::STDString>::~HashMap.
using TextMapEnd = void*(void* map);
// ls::Split<2>.
using SplitInTwo = GameSplit(GameStringView const* text, GameStringView const* delimiters, bool flag);
// eoc::StatsFunctorListManager::CreateStatsFunctorList and
// eoc::StatsFunctorBase::ParseStatsFunctor.
using CreateFunctorList = void*(void* manager, void const* object, uint32_t const* attribute, uint32_t const* textKey);
using ParseFunctor = void*(void const* text, uint64_t index);
// A functor list's Insert, which the list's vtable holds (kFunctorListInsertSlot).
using FunctorListInsert = void(void* list, void* functor);
// ls::gst::Acquire(unsigned int): one more reference to a fixed string.
using FixedStringAcquire = void(uint32_t index);
// ls::FixedString::Create(char const*, int), move assignment and destructor.
using FixedStringCreate = void(uint32_t* result, char const* text, int32_t length);
using FixedStringMoveAssign = uint32_t*(uint32_t* to, uint32_t* from);
using FixedStringEnd = void*(uint32_t* string);
// ls::DynamicArray<T>::Reallocate(int) and ~DynamicArray, and
// ls::HashMap<ls::FixedString, ls::DynamicArray<T>>::SetUniversal, for the
// elements of a stats object's functors and of its roll conditions.
using ArrayReallocate = void(void* array, int32_t capacity);
using ArrayEnd = void*(void* array);
using ArrayMapSet = void(void* map, uint32_t const* key, void const* array);

// eoc::PassivePrototype::Clean() and eoc::StatsDescription::SetFromStatsObject.
using PassiveClean = void(void* prototype);
using DescriptionFromObject = void(void* description, void const* object);
// ls::FixedString::operator=(ls::FixedString const&): the string assigned
// arrives by value. ls::FixedString::GetHash() const: so does the string.
using FixedStringAssign = uint32_t*(uint32_t* to, uint32_t from);
using FixedStringHash = uint32_t(uint32_t string);
// eoc::action_resource::ParseActionResourceCostWithError.
using ParseResourceCosts = void(GameStringView const* text, void* costs);
// A manager's virtual functions, called through its vtable (the game's, or
// Norbyte's for a prototype his code made), and a functor's Clone.
using ManagerDestroy = void(void* manager);
using ManagerCount = uint64_t(void const* manager);
using ManagerEntry = void*(void* manager, uint64_t index);
using FunctorClone = void*(void const* functor);

}

template <> inline constexpr engine::Returns kComesBackIn<adapter::GameSplit> = engine::Returns::IndirectX8;

// The adapters above, for Binding/CallAdapters.cpp's list.
std::span<CallAdapter const> StatsFunctionAdapters();

}
