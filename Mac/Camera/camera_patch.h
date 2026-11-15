// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from ersh1's BG3 Native Camera Tweaks: https://github.com/ersh1/BG3_NativeCameraTweaks

/**
 * camera_patch.h - bl instruction encoding for the camera tweaks' call sites.
 */

#ifndef CAMERA_PATCH_H
#define CAMERA_PATCH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Decode a BL; false when word is not a BL. */
bool camera_decode_bl(uint64_t site_va, uint32_t word, uint64_t *target);

/** Encode a BL from site_va to target; 0 when out of range or misaligned. */
uint32_t camera_encode_bl(uint64_t site_va, uint64_t target);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_PATCH_H */
