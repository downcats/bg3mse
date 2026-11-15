// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from ersh1's BG3 Native Camera Tweaks: https://github.com/ersh1/BG3_NativeCameraTweaks

/**
 * camera_tweaks.h - Native Camera Tweaks, bg3mse's camera module.
 *
 * macOS port of ersh1's BG3 Native Camera Tweaks: unlocked camera pitch
 * (right stick / L3 + right stick zoom on a controller), zoom range, FOV and
 * offset overrides, rotation and zoom speed multipliers, right-stick deadzone.
 * Settings: <data dir>/NativeCameraTweaks.toml (camera_settings.h).
 * Where the tweaks attach and how that is checked: CameraSites.h.
 *
 * Opt-in (Enabled = true in the settings file), arm64 only, and fail-closed:
 * nothing is patched unless every function the tweaks rely on is in the
 * game's symbol table with the code shape its offsets were read from, and
 * every call site is found where that code puts it (camera_sites_resolve).
 */

#ifndef CAMERA_TWEAKS_H
#define CAMERA_TWEAKS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Load settings and, when enabled and verified, install the camera hooks.
 *  Call once at load, before the client thread runs. */
bool camera_tweaks_init(void);

/** True when the hooks are installed. */
bool camera_tweaks_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_TWEAKS_H */
