// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from ersh1's BG3 Native Camera Tweaks: https://github.com/ersh1/BG3_NativeCameraTweaks

/**
 * camera_patch.c - bl instruction encoding for the camera tweaks' call sites.
 */

#include "camera_patch.h"

bool camera_decode_bl(uint64_t site_va, uint32_t word, uint64_t *target) {
    if ((word & 0xfc000000u) != 0x94000000u) return false;
    int64_t imm = (int64_t)(word & 0x03ffffffu);
    if (imm & (1 << 25)) imm -= (1 << 26);
    if (target) *target = site_va + (uint64_t)(imm * 4);
    return true;
}

uint32_t camera_encode_bl(uint64_t site_va, uint64_t target) {
    int64_t delta = (int64_t)(target - site_va);
    if (delta & 3) return 0;
    if (delta < -(128LL << 20) || delta >= (128LL << 20)) return 0;
    return 0x94000000u | ((uint32_t)(delta >> 2) & 0x03ffffffu);
}
