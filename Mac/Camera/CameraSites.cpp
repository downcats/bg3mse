// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from ersh1's BG3 Native Camera Tweaks: https://github.com/ersh1/BG3_NativeCameraTweaks

#include <Camera/CameraSites.h>
#include <Platform/CodeSearch.h>
#include <Platform/Config.h>
#include <Platform/EntryHook.h>
#include <Platform/Log.h>

#include <cstdarg>
#include <cstdio>
#include <mach/mach_vm.h>

namespace bg3se::mac {

namespace {

// Functions the tweaks read fields from, call with a hand-derived ABI, or hook.
struct KnownFunction
{
    char const* Name;
    char const* Symbol;
    bool Prefix;            // the full name carries a long template list: match its start
    CodeShape Shape;        // read from build 4.1.1.7631656 (arm64)
};

enum : size_t { kUpdate, kUpdateGcb, kPitch, kCameraInput, kClientInput, kGetInputValue, kFloorHeight, kMouseMoved, kFunctionCount };

constexpr KnownFunction kFunctions[kFunctionCount] = {
    // Only searched for its call to UpdateGameCameraBehavior: its shape is not checked.
    { "ecl::CameraSystem::Update", "__ZN3ecl12CameraSystem6UpdateE", true, { 0, 0 } },
    { "ecl::CameraSystem::UpdateGameCameraBehavior", "__ZN3ecl12CameraSystem24UpdateGameCameraBehaviorE", true, { 8881, 0xd51e5bd169150560ull } },
    { "ecl::GameCameraBehavior::GetCameraPitchDegrees", "__ZNK3ecl18GameCameraBehavior21GetCameraPitchDegreesEbb", false, { 110, 0x6fd7e452a06205e4ull } },
    { "ecl::CameraSystem::OnInputEvent", "__ZN3ecl12CameraSystem12OnInputEventERKN3ecs9EntityRefERKN2ls10InputEventE", false, { 1122, 0x5045cd5f68bf0bbfull } },
    { "ecl::EocClient::OnInputEvent", "__ZN3ecl9EocClient12OnInputEventERKN2ls10InputEventE", false, { 801, 0xb6d2f351f2c5ba45ull } },
    { "ls::InputManager::GetInputValue", "__ZNK2ls12InputManager13GetInputValueERKjNS_17EInputPlayerIndexE", false, { 73, 0x8a117bd1847391a7ull } },
    { "ecl::CameraHelpers::RestrictCamTargetDestHeight", "__ZN3ecl13CameraHelpers27RestrictCamTargetDestHeightE", true, { 549, 0x2e3ed47ed2fb5472ull } },
    { "-[LSMTLView mouseMoved:]", "-[LSMTLView mouseMoved:]", false, { 273, 0xc74088dbfb53fa2eull } },
};

constexpr char kMouseDeltaYStub[] = "_objc_msgSend$deltaY";
constexpr char kInputManagerPtr[] = "__ZN2ls12InputManager5m_ptrE";
constexpr char kGameControlPtr[] = "__ZN3ecl11GameControl5m_ptrE";

// Under the shapes above:
// UpdateGameCameraBehavior calls GetCameraPitchDegrees four times; the per-frame
// pitch adjustment (followed by the interpolation of behavior+0x164) is this one.
constexpr uint64_t kPitchCallOffset = 0x5504;
// GetCameraPitchDegrees: adrp/ldr of the EoCGlobalSwitches pointer, and adrp/ldrb of the controller-mode flag.
constexpr uint64_t kPitchGlobalSwitchesLoad = 28;
constexpr uint64_t kPitchControllerFlagLoad = 124;

}

CameraSitesCheck CheckCameraSites(GameImage const& image)
{
    CameraSitesCheck check;
    uint64_t base = image.BaseVA();
    uint64_t va[kFunctionCount] = {}, end[kFunctionCount] = {};

    for (size_t i = 0; i < kFunctionCount; i++) {
        auto const& fn = kFunctions[i];
        CameraFunctionCheck result{ fn.Name, fn.Shape, {}, false };
        va[i] = fn.Prefix ? image.FindVAByPrefix(fn.Symbol) : image.FindVA(fn.Symbol);
        end[i] = va[i] ? image.NextSymbolVA(va[i]) : 0;
        if (va[i] && end[i] > va[i]) {
            result.Present = true;
            result.Found = ShapeOfRange(image.CodeAt(va[i]), (uint32_t)((end[i] - va[i]) / 4));
        }
        check.Functions.push_back(result);
    }
    for (auto const& fn : check.Functions) {
        if (!fn.Present) {
            check.Problem = std::string(fn.Name) + " is not in the game's symbol table";
            return check;
        }
    }
    // The caller is only searched for the call; its own code may change freely.
    for (size_t i = kUpdateGcb; i < kFunctionCount; i++) {
        if (check.Functions[i].Found != check.Functions[i].Expected) {
            check.Problem = std::string(check.Functions[i].Name) + " changed, so the camera offsets have to be checked again";
            return check;
        }
    }

    auto& row = check.Row;
    row.update_gcb_fn = va[kUpdateGcb] - base;
    row.pitch_fn = va[kPitch] - base;
    row.camera_on_input_fn = va[kCameraInput] - base;
    row.camera_on_input_bytes = end[kCameraInput] - va[kCameraInput];
    row.client_on_input_fn = va[kClientInput] - base;
    row.client_on_input_bytes = end[kClientInput] - va[kClientInput];
    row.get_input_value_fn = va[kGetInputValue] - base;
    row.floor_height_fn = va[kFloorHeight] - base;

    auto updateCalls = CallsTo(image, va[kUpdate], end[kUpdate], va[kUpdateGcb]);
    if (updateCalls.size() != 1) {
        check.Problem = "CameraSystem::Update does not call UpdateGameCameraBehavior exactly once";
        return check;
    }
    row.update_gcb_call = { (uintptr_t)(updateCalls[0] - base), image.Word(updateCalls[0]), 0 };

    uint64_t pitchCall = va[kUpdateGcb] + kPitchCallOffset;
    if (CallTarget(pitchCall, image.Word(pitchCall)) != va[kPitch]) {
        check.Problem = "the pitch-adjust call is not where it was read";
        return check;
    }
    row.pitch_call = { (uintptr_t)(pitchCall - base), image.Word(pitchCall), 0 };

    // The two entry hooks, as the planner sees them.
    for (size_t i : { kCameraInput, kClientInput }) {
        if (auto refusal = CheckEntryHook(image, va[i], end[i]); refusal != HookRefusal::None) {
            check.Problem = std::string(kFunctions[i].Name) + " cannot be hooked at its entry: " + Describe(refusal);
            return check;
        }
    }

    uint64_t slot = PageAddress(va[kPitch] + kPitchGlobalSwitchesLoad, image.Word(va[kPitch] + kPitchGlobalSwitchesLoad),
        image.Word(va[kPitch] + kPitchGlobalSwitchesLoad + 4));
    uint64_t controllerFlag = PageAddress(va[kPitch] + kPitchControllerFlagLoad, image.Word(va[kPitch] + kPitchControllerFlagLoad),
        image.Word(va[kPitch] + kPitchControllerFlagLoad + 4));
    uint64_t inputManager = image.FindVA(kInputManagerPtr);
    uint64_t gameControl = image.FindVA(kGameControlPtr);
    if (!slot || !controllerFlag || !inputManager || !gameControl) {
        check.Problem = "a camera global could not be located";
        return check;
    }
    check.GlobalSwitchesSlotVA = slot;
    row.controller_mode_flag = controllerFlag - base;
    row.input_manager_ptr = inputManager - base;
    row.game_control_ptr = gameControl - base;

    // Field offsets, as the functions checked above use them.
    row.gs_exploration_def = 0x7c4;
    row.gs_combat_def = 0x958;
    row.gs_override_def = 0xc80;
    row.gs_override_flag = 0x1362;
    row.gs_controller_alt_flag = 0x6a7;
    row.camera_system_level_manager = 0x110;
    row.game_control_distance_override = 0x20;

    // Mouse pitch is optional: without its site the rest still installs.
    uint64_t stub = image.FindVA(kMouseDeltaYStub);
    auto mouseCalls = stub ? CallsTo(image, va[kMouseMoved], end[kMouseMoved], stub) : std::vector<uint64_t>{};
    if (mouseCalls.size() == 1) {
        row.mouse_dy_fn = stub - base;
        row.mouse_dy_call = { (uintptr_t)(mouseCalls[0] - base), image.Word(mouseCalls[0]), 0 };
    }
    return check;
}

}

using namespace bg3se::mac;

extern "C" bool camera_sites_resolve(CameraTweaksOffsets* out, uintptr_t* base, uintptr_t* globalSwitchesSlot, char* why, size_t whyLen)
{
    auto image = GameImage::Current();
    if (!image) {
        snprintf(why, whyLen, "the game's symbol table cannot be read");
        return false;
    }
    auto check = CheckCameraSites(*image);
    if (!check.Ok()) {
        snprintf(why, whyLen, "%s", check.Problem.c_str());
        return false;
    }
    *out = check.Row;
    *base = (uintptr_t)image->LoadedAt(image->BaseVA());
    *globalSwitchesSlot = (uintptr_t)image->LoadedAt(check.GlobalSwitchesSlotVA);
    return true;
}

extern "C" const char* camera_data_dir(void)
{
    static std::string dir = DataDirectory();
    return dir.c_str();
}

extern "C" bool safe_memory_read(mach_vm_address_t address, void* out, size_t size)
{
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), address, size, (mach_vm_address_t)(uintptr_t)out, &got) == KERN_SUCCESS && got == size;
}

extern "C" void camera_log(int level, const char* fmt, ...)
{
    char line[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    Log(level >= 3 ? LogLevel::Error : level == 2 ? LogLevel::Warning : LogLevel::Info, "%s", line);
}
