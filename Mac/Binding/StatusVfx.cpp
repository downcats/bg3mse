#include <Binding/StatusVfx.h>

#include <atomic>
#include <cstring>
#include <iterator>

#include <Engine/EngineResolver.h>
#include <Platform/Log.h>

namespace bg3se::mac::binding {

namespace {

constexpr size_t kClasses = static_cast<size_t>(StatusVfxClass::Count);

constexpr char const* kEntryNames[kClasses] = {
    nullptr,
    "ecl::StatusVFXBoostMaterial",
    "ecl::StatusVFXApplyEffect",
    "ecl::StatusVFXEffect",
    "ecl::StatusVisual",
    "ecl::ManagedStatusVFX",
    "ecl::StatusVFXBeam",
};

std::atomic<void const*> gVtables[kClasses]{};
std::atomic<bool> gBound{ false };

}

char const* StatusVfxEntryName(StatusVfxClass cls)
{
    return cls < StatusVfxClass::Count ? kEntryNames[static_cast<size_t>(cls)] : nullptr;
}

StatusVfxClass ClassOfStatusVfx(void const* object)
{
    if (!object || !gBound.load(std::memory_order_acquire)) return StatusVfxClass::Base;
    void const* held;
    memcpy(&held, object, sizeof(held));
    if (!held) return StatusVfxClass::Base;
    for (size_t n = 1; n < kClasses; n++) {
        if (gVtables[n].load(std::memory_order_relaxed) == held) return static_cast<StatusVfxClass>(n);
    }
    return StatusVfxClass::Base;
}

void SetStatusVfxForTest(void const* const* vtables)
{
    gBound.store(false, std::memory_order_release);
    for (size_t n = 1; n < kClasses; n++) gVtables[n].store(vtables ? vtables[n] : nullptr, std::memory_order_relaxed);
    gBound.store(vtables != nullptr, std::memory_order_release);
}

std::string StatusVfxRefusal(engine::Resolution const& resolution)
{
    uint64_t seen[kClasses + std::size(kStatusVfxBaseEntryNames)]{};
    size_t count = 0;
    auto refusal = [&](char const* name) -> std::string {
        auto entry = resolution.Find(name);
        if (!entry) return std::string(name) + " is not in the engine table";
        if (!entry->Usable()) return std::string(name) + " is not resolved: " + entry->Reason;
        for (size_t n = 0; n < count; n++) {
            if (seen[n] == entry->VA) return std::string(name) + " is at the address of another class's vtable";
        }
        seen[count++] = entry->VA;
        return {};
    };
    for (size_t n = 1; n < kClasses; n++) {
        if (auto why = refusal(kEntryNames[n]); !why.empty()) return why;
    }
    for (auto name : kStatusVfxBaseEntryNames) {
        if (auto why = refusal(name); !why.empty()) return why;
    }
    return {};
}

void BindStatusVfx(engine::Resolution const& resolution)
{
    if (auto why = StatusVfxRefusal(resolution); !why.empty()) {
        Log(LogLevel::Warning, "the class of a status effect object is not known: %s. Every ecl::StatusVFX is given to Lua as the base class",
            why.c_str());
        SetStatusVfxForTest(nullptr);
        return;
    }
    void const* vtables[kClasses]{};
    for (size_t n = 1; n < kClasses; n++) vtables[n] = resolution.Address(*resolution.Find(kEntryNames[n]));
    SetStatusVfxForTest(vtables);
}

}
