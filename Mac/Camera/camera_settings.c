// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from ersh1's BG3 Native Camera Tweaks: https://github.com/ersh1/BG3_NativeCameraTweaks

/**
 * camera_settings.c - Native Camera Tweaks settings parser and camera math.
 *
 * Ported from ersh1's BG3 Native Camera Tweaks (Settings.cpp,
 * CameraTweaksManager.cpp). Deviations from upstream, all deliberate:
 *   - Enabled (new): master switch; the hooks are only installed when true.
 *     The default file leaves it out (off), so that a file holds it only
 *     when it was chosen: bg3mse-install asks, and writes the answer.
 *   - *UnlockedPitchMin defaults to 10 instead of -85: near and below the
 *     horizon a far camera passes through walls the floor guard cannot see.
 *   - Zoom defaults are tighter (ZoomMin 2, TacticalZoomMax 30, AltZoomMax 25
 *     instead of 0.5/50/40), UnlockedPitchFloorOffset is 0.5 instead of 0.1,
 *     and LimitZoomAtLowPitch (new) shrinks the zoom limit near the horizon.
 *   - The floor guard (UnlockedPitchLimitClipping) restores the zoom the
 *     player asked for once the ground drops away; upstream kept it pulled in.
 *   - PitchTransitionSpeed, ZoomSmoothing and DeadzoneHysteresis (new) ease
 *     the pitch hand-over, the stick zoom rate and the deadzone edge.
 *   - InterpTo compares |distance|; upstream's signed compare snapped
 *     instantly whenever the target was below the current pitch.
 *   - A pitch step that would pass a limit clamps to it (upstream kept the
 *     previous pitch, stalling a fast stick short of the limit).
 *   - ZoomModifierSource / ApplyDeadzoneToZoom (new, [Controller]): see
 *     camera_settings.h.
 *   - Pitch speed: upstream multiplied stick input by the camera's rotation
 *     speed (CameraObject +0xc4); on macOS that offset is part of a position
 *     vector, so ControllerPitchSpeed (new) replaces it. StickResponseCurve
 *     and PitchSmoothing (new) shape and ease stick tilt/rotation, and
 *     ControllerCameraRotationMult defaults to 1.25 instead of 2.
 *   - Combat: the zoom overrides also cover the multi-target range the game
 *     uses while it frames two or more characters, and *KeepZoom (new)
 *     undoes the game's own zoom changes (see camera_settings.h).
 *   - Mouse pitch sums every mouse Y movement since the last frame (upstream
 *     kept only the last SDL event's), in points: macOS mouse deltas.
 */

#include "camera_settings.h"

#include <ctype.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xlocale.h>

static void context_defaults(CameraContextSettings *c, bool combat) {
    c->unlock_pitch = true;
    c->keep_tactical_pitch_locked = false;
    c->unlocked_pitch_min = 10.0f;
    c->unlocked_pitch_max = 85.0f;

    c->override_locked_pitch = false;
    c->locked_pitch_close = combat ? 32.73f : 19.05f;
    c->locked_pitch_far = combat ? 52.42f : 40.71f;
    c->locked_tactical_pitch_close = 85.55f;
    c->locked_tactical_pitch_far = 85.55f;
    c->locked_alt_pitch_close = 32.69f;
    c->locked_alt_pitch_far = 39.7f;

    c->override_zoom = true;
    c->zoom_min = 2.0f;
    c->zoom_max = 20.0f;
    c->tactical_zoom_min = 10.0f;
    c->tactical_zoom_max = 30.0f;
    c->alt_zoom_min = 10.0f;
    c->alt_zoom_max = 25.0f;
    c->keep_zoom = false;

    c->override_fov = false;
    c->fov_close = 55.0f;
    c->fov_far = 55.0f;
    c->tactical_fov = 25.0f;
    c->alt_fov_close = 45.0f;
    c->alt_fov_far = 45.0f;

    c->override_offset = false;
    c->horizontal_offset_mult = 0.0f;
    c->vertical_offset_mult = 0.8f;
}

void camera_settings_defaults(CameraSettings *s) {
    memset(s, 0, sizeof(*s));
    s->enabled = false;
    s->unlocked_pitch_initial_value = 25.0f;
    s->unlocked_pitch_clamp_speed = 5.0f;
    s->unlocked_pitch_limit_clipping = true;
    s->unlocked_pitch_floor_offset = 0.5f;
    s->reset_zoom_on_zone_change = false;
    s->watch_for_config_changes = true;
    s->pitch_transition_speed = 4.0f;
    s->limit_zoom_at_low_pitch = true;
    s->low_pitch_angle = 30.0f;
    s->low_pitch_max_zoom = 8.0f;
    s->log_camera_events = false;

    context_defaults(&s->exploration, false);
    context_defaults(&s->combat, true);

    s->mouse_camera_rotation_mult = 1.0f;
    s->mouse_pitch_mult = 0.25f;
    s->mouse_zoom_mult = 0.5f;
    s->invert_mouse_pitch = false;

    s->keyboard_camera_rotation_mult = 2.0f;

    s->controller_camera_rotation_mult = 1.25f;
    s->controller_pitch_mult = 0.5f;
    s->controller_pitch_speed = 90.0f;
    s->stick_response_curve = 1.6f;
    s->pitch_smoothing = 0.12f;
    s->controller_zoom_mult = 0.5f;
    s->zoom_smoothing = 0.15f;
    s->invert_controller_pitch = false;
    s->swap_zoom_and_pitch = false;
    s->use_right_stick_press_for_zoom = false;
    s->zoom_modifier_source = CAMERA_MODIFIER_BOTH;
    s->apply_deadzone_to_zoom = true;

    s->override_right_stick_deadzone = true;
    s->new_deadzone = 0.15f;
    s->deadzone_hysteresis = 0.05f;
}

/* ---- parser -------------------------------------------------------------- */

typedef enum { KEY_BOOL, KEY_FLOAT, KEY_SOURCE } KeyKind;

typedef struct KeyDef {
    const char *section;     /* NULL = context section, resolved per prefix */
    const char *name;
    KeyKind kind;
    size_t offset;           /* into CameraSettings, or CameraContextSettings */
    float lo, hi;            /* accepted range for floats */
} KeyDef;

#define G(field) offsetof(CameraSettings, field)
#define C(field) offsetof(CameraContextSettings, field)

static const KeyDef g_global_keys[] = {
    {"General", "Enabled", KEY_BOOL, G(enabled), 0, 0},
    {"General", "UnlockedPitchInitialValue", KEY_FLOAT, G(unlocked_pitch_initial_value), -89, 89},
    {"General", "UnlockedPitchClampSpeed", KEY_FLOAT, G(unlocked_pitch_clamp_speed), 0, 1000},
    {"General", "UnlockedPitchLimitClipping", KEY_BOOL, G(unlocked_pitch_limit_clipping), 0, 0},
    {"General", "UnlockedPitchFloorOffset", KEY_FLOAT, G(unlocked_pitch_floor_offset), -100, 100},
    {"General", "ResetZoomOnZoneChange", KEY_BOOL, G(reset_zoom_on_zone_change), 0, 0},
    {"General", "WatchForConfigChanges", KEY_BOOL, G(watch_for_config_changes), 0, 0},
    {"General", "PitchTransitionSpeed", KEY_FLOAT, G(pitch_transition_speed), 0, 1000},
    {"General", "LimitZoomAtLowPitch", KEY_BOOL, G(limit_zoom_at_low_pitch), 0, 0},
    {"General", "LowPitchAngle", KEY_FLOAT, G(low_pitch_angle), 0, 89},
    {"General", "LowPitchMaxZoom", KEY_FLOAT, G(low_pitch_max_zoom), 0.01f, 500},
    {"General", "LogCameraEvents", KEY_BOOL, G(log_camera_events), 0, 0},
    {"Mouse", "MouseCameraRotationMult", KEY_FLOAT, G(mouse_camera_rotation_mult), 0, 100},
    {"Mouse", "MousePitchMult", KEY_FLOAT, G(mouse_pitch_mult), 0, 100},
    {"Mouse", "MouseZoomMult", KEY_FLOAT, G(mouse_zoom_mult), 0, 100},
    {"Mouse", "InvertMousePitch", KEY_BOOL, G(invert_mouse_pitch), 0, 0},
    {"Keyboard", "KeyboardCameraRotationMult", KEY_FLOAT, G(keyboard_camera_rotation_mult), 0, 100},
    {"Controller", "ControllerCameraRotationMult", KEY_FLOAT, G(controller_camera_rotation_mult), 0, 100},
    {"Controller", "ControllerPitchMult", KEY_FLOAT, G(controller_pitch_mult), 0, 100},
    {"Controller", "ControllerPitchSpeed", KEY_FLOAT, G(controller_pitch_speed), 0, 1000},
    {"Controller", "StickResponseCurve", KEY_FLOAT, G(stick_response_curve), 1, 4},
    {"Controller", "PitchSmoothing", KEY_FLOAT, G(pitch_smoothing), 0, 2},
    {"Controller", "ControllerZoomMult", KEY_FLOAT, G(controller_zoom_mult), 0, 100},
    {"Controller", "ZoomSmoothing", KEY_FLOAT, G(zoom_smoothing), 0, 2},
    {"Controller", "InvertControllerPitch", KEY_BOOL, G(invert_controller_pitch), 0, 0},
    {"Controller", "SwapZoomAndPitch", KEY_BOOL, G(swap_zoom_and_pitch), 0, 0},
    {"Controller", "UseRightStickPressForZoom", KEY_BOOL, G(use_right_stick_press_for_zoom), 0, 0},
    {"Controller", "ZoomModifierSource", KEY_SOURCE, G(zoom_modifier_source), 0, 0},
    {"Controller", "ApplyDeadzoneToZoom", KEY_BOOL, G(apply_deadzone_to_zoom), 0, 0},
    {"ControllerDeadzone", "OverrideRightStickDeadzone", KEY_BOOL, G(override_right_stick_deadzone), 0, 0},
    {"ControllerDeadzone", "NewDeadzone", KEY_FLOAT, G(new_deadzone), 0, 0.95f},
    {"ControllerDeadzone", "DeadzoneHysteresis", KEY_FLOAT, G(deadzone_hysteresis), 0, 0.5f},
};

/* Context keys appear as <Prefix><Name> in section <Prefix><Suffix>,
 * e.g. ExplorationZoomMin in [ExplorationZoom]. */
static const KeyDef g_context_keys[] = {
    {"Pitch", "UnlockPitch", KEY_BOOL, C(unlock_pitch), 0, 0},
    {"Pitch", "KeepTacticalPitchLocked", KEY_BOOL, C(keep_tactical_pitch_locked), 0, 0},
    {"Pitch", "UnlockedPitchMin", KEY_FLOAT, C(unlocked_pitch_min), -89, 89},
    {"Pitch", "UnlockedPitchMax", KEY_FLOAT, C(unlocked_pitch_max), -89, 89},
    {"Pitch", "OverrideLockedPitch", KEY_BOOL, C(override_locked_pitch), 0, 0},
    {"Pitch", "LockedPitchClose", KEY_FLOAT, C(locked_pitch_close), -89, 89},
    {"Pitch", "LockedPitchFar", KEY_FLOAT, C(locked_pitch_far), -89, 89},
    {"Pitch", "LockedTacticalPitchClose", KEY_FLOAT, C(locked_tactical_pitch_close), -89, 89},
    {"Pitch", "LockedTacticalPitchFar", KEY_FLOAT, C(locked_tactical_pitch_far), -89, 89},
    {"Pitch", "LockedAltPitchClose", KEY_FLOAT, C(locked_alt_pitch_close), -89, 89},
    {"Pitch", "LockedAltPitchFar", KEY_FLOAT, C(locked_alt_pitch_far), -89, 89},
    {"Zoom", "OverrideZoom", KEY_BOOL, C(override_zoom), 0, 0},
    {"Zoom", "ZoomMin", KEY_FLOAT, C(zoom_min), 0.01f, 500},
    {"Zoom", "ZoomMax", KEY_FLOAT, C(zoom_max), 0.01f, 500},
    {"Zoom", "TacticalZoomMin", KEY_FLOAT, C(tactical_zoom_min), 0.01f, 500},
    {"Zoom", "TacticalZoomMax", KEY_FLOAT, C(tactical_zoom_max), 0.01f, 500},
    {"Zoom", "AltZoomMin", KEY_FLOAT, C(alt_zoom_min), 0.01f, 500},
    {"Zoom", "AltZoomMax", KEY_FLOAT, C(alt_zoom_max), 0.01f, 500},
    {"Zoom", "KeepZoom", KEY_BOOL, C(keep_zoom), 0, 0},
    {"FOV", "OverrideFOV", KEY_BOOL, C(override_fov), 0, 0},
    {"FOV", "FOVClose", KEY_FLOAT, C(fov_close), 1, 170},
    {"FOV", "FOVFar", KEY_FLOAT, C(fov_far), 1, 170},
    {"FOV", "TacticalFOV", KEY_FLOAT, C(tactical_fov), 1, 170},
    {"FOV", "AltFOVClose", KEY_FLOAT, C(alt_fov_close), 1, 170},
    {"FOV", "AltFOVFar", KEY_FLOAT, C(alt_fov_far), 1, 170},
    {"Offset", "OverrideOffset", KEY_BOOL, C(override_offset), 0, 0},
    {"Offset", "HorizontalOffsetMult", KEY_FLOAT, C(horizontal_offset_mult), -100, 100},
    {"Offset", "VerticalOffsetMult", KEY_FLOAT, C(vertical_offset_mult), -100, 100},
};

#undef G
#undef C

static char *trim(char *p) {
    while (isspace((unsigned char)*p)) p++;
    char *e = p + strlen(p);
    while (e > p && isspace((unsigned char)e[-1])) *--e = '\0';
    return p;
}

/* Strip a trailing '#' comment that is not inside a quoted string. */
static void strip_comment(char *p) {
    bool quoted = false;
    for (; *p; p++) {
        if (*p == '"') quoted = !quoted;
        else if (*p == '#' && !quoted) { *p = '\0'; return; }
    }
}

static bool parse_bool(const char *v, bool *out) {
    if (strcmp(v, "true") == 0) { *out = true; return true; }
    if (strcmp(v, "false") == 0) { *out = false; return true; }
    return false;
}

static bool parse_float(const char *v, float lo, float hi, float *out) {
    char *end = NULL;
    /* In the "C" locale: the game sets the system's (a German Mac's reads "1,5"). */
    double d = strtod_l(v, &end, LC_C_LOCALE);
    if (end == v || *end != '\0' || !isfinite(d)) return false;
    if (d < lo || d > hi) return false;
    *out = (float)d;
    return true;
}

static bool parse_source(const char *v, CameraModifierSource *out) {
    char buf[32];
    size_t n = strlen(v);
    if (n >= 2 && v[0] == '"' && v[n - 1] == '"') {
        if (n - 2 >= sizeof(buf)) return false;
        memcpy(buf, v + 1, n - 2);
        buf[n - 2] = '\0';
        v = buf;
    }
    if (strcmp(v, "both") == 0) { *out = CAMERA_MODIFIER_BOTH; return true; }
    if (strcmp(v, "game") == 0) { *out = CAMERA_MODIFIER_GAME; return true; }
    if (strcmp(v, "gamepad") == 0) { *out = CAMERA_MODIFIER_GAMEPAD; return true; }
    return false;
}

static bool apply_key(const KeyDef *k, void *base, const char *value) {
    char *field = (char *)base + k->offset;
    switch (k->kind) {
    case KEY_BOOL:   return parse_bool(value, (bool *)field);
    case KEY_FLOAT:  return parse_float(value, k->lo, k->hi, (float *)field);
    case KEY_SOURCE: return parse_source(value, (CameraModifierSource *)field);
    }
    return false;
}

static bool set_key(CameraSettings *s, const char *section, const char *key,
                    const char *value) {
    for (size_t i = 0; i < sizeof(g_global_keys) / sizeof(g_global_keys[0]); i++) {
        const KeyDef *k = &g_global_keys[i];
        if (strcmp(k->section, section) == 0 && strcmp(k->name, key) == 0)
            return apply_key(k, s, value);
    }

    static const struct { const char *prefix; size_t offset; } contexts[] = {
        {"Exploration", offsetof(CameraSettings, exploration)},
        {"Combat", offsetof(CameraSettings, combat)},
    };
    for (size_t c = 0; c < 2; c++) {
        size_t plen = strlen(contexts[c].prefix);
        if (strncmp(section, contexts[c].prefix, plen) != 0) continue;
        if (strncmp(key, contexts[c].prefix, plen) != 0) continue;
        const char *sec_suffix = section + plen;
        const char *key_suffix = key + plen;
        for (size_t i = 0; i < sizeof(g_context_keys) / sizeof(g_context_keys[0]); i++) {
            const KeyDef *k = &g_context_keys[i];
            if (strcmp(k->section, sec_suffix) == 0 && strcmp(k->name, key_suffix) == 0)
                return apply_key(k, (char *)s + contexts[c].offset, value);
        }
    }
    return false;
}

int camera_settings_parse(CameraSettings *s, const char *text, int *first_error) {
    int rejected = 0;
    if (first_error) *first_error = 0;
    if (!s || !text) return 0;

    char section[64] = "";
    char line[512];
    int lineno = 0;
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        lineno++;
        bool too_long = len >= sizeof(line);
        if (!too_long) {
            memcpy(line, p, len);
            line[len] = '\0';
        }
        p += len + (nl ? 1 : 0);

        bool ok = !too_long;
        if (ok) {
            strip_comment(line);
            char *t = trim(line);
            if (*t == '\0') continue;
            if (*t == '[') {
                char *close = strchr(t, ']');
                ok = close && close[1] == '\0' && (size_t)(close - t - 1) < sizeof(section);
                if (ok) {
                    *close = '\0';
                    snprintf(section, sizeof(section), "%s", trim(t + 1));
                }
            } else {
                char *eq = strchr(t, '=');
                ok = eq != NULL;
                if (ok) {
                    *eq = '\0';
                    ok = set_key(s, section, trim(t), trim(eq + 1));
                }
            }
        }
        if (!ok) {
            if (!rejected && first_error) *first_error = lineno;
            rejected++;
        }
    }

    /* A min above its max would invert the clamp; restore the pair. */
    CameraSettings def;
    camera_settings_defaults(&def);
    CameraContextSettings *ctx[2] = {&s->exploration, &s->combat};
    const CameraContextSettings *dctx[2] = {&def.exploration, &def.combat};
    for (int i = 0; i < 2; i++) {
        if (ctx[i]->unlocked_pitch_min > ctx[i]->unlocked_pitch_max) {
            ctx[i]->unlocked_pitch_min = dctx[i]->unlocked_pitch_min;
            ctx[i]->unlocked_pitch_max = dctx[i]->unlocked_pitch_max;
            rejected++;
        }
        if (ctx[i]->zoom_min > ctx[i]->zoom_max) {
            ctx[i]->zoom_min = dctx[i]->zoom_min;
            ctx[i]->zoom_max = dctx[i]->zoom_max;
            rejected++;
        }
        if (ctx[i]->tactical_zoom_min > ctx[i]->tactical_zoom_max) {
            ctx[i]->tactical_zoom_min = dctx[i]->tactical_zoom_min;
            ctx[i]->tactical_zoom_max = dctx[i]->tactical_zoom_max;
            rejected++;
        }
        if (ctx[i]->alt_zoom_min > ctx[i]->alt_zoom_max) {
            ctx[i]->alt_zoom_min = dctx[i]->alt_zoom_min;
            ctx[i]->alt_zoom_max = dctx[i]->alt_zoom_max;
            rejected++;
        }
    }
    return rejected;
}

/* ---- default file -------------------------------------------------------- */

#define CONTEXT_SECTIONS(P, PITCH_CLOSE, PITCH_FAR, ZMIN_NOTE, ZMAX_NOTE) \
    "[" P "Pitch]\n" \
    P "UnlockPitch = true\n" \
    P "KeepTacticalPitchLocked = false\n" \
    "# Degrees. Upstream default for the minimum is -85; it is 10 here because\n" \
    "# a far camera near the horizon ends up behind walls and outside the level.\n" \
    P "UnlockedPitchMin = 10.0\n" \
    P "UnlockedPitchMax = 85.0\n" \
    P "OverrideLockedPitch = false\n" \
    P "LockedPitchClose = " PITCH_CLOSE "\n" \
    P "LockedPitchFar = " PITCH_FAR "\n" \
    P "LockedTacticalPitchClose = 85.55\n" \
    P "LockedTacticalPitchFar = 85.55\n" \
    P "LockedAltPitchClose = 32.69\n" \
    P "LockedAltPitchFar = 39.7\n" \
    "\n" \
    "[" P "Zoom]\n" \
    P "OverrideZoom = true\n" \
    "# Zoom is the camera's distance from the character, in metres.\n" \
    "# ZoomMin/ZoomMax: mouse and keyboard camera.\n" \
    P "ZoomMin = 2.0   # game: " ZMIN_NOTE "\n" \
    P "ZoomMax = 20.0  # game: " ZMAX_NOTE "\n" \
    P "TacticalZoomMin = 10.0\n" \
    P "TacticalZoomMax = 30.0\n" \
    "# Alt = the controller camera (these are the limits on a controller).\n" \
    P "AltZoomMin = 10.0  # game: 10\n" \
    P "AltZoomMax = 25.0  # game: 40\n" \
    "# macOS: keep the zoom you chose when the game moves the camera itself\n" \
    "# (a new turn, an attack or spell shot). While it frames two or more\n" \
    "# characters it may still zoom out to show them all, never in past yours.\n" \
    "# Center Camera still resets it.\n" \
    P "KeepZoom = false\n" \
    "\n" \
    "[" P "FOV]\n" \
    P "OverrideFOV = false\n" \
    P "FOVClose = 55.0\n" \
    P "FOVFar = 55.0\n" \
    P "TacticalFOV = 25.0\n" \
    P "AltFOVClose = 45.0\n" \
    P "AltFOVFar = 45.0\n" \
    "\n" \
    "[" P "Offset]\n" \
    P "OverrideOffset = false\n" \
    P "HorizontalOffsetMult = 0.0\n" \
    P "VerticalOffsetMult = 0.8\n" \
    "\n"

static const char g_default_file[] =
    "# bg3mse Native Camera Tweaks — a macOS port of ersh1's BG3 Native Camera\n"
    "# Tweaks (https://github.com/ersh1/BG3_NativeCameraTweaks), built as a\n"
    "# separate module of bg3mse. Keys match upstream's BG3NativeCameraTweaks.toml.\n"
    "#\n"
    "# Enabled is read when the game starts; every other value reloads while\n"
    "# the game runs when WatchForConfigChanges is true. A malformed or\n"
    "# out-of-range value is logged and ignored (the previous value stays).\n"
    "\n"
    "[General]\n"
    "# Master switch (macOS addition): the camera hooks are installed only when\n"
    "# \"Enabled = true\" is in this section when the game starts. bg3mse-install\n"
    "# asks whether to turn them on, and \"bg3mse-install options\" changes it.\n"
    "UnlockedPitchInitialValue = 25.0\n"
    "UnlockedPitchClampSpeed = 5.0\n"
    "# Floor guard: with the pitch unlocked, pull the camera in so it stays\n"
    "# UnlockedPitchFloorOffset metres above the ground (it eases back out when\n"
    "# the ground drops away).\n"
    "UnlockedPitchLimitClipping = true\n"
    "UnlockedPitchFloorOffset = 0.5\n"
    "# false: the zoom you had carries over when the game makes a new camera\n"
    "# (a zone change or loading a save in game). true: the game's default.\n"
    "ResetZoomOnZoneChange = false\n"
    "WatchForConfigChanges = true\n"
    "# macOS: how fast the camera eases to your tilt when it takes over from\n"
    "# the game's (after loading and after cutscenes). 0 = snap.\n"
    "PitchTransitionSpeed = 4.0\n"
    "# macOS: with the pitch unlocked, the zoom limit shrinks below\n"
    "# LowPitchAngle degrees, down to LowPitchMaxZoom at the horizon, so a\n"
    "# low camera cannot swing out through walls.\n"
    "LimitZoomAtLowPitch = true\n"
    "LowPitchAngle = 30.0\n"
    "LowPitchMaxZoom = 8.0\n"
    "# macOS, for bug reports: log the game's own camera moves (mode and\n"
    "# target changes, zoom it sets, pinned pitch) to the bg3mse log.\n"
    "LogCameraEvents = false\n"
    "\n"
    CONTEXT_SECTIONS("Exploration", "19.05", "40.71", "3.5", "12")
    CONTEXT_SECTIONS("Combat", "32.73", "52.42", "4", "15")
    "[Mouse]\n"
    "MouseCameraRotationMult = 1.0\n"
    "# Hold the middle mouse button (the game's mouse rotation) and move the\n"
    "# mouse up/down to tilt the camera: degrees per point of movement, times\n"
    "# MouseCameraRotationMult.\n"
    "MousePitchMult = 0.25\n"
    "MouseZoomMult = 0.5\n"
    "InvertMousePitch = false\n"
    "\n"
    "[Keyboard]\n"
    "KeyboardCameraRotationMult = 2.0\n"
    "\n"
    "[Controller]\n"
    "# Right stick up/down pitches the camera; hold L3 (ToggleInputMode) to\n"
    "# zoom instead. Releasing L3 after zooming does not toggle input mode.\n"
    "# Upstream default 2.0; 1.25 here for a calmer camera.\n"
    "ControllerCameraRotationMult = 1.25\n"
    "ControllerPitchMult = 0.5\n"
    "# macOS: tilt speed in degrees/s at full stick (before the two mults\n"
    "# above), the stick response curve (1 = linear, higher = finer near the\n"
    "# centre, used for tilt, zoom and rotation) and how long the tilt speed\n"
    "# takes to settle, in seconds (0 = instant).\n"
    "ControllerPitchSpeed = 90.0\n"
    "StickResponseCurve = 1.6\n"
    "PitchSmoothing = 0.12\n"
    "ControllerZoomMult = 0.5\n"
    "# macOS: seconds for the stick zoom speed to settle (0 = instant).\n"
    "ZoomSmoothing = 0.15\n"
    "InvertControllerPitch = false\n"
    "SwapZoomAndPitch = false\n"
    "# Hold R3 instead of L3 to zoom.\n"
    "UseRightStickPressForZoom = false\n"
    "# macOS: where \"L3 is held\" comes from: \"both\" (the game's input state or\n"
    "# the physical button via the GameController framework), \"game\" or\n"
    "# \"gamepad\".\n"
    "ZoomModifierSource = \"both\"\n"
    "# macOS: apply NewDeadzone to zoom as well, and treat any deflection past it\n"
    "# as zooming (the game otherwise ignores the stick below 0.65).\n"
    "ApplyDeadzoneToZoom = true\n"
    "\n"
    "[ControllerDeadzone]\n"
    "OverrideRightStickDeadzone = true\n"
    "NewDeadzone = 0.15\n"
    "# macOS: once the stick is past the deadzone it stays live until it falls\n"
    "# this far below it, so the edge cannot flicker.\n"
    "DeadzoneHysteresis = 0.05\n";

const char *camera_settings_default_file(void) {
    return g_default_file;
}

/* ---- camera math --------------------------------------------------------- */

CameraMode camera_mode_from_flags(uint32_t flags) {
    bool tactical = (flags & CAMERA_MODE_FLAG_TACTICAL) != 0;
    if (flags & CAMERA_MODE_FLAG_COMBAT)
        return tactical ? CAMERA_MODE_COMBAT_TACTICAL : CAMERA_MODE_COMBAT;
    return tactical ? CAMERA_MODE_EXPLORATION_TACTICAL : CAMERA_MODE_EXPLORATION;
}

bool camera_can_adjust_pitch(const CameraSettings *s, CameraMode mode) {
    switch (mode) {
    case CAMERA_MODE_EXPLORATION:
        return s->exploration.unlock_pitch;
    case CAMERA_MODE_COMBAT:
        return s->combat.unlock_pitch;
    case CAMERA_MODE_EXPLORATION_TACTICAL:
        return s->exploration.unlock_pitch && !s->exploration.keep_tactical_pitch_locked;
    case CAMERA_MODE_COMBAT_TACTICAL:
        return s->combat.unlock_pitch && !s->combat.keep_tactical_pitch_locked;
    }
    return false;
}

float camera_deadzone(const CameraSettings *s) {
    return s->override_right_stick_deadzone ? s->new_deadzone : CAMERA_VANILLA_DEADZONE;
}

float camera_stick_normalize(float v, float deadzone) {
    float a = fabsf(v);
    if (!(a > deadzone) || deadzone >= 1.0f) return 0.0f;
    return (a - deadzone) / (1.0f - deadzone);
}

float camera_stick_normalize_held(float v, float deadzone, float hysteresis, bool *engaged) {
    float a = fabsf(v);
    float release = deadzone - hysteresis;
    if (release < 0.0f) release = 0.0f;
    bool was = engaged && *engaged;
    bool live = was ? a > release : a > deadzone;
    if (engaged) *engaged = live;
    if (!live || deadzone >= 1.0f) return 0.0f;
    /* While held, measure from the release point so the output still falls
     * continuously to 0 there; engaging starts from the deadzone as usual. */
    float edge = was ? release : deadzone;
    float n = (a - edge) / (1.0f - edge);
    return n > 0.0f ? (n < 1.0f ? n : 1.0f) : 0.0f;
}

static float denormalize(float lo, float hi, float v) {
    return v * (hi - lo) + lo;
}

float camera_adjust_rotation_input(const CameraSettings *s, float v, bool apply_mult) {
    float dz = camera_deadzone(s);
    if (!(v > dz)) return 0.0f;
    float n = powf((v - dz) * (1.0f / (1.0f - dz)), s->stick_response_curve);
    if (apply_mult) n *= s->controller_camera_rotation_mult;
    return denormalize(CAMERA_VANILLA_DEADZONE, 1.0f, n);
}

float camera_adjust_zoom_input(const CameraSettings *s, float v, bool *engaged) {
    float n;
    if (s->apply_deadzone_to_zoom) {
        n = camera_stick_normalize_held(v, camera_deadzone(s), s->deadzone_hysteresis, engaged);
        n = powf(n, s->stick_response_curve);
    } else {
        n = camera_stick_normalize(v, CAMERA_VANILLA_DEADZONE);
    }
    if (n <= 0.0f) return 0.0f;
    return denormalize(CAMERA_VANILLA_DEADZONE, 1.0f, n * s->controller_zoom_mult);
}

float camera_controller_pitch_delta(const CameraSettings *s, float signed_value, bool *engaged) {
    float dz = camera_deadzone(s);
    float n = camera_stick_normalize_held(signed_value, dz, s->deadzone_hysteresis, engaged);
    if (n <= 0.0f) return 0.0f;
    float sign = signed_value < 0.0f ? -1.0f : 1.0f;
    float value = sign * powf(n, s->stick_response_curve);
    if (s->invert_controller_pitch) value = -value;
    return value * s->controller_camera_rotation_mult;
}

float camera_interp_to(float current, float target, float dt, float speed) {
    if (speed <= 0.0f) return target;
    float distance = target - current;
    if (fabsf(distance) < FLT_EPSILON) return target;
    float alpha = dt * speed;
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    return current + distance * alpha;
}

float camera_smooth_toward(float current, float target, float dt, float seconds) {
    if (!(seconds > 0.0f)) return target;
    float alpha = 1.0f - expf(-dt / seconds);
    return current + (target - current) * alpha;
}

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

bool camera_calculate_pitch(const CameraSettings *s, CameraPitchState *state,
                            uint32_t mode_flags, float dt,
                            float game_pitch, float *out_pitch) {
    CameraMode mode = camera_mode_from_flags(mode_flags);
    *out_pitch = game_pitch;
    /* Upstream counts mouse Y only while the game rotates with the mouse. */
    float mouse_dy = (mode_flags & CAMERA_MODE_FLAG_MOUSE_ROTATION) ? state->mouse_dy : 0.0f;
    state->mouse_dy = 0.0f;

    if (camera_can_adjust_pitch(s, mode)) {
        bool combat = mode == CAMERA_MODE_COMBAT || mode == CAMERA_MODE_COMBAT_TACTICAL;
        const CameraContextSettings *c = combat ? &s->combat : &s->exploration;
        float lo = c->unlocked_pitch_min, hi = c->unlocked_pitch_max;

        /* Ease the rate toward the stick so taps and event gaps don't jerk. */
        float target_rate = state->controller_pitch_delta * s->controller_pitch_speed *
                            s->controller_pitch_mult;
        float alpha = s->pitch_smoothing > 0.0f ? 1.0f - expf(-dt / s->pitch_smoothing) : 1.0f;
        state->pitch_rate += (target_rate - state->pitch_rate) * alpha;
        if (fabsf(state->pitch_rate) < 1e-3f && target_rate == 0.0f) state->pitch_rate = 0.0f;
        float delta = state->pitch_rate * dt;
        if (isfinite(mouse_dy))
            delta += mouse_dy * s->mouse_pitch_mult * s->mouse_camera_rotation_mult *
                     (s->invert_mouse_pitch ? -1.0f : 1.0f);

        if (!state->has_pitch) {
            state->pitch = clampf(s->unlocked_pitch_initial_value, lo, hi);
            state->has_pitch = true;
            state->resync = true;
        } else if (state->pitch >= lo && state->pitch <= hi) {
            /* Upstream kept the old pitch when a step would overshoot, so a
             * fast stick stalled short of the limit; clamp to it instead. */
            state->pitch = clampf(state->pitch + delta, lo, hi);
        } else if ((state->pitch > hi && delta < 0.0f) || (state->pitch < lo && delta > 0.0f)) {
            state->pitch += delta;   /* outside new limits: input may move inward */
        }

        /* Smoothly follow limits that changed since the last frame. */
        if (state->pitch > hi)
            state->pitch = camera_interp_to(state->pitch, hi, dt, s->unlocked_pitch_clamp_speed);
        else if (state->pitch < lo)
            state->pitch = camera_interp_to(state->pitch, lo, dt, s->unlocked_pitch_clamp_speed);

        /* Hand-over from the game's pitch: ease in instead of snapping. */
        if (state->resync) {
            state->resync = false;
            state->shown = game_pitch;
            state->blending = s->pitch_transition_speed > 0.0f;
        }
        if (state->blending) {
            state->shown = camera_interp_to(state->shown, state->pitch, dt,
                                            s->pitch_transition_speed);
            if (fabsf(state->shown - state->pitch) < 0.05f) state->blending = false;
        }
        *out_pitch = state->blending ? state->shown : state->pitch;
        return true;
    }

    /* Locked in this mode: ease from our pitch back to the game's. */
    state->blending = false;
    state->pitch_rate = 0.0f;
    if (state->has_pitch) {
        if (state->pitch != game_pitch) {
            state->pitch = camera_interp_to(state->pitch, game_pitch, dt,
                                            s->unlocked_pitch_clamp_speed);
            *out_pitch = state->pitch;
        } else {
            state->has_pitch = false;
        }
    }
    return false;
}

/* ---- zoom limits and floor guard ----------------------------------------- */

void camera_multi_target_zoom_range(const CameraContextSettings *c,
                                    const CameraContextSettings *other,
                                    float game_min, float game_max,
                                    float *min_out, float *max_out) {
    float hi = fmaxf(c->zoom_max, c->alt_zoom_max);
    if (other) hi = fmaxf(hi, fmaxf(other->zoom_max, other->alt_zoom_max));
    if (isfinite(game_max) && game_max > hi) hi = game_max;
    float lo = isfinite(game_min) && game_min > 0.0f ? game_min : c->zoom_min;
    *min_out = lo < hi ? lo : hi;
    *max_out = hi;
}

float camera_keep_zoom_step(CameraKeepZoomState *k, float target, float frame_end,
                            float rate, float dt, bool multi, bool reset,
                            float lo, float hi, float *floor_out) {
    *floor_out = NAN;
    if (!isfinite(target)) return target;
    if (!k->have || reset) {
        k->zoom = !reset && isfinite(frame_end) ? frame_end : target;
        k->have = true;
    } else if (fabsf(rate) > 1e-6f && isfinite(frame_end)) {
        /* The player is zooming: move their zoom by the step the game took
         * (target += rate * dt). A different step means something else moved
         * the zoom in the same frame, or the game stopped at a limit; then the
         * zoom on screen is the one they are zooming from. */
        float step = target - frame_end;
        float expect = rate * dt;
        if (fabsf(step - expect) <= fabsf(expect) * 0.5f + 0.01f) k->zoom += step;
        else k->zoom = target;
    }
    if (multi) {
        /* The multi-target range is the game's framing range, not the
         * player's: clamp the floor to it, but keep their zoom for later. */
        *floor_out = clampf(k->zoom, lo, hi);
        return target;
    }
    k->zoom = clampf(k->zoom, lo, hi);
    return k->zoom;
}

float camera_low_pitch_zoom_cap(const CameraSettings *s, float pitch, float max_zoom) {
    if (!s->limit_zoom_at_low_pitch || !(s->low_pitch_angle > 0.0f)) return max_zoom;
    float low = s->low_pitch_max_zoom;
    if (low >= max_zoom || pitch >= s->low_pitch_angle) return max_zoom;
    float t = pitch > 0.0f ? pitch / s->low_pitch_angle : 0.0f;
    t = t * t * (3.0f - 2.0f * t);   /* smoothstep: no kink at either end */
    return low + (max_zoom - low) * t;
}

static bool under_floor(const float origin[3], const float dir[3], float zoom, float offset,
                        CameraFloorFn floor_fn, void *ctx) {
    float pos[3] = {
        origin[0] + dir[0] * zoom, origin[1] + dir[1] * zoom, origin[2] + dir[2] * zoom,
    };
    float floor_y = 0.0f;
    if (!floor_fn(ctx, pos, &floor_y) || !isfinite(floor_y)) return false;
    return pos[1] < floor_y + offset;
}

#define FLOOR_GUARD_STEPS     16
#define FLOOR_GUARD_MIN_STEP  0.25f
#define FLOOR_GUARD_BISECT    5

float camera_floor_guard(const float origin[3], const float dir[3], float zoom,
                         float min_zoom, float offset, CameraFloorFn floor_fn, void *ctx) {
    if (!floor_fn || !(zoom > min_zoom)) return zoom;
    if (!under_floor(origin, dir, zoom, offset, floor_fn, ctx)) return zoom;

    float step = (zoom - min_zoom) / FLOOR_GUARD_STEPS;
    if (step < FLOOR_GUARD_MIN_STEP) step = FLOOR_GUARD_MIN_STEP;
    float bad = zoom, good = zoom;
    for (;;) {
        good = bad - step;
        if (good <= min_zoom) return min_zoom;
        if (!under_floor(origin, dir, good, offset, floor_fn, ctx)) break;
        bad = good;
    }
    /* good is clear, bad (= good + step) is under: find the edge. */
    for (int i = 0; i < FLOOR_GUARD_BISECT; i++) {
        float mid = 0.5f * (good + bad);
        if (under_floor(origin, dir, mid, offset, floor_fn, ctx)) bad = mid;
        else good = mid;
    }
    return good;
}
