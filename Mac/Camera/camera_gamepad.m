// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from ersh1's BG3 Native Camera Tweaks: https://github.com/ersh1/BG3_NativeCameraTweaks

/**
 * camera_gamepad.m - GameController stick-button polling for camera tweaks.
 */

#import <GameController/GameController.h>

#include "camera_gamepad.h"

bool camera_gamepad_stick_pressed(CameraStickButton button, bool *have_gamepad) {
    bool pressed = false;
    bool any = false;
    @autoreleasepool {
        for (GCController *controller in [GCController controllers]) {
            GCExtendedGamepad *pad = controller.extendedGamepad;
            if (!pad) continue;
            any = true;
            GCControllerButtonInput *stick = (button == CAMERA_STICK_LEFT)
                ? pad.leftThumbstickButton : pad.rightThumbstickButton;
            if (stick && stick.isPressed) {
                pressed = true;
                break;
            }
        }
    }
    if (have_gamepad) *have_gamepad = any;
    return pressed;
}
