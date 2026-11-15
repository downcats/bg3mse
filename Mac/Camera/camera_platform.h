// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from ersh1's BG3 Native Camera Tweaks: https://github.com/ersh1/BG3_NativeCameraTweaks

/**
 * camera_platform.h - what the camera tweaks need from bg3mse's platform
 * layer: where the game's camera code and data are, logging, and guarded
 * memory reads (CameraSites.cpp).
 */

#ifndef CAMERA_PLATFORM_H
#define CAMERA_PLATFORM_H

#include <mach/mach.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A call site that is rewritten to call a replacement. `site` is an offset
 *  from the image base; `expected` is the bl found there. */
typedef struct CodePatchSite {
    uintptr_t site;
    uint32_t  expected;
    uint32_t  patched;
} CodePatchSite;

/**
 * Where the camera tweaks attach, as offsets from the image base. Filled at
 * start-up from the game's symbol table (camera_sites_resolve); nothing here
 * is a constant of one game build.
 */
typedef struct CameraTweaksOffsets {
    /* ecl::CameraSystem::Update -> UpdateGameCameraBehavior(this, world, GameTime const&, view) */
    uintptr_t     update_gcb_fn;
    CodePatchSite update_gcb_call;
    /* UpdateGameCameraBehavior -> GameCameraBehavior::GetCameraPitchDegrees(bool,bool) const,
     * the per-frame pitch-adjust site (followed by the +0x164 interpolation) */
    uintptr_t     pitch_fn;
    CodePatchSite pitch_call;
    /* ecl::CameraSystem::OnInputEvent(EntityRef const&, InputEvent const&) */
    uintptr_t     camera_on_input_fn;
    size_t        camera_on_input_bytes;   /* up to the next symbol */
    /* ecl::EocClient::OnInputEvent(InputEvent const&) - ToggleInputMode (L3) */
    uintptr_t     client_on_input_fn;
    size_t        client_on_input_bytes;
    /* ls::InputManager::GetInputValue (LTO: x1 = event id BY VALUE, w2 = player
     * index; returns {Value, Value2} in x0 and the state byte in w1) */
    uintptr_t     get_input_value_fn;
    uintptr_t     input_manager_ptr;     /* ls::InputManager::m_ptr */
    uintptr_t     controller_mode_flag;  /* anonymous bool, nonzero in controller mode */
    /* EoCGlobalSwitches camera definitions */
    uint32_t      gs_exploration_def;
    uint32_t      gs_combat_def;
    uint32_t      gs_override_def;       /* used while gs_override_flag != 0 */
    uint32_t      gs_override_flag;
    uint32_t      gs_controller_alt_flag;
    /* ecl::CameraHelpers::RestrictCamTargetDestHeight (LTO ABI: x0 = WorldView&,
     * w1 = bool, x2 = CameraSettings const&, x3 = Vector3f const& position,
     * x4 = LevelManager const*; returns the floor height bits in w0 and a
     * "no data" byte in w1), and the LevelManager* slot in ecl::CameraSystem */
    uintptr_t     floor_height_fn;
    uint32_t      camera_system_level_manager;
    /* ecl::GameControl::m_ptr and the float SetCameraDistanceOverride stores */
    uintptr_t     game_control_ptr;
    uint32_t      game_control_distance_override;
    /* -[LSMTLView mouseMoved:] calls [event deltaY] through this selector stub */
    uintptr_t     mouse_dy_fn;
    CodePatchSite mouse_dy_call;
} CameraTweaksOffsets;

/**
 * Finds every site in the running game and checks that the functions the
 * tweaks rely on are the code their offsets were read from. False, with a
 * reason in `why`, when anything is missing or changed: nothing is patched then.
 * On success *base is the image base and *global_switches_slot the address of
 * the EoCGlobalSwitches pointer.
 */
bool camera_sites_resolve(CameraTweaksOffsets *out, uintptr_t *base, uintptr_t *global_switches_slot,
                          char *why, size_t why_len);

/** bg3mse's data directory (~/Library/Application Support/bg3mse). */
const char *camera_data_dir(void);

/** Copies `size` bytes from `address`, or returns false when they are not readable. */
bool safe_memory_read(mach_vm_address_t address, void *out, size_t size);

void camera_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define LOG_GAME_INFO(...)  camera_log(1, __VA_ARGS__)
#define LOG_GAME_WARN(...)  camera_log(2, __VA_ARGS__)
#define LOG_GAME_ERROR(...) camera_log(3, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_PLATFORM_H */
