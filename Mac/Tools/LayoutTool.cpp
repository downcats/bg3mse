// bg3mse-layouts components <Game.app>
// bg3mse-layouts members <Game.app>
// bg3mse-layouts objects <Game.app>
// bg3mse-layouts shapes <Game.app>
// bg3mse-layouts locks <Game.app>
// bg3mse-layouts story <Game.app>
//
// components:
// Compares the size this build gives every component type Norbyte's
// definitions bind with the sizes the game registers for it, decoded from the
// game binary on disk (Layout/ComponentRegistry.h; the game is not started),
// and the game's two sources of a component's size with each other
// (Layout/ComponentSizes.h is the second). Prints every difference, then the
// totals. Exits non-zero when the sizes cannot be decoded, when the game's
// two sources contradict each other, when a bound type is left without a
// size, or when one differs from the game's.
//
// members: compares where this build puts the members of bound component
// types with where the game's own code and data put the members it
// replicates (Layout/GameFields.h): the named member pointers, and what each
// component's serializer writes. Prints every difference, then the totals.
// Exits non-zero when the game's side cannot be decoded or is not all read,
// or when a value the game reads or writes is not one member of that size
// here (or several whole ones) and is not a difference that was reviewed.
//
// objects: compares the size this build gives the classes listed in
// Layout/BoundObjects.h with the size the game allocates for an object of
// each (Layout/ObjectSizes.h). Prints every difference, then the totals.
// Exits non-zero when the game's sizes cannot be decoded, when the game
// allocates a listed class with another size, or when it has no size for one.
//
// shapes: runs the proofs of the Mac shapes of game types (Layout/GameShapes.h)
// on the game binary on disk and prints each with the code it found. Exits
// non-zero when a shape is not confirmed.
//
// vtables: for the virtual functions of game classes that Norbyte's code
// calls or overrides (Layout/VirtualSlots.h), whether the slot this build
// uses is the slot the game's vtable holds the function in. Exits non-zero
// when one is not.
//
// locks: what the start-up gate of the lock functions (Layout/GameLockCalls.h)
// would decide for this game binary: the lock shapes confirmed and the
// game's lock functions found. Exits non-zero when the gate would stay shut.
//
// story: runs the proofs of the shapes of the story library's structures
// (Layout/OsirisShapes.h) on libOsiris.dylib on disk and prints each with the
// code it found. Exits non-zero when a shape is not confirmed.

#include <Layout/BoundComponents.h>
#include <Layout/BoundMembers.h>
#include <Layout/BoundObjects.h>
#include <Layout/ComponentRegistry.h>
#include <Layout/ComponentMembers.h>
#include <Layout/ComponentSizes.h>
#include <Layout/GameFields.h>
#include <Layout/GameLockCalls.h>
#include <Layout/GameShapes.h>
#include <Layout/ObjectSizes.h>
#include <Layout/OsirisShapes.h>
#include <Layout/VirtualSlots.h>
#include <Platform/Log.h>
#include <Platform/Symbols.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace bg3se::mac;

namespace {

// A binary mapped read-only for the rest of the process.
std::span<uint8_t const> MapFile(std::string const& path)
{
    int fd = open(path.c_str(), O_RDONLY);
    struct stat st{};
    if (fd < 0 || fstat(fd, &st) != 0) {
        if (fd >= 0) close(fd);
        fprintf(stderr, "cannot open %s\n", path.c_str());
        return {};
    }
    void* data = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED) {
        fprintf(stderr, "cannot map %s\n", path.c_str());
        return {};
    }
    return { static_cast<uint8_t const*>(data), (size_t)st.st_size };
}

bool OpenGame(std::string const& app, GameImage& image, char const* binary = "/Contents/MacOS/Baldur's Gate 3")
{
    std::string path = app + binary;
    auto file = MapFile(path);
    if (file.empty()) return false;
    if (!image.OpenFile(file)) {
        fprintf(stderr, "%s: no readable arm64 symbol table\n", path.c_str());
        return false;
    }
    return true;
}

int Components(GameImage const& image)
{
    // The two sources of a component's size in the game's code.
    auto added = DecodeComponentSizes(image);
    if (!added.Ok()) {
        printf("component sizes cannot be decoded: %s\n", added.Problem.c_str());
        return 1;
    }
    auto registry = DecodeComponentRegistry(image);
    if (!registry.Ok()) {
        printf("the component registrations cannot be decoded: %s\n", registry.Problem.c_str());
        return 1;
    }
    printf("game: %u registrations of a component type, %zu types read with both sizes, %zu not read\n",
        registry.Sites, registry.Components.size(), registry.Undecoded.size());
    for (auto const& what : registry.Undecoded) printf("  not read: %s\n", what.c_str());
    printf("game: %u AddComponent<T, Args...> functions, %u decoded (the size is stored at +%#x of the frame storage); "
           "%zu component types with a size, %zu without\n",
        added.Functions, added.FunctionsDecoded, added.SizeFieldOffset, added.Sizes.size(), added.Undecoded.size());
    for (auto const& name : added.Undecoded) printf("  not decoded: %s\n", name.c_str());

    // Where both give a size they are two statements of it by different
    // code: what AddComponent<T> allocates per entity is what the entity
    // storage holds, the registration's inline size.
    size_t both = 0, agree = 0;
    for (auto const& size : added.Sizes) {
        auto registered = registry.Find(size.Name);
        if (!registered) continue;
        both++;
        if (registered->InlineSize == size.Size) agree++;
        else printf("  the game's two sources differ for %s: registered %u, allocated %u\n", size.Name.c_str(), registered->InlineSize, size.Size);
    }
    printf("game: both sources give a size for %zu component types, and agree on %zu\n", both, agree);

    auto bound = BoundComponents();
    size_t same = 0, differ = 0, kind = 0, absent = 0, proxies = 0;
    printf("differences (bound type, game class, then what differs):\n");
    for (auto const& component : bound) {
        auto registered = registry.Find(component.EngineClass);
        if (!registered) {
            absent++;
            printf("  %-60s %-60s the game's registration of it was not read\n", component.Type, component.EngineClass);
            continue;
        }
        proxies += component.Proxy;
        // What EntitySystemHelpersBase::ValidateMappedComponentSize compares.
        // A proxy component is a pointer in the storage, and the game's
        // total is that pointer and the object behind it; any other
        // component is the object in the storage, and the two sizes are one.
        bool sizes = component.Proxy
            ? registered->InlineSize == sizeof(void*) && registered->TotalSize == sizeof(void*) + component.Size
            : registered->InlineSize == component.Size && registered->TotalSize == component.Size;
        bool frames = component.OneFrame == registered->OneFrame;
        if (sizes && frames) {
            same++;
            continue;
        }
        if (!sizes) {
            differ++;
            if (component.Proxy) {
                printf("  %-60s %-60s a pointer to %u bytes here; the game stores %u, and %u behind it\n", component.Type, component.EngineClass,
                    component.Size, registered->InlineSize, registered->TotalSize - registered->InlineSize);
            } else {
                printf("  %-60s %-60s %u bytes here; the game stores %u (%u with what is behind it)  (%+d)\n", component.Type, component.EngineClass,
                    component.Size, registered->InlineSize, registered->TotalSize, (int)component.Size - (int)registered->InlineSize);
            }
        }
        if (!frames) {
            kind++;
            printf("  %-60s %-60s declared %s, the game registers it as %s\n", component.Type, component.EngineClass,
                component.OneFrame ? "one-frame" : "persistent", registered->OneFrame ? "one-frame" : "persistent");
        }
    }
    printf("bound component types: %zu (%zu stored by pointer)\n", bound.size(), proxies);
    printf("  as the game registers them:           %zu\n", same);
    printf("  a different size:                     %zu\n", differ);
    printf("  one-frame where the game is not, or the reverse: %zu\n", kind);
    printf("  registration not read:                %zu\n", absent);
    // Every bound type is as the game registers it, on the game the
    // definitions were last compared with: anything else is a failure.
    return both == agree && absent == 0 && differ == 0 && kind == 0 ? 0 : 1;
}

int Members(GameImage const& image)
{
    auto classes = BoundClasses();
    auto game = DecodeBoundComponentFields(image, classes);
    if (!game.Ok()) {
        printf("the game's members cannot be decoded: %s\n", game.Problem.c_str());
        return 1;
    }
    size_t mapped = 0, members = 0;
    for (auto const& cls : classes) {
        if (!cls.Name) continue;
        mapped++;
        members += cls.Members.size();
    }
    printf("bound: %zu structures with a property map, %zu mapped members\n", mapped, members);
    printf("game: %zu named member pointers (%u not read), %zu functions of components (%u serializers not read, %u save-game functions "
           "that fetch no component by a call, %u destructors not read)\n", game.Named.size(), game.NamedNotRead, game.Serialized.size(),
        game.SerializersNotRead, game.SavesNotFollowed, game.DestructorsNotRead);
    if (!game.NotFound.empty()) printf("not found: %s\n", game.NotFound.c_str());

    auto compared = CompareComponentMembers(game, classes);
    printf("differences (the game's class and member, then what this build has there):\n");
    for (auto const& difference : compared.Differences) printf("  %s\n", difference.Describe().c_str());
    for (auto const& stale : compared.Stale) printf("  a reviewed difference is not there any more: %s\n", stale.c_str());
    auto const& counts = compared.Counts;
    printf("named member pointers into bound component types: %zu (%zu more into types that are not bound components)\n", compared.NamedBound,
        compared.NamedUnbound);
    printf("functions of bound component types (serializers, deserializers, save, load, destructors): %zu, writing or handing on %zu members at a "
           "constant place (%zu writes at a place not read)\n", compared.SerializedBound, compared.Fields, compared.WritesNotRead);
    printf("  loads and stores through the component: %zu of exactly one member, %zu of anything else (copies: no evidence)\n",
        compared.CopiesOfMembers, compared.Copies);
    printf("  one member of that size here, or several whole ones: %zu\n", counts[(size_t)Place::Member]);
    printf("  inside a member without a map:           %zu\n", counts[(size_t)Place::InsideMember]);
    printf("  no mapped member there:                  %zu\n", counts[(size_t)Place::NotMapped]);
    printf("  another width at the same place, reviewed: %zu\n", compared.ReviewedSeen);
    printf("  a scalar member elsewhere or of another size: %zu\n", counts[(size_t)Place::WrongScalar]);
    printf("  ends past the member it begins in:       %zu\n", counts[(size_t)Place::Straddles]);
    printf("  serializers that write no member's own bytes: %zu\n", compared.Nothing);
    // A value the game reads or writes that is not one member here means a
    // member in the wrong place, until it has been read against the game's
    // code and listed in Layout/ComponentMembers.cpp; in the game such a
    // type is withheld from Lua (Binding/Layouts.h). The comparison also has
    // to be of something: every member pointer and every serializer the game
    // has must be read, and every kind of function found (a reviewed
    // difference that is not found any more fails, which is also what a
    // decoder that reads less would show).
    bool read = game.NamedNotRead == 0 && game.SerializersNotRead == 0 && game.NotFound.empty() && compared.NamedBound != 0 && compared.Fields != 0;
    return read && compared.Contradictions() == 0 && compared.Stale.empty() ? 0 : 1;
}

int Objects(GameImage const& image)
{
    auto game = DecodeObjectSizes(image);
    if (!game.Ok()) {
        printf("the game's object sizes cannot be decoded: %s\n", game.Problem.c_str());
        return 1;
    }
    printf("game: %u allocations of a constant size, %zu classes always allocated with one size, %zu with more than one\n", game.Allocations,
        game.Sizes.size(), game.Conflicting.size());
    auto bound = BoundObjects();
    size_t same = 0, differ = 0, absent = 0;
    printf("differences (bound type, the game's symbol, then what differs):\n");
    for (auto const& object : bound) {
        auto size = game.Find(object.Witness);
        if (!size) {
            absent++;
            printf("  %-56s %s: %s\n", object.Type, object.Witness,
                game.Conflicts(object.Witness) ? "the game allocates it with more than one size" : "the game has no allocation of it");
        } else if (size->Size != object.Size) {
            differ++;
            printf("  %-56s %s: %u bytes here; the game allocates %u  (%+d)\n", object.Type, object.Witness, object.Size, size->Size,
                (int)object.Size - (int)size->Size);
        } else {
            same++;
        }
    }
    printf("listed classes: %zu\n", bound.size());
    printf("  the size the game allocates:  %zu\n", same);
    printf("  a different size:             %zu\n", differ);
    printf("  no size in the game:          %zu\n", absent);
    return differ == 0 && absent == 0 ? 0 : 1;
}

int Missing(GameImage const& image)
{
    auto registry = DecodeComponentRegistry(image);
    if (!registry.Ok()) {
        printf("the component registrations cannot be decoded: %s\n", registry.Problem.c_str());
        return 1;
    }
    for (auto const& component : BoundComponents()) {
        if (!registry.Find(component.EngineClass)) printf("%s\t%s\t%u\n", component.Type, component.EngineClass, component.Size);
    }
    return 0;
}

int Shapes(GameImage const& image)
{
    auto shapes = CheckGameShapes(image);
    for (auto const& proof : shapes.Proofs) {
        printf("  %-26s %-84s %4u/%016llx  %s\n", Describe(proof.Shape), proof.Function, proof.Code.Instructions,
            (unsigned long long)proof.Code.Hash, proof.Problem.empty() ? "holds" : proof.Problem.c_str());
    }
    size_t confirmed = 0;
    for (uint8_t n = 0; n < (uint8_t)GameShape::Count; n++) {
        bool ok = shapes.Confirmed((GameShape)n);
        confirmed += ok;
        printf("%-26s %s\n", Describe((GameShape)n), ok ? "confirmed" : "NOT CONFIRMED");
    }
    printf("%zu of %u shapes confirmed\n", confirmed, (unsigned)GameShape::Count);
    LogUnconfirmedShapes(shapes);
    return shapes.AllConfirmed() ? 0 : 1;
}

int Vtables(GameImage const& image)
{
    auto results = CheckVirtualSlots(image, DeclaredVirtuals());
    auto shapes = CheckGameShapes(image);
    RequireResultProofs(results, shapes);
    RequireMemberShapes(results, shapes, ClassMemberShapes());
    size_t bad = 0;
    char const* shown = "";
    for (auto const& result : results) {
        if (strcmp(shown, result.Of->Class) != 0) {
            shown = result.Of->Class;
            printf("%s\n", shown);
        }
        printf("  %-44s slot %2u  %s%s\n", result.Of->Method, result.Of->Slot, result.Problem.empty() ? "as in the game" : "** ",
            result.Problem.c_str());
        bad += !result.Problem.empty();
    }
    printf("%zu virtual functions, %zu not in the game's slot\n", results.size(), bad);
    return bad || results.empty() ? 1 : 0;
}

int Locks(GameImage const& image)
{
    auto check = CheckGameLocks(image);
    for (int n = 0; n < GameLockCheck::Count; n++) {
        if (check.VAs[n]) printf("  %-52s %#llx\n", check.Names[n], (unsigned long long)check.VAs[n]);
        else printf("  %-52s -\n", check.Names[n]);
    }
    if (!check.Ok()) {
        printf("the game's locks cannot be taken: %s\n", check.Problem.c_str());
        return 1;
    }
    printf("the lock shapes are confirmed and the game's lock functions are found: the gate would open\n");
    return 0;
}

int Story(GameImage const& library)
{
    auto shapes = osiris::CheckOsirisShapes(library);
    for (auto const& proof : shapes.Proofs) {
        printf("  %-18s %-100s %4u/%016llx  %s\n", osiris::Describe(proof.Of), proof.Function, proof.Code.Instructions,
            (unsigned long long)proof.Code.Hash, proof.Problem.empty() ? "holds" : proof.Problem.c_str());
    }
    size_t confirmed = 0;
    for (uint8_t n = 0; n < (uint8_t)osiris::Shape::Count; n++) {
        bool ok = shapes.Confirmed((osiris::Shape)n);
        confirmed += ok;
        printf("%-18s %s\n", osiris::Describe((osiris::Shape)n), ok ? "confirmed" : "NOT CONFIRMED");
    }
    printf("%zu of %u shapes of the story library confirmed\n", confirmed, (unsigned)osiris::Shape::Count);
    return shapes.AllConfirmed() ? 0 : 1;
}

}

int main(int argc, char** argv)
{
    SetLogStream(stdout);
    std::string_view command = argc == 3 ? argv[1] : "";
    if (command != "components" && command != "members" && command != "objects" && command != "unsized" && command != "shapes" && command != "vtables" && command != "locks" && command != "story") {
        fprintf(stderr, "usage: bg3mse-layouts components <Game.app>   compare bound component sizes with the game binary\n"
                        "       bg3mse-layouts members <Game.app>      compare where bound components' members are with the game binary\n"
                        "       bg3mse-layouts objects <Game.app>      compare the sizes of listed classes with what the game allocates\n"
                        "       bg3mse-layouts unsized <Game.app>      list the bound component types the binary has no size for\n"
                        "       bg3mse-layouts shapes <Game.app>       check the Mac shapes of game types against the game's code\n"
                        "       bg3mse-layouts vtables <Game.app>      check the slots of the virtual functions Norbyte's code uses\n"
                        "       bg3mse-layouts locks <Game.app>        whether the game's lock functions can be used\n"
                        "       bg3mse-layouts story <Game.app>        check the shapes of the story library's structures against libOsiris.dylib\n");
        return 2;
    }
    GameImage image;
    if (command == "story") {
        if (!OpenGame(argv[2], image, "/Contents/Frameworks/libOsiris.dylib")) return 1;
        return Story(image);
    }
    if (!OpenGame(argv[2], image)) return 1;
    if (command == "components") return Components(image);
    if (command == "members") return Members(image);
    if (command == "objects") return Objects(image);
    if (command == "unsized") return Missing(image);
    if (command == "locks") return Locks(image);
    if (command == "vtables") return Vtables(image);
    return Shapes(image);
}
