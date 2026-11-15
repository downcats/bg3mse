// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from ersh1's BG3 Native Camera Tweaks: https://github.com/ersh1/BG3_NativeCameraTweaks

/**
 * camera_settings.h - Native Camera Tweaks settings and camera math.
 *
 * Pure code (no game memory, no logging) so the unit tests can run it. The settings
 * mirror ersh1's BG3 Native Camera Tweaks (BG3NativeCameraTweaks.toml): same
 * section and key names, same defaults, except where camera_settings.c notes
 * a macOS deviation. The file lives at <data dir>/NativeCameraTweaks.toml.
 */

#ifndef CAMERA_SETTINGS_H
#define CAMERA_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAMERA_SETTINGS_FILENAME "NativeCameraTweaks.toml"

/* The game's own right-stick threshold (0x3f266666 in CameraSystem::OnInputEvent). */
#define CAMERA_VANILLA_DEADZONE 0.65f

/* ecl::GameCameraBehavior + 0xa8 mode flags (same bits as the Windows build). */
#define CAMERA_MODE_FLAG_COMBAT         (1u << 0)
#define CAMERA_MODE_FLAG_TACTICAL       (1u << 2)
#define CAMERA_MODE_FLAG_MOUSE_ROTATION (1u << 8)

/* Where "the zoom modifier (L3) is held" is read from. */
typedef enum CameraModifierSource {
    CAMERA_MODIFIER_BOTH = 0,   /* game input state OR physical gamepad button */
    CAMERA_MODIFIER_GAME,       /* ls::InputManager ToggleInputMode value only */
    CAMERA_MODIFIER_GAMEPAD,    /* GameController framework button only */
} CameraModifierSource;

/* One camera context (exploration or combat). */
typedef struct CameraContextSettings {
    bool  unlock_pitch;
    bool  keep_tactical_pitch_locked;
    float unlocked_pitch_min;
    float unlocked_pitch_max;

    bool  override_locked_pitch;
    float locked_pitch_close, locked_pitch_far;
    float locked_tactical_pitch_close, locked_tactical_pitch_far;
    float locked_alt_pitch_close, locked_alt_pitch_far;

    bool  override_zoom;
    float zoom_min, zoom_max;
    float tactical_zoom_min, tactical_zoom_max;
    float alt_zoom_min, alt_zoom_max;
    bool  keep_zoom;   /* macOS: put the player's zoom back after the game moves it */

    bool  override_fov;
    float fov_close, fov_far, tactical_fov, alt_fov_close, alt_fov_far;

    bool  override_offset;
    float horizontal_offset_mult, vertical_offset_mult;
} CameraContextSettings;

typedef struct CameraSettings {
    /* [General] */
    bool  enabled;                      /* macOS: master switch, read at startup */
    float unlocked_pitch_initial_value;
    float unlocked_pitch_clamp_speed;
    bool  unlocked_pitch_limit_clipping; /* floor guard: pull the camera in above the ground */
    float unlocked_pitch_floor_offset;   /* metres the camera keeps above the floor */
    bool  reset_zoom_on_zone_change;     /* false: a new camera (zone change, load) keeps the zoom */
    bool  watch_for_config_changes;
    /* macOS additions */
    float pitch_transition_speed;        /* eases our pitch in when it takes over (0 = snap) */
    bool  limit_zoom_at_low_pitch;       /* shrink the zoom limit near the horizon */
    float low_pitch_angle;               /* degrees: below this the limit shrinks */
    float low_pitch_max_zoom;            /* zoom limit at 0 degrees and below */
    bool  log_camera_events;             /* debug: log the game's camera moves */

    CameraContextSettings exploration;
    CameraContextSettings combat;

    /* [Mouse] */
    float mouse_camera_rotation_mult;
    float mouse_pitch_mult;              /* degrees per point of mouse Y while mouse-rotating */
    float mouse_zoom_mult;
    bool  invert_mouse_pitch;

    /* [Keyboard] */
    float keyboard_camera_rotation_mult;

    /* [Controller] */
    float controller_camera_rotation_mult;
    float controller_pitch_mult;
    /* macOS: the Windows build scaled pitch by a per-camera rotation speed
     * whose offset does not hold on macOS; these replace it. */
    float controller_pitch_speed;   /* degrees/s at full stick, before the mults */
    float stick_response_curve;     /* exponent on stick deflection (1 = linear) */
    float pitch_smoothing;          /* seconds for the pitch rate to settle (0 = none) */
    float controller_zoom_mult;
    float zoom_smoothing;           /* macOS: seconds for the stick zoom rate to settle */
    bool  invert_controller_pitch;
    bool  swap_zoom_and_pitch;
    bool  use_right_stick_press_for_zoom;
    /* macOS additions */
    CameraModifierSource zoom_modifier_source;
    bool  apply_deadzone_to_zoom;

    /* [ControllerDeadzone] */
    bool  override_right_stick_deadzone;
    float new_deadzone;
    float deadzone_hysteresis;      /* macOS: release threshold sits this far below the deadzone */
} CameraSettings;

/** Fill s with the defaults (upstream defaults + macOS deviations). */
void camera_settings_defaults(CameraSettings *s);

/**
 * Parse TOML-subset text ("[Section]", "Key = value", '#' comments,
 * true/false, numbers, quoted strings) over the current contents of s.
 * Unknown keys, malformed lines and out-of-range numbers are skipped and
 * counted; the setting keeps its previous value. Returns the number of
 * rejected lines (0 = clean). `first_error`, when non-NULL, receives the
 * 1-based line number of the first rejected line (0 if none).
 */
int camera_settings_parse(CameraSettings *s, const char *text, int *first_error);

/** The commented default file written when none exists. */
const char *camera_settings_default_file(void);

/* ---- camera math (upstream CameraTweaksManager equivalents) ------------- */

typedef enum CameraMode {
    CAMERA_MODE_EXPLORATION = 0,
    CAMERA_MODE_EXPLORATION_TACTICAL,
    CAMERA_MODE_COMBAT,
    CAMERA_MODE_COMBAT_TACTICAL,
} CameraMode;

CameraMode camera_mode_from_flags(uint32_t flags);
bool camera_can_adjust_pitch(const CameraSettings *s, CameraMode mode);

/** Active right-stick deadzone (override or vanilla). */
float camera_deadzone(const CameraSettings *s);

/** 0 inside the deadzone, else |v| rescaled so the deadzone edge maps to 0
 *  and full deflection to 1. Sign is dropped. */
float camera_stick_normalize(float v, float deadzone);

/** camera_stick_normalize with a release buffer: once *engaged, the stick
 *  stays live down to deadzone - hysteresis and is measured from there (so it
 *  still fades to 0), and noise at the edge cannot flicker it on and off.
 *  `engaged` may be NULL (plain deadzone). */
float camera_stick_normalize_held(float v, float deadzone, float hysteresis, bool *engaged);

/** Upstream AdjustInputValueForDeadzone: remap a stick value into the game's
 *  [0.65, 1] response band (0 inside our deadzone), optionally scaled by the
 *  controller rotation multiplier. */
float camera_adjust_rotation_input(const CameraSettings *s, float v, bool apply_mult);

/** Zoom stick value for the game: our deadzone (when enabled) mapped into the
 *  game's response band through StickResponseCurve, then scaled by
 *  ControllerZoomMult around 0.65. Returns 0 inside the deadzone. `engaged`
 *  is the stick's hysteresis state (may be NULL). */
float camera_adjust_zoom_input(const CameraSettings *s, float v, bool *engaged);

/** Upstream SetControllerPitchDelta: signed pitch rate from a stick value.
 *  `engaged` as for camera_adjust_zoom_input. */
float camera_controller_pitch_delta(const CameraSettings *s, float signed_value, bool *engaged);

/** Upstream InterpTo (with the same early-out on distance < FLT_EPSILON). */
float camera_interp_to(float current, float target, float dt, float speed);

/** Exponential approach: `seconds` is the time constant (0 = jump to target). */
float camera_smooth_toward(float current, float target, float dt, float seconds);

/** Per-player pitch state (upstream PlayerData). */
typedef struct CameraPitchState {
    bool  has_pitch;
    float pitch;
    float controller_pitch_delta;
    float pitch_rate;               /* smoothed degrees/s */
    float mouse_dy;                 /* mouse Y (points, + = down) since the last frame;
                                     * set by the caller, consumed by the next call */
    /* Hand-over blend: when our pitch takes over from the game's (first
     * frame, after a pinned/cutscene pitch), the shown pitch starts at the
     * game's and eases to ours instead of snapping. */
    bool  resync;                   /* set by the caller: blend from the game's pitch */
    bool  blending;
    float shown;
} CameraPitchState;

/**
 * Upstream CalculateCameraPitch. `game_pitch` is the game's computed pitch;
 * the return value is the pitch to use. Stick input moves the pitch at
 * controller_pitch_delta x ControllerPitchSpeed x ControllerPitchMult degrees
 * per second, eased by PitchSmoothing. While the game's mouse rotation is on
 * (mode flag 0x100), mouse_dy x MousePitchMult x MouseCameraRotationMult
 * degrees are added (negated by InvertMousePitch). Returns true when the mode
 * is unlocked (state->pitch drives the camera).
 */
bool camera_calculate_pitch(const CameraSettings *s, CameraPitchState *state,
                            uint32_t mode_flags, float dt, float game_pitch, float *out_pitch);

/* ---- zoom limits and floor guard (macOS additions) ----------------------- */

/** The zoom range to write for multi-target framing (two or more camera
 *  targets) in context `c`. The game's own range (`game_min`..`game_max`,
 *  3.5..50 on 4.1.1.7631656) is wider than the player's, so it is only ever
 *  widened: the maximum is the farthest of `game_max`, ZoomMax and AltZoomMax
 *  (and `other`'s, when non-NULL: the game also caps every camera's arm at the
 *  combat definition's multi-target maximum). The minimum stays the game's
 *  framing floor, kept at or below that maximum. */
void camera_multi_target_zoom_range(const CameraContextSettings *c,
                                    const CameraContextSettings *other,
                                    float game_min, float game_max,
                                    float *min_out, float *max_out);

/* KeepZoom (macOS addition): in combat the game sets the zoom itself when it
 * retargets the camera (a new turn, an attack or spell shot) and refits it
 * every frame while two or more targets are framed. */
typedef struct CameraKeepZoomState {
    bool  have;
    float zoom;    /* the zoom the player chose */
} CameraKeepZoomState;

/**
 * One frame of KeepZoom, at the pitch site. `target` is the zoom target there,
 * `frame_end` the target when the previous update finished (NAN if unknown),
 * `rate` the stick/wheel zoom rate the game applied this frame (target +=
 * rate * dt), `multi` whether two or more targets are framed, `reset` whether
 * the CameraCenter input reset the zoom, and [lo, hi] the active range.
 * Returns the zoom target to use: the player's zoom with one target, the
 * game's otherwise. `*floor_out` is the minimum the game's multi-target fit
 * must keep (the player's zoom, clamped to [lo, hi]) or NAN with one target.
 * A first frame (or `reset`) takes the zoom from `frame_end` (or `target`).
 */
float camera_keep_zoom_step(CameraKeepZoomState *k, float target, float frame_end,
                            float rate, float dt, bool multi, bool reset,
                            float lo, float hi, float *floor_out);

/** Zoom limit at `pitch` degrees: `max_zoom` at or above LowPitchAngle,
 *  easing down to LowPitchMaxZoom at 0 degrees and below. A far camera near
 *  the horizon is what ends up behind walls and outside the level. */
float camera_low_pitch_zoom_cap(const CameraSettings *s, float pitch, float max_zoom);

/** Floor height below `pos` (x, y-up, z). Returns false when the game has no
 *  floor data there, which counts as "not under the floor". */
typedef bool (*CameraFloorFn)(void *ctx, const float pos[3], float *floor_y);

/**
 * Upstream AdjustCameraZoomForPitch: the largest zoom <= `zoom` (and >=
 * `min_zoom`) whose camera position origin + dir * zoom stays `offset`
 * metres above the floor. Coarse steps down, then a short bisection, so it
 * costs about 20 floor queries at worst instead of upstream's 0.01 steps.
 */
float camera_floor_guard(const float origin[3], const float dir[3], float zoom,
                         float min_zoom, float offset, CameraFloorFn floor_fn, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_SETTINGS_H */
