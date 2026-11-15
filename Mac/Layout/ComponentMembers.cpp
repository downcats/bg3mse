#include <Layout/ComponentMembers.h>

#include <algorithm>
#include <cstdio>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace bg3se::mac {

namespace {

struct Found
{
    Place Where;
    std::string Path;       // the member, e.g. "Hp.Value"
    uint32_t Offset{ 0 }, Size{ 0 };    // of the innermost member reached
};

Found Leaf(bool scalar, uint32_t memberSize, uint32_t inner, uint32_t size, std::string path)
{
    bool whole = inner == 0 && (size == 0 || size == memberSize);
    if (whole) return { Place::Member, std::move(path), 0, memberSize };
    return { scalar ? Place::WrongScalar : Place::InsideMember, std::move(path), 0, memberSize };
}

// Whether `size` bytes from `first` are exactly `first` and the members that
// follow it, with nothing between them: the game writing as one value what
// the definitions declare as several.
bool CoversWholeMembers(BoundClass const& cls, BoundMember const& first, uint32_t size)
{
    uint32_t end = first.Offset + first.Size, wanted = first.Offset + size;
    while (end < wanted) {
        BoundMember const* next = nullptr;
        for (auto const& member : cls.Members) {
            if (member.Offset == end) next = &member;
        }
        if (!next) return false;
        end += next->Size;
    }
    return end == wanted;
}

// Differences that were read against the game's code and are not a member
// in another place: the game writes a value of another width than the
// member the definitions declare there, at the same offset.
struct Reviewed
{
    char const* Class;
    uint32_t Offset, Size;
    char const* Why;
};

constexpr Reviewed kReviewed[] = {
    { "eoc::CombinedLightComponent", 0x8, 4, "the game's value is four bytes; TemplateType is its first byte, and the next member follows the four" },
    { "eoc::InvisibilityComponent", 0x0, 2, "the game's value is two bytes; field_0 is its first byte, and the next member is at +4" },
    { "eoc::photo_mode::DummyEquipmentVisualComponent", 0x0, 1, "four separate bytes in the game; field_0 is the four as one integer" },
    { "eoc::photo_mode::DummyEquipmentVisualComponent", 0x1, 1, "four separate bytes in the game; field_0 is the four as one integer" },
    { "eoc::photo_mode::DummyEquipmentVisualComponent", 0x2, 1, "four separate bytes in the game; field_0 is the four as one integer" },
    { "eoc::photo_mode::DummyEquipmentVisualComponent", 0x3, 1, "four separate bytes in the game; field_0 is the four as one integer" },
};

Found Locate(std::span<BoundClass const> classes, int32_t id, uint32_t offset, uint32_t size)
{
    for (; id >= 0; id = classes[id].Parent) {
        auto const& cls = classes[id];
        for (auto const& member : cls.Members) {
            if (offset < member.Offset || offset >= member.Offset + member.Size) continue;
            uint32_t inner = offset - member.Offset;
            Found found;
            if (inner + std::max(size, 1u) > member.Size) {
                bool whole = inner == 0 && CoversWholeMembers(cls, member, size);
                found = { whole ? Place::Member : Place::Straddles, member.Name, 0, member.Size };
            } else if (inner == 0 && size == member.Size) {
                found = { Place::Member, member.Name, 0, member.Size };
            } else if (member.Class >= 0) {
                found = Locate(classes, member.Class, inner, size);
                found.Path = found.Path.empty() ? member.Name : std::string(member.Name) + "." + found.Path;
                found.Offset += member.Offset;
                return found;
            } else if (member.ElementSize) {
                uint32_t within = inner % member.ElementSize;
                std::string path = std::string(member.Name) + "[" + std::to_string(inner / member.ElementSize) + "]";
                if (within + std::max(size, 1u) > member.ElementSize) {
                    found = { Place::Straddles, path, 0, member.ElementSize };
                } else if (member.ElementClass >= 0 && !(within == 0 && size == member.ElementSize)) {
                    found = Locate(classes, member.ElementClass, within, size);
                    found.Path = found.Path.empty() ? path : path + "." + found.Path;
                    found.Offset += member.Offset + inner - within;
                    return found;
                } else {
                    found = Leaf(member.ElementScalar, member.ElementSize, within, size, path);
                }
                found.Offset = member.Offset + inner - within;
                return found;
            } else {
                found = Leaf(member.Scalar, member.Size, inner, size, member.Name);
            }
            found.Offset = member.Offset;
            return found;
        }
        // Not one of its own members: one of the structure it inherits from.
        if (cls.Parent < 0 || offset < cls.ParentOffset) break;
        offset -= cls.ParentOffset;
    }
    return { Place::NotMapped, {} };
}

}

bool MemberDifference::Contradicts() const
{
    return !Reviewed && (Where == Place::WrongScalar || Where == Place::Straddles);
}

std::string MemberDifference::Describe() const
{
    char line[1024];
    std::string game = GameName.empty() ? std::string() : " (" + GameName + ")";
    if (Where == Place::Straddles && Path.empty()) {
        snprintf(line, sizeof(line), "%s: the game's %s touches %u bytes at +%#x%s, past the end of the %u bytes here", Class.c_str(), Source, Size,
            Offset, game.c_str(), ClassSize);
        return line;
    }
    char const* what = Reviewed ? Reviewed
        : Where == Place::InsideMember ? "inside a member that has no map of its own"
        : Where == Place::NotMapped ? "no mapped member there"
        : Where == Place::WrongScalar ? "a scalar member that begins elsewhere or has another size"
        : "a value that ends past the member it begins in";
    char const* verb = How == FieldAccess::Written ? "writes"
        : How == FieldAccess::Passed || How == FieldAccess::PassedOn ? "hands on the address of"
        : How == FieldAccess::Read ? "reads" : "stores";
    if (std::string_view(Source) == "pointer") verb = "names";
    snprintf(line, sizeof(line), "%s: the game's %s %s %u bytes at +%#x%s; here %s%s", Class.c_str(), Source, verb, Size, Offset, game.c_str(),
        Reviewed ? "reviewed: " : "", what);
    std::string text = line;
    if (!Path.empty()) {
        snprintf(line, sizeof(line), ": %s at +%#x, %u bytes", Path.c_str(), PathOffset, PathSize);
        text += line;
    }
    return text;
}

size_t MemberComparison::Contradictions() const
{
    return (size_t)std::count_if(Differences.begin(), Differences.end(), [](MemberDifference const& d) { return d.Contradicts(); });
}

MemberComparison CompareComponentMembers(GameFieldScan const& game, std::span<BoundClass const> classes)
{
    MemberComparison result;
    std::unordered_map<std::string_view, int32_t> byClass;
    for (size_t n = 0; n < classes.size(); n++) {
        if (classes[n].EngineClass) byClass.emplace(classes[n].EngineClass, (int32_t)n);
    }
    auto classOf = [&](std::string const& engineClass) -> int32_t {
        auto found = byClass.find(engineClass);
        return found == byClass.end() ? -1 : found->second;
    };

    bool seen[std::size(kReviewed)]{};
    auto compare = [&](char const* source, std::string const& engineClass, int32_t id, uint32_t offset, uint32_t size, std::string const& gameName,
        FieldAccess how) {
        MemberDifference difference;
        difference.Source = source;
        difference.Class = engineClass;
        difference.EngineClass = classes[id].EngineClass;
        difference.Offset = offset;
        difference.Size = size;
        difference.GameName = gameName;
        difference.How = how;
        difference.Where = Place::Member;
        difference.ClassSize = classes[id].Size;
        // A function's load or store is the code copying bytes as wide as
        // it likes, several members and their padding at once, on paths
        // that need not run: it confirms a member where it is exactly one,
        // and says nothing otherwise. So does an address handed to a
        // function that was not followed, and one just past the end (the
        // end of a range). A named member pointer is not a copy.
        bool copy = std::string_view(source) != "pointer"
            && (how == FieldAccess::Read || how == FieldAccess::Stored || how == FieldAccess::PassedOn
                || (how == FieldAccess::Passed && offset == classes[id].Size));
        if (offset >= classes[id].Size || offset + size > classes[id].Size) {
            if (copy) {
                result.Copies++;
                return;
            }
            difference.Where = Place::Straddles;
        } else {
            auto found = Locate(classes, id, offset, size);
            if (copy) {
                if (found.Where == Place::Member && found.Size == size) result.CopiesOfMembers++;
                else result.Copies++;
                return;
            }
            if (found.Where == Place::Member) {
                result.Counts[(size_t)Place::Member]++;
                return;
            }
            difference.Where = found.Where;
            difference.Path = std::move(found.Path);
            difference.PathOffset = found.Offset;
            difference.PathSize = found.Size;
            if (found.Where == Place::WrongScalar || found.Where == Place::Straddles) {
                for (size_t n = 0; n < std::size(kReviewed) && !difference.Reviewed; n++) {
                    if (engineClass == kReviewed[n].Class && offset == kReviewed[n].Offset && size == kReviewed[n].Size) {
                        seen[n] = true;
                        difference.Reviewed = kReviewed[n].Why;
                    }
                }
            }
        }
        if (difference.Reviewed) result.ReviewedSeen++;
        else result.Counts[(size_t)difference.Where]++;
        result.Differences.push_back(std::move(difference));
    };

    for (auto const& field : game.Named) {
        int32_t id = classOf(field.Class);
        if (id < 0) {
            result.NamedUnbound++;
            continue;
        }
        result.NamedBound++;
        compare("pointer", field.Class, id, field.Offset, field.Size, field.Member, FieldAccess::Read);
    }
    for (auto const& component : game.Serialized) {
        int32_t id = classOf(component.Class);
        if (id < 0) continue;
        result.SerializedBound++;
        result.WritesNotRead += component.WritesNotRead;
        // A type without members has a serializer that writes nothing, and
        // one that writes only what its members point to gives no place.
        if (component.Fields.empty() && !component.WritesNotRead) result.Nothing++;
        for (auto const& field : component.Fields) {
            if (field.How == FieldAccess::Written || field.How == FieldAccess::Passed) result.Fields++;
            compare(component.Source, component.Class, id, field.Offset, field.Size, {}, field.How);
        }
    }
    for (size_t n = 0; n < std::size(kReviewed); n++) {
        if (!seen[n]) {
            char line[256];
            snprintf(line, sizeof(line), "%s +%#x, %u bytes", kReviewed[n].Class, kReviewed[n].Offset, kReviewed[n].Size);
            result.Stale.push_back(line);
        }
    }
    return result;
}

GameFieldScan DecodeBoundComponentFields(GameImage const& image, std::span<BoundClass const> classes)
{
    std::unordered_set<std::string_view> bound;
    for (auto const& cls : classes) {
        if (cls.EngineClass) bound.insert(cls.EngineClass);
    }
    return DecodeGameFields(image, [&](std::string_view cls) { return bound.contains(cls); });
}

}
