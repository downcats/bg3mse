// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from ersh1's BG3 Native Camera Tweaks: https://github.com/ersh1/BG3_NativeCameraTweaks

/*
 * Tests for the camera tweaks: the settings parser and camera math
 * (Camera/camera_settings.c) and bl encoding (Camera/camera_patch.c).
 * No game process needed.
 */

#include <Camera/camera_patch.h>
#include <Camera/camera_settings.h>

#include <locale.h>
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static jmp_buf g_test_jmp;
static int g_failed;
static int g_run;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    g_run++; \
    if (setjmp(g_test_jmp) == 0) test_##name(); \
    else { g_failed++; printf("FAIL camera %s\n", #name); } \
} while (0)
#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { printf("  assertion failed: %s (line %d)\n", #cond, __LINE__); longjmp(g_test_jmp, 1); } \
} while (0)
#define ASSERT_FALSE(cond)    ASSERT_TRUE(!(cond))
#define ASSERT_EQ(a, b)       ASSERT_TRUE((a) == (b))
#define ASSERT_NE(a, b)       ASSERT_TRUE((a) != (b))
#define ASSERT_STR_EQ(a, b)   ASSERT_TRUE(strcmp((a), (b)) == 0)

#define NEAR(a, b) (fabsf((float)(a) - (float)(b)) < 1e-4f)

/* ---- settings ------------------------------------------------------------ */

TEST(defaults_match_upstream) {
    CameraSettings s;
    camera_settings_defaults(&s);
    ASSERT_FALSE(s.enabled);
    ASSERT_TRUE(NEAR(s.unlocked_pitch_initial_value, 25.0f));
    ASSERT_TRUE(NEAR(s.unlocked_pitch_clamp_speed, 5.0f));
    ASSERT_TRUE(s.exploration.unlock_pitch && s.combat.unlock_pitch);
    ASSERT_TRUE(NEAR(s.exploration.unlocked_pitch_max, 85.0f));
    ASSERT_TRUE(s.exploration.override_zoom);
    /* macOS: tighter zoom and pitch limits than upstream (0.5/50/40, -85) */
    ASSERT_TRUE(NEAR(s.exploration.zoom_min, 2.0f) && NEAR(s.exploration.zoom_max, 20.0f));
    ASSERT_TRUE(NEAR(s.exploration.alt_zoom_max, 25.0f) && NEAR(s.combat.tactical_zoom_max, 30.0f));
    ASSERT_TRUE(NEAR(s.exploration.unlocked_pitch_min, 10.0f));
    ASSERT_TRUE(s.unlocked_pitch_limit_clipping && NEAR(s.unlocked_pitch_floor_offset, 0.5f));
    ASSERT_TRUE(s.limit_zoom_at_low_pitch && NEAR(s.low_pitch_angle, 30.0f) &&
                NEAR(s.low_pitch_max_zoom, 8.0f));
    ASSERT_TRUE(NEAR(s.pitch_transition_speed, 4.0f) && NEAR(s.zoom_smoothing, 0.15f));
    ASSERT_TRUE(NEAR(s.deadzone_hysteresis, 0.05f));
    ASSERT_TRUE(NEAR(s.combat.locked_pitch_close, 32.73f));
    ASSERT_TRUE(NEAR(s.exploration.locked_pitch_close, 19.05f));
    ASSERT_TRUE(NEAR(s.controller_camera_rotation_mult, 1.25f));  /* macOS: upstream 2 */
    ASSERT_TRUE(NEAR(s.controller_pitch_speed, 90.0f) && NEAR(s.pitch_smoothing, 0.12f));
    ASSERT_TRUE(NEAR(s.controller_zoom_mult, 0.5f));
    ASSERT_TRUE(s.override_right_stick_deadzone && NEAR(s.new_deadzone, 0.15f));
    ASSERT_EQ(s.zoom_modifier_source, CAMERA_MODIFIER_BOTH);
}

TEST(default_file_parses_clean_and_matches_defaults) {
    CameraSettings parsed, def;
    camera_settings_defaults(&def);
    camera_settings_defaults(&parsed);
    parsed.enabled = true;            /* the file leaves the master switch out: it stays */
    parsed.exploration.zoom_max = 99; /* ... and sets every other key it lists */
    int line = -1;
    ASSERT_EQ(camera_settings_parse(&parsed, camera_settings_default_file(), &line), 0);
    ASSERT_EQ(line, 0);
    ASSERT_TRUE(parsed.enabled);
    parsed.enabled = def.enabled;
    ASSERT_EQ(memcmp(&parsed, &def, sizeof(def)), 0);
}

TEST(parse_upstream_keys_and_sections) {
    CameraSettings s;
    camera_settings_defaults(&s);
    const char *text =
        "[General]\n"
        "Enabled = true  # trailing comment\n"
        "UnlockedPitchInitialValue = 30\n"
        "[CombatZoom]\n"
        "CombatZoomMax = 33.5\n"
        "[ExplorationPitch]\n"
        "ExplorationUnlockedPitchMin = -40\n"
        "ExplorationKeepTacticalPitchLocked = true\n"
        "[Controller]\n"
        "SwapZoomAndPitch = true\n"
        "ZoomModifierSource = \"gamepad\"\n";
    ASSERT_EQ(camera_settings_parse(&s, text, NULL), 0);
    ASSERT_TRUE(s.enabled);
    ASSERT_TRUE(NEAR(s.unlocked_pitch_initial_value, 30));
    ASSERT_TRUE(NEAR(s.combat.zoom_max, 33.5f));
    ASSERT_TRUE(NEAR(s.exploration.zoom_max, 20.0f));   /* other context untouched */
    ASSERT_TRUE(NEAR(s.exploration.unlocked_pitch_min, -40));
    ASSERT_TRUE(s.exploration.keep_tactical_pitch_locked);
    ASSERT_TRUE(s.swap_zoom_and_pitch);
    ASSERT_EQ(s.zoom_modifier_source, CAMERA_MODIFIER_GAMEPAD);
}

TEST(parse_rejects_bad_values_and_keeps_previous) {
    CameraSettings s;
    camera_settings_defaults(&s);
    const char *text =
        "[ControllerDeadzone]\n"
        "NewDeadzone = 1.5\n"                 /* out of range */
        "[General]\n"
        "UnlockedPitchClampSpeed = nan\n"     /* non-finite */
        "Enabled = yes\n"                     /* not a TOML bool */
        "[Mouse]\n"
        "CombatZoomMax = 5\n"                 /* key in the wrong section */
        "NoSuchKey = 1\n"
        "[Controller\n";                      /* malformed header */
    int line = 0;
    ASSERT_EQ(camera_settings_parse(&s, text, &line), 6);
    ASSERT_EQ(line, 2);
    ASSERT_TRUE(NEAR(s.new_deadzone, 0.15f));
    ASSERT_TRUE(NEAR(s.unlocked_pitch_clamp_speed, 5.0f));
    ASSERT_FALSE(s.enabled);
    ASSERT_TRUE(NEAR(s.combat.zoom_max, 20.0f));
}

TEST(parse_restores_inverted_ranges) {
    CameraSettings s;
    camera_settings_defaults(&s);
    const char *text =
        "[CombatPitch]\nCombatUnlockedPitchMin = 60\nCombatUnlockedPitchMax = 10\n"
        "[ExplorationZoom]\nExplorationZoomMin = 30\n";
    ASSERT_EQ(camera_settings_parse(&s, text, NULL), 2);
    ASSERT_TRUE(NEAR(s.combat.unlocked_pitch_min, 10) && NEAR(s.combat.unlocked_pitch_max, 85));
    ASSERT_TRUE(NEAR(s.exploration.zoom_min, 2.0f) && NEAR(s.exploration.zoom_max, 20));
}

/* ---- math ---------------------------------------------------------------- */

TEST(mode_flags_and_pitch_unlock) {
    CameraSettings s;
    camera_settings_defaults(&s);
    ASSERT_EQ(camera_mode_from_flags(0), CAMERA_MODE_EXPLORATION);
    ASSERT_EQ(camera_mode_from_flags(CAMERA_MODE_FLAG_TACTICAL), CAMERA_MODE_EXPLORATION_TACTICAL);
    ASSERT_EQ(camera_mode_from_flags(CAMERA_MODE_FLAG_COMBAT | CAMERA_MODE_FLAG_MOUSE_ROTATION),
              CAMERA_MODE_COMBAT);
    ASSERT_EQ(camera_mode_from_flags(CAMERA_MODE_FLAG_COMBAT | CAMERA_MODE_FLAG_TACTICAL),
              CAMERA_MODE_COMBAT_TACTICAL);
    s.combat.keep_tactical_pitch_locked = true;
    ASSERT_TRUE(camera_can_adjust_pitch(&s, CAMERA_MODE_COMBAT));
    ASSERT_FALSE(camera_can_adjust_pitch(&s, CAMERA_MODE_COMBAT_TACTICAL));
    ASSERT_TRUE(camera_can_adjust_pitch(&s, CAMERA_MODE_EXPLORATION_TACTICAL));
}

TEST(stick_deadzone_mapping) {
    CameraSettings s;
    camera_settings_defaults(&s);
    s.controller_camera_rotation_mult = 2.0f;  /* upstream's numbers */
    s.stick_response_curve = 1.0f;
    ASSERT_TRUE(NEAR(camera_deadzone(&s), 0.15f));
    ASSERT_TRUE(NEAR(camera_stick_normalize(0.1f, 0.15f), 0));
    ASSERT_TRUE(NEAR(camera_stick_normalize(-1.0f, 0.15f), 1));
    ASSERT_TRUE(NEAR(camera_adjust_rotation_input(&s, 0.10f, true), 0));
    ASSERT_TRUE(NEAR(camera_adjust_rotation_input(&s, 1.0f, false), 1.0f));
    /* half way past the deadzone, doubled -> full game deflection */
    ASSERT_TRUE(NEAR(camera_adjust_rotation_input(&s, 0.575f, true), 1.0f));
    s.override_right_stick_deadzone = false;
    ASSERT_TRUE(NEAR(camera_deadzone(&s), CAMERA_VANILLA_DEADZONE));
}

TEST(zoom_input_reaches_game_band) {
    CameraSettings s;
    camera_settings_defaults(&s);              /* zoom mult 0.5 */
    ASSERT_TRUE(NEAR(camera_adjust_zoom_input(&s, 0.1f, NULL), 0));
    /* a small deflection (below the game's 0.65) still zooms */
    ASSERT_TRUE(camera_adjust_zoom_input(&s, 0.3f, NULL) > CAMERA_VANILLA_DEADZONE);
    ASSERT_TRUE(NEAR(camera_adjust_zoom_input(&s, 1.0f, NULL), 0.65f + 0.35f * 0.5f));
    /* without the macOS deadzone, upstream's "zoomDelta *= mult" exactly */
    s.apply_deadzone_to_zoom = false;
    ASSERT_TRUE(NEAR(camera_adjust_zoom_input(&s, 0.3f, NULL), 0));
    ASSERT_TRUE(NEAR(camera_adjust_zoom_input(&s, 0.85f, NULL) - 0.65f, (0.85f - 0.65f) * 0.5f));
}

TEST(controller_pitch_delta_sign_and_invert) {
    CameraSettings s;
    camera_settings_defaults(&s);
    ASSERT_TRUE(NEAR(camera_controller_pitch_delta(&s, 0.1f, NULL), 0));
    ASSERT_TRUE(NEAR(camera_controller_pitch_delta(&s, -1.0f, NULL), -1.25f));
    /* the response curve makes half deflection gentler than linear */
    float half = camera_controller_pitch_delta(&s, 0.575f, NULL);
    ASSERT_TRUE(half > 0.0f && half < 0.5f * 1.25f);
    s.invert_controller_pitch = true;
    ASSERT_TRUE(NEAR(camera_controller_pitch_delta(&s, -1.0f, NULL), 1.25f));
}

TEST(interp_to_converges_both_directions) {
    ASSERT_TRUE(NEAR(camera_interp_to(10, 20, 0.1f, 5), 15));
    ASSERT_TRUE(NEAR(camera_interp_to(20, 10, 0.1f, 5), 15));  /* upstream snapped here */
    ASSERT_TRUE(NEAR(camera_interp_to(20, 10, 1.0f, 5), 10));
    ASSERT_TRUE(NEAR(camera_interp_to(3, 7, 0.1f, 0), 7));
}

TEST(pitch_state_machine) {
    CameraSettings s;
    camera_settings_defaults(&s);
    CameraPitchState st = {0};
    float out = 0;

    /* First unlocked frame starts at the initial value, shown via an ease
     * from the game's pitch rather than a snap. */
    ASSERT_TRUE(camera_calculate_pitch(&s, &st, 0, 0.016f, 40.0f, &out));
    ASSERT_TRUE(st.has_pitch && NEAR(st.pitch, 25.0f));
    ASSERT_TRUE(st.blending && out < 40.0f && out > 25.0f);
    for (int i = 0; i < 200 && st.blending; i++)
        camera_calculate_pitch(&s, &st, 0, 0.016f, 40.0f, &out);
    ASSERT_FALSE(st.blending);
    ASSERT_TRUE(NEAR(out, 25.0f));

    /* Stick input moves it: delta * ControllerPitchSpeed * ControllerPitchMult * dt
     * (smoothing off), and with smoothing the rate ramps instead of jumping. */
    s.pitch_smoothing = 0.0f;
    st.controller_pitch_delta = 0.5f;
    camera_calculate_pitch(&s, &st, 0, 0.5f, 40.0f, &out);
    ASSERT_TRUE(NEAR(out, 25.0f + 0.5f * 90.0f * 0.5f * 0.5f));
    s.pitch_smoothing = 0.12f;
    CameraPitchState ramp = { .has_pitch = true, .pitch = 25.0f, .controller_pitch_delta = 1.0f };
    camera_calculate_pitch(&s, &ramp, 0, 0.016f, 40.0f, &out);
    ASSERT_TRUE(ramp.pitch_rate > 0.0f && ramp.pitch_rate < 0.25f * 90.0f * 0.5f);

    /* Never past the limit. */
    st.controller_pitch_delta = 100.0f;
    camera_calculate_pitch(&s, &st, 0, 0.5f, 40.0f, &out);
    ASSERT_TRUE(out <= 85.0f);

    /* Locked mode (exploration unlock off): eases back to the game's pitch,
     * then hands control back. */
    s.exploration.unlock_pitch = false;
    st.controller_pitch_delta = 0;
    float before = st.pitch;
    ASSERT_FALSE(camera_calculate_pitch(&s, &st, 0, 0.1f, 40.0f, &out));
    ASSERT_TRUE(out < before && out > 40.0f);
    for (int i = 0; i < 2000 && st.has_pitch; i++)
        camera_calculate_pitch(&s, &st, 0, 0.1f, 40.0f, &out);
    ASSERT_FALSE(st.has_pitch);
    ASSERT_TRUE(NEAR(out, 40.0f));
}

TEST(pitch_resync_eases_after_pinned_pitch) {
    CameraSettings s;
    camera_settings_defaults(&s);
    CameraPitchState st = { .has_pitch = true, .pitch = 60.0f };
    float out = 0;
    /* A cutscene pinned the pitch at 20; when it lets go we start there. */
    st.resync = true;
    camera_calculate_pitch(&s, &st, 0, 0.016f, 20.0f, &out);
    ASSERT_TRUE(out > 20.0f && out < 30.0f);
    /* PitchTransitionSpeed 0 snaps to the pitch held before, without easing. */
    s.pitch_transition_speed = 0.0f;
    st.resync = true;
    camera_calculate_pitch(&s, &st, 0, 0.016f, 20.0f, &out);
    ASSERT_TRUE(NEAR(out, 60.0f));
}

TEST(deadzone_hysteresis_holds_then_fades) {
    bool engaged = false;
    ASSERT_TRUE(NEAR(camera_stick_normalize_held(0.14f, 0.15f, 0.05f, &engaged), 0));
    ASSERT_FALSE(engaged);
    ASSERT_TRUE(camera_stick_normalize_held(0.2f, 0.15f, 0.05f, &engaged) > 0.0f);
    ASSERT_TRUE(engaged);
    /* inside the deadzone but above the release point: still live, still > 0 */
    float held = camera_stick_normalize_held(0.13f, 0.15f, 0.05f, &engaged);
    ASSERT_TRUE(engaged && held > 0.0f && held < 0.05f);
    ASSERT_TRUE(NEAR(camera_stick_normalize_held(0.09f, 0.15f, 0.05f, &engaged), 0));
    ASSERT_FALSE(engaged);
    /* full deflection stays 1 either way; NULL state = plain deadzone */
    engaged = true;
    ASSERT_TRUE(NEAR(camera_stick_normalize_held(1.0f, 0.15f, 0.05f, &engaged), 1.0f));
    ASSERT_TRUE(NEAR(camera_stick_normalize_held(0.13f, 0.15f, 0.05f, NULL), 0));
}

TEST(zoom_input_uses_response_curve) {
    CameraSettings s;
    camera_settings_defaults(&s);
    float half = camera_adjust_zoom_input(&s, 0.575f, NULL) - CAMERA_VANILLA_DEADZONE;
    float linear = 0.5f * 0.35f * s.controller_zoom_mult;
    ASSERT_TRUE(half > 0.0f && half < linear);
    s.stick_response_curve = 1.0f;
    ASSERT_TRUE(NEAR(camera_adjust_zoom_input(&s, 0.575f, NULL) - CAMERA_VANILLA_DEADZONE, linear));
}

TEST(smooth_toward_is_exponential) {
    ASSERT_TRUE(NEAR(camera_smooth_toward(0, 10, 0.1f, 0), 10));
    float v = camera_smooth_toward(0, 10, 0.1f, 0.1f);
    ASSERT_TRUE(NEAR(v, 10.0f * (1.0f - expf(-1.0f))));
}

TEST(low_pitch_zoom_cap) {
    CameraSettings s;
    camera_settings_defaults(&s);   /* 30 degrees, 8 at the horizon */
    ASSERT_TRUE(NEAR(camera_low_pitch_zoom_cap(&s, 45.0f, 25.0f), 25.0f));
    ASSERT_TRUE(NEAR(camera_low_pitch_zoom_cap(&s, 30.0f, 25.0f), 25.0f));
    ASSERT_TRUE(NEAR(camera_low_pitch_zoom_cap(&s, 0.0f, 25.0f), 8.0f));
    ASSERT_TRUE(NEAR(camera_low_pitch_zoom_cap(&s, -20.0f, 25.0f), 8.0f));
    float mid = camera_low_pitch_zoom_cap(&s, 15.0f, 25.0f);
    ASSERT_TRUE(NEAR(mid, 16.5f));                /* smoothstep(0.5) = 0.5 */
    ASSERT_TRUE(camera_low_pitch_zoom_cap(&s, 10.0f, 25.0f) < mid);
    /* never raises a limit that is already lower; off = untouched */
    ASSERT_TRUE(NEAR(camera_low_pitch_zoom_cap(&s, 0.0f, 5.0f), 5.0f));
    s.limit_zoom_at_low_pitch = false;
    ASSERT_TRUE(NEAR(camera_low_pitch_zoom_cap(&s, 0.0f, 25.0f), 25.0f));
}

TEST(multi_target_zoom_range) {
    CameraSettings s;
    camera_settings_defaults(&s);
    float lo, hi;
    /* the game's range (3.5..50 live) is wider than the defaults: kept */
    camera_multi_target_zoom_range(&s.combat, NULL, 3.5f, 50.0f, &lo, &hi);
    ASSERT_TRUE(NEAR(lo, 3.5f) && NEAR(hi, 50.0f));
    /* a farther ZoomMax or AltZoomMax widens it */
    s.combat.zoom_max = 60.0f;
    camera_multi_target_zoom_range(&s.combat, NULL, 3.5f, 50.0f, &lo, &hi);
    ASSERT_TRUE(NEAR(hi, 60.0f));
    s.combat.alt_zoom_max = 65.0f;
    camera_multi_target_zoom_range(&s.combat, NULL, 3.5f, 50.0f, &lo, &hi);
    ASSERT_TRUE(NEAR(hi, 65.0f));
    /* the other context's limits count too (the arm cap) */
    s.exploration.zoom_max = 80.0f;
    camera_multi_target_zoom_range(&s.combat, &s.exploration, 3.5f, 50.0f, &lo, &hi);
    ASSERT_TRUE(NEAR(hi, 80.0f));
    /* no usable game maximum: the farther of ZoomMax (20) and AltZoomMax (25) */
    camera_settings_defaults(&s);
    camera_multi_target_zoom_range(&s.combat, NULL, 6.0f, NAN, &lo, &hi);
    ASSERT_TRUE(NEAR(lo, 6.0f) && NEAR(hi, 25.0f));
    /* a game floor past the maximum is lowered to it, so the range stays valid */
    s.combat.zoom_max = 5.0f;
    s.combat.alt_zoom_max = 5.0f;
    camera_multi_target_zoom_range(&s.combat, NULL, 6.0f, NAN, &lo, &hi);
    ASSERT_TRUE(NEAR(lo, 5.0f) && NEAR(hi, 5.0f));
    /* an unusable game floor falls back to ZoomMin */
    camera_multi_target_zoom_range(&s.combat, NULL, NAN, NAN, &lo, &hi);
    ASSERT_TRUE(NEAR(lo, 2.0f));
    camera_multi_target_zoom_range(&s.combat, NULL, 0.0f, NAN, &lo, &hi);
    ASSERT_TRUE(NEAR(lo, 2.0f));
}

TEST(keep_zoom_key_parses_per_context) {
    CameraSettings s;
    camera_settings_defaults(&s);
    ASSERT_FALSE(s.exploration.keep_zoom);
    ASSERT_FALSE(s.combat.keep_zoom);
    ASSERT_EQ(camera_settings_parse(&s, "[CombatZoom]\nCombatKeepZoom = true\n", NULL), 0);
    ASSERT_TRUE(s.combat.keep_zoom);
    ASSERT_FALSE(s.exploration.keep_zoom);
    /* the key belongs to the zoom section */
    ASSERT_EQ(camera_settings_parse(&s, "[CombatPitch]\nCombatKeepZoom = false\n", NULL), 1);
    ASSERT_TRUE(s.combat.keep_zoom);
}

TEST(keep_zoom_restores_after_the_game_moves_it) {
    CameraKeepZoomState k = {0};
    float floor;
    const float dt = 0.1f;
    /* first frame: the zoom on screen when the last update ended, not a
     * zoom the game wrote this frame (a combat-start retarget) */
    ASSERT_TRUE(NEAR(camera_keep_zoom_step(&k, 8.0f, 20.0f, 0.0f, dt, false, false,
                                           2.0f, 25.0f, &floor), 20.0f));
    ASSERT_TRUE(isnan(floor));
    /* a retarget sets 8: put 20 back */
    ASSERT_TRUE(NEAR(camera_keep_zoom_step(&k, 8.0f, 20.0f, 0.0f, dt, false, false,
                                           2.0f, 25.0f, &floor), 20.0f));
    /* the player zooms out at 5/s: the game stepped 0.5 */
    ASSERT_TRUE(NEAR(camera_keep_zoom_step(&k, 20.5f, 20.0f, 5.0f, dt, false, false,
                                           2.0f, 25.0f, &floor), 20.5f));
    ASSERT_TRUE(NEAR(k.zoom, 20.5f));
    /* two targets: the game's fit (15) stands, floored at the player's zoom */
    ASSERT_TRUE(NEAR(camera_keep_zoom_step(&k, 15.0f, 15.0f, 0.0f, dt, true, false,
                                           6.0f, 25.0f, &floor), 15.0f));
    ASSERT_TRUE(NEAR(floor, 20.5f));
    /* zooming in during the fit lowers the floor by the step */
    camera_keep_zoom_step(&k, 20.0f, 20.5f, -5.0f, dt, true, false, 6.0f, 25.0f, &floor);
    ASSERT_TRUE(NEAR(k.zoom, 20.0f) && NEAR(floor, 20.0f));
    /* the floor stays inside the framing range, the player's zoom is kept */
    k.zoom = 3.0f;
    camera_keep_zoom_step(&k, 15.0f, 15.0f, 0.0f, dt, true, false, 6.0f, 25.0f, &floor);
    ASSERT_TRUE(NEAR(floor, 6.0f) && NEAR(k.zoom, 3.0f));
    /* one target again: back to the player's zoom */
    ASSERT_TRUE(NEAR(camera_keep_zoom_step(&k, 15.0f, 15.0f, 0.0f, dt, false, false,
                                           2.0f, 25.0f, &floor), 3.0f));
    /* Center Camera: take the reset zoom */
    ASSERT_TRUE(NEAR(camera_keep_zoom_step(&k, 10.0f, 3.0f, 0.0f, dt, false, true,
                                           2.0f, 25.0f, &floor), 10.0f));
    ASSERT_TRUE(NEAR(k.zoom, 10.0f));
    /* the game stops at its limit while the player keeps zooming: take what
     * is on screen */
    k.zoom = 25.0f;
    ASSERT_TRUE(NEAR(camera_keep_zoom_step(&k, 24.0f, 24.0f, 5.0f, dt, false, false,
                                           2.0f, 25.0f, &floor), 24.0f));
    /* a smaller range clamps the kept zoom */
    ASSERT_TRUE(NEAR(camera_keep_zoom_step(&k, 24.0f, 24.0f, 0.0f, dt, false, false,
                                           2.0f, 20.0f, &floor), 20.0f));
    /* an unusable zoom is passed through untouched */
    ASSERT_TRUE(isnan(camera_keep_zoom_step(&k, NAN, 20.0f, 0.0f, dt, false, false,
                                            2.0f, 20.0f, &floor)));
}

/* Flat ground at y = floor_y, except "no data" for x < 0. */
typedef struct { float floor_y; int calls; } FloorFixture;

static bool fixture_floor(void *ctx, const float pos[3], float *floor_y) {
    FloorFixture *f = ctx;
    f->calls++;
    if (pos[0] < 0.0f) return false;
    *floor_y = f->floor_y;
    return true;
}

TEST(floor_guard_pulls_in_above_ground) {
    /* Camera arm from (0, 2, 0) going out and down 30 degrees. */
    const float origin[3] = {0.0f, 2.0f, 0.0f};
    const float dir[3] = {0.8660254f, -0.5f, 0.0f};
    FloorFixture f = {0.0f, 0};

    /* Clear: untouched, one query. */
    ASSERT_TRUE(NEAR(camera_floor_guard(origin, dir, 3.0f, 1.0f, 0.5f, fixture_floor, &f), 3.0f));
    ASSERT_EQ(f.calls, 1);

    /* Under: y = 2 - 0.5 z must stay >= 0.5, so z <= 3. */
    f.calls = 0;
    float z = camera_floor_guard(origin, dir, 20.0f, 1.0f, 0.5f, fixture_floor, &f);
    ASSERT_TRUE(z <= 3.0f && z > 2.9f);
    ASSERT_TRUE(f.calls <= 1 + 16 + 5);

    /* Never below the minimum zoom. */
    f.floor_y = 10.0f;
    ASSERT_TRUE(NEAR(camera_floor_guard(origin, dir, 20.0f, 1.5f, 0.5f, fixture_floor, &f), 1.5f));

    /* No floor data counts as clear; no function = no guard. */
    const float back[3] = {-0.8660254f, -0.5f, 0.0f};
    ASSERT_TRUE(NEAR(camera_floor_guard(origin, back, 20.0f, 1.0f, 0.5f, fixture_floor, &f), 20.0f));
    ASSERT_TRUE(NEAR(camera_floor_guard(origin, dir, 20.0f, 1.0f, 0.5f, NULL, NULL), 20.0f));
}

TEST(mouse_pitch_moves_only_while_mouse_rotating) {
    CameraSettings s;
    camera_settings_defaults(&s);
    s.pitch_transition_speed = 0.0f;   /* no hand-over blend: out is the pitch */
    CameraPitchState st = {0};
    float out = 0.0f;
    const float dt = 0.016f;
    const uint32_t rotating = CAMERA_MODE_FLAG_MOUSE_ROTATION;
    camera_calculate_pitch(&s, &st, 0, dt, 20.0f, &out);
    ASSERT_TRUE(NEAR(st.pitch, 25.0f));               /* UnlockedPitchInitialValue */
    /* without the game's mouse rotation the movement is dropped, not saved up */
    st.mouse_dy = 40.0f;
    camera_calculate_pitch(&s, &st, 0, dt, 20.0f, &out);
    ASSERT_TRUE(NEAR(st.pitch, 25.0f) && st.mouse_dy == 0.0f);
    /* 40 points down x MousePitchMult 0.25 x MouseCameraRotationMult 1 = +10 degrees */
    st.mouse_dy = 40.0f;
    camera_calculate_pitch(&s, &st, rotating, dt, 20.0f, &out);
    ASSERT_TRUE(NEAR(st.pitch, 35.0f) && NEAR(out, 35.0f) && st.mouse_dy == 0.0f);
    s.invert_mouse_pitch = true;
    st.mouse_dy = 40.0f;
    camera_calculate_pitch(&s, &st, rotating, dt, 20.0f, &out);
    ASSERT_TRUE(NEAR(st.pitch, 25.0f));
    /* a big move stops at UnlockedPitchMax */
    s.invert_mouse_pitch = false;
    st.mouse_dy = 1000.0f;
    camera_calculate_pitch(&s, &st, rotating, dt, 20.0f, &out);
    ASSERT_TRUE(NEAR(st.pitch, s.exploration.unlocked_pitch_max));
    /* a locked context drops it: the pitch only eases back toward the game's
     * (85 -> ~80.6 at UnlockedPitchClampSpeed 5; the mouse would add -10) */
    s.combat.unlock_pitch = false;
    st.mouse_dy = -40.0f;
    ASSERT_FALSE(camera_calculate_pitch(&s, &st, rotating | CAMERA_MODE_FLAG_COMBAT, dt,
                                        30.0f, &out));
    ASSERT_TRUE(out > 75.0f && out < 85.0f && st.mouse_dy == 0.0f);
}

TEST(parse_macos_camera_keys) {
    CameraSettings s;
    camera_settings_defaults(&s);
    ASSERT_FALSE(s.log_camera_events);
    const char *text =
        "[General]\n"
        "PitchTransitionSpeed = 0\n"
        "LimitZoomAtLowPitch = false\n"
        "LowPitchAngle = 45\n"
        "LowPitchMaxZoom = 12.5\n"
        "LogCameraEvents = true\n"
        "[Controller]\n"
        "ZoomSmoothing = 0.3\n"
        "[ControllerDeadzone]\n"
        "DeadzoneHysteresis = 0.1\n"
        "LowPitchAngle = 5\n";            /* wrong section: rejected */
    int line = 0;
    ASSERT_EQ(camera_settings_parse(&s, text, &line), 1);
    ASSERT_EQ(line, 11);
    ASSERT_TRUE(NEAR(s.pitch_transition_speed, 0) && !s.limit_zoom_at_low_pitch);
    ASSERT_TRUE(s.log_camera_events);
    ASSERT_TRUE(NEAR(s.low_pitch_angle, 45) && NEAR(s.low_pitch_max_zoom, 12.5f));
    ASSERT_TRUE(NEAR(s.zoom_smoothing, 0.3f) && NEAR(s.deadzone_hysteresis, 0.1f));
}

/* ---- call sites ------------------------------------------------------------ */

#define VA(off) ((off) + 0x100000000ull)

TEST(bl_encode_round_trip) {
    uint64_t t = 0;
    uint32_t w = camera_encode_bl(VA(0x0333fd7c), VA(0x034f3030));
    ASSERT_EQ(w, 0x9406ccadu);
    ASSERT_TRUE(camera_decode_bl(VA(0x0333fd7c), w, &t) && t == VA(0x034f3030));
    w = camera_encode_bl(0x200000000ull, 0x200000000ull - 0x1000);  /* backwards */
    ASSERT_TRUE(camera_decode_bl(0x200000000ull, w, &t) && t == 0x200000000ull - 0x1000);
    ASSERT_EQ(camera_encode_bl(0x100000000ull, 0x100000000ull + (128ull << 20)), 0u);
    ASSERT_EQ(camera_encode_bl(0x100000000ull, 0x100000002ull), 0u);
}

/* Under the locale a German Mac gives the game, a value is still read with a point. */
TEST(parse_reads_numbers_in_the_c_locale) {
    CameraSettings s;
    camera_settings_defaults(&s);
    char *before = strdup(setlocale(LC_ALL, NULL));
    ASSERT_TRUE(setlocale(LC_ALL, "de_DE.UTF-8") != NULL);
    int result = camera_settings_parse(&s, "[CombatZoom]\nCombatZoomMax = 33.5\n", NULL);
    setlocale(LC_ALL, before);
    free(before);
    ASSERT_EQ(result, 0);
    ASSERT_TRUE(NEAR(s.combat.zoom_max, 33.5f));
}

int main(void) {
    RUN_TEST(defaults_match_upstream);
    RUN_TEST(parse_reads_numbers_in_the_c_locale);
    RUN_TEST(default_file_parses_clean_and_matches_defaults);
    RUN_TEST(parse_upstream_keys_and_sections);
    RUN_TEST(parse_rejects_bad_values_and_keeps_previous);
    RUN_TEST(parse_restores_inverted_ranges);
    RUN_TEST(mode_flags_and_pitch_unlock);
    RUN_TEST(stick_deadzone_mapping);
    RUN_TEST(zoom_input_reaches_game_band);
    RUN_TEST(controller_pitch_delta_sign_and_invert);
    RUN_TEST(interp_to_converges_both_directions);
    RUN_TEST(pitch_state_machine);
    RUN_TEST(pitch_resync_eases_after_pinned_pitch);
    RUN_TEST(deadzone_hysteresis_holds_then_fades);
    RUN_TEST(zoom_input_uses_response_curve);
    RUN_TEST(smooth_toward_is_exponential);
    RUN_TEST(low_pitch_zoom_cap);
    RUN_TEST(multi_target_zoom_range);
    RUN_TEST(keep_zoom_key_parses_per_context);
    RUN_TEST(keep_zoom_restores_after_the_game_moves_it);
    RUN_TEST(floor_guard_pulls_in_above_ground);
    RUN_TEST(mouse_pitch_moves_only_while_mouse_rotating);
    RUN_TEST(parse_macos_camera_keys);
    RUN_TEST(bl_encode_round_trip);
    printf("%d tests, %d failures\n", g_run, g_failed);
    return g_failed ? 1 : 0;
}
