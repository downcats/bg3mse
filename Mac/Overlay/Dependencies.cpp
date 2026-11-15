#include <Overlay/Dependencies.h>

#include <Engine/EngineTable.h>
#include <Engine/Entries/OverlayNames.h>
#include <Overlay/Controller.h>
#include <Overlay/GameInput.h>
#include <Overlay/GameTextures.h>

#include <bit>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <optional>
#include <xlocale.h>

namespace bg3se::mac::overlay {

using namespace engine::overlay;

namespace {

std::string Format(char const* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string Format(char const* fmt, ...)
{
    char text[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf_l(text, sizeof(text), LC_C_LOCALE, fmt, args);
    va_end(args);
    return text;
}

struct Checker
{
    engine::Resolution const& Resolution;
    ObjcClasses const& Classes;
    Dependencies& Out;

    bool Add(Part part, std::string what, bool passed, std::string detail)
    {
        Out.Checks.push_back({ part, std::move(what), passed, std::move(detail) });
        return passed;
    }

    // The entry, when it resolved; a failed check otherwise.
    engine::Resolved const* Entry(Part part, char const* name)
    {
        auto r = Resolution.Find(name);
        if (!r) {
            Add(part, name, false, "the table resolved has no such entry");
            return nullptr;
        }
        if (!r->Usable()) {
            Add(part, name, false, r->Reason.empty() ? std::string("unresolved") : r->Reason);
            return nullptr;
        }
        Add(part, name, true, Format("%s at %#llx", engine::Describe(r->Of->What), (unsigned long long)r->VA));
        return r;
    }

    // The entry's address: of this process when the game is loaded here.
    uint64_t Address(Part part, engine::Resolved const& r)
    {
        if (!Out.Live) return r.VA;
        auto address = (uint64_t)(uintptr_t)Resolution.Address(r);
        if (!address) Add(part, r.Of->Name, false, "its image is not loaded in this process");
        return address;
    }

    // The value the resolver read for the operand labelled `label`.
    std::optional<uint64_t> Operand(Part part, engine::Resolved const& r, char const* label)
    {
        auto const& operands = r.Of->Operands;
        for (size_t i = 0; i < operands.size() && i < r.Operands.size(); i++) {
            if (!strcmp(operands[i].What, label)) return r.Operands[i];
        }
        Add(part, label, false, Format("%s has no such operand", r.Of->Name));
        return {};
    }

    // Two places that must read the same offset.
    bool Same(Part part, char const* what, std::optional<uint64_t> a, std::optional<uint64_t> b)
    {
        if (!a || !b) return false;
        return Add(part, what, *a == *b, *a == *b ? Format("0x%llx in both", (unsigned long long)*a)
            : Format("0x%llx in one, 0x%llx in the other", (unsigned long long)*a, (unsigned long long)*b));
    }

    void Method(Part part, char const* className, char const* selector, char const* types, uint64_t code)
    {
        Out.Checks.push_back(CheckReplacedMethod(Classes, part, className, selector, types, code));
    }

    // An offset the game's code shows against the one the build was made for.
    void Place(Part part, char const* what, std::optional<uint64_t> found, uint32_t built, uint32_t& out)
    {
        if (!found) return;
        out = (uint32_t)*found;
        Add(part, what, *found == built, *found == built ? Format("+0x%x, as the build has it", built)
            : Format("+0x%llx in the game, +0x%x in the build", (unsigned long long)*found, built));
    }

    void Ancestor(Part part, char const* className, char const* ancestor)
    {
        auto derives = Classes.DerivesFrom(className, ancestor);
        std::string what = Format("%s is a %s", className, ancestor);
        if (!derives) Add(part, what, true, "known in the running game only: the superclass is bound when the game is loaded");
        else Add(part, what, *derives, *derives ? "it derives from it" : "it does not derive from it");
    }

    void Frame()
    {
        constexpr Part part = Part::Frame;
        if (auto vtable = Entry(part, kRenderFrameVtable)) {
            int32_t offset = -1;
            for (auto const& slot : vtable->Slots) {
                if (!strcmp(slot.Method->Name, kFlipBuffersSlot)) offset = slot.Offset;
            }
            if (Add(part, "the FlipBuffers slot", offset >= 0, offset >= 0 ? Format("+0x%x", offset) : "not found in the vtable")) {
                if (uint64_t address = Address(part, *vtable)) Out.FlipBuffersSlot = address + (uint32_t)offset;
            }
        }
        std::optional<uint64_t> buffer;
        if (auto flip = Entry(part, kFlipBuffers)) {
            Out.FlipBuffers = Address(part, *flip);
            auto drawable = Operand(part, *flip, kFrameDrawable);
            auto renderer = Operand(part, *flip, kFrameRenderer);
            buffer = Operand(part, *flip, kRendererBuffer);
            // Both of its paths present on the same buffer.
            Same(part, "the renderer on both paths", renderer, Operand(part, *flip, kFrameRendererPaced));
            Same(part, "the command buffer on both paths", buffer, Operand(part, *flip, kRendererBufferPaced));
            Out.FrameDrawable = (uint32_t)drawable.value_or(0);
            Out.FrameRenderer = (uint32_t)renderer.value_or(0);
            Out.RendererCommandBuffer = (uint32_t)buffer.value_or(0);
        }
        if (auto commit = Entry(part, kCommitCommandBuffer)) {
            Out.CommitCommandBuffer = Address(part, *commit);
            Out.CommitCommandBufferBytes = commit->End - commit->VA;
            // What FlipBuffers schedules the present on is what this commits.
            Same(part, "the buffer presented and the buffer committed", buffer, Operand(part, *commit, kCommittedBuffer));
        }
    }

    void Events()
    {
        constexpr Part part = Part::Events;
        if (auto send = Entry(part, kSendEvent)) {
            Out.SendEvent = Address(part, *send);
            Method(part, kBG3Application, kSendEventSelector, kSendEventTypes, Out.SendEvent);
            Ancestor(part, kBG3Application, "NSApplication");
        }
        if (auto init = Entry(part, kViewInit)) {
            Out.ViewInit = Address(part, *init);
            Method(part, kLSMTLView, kViewInitSelector, kViewInitTypes, Out.ViewInit);
            Ancestor(part, kLSMTLView, "NSView");
            auto layer = Classes.InstanceVariable(kLSMTLView, kViewLayerVariable);
            std::string what = Format("%s.%s", kLSMTLView, kViewLayerVariable);
            if (!layer.Found) Add(part, what, false, "the class has no such instance variable");
            else Add(part, what, layer.Type == kViewLayerType, layer.Type == kViewLayerType ? layer.Type
                : Format("its type is %s, expected %s", layer.Type.c_str(), kViewLayerType));
        }
    }

    void Injection()
    {
        constexpr Part part = Part::Injection;
        if (auto inject = Entry(part, kInjectInput)) {
            Out.InjectInput = Address(part, *inject);
            if (auto pressed = Operand(part, *inject, kChangePressed)) {
                bool same = *pressed == offsetof(GameInputChange, Pressed);
                Add(part, kChangePressed, same, same ? Format("+0x%llx, as GameInputChange has it", (unsigned long long)*pressed)
                    : Format("+0x%llx in the game, +0x%zx in GameInputChange", (unsigned long long)*pressed, offsetof(GameInputChange, Pressed)));
            }
        }
        if (auto manager = Entry(part, kInputManager)) Out.InputManager = Address(part, *manager);
        if (auto keys = Entry(part, kKeyboardKeys)) {
            Out.KeyboardKeys = Address(part, *keys);
            if (auto last = Operand(part, *keys, kLastKeyCode)) {
                // The table's symbol must have room for every entry the game indexes.
                bool fits = (*last + 1) * sizeof(uint16_t) <= keys->Of->Bytes;
                if (Add(part, "key codes", fits, Format("%llu, of 16 bits each", (unsigned long long)*last + 1))) Out.KeyCodes = (uint32_t)*last + 1;
            }
        }
    }

    // The modifier keys are queued as the key view queues them: from the same
    // table, through the same function, with the pressed byte in its place.
    void InjectedModifiers()
    {
        constexpr Part part = Part::InjectedModifiers;
        auto modifiers = Entry(part, kModifierKeys);
        if (!modifiers) return;
        uint64_t table = Address(part, *modifiers);
        Add(part, "the table of the key view", table && table == Out.KeyboardKeys,
            table == Out.KeyboardKeys ? std::string("the one -[LSMTLView keyDown:] reads") : std::string("not the one -[LSMTLView keyDown:] reads"));
        if (auto pressed = Operand(part, *modifiers, kModifierPressed)) {
            bool same = *pressed == offsetof(GameInputChange, Pressed);
            Add(part, kModifierPressed, same, same ? Format("+0x%llx, as GameInputChange has it", (unsigned long long)*pressed)
                : Format("+0x%llx in the game, +0x%zx in GameInputChange", (unsigned long long)*pressed, offsetof(GameInputChange, Pressed)));
        }
    }

    void Variable(Part part, char const* className, char const* name, char const* type)
    {
        auto variable = Classes.InstanceVariable(className, name);
        std::string what = Format("%s.%s", className, name);
        if (!variable.Found) Add(part, what, false, "the class has no such instance variable");
        else Add(part, what, variable.Type == type, variable.Type == type ? variable.Type : Format("its type is %s, expected %s", variable.Type.c_str(), type));
    }

    // The view keeps the cursor it wants, which the overlay puts back when it
    // lets the pointer go, and says whether the game holds the pointer, when
    // the overlay does not move it.
    void Pointer()
    {
        constexpr Part part = Part::Pointer;
        Variable(part, kLSMTLView, kViewCursorVariable, kViewCursorType);
        Variable(part, kLSMTLView, kViewCaptureVariable, kViewCaptureType);
    }

    void Textures()
    {
        constexpr Part part = Part::Textures;
        std::optional<uint64_t> manager, guid;
        if (auto add = Entry(part, kAddTexture)) {
            Out.AddTexture = Address(part, *add);
            manager = Operand(part, *add, kTextureManagerAdded);
            guid = Operand(part, *add, kResourceGuidAdded);
            Place(part, kResourceSourceFile, Operand(part, *add, kResourceSourceFile), layout::kResourceSourceFile, Out.ResourceSourceFile);
            Place(part, kResourceType, Operand(part, *add, kResourceType), layout::kResourceType, Out.ResourceType);
        }
        if (auto instantiate = Entry(part, kInstantiateTexture)) {
            Out.InstantiateTexture = Address(part, *instantiate);
            Same(part, "the texture manager for both calls", manager, Operand(part, *instantiate, kTextureManager));
            Same(part, "the GUID for both calls", guid, Operand(part, *instantiate, kResourceGuid));
            auto texture = Operand(part, *instantiate, kMetalTexture);
            Same(part, "the MTLTexture in both functions", texture, Operand(part, *instantiate, kMetalTextureCreated));
            Place(part, kResourceGuid, guid, layout::kResourceGuid, Out.ResourceGuid);
            Place(part, kResourceState, Operand(part, *instantiate, kResourceState), layout::kResourceState, Out.ResourceState);
            Place(part, kResourceStreaming, Operand(part, *instantiate, kResourceStreaming), layout::kResourceStreaming, Out.ResourceStreaming);
            Place(part, kResourceSRGB, Operand(part, *instantiate, kResourceSRGB), layout::kResourceSRGB, Out.ResourceSRGB);
            Out.MetalTexture = (uint32_t)texture.value_or(0);
            Out.ResourceBanks = (uint32_t)Operand(part, *instantiate, kResourceBanks).value_or(0);
        }
        if (auto put = Entry(part, kPutTexture)) {
            Out.PutTexture = Address(part, *put);
            // The release takes the manager from where the load takes it.
            Same(part, "the texture manager for load and release", manager, Operand(part, *put, kPutTextureManager));
            Out.TextureManager = (uint32_t)manager.value_or(0);
        }
        if (auto resources = Entry(part, kResourceManager)) Out.ResourceManager = Address(part, *resources);
        // The function the offsets were read from is, instruction for
        // instruction, the one that was read: its texture loading is the
        // sequence the overlay repeats.
        Entry(part, kLoadingHost);
    }

    void Controllers()
    {
        constexpr Part part = Part::Controllers;
        if (auto inject = Entry(part, kInjectInput)) {
            Out.InjectInput = Address(part, *inject);
            Out.InjectInputBytes = inject->End - inject->VA;
        }
        if (auto update = Entry(part, kUpdateControls)) {
            Out.UpdateControls = Address(part, *update);
            Out.UpdateControlsBytes = update->End - update->VA;
            Out.DeviceEnabled = (uint32_t)Operand(part, *update, kDeviceEnabled).value_or(0);
            Out.DeviceController = (uint32_t)Operand(part, *update, kDeviceController).value_or(0);
            Out.DevicePlayer = (uint32_t)Operand(part, *update, kDevicePlayer).value_or(0);
            // A change's device is what Overlay/Controller.h takes it for.
            if (auto first = Operand(part, *update, kControllerDeviceBase)) {
                bool same = *first == overlay::kFirstControllerDevice;
                Add(part, kControllerDeviceBase, same, same ? Format("%llu, as Overlay/Controller.h has it", (unsigned long long)*first)
                    : Format("%llu in the game, %u in Overlay/Controller.h", (unsigned long long)*first, overlay::kFirstControllerDevice));
            }
        }
        if (auto buttons = Entry(part, kControllerButtons)) {
            Out.ControllerButtons = Address(part, *buttons);
            Out.ControllerButtonCount = buttons->Of->Bytes / sizeof(uint16_t);
        }
        // A controller's arrival and departure, under the device id its changes carry.
        if (auto registers = Entry(part, kRegisterDevice)) {
            Out.RegisterDevice = Address(part, *registers);
            Out.RegisterDeviceBytes = registers->End - registers->VA;
            auto id = Operand(part, *registers, kRegisteredDeviceId);
            Same(part, "the device id both OpenControllers register", id, Operand(part, *registers, kRegisteredDeviceIdHid));
            Out.RegisteredDeviceId = (uint32_t)id.value_or(0);
            Same(part, "the player index registered and read (GameController)", Operand(part, *registers, kRegisteredPlayer), Out.DevicePlayer);
            Same(part, "the player index registered and read (IOHID)", Operand(part, *registers, kRegisteredPlayerHid), Out.DevicePlayer);
            FirstController(part, kRegisteredBase, Operand(part, *registers, kRegisteredBase));
            FirstController(part, kRegisteredBaseHid, Operand(part, *registers, kRegisteredBaseHid));
        }
        if (auto unregisters = Entry(part, kUnregisterDevice)) {
            Out.UnregisterDevice = Address(part, *unregisters);
            Out.UnregisterDeviceBytes = unregisters->End - unregisters->VA;
            FirstController(part, kUnregisteredBase, Operand(part, *unregisters, kUnregisteredBase));
            FirstController(part, kUnregisteredBaseAll, Operand(part, *unregisters, kUnregisteredBaseAll));
        }
        if (auto opens = Entry(part, kOpenController)) {
            Out.OpenController = Address(part, *opens);
            Out.OpenControllerBytes = opens->End - opens->VA;
        }
    }

    // A device id formed as the player index plus `found`: the first controller's.
    void FirstController(Part part, char const* what, std::optional<uint64_t> found)
    {
        if (!found) return;
        bool same = *found == overlay::kFirstControllerDevice;
        Add(part, what, same, same ? Format("%llu, as Overlay/Controller.h has it", (unsigned long long)*found)
            : Format("%llu in the game, %u in Overlay/Controller.h", (unsigned long long)*found, overlay::kFirstControllerDevice));
    }

    void HidControllerAxes()
    {
        constexpr Part part = Part::HidControllerAxes;
        if (auto update = Entry(part, kHidUpdateControls)) {
            Out.HidUpdateControls = Address(part, *update);
            Out.HidUpdateControlsBytes = update->End - update->VA;
            // The places the MFi device has: the tracker reads both kinds of device by them.
            Same(part, "acquired, in both kinds of device", Operand(part, *update, kHidAcquired), Out.DeviceEnabled);
            Same(part, "the player index, in both kinds of device", Operand(part, *update, kHidPlayer), Out.DevicePlayer);
            if (auto base = Operand(part, *update, kHidDeviceBase)) FirstController(part, kHidDeviceBase, *base >> 16);
            auto axes = Operand(part, *update, kHidAxes);
            auto value = Operand(part, *update, kHidAxisValue);
            // What UpdateControls reads is what the value callback writes.
            Same(part, "the axes read and written", axes, Operand(part, *update, kHidCallbackAxes));
            Same(part, "an axis's value read and written", value, Operand(part, *update, kHidCallbackValue));
            Out.HidAxes = (uint32_t)axes.value_or(0);
            Out.HidAxisCount = (uint32_t)Operand(part, *update, kHidAxisCount).value_or(0);
            Out.HidAxisValue = (uint32_t)value.value_or(0);
        }
        if (auto halves = Entry(part, kControllerAxes)) {
            Out.ControllerAxes = Address(part, *halves);
            Out.ControllerAxisCount = halves->Of->Bytes / sizeof(uint16_t);
        }
        if (auto minimum = Entry(part, kTriggerCalibrationMin)) Out.TriggerCalibrationMin = Address(part, *minimum);
    }

    void HighDynamicRange()
    {
        constexpr Part part = Part::HighDynamicRange;
        if (auto settings = Entry(part, kGraphicSettings)) {
            Out.GraphicSettings = Address(part, *settings);
            Out.PaperWhite = (uint32_t)Operand(part, *settings, kPaperWhite).value_or(0);
            if (auto white = Operand(part, *settings, kUiWhite)) Out.UiWhite = std::bit_cast<float>((uint32_t)*white);
        }
    }
};

// The entries of other areas the overlay uses, copied so that the table
// resolved for the overlay holds them.
struct SharedEntries
{
    engine::Entry Entries[4]{};
    size_t Count{ 0 };

    SharedEntries()
    {
        for (char const* name : { kInputManager, kResourceManager, kPutTexture, kLoadingHost }) {
            for (auto const& area : engine::EngineTable()) {
                for (auto const& entry : area.Entries) {
                    if (!strcmp(entry.Name, name)) Entries[Count++] = entry;
                }
            }
        }
    }
};

struct Table
{
    SharedEntries Shared;
    engine::Area Areas[2]{};

    Table()
    {
        Areas[0] = { kArea, {} };
        for (auto const& area : engine::EngineTable()) {
            if (!strcmp(area.Name, kArea)) Areas[0] = area;
        }
        Areas[1] = { "used by the overlay", { Shared.Entries, Shared.Count } };
    }
};

}

DependencyCheck CheckReplacedMethod(ObjcClasses const& classes, Part part, char const* className, char const* selector, char const* types,
    uint64_t code)
{
    DependencyCheck check{ part, Format("-[%s %s]", className, selector), false, {} };
    if (!classes.HasClass(className)) {
        check.Detail = Format("there is no class %s", className);
        return check;
    }
    auto method = classes.InstanceMethod(className, selector);
    if (!method.Found) {
        check.Detail = "the class has no such method";
    } else if (!method.Own) {
        check.Detail = "the class inherits the method: it has none of its own to replace";
    } else if (method.Types != types) {
        check.Detail = Format("its type encoding is %s, expected %s", method.Types.c_str(), types);
    } else if (!code || method.Implementation != code) {
        check.Detail = Format("its implementation is at %#llx, and the code that was checked at %#llx",
            (unsigned long long)method.Implementation, (unsigned long long)code);
    } else {
        check.Passed = true;
        check.Detail = Format("the class's own, %s, implemented by the code that was checked", types);
    }
    return check;
}

char const* Describe(Part part)
{
    switch (part) {
    case Part::Frame: return "frames";
    case Part::Events: return "events";
    case Part::Injection: return "injected keys";
    case Part::Textures: return "textures";
    case Part::HighDynamicRange: return "colours in high dynamic range";
    case Part::Controllers: return "controllers";
    case Part::InjectedModifiers: return "modifiers of injected keys";
    case Part::Pointer: return "the pointer";
    case Part::HidControllerAxes: return "axes of IOHID controllers";
    default: return "?";
    }
}

bool Dependencies::Usable(Part part) const
{
    bool any = false;
    for (auto const& check : Checks) {
        if (check.Of != part) continue;
        if (!check.Passed) return false;
        any = true;
    }
    return any;
}

std::string Dependencies::Problem(Part part) const
{
    std::string problem;
    bool any = false;
    for (auto const& check : Checks) {
        if (check.Of != part) continue;
        any = true;
        if (check.Passed) continue;
        if (!problem.empty()) problem += "; ";
        problem += check.What + ": " + check.Detail;
    }
    if (!any) return "nothing was checked";
    return problem;
}

std::span<engine::Area const> DependencyTable()
{
    static const Table table;
    return table.Areas;
}

Dependencies CheckDependencies(engine::Resolution const& resolution, ObjcClasses const& classes)
{
    Dependencies out;
    out.Live = resolution.In.Game.Symbols && !resolution.In.Game.Symbols->FromFile();
    Checker checker{ resolution, classes, out };
    checker.Frame();
    checker.Events();
    checker.Injection();
    checker.InjectedModifiers();
    checker.Pointer();
    checker.Textures();
    checker.HighDynamicRange();
    checker.Controllers();
    checker.HidControllerAxes();
    return out;
}

}
