// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from ersh1's BG3 Native Camera Tweaks: https://github.com/ersh1/BG3_NativeCameraTweaks

/**
 * camera_gamepad.h - Physical stick-button state via the GameController
 * framework, for the camera tweaks' "zoom modifier held" test.
 *
 * BG3 on macOS reads controllers through GameController (MFIInputDevice), so
 * the same GCController objects are visible in-process. Reading the button
 * directly does not depend on how the game turns L3 into input events.
 */

#ifndef CAMERA_GAMEPAD_H
#define CAMERA_GAMEPAD_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum CameraStickButton {
    CAMERA_STICK_LEFT = 0,   /* L3 */
    CAMERA_STICK_RIGHT,      /* R3 */
} CameraStickButton;

/** True when any connected extended gamepad has the stick button pressed.
 *  *have_gamepad is set to whether one is connected at all. */
bool camera_gamepad_stick_pressed(CameraStickButton button, bool *have_gamepad);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_GAMEPAD_H */
