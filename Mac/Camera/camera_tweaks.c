// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from ersh1's BG3 Native Camera Tweaks: https://github.com/ersh1/BG3_NativeCameraTweaks

/**
 * camera_tweaks.c - Native Camera Tweaks runtime (hooks and game memory).
 *
 * Port of ersh1's BG3 Native Camera Tweaks (Hooks.cpp). On Windows the mod
 * rewrites eight call sites found by byte patterns; on macOS the binary is
 * symbolized, so most upstream hooks map to a named function here
 * (found and checked at start-up by CameraSites.cpp):
 *
 *   upstream                   macOS (4.1.1.7631656)
 *   UpdateCamera               BL CameraSystem::Update -> UpdateGameCameraBehavior
 *   UpdateCameraPitch          (inlined) pitch-adjust block in UpdateGameCameraBehavior
 *   CalculateCameraPitch       BL -> GameCameraBehavior::GetCameraPitchDegrees at that block
 *   HandleCameraInput          (inlined) ecl::CameraSystem::OnInputEvent, entry hook
 *   HandleToggleInputMode      (inlined) ecl::EocClient::OnInputEvent, entry hook
 *   GetInputValue              ls::InputManager::GetInputValue (LTO ABI, see camera_platform.h)
 *   bIsInControllerMode        anonymous bool read by GetCameraPitchDegrees
 *   UnkCameraSingleton         EoCGlobalSwitches (s_global_switches_slot); every
 *                              CameraDefinition sits +0x28 from the Windows offsets
 *   AfterUpdateCameraZoom      at the pitch site (same frame, before the arm check)
 *   GetFloorLevel              ecl::CameraHelpers::RestrictCamTargetDestHeight (LTO ABI)
 *   SetDefaultZoom             (inlined) CameraSystem::Update's loop over new cameras;
 *                              undone in hook_pitch_site on a camera's first frame
 *   SDLMouseYHook              BL [NSEvent deltaY] in the game view's mouseMoved:
 *
 * Call sites are patched with one word, a BL to a jump pad (an island within
 * branch range of the site), so no PC-relative instruction is ever relocated.
 * The two entry hooks replace one word each (Platform/EntryHook.h); the entry
 * hook planner, run by the site check, decides whether they can go in.
 *
 * Threads: the update hooks run on the client thread; input listeners may
 * run on another. Settings are double-buffered and swapped atomically; the
 * per-player pitch state is plain floats, as upstream (a torn float costs at
 * most one frame of camera input).
 */

#include "camera_tweaks.h"
#include "camera_gamepad.h"
#include "camera_patch.h"
#include "camera_settings.h"

#include "camera_platform.h"
#include "../Platform/code_patch.h"

#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <mach/mach_time.h>

/* ---- game layout (same as the Windows build, verified on 7631656) -------- */

/* ls::InputEvent */
#define INPUT_EVENT_ID          0x00   /* uint32 */
#define INPUT_EVENT_PLAYER      0x06   /* int8 EInputPlayerIndex */
#define INPUT_EVENT_VALUE       0x14   /* float: mouse/keyboard magnitude */
#define INPUT_EVENT_VALUE2      0x18   /* float: controller magnitude */
#define INPUT_EVENT_PRESSED     0x1c   /* uint8: new state, 0 = released */
#define INPUT_EVENT_COPY_SIZE   0x40

#define INPUT_ID_CAMERA_CENTER  0x61   /* LEGACY_SetTarget + ResetToDefault + ResetRotation */
#define INPUT_ID_ZOOM_IN        104
#define INPUT_ID_ZOOM_OUT       105
#define INPUT_ID_ROTATE_LEFT    107
#define INPUT_ID_ROTATE_RIGHT   108
#define INPUT_ID_MOUSE_ROTATE_L 109
#define INPUT_ID_MOUSE_ROTATE_R 110
#define INPUT_ID_TOGGLE_INPUT   0xC0   /* ToggleInputMode (L3), acts on release */

/* ecl::GameCameraBehavior */
#define BEHAVIOR_ARM_ORIGIN     0x48   /* Vector3f: the camera arm starts here (CollideCameraArm) */
#define BEHAVIOR_ZOOM_SHOWN     0x58   /* float: zoom in use, eased toward +0x5c */
#define BEHAVIOR_ZOOM_TARGET    0x5c   /* float: zoom the stick/wheel moves, clamped later */
#define BEHAVIOR_ARM_DIR        0x70   /* Vector3f: unit arm direction, camera = origin + dir*zoom */
#define BEHAVIOR_ANGLE_DELTA    0x9c   /* float: rotate rate (keys, stick) */
#define BEHAVIOR_MOUSE_ROTATE   0xa0   /* float: mouse rotation delta */
#define BEHAVIOR_ZOOM_DELTA     0xa4   /* float: zoom rate, written only by OnInputEvent */
#define BEHAVIOR_MODE_FLAGS     0xa8   /* uint32 */
#define BEHAVIOR_YAW            0xac   /* float */
#define BEHAVIOR_TARGET_COUNT   0x13c  /* uint32: size of the target array at +0x130 (SetTarget
                                        * appends); > 1 selects the multi-target zoom range */
#define BEHAVIOR_ZOOM_SAVED     0x160  /* float: the player's zoom, copied from +0x5c while they
                                        * zoom (in UpdateGameCameraBehavior); turn changes
                                        * restore it */
#define BEHAVIOR_FORCED_PITCH   0x1e4  /* bool: GetCameraPitchDegrees returns +0x1e0 */

/* GameTime passed to UpdateGameCameraBehavior */
#define GAME_TIME_DELTA         0x08   /* float seconds */

/* CameraDefinition fields */
#define DEF_MAX_ZOOM            0x28
#define DEF_MIN_ZOOM            0x2c
#define DEF_ALT_MAX_ZOOM        0x30
#define DEF_ALT_MIN_ZOOM        0x34
#define DEF_PITCH_SPEED_A       0x48
#define DEF_MULTI_MAX_ZOOM      0x58   /* zoom range with two or more camera targets */
#define DEF_MULTI_MIN_ZOOM      0x5c
#define DEF_HORIZONTAL_OFFSET   0x64
#define DEF_VERTICAL_OFFSET     0x68
#define DEF_FOV_CLOSE           0x84
#define DEF_FOV_FAR             0x88
#define DEF_ALT_FOV_CLOSE       0x8c
#define DEF_ALT_FOV_FAR         0x90
#define DEF_TACTICAL_MIN_ZOOM   0xc8
#define DEF_TACTICAL_MAX_ZOOM   0xcc
#define DEF_TACTICAL_FOV        0xd0
#define DEF_PITCH_SPEED_B       0xf0
#define DEF_PITCH_SPEED_C       0xf4
#define DEF_PITCH_FAR           0x160
#define DEF_PITCH_CLOSE         0x164
#define DEF_COMBAT_PITCH_FAR    0x168
#define DEF_COMBAT_PITCH_CLOSE  0x16c
#define DEF_TACTICAL_PITCH_FAR  0x170
#define DEF_TACTICAL_PITCH_CLOSE 0x174
#define DEF_ALT_PITCH_FAR       0x178
#define DEF_ALT_PITCH_CLOSE     0x17c

#define PITCH_SPEED_UNLOCKED    100000.0f
#define MAX_DELTA_TIME          0.25f
#define RELOAD_INTERVAL_SEC     1.0

/* Every CameraDefinition field this module may write (snapshot/restore). */
static const uint32_t g_def_fields[] = {
    DEF_MAX_ZOOM, DEF_MIN_ZOOM, DEF_ALT_MAX_ZOOM, DEF_ALT_MIN_ZOOM,
    DEF_MULTI_MAX_ZOOM, DEF_MULTI_MIN_ZOOM,
    DEF_HORIZONTAL_OFFSET, DEF_VERTICAL_OFFSET,
    DEF_FOV_CLOSE, DEF_FOV_FAR, DEF_ALT_FOV_CLOSE, DEF_ALT_FOV_FAR,
    DEF_TACTICAL_MIN_ZOOM, DEF_TACTICAL_MAX_ZOOM, DEF_TACTICAL_FOV,
    DEF_PITCH_FAR, DEF_PITCH_CLOSE, DEF_COMBAT_PITCH_FAR, DEF_COMBAT_PITCH_CLOSE,
    DEF_TACTICAL_PITCH_FAR, DEF_TACTICAL_PITCH_CLOSE,
    DEF_ALT_PITCH_FAR, DEF_ALT_PITCH_CLOSE,
};
#define DEF_FIELD_COUNT (sizeof(g_def_fields) / sizeof(g_def_fields[0]))

enum { DEF_EXPLORATION = 0, DEF_COMBAT, DEF_COUNT };

/* ---- function types ------------------------------------------------------ */

typedef uint64_t (*UpdateGcbFn)(void *self, void *world, void *game_time, void *view);
typedef float (*PitchFn)(void *behavior, bool a1, bool a2);
typedef uint64_t (*CameraOnInputFn)(void *self, void *entity_ref, const void *event);
typedef uint64_t (*ClientOnInputFn)(void *self, const void *event);
typedef struct { uint64_t values; uint64_t pressed; } InputValueRet;
typedef InputValueRet (*GetInputValueFn)(void *manager, uint32_t id, int32_t player);
/* RestrictCamTargetDestHeight, LTO ABI (one Vector3f and the debug pointer
 * dropped): x0 = floor height bits | flag byte << 32, x1 low byte = no data. */
typedef struct { uint64_t floor; uint64_t no_data; } FloorHeightRet;
typedef FloorHeightRet (*FloorHeightFn)(void *world, uint32_t flag, const void *definition,
                                        const float *pos, void *level_manager);
/* The objc_msgSend$deltaY selector stub: x0 = the NSEvent, CGFloat in d0. */
typedef double (*MouseDeltaFn)(void *event);

/* ---- state --------------------------------------------------------------- */

static bool s_active = false;
static uintptr_t s_base = 0;
static const CameraTweaksOffsets *s_off = NULL;
static uintptr_t s_global_switches_slot = 0;

static UpdateGcbFn s_orig_update_gcb = NULL;
static PitchFn s_orig_pitch = NULL;
static CameraOnInputFn s_orig_camera_input = NULL;
static ClientOnInputFn s_orig_client_input = NULL;
static GetInputValueFn s_get_input_value = NULL;
static FloorHeightFn s_floor_height = NULL;
static MouseDeltaFn s_orig_mouse_dy = NULL;

static bool s_pitch_hooked = false;   /* pitch replacement live: zoom may become pitch */

/* Mouse pitch (upstream SDLMouseYHook): the game view's mouseMoved: (main
 * thread) adds each event's deltaY; the next pitch frame (client thread)
 * takes the sum. Capped so a stall cannot turn into one huge tilt. */
#define MOUSE_DY_MAX_PER_FRAME 400.0f
static _Atomic float s_mouse_dy = 0.0f;

/* ResetZoomOnZoneChange (upstream SetDefaultZoom): the zoom target the last
 * camera ended its update with, for the next new camera (client thread). */
static float s_last_zoom = NAN;

static CameraSettings s_settings[2];
static _Atomic int s_settings_index = 0;
static _Atomic bool s_settings_dirty = true;

static char s_config_path[1024];
static struct timespec s_config_mtime;
static uint64_t s_last_reload_check = 0;

static CameraPitchState s_pitch;              /* single local player */
static float s_delta_time = 0.0f;
static _Atomic uint32_t s_last_mode_flags = 0;
static _Atomic bool s_pitch_active = false;   /* last frame: our pitch drives the camera */
static _Atomic bool s_skip_toggle_input = false;
static bool s_logged_forced_pitch = false;
static _Atomic bool s_logged_pitch = false;
static _Atomic bool s_logged_input = false;
static _Atomic bool s_logged_zoom_modifier = false;
static _Atomic bool s_logged_toggle_skip = false;

/* Set by hook_update_gcb for the pitch site nested inside it (client thread). */
static void *s_frame_world = NULL;
static void *s_frame_level_manager = NULL;

/* Zoom state (client thread, pitch site). */
static bool  s_zoom_guarded = false;    /* last frame the floor guard lowered the zoom */
static float s_zoom_intent = 0.0f;      /* the zoom the player asked for */
static float s_zoom_written = 0.0f;     /* what the guard wrote to +0x5c */
static float s_zoom_rate = 0.0f;        /* smoothed stick zoom rate */
static float s_zoom_rate_target = 0.0f; /* last rate OnInputEvent wrote */
static float s_zoom_rate_written = 0.0f;
static _Atomic bool s_logged_floor_guard = false;
static _Atomic bool s_logged_zoom_cap = false;
static _Atomic bool s_logged_floor_query = false;
static _Atomic bool s_logged_multi_target = false;

/* KeepZoom (client thread). UpdateGameCameraBehavior runs once per camera
 * entity (a query loop in CameraSystem::Update), so the state is per camera.
 * A camera's GameCameraBehavior moves when the game adds or removes one of
 * its components (live: every turn change), so a slot follows its camera to
 * the new address (keep_zoom_slot). */
#define KEEP_ZOOM_SLOTS 4
#define KEEP_ZOOM_PASS_NS   2000000ull      /* one CameraSystem::Update pass */
#define KEEP_ZOOM_ALIVE_NS  250000000ull    /* a camera updated this recently moved */
typedef struct KeepZoomSlot {
    const uint8_t *behavior;
    CameraKeepZoomState keep;
    float frame_end;          /* zoom target when its last update finished */
    uint64_t used;
    uint64_t last_ns;         /* when keep_zoom last ran for it */
    bool fresh;               /* a new camera, not a moved one: not seen by the pitch site yet */
    /* LogCameraEvents: last values logged, and yaw turns the player did not make */
    uint32_t seen_flags, seen_targets;
    bool seen, turning, pinned;
    float frame_end_yaw, turn_from;
} KeepZoomSlot;
static KeepZoomSlot s_keep_slots[KEEP_ZOOM_SLOTS];
static uint64_t s_keep_clock = 0;
static const uint8_t *s_frame_behavior = NULL;   /* camera of the update in progress */
static _Atomic bool s_camera_center_input = false;
static _Atomic bool s_logged_keep_zoom = false;
static _Atomic bool s_logged_keep_floor = false;
static _Atomic bool s_logged_mouse_pitch = false;

/* LogCameraEvents: a debug trace of the game's own camera moves, bounded so a
 * session left with it on cannot flood the log. */
#define CAMERA_EVENT_LOG_LIMIT 2000
static int s_camera_events_logged = 0;
#define LOG_CAMERA_EVENT(...) do { \
        if (s_camera_events_logged < CAMERA_EVENT_LOG_LIMIT) { \
            s_camera_events_logged++; \
            LOG_GAME_INFO("[CameraTweaks] event: " __VA_ARGS__); \
        } \
    } while (0)

/* Right-stick Y hysteresis, per event direction (input thread). */
static bool s_stick_engaged[2];

static float s_def_snapshot[DEF_COUNT][DEF_FIELD_COUNT];
static float s_def_desired[DEF_COUNT][DEF_FIELD_COUNT];   /* from the settings */
static float s_def_written[DEF_COUNT][DEF_FIELD_COUNT];   /* in game memory now */
static bool s_def_have_snapshot = false;
static bool s_def_refused = false;

static const CameraSettings *settings(void) {
    return &s_settings[atomic_load_explicit(&s_settings_index, memory_order_acquire)];
}

/* ---- settings file ------------------------------------------------------- */

static char *read_file(const char *path, size_t limit) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = malloc(limit + 1);
    size_t n = buf ? fread(buf, 1, limit, f) : 0;
    fclose(f);
    if (!buf) return NULL;
    buf[n] = '\0';
    return buf;
}

static bool stat_mtime(const char *path, struct timespec *out) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    *out = st.st_mtimespec;
    return true;
}

/* Parse the file into the inactive buffer and publish it. `first` creates
 * the default file when none exists. */
static void load_settings(bool first) {
    int next = 1 - atomic_load(&s_settings_index);
    CameraSettings *s = &s_settings[next];
    camera_settings_defaults(s);

    char *text = read_file(s_config_path, 64 * 1024);
    if (!text && first) {
        FILE *f = fopen(s_config_path, "wx");
        if (f) {
            fputs(camera_settings_default_file(), f);
            fclose(f);
            LOG_GAME_INFO("[CameraTweaks] Wrote default settings to %s", s_config_path);
        }
        text = read_file(s_config_path, 64 * 1024);
    }
    if (text) {
        int first_error = 0;
        int rejected = camera_settings_parse(s, text, &first_error);
        if (rejected)
            LOG_GAME_WARN("[CameraTweaks] %s: %d setting(s) ignored (first at line %d)",
                          s_config_path, rejected, first_error);
        free(text);
    }
    if (!first) {
        /* Enabled only takes effect at startup. */
        s->enabled = settings()->enabled;
    }
    stat_mtime(s_config_path, &s_config_mtime);
    atomic_store_explicit(&s_settings_index, next, memory_order_release);
    atomic_store(&s_settings_dirty, true);
}

static void maybe_reload_settings(void) {
    if (!settings()->watch_for_config_changes) return;
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    uint64_t now = mach_absolute_time();
    double elapsed = (double)(now - s_last_reload_check) * tb.numer / tb.denom / 1e9;
    if (elapsed < RELOAD_INTERVAL_SEC) return;
    s_last_reload_check = now;

    struct timespec m;
    if (!stat_mtime(s_config_path, &m)) return;
    if (m.tv_sec == s_config_mtime.tv_sec && m.tv_nsec == s_config_mtime.tv_nsec) return;
    LOG_GAME_INFO("[CameraTweaks] Settings changed; reloading");
    load_settings(false);
}

/* ---- camera definitions (upstream SetCameraSettings) --------------------- */

static uint8_t *global_switches(void) {
    void *gs = NULL;
    if (!safe_memory_read((mach_vm_address_t)s_global_switches_slot, &gs, sizeof(gs)))
        return NULL;
    return (uint8_t *)gs;
}

static int field_index(uint32_t offset) {
    for (size_t i = 0; i < DEF_FIELD_COUNT; i++)
        if (g_def_fields[i] == offset) return (int)i;
    return -1;
}

static void set_field(float *values, uint32_t offset, float v) {
    int i = field_index(offset);
    if (i >= 0) values[i] = v;
}

static bool snapshot_plausible(const float *v) {
    for (size_t i = 0; i < DEF_FIELD_COUNT; i++)
        if (!isfinite(v[i]) || fabsf(v[i]) > 1000.0f) return false;
    float min_zoom = v[field_index(DEF_MIN_ZOOM)], max_zoom = v[field_index(DEF_MAX_ZOOM)];
    return min_zoom > 0.0f && max_zoom > min_zoom;
}

/* Desired values for one definition: the game's own values, then overrides. */
static void desired_values(int which, const CameraSettings *s, float *out) {
    memcpy(out, s_def_snapshot[which], sizeof(s_def_snapshot[which]));
    bool combat = which == DEF_COMBAT;
    const CameraContextSettings *c = combat ? &s->combat : &s->exploration;

    if (c->override_locked_pitch) {
        if (!c->unlock_pitch) {
            set_field(out, combat ? DEF_COMBAT_PITCH_CLOSE : DEF_PITCH_CLOSE, c->locked_pitch_close);
            set_field(out, combat ? DEF_COMBAT_PITCH_FAR : DEF_PITCH_FAR, c->locked_pitch_far);
            set_field(out, DEF_TACTICAL_PITCH_CLOSE, c->locked_tactical_pitch_close);
            set_field(out, DEF_TACTICAL_PITCH_FAR, c->locked_tactical_pitch_far);
            set_field(out, DEF_ALT_PITCH_CLOSE, c->locked_alt_pitch_close);
            set_field(out, DEF_ALT_PITCH_FAR, c->locked_alt_pitch_far);
        } else if (c->keep_tactical_pitch_locked) {
            set_field(out, DEF_TACTICAL_PITCH_CLOSE, c->locked_tactical_pitch_close);
            set_field(out, DEF_TACTICAL_PITCH_FAR, c->locked_tactical_pitch_far);
        }
    }
    if (c->override_zoom) {
        set_field(out, DEF_MIN_ZOOM, c->zoom_min);
        set_field(out, DEF_MAX_ZOOM, c->zoom_max);
        set_field(out, DEF_TACTICAL_MIN_ZOOM, c->tactical_zoom_min);
        set_field(out, DEF_TACTICAL_MAX_ZOOM, c->tactical_zoom_max);
        set_field(out, DEF_ALT_MIN_ZOOM, c->alt_zoom_min);
        set_field(out, DEF_ALT_MAX_ZOOM, c->alt_zoom_max);
        /* Two or more camera targets (combat framing an attacker and its
         * target, swarm turns) clamp to their own pair, the game's framing
         * range: widened past the player's limits, never narrowed. The game
         * caps every camera's arm at the combat pair's maximum (in
         * UpdateGameCameraBehavior), so that one covers the exploration
         * limits too. */
        const CameraContextSettings *other =
            combat && s->exploration.override_zoom ? &s->exploration : NULL;
        float multi_min, multi_max;
        camera_multi_target_zoom_range(c, other, out[field_index(DEF_MULTI_MIN_ZOOM)],
                                       out[field_index(DEF_MULTI_MAX_ZOOM)],
                                       &multi_min, &multi_max);
        set_field(out, DEF_MULTI_MIN_ZOOM, multi_min);
        set_field(out, DEF_MULTI_MAX_ZOOM, multi_max);
    }
    if (c->override_fov) {
        set_field(out, DEF_FOV_CLOSE, c->fov_close);
        set_field(out, DEF_FOV_FAR, c->fov_far);
        set_field(out, DEF_TACTICAL_FOV, c->tactical_fov);
        set_field(out, DEF_ALT_FOV_CLOSE, c->alt_fov_close);
        set_field(out, DEF_ALT_FOV_FAR, c->alt_fov_far);
    }
    if (c->override_offset) {
        set_field(out, DEF_VERTICAL_OFFSET, c->vertical_offset_mult);
        set_field(out, DEF_HORIZONTAL_OFFSET, c->horizontal_offset_mult);
    }
}

/* Called from the client thread before UpdateGameCameraBehavior. Writes the
 * overrides when settings changed, and notices when the game reloaded its
 * own values (they then become the new baseline). */
static void sync_camera_definitions(void) {
    if (s_def_refused) return;
    uint8_t *gs = global_switches();
    if (!gs) return;
    uint8_t *defs[DEF_COUNT] = {
        gs + s_off->gs_exploration_def, gs + s_off->gs_combat_def,
    };

    float live[DEF_COUNT][DEF_FIELD_COUNT];
    for (int d = 0; d < DEF_COUNT; d++)
        for (size_t i = 0; i < DEF_FIELD_COUNT; i++)
            memcpy(&live[d][i], defs[d] + g_def_fields[i], sizeof(float));

    bool dirty = atomic_exchange(&s_settings_dirty, false);
    if (!s_def_have_snapshot) {
        for (int d = 0; d < DEF_COUNT; d++) {
            if (!snapshot_plausible(live[d])) {
                LOG_GAME_WARN("[CameraTweaks] Camera definition %d looks wrong "
                              "(zoom %.2f..%.2f) — not writing camera definitions",
                              d, live[d][field_index(DEF_MIN_ZOOM)],
                              live[d][field_index(DEF_MAX_ZOOM)]);
                s_def_refused = true;
                return;
            }
        }
        memcpy(s_def_snapshot, live, sizeof(live));
        s_def_have_snapshot = true;
        dirty = true;
        LOG_GAME_INFO("[CameraTweaks] Game camera: exploration zoom %.2f..%.2f "
                      "(multi-target %.2f..%.2f), combat zoom %.2f..%.2f "
                      "(multi-target %.2f..%.2f)",
                      live[0][field_index(DEF_MIN_ZOOM)], live[0][field_index(DEF_MAX_ZOOM)],
                      live[0][field_index(DEF_MULTI_MIN_ZOOM)],
                      live[0][field_index(DEF_MULTI_MAX_ZOOM)],
                      live[1][field_index(DEF_MIN_ZOOM)], live[1][field_index(DEF_MAX_ZOOM)],
                      live[1][field_index(DEF_MULTI_MIN_ZOOM)],
                      live[1][field_index(DEF_MULTI_MAX_ZOOM)]);
    } else {
        for (int d = 0; d < DEF_COUNT; d++)
            for (size_t i = 0; i < DEF_FIELD_COUNT; i++)
                if (memcmp(&live[d][i], &s_def_written[d][i], sizeof(float)) != 0) {
                    s_def_snapshot[d][i] = live[d][i];
                    dirty = true;
                }
    }
    if (!dirty) return;

    const CameraSettings *s = settings();
    for (int d = 0; d < DEF_COUNT; d++) {
        desired_values(d, s, s_def_desired[d]);
        memcpy(s_def_written[d], s_def_desired[d], sizeof(s_def_written[d]));
        for (size_t i = 0; i < DEF_FIELD_COUNT; i++)
            memcpy(defs[d] + g_def_fields[i], &s_def_written[d][i], sizeof(float));
    }
}

/* KeepZoom raises the multi-target minimum of the definition in use to the
 * player's zoom while two or more targets are framed: the game's fit and the
 * clamp after the pitch site may then zoom out to show them all, but not in
 * past the player. NAN restores the settings' value. s_def_written moves with
 * it, so sync_camera_definitions does not take it for a game reload. */
static void set_multi_target_floor(const uint8_t *def, float floor) {
    if (s_def_refused || !s_def_have_snapshot) return;
    uint8_t *gs = global_switches();
    if (!gs) return;
    uint8_t *defs[DEF_COUNT] = {
        gs + s_off->gs_exploration_def, gs + s_off->gs_combat_def,
    };
    int lo_i = field_index(DEF_MULTI_MIN_ZOOM), hi_i = field_index(DEF_MULTI_MAX_ZOOM);
    for (int d = 0; d < DEF_COUNT; d++) {
        float want = s_def_desired[d][lo_i];
        if (defs[d] == def && isfinite(floor))
            want = fminf(fmaxf(floor, want), s_def_desired[d][hi_i]);
        if (memcmp(&want, &s_def_written[d][lo_i], sizeof(want)) == 0) continue;
        memcpy(defs[d] + DEF_MULTI_MIN_ZOOM, &want, sizeof(want));
        s_def_written[d][lo_i] = want;
    }
}

/* Upstream GetCurrentCameraDefinition, inlined in GetCameraPitchDegrees. */
static uint8_t *current_definition(uint32_t mode_flags) {
    uint8_t *gs = global_switches();
    if (!gs) return NULL;
    if (gs[s_off->gs_override_flag]) return gs + s_off->gs_override_def;
    return gs + ((mode_flags & CAMERA_MODE_FLAG_COMBAT)
                 ? s_off->gs_combat_def : s_off->gs_exploration_def);
}

/* ---- zoom modifier (L3 / R3) --------------------------------------------- */

static bool controller_mode(void) {
    return *(volatile uint8_t *)(s_base + s_off->controller_mode_flag) != 0;
}

static bool zoom_modifier_held(const CameraSettings *s, int8_t player) {
    bool use_r3 = s->use_right_stick_press_for_zoom;
    bool held = false;
    if (s->zoom_modifier_source != CAMERA_MODIFIER_GAME)
        held = camera_gamepad_stick_pressed(use_r3 ? CAMERA_STICK_RIGHT : CAMERA_STICK_LEFT, NULL);
    /* The game-side ToggleInputMode value exists only for L3. */
    if (!held && !use_r3 && s->zoom_modifier_source != CAMERA_MODIFIER_GAMEPAD && player >= 0) {
        void *manager = *(void *volatile *)(s_base + s_off->input_manager_ptr);
        if (manager) {
            InputValueRet r = s_get_input_value(manager, INPUT_ID_TOGGLE_INPUT, player);
            held = (r.pressed & 0xff) != 0;
        }
    }
    return held;
}

/* ---- zoom limits, smoothing and floor guard ------------------------------ */

static float read_f32(const uint8_t *p) {
    float v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static void write_f32(uint8_t *p, float v) {
    memcpy(p, &v, sizeof(v));
}

/* GameControl's camera distance override (SetCameraDistanceOverride; combat
 * framing sets it, 0 = none). */
static float distance_override(void) {
    if (!s_off->game_control_ptr) return 0.0f;
    uint8_t *gc = *(uint8_t *volatile *)(s_base + s_off->game_control_ptr);
    if (!gc) return 0.0f;
    float v = read_f32(gc + s_off->game_control_distance_override);
    return isfinite(v) && v > 0.0f ? v : 0.0f;
}

static uint32_t target_count(const uint8_t *b) {
    uint32_t n;
    memcpy(&n, b + BEHAVIOR_TARGET_COUNT, sizeof(n));
    return n;
}

/* The range UpdateGameCameraBehavior clamps the zoom target to:
 * tactical fields in tactical view, the multi-target pair with two or more
 * camera targets, Alt fields in controller mode with the alt option. Outside
 * tactical view the maximum is raised to GameControl's distance override. */
static bool zoom_range(const uint8_t *b, uint32_t mode_flags, const uint8_t **def_out,
                       float *lo, float *hi) {
    uint8_t *gs = global_switches();
    const uint8_t *def = current_definition(mode_flags);
    if (!gs || !def) return false;
    uint32_t min_off = DEF_MIN_ZOOM, max_off = DEF_MAX_ZOOM;
    bool tactical = (mode_flags & CAMERA_MODE_FLAG_TACTICAL) != 0;
    if (tactical) {
        min_off = DEF_TACTICAL_MIN_ZOOM;
        max_off = DEF_TACTICAL_MAX_ZOOM;
    } else if (target_count(b) > 1) {
        min_off = DEF_MULTI_MIN_ZOOM;
        max_off = DEF_MULTI_MAX_ZOOM;
    } else if (controller_mode() && gs[s_off->gs_controller_alt_flag]) {
        min_off = DEF_ALT_MIN_ZOOM;
        max_off = DEF_ALT_MAX_ZOOM;
    }
    *lo = read_f32(def + min_off);
    *hi = read_f32(def + max_off);
    if (!tactical) *hi = fmaxf(*hi, distance_override());
    *def_out = def;
    return isfinite(*lo) && isfinite(*hi) && *lo > 0.0f && *hi >= *lo;
}

static bool floor_query(void *definition, const float pos[3], float *floor_y) {
    bool first = !atomic_exchange(&s_logged_floor_query, true);
    if (first)  /* breadcrumb: the first call through the reconstructed LTO ABI */
        LOG_GAME_INFO("[CameraTweaks] First floor query at (%.2f, %.2f, %.2f)",
                      pos[0], pos[1], pos[2]);
    FloorHeightRet r = s_floor_height(s_frame_world, 0, definition, pos, s_frame_level_manager);
    uint32_t bits = (uint32_t)r.floor;
    memcpy(floor_y, &bits, sizeof(*floor_y));
    if (first)
        LOG_GAME_INFO("[CameraTweaks] First floor query returned %s %.2f",
                      (r.no_data & 0xff) ? "no data" : "floor", *floor_y);
    return !(r.no_data & 0xff);
}

/* Ease the stick zoom rate. OnInputEvent is the only writer of +0xa4 and
 * holds the value between events, so a value that differs from ours is a new
 * stick reading. The game used last frame's +0xa4 before this site runs, so
 * the eased rate takes effect next frame. */
static void smooth_zoom_rate(uint8_t *b, const CameraSettings *s) {
    float raw = read_f32(b + BEHAVIOR_ZOOM_DELTA);
    if (!controller_mode() || !(s->zoom_smoothing > 0.0f) || !isfinite(raw)) {
        s_zoom_rate = s_zoom_rate_target = s_zoom_rate_written = raw;
        return;
    }
    if (raw != s_zoom_rate_written) s_zoom_rate_target = raw;
    s_zoom_rate = camera_smooth_toward(s_zoom_rate, s_zoom_rate_target, s_delta_time,
                                       s->zoom_smoothing);
    if (s_zoom_rate_target == 0.0f && fabsf(s_zoom_rate) < 0.01f) s_zoom_rate = 0.0f;
    write_f32(b + BEHAVIOR_ZOOM_DELTA, s_zoom_rate);
    s_zoom_rate_written = s_zoom_rate;
}

static uint64_t now_ns(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;
}

/* The slot for camera `b`. A new address takes over the most recently used
 * slot when that camera has not been updated in this pass but was a moment
 * ago: the same camera, moved. Otherwise it is a new camera. */
static KeepZoomSlot *keep_zoom_slot(const uint8_t *b) {
    KeepZoomSlot *oldest = &s_keep_slots[0], *newest = NULL;
    for (int i = 0; i < KEEP_ZOOM_SLOTS; i++) {
        KeepZoomSlot *s = &s_keep_slots[i];
        if (s->behavior == b) return s;
        if (s->used < oldest->used) oldest = s;
        if (s->behavior && (!newest || s->used > newest->used)) newest = s;
    }
    uint64_t now = now_ns();
    if (newest && now - newest->last_ns > KEEP_ZOOM_PASS_NS &&
        now - newest->last_ns < KEEP_ZOOM_ALIVE_NS) {
        if (settings()->log_camera_events)
            LOG_CAMERA_EVENT("camera %p moved to %p", (const void *)newest->behavior,
                             (const void *)b);
        newest->behavior = b;
        return newest;
    }
    memset(oldest, 0, sizeof(*oldest));
    oldest->behavior = b;
    oldest->fresh = true;
    oldest->frame_end = NAN;
    oldest->frame_end_yaw = NAN;
    return oldest;
}

/* KeepZoom for the context in use (camera_keep_zoom_step). Tactical view has
 * its own zoom: the player's is held until it ends. `rate` is the zoom rate
 * the game stepped by this frame (before smooth_zoom_rate rewrites it).
 * Returns the zoom target. */
static float keep_zoom(uint8_t *b, uint32_t flags, const uint8_t *def, float target,
                       float rate, float lo, float hi, uint32_t targets) {
    const CameraSettings *s = settings();
    const CameraContextSettings *c =
        (flags & CAMERA_MODE_FLAG_COMBAT) ? &s->combat : &s->exploration;
    KeepZoomSlot *slot = keep_zoom_slot(b);
    slot->used = ++s_keep_clock;
    slot->last_ns = now_ns();
    bool reset = atomic_exchange(&s_camera_center_input, false);

    if (s->log_camera_events) {
        if (!slot->seen || slot->seen_flags != flags || slot->seen_targets != targets)
            LOG_CAMERA_EVENT("camera %p flags 0x%x, %u target(s), zoom %.2f, range %.2f..%.2f, "
                             "distance override %.2f", (const void *)b, flags, targets,
                             target, lo, hi, distance_override());
        slot->seen = true;
        slot->seen_flags = flags;
        slot->seen_targets = targets;
        /* What the player's zoom input would have done since the last frame. */
        float expect = slot->frame_end + rate * s_delta_time;
        if (isfinite(slot->frame_end) && fabsf(target - expect) > 0.05f)
            LOG_CAMERA_EVENT("camera %p: the game set zoom %.2f (was %.2f, flags 0x%x, "
                             "%u target(s)%s)", (const void *)b, target, slot->frame_end,
                             flags, targets, reset ? ", Center Camera" : "");
        float yaw = read_f32(b + BEHAVIOR_YAW);
        float turn_input = fabsf(read_f32(b + BEHAVIOR_ANGLE_DELTA)) +
                           fabsf(read_f32(b + BEHAVIOR_MOUSE_ROTATE));
        bool turned = isfinite(slot->frame_end_yaw) && turn_input < 1e-6f &&
                      fabsf(yaw - slot->frame_end_yaw) > 1e-3f;
        if (turned && !slot->turning) {
            slot->turning = true;
            slot->turn_from = slot->frame_end_yaw;
        } else if (!turned && slot->turning) {
            slot->turning = false;
            LOG_CAMERA_EVENT("camera %p: the game turned the camera, yaw %.3f -> %.3f "
                             "(flags 0x%x)", (const void *)b, slot->turn_from, yaw, flags);
        }
    }

    float floor = NAN, out = target;
    if (flags & CAMERA_MODE_FLAG_TACTICAL) {
        /* hold */
    } else if (!c->keep_zoom) {
        slot->keep.have = false;   /* turning it on starts from the zoom on screen */
    } else {
        out = camera_keep_zoom_step(&slot->keep, target, slot->frame_end, rate,
                                    s_delta_time, targets > 1, reset, lo, hi, &floor);
        if (fabsf(out - target) > 0.01f && !atomic_exchange(&s_logged_keep_zoom, true))
            LOG_GAME_INFO("[CameraTweaks] KeepZoom: the game set zoom %.2f; kept %.2f "
                          "(flags 0x%x)", target, out, flags);
        if (isfinite(floor) && !atomic_exchange(&s_logged_keep_floor, true))
            LOG_GAME_INFO("[CameraTweaks] KeepZoom: %u targets framed; the fit keeps zoom "
                          ">= %.2f (range %.2f..%.2f)", targets, floor, lo, hi);
    }
    set_multi_target_floor(def, floor);
    return out;
}

/* Runs at the pitch site, before the game's arm-collision check and before it
 * eases +0x58 toward +0x5c: caps the zoom target to the active range (the
 * game only stops zooming once past the maximum, and keeps a zoom left over
 * from a wider range, e.g. after tactical view), shrinks the cap near the
 * horizon, and keeps the camera above the floor (upstream AfterUpdateCameraZoom). */
static void adjust_zoom(uint8_t *b, uint32_t flags, float pitch, bool unlocked) {
    const CameraSettings *s = settings();
    float applied_rate = read_f32(b + BEHAVIOR_ZOOM_DELTA);
    smooth_zoom_rate(b, s);

    const uint8_t *def = NULL;
    float lo, hi;
    if (!zoom_range(b, flags, &def, &lo, &hi)) { s_zoom_guarded = false; return; }
    uint32_t targets = target_count(b);
    if (targets > 1 && !(flags & CAMERA_MODE_FLAG_TACTICAL) &&
        !atomic_exchange(&s_logged_multi_target, true))
        LOG_GAME_INFO("[CameraTweaks] First multi-target frame: %u targets, zoom range "
                      "%.2f..%.2f (distance override %.2f, flags 0x%x)",
                      targets, lo, hi, distance_override(), flags);

    float stored = read_f32(b + BEHAVIOR_ZOOM_TARGET);
    float target = keep_zoom(b, flags, def, stored, applied_rate, lo, hi, targets);
    float shown = read_f32(b + BEHAVIOR_ZOOM_SHOWN);

    float cap = unlocked ? camera_low_pitch_zoom_cap(s, pitch, hi) : hi;
    if (cap < lo) cap = lo;

    /* While the guard holds the zoom in, carry the player's stick changes
     * over to the zoom they asked for, so it comes back when the floor drops.
     * Only a change the zoom rate could have made this frame is carried; a
     * bigger one (the game reset or re-clamped the zoom) starts over from it. */
    float intent = target;
    if (s_zoom_guarded) {
        float rate = fmaxf(fabsf(s_zoom_rate_written), fabsf(read_f32(b + BEHAVIOR_ZOOM_DELTA)));
        float change = target - s_zoom_written;
        if (fabsf(change) <= rate * s_delta_time * 1.5f + 0.01f)
            intent = s_zoom_intent + change;
    }
    if (!isfinite(intent)) intent = cap;
    if (intent > cap) {
        if (!atomic_exchange(&s_logged_zoom_cap, true))
            LOG_GAME_INFO("[CameraTweaks] Zoom %.2f capped to %.2f (range %.2f..%.2f, pitch %.1f)",
                          intent, cap, lo, hi, pitch);
        intent = cap;
    }

    float result = intent;
    if (unlocked && s->unlocked_pitch_limit_clipping && s_floor_height &&
        s_frame_world && s_frame_level_manager) {
        float origin[3], dir[3];
        memcpy(origin, b + BEHAVIOR_ARM_ORIGIN, sizeof(origin));
        memcpy(dir, b + BEHAVIOR_ARM_DIR, sizeof(dir));
        bool sane = true;
        for (int i = 0; i < 3; i++) sane = sane && isfinite(origin[i]) && isfinite(dir[i]);
        if (sane)
            result = camera_floor_guard(origin, dir, intent, lo, s->unlocked_pitch_floor_offset,
                                        floor_query, (void *)def);
    }

    if (result < intent) {
        if (!atomic_exchange(&s_logged_floor_guard, true))
            LOG_GAME_INFO("[CameraTweaks] Floor guard: zoom %.2f -> %.2f (pitch %.1f)",
                          intent, result, pitch);
        s_zoom_guarded = true;
        s_zoom_intent = intent;
        s_zoom_written = result;
        /* Pull in now; easing in would dip under the floor for a few frames. */
        if (!(shown <= result)) write_f32(b + BEHAVIOR_ZOOM_SHOWN, result);
    } else {
        s_zoom_guarded = false;
        if (!isfinite(shown)) write_f32(b + BEHAVIOR_ZOOM_SHOWN, result);
    }
    if (result != stored) write_f32(b + BEHAVIOR_ZOOM_TARGET, result);
}

/* ---- hooks --------------------------------------------------------------- */

static uint64_t hook_update_gcb(void *self, void *world, void *game_time, void *view) {
    float dt = 0.0f;
    if (game_time) memcpy(&dt, (uint8_t *)game_time + GAME_TIME_DELTA, sizeof(dt));
    if (!isfinite(dt) || dt < 0.0f) dt = 0.0f;
    if (dt > MAX_DELTA_TIME) dt = MAX_DELTA_TIME;
    s_delta_time = dt;

    maybe_reload_settings();
    sync_camera_definitions();

    /* Upstream Hook_UpdateCameraPitch: while our pitch drives the camera,
     * let the game's pitch interpolation reach it in one step. */
    uint8_t *def = NULL;
    float saved[3];
    if (s_pitch_hooked && atomic_load(&s_pitch_active))
        def = current_definition(atomic_load(&s_last_mode_flags));
    if (def) {
        memcpy(&saved[0], def + DEF_PITCH_SPEED_A, sizeof(float));
        memcpy(&saved[1], def + DEF_PITCH_SPEED_B, sizeof(float));
        memcpy(&saved[2], def + DEF_PITCH_SPEED_C, sizeof(float));
        float fast = PITCH_SPEED_UNLOCKED;
        memcpy(def + DEF_PITCH_SPEED_A, &fast, sizeof(float));
        memcpy(def + DEF_PITCH_SPEED_B, &fast, sizeof(float));
        memcpy(def + DEF_PITCH_SPEED_C, &fast, sizeof(float));
    }

    s_frame_world = world;
    s_frame_level_manager = NULL;
    if (self && s_off->camera_system_level_manager) {
        void *lm = NULL;
        if (safe_memory_read((mach_vm_address_t)((uintptr_t)self + s_off->camera_system_level_manager),
                             &lm, sizeof(lm)))
            s_frame_level_manager = lm;
    }

    s_frame_behavior = NULL;
    uint64_t r = s_orig_update_gcb(self, world, game_time, view);
    s_frame_world = NULL;
    s_frame_level_manager = NULL;
    if (s_frame_behavior) {
        /* KeepZoom: the zoom this camera ends the frame with (after the
         * multi-target fit and the clamp), the baseline for its next frame. */
        float end = NAN;
        if (!safe_memory_read((mach_vm_address_t)(uintptr_t)(s_frame_behavior +
                                                             BEHAVIOR_ZOOM_TARGET),
                              &end, sizeof(end)))
            end = NAN;
        KeepZoomSlot *slot = keep_zoom_slot(s_frame_behavior);
        slot->frame_end = end;
        if (isfinite(end)) s_last_zoom = end;
        if (settings()->log_camera_events) {
            float yaw = NAN;
            if (!safe_memory_read((mach_vm_address_t)(uintptr_t)(s_frame_behavior +
                                                                 BEHAVIOR_YAW),
                                  &yaw, sizeof(yaw)))
                yaw = NAN;
            slot->frame_end_yaw = yaw;
        }
        s_frame_behavior = NULL;
    }

    if (def) {
        memcpy(def + DEF_PITCH_SPEED_A, &saved[0], sizeof(float));
        memcpy(def + DEF_PITCH_SPEED_B, &saved[1], sizeof(float));
        memcpy(def + DEF_PITCH_SPEED_C, &saved[2], sizeof(float));
    }
    return r;
}

/* Upstream Hook_SDLMouseYHook: every mouse Y movement the game reads. */
static double hook_mouse_delta_y(void *event) {
    double dy = s_orig_mouse_dy(event);
    if (isfinite(dy) && dy != 0.0) {
        float cur = atomic_load(&s_mouse_dy);
        while (!atomic_compare_exchange_weak(&s_mouse_dy, &cur, cur + (float)dy)) {}
    }
    return dy;
}

static float take_mouse_dy(void) {
    float dy = atomic_exchange(&s_mouse_dy, 0.0f);
    return fmaxf(-MOUSE_DY_MAX_PER_FRAME, fminf(dy, MOUSE_DY_MAX_PER_FRAME));
}

/* Upstream Hook_SetDefaultZoom keeps desiredZoom (+0x5c) and currentZoom_160
 * when the game resets the camera on a zone change. On macOS a zone change or
 * a load in game makes a new GameCameraBehavior with the game's default zoom,
 * so its first frame takes the zoom the last camera ended with. +0x58 too:
 * otherwise the new camera's zoom on screen eases out from the default. */
static void carry_zoom_to_new_camera(uint8_t *b) {
    if (settings()->reset_zoom_on_zone_change || !isfinite(s_last_zoom)) return;
    float was = read_f32(b + BEHAVIOR_ZOOM_TARGET);
    write_f32(b + BEHAVIOR_ZOOM_TARGET, s_last_zoom);
    write_f32(b + BEHAVIOR_ZOOM_SAVED, s_last_zoom);
    write_f32(b + BEHAVIOR_ZOOM_SHOWN, s_last_zoom);
    LOG_GAME_INFO("[CameraTweaks] New camera %p: kept zoom %.2f (the game set %.2f)",
                  (void *)b, s_last_zoom, was);
}

static float hook_pitch_site(void *behavior, bool a1, bool a2) {
    float pitch = s_orig_pitch(behavior, a1, a2);
    if (!behavior) return pitch;
    uint8_t *b = (uint8_t *)behavior;
    s_frame_behavior = b;
    float mouse_dy = take_mouse_dy();

    KeepZoomSlot *slot = keep_zoom_slot(b);
    slot->last_ns = now_ns();
    if (slot->fresh) {
        slot->fresh = false;
        carry_zoom_to_new_camera(b);
    }

    if (b[BEHAVIOR_FORCED_PITCH] && !a1) {
        /* The game pins this camera's pitch (GetCameraPitchDegrees early-out). */
        if (settings()->log_camera_events && !slot->pinned)
            LOG_CAMERA_EVENT("camera %p: the game pins the pitch at %.2f", behavior, pitch);
        slot->pinned = true;
        if (!s_logged_forced_pitch) {
            LOG_GAME_INFO("[CameraTweaks] Camera pitch is pinned by the game; leaving it");
            s_logged_forced_pitch = true;
        }
        atomic_store(&s_pitch_active, false);
        /* When the game lets go, ease from its pitch back to ours. */
        if (s_pitch.has_pitch) s_pitch.resync = true;
        return pitch;
    }

    if (slot->pinned) {
        slot->pinned = false;
        if (settings()->log_camera_events)
            LOG_CAMERA_EVENT("camera %p: the game released the pitch (%.2f)", behavior, pitch);
    }

    uint32_t flags;
    memcpy(&flags, b + BEHAVIOR_MODE_FLAGS, sizeof(flags));
    atomic_store(&s_last_mode_flags, flags);

    float out = pitch;
    s_pitch.mouse_dy = mouse_dy;
    bool unlocked = camera_calculate_pitch(settings(), &s_pitch, flags, s_delta_time,
                                           pitch, &out);
    if (mouse_dy != 0.0f && (flags & CAMERA_MODE_FLAG_MOUSE_ROTATION) && unlocked &&
        !atomic_exchange(&s_logged_mouse_pitch, true))
        LOG_GAME_INFO("[CameraTweaks] First mouse pitch: %.1f points -> pitch %.2f",
                      mouse_dy, out);
    atomic_store(&s_pitch_active, s_pitch.has_pitch);
    adjust_zoom(b, flags, out, unlocked);
    if (!atomic_exchange(&s_logged_pitch, true))
        LOG_GAME_INFO("[CameraTweaks] First pitch frame: game %.2f -> %.2f (%s, flags 0x%x, "
                      "dt %.4f)", pitch, out, unlocked ? "unlocked" : "locked", flags,
                      s_delta_time);
    return out;
}

static uint64_t hook_camera_on_input(void *self, void *entity_ref, const void *event) {
    if (!event) return s_orig_camera_input(self, entity_ref, event);
    const uint8_t *ev = (const uint8_t *)event;
    uint32_t id;
    memcpy(&id, ev + INPUT_EVENT_ID, sizeof(id));
    if (id == INPUT_ID_CAMERA_CENTER) {
        /* Center Camera resets the zoom: KeepZoom takes the new one. Flag it
         * once the reset is written (input may run off the client thread). */
        uint64_t r = s_orig_camera_input(self, entity_ref, event);
        atomic_store(&s_camera_center_input, true);
        return r;
    }
    if (id < INPUT_ID_ZOOM_IN || id > INPUT_ID_MOUSE_ROTATE_R || id == 106)
        return s_orig_camera_input(self, entity_ref, event);

    const CameraSettings *s = settings();
    uint8_t copy[INPUT_EVENT_COPY_SIZE];
    memcpy(copy, ev, sizeof(copy));
    float value, value2;
    memcpy(&value, copy + INPUT_EVENT_VALUE, sizeof(float));
    memcpy(&value2, copy + INPUT_EVENT_VALUE2, sizeof(float));
    bool controller = controller_mode();
    if (!atomic_exchange(&s_logged_input, true))
        LOG_GAME_INFO("[CameraTweaks] First camera input: id %u value %.3f value2 %.3f "
                      "pressed %u player %d (%s)", id, value, value2, ev[INPUT_EVENT_PRESSED],
                      (int8_t)ev[INPUT_EVENT_PLAYER], controller ? "controller" : "mouse/keyboard");

    switch (id) {
    case INPUT_ID_ZOOM_IN:
    case INPUT_ID_ZOOM_OUT:
        if (controller) {
            CameraMode mode = camera_mode_from_flags(atomic_load(&s_last_mode_flags));
            bool can_pitch = s_pitch_hooked && camera_can_adjust_pitch(s, mode);
            bool modifier = can_pitch && zoom_modifier_held(s, (int8_t)ev[INPUT_EVENT_PLAYER]);
            bool do_zoom = !can_pitch || (s->swap_zoom_and_pitch ? !modifier : modifier);
            if (modifier && !atomic_exchange(&s_logged_zoom_modifier, true))
                LOG_GAME_INFO("[CameraTweaks] Zoom modifier (%s) seen held",
                              s->use_right_stick_press_for_zoom ? "R3" : "L3");

            bool *engaged = &s_stick_engaged[id - INPUT_ID_ZOOM_IN];
            float zoom = camera_adjust_zoom_input(s, value2, engaged);
            if (can_pitch && modifier && !s->use_right_stick_press_for_zoom && zoom > 0.0f) {
                /* Zooming with L3 held: its release must not toggle input mode. */
                atomic_store(&s_skip_toggle_input, true);
            }

            if (do_zoom) {
                s_pitch.controller_pitch_delta = 0.0f;
                memcpy(copy + INPUT_EVENT_VALUE2, &zoom, sizeof(float));
                /* The game ignores the stick unless the event is "pressed". */
                if (s->apply_deadzone_to_zoom && zoom > CAMERA_VANILLA_DEADZONE)
                    copy[INPUT_EVENT_PRESSED] = 1;
            } else {
                float sign = id == INPUT_ID_ZOOM_IN ? -1.0f : 1.0f;
                s_pitch.controller_pitch_delta =
                    camera_controller_pitch_delta(s, value2 * sign, engaged);
                float zero = 0.0f;   /* below 0.65: the game zeroes its zoom delta */
                memcpy(copy + INPUT_EVENT_VALUE2, &zero, sizeof(float));
            }
        } else {
            value *= s->mouse_zoom_mult;
            memcpy(copy + INPUT_EVENT_VALUE, &value, sizeof(float));
        }
        break;

    case INPUT_ID_ROTATE_LEFT:
    case INPUT_ID_ROTATE_RIGHT:
        if (controller) {
            value2 = camera_adjust_rotation_input(s, value2, true);
            memcpy(copy + INPUT_EVENT_VALUE2, &value2, sizeof(float));
        } else {
            value *= s->keyboard_camera_rotation_mult;
            memcpy(copy + INPUT_EVENT_VALUE, &value, sizeof(float));
        }
        break;

    case INPUT_ID_MOUSE_ROTATE_L:
    case INPUT_ID_MOUSE_ROTATE_R:
        value *= s->mouse_camera_rotation_mult;
        memcpy(copy + INPUT_EVENT_VALUE, &value, sizeof(float));
        break;
    }
    return s_orig_camera_input(self, entity_ref, copy);
}

static uint64_t hook_client_on_input(void *self, const void *event) {
    if (event) {
        const uint8_t *ev = (const uint8_t *)event;
        uint32_t id;
        memcpy(&id, ev + INPUT_EVENT_ID, sizeof(id));
        if (id == INPUT_ID_TOGGLE_INPUT && atomic_load(&s_skip_toggle_input)) {
            atomic_store(&s_skip_toggle_input, false);
            /* A press means the release that should have been eaten was lost;
             * only the release (state 0) performs the toggle. */
            if (ev[INPUT_EVENT_PRESSED] == 0) {
                if (!atomic_exchange(&s_logged_toggle_skip, true))
                    LOG_GAME_INFO("[CameraTweaks] Swallowed the L3 release after a zoom");
                return 0;
            }
        }
    }
    return s_orig_client_input(self, event);
}

/* ---- install ------------------------------------------------------------- */

static bool patch_call_site(const char *name, const CodePatchSite *site, void *replacement) {
    void *addr = (void *)(s_base + site->site);
    char why[160] = "";
    void *pad = bg3mse_jump_pad(addr, replacement, why, sizeof(why));
    uint32_t word = pad ? camera_encode_bl((uint64_t)(uintptr_t)addr, (uint64_t)(uintptr_t)pad) : 0;
    if (pad && !word) snprintf(why, sizeof(why), "the jump pad is out of branch range");
    if (!word || !bg3mse_code_write_word(addr, word, why, sizeof(why))) {
        LOG_GAME_ERROR("[CameraTweaks] %s: could not patch %p: %s", name, addr, why);
        return false;
    }
    return true;
}

bool camera_tweaks_is_active(void) {
    return s_active;
}

bool camera_tweaks_init(void) {
    if (s_active) return true;

    snprintf(s_config_path, sizeof(s_config_path), "%s/%s",
             camera_data_dir(), CAMERA_SETTINGS_FILENAME);
    load_settings(true);

    if (!settings()->enabled) {
        LOG_GAME_INFO("[CameraTweaks] Disabled (set Enabled = true in %s)", s_config_path);
        return false;
    }

    /* Find and check everything before writing anything. */
    static CameraTweaksOffsets row;
    char why[256] = "";
    if (!camera_sites_resolve(&row, &s_base, &s_global_switches_slot, why, sizeof(why))) {
        LOG_GAME_WARN("[CameraTweaks] Disabled: %s", why);
        return false;
    }
    s_off = &row;
    bool pitch_ok = s_off->pitch_call.site != 0;
    bool camera_input_ok = s_off->camera_on_input_fn && s_off->get_input_value_fn &&
        s_off->input_manager_ptr && s_off->controller_mode_flag;
    bool client_input_ok = s_off->client_on_input_fn != 0;
    bool mouse_dy_ok = s_off->mouse_dy_fn && s_off->mouse_dy_call.site;

    s_orig_update_gcb = (UpdateGcbFn)(s_base + s_off->update_gcb_fn);
    s_orig_pitch = (PitchFn)(s_base + s_off->pitch_fn);
    s_get_input_value = (GetInputValueFn)(s_base + s_off->get_input_value_fn);
    if (s_off->floor_height_fn && s_off->camera_system_level_manager)
        s_floor_height = (FloorHeightFn)(s_base + s_off->floor_height_fn);

    if (!patch_call_site("UpdateGameCameraBehavior call", &s_off->update_gcb_call,
                         (void *)hook_update_gcb))
        return false;
    s_active = true;

    if (pitch_ok)
        s_pitch_hooked = patch_call_site("GetCameraPitchDegrees call", &s_off->pitch_call,
                                         (void *)hook_pitch_site);
    bool mouse_pitch_hooked = false;
    if (s_pitch_hooked && mouse_dy_ok) {
        s_orig_mouse_dy = (MouseDeltaFn)(s_base + s_off->mouse_dy_fn);
        mouse_pitch_hooked = patch_call_site("mouseMoved: deltaY call", &s_off->mouse_dy_call,
                                             (void *)hook_mouse_delta_y);
    }

    /* Each hook sets its original before its patch goes live. */
    bool camera_input_hooked = false;
    if (camera_input_ok) {
        char why[256] = "";
        camera_input_hooked = bg3mse_entry_hook((void *)(s_base + s_off->camera_on_input_fn),
                                                s_off->camera_on_input_bytes, (void *)hook_camera_on_input,
                                                (void **)&s_orig_camera_input, why, sizeof(why)) != NULL;
        if (!camera_input_hooked) LOG_GAME_ERROR("[CameraTweaks] CameraSystem::OnInputEvent: %s", why);
    }
    bool client_input_hooked = false;
    if (client_input_ok && camera_input_hooked) {
        char why[256] = "";
        client_input_hooked = bg3mse_entry_hook((void *)(s_base + s_off->client_on_input_fn),
                                                s_off->client_on_input_bytes, (void *)hook_client_on_input,
                                                (void **)&s_orig_client_input, why, sizeof(why)) != NULL;
        if (!client_input_hooked) LOG_GAME_ERROR("[CameraTweaks] EocClient::OnInputEvent: %s", why);
    }

    LOG_GAME_INFO("[CameraTweaks] Active: definitions+update yes, pitch %s, "
                  "zoom limits %s, floor guard %s, camera input %s, L3 toggle guard %s, "
                  "mouse pitch %s (settings: %s)",
                  s_pitch_hooked ? "yes" : "NO", s_pitch_hooked ? "yes" : "NO",
                  s_pitch_hooked && s_floor_height ? "yes" : "NO",
                  camera_input_hooked ? "yes" : "NO", client_input_hooked ? "yes" : "NO",
                  mouse_pitch_hooked ? "yes" : "NO", s_config_path);
    return true;
}
