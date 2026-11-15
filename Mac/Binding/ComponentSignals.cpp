#include <Binding/ComponentSignalCheck.h>

#include <Platform/Log.h>

#include <cstring>
#include <mutex>
#include <unordered_set>

namespace bg3se::mac::binding {

namespace {

std::string Hex(uint64_t value)
{
    char text[24];
    snprintf(text, sizeof(text), "0x%llx", (unsigned long long)value);
    return text;
}

// The loaded address `address` as a link-time address of `image`.
uint64_t LinkTime(GameImage const& image, uint64_t address)
{
    return address - (uint64_t)(uintptr_t)image.LoadedAt(image.BaseVA()) + image.BaseVA();
}

bool CheckSender(GameImage const& image, ComponentSignalSender const& sender, uint64_t table, size_t slot, std::string& problem)
{
    auto function = image.PointerAt(table + slot * sizeof(uint64_t));
    if (!function) {
        problem = "slot " + std::to_string(slot) + " of the operations' virtual table cannot be read (" + Hex(table) + ")";
        return false;
    }
    uint64_t va = *function;
    if ((va & 3) || va < image.TextBegin() || va >= image.TextEnd()) {
        problem = std::string(sender.Name) + " is not in the game's code (" + Hex(va) + ")";
        return false;
    }
    size_t available = (size_t)((image.TextEnd() - va) / 4);
    if (!IsComponentSignalSender(sender, image.CodeAt(va), available)) {
        auto found = ShapeOfRange(image.CodeAt(va), available < sender.Inline.Instructions ? (uint32_t)available : sender.Inline.Instructions);
        problem = std::string(sender.Name) + " is not the code that was read (its first " + std::to_string(found.Instructions)
            + " instructions have shape " + Hex(found.Hash) + ")";
        return false;
    }
    return true;
}

}

bool IsComponentSignalSender(ComponentSignalSender const& sender, void const* code, size_t available)
{
    for (auto const& form : { sender.Inline, sender.Boxed }) {
        if (available >= form.Instructions && ShapeOfRange(code, form.Instructions) == form) return true;
    }
    return false;
}

bool ChecksComponentSignalTable(GameImage const& image, uint64_t table, std::string& problem)
{
    constexpr size_t slots = kComponentOpsSendDetached + 1;
    if (!table || (table & 7) || !image.DataAt(table, slots * sizeof(uint64_t))) {
        problem = "the operations object's virtual table is not in the game's image (" + Hex(table) + ")";
        return false;
    }
    return CheckSender(image, kSendComponentAttached, table, kComponentOpsSendAttached, problem)
        && CheckSender(image, kSendComponentDetached, table, kComponentOpsSendDetached, problem);
}

namespace {

uint64_t WordAt(void const* object, size_t offset)
{
    uint64_t word;
    memcpy(&word, static_cast<char const*>(object) + offset, sizeof(word));
    return word;
}

// What the senders read of the operations object: the callbacks whose
// connections they call, and the world they pass.
bool CheckMembers(void const* componentOps, void const* callbacks, void const* world, std::string& problem)
{
    if (!callbacks || WordAt(componentOps, kComponentOpsCallbacks) != reinterpret_cast<uint64_t>(callbacks)) {
        problem = "the operations object's callbacks are not the component type's (" + Hex(WordAt(componentOps, kComponentOpsCallbacks)) + ")";
        return false;
    }
    if (!world || WordAt(componentOps, kComponentOpsWorld) != reinterpret_cast<uint64_t>(world)) {
        problem = "the operations object's world is not the entity world (" + Hex(WordAt(componentOps, kComponentOpsWorld)) + ")";
        return false;
    }
    return true;
}

}

bool ChecksComponentSignals(GameImage const& image, void const* componentOps, void const* callbacks, void const* world,
    std::string& problem)
{
    if (!componentOps) {
        problem = "the component type has no operations object";
        return false;
    }
    if (image.FromFile()) {
        problem = "the game's image is a file";
        return false;
    }
    return ChecksComponentSignalTable(image, LinkTime(image, WordAt(componentOps, 0)), problem)
        && CheckMembers(componentOps, callbacks, world, problem);
}

bool ComponentSignalsConfirmed(void const* componentOps, void const* callbacks, void const* world)
{
    // Subscriptions come from the client's and the server's Lua threads.
    static std::mutex mutex;
    static std::unordered_set<uint64_t> confirmedTables;
    static std::unordered_set<void const*> refused;
    std::lock_guard lock(mutex);

    std::string problem;
    bool ok = false;
    if (!componentOps) {
        problem = "the component type has no operations object";
    } else if (auto image = GameImage::Current(); !image) {
        problem = "the game's image cannot be read";
    } else if (uint64_t table = WordAt(componentOps, 0); confirmedTables.contains(table)) {
        ok = CheckMembers(componentOps, callbacks, world, problem);
    } else {
        ok = ChecksComponentSignals(*image, componentOps, callbacks, world, problem);
        // The table and its code are the game's, whatever this object's members are.
        std::string ofTable;
        if (ok || ChecksComponentSignalTable(*image, LinkTime(*image, table), ofTable)) confirmedTables.insert(table);
    }

    if (!ok && refused.insert(componentOps).second) {
        Log(LogLevel::Error, "component events are withheld for the component type whose operations object is at %p: %s",
            componentOps, problem.c_str());
    }
    return ok;
}

}
