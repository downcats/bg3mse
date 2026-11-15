#include <Binding/StatsFunctions.h>

#include <cstring>

#include <Layout/GameFiles.h>
#include <Layout/GameMembers.h>
#include <Layout/GameStats.h>

namespace bg3se::mac::binding {

namespace {

adapter::ParseBoosts* gParseBoosts{ nullptr };
adapter::BoostCall* gBoostCall{ nullptr };
adapter::BoostCopy* gBoostCopy{ nullptr };
adapter::BoostMoveDestroy* gBoostMoveDestroy{ nullptr };

// What stats::Object::SetPropertyString is made of.
void** gStats{ nullptr };                   // RPGStats::m_ptr
uint32_t** gDefaultTextKey{ nullptr };    // eoc::strDefault: a reference to a fixed string
// The names of the lists of values that are set in a way of their own.
uint32_t* gStatsFunctorsName{ nullptr };
uint32_t* gConditionsName{ nullptr };
uint32_t* gTargetConditionsName{ nullptr };
uint32_t* gUseConditionsName{ nullptr };
uint32_t* gRollConditionsName{ nullptr };
uint32_t* gAIFlagsName{ nullptr };
uint32_t* gConstantIntName{ nullptr };
uint32_t* gRequirementsName{ nullptr };
adapter::ModifierHandleByName* gModifierHandleByName{ nullptr };
adapter::ModifierByHandle* gModifierByHandle{ nullptr };
adapter::ModifierGetPosition* gModifierGetPosition{ nullptr };
adapter::StatsAddCondition* gAddCondition{ nullptr };
adapter::StatsParseRequirements* gParseRequirements{ nullptr };
adapter::StringFromText* gStringFromText{ nullptr };
adapter::StringFromView* gStringFromView{ nullptr };
adapter::StringTrim* gStringTrim{ nullptr };
adapter::StringEnd* gStringEnd{ nullptr };
adapter::SplitPerTextKey* gSplitPerTextKeyWithMerge{ nullptr };
adapter::SplitPerTextKey* gSplitPerTextKeyNoMerge{ nullptr };
adapter::TextMapEnd* gTextMapEnd{ nullptr };
adapter::SplitInTwo* gSplitInTwo{ nullptr };
adapter::CreateFunctorList* gCreateFunctorList{ nullptr };
adapter::ParseFunctor* gParseFunctor{ nullptr };
// The vtables of a list of modifiers and of a list of functors: not read
// here. Their entries state what the slots the inlined code calls through
// hold, which is what this code calls.
void* gModifierListVtable{ nullptr };
void* gFunctorListVtable{ nullptr };
adapter::FixedStringAcquire* gFixedStringAcquire{ nullptr };
adapter::FixedStringCreate* gFixedStringCreate{ nullptr };
adapter::FixedStringMoveAssign* gFixedStringMoveAssign{ nullptr };
adapter::FixedStringEnd* gFixedStringEnd{ nullptr };
adapter::ArrayReallocate* gFunctorGroupsReallocate{ nullptr };
adapter::ArrayEnd* gFunctorGroupsEnd{ nullptr };
adapter::ArrayMapSet* gFunctorGroupsSet{ nullptr };
adapter::ArrayReallocate* gRollConditionsReallocate{ nullptr };
adapter::ArrayEnd* gRollConditionsEnd{ nullptr };
adapter::ArrayMapSet* gRollConditionsSet{ nullptr };

// What the two prototype initialisers are made of besides.
adapter::GstGet* gFixedStringText{ nullptr };
adapter::PassiveClean* gPassiveClean{ nullptr };
adapter::DescriptionFromObject* gDescriptionFromObject{ nullptr };
adapter::FixedStringAssign* gFixedStringAssign{ nullptr };
adapter::FixedStringHash* gFixedStringHash{ nullptr };
adapter::ParseResourceCosts* gParseResourceCosts{ nullptr };
uint32_t* gEmptyFixedString{ nullptr };         // ls::FixedString::Empty
void* gDefaultTextKeyEntry{ nullptr };          // the registry's entry for "Default"
// The names of a passive's attributes. A name the game has as a reference
// is the address of the variable that holds the reference.
uint32_t** gToggleOnEffectName{ nullptr };
uint32_t** gToggleOffEffectName{ nullptr };
uint32_t* gPropertiesName{ nullptr };
uint32_t** gEnabledContextName{ nullptr };
uint32_t** gStatsFunctorContextName{ nullptr };
uint32_t** gEnabledConditionsName{ nullptr };
uint32_t* gBoostConditionsName{ nullptr };
uint32_t** gToggleOnFunctorsName{ nullptr };
uint32_t** gToggleOffFunctorsName{ nullptr };
uint32_t* gBoostContextName{ nullptr };
uint32_t* gBoostsName{ nullptr };
uint32_t** gToggleOffContextName{ nullptr };
uint32_t** gToggleGroupName{ nullptr };
uint32_t** gPriorityOrderName{ nullptr };
uint32_t** gTooltipConditionalDamageName{ nullptr };
// And of an interrupt's.
uint32_t** gInterruptContextName{ nullptr };
uint32_t** gInterruptContextScopeName{ nullptr };
uint32_t** gInterruptFlagsName{ nullptr };
uint32_t** gInterruptConditionsName{ nullptr };
uint32_t** gContainerName{ nullptr };
uint32_t** gRollName{ nullptr };
uint32_t** gInterruptPropertiesName{ nullptr };
uint32_t** gSuccessName{ nullptr };
uint32_t** gFailureName{ nullptr };
uint32_t** gCostName{ nullptr };
uint32_t** gStackName{ nullptr };
uint32_t** gInterruptDefaultValueName{ nullptr };
uint32_t** gEnableConditionName{ nullptr };
uint32_t** gEnableContextName{ nullptr };

template <class T>
T ReadAt(void const* object, size_t offset)
{
    T value;
    memcpy(&value, static_cast<char const*>(object) + offset, sizeof(value));
    return value;
}

template <class T>
void WriteAt(void* object, size_t offset, T value)
{
    memcpy(static_cast<char*>(object) + offset, &value, sizeof(value));
}

// An ls::STDString of the game's (Layout/GameFiles.h), made and ended by the
// game's own functions.
struct GameString
{
    alignas(8) char Bytes[kGameStringBytes];

    GameString() = default;
    GameString(GameString const&) = delete;
    GameString& operator = (GameString const&) = delete;
};

// The characters of a game string and their number, as the game's code
// reads them: behind the pointer when the last byte is negative.
GameStringView TextOf(void const* string)
{
    auto flag = ReadAt<int8_t>(string, kGameStringFlag);
    if (flag < 0) return { ReadAt<char const*>(string, 0), ReadAt<uint32_t>(string, kGameStringLongLength) };
    return { static_cast<char const*>(string), static_cast<uint8_t>(flag) };
}

// An ls::DynamicArray: the elements, 32 bits of capacity, 32 bits of size.
struct GameArray
{
    void* Elements{ nullptr };
    uint32_t Capacity{ 0 };
    uint32_t Size{ 0 };
};
static_assert(offsetof(GameArray, Capacity) == kArrayCapacity && offsetof(GameArray, Size) == kArraySize && sizeof(GameArray) == 0x10);

// Room for one more element, as the game's code makes it: twice the
// capacity, or one more than the size where that is more.
void GrowForOne(GameArray& array, adapter::ArrayReallocate* reallocate)
{
    auto capacity = static_cast<int32_t>(array.Capacity), size = static_cast<int32_t>(array.Size);
    if (capacity > size) return;
    int32_t wanted = 2 * capacity > size + 1 ? 2 * capacity : size + 1;
    if (capacity < wanted) reallocate(&array, wanted);
}

// The parts of a text between delimiters, walked as the game's code walks
// them with ls::Split<2>: the first part is split off the rest, until
// neither a part nor a rest is left. The count never reaches the limit the
// game gives it.
struct SplitParts
{
    GameStringView Delimiters;
    GameStringView Part{ nullptr, 0 };
    GameStringView Rest;
    int32_t Count{ 0 };
    int32_t Limit{ 0x7fffffff };
    bool Flag{ false };

    SplitParts(GameStringView text, GameStringView delimiters) : Delimiters(delimiters), Rest(text) { Split(false); }

    bool More() const { return Rest.Text != nullptr || Part.Text != nullptr; }

    void Next()
    {
        if (Count < Limit) {
            Split(Flag);
        } else {
            Part = Rest;
            Rest = { nullptr, 0 };
        }
    }

private:
    void Split(bool flag)
    {
        auto split = gSplitInTwo(&Rest, &Delimiters, flag);
        Part = split.First;
        Rest = split.Rest;
        Count++;
    }
};

// StatsFunctors: the text is split by text key; each key gets a list of
// functors of its own in the stats, filled from the parts of its text
// between semicolons; the object's entry for the attribute becomes the keys
// with their lists.
void SetStatsFunctors(void* stats, void* object, uint32_t const* name, char const* value)
{
    GameString text;
    gStringFromText(&text, value);
    alignas(8) char parts[kHashMapSize];
    gSplitPerTextKeyWithMerge(parts, &text, *gDefaultTextKey);
    gStringEnd(&text);

    GameArray groups;
    auto keys = ReadAt<uint32_t const*>(parts, kHashMapKeys);
    auto count = ReadAt<int32_t>(parts, kHashMapKeysSize);
    auto texts = ReadAt<char const*>(parts, kHashMapValues);
    void* manager = static_cast<char*>(stats) + kStatsFunctors;
    // Functors are numbered through all the keys of the attribute.
    uint64_t index = 0;
    for (int32_t n = 0; n < count; n++) {
        // The game first looks the object's own name up among the stats'
        // functor lists, and leaves the key out when a list has that name.
        auto objectName = ReadAt<uint32_t>(object, kStatsObjectName);
        auto buckets = ReadAt<uint32_t>(manager, kMapBucketCount);
        bool named = false;
        if (buckets != 0) {
            auto node = ReadAt<void const* const*>(manager, kMapBuckets)[objectName % buckets];
            for (; node != nullptr; node = ReadAt<void const*>(node, kMapNodeNext)) {
                if (ReadAt<uint32_t>(node, kMapNodeKey) != objectName) continue;
                named = ReadAt<void const*>(node, kMapNodePointerValue) != nullptr;
                break;
            }
        }
        if (named) continue;

        uint32_t const* textKey = keys + n;
        void* list = gCreateFunctorList(manager, object, name, textKey);
        for (SplitParts split(TextOf(texts + n * kGameStringBytes), { ";", 1 }); split.More(); split.Next()) {
            GameString part;
            gStringFromView(&part, split.Part.Text, split.Part.Length);
            gStringTrim(&part);
            if (TextOf(&part).Length != 0) {
                // The game does not test the list it was given; without one there is nothing to insert into.
                if (void* functor = gParseFunctor(&part, index); functor && list) {
                    auto insert = ReadAt<adapter::FunctorListInsert*>(ReadAt<void const*>(list, 0), kFunctorListInsertSlot);
                    insert(list, functor);
                }
                index++;
            }
            gStringEnd(&part);
        }

        // The element holds a reference to its key.
        uint32_t key = *textKey;
        if (key != 0xffffffffu) gFixedStringAcquire(key);
        GrowForOne(groups, gFunctorGroupsReallocate);
        char* element = static_cast<char*>(groups.Elements) + groups.Size * kFunctorGroupSize;
        groups.Size++;
        WriteAt(element, kFunctorGroupTextKey, key);
        WriteAt(element, kFunctorGroupList, list);
    }

    gFunctorGroupsSet(static_cast<char*>(object) + kStatsObjectFunctors, name, &groups);
    gFunctorGroupsEnd(&groups);
    gTextMapEnd(parts);
}

// RollConditions: the text is split by text key, each key's text becomes a
// condition of the stats, and the object's entry for the attribute becomes
// the keys with their conditions.
void SetRollConditions(void* stats, void* object, uint32_t const* name, char const* value)
{
    GameString text;
    gStringFromText(&text, value);
    alignas(8) char parts[kHashMapSize];
    gSplitPerTextKeyNoMerge(parts, &text, *gDefaultTextKey);
    gStringEnd(&text);

    GameArray conditions;
    auto keys = ReadAt<uint32_t const*>(parts, kHashMapKeys);
    auto count = ReadAt<int32_t>(parts, kHashMapKeysSize);
    auto texts = ReadAt<char const*>(parts, kHashMapValues);
    for (int32_t n = 0; n < count; n++) {
        int32_t condition = gAddCondition(stats, texts + n * kGameStringBytes);
        uint32_t key = keys[n];
        if (key != 0xffffffffu) gFixedStringAcquire(key);
        GrowForOne(conditions, gRollConditionsReallocate);
        char* element = static_cast<char*>(conditions.Elements) + conditions.Size * kRollConditionSize;
        conditions.Size++;
        WriteAt(element, kRollConditionTextKey, key);
        WriteAt(element, kRollConditionCondition, condition);
    }

    gRollConditionsSet(static_cast<char*>(object) + kStatsObjectRollConditions, name, &conditions);
    gRollConditionsEnd(&conditions);
    gTextMapEnd(parts);
}

// --- a stats object's attributes, as the code that fills a prototype reads them

constexpr uint32_t kNoFixedString = 0xffffffffu;

// The object's list of modifiers: none for an index below zero, an index
// not below the lists' next handle, or no list there.
void const* ModifierListOf(void const* stats, void const* object)
{
    auto listIndex = ReadAt<int32_t>(object, kStatsObjectModifierList);
    if (listIndex < 0) return nullptr;
    if (ReadAt<int32_t>(stats, kStatsModifierLists + kNamedManagerNextHandle) <= listIndex) return nullptr;
    return ReadAt<void const* const*>(stats, kStatsModifierLists + kNamedManagerValues)[listIndex];
}

// The object's 32-bit value for the attribute, when its list has a modifier
// of that name (the handle by the name, the modifier by the handle).
bool AttributeValue(void const* stats, void const* object, uint32_t const* name, int32_t& value)
{
    void const* list = ModifierListOf(stats, object);
    if (list == nullptr) return false;
    int32_t handle = gModifierHandleByName(list, name);
    if (gModifierByHandle(list, handle) == nullptr) return false;
    value = ReadAt<int32_t const*>(object, kStatsObjectProperties)[handle];
    return true;
}

// A fixed string attribute: the string at the value's position in the
// stats' fixed strings, or the empty fixed string.
uint32_t FixedStringAttribute(void const* stats, void const* object, uint32_t const* name)
{
    int32_t value;
    if (!AttributeValue(stats, object, name, value) || value < 0) return *gEmptyFixedString;
    return ReadAt<uint32_t const*>(stats, kStatsFixedStrings)[value];
}

// A flags attribute: the pointer at the value's position in the stats'
// 64-bit values, which is to the flags; null when there are none.
void const* FlagsAttribute(void const* stats, void const* object, uint32_t const* name)
{
    int32_t value;
    if (!AttributeValue(stats, object, name, value) || value < 0) return nullptr;
    return ReadAt<void const* const*>(stats, kStatsInt64s)[value];
}

// The flags of a context: their low 44 bits.
uint64_t ContextFlags(void const* flags)
{
    if (flags == nullptr) return 0;
    uint64_t bits = ReadAt<uint32_t>(flags, 0) | static_cast<uint64_t>(ReadAt<uint32_t>(flags, 4)) << 32;
    return bits & ((uint64_t{ 1 } << kContextFlagBits) - 1);
}

// A condition attribute: the object's value at the attribute's handle,
// which is the condition's index; -1 without one. Only the handle is looked
// up, and the game takes a handle up to the number of the object's values,
// that number included.
int32_t ConditionAttribute(void const* stats, void const* object, uint32_t const* name)
{
    void const* list = ModifierListOf(stats, object);
    if (list == nullptr) return -1;
    int32_t handle = gModifierHandleByName(list, name);
    if (handle < 0) return -1;
    auto begin = ReadAt<int32_t const*>(object, kStatsObjectProperties);
    auto end = ReadAt<int32_t const*>(object, kStatsObjectProperties + sizeof(void*));
    auto count = static_cast<int32_t>(static_cast<uint64_t>(reinterpret_cast<char const*>(end) - reinterpret_cast<char const*>(begin)) >> 2);
    if (handle <= count) return begin[handle];
    return -1;
}

// The text of a fixed string: none for the null string.
GameStringView TextOfFixedString(uint32_t string)
{
    if (string == kNoFixedString) return { "", 0 };
    return gFixedStringText(string);
}

template <class Function>
Function* SlotOf(void const* object, size_t slot)
{
    return ReadAt<Function*>(ReadAt<void const*>(object, 0), slot);
}

// The functors the object has under `attribute` for the text key "Default"
// become those of `destination`, a functor list of a prototype: it is
// emptied, given a clone of each, and the list's name. False, and the
// destination as it was, when the object has none.
bool CopyDefaultFunctors(void const* object, uint32_t attribute, void* destination)
{
    void const* map = static_cast<char const*>(object) + kStatsObjectFunctors;
    auto buckets = ReadAt<uint32_t>(map, kHashMapBucketCount);
    if (buckets == 0) return false;
    int32_t index = ReadAt<int32_t const*>(map, kHashMapBuckets)[gFixedStringHash(attribute) % buckets];
    while (index >= 0 && ReadAt<uint32_t const*>(map, kHashMapKeys)[index] != attribute) index = ReadAt<int32_t const*>(map, kHashMapNext)[index];
    if (index < 0) return false;

    void const* groups = ReadAt<char const*>(map, kHashMapValues) + static_cast<size_t>(index) * sizeof(GameArray);
    auto begin = ReadAt<char const*>(groups, 0);
    auto count = ReadAt<int32_t>(groups, kArraySize);
    if (begin == nullptr || count <= 0) return false;
    auto defaultKey = ReadAt<uint32_t>(gDefaultTextKeyEntry, kRegistryEntryString);
    void* list = nullptr;
    bool found = false;
    for (int32_t n = 0; n < count && !found; n++) {
        char const* group = begin + static_cast<size_t>(n) * kFunctorGroupSize;
        if (ReadAt<uint32_t>(group, kFunctorGroupTextKey) != defaultKey) continue;
        list = ReadAt<void*>(group, kFunctorGroupList);
        found = true;
    }
    if (list == nullptr) return false;

    SlotOf<adapter::ManagerDestroy>(destination, kNamedManagerDestroySlot)(destination);
    uint64_t functors = SlotOf<adapter::ManagerCount>(list, kNamedManagerCountSlot)(list);
    for (uint64_t n = 0; n < functors; n++) {
        void* functor = SlotOf<adapter::ManagerEntry>(list, kNamedManagerConstEntrySlot)(list, n);
        void* clone = SlotOf<adapter::FunctorClone>(functor, kFunctorCloneSlot)(functor);
        SlotOf<adapter::FunctorListInsert>(destination, kNamedManagerInsertSlot)(destination, clone);
    }
    gFixedStringAssign(static_cast<uint32_t*>(static_cast<void*>(static_cast<char*>(destination) + kFunctorListUniqueName)),
        ReadAt<uint32_t>(list, kFunctorListUniqueName));
    return true;
}

// Every functor of a prototype's list runs in this context.
void SetFunctorContext(void* list, uint64_t context)
{
    uint64_t functors = SlotOf<adapter::ManagerCount>(list, kNamedManagerCountSlot)(list);
    for (uint64_t n = 0; n < functors; n++) {
        void* functor = SlotOf<adapter::ManagerEntry>(list, kNamedManagerEntrySlot)(list, n);
        WriteAt(functor, kFunctorPropertyContext, context);
    }
}

void AssignFixedString(void* object, size_t offset, uint32_t string)
{
    gFixedStringAssign(static_cast<uint32_t*>(static_cast<void*>(static_cast<char*>(object) + offset)), string);
}

// An ls::Function as the game's code lays one out (Layout/GameMembers.h).
struct GameFunction
{
    void* Methods;
    void* Call;
    void* Copy;
    void* MoveDestroy;
    void* Captures[4];
};
static_assert(offsetof(GameFunction, Methods) == kFunctionMethods && offsetof(GameFunction, Call) == kFunctionCall
    && offsetof(GameFunction, Copy) == kFunctionCopy && offsetof(GameFunction, MoveDestroy) == kFunctionMoveDestroy
    && offsetof(GameFunction, Captures) == kFunctionCaptures && sizeof(GameFunction) == kFunctionSize);

}

namespace adapter {

// eoc::ParseStaticBoosts(text, boosts, filter) is, in each of its four
// callers, this (eoc::StatusPrototypeManager::Init is the one it was read
// from):
//
//     an ls::Function is built on the stack whose methods are the three
//     functions of the lambda `$_1` of eoc::ParseStaticBoosts and whose
//     captures are the address of `filter` and the address of `boosts`;
//     ParseBoosts(text.Text, text.Length, &function) is called;
//     the function is ended: when its methods pointer is not null, its
//     MoveDestroy is called with the methods, the methods and no destination.
//
// The lambda's Call (the game calls it once for each boost in the text) asks
// the filter, when the filter holds something, whether it takes the boost's
// name; makes the boost's prototype (eoc::BoostPrototypeManager::
// CreateBoostPrototype); and appends its GUID, when there is one, to the
// array. A filter whose first word is null is not called.
//
// The callers empty the array first and do not call for a null fixed string;
// Norbyte's caller does both itself (StatusPrototypeManager::SyncStat).
bool ParseStaticBoosts(GameStringView& text, void* boosts, void* filter)
{
    // No filter is a filter that holds nothing.
    GameFunction none{};
    GameFunction function{};
    function.Methods = &function.Call;
    function.Call = reinterpret_cast<void*>(gBoostCall);
    function.Copy = reinterpret_cast<void*>(gBoostCopy);
    function.MoveDestroy = reinterpret_cast<void*>(gBoostMoveDestroy);
    function.Captures[0] = filter ? filter : &none;
    function.Captures[1] = boosts;

    bool parsed = gParseBoosts(text.Text, text.Length, &function);

    if (function.Methods) {
        BoostMoveDestroy* end;
        memcpy(&end, static_cast<char*>(function.Methods) + (kFunctionMoveDestroy - kFunctionCall), sizeof(end));
        end(function.Methods, function.Methods, nullptr);
    }
    return parsed;
}

// stats::Object::SetPropertyString(name, value) is, in RPGStats::ParseDataBuffer
// (the one place a `data` line of a stats file is applied, after its tests
// for the keys ExtraProperties and ComboCategory, which Norbyte's Windows
// mapping shows to be the caller's), this:
//
//     the object's list of modifiers is taken from the stats by the object's
//     index of it; the attribute's handle in the list by its name
//     (GetHandleByName, -1 when the list has none); the modifier by the
//     handle (GetByHandle); the modifier's list of values from the stats by
//     the modifier's index of it. Anything missing on the way: nothing is set.
//
//     By the name of the list of values:
//       StatsFunctors                  SetStatsFunctors above
//       Conditions, TargetConditions,  the text becomes a condition of the stats
//       UseConditions                  (RPGStats::AddCondition), whose index is the value
//       RollConditions                 SetRollConditions above
//       AIFlags                        the object's AI flags become the fixed string of the text
//       ConstantInt                    the value is CRPGStats_Modifier::GetPosition of the text
//       Requirements                   ParseRequirements fills the object's requirements
//       any other                      the value is GetPosition of the text when that is not -1;
//                                      0 when it is -1 and the text is empty; else it is left
//
// "The value" is the object's 32-bit entry for the attribute, at the
// attribute's handle. GetPosition, by the type of the list, parses a number,
// looks an enumeration label up, or finds or adds the text in the stats'
// pool for the type, and answers the position.
void StatsObjectSetPropertyString(void* object, uint32_t const* name, char const* value)
{
    void* stats = *gStats;
    // The game's code has an object, a name and a text at this point, and stats that are loaded.
    if (stats == nullptr || object == nullptr || name == nullptr || value == nullptr) return;
    auto listIndex = ReadAt<int32_t>(object, kStatsObjectModifierList);
    if (listIndex < 0) return;
    if (ReadAt<int32_t>(stats, kStatsModifierLists + kNamedManagerNextHandle) <= listIndex) return;
    void const* list = ReadAt<void const* const*>(stats, kStatsModifierLists + kNamedManagerValues)[listIndex];
    if (list == nullptr) return;
    int32_t attribute = gModifierHandleByName(list, name);
    if (attribute == -1) return;
    void const* modifier = gModifierByHandle(list, attribute);
    if (modifier == nullptr) return;
    auto valueListIndex = ReadAt<int32_t>(modifier, kModifierValueList);
    if (valueListIndex < 0) return;
    if (ReadAt<int32_t>(stats, kStatsValueLists + kNamedManagerNextHandle) <= valueListIndex) return;
    void const* valueList = ReadAt<void const* const*>(stats, kStatsValueLists + kNamedManagerValues)[valueListIndex];
    if (valueList == nullptr) return;
    auto type = ReadAt<uint32_t>(valueList, kValueListName);

    auto setValue = [&](int32_t position) {
        ReadAt<int32_t*>(object, kStatsObjectProperties)[attribute] = position;
    };
    auto setCondition = [&]() {
        GameString text;
        gStringFromText(&text, value);
        setValue(gAddCondition(stats, &text));
        gStringEnd(&text);
    };

    if (type == *gStatsFunctorsName) {
        SetStatsFunctors(stats, object, name, value);
    } else if (type == *gConditionsName || type == *gTargetConditionsName) {
        setCondition();
    } else if (type == *gRollConditionsName) {
        SetRollConditions(stats, object, name, value);
    } else if (type == *gUseConditionsName) {
        setCondition();
    } else if (type == *gAIFlagsName) {
        uint32_t flags;
        gFixedStringCreate(&flags, value, -1);
        gFixedStringMoveAssign(static_cast<uint32_t*>(static_cast<void*>(static_cast<char*>(object) + kStatsObjectAIFlags)), &flags);
        gFixedStringEnd(&flags);
    } else if (type == *gConstantIntName) {
        setValue(gModifierGetPosition(valueListIndex, value));
    } else if (type == *gRequirementsName) {
        GameString text;
        gStringFromText(&text, value);
        gParseRequirements(&text, static_cast<char*>(object) + kStatsObjectRequirements);
        gStringEnd(&text);
    } else {
        int32_t position = gModifierGetPosition(valueListIndex, value);
        if (position != -1) {
            setValue(position);
        } else if (value[0] == 0) {
            setValue(0);
        }
    }
}

// eoc::PassivePrototype::Init(object) is, in the job that loads the stats
// (the function object of ecl::ModuleLoadSystem::DoLoadStats), between that
// job finding or making the passive's node and going on to the next object,
// this, in this order:
//
//     Clean()
//     Name                        the object's name
//     Description                 eoc::StatsDescription::SetFromStatsObject
//     ToggleOnEffect, ToggleOffEffect     the fixed string attributes of those names
//     Properties                  the bits 0 to 17 of the flags attribute Properties are added
//     EnabledContext, StatsFunctorContext     the contexts of those flags attributes
//     EnabledConditions, ConditionsIndex, BoostConditionsIndex
//                                 the condition attributes EnabledConditions, Conditions, BoostConditions
//     StatsFunctors               the object's functors StatsFunctors for the text key Default, when it has them;
//                                 every functor of the list then runs in StatsFunctorContext
//     ToggleOnFunctors, ToggleOffFunctors     the object's functors of those names, when it has them
//     BoostContext                the context of that flags attribute
//     Boosts                      emptied, then eoc::ParseStaticBoosts of the fixed string attribute Boosts
//     ToggleOffContext            the context of that flags attribute
//     ToggleGroup                 the fixed string attribute
//     PriorityOrder               the value of the attribute, 0 when it has none or one below 0
//     TooltipConditionalDamage    the fixed string attribute
//
// An attribute the object's list of modifiers does not have gives the empty
// fixed string, no flags, no condition (-1).
void PassivePrototypeInit(void* prototype, void* object)
{
    void* stats = *gStats;
    if (stats == nullptr || prototype == nullptr || object == nullptr) return;
    auto fixedString = [&](uint32_t const* name) { return FixedStringAttribute(stats, object, name); };
    auto context = [&](uint32_t const* name) { return ContextFlags(FlagsAttribute(stats, object, name)); };
    auto condition = [&](uint32_t const* name) { return ConditionAttribute(stats, object, name); };
    auto at = [&](size_t offset) { return static_cast<char*>(prototype) + offset; };

    gPassiveClean(prototype);
    AssignFixedString(prototype, kPassiveName, ReadAt<uint32_t>(object, kStatsObjectName));
    gDescriptionFromObject(at(kPassiveDescription), object);
    AssignFixedString(prototype, kPassiveToggleOnEffect, fixedString(*gToggleOnEffectName));
    AssignFixedString(prototype, kPassiveToggleOffEffect, fixedString(*gToggleOffEffectName));
    if (void const* properties = FlagsAttribute(stats, object, gPropertiesName)) {
        WriteAt(prototype, kPassiveProperties, ReadAt<uint32_t>(prototype, kPassiveProperties) | (ReadAt<uint32_t>(properties, 0) & kPassivePropertyBits));
    }
    WriteAt(prototype, kPassiveEnabledContext, context(*gEnabledContextName));
    uint64_t functorContext = context(*gStatsFunctorContextName);
    WriteAt(prototype, kPassiveStatsFunctorContext, functorContext);
    WriteAt(prototype, kPassiveEnabledConditions, condition(*gEnabledConditionsName));
    WriteAt(prototype, kPassiveConditions, condition(gConditionsName));
    WriteAt(prototype, kPassiveBoostConditions, condition(gBoostConditionsName));

    CopyDefaultFunctors(object, *gStatsFunctorsName, at(kPassiveStatsFunctors));
    SetFunctorContext(at(kPassiveStatsFunctors), functorContext);
    CopyDefaultFunctors(object, **gToggleOnFunctorsName, at(kPassiveToggleOnFunctors));
    CopyDefaultFunctors(object, **gToggleOffFunctorsName, at(kPassiveToggleOffFunctors));

    WriteAt(prototype, kPassiveBoostContext, context(gBoostContextName));
    GameStringView boosts = TextOfFixedString(fixedString(gBoostsName));
    WriteAt<uint32_t>(prototype, kPassiveBoosts + kArraySize, 0);
    adapter::ParseStaticBoosts(boosts, at(kPassiveBoosts), nullptr);
    WriteAt(prototype, kPassiveToggleOffContext, context(*gToggleOffContextName));
    AssignFixedString(prototype, kPassiveToggleGroup, fixedString(*gToggleGroupName));
    int32_t priority;
    if (!AttributeValue(stats, object, *gPriorityOrderName, priority) || priority < 0) priority = 0;
    WriteAt(prototype, kPassivePriorityOrder, priority);
    AssignFixedString(prototype, kPassiveTooltipConditionalDamage, fixedString(*gTooltipConditionalDamageName));
}

// eoc::InterruptPrototype::Init(object) is, in the same job, after the job
// has made the interrupt's place in the manager's map, this, in this order:
//
//     Name                        the object's name
//     Description                 eoc::StatsDescription::SetFromStatsObject
//     InterruptContext            the bits 0 to 7 of that flags attribute
//     InterruptContextScope       the bits 0 to 2 of that flags attribute
//     InterruptFlags              the bit 0 of that flags attribute
//     Conditions                  the condition attribute Conditions
//     Container                   the fixed string attribute
//     RollCondition               the condition attribute Roll
//     PropertiesFunctors, SuccessFunctors, FailureFunctors
//                                 the object's functors Properties, Success, Failure for the text key Default,
//                                 each when the object has them; every functor of such a list then runs in
//                                 the context 0x4000000000
//     Costs                       eoc::action_resource::ParseActionResourceCostWithError of the fixed string attribute Cost
//     Stack                       the fixed string attribute
//     InterruptDefaultValue       the bits 0 and 1 of that flags attribute
//     EnableCondition             the condition attribute
//     EnableContext               of the context of that flags attribute, the bits 0x8, 0x400, 0x80000 and 0x800000000
//
// The job makes a prototype only for an interrupt the manager does not have
// yet, so nothing is cleaned first: a functor list the object has no
// functors for stays as it is.
void InterruptPrototypeInit(void* prototype, void* object)
{
    void* stats = *gStats;
    if (stats == nullptr || prototype == nullptr || object == nullptr) return;
    auto fixedString = [&](uint32_t const* name) { return FixedStringAttribute(stats, object, name); };
    auto bits = [&](uint32_t const* name, uint32_t taken) -> uint32_t {
        void const* flags = FlagsAttribute(stats, object, name);
        return flags ? ReadAt<uint32_t>(flags, 0) & taken : 0;
    };
    auto condition = [&](uint32_t const* name) { return ConditionAttribute(stats, object, name); };
    auto at = [&](size_t offset) { return static_cast<char*>(prototype) + offset; };

    AssignFixedString(prototype, kInterruptName, ReadAt<uint32_t>(object, kStatsObjectName));
    gDescriptionFromObject(at(kInterruptDescription), object);
    WriteAt(prototype, kInterruptContext, static_cast<uint16_t>(bits(*gInterruptContextName, kInterruptContextBits)));
    WriteAt(prototype, kInterruptContextScope, static_cast<uint8_t>(bits(*gInterruptContextScopeName, kInterruptContextScopeBits)));
    WriteAt(prototype, kInterruptFlags, static_cast<uint8_t>(bits(*gInterruptFlagsName, kInterruptFlagBits)));
    WriteAt(prototype, kInterruptConditions, condition(*gInterruptConditionsName));
    AssignFixedString(prototype, kInterruptContainer, fixedString(*gContainerName));
    WriteAt(prototype, kInterruptRoll, condition(*gRollName));

    struct { uint32_t** Name; size_t List; } const lists[] = {
        { gInterruptPropertiesName, kInterruptPropertiesFunctors },
        { gSuccessName, kInterruptSuccessFunctors },
        { gFailureName, kInterruptFailureFunctors },
    };
    for (auto const& list : lists) {
        if (CopyDefaultFunctors(object, **list.Name, at(list.List))) SetFunctorContext(at(list.List), kInterruptFunctorContext);
    }

    GameStringView cost = TextOfFixedString(fixedString(*gCostName));
    gParseResourceCosts(&cost, at(kInterruptCosts));
    AssignFixedString(prototype, kInterruptStack, fixedString(*gStackName));
    WriteAt(prototype, kInterruptDefaultValue, static_cast<uint8_t>(bits(*gInterruptDefaultValueName, kInterruptDefaultValueBits)));
    WriteAt(prototype, kInterruptEnableCondition, condition(*gEnableConditionName));
    WriteAt(prototype, kInterruptEnableContext,
        ContextFlags(FlagsAttribute(stats, object, *gEnableContextName)) & kInterruptEnableContextBits);
}

}

namespace {

template <class Function>
constexpr void** TargetOf(Function** target)
{
    return reinterpret_cast<void**>(target);
}

#define BG3MSE_NEED(entry, target) \
    AdapterNeed{ entry, kPrototypeOf<std::remove_pointer_t<decltype(target)>>, TargetOf(&target) }

const AdapterNeed kParseStaticBoostsNeeds[] = {
    BG3MSE_NEED("(anonymous namespace)::ParseBoosts", gParseBoosts),
    BG3MSE_NEED("eoc::ParseStaticBoosts: the boost function's Call", gBoostCall),
    BG3MSE_NEED("eoc::ParseStaticBoosts: the boost function's Copy", gBoostCopy),
    BG3MSE_NEED("eoc::ParseStaticBoosts: the boost function's MoveDestroy", gBoostMoveDestroy),
};

template <class T>
constexpr void** AddressOf(T** target)
{
    return reinterpret_cast<void**>(target);
}

// A variable, an object or a vtable of the game: no prototype.
#define BG3MSE_NEED_ADDRESS(entry, target) AdapterNeed{ entry, Prototype{}, AddressOf(&target) }

const AdapterNeed kSetPropertyStringNeeds[] = {
    BG3MSE_NEED_ADDRESS("gRPGStats", gStats),
    BG3MSE_NEED_ADDRESS("eoc::strDefault", gDefaultTextKey),
    BG3MSE_NEED_ADDRESS("EoCFS::strStatsFunctors", gStatsFunctorsName),
    BG3MSE_NEED_ADDRESS("EoCFS::strConditions", gConditionsName),
    BG3MSE_NEED_ADDRESS("EoCFS::strTargetConditions", gTargetConditionsName),
    BG3MSE_NEED_ADDRESS("EoCFS::strUseConditions", gUseConditionsName),
    BG3MSE_NEED_ADDRESS("EoCFS::strRollConditions", gRollConditionsName),
    BG3MSE_NEED_ADDRESS("EoCFS::strAIFlags", gAIFlagsName),
    BG3MSE_NEED_ADDRESS("EoCFS::strConstantInt", gConstantIntName),
    BG3MSE_NEED_ADDRESS("EoCFS::strRequirements", gRequirementsName),
    BG3MSE_NEED("CNamedElementManager<CRPGStats_Modifier>::GetHandleByName", gModifierHandleByName),
    BG3MSE_NEED("CNamedElementManager<CRPGStats_Modifier>::GetByHandle", gModifierByHandle),
    BG3MSE_NEED_ADDRESS("CRPGStats_Modifier_List", gModifierListVtable),
    BG3MSE_NEED("CRPGStats_Modifier::GetPosition", gModifierGetPosition),
    BG3MSE_NEED("RPGStats::AddCondition", gAddCondition),
    BG3MSE_NEED("ParseRequirements", gParseRequirements),
    BG3MSE_NEED("ls::STDString::STDString(char const*)", gStringFromText),
    BG3MSE_NEED("ls::STDString::STDString(ls::_StringView<char> const&)", gStringFromView),
    BG3MSE_NEED("ls::STDString::TrimWhiteSpaceLeftAndRight", gStringTrim),
    BG3MSE_NEED("ls::STDString::~STDString", gStringEnd),
    BG3MSE_NEED("eoc::TextKeyParsingHelpers::SplitStringPerTextKeyWithMerge", gSplitPerTextKeyWithMerge),
    BG3MSE_NEED("eoc::TextKeyParsingHelpers::SplitStringPerTextKeyNoMerge", gSplitPerTextKeyNoMerge),
    BG3MSE_NEED("ls::HashMap<ls::FixedString, ls::STDString>::~HashMap", gTextMapEnd),
    BG3MSE_NEED("ls::Split<2>", gSplitInTwo),
    BG3MSE_NEED("eoc::StatsFunctorListManager::CreateStatsFunctorList", gCreateFunctorList),
    BG3MSE_NEED("eoc::StatsFunctorBase::ParseStatsFunctor", gParseFunctor),
    BG3MSE_NEED_ADDRESS("eoc::StatsFunctorList", gFunctorListVtable),
    BG3MSE_NEED("ls__FixedString__IncRef", gFixedStringAcquire),
    BG3MSE_NEED("ls::FixedString::Create", gFixedStringCreate),
    BG3MSE_NEED("ls::FixedString::operator=(ls::FixedString&&)", gFixedStringMoveAssign),
    BG3MSE_NEED("ls::FixedString::~FixedString", gFixedStringEnd),
    BG3MSE_NEED("ls::DynamicArray<TextKeyStatsFunctorList>::Reallocate", gFunctorGroupsReallocate),
    BG3MSE_NEED("ls::DynamicArray<TextKeyStatsFunctorList>::~DynamicArray", gFunctorGroupsEnd),
    BG3MSE_NEED("ls::HashMap<ls::FixedString, ls::DynamicArray<TextKeyStatsFunctorList>>::SetUniversal", gFunctorGroupsSet),
    BG3MSE_NEED("ls::DynamicArray<TextKeyRollCondition>::Reallocate", gRollConditionsReallocate),
    BG3MSE_NEED("ls::DynamicArray<TextKeyRollCondition>::~DynamicArray", gRollConditionsEnd),
    BG3MSE_NEED("ls::HashMap<ls::FixedString, ls::DynamicArray<TextKeyRollCondition>>::SetUniversal", gRollConditionsSet),
};

// What both prototype initialisers use.
#define BG3MSE_PROTOTYPE_NEEDS \
    BG3MSE_NEED_ADDRESS("gRPGStats", gStats), \
    BG3MSE_NEED("CNamedElementManager<CRPGStats_Modifier>::GetHandleByName", gModifierHandleByName), \
    BG3MSE_NEED("CNamedElementManager<CRPGStats_Modifier>::GetByHandle", gModifierByHandle), \
    BG3MSE_NEED_ADDRESS("CRPGStats_Modifier_List", gModifierListVtable), \
    BG3MSE_NEED_ADDRESS("eoc::StatsFunctorList", gFunctorListVtable), \
    BG3MSE_NEED_ADDRESS("ls::FixedString::Empty", gEmptyFixedString), \
    BG3MSE_NEED_ADDRESS("ls::StaticFixedStringRegistry: the entry for Default", gDefaultTextKeyEntry), \
    BG3MSE_NEED("ls__FixedString__GetString", gFixedStringText), \
    BG3MSE_NEED("eoc::StatsDescription::SetFromStatsObject", gDescriptionFromObject), \
    BG3MSE_NEED("ls::FixedString::operator=(ls::FixedString const&)", gFixedStringAssign), \
    BG3MSE_NEED("ls::FixedString::GetHash", gFixedStringHash)

const AdapterNeed kPassiveInitNeeds[] = {
    BG3MSE_PROTOTYPE_NEEDS,
    BG3MSE_NEED("eoc::PassivePrototype::Clean", gPassiveClean),
    BG3MSE_NEED("(anonymous namespace)::ParseBoosts", gParseBoosts),
    BG3MSE_NEED("eoc::ParseStaticBoosts: the boost function's Call", gBoostCall),
    BG3MSE_NEED("eoc::ParseStaticBoosts: the boost function's Copy", gBoostCopy),
    BG3MSE_NEED("eoc::ParseStaticBoosts: the boost function's MoveDestroy", gBoostMoveDestroy),
    BG3MSE_NEED_ADDRESS("(anonymous namespace)::strToggleOnEffect", gToggleOnEffectName),
    BG3MSE_NEED_ADDRESS("(anonymous namespace)::strToggleOffEffect", gToggleOffEffectName),
    BG3MSE_NEED_ADDRESS("EoCFS::strProperties", gPropertiesName),
    BG3MSE_NEED_ADDRESS("(anonymous namespace)::strEnabledContext", gEnabledContextName),
    BG3MSE_NEED_ADDRESS("eoc::strStatsFunctorContext", gStatsFunctorContextName),
    BG3MSE_NEED_ADDRESS("(anonymous namespace)::strEnabledConditions", gEnabledConditionsName),
    BG3MSE_NEED_ADDRESS("EoCFS::strConditions", gConditionsName),
    BG3MSE_NEED_ADDRESS("EoCFS::strBoostConditions", gBoostConditionsName),
    BG3MSE_NEED_ADDRESS("EoCFS::strStatsFunctors", gStatsFunctorsName),
    BG3MSE_NEED_ADDRESS("(anonymous namespace)::strToggleOnFunctors", gToggleOnFunctorsName),
    BG3MSE_NEED_ADDRESS("(anonymous namespace)::strToggleOffFunctors", gToggleOffFunctorsName),
    BG3MSE_NEED_ADDRESS("EoCFS::strBoostContext", gBoostContextName),
    BG3MSE_NEED_ADDRESS("EoCFS::strBoosts", gBoostsName),
    BG3MSE_NEED_ADDRESS("(anonymous namespace)::strToggleOffContext", gToggleOffContextName),
    BG3MSE_NEED_ADDRESS("(anonymous namespace)::strToggleGroup", gToggleGroupName),
    BG3MSE_NEED_ADDRESS("(anonymous namespace)::strPriorityOrder", gPriorityOrderName),
    BG3MSE_NEED_ADDRESS("(anonymous namespace)::strTooltipConditionalDamage", gTooltipConditionalDamageName),
};

const AdapterNeed kInterruptInitNeeds[] = {
    BG3MSE_PROTOTYPE_NEEDS,
    BG3MSE_NEED("eoc::action_resource::ParseActionResourceCostWithError", gParseResourceCosts),
    BG3MSE_NEED_ADDRESS("(anonymous namespace)::strInterruptContext", gInterruptContextName),
    BG3MSE_NEED_ADDRESS("(anonymous namespace)::strInterruptContextScope", gInterruptContextScopeName),
    BG3MSE_NEED_ADDRESS("(anonymous namespace)::strInterruptFlags", gInterruptFlagsName),
    BG3MSE_NEED_ADDRESS("(anonymous namespace)::strConditions of the interrupts", gInterruptConditionsName),
    BG3MSE_NEED_ADDRESS("(anonymous namespace)::strContainer", gContainerName),
    BG3MSE_NEED_ADDRESS("(anonymous namespace)::strRoll", gRollName),
    BG3MSE_NEED_ADDRESS("(anonymous namespace)::strProperties", gInterruptPropertiesName),
    BG3MSE_NEED_ADDRESS("(anonymous namespace)::strSuccess", gSuccessName),
    BG3MSE_NEED_ADDRESS("(anonymous namespace)::strFailure", gFailureName),
    BG3MSE_NEED_ADDRESS("(anonymous namespace)::strCost", gCostName),
    BG3MSE_NEED_ADDRESS("(anonymous namespace)::strStack", gStackName),
    BG3MSE_NEED_ADDRESS("(anonymous namespace)::strInterruptDefaultValue", gInterruptDefaultValueName),
    BG3MSE_NEED_ADDRESS("(anonymous namespace)::strEnableCondition", gEnableConditionName),
    BG3MSE_NEED_ADDRESS("(anonymous namespace)::strEnableContext", gEnableContextName),
};

#define BG3MSE_STATS_FUNCTION(member, function, needs) \
    CallAdapter{ member, kPrototypeOf<decltype(adapter::function)>, Prototype{}, reinterpret_cast<void*>(&adapter::function), nullptr, \
        nullptr, nullptr, 0, needs }

const CallAdapter kAdapters[] = {
    BG3MSE_STATS_FUNCTION("eoc__ParseStaticBoosts", ParseStaticBoosts, kParseStaticBoostsNeeds),
    BG3MSE_STATS_FUNCTION("stats__Object__SetPropertyString", StatsObjectSetPropertyString, kSetPropertyStringNeeds),
    BG3MSE_STATS_FUNCTION("eoc__PassivePrototype__Init", PassivePrototypeInit, kPassiveInitNeeds),
    BG3MSE_STATS_FUNCTION("eoc__InterruptPrototype__Init", InterruptPrototypeInit, kInterruptInitNeeds),
};

}

std::span<CallAdapter const> StatsFunctionAdapters()
{
    return kAdapters;
}

}
