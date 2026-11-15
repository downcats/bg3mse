#include <Binding/TypeIndexBinding.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace bg3se::mac::binding {

using engine::RegisteredTypeName;

namespace {

constexpr TypeContextRow kContexts[] = {
    { TypeContext::Component, "TypeIdContext::Component", "ecs::ComponentTypeIdContext" },
    { TypeContext::OneFrameComponent, "TypeIdContext::OneFrameComponent", "ecs::OneFrameComponentTypeIdContext" },
    { TypeContext::System, "TypeIdContext::System", "ecs::EntityWorld::SystemsContext" },
    { TypeContext::Replication, "TypeIdContext::Replication", "ecs::sync::ReplicatedTypeContext" },
    { TypeContext::ImmutableData, "TypeIdContext::ImmutableData", "ls::ImmutableDataHeadmaster" },
};
static_assert(std::size(kContexts) == (size_t)TypeContext::Count);

char const* Describe(BoundAs as)
{
    switch (as) {
    case BoundAs::Component: return "component";
    case BoundAs::System: return "system";
    case BoundAs::StaticData: return "static data";
    default: return "?";
    }
}

char const* Describe(TypeContext context)
{
    switch (context) {
    case TypeContext::Component: return "components";
    case TypeContext::OneFrameComponent: return "one-frame components";
    case TypeContext::System: return "systems";
    case TypeContext::Replication: return "replicated types";
    case TypeContext::ImmutableData: return "static data";
    default: return "?";
    }
}

// The variables of one context under one registered name.
struct Named
{
    std::vector<engine::TypeIndex const*> Variables;    // distinct addresses
    std::set<std::string_view> Spellings;               // the bound names that reach it
    bool LeftOut{ false };
};

using ByName = std::map<std::string, Named, std::less<>>;

}

std::span<TypeContextRow const> TypeContextRows()
{
    return kContexts;
}

bool TypeIndexPlan::AllMapped() const
{
    for (auto const& tally : Types) {
        if (tally.Unmapped || tally.Ambiguous) return false;
    }
    for (auto const& context : Contexts) {
        if (!context.StateVA) return false;
    }
    return true;
}

TypeIndexPlan PlanTypeIndices(engine::Resolution const& resolution, std::span<BoundType const> bound, std::span<WithheldType const> withheld)
{
    TypeIndexPlan plan;
    ByName names[(size_t)TypeContext::Count];

    for (auto const& row : kContexts) {
        auto& context = plan.Contexts[(size_t)row.Is];
        context.Row = &row;
        auto entry = resolution.Find(row.Entry);
        if (!entry) {
            context.Refused = "the engine table has no such entry";
        } else if (!entry->Usable() || !entry->VA) {
            context.Refused = entry->Reason.empty() ? "the entry did not resolve" : entry->Reason;
        } else {
            context.StateVA = entry->VA;
            for (auto const& index : resolution.Types.All()) {
                if (index.Context != entry->Of->Context) continue;
                context.Variables++;
                auto& named = names[(size_t)row.Is][RegisteredTypeName(index.Type)];
                bool known = std::any_of(named.Variables.begin(), named.Variables.end(),
                    [&](engine::TypeIndex const* other) { return other->VA == index.VA; });
                if (!known) named.Variables.push_back(&index);
            }
        }
        if (!context.Refused.empty()) {
            plan.Report.push_back(std::string("the context of the ") + Describe(row.Is) + " (" + row.Entry + ") is refused: "
                + context.Refused + "; none of its types is known");
        }
    }

    auto contextOf = [&](TypeContext is) -> TypeIndexPlan::Context const& { return plan.Contexts[(size_t)is]; };
    auto find = [&](TypeContext is, std::string const& registered) -> Named* {
        auto it = names[(size_t)is].find(registered);
        return it == names[(size_t)is].end() ? nullptr : &it->second;
    };

    // Each bound name to the variables it reaches. A name is mapped only when
    // it reaches one variable and no other spelling of the definitions
    // reaches the same one, which is known once all of them are placed.
    struct Placed
    {
        BoundType const* Type;
        Named* At;
        TypeContext In;
    };
    std::vector<Placed> placed;
    auto say = [&](BoundType const& type, char const* what, std::string const& why) {
        plan.Report.push_back(std::string(Describe(type.As)) + " " + type.EngineClass + ": " + what + ": " + why);
    };
    for (auto const& type : bound) {
        auto& tally = plan.Types[(size_t)type.As];
        tally.Bound++;
        std::string registered = RegisteredTypeName(type.EngineClass);

        // The contexts in which his code would find the name.
        TypeContext in[2];
        size_t count = 0;
        switch (type.As) {
        case BoundAs::Component:
            in[count++] = TypeContext::Component;
            in[count++] = TypeContext::OneFrameComponent;
            break;
        case BoundAs::System: in[count++] = TypeContext::System; break;
        case BoundAs::StaticData: in[count++] = TypeContext::ImmutableData; break;
        default: break;
        }

        char const* refused = nullptr;
        size_t variables = 0;
        Named* found = nullptr;
        TypeContext foundIn{};
        for (size_t n = 0; n < count; n++) {
            if (!contextOf(in[n]).StateVA) {
                refused = contextOf(in[n]).Row->Entry;
                continue;
            }
            if (auto named = find(in[n], registered)) {
                variables += named->Variables.size();
                found = named;
                foundIn = in[n];
            }
        }

        auto held = std::find_if(withheld.begin(), withheld.end(),
            [&](WithheldType const& w) { return std::string_view(w.EngineClass) == type.EngineClass; });
        if (held != withheld.end()) {
            // Not in the maps under any name: his lookup must not find it.
            tally.Unmapped++;
            say(type, "unmapped", held->Why);
            for (size_t n = 0; n < count; n++) {
                if (auto named = contextOf(in[n]).StateVA ? find(in[n], registered) : nullptr) named->LeftOut = true;
            }
        } else if (variables > 1) {
            tally.Ambiguous++;
            say(type, "ambiguous", "the game has " + std::to_string(variables) + " index variables of that name");
            for (size_t n = 0; n < count; n++) {
                if (auto named = find(in[n], registered)) named->LeftOut = true;
            }
        } else if (variables == 0 || refused) {
            // With one of its contexts refused the name could be in that one
            // as well: nothing is known about it.
            tally.Unmapped++;
            say(type, "unmapped", refused ? std::string("the context ") + refused + " is refused"
                : "the game has no index variable whose type registers as " + registered);
            if (found) found->LeftOut = true;
        } else {
            found->Spellings.insert(type.EngineClass);
            placed.push_back({ &type, found, foundIn });
        }
    }

    for (auto const& place : placed) {
        auto const& type = *place.Type;
        auto& tally = plan.Types[(size_t)type.As];
        if (place.At->Spellings.size() > 1) {
            // His lookups could find only one of the spellings: none is taken.
            tally.Ambiguous++;
            place.At->LeftOut = true;
            say(type, "ambiguous", std::to_string(place.At->Spellings.size()) + " names of the definitions register as "
                + RegisteredTypeName(type.EngineClass));
            continue;
        }
        tally.Mapped++;
        if (place.At->Variables[0]->Type != type.EngineClass) plan.ByRule++;
        if (type.As != BoundAs::Component) continue;
        bool oneFrame = place.In == TypeContext::OneFrameComponent;
        if (oneFrame != type.OneFrame) {
            say(type, "mapped", std::string("the game registers it as ") + (oneFrame ? "a one-frame component" : "a component that persists")
                + ", the declaration says the other");
        }
        if (!contextOf(TypeContext::Replication).StateVA) continue;
        if (auto replicated = find(TypeContext::Replication, RegisteredTypeName(type.EngineClass))) {
            if (replicated->Variables.size() == 1) {
                replicated->Spellings.insert(type.EngineClass);
                plan.Replicated++;
            } else {
                replicated->LeftOut = true;
                say(type, "mapped", "its replication index is ambiguous (" + std::to_string(replicated->Variables.size())
                    + " variables) and is left out");
            }
        }
    }

    for (size_t c = 0; c < (size_t)TypeContext::Count; c++) {
        if (!plan.Contexts[c].StateVA) continue;
        for (auto const& [registered, named] : names[c]) {
            if (named.LeftOut) continue;
            if (named.Variables.size() != 1) {
                plan.Report.push_back(std::string(Describe((TypeContext)c)) + ": " + registered + " is left out: the game has "
                    + std::to_string(named.Variables.size()) + " index variables of that name");
                continue;
            }
            TypeVariable variable;
            variable.Context = (TypeContext)c;
            variable.VA = named.Variables[0]->VA;
            variable.Bound = named.Spellings.size() == 1;
            variable.Name = variable.Bound ? std::string(*named.Spellings.begin()) : registered;
            variable.Registered = registered;
            plan.Variables.push_back(std::move(variable));
        }
    }
    return plan;
}

TypeIndexFill FillTypeIndices(TypeIndexPlan const& plan, std::function<void*(uint64_t)> const& at, TypeIndexSink& sink)
{
    constexpr size_t kLinesForOneContext = 8;
    TypeIndexFill fill;
    int32_t* states[(size_t)TypeContext::Count]{};
    engine::GameTypeNames const* numbered[(size_t)TypeContext::Count]{};
    size_t refusedIn[(size_t)TypeContext::Count]{};

    for (auto const& context : plan.Contexts) {
        if (!context.StateVA) continue;
        auto c = (size_t)context.Row->Is;
        auto state = static_cast<engine::GameTypeContextState*>(at(context.StateVA));
        if (!state) {
            fill.Report.push_back(std::string("the state of the context of the ") + Describe(context.Row->Is) + " is not in this process");
            continue;
        }
        if (!state->Numbered || !state->Numbered->Names) {
            fill.Report.push_back(std::string("the game has not numbered the ") + Describe(context.Row->Is)
                + " yet (CoreLibSDM::Init does): none of them is known");
            continue;
        }
        states[c] = reinterpret_cast<int32_t*>(state);
        numbered[c] = state->Numbered;
        sink.Context(states[c], context.Row->NorbyteName);
        fill.Contexts++;
    }

    for (auto const& variable : plan.Variables) {
        auto c = (size_t)variable.Context;
        if (!numbered[c]) continue;
        auto refuse = [&](std::string const& why) {
            fill.Refused++;
            if (refusedIn[c]++ < kLinesForOneContext) {
                fill.Report.push_back(std::string(Describe(variable.Context)) + ": " + variable.Name + " is left unknown: " + why);
            }
        };
        auto value = static_cast<int32_t*>(at(variable.VA));
        if (!value) {
            refuse("its index variable is not in this process");
            continue;
        }
        int32_t index = *value;
        if (index < 0 || (uint32_t)index >= numbered[c]->Count) {
            char text[96];
            snprintf(text, sizeof(text), "its variable holds %#x, and the game numbered %u types", (unsigned)index, numbered[c]->Count);
            refuse(text);
            continue;
        }
        auto const& name = numbered[c]->Names[index];
        if (!name.Text || name.Length < 0 || (size_t)name.Length != variable.Registered.size()
            || memcmp(name.Text, variable.Registered.data(), variable.Registered.size()) != 0) {
            std::string theirs = name.Text && name.Length > 0 ? std::string(name.Text, (size_t)std::min(name.Length, 200)) : std::string();
            refuse("its variable holds " + std::to_string(index) + ", which the game gave to " + theirs);
            continue;
        }
        sink.Type(value, variable.Name.c_str(), states[c]);
        fill.Types++;
    }
    for (size_t c = 0; c < (size_t)TypeContext::Count; c++) {
        if (refusedIn[c] > kLinesForOneContext) {
            fill.Report.push_back(std::string(Describe((TypeContext)c)) + ": " + std::to_string(refusedIn[c] - kLinesForOneContext)
                + " more are left unknown for the same kind of reason");
        }
    }
    return fill;
}

}
