// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "config.h"
#include "display_settings_manager.h"
#include "platform_capabilities.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// Screensaver Settings Tests
// ============================================================================

#ifdef HELIX_ENABLE_SCREENSAVER

TEST_CASE_METHOD(LVGLTestFixture,
                 "Screensaver defaults to tier-appropriate screensaver type when compiled in",
                 "[screensaver][display_settings]") {
    Config::get_instance();
    DisplaySettingsManager::instance().init_subjects();

    int expected = helix::PlatformCapabilities::detect().supports_animations ? 1 : 0;
    REQUIRE(DisplaySettingsManager::instance().get_screensaver_type() == expected);

    DisplaySettingsManager::instance().deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "Screensaver type set/get round trip",
                 "[screensaver][display_settings]") {
    Config::get_instance();
    DisplaySettingsManager::instance().init_subjects();

    SECTION("set to Off") {
        DisplaySettingsManager::instance().set_screensaver_type(0);
        REQUIRE(DisplaySettingsManager::instance().get_screensaver_type() == 0);
    }

    SECTION("set to Starfield") {
        DisplaySettingsManager::instance().set_screensaver_type(2);
        REQUIRE(DisplaySettingsManager::instance().get_screensaver_type() == 2);
    }

    SECTION("set to 3D Pipes") {
        DisplaySettingsManager::instance().set_screensaver_type(3);
        REQUIRE(DisplaySettingsManager::instance().get_screensaver_type() == 3);
    }

    SECTION("set back to Flying Toasters") {
        DisplaySettingsManager::instance().set_screensaver_type(0);
        DisplaySettingsManager::instance().set_screensaver_type(1);
        REQUIRE(DisplaySettingsManager::instance().get_screensaver_type() == 1);
    }

    SECTION("out of range clamped") {
        DisplaySettingsManager::instance().set_screensaver_type(99);
        REQUIRE(DisplaySettingsManager::instance().get_screensaver_type() == 3);

        DisplaySettingsManager::instance().set_screensaver_type(-1);
        REQUIRE(DisplaySettingsManager::instance().get_screensaver_type() == 0);
    }

    DisplaySettingsManager::instance().deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "Screensaver type subject reflects setter",
                 "[screensaver][display_settings]") {
    Config::get_instance();
    DisplaySettingsManager::instance().init_subjects();

    DisplaySettingsManager::instance().set_screensaver_type(0);
    REQUIRE(lv_subject_get_int(DisplaySettingsManager::instance().subject_screensaver_type()) == 0);

    DisplaySettingsManager::instance().set_screensaver_type(2);
    REQUIRE(lv_subject_get_int(DisplaySettingsManager::instance().subject_screensaver_type()) == 2);

    DisplaySettingsManager::instance().set_screensaver_type(1);
    REQUIRE(lv_subject_get_int(DisplaySettingsManager::instance().subject_screensaver_type()) == 1);

    DisplaySettingsManager::instance().deinit_subjects();
}

// ============================================================================
// FlyingToasterScreensaver Lifecycle Tests
// ============================================================================

#include "ui_screensaver.h"

TEST_CASE_METHOD(LVGLTestFixture, "FlyingToasterScreensaver starts inactive", "[screensaver]") {
    FlyingToasterScreensaver ss;
    REQUIRE(ss.is_active() == false);
}

TEST_CASE_METHOD(LVGLTestFixture, "FlyingToasterScreensaver start/stop lifecycle",
                 "[screensaver]") {
    FlyingToasterScreensaver ss;

    SECTION("start activates screensaver") {
        ss.start();
        REQUIRE(ss.is_active() == true);
        ss.stop();
    }

    SECTION("stop deactivates screensaver") {
        ss.start();
        ss.stop();
        REQUIRE(ss.is_active() == false);
    }

    SECTION("double start is safe") {
        ss.start();
        ss.start();
        REQUIRE(ss.is_active() == true);
        ss.stop();
    }

    SECTION("double stop is safe") {
        ss.start();
        ss.stop();
        ss.stop();
        REQUIRE(ss.is_active() == false);
    }

    SECTION("stop without start is safe") {
        ss.stop();
        REQUIRE(ss.is_active() == false);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "FlyingToasterScreensaver creates overlay on lv_layer_top",
                 "[screensaver]") {
    FlyingToasterScreensaver ss;

    int children_before = lv_obj_get_child_count(lv_layer_top());
    ss.start();
    int children_after = lv_obj_get_child_count(lv_layer_top());
    REQUIRE(children_after > children_before);

    ss.stop();
    REQUIRE_FALSE(ss.is_active());
    // Overlay is hidden and queued for async deletion (can't flush without
    // running all timers, which disrupts other fixture state)
    int children_final = lv_obj_get_child_count(lv_layer_top());
    REQUIRE(children_final <= children_after);
}

// ============================================================================
// FlyingToasterScreensaver motion follows elapsed time
// ============================================================================

#include "../test_helpers/screensaver_test_access.h"
#include "screensaver_motion.h"

namespace {

using ToasterAccess = FlyingToasterScreensaverTestAccess;
using helix::ui::screensaver::flap_frame_at;
using helix::ui::screensaver::flight_pos_at;
using helix::ui::screensaver::FlightPos;

constexpr int32_t TOASTER_FLIGHT_PX = 1600;

void run_toaster_tick(const FlyingToasterScreensaver& ss) {
    lv_timer_t* timer = ToasterAccess::tick_timer(ss);
    REQUIRE(timer != nullptr);
    REQUIRE(timer->timer_cb != nullptr);
    timer->timer_cb(timer);
}

struct SpriteView {
    bool hidden;
    int32_t x;
    int32_t y;
    int frame;

    bool operator==(const SpriteView& o) const {
        return hidden == o.hidden && x == o.x && y == o.y && frame == o.frame;
    }
};

std::vector<SpriteView> view_sprites(const FlyingToasterScreensaver& ss) {
    std::vector<SpriteView> out;
    for (const auto& s : ToasterAccess::sprites(ss)) {
        out.push_back(
            {lv_obj_has_flag(s.img, LV_OBJ_FLAG_HIDDEN), lv_obj_get_style_x(s.img, LV_PART_MAIN),
             lv_obj_get_style_y(s.img, LV_PART_MAIN), ToasterAccess::frame_of(ss, s.img)});
    }
    return out;
}

std::vector<int> shown_frames(const FlyingToasterScreensaver& ss) {
    std::vector<int> frames;
    for (const auto& s : ToasterAccess::sprites(ss)) {
        frames.push_back(ToasterAccess::frame_of(ss, s.img));
    }
    return frames;
}

/// A toaster on a 10 s or 16 s flight changes wing frame every 50 ms; one on a 24 s flight,
/// every 100 ms.
uint32_t flap_step_ms_for(int32_t fly_ms) {
    return fly_ms >= 20000 ? 100 : 50;
}

struct SpriteTally {
    int visible_toasters = 0;
    int visible_reversed_toasters = 0;
    int visible_only_at_full_size = 0;
};

/// Checks every flying sprite against where `elapsed_ms` of flight puts it, and counts what
/// the check covered. `initial_frames` are the frames the sprites showed at start.
SpriteTally check_sprites_at(const FlyingToasterScreensaver& ss, uint32_t elapsed_ms,
                             const std::vector<int>& initial_frames) {
    lv_display_t* disp = lv_display_get_default();
    const int32_t w = lv_display_get_horizontal_resolution(disp);
    const int32_t h = lv_display_get_vertical_resolution(disp);
    const int32_t size = w > 800 ? 128 : 64;
    const auto sprites = ToasterAccess::sprites(ss);
    REQUIRE(sprites.size() == initial_frames.size());

    SpriteTally tally;
    for (size_t i = 0; i < sprites.size(); i++) {
        const auto& s = sprites[i];
        CAPTURE(i, elapsed_ms);
        const FlightPos pos = flight_pos_at(elapsed_ms, s.start_x, s.start_y, s.fly_ms, s.delay_ms,
                                            TOASTER_FLIGHT_PX);
        if (!pos.started) {
            continue;
        }
        const bool on_screen = pos.x + size > 0 && pos.x < w && pos.y + size > 0 && pos.y < h;
        CHECK(lv_obj_has_flag(s.img, LV_OBJ_FLAG_HIDDEN) == !on_screen);
        if (!on_screen) {
            continue;
        }
        CHECK(lv_obj_get_style_x(s.img, LV_PART_MAIN) == pos.x);
        CHECK(lv_obj_get_style_y(s.img, LV_PART_MAIN) == pos.y);
        if (size > 64 && (pos.x + 64 <= 0 || pos.y + 64 <= 0)) {
            tally.visible_only_at_full_size++;
        }
        if (s.is_toaster) {
            tally.visible_toasters++;
            // Reversed toasters start on wing frame 2.
            if (initial_frames[i] == 2) {
                tally.visible_reversed_toasters++;
            }
            CHECK(ToasterAccess::frame_of(ss, s.img) ==
                  flap_frame_at(elapsed_ms, s.delay_ms, flap_step_ms_for(s.fly_ms),
                                static_cast<uint8_t>(initial_frames[i])));
        }
    }
    return tally;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "FlyingToasterScreensaver ticks at the display refresh period",
                 "[screensaver][screensaver_motion]") {
    // Matches neither LVGL's default refresh period nor any fixed saver period.
    ScopedRefreshPeriod refresh(20);
    FlyingToasterScreensaver ss;
    ScreensaverStopOnExit<FlyingToasterScreensaver> stop_on_exit{ss};

    ss.start();
    REQUIRE(ss.is_active());
    lv_timer_t* timer = ToasterAccess::tick_timer(ss);
    REQUIRE(timer != nullptr);
    CHECK(timer->period == 20);
}

TEST_CASE_METHOD(
    LVGLTestFixture,
    "FlyingToasterScreensaver lands sprites by elapsed time however callbacks split it",
    "[screensaver][screensaver_motion]") {
    FlyingToasterScreensaver one_tick;
    ScreensaverStopOnExit<FlyingToasterScreensaver> stop_one{one_tick};
    FlyingToasterScreensaver eleven_ticks;
    ScreensaverStopOnExit<FlyingToasterScreensaver> stop_eleven{eleven_ticks};

    one_tick.start();
    eleven_ticks.start();
    REQUIRE(one_tick.is_active());
    REQUIRE(eleven_ticks.is_active());
    REQUIRE(ToasterAccess::frames_decoded(one_tick));
    REQUIRE(ToasterAccess::frames_decoded(eleven_ticks));

    // Every sprite starts off-screen. Four seconds in, the 16 s and 24 s flights are
    // crossing it; both savers get there by the same calls.
    for (int i = 0; i < 10; i++) {
        lv_tick_inc(400);
        run_toaster_tick(one_tick);
        run_toaster_tick(eleven_ticks);
    }
    const auto before = view_sprites(one_tick);

    for (int i = 0; i < 11; i++) {
        lv_tick_inc(10);
        run_toaster_tick(eleven_ticks);
    }
    run_toaster_tick(one_tick);

    const auto one = view_sprites(one_tick);
    const auto eleven = view_sprites(eleven_ticks);
    const auto sprites = ToasterAccess::sprites(one_tick);
    REQUIRE(one.size() == eleven.size());
    REQUIRE(one.size() == before.size());
    int moved = 0;
    int flapped = 0;
    for (size_t i = 0; i < one.size(); i++) {
        CAPTURE(i);
        CHECK(one[i].hidden == eleven[i].hidden);
        if (one[i].hidden || eleven[i].hidden) {
            continue;
        }
        CHECK(one[i].x == eleven[i].x);
        CHECK(one[i].y == eleven[i].y);
        CHECK(one[i].frame == eleven[i].frame);
        if (!before[i].hidden && one[i].x != before[i].x) {
            moved++;
        }
        if (sprites[i].is_toaster && !before[i].hidden && one[i].frame != before[i].frame) {
            flapped++;
        }
    }
    // The 110 ms window moved visible sprites and changed toasters' wing frames, so the
    // comparison covered both.
    REQUIRE(moved > 0);
    REQUIRE(flapped > 0);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "FlyingToasterScreensaver flies 128 px sprites on displays wider than 800 px",
                 "[screensaver][screensaver_motion]") {
    ScopedResolution res(lv_display_get_default(), 1024, 600);
    FlyingToasterScreensaver ss;
    ScreensaverStopOnExit<FlyingToasterScreensaver> stop_on_exit{ss};

    ss.start();
    REQUIRE(ss.is_active());
    REQUIRE(ToasterAccess::frames_decoded(ss));
    for (const auto& s : ToasterAccess::sprites(ss)) {
        CHECK(lv_image_get_scale(s.img) == 512);
    }
    const auto initial = shown_frames(ss);

    lv_tick_inc(100);
    run_toaster_tick(ss);

    const SpriteTally tally = check_sprites_at(ss, 100, initial);
    REQUIRE(tally.visible_toasters > 0);
    // Some visible sprite overlaps the screen only because it is 128 px, so the sprite size
    // decided its visibility.
    REQUIRE(tally.visible_only_at_full_size > 0);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "FlyingToasterScreensaver flaps each toaster from its own initial frame",
                 "[screensaver][screensaver_motion]") {
    FlyingToasterScreensaver ss;
    ScreensaverStopOnExit<FlyingToasterScreensaver> stop_on_exit{ss};

    ss.start();
    REQUIRE(ss.is_active());
    REQUIRE(ToasterAccess::frames_decoded(ss));
    const auto initial = shown_frames(ss);

    // 4.1 s in, the reversed 16 s toasters are on screen at a point in the wing cycle where
    // one started from frame 2 shows a different frame than one started from frame 0.
    for (int i = 0; i < 10; i++) {
        lv_tick_inc(400);
        run_toaster_tick(ss);
    }
    lv_tick_inc(100);
    run_toaster_tick(ss);

    REQUIRE(check_sprites_at(ss, 4100, initial).visible_reversed_toasters > 0);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "FlyingToasterScreensaver restarts its flight on start after stop",
                 "[screensaver][screensaver_motion]") {
    FlyingToasterScreensaver ss;
    ScreensaverStopOnExit<FlyingToasterScreensaver> stop_on_exit{ss};

    ss.start();
    REQUIRE(ss.is_active());
    REQUIRE(ToasterAccess::frames_decoded(ss));
    const auto initial = shown_frames(ss);
    // 370 ms puts both flap rates on a frame other than the initial one.
    lv_tick_inc(370);
    run_toaster_tick(ss);
    REQUIRE(check_sprites_at(ss, 370, initial).visible_toasters > 0);
    const auto first_run = view_sprites(ss);

    ss.stop();
    lv_tick_inc(5000);
    ss.start();
    REQUIRE(ss.is_active());
    REQUIRE(ToasterAccess::frames_decoded(ss));
    CHECK(shown_frames(ss) == initial);

    lv_tick_inc(370);
    run_toaster_tick(ss);
    CHECK(check_sprites_at(ss, 370, initial).visible_toasters > 0);
    CHECK(view_sprites(ss) == first_run);
}

// ============================================================================
// ScreensaverManager Tests
// ============================================================================

#include "screensaver.h"

TEST_CASE_METHOD(LVGLTestFixture, "ScreensaverManager starts inactive", "[screensaver]") {
    REQUIRE(ScreensaverManager::instance().is_active() == false);
}

TEST_CASE_METHOD(LVGLTestFixture, "ScreensaverManager start/stop lifecycle", "[screensaver]") {
    auto& mgr = ScreensaverManager::instance();

    SECTION("start OFF does nothing") {
        mgr.start(ScreensaverType::OFF);
        REQUIRE(mgr.is_active() == false);
    }

    SECTION("start and stop Flying Toasters") {
        mgr.start(ScreensaverType::FLYING_TOASTERS);
        REQUIRE(mgr.is_active() == true);
        mgr.stop();
        REQUIRE(mgr.is_active() == false);
    }

    SECTION("start and stop Starfield") {
        mgr.start(ScreensaverType::STARFIELD);
        REQUIRE(mgr.is_active() == true);
        mgr.stop();
        REQUIRE(mgr.is_active() == false);
    }

    SECTION("start and stop 3D Pipes") {
        mgr.start(ScreensaverType::PIPES_3D);
        REQUIRE(mgr.is_active() == true);
        mgr.stop();
        REQUIRE(mgr.is_active() == false);
    }

    SECTION("switching types stops previous") {
        mgr.start(ScreensaverType::FLYING_TOASTERS);
        REQUIRE(mgr.is_active() == true);
        mgr.start(ScreensaverType::STARFIELD);
        REQUIRE(mgr.is_active() == true);
        mgr.stop();
        REQUIRE(mgr.is_active() == false);
    }

    SECTION("double stop is safe") {
        mgr.start(ScreensaverType::FLYING_TOASTERS);
        mgr.stop();
        mgr.stop();
        REQUIRE(mgr.is_active() == false);
    }
}

// ============================================================================
// Active screen hidden while a saver runs
// ============================================================================

#include "screen_hide_hold.h"

namespace {

bool is_hidden(lv_obj_t* obj) {
    return lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

/// Stops the shared manager however the test exits, so a failed assertion cannot
/// leave a saver and its screen hold running into the next test.
struct StopSaverOnExit {
    ~StopSaverOnExit() {
        ScreensaverManager::instance().stop();
    }
};

void count_event(lv_event_t* e) {
    ++*static_cast<int*>(lv_event_get_user_data(e));
}

/// lv_obj_remove_flag(HIDDEN) invalidates the screen after clearing the flag, so an
/// unhide at any moment reaches the display as an invalidation with the flag clear.
struct UnhideProbe {
    lv_obj_t* screen;
    bool saw_visible;
};

void record_invalidate_while_visible(lv_event_t* e) {
    auto* probe = static_cast<UnhideProbe*>(lv_event_get_user_data(e));
    if (!lv_obj_has_flag(probe->screen, LV_OBJ_FLAG_HIDDEN)) {
        probe->saw_visible = true;
    }
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "ScreensaverManager hides the active screen while a saver runs",
                 "[screensaver][screen_hide]") {
    auto& mgr = ScreensaverManager::instance();
    StopSaverOnExit stop_on_exit;
    REQUIRE_FALSE(helix::active_screen_hide_hold().is_held());
    REQUIRE(lv_screen_active() == test_screen());

    for (ScreensaverType type : {ScreensaverType::FLYING_TOASTERS, ScreensaverType::STARFIELD,
                                 ScreensaverType::PIPES_3D}) {
        CAPTURE(static_cast<int>(type));
        REQUIRE_FALSE(is_hidden(test_screen()));

        mgr.start(type);
        REQUIRE(mgr.is_active());
        CHECK(is_hidden(test_screen()));

        mgr.stop();
        CHECK_FALSE(is_hidden(test_screen()));
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "ScreensaverManager leaves an already hidden screen hidden",
                 "[screensaver][screen_hide]") {
    auto& mgr = ScreensaverManager::instance();
    StopSaverOnExit stop_on_exit;
    REQUIRE_FALSE(helix::active_screen_hide_hold().is_held());
    lv_obj_add_flag(test_screen(), LV_OBJ_FLAG_HIDDEN);

    mgr.start(ScreensaverType::STARFIELD);
    REQUIRE(mgr.is_active());
    REQUIRE(helix::active_screen_hide_hold().is_held());

    mgr.stop();
    CHECK_FALSE(helix::active_screen_hide_hold().is_held());
    CHECK(is_hidden(test_screen()));
}

TEST_CASE_METHOD(LVGLTestFixture, "switching screensaver type keeps the screen hidden throughout",
                 "[screensaver][screen_hide]") {
    auto& mgr = ScreensaverManager::instance();
    StopSaverOnExit stop_on_exit;
    REQUIRE_FALSE(helix::active_screen_hide_hold().is_held());
    lv_display_t* disp = lv_obj_get_display(test_screen());

    mgr.start(ScreensaverType::FLYING_TOASTERS);
    REQUIRE(mgr.is_active());
    REQUIRE(is_hidden(test_screen()));

    UnhideProbe probe{test_screen(), false};
    lv_display_add_event_cb(disp, record_invalidate_while_visible, LV_EVENT_INVALIDATE_AREA,
                            &probe);

    mgr.start(ScreensaverType::STARFIELD);
    const bool starfield_active = mgr.is_active();
    mgr.start(ScreensaverType::PIPES_3D);
    const bool pipes_active = mgr.is_active();
    const bool unhidden_during_switch = probe.saw_visible;
    const bool hidden_after_switch = is_hidden(test_screen());

    mgr.stop();
    const bool probe_saw_final_unhide = probe.saw_visible;
    lv_display_remove_event_cb_with_user_data(disp, record_invalidate_while_visible, &probe);

    CHECK(starfield_active);
    CHECK(pipes_active);
    CHECK_FALSE(unhidden_during_switch);
    CHECK(hidden_after_switch);
    // The probe does register an unhide when one happens.
    CHECK(probe_saw_final_unhide);
    CHECK_FALSE(is_hidden(test_screen()));
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a screensaver that fails to start leaves the screen visible and the manager idle",
                 "[screensaver][screen_hide]") {
    auto& mgr = ScreensaverManager::instance();
    StopSaverOnExit stop_on_exit;
    REQUIRE_FALSE(helix::active_screen_hide_hold().is_held());
    const ScreensaverType type = GENERATE(ScreensaverType::STARFIELD, ScreensaverType::PIPES_3D);
    CAPTURE(static_cast<int>(type));

    // Starfield and pipes refuse to start without a default display.
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);
    lv_display_set_default(nullptr);
    mgr.start(type);
    lv_display_set_default(disp);

    CHECK_FALSE(mgr.is_active());
    CHECK_FALSE(helix::active_screen_hide_hold().is_held());
    CHECK_FALSE(is_hidden(test_screen()));

    // Nothing was left behind: the next saver that does start still hides the screen.
    mgr.start(ScreensaverType::FLYING_TOASTERS);
    REQUIRE(mgr.is_active());
    CHECK(is_hidden(test_screen()));
    mgr.stop();
    CHECK_FALSE(is_hidden(test_screen()));
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a widget created on the screen during a saver is visible after it stops",
                 "[screensaver][screen_hide]") {
    auto& mgr = ScreensaverManager::instance();
    StopSaverOnExit stop_on_exit;
    REQUIRE_FALSE(helix::active_screen_hide_hold().is_held());

    mgr.start(ScreensaverType::FLYING_TOASTERS);
    REQUIRE(mgr.is_active());

    // Stands in for a modal or notification opened while the saver is up: both are
    // children of the active screen.
    lv_obj_t* dialog = lv_obj_create(lv_screen_active());
    lv_obj_set_size(dialog, 200, 120);
    lv_obj_center(dialog);
    lv_obj_update_layout(dialog);
    CHECK_FALSE(lv_obj_is_visible(dialog));

    mgr.stop();
    lv_obj_update_layout(dialog);
    CHECK(lv_obj_is_visible(dialog));
}

TEST_CASE_METHOD(LVGLTestFixture, "the panel under a running screensaver is not drawn",
                 "[screensaver][screen_hide]") {
    auto& mgr = ScreensaverManager::instance();
    StopSaverOnExit stop_on_exit;
    REQUIRE_FALSE(helix::active_screen_hide_hold().is_held());
    const ScreensaverType type = GENERATE(ScreensaverType::FLYING_TOASTERS,
                                          ScreensaverType::STARFIELD, ScreensaverType::PIPES_3D);
    CAPTURE(static_cast<int>(type));

    lv_obj_t* child = lv_obj_create(test_screen());
    lv_obj_set_size(child, 100, 100);
    lv_obj_update_layout(child);
    int draws = 0;
    lv_obj_add_event_cb(child, count_event, LV_EVENT_DRAW_MAIN, &draws);
    lv_display_t* disp = lv_obj_get_display(child);

    mgr.start(type);
    REQUIRE(mgr.is_active());
    lv_obj_invalidate(child);
    lv_refr_now(disp);
    const int draws_under_saver = draws;

    mgr.stop();
    lv_obj_invalidate(child);
    lv_refr_now(disp);

    CHECK(draws_under_saver == 0);
    // The same invalidate and refresh draw the child once the saver is gone.
    CHECK(draws > draws_under_saver);
}

// ============================================================================
// Starfield and pipes redraw only what their canvas changes
// ============================================================================

#include <algorithm>
#include <cstring>

namespace {

using StarAccess = StarfieldScreensaverTestAccess;
using PipesAccess = PipesScreensaverTestAccess;

// A pipe grows one grid step per 100 ms of frame time.
constexpr uint32_t PIPES_STEP_MS = 100;

void run_timer(lv_timer_t* timer) {
    REQUIRE(timer != nullptr);
    REQUIRE(timer->timer_cb != nullptr);
    timer->timer_cb(timer);
}

/// Counts the draw tasks `obj` itself adds to refreshes and snapshots, for as long as it lives.
class DrawTaskCounter {
  public:
    explicit DrawTaskCounter(lv_obj_t* obj) : obj_(obj) {
        lv_obj_add_flag(obj_, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
        lv_obj_add_event_cb(obj_, on_task_added, LV_EVENT_DRAW_TASK_ADDED, this);
    }
    ~DrawTaskCounter() {
        if (lv_obj_is_valid(obj_)) {
            lv_obj_remove_event_cb_with_user_data(obj_, on_task_added, this);
            lv_obj_remove_flag(obj_, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
        }
    }
    DrawTaskCounter(const DrawTaskCounter&) = delete;
    DrawTaskCounter& operator=(const DrawTaskCounter&) = delete;

    int total = 0;
    int fills = 0;
    int images = 0;

  private:
    static void on_task_added(lv_event_t* e) {
        auto* self = static_cast<DrawTaskCounter*>(lv_event_get_user_data(e));
        const lv_draw_task_type_t type = lv_draw_task_get_type(lv_event_get_draw_task(e));
        self->total++;
        self->fills += type == LV_DRAW_TASK_TYPE_FILL;
        self->images += type == LV_DRAW_TASK_TYPE_IMAGE;
    }

    lv_obj_t* obj_;
};

/// Records every area invalidated on `disp`, for as long as it lives.
class InvalidatedAreas {
  public:
    explicit InvalidatedAreas(lv_display_t* disp) : disp_(disp) {
        lv_display_add_event_cb(disp_, on_invalidate, LV_EVENT_INVALIDATE_AREA, this);
    }
    ~InvalidatedAreas() {
        lv_display_remove_event_cb_with_user_data(disp_, on_invalidate, this);
    }
    InvalidatedAreas(const InvalidatedAreas&) = delete;
    InvalidatedAreas& operator=(const InvalidatedAreas&) = delete;

    std::vector<lv_area_t> areas;

  private:
    static void on_invalidate(lv_event_t* e) {
        auto* self = static_cast<InvalidatedAreas*>(lv_event_get_user_data(e));
        self->areas.push_back(*static_cast<const lv_area_t*>(lv_event_get_param(e)));
    }

    lv_display_t* disp_;
};

bool area_contains(const lv_area_t& outer, const lv_area_t& inner) {
    return outer.x1 <= inner.x1 && outer.y1 <= inner.y1 && outer.x2 >= inner.x2 &&
           outer.y2 >= inner.y2;
}

lv_area_t coords_of(lv_obj_t* obj) {
    lv_obj_update_layout(obj);
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    return area;
}

/// Redraws everything on the top layer now.
void refresh_top_layer() {
    lv_obj_invalidate(lv_layer_top());
    lv_refr_now(lv_display_get_default());
}

void check_overlay_draws_only_its_canvas(lv_obj_t* overlay, lv_obj_t* canvas) {
    REQUIRE(overlay != nullptr);
    REQUIRE(canvas != nullptr);
    DrawTaskCounter overlay_tasks(overlay);
    DrawTaskCounter canvas_tasks(canvas);

    refresh_top_layer();

    CHECK(overlay_tasks.fills == 0);
    // The refresh did reach the overlay: it drew the canvas on top of it.
    CHECK(canvas_tasks.images > 0);
}

/// The overlay is deleted on a later timer pass. Until then a refresh (a wake's
/// lv_refr_now) or a top-layer snapshot (a screenshot) must not draw the canvas, whose
/// buffer is already freed.
void check_stopped_canvas_is_not_drawn(lv_obj_t* canvas) {
    REQUIRE(lv_obj_is_valid(canvas));
    CHECK(lv_obj_has_flag(canvas, LV_OBJ_FLAG_HIDDEN));

    DrawTaskCounter canvas_tasks(canvas);
    refresh_top_layer();
    lv_draw_buf_t* snapshot = lv_snapshot_take(lv_layer_top(), LV_COLOR_FORMAT_ARGB8888);
    REQUIRE(snapshot != nullptr);
    lv_draw_buf_destroy(snapshot);

    CHECK(canvas_tasks.total == 0);
}

struct PixelChanges {
    size_t changed = 0;
    size_t outside_areas = 0;
};

/// Compares the canvas buffer with `before`, and counts the changed pixels no area in
/// `areas` (display coordinates) covers.
PixelChanges diff_canvas(const std::vector<uint8_t>& before, const lv_draw_buf_t* buf,
                         const lv_area_t& canvas_coords, const std::vector<lv_area_t>& areas) {
    PixelChanges out;
    for (uint32_t y = 0; y < buf->header.h; y++) {
        for (uint32_t x = 0; x < buf->header.w; x++) {
            const size_t i = y * buf->header.stride + x * 4;
            if (std::memcmp(&before[i], &buf->data[i], 4) == 0) {
                continue;
            }
            out.changed++;
            const int32_t px = canvas_coords.x1 + static_cast<int32_t>(x);
            const int32_t py = canvas_coords.y1 + static_cast<int32_t>(y);
            const bool covered =
                std::any_of(areas.begin(), areas.end(), [px, py](const lv_area_t& a) {
                    return px >= a.x1 && px <= a.x2 && py >= a.y1 && py <= a.y2;
                });
            out.outside_areas += covered ? 0 : 1;
        }
    }
    return out;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "the starfield and pipes overlays draw nothing under their canvas",
                 "[screensaver][screensaver_canvas]") {
    SECTION("starfield") {
        StarfieldScreensaver ss;
        ScreensaverStopOnExit<StarfieldScreensaver> stop_on_exit{ss};
        ss.start();
        REQUIRE(ss.is_active());
        check_overlay_draws_only_its_canvas(StarAccess::overlay(ss), StarAccess::canvas(ss));
    }

    SECTION("pipes") {
        PipesScreensaver ss;
        ScreensaverStopOnExit<PipesScreensaver> stop_on_exit{ss};
        ss.start();
        REQUIRE(ss.is_active());
        check_overlay_draws_only_its_canvas(PipesAccess::overlay(ss), PipesAccess::canvas(ss));
    }
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "each pipes grow step invalidates less than the canvas and every pixel it changes",
                 "[screensaver][screensaver_canvas]") {
    PipesScreensaver ss;
    ScreensaverStopOnExit<PipesScreensaver> stop_on_exit{ss};
    PipesAccess::set_fixed_seed(ss, 42);
    ss.start();
    REQUIRE(ss.is_active());
    lv_obj_t* canvas = PipesAccess::canvas(ss);
    REQUIRE(canvas != nullptr);
    const lv_draw_buf_t* buf = lv_canvas_get_draw_buf(canvas);
    REQUIRE(buf != nullptr);
    REQUIRE(lv_color_format_get_size(static_cast<lv_color_format_t>(buf->header.cf)) == 4);
    const lv_area_t canvas_coords = coords_of(canvas);
    const int64_t canvas_px = lv_area_get_size(&canvas_coords);

    InvalidatedAreas invalidated(lv_obj_get_display(canvas));
    // The first step also starts the third pipe; later ones turn corners and draw joints.
    for (int step = 0; step < 40; step++) {
        CAPTURE(step);
        const std::vector<uint8_t> before(buf->data, buf->data + buf->data_size);
        invalidated.areas.clear();

        lv_tick_inc(PIPES_STEP_MS);
        run_timer(PipesAccess::timer(ss));

        const PixelChanges changes = diff_canvas(before, buf, canvas_coords, invalidated.areas);
        REQUIRE(changes.changed > 0);
        CHECK(changes.outside_areas == 0);
        REQUIRE_FALSE(invalidated.areas.empty());
        for (const lv_area_t& area : invalidated.areas) {
            CHECK(lv_area_get_size(&area) < canvas_px);
        }
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "a pipes grid reset invalidates the whole canvas",
                 "[screensaver][screensaver_canvas]") {
    PipesScreensaver ss;
    ScreensaverStopOnExit<PipesScreensaver> stop_on_exit{ss};
    PipesAccess::set_fixed_seed(ss, 11);
    ss.start();
    REQUIRE(ss.is_active());
    lv_obj_t* canvas = PipesAccess::canvas(ss);
    REQUIRE(canvas != nullptr);
    const lv_area_t canvas_coords = coords_of(canvas);

    PipesAccess::set_total_segments(ss, PipesAccess::max_segments() + 1);
    InvalidatedAreas invalidated(lv_obj_get_display(canvas));
    lv_tick_inc(PIPES_STEP_MS);
    run_timer(PipesAccess::timer(ss));
    REQUIRE(PipesAccess::total_segments(ss) == 0);

    CHECK(std::any_of(invalidated.areas.begin(), invalidated.areas.end(),
                      [&](const lv_area_t& a) { return area_contains(a, canvas_coords); }));
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a stopped starfield or pipes canvas is hidden until its overlay is deleted",
                 "[screensaver][screensaver_canvas]") {
    SECTION("starfield") {
        StarfieldScreensaver ss;
        ScreensaverStopOnExit<StarfieldScreensaver> stop_on_exit{ss};
        ss.start();
        REQUIRE(ss.is_active());
        lv_obj_t* canvas = StarAccess::canvas(ss);
        REQUIRE(canvas != nullptr);
        ss.stop();
        check_stopped_canvas_is_not_drawn(canvas);
    }

    SECTION("pipes") {
        PipesScreensaver ss;
        ScreensaverStopOnExit<PipesScreensaver> stop_on_exit{ss};
        ss.start();
        REQUIRE(ss.is_active());
        lv_obj_t* canvas = PipesAccess::canvas(ss);
        REQUIRE(canvas != nullptr);
        ss.stop();
        check_stopped_canvas_is_not_drawn(canvas);
    }
}

// ============================================================================
// Refresh period while a saver runs
// ============================================================================

#include "../test_helpers/refresh_period_hold_test_access.h"
#include "../test_helpers/scoped_env.h"
#include "../test_helpers/screensaver_manager_test_access.h"
#include "refresh_period_hold.h"
#include "refresh_timing_env.h"

namespace {

using helix::anim_timer_period;
using helix::default_refr_timer_period;

constexpr uint32_t GLOBAL_PERIOD_MS = 40;
constexpr uint32_t SAVER_PERIOD_MS = 16;

/// Sets the global and screensaver refresh periods through the environment and applies
/// them as DisplayManager::init() does. Puts the environment and both timers back when the
/// test ends.
struct SaverRefreshEnv {
    helix::ScopedTimerPeriods timers;
    helix::ScopedEnv global{"HELIX_REFR_PERIOD_MS"};
    helix::ScopedEnv scope{"HELIX_REFR_PERIOD_SCOPE"};
    helix::ScopedEnv saver{"HELIX_SCREENSAVER_REFR_PERIOD_MS"};

    SaverRefreshEnv() {
        setenv("HELIX_REFR_PERIOD_MS", "40", 1);
        unsetenv("HELIX_REFR_PERIOD_SCOPE");
        setenv("HELIX_SCREENSAVER_REFR_PERIOD_MS", "16", 1);
        helix::apply_refresh_timing(helix::refresh_timing_from_env());
    }
};

/// Period of the timer the manager's running saver ticks on.
uint32_t running_saver_timer_period(ScreensaverType type) {
    Screensaver* active =
        helix::ScreensaverManagerTestAccess::active(ScreensaverManager::instance());
    REQUIRE(active != nullptr);
    REQUIRE(active->type() == type);
    const lv_timer_t* timer = nullptr;
    switch (type) {
    case ScreensaverType::FLYING_TOASTERS:
        timer = FlyingToasterScreensaverTestAccess::tick_timer(
            static_cast<const FlyingToasterScreensaver&>(*active));
        break;
    case ScreensaverType::STARFIELD:
        timer = StarfieldScreensaverTestAccess::timer(
            static_cast<const StarfieldScreensaver&>(*active));
        break;
    case ScreensaverType::PIPES_3D:
        timer = PipesScreensaverTestAccess::timer(static_cast<const PipesScreensaver&>(*active));
        break;
    case ScreensaverType::OFF:
        break;
    }
    REQUIRE(timer != nullptr);
    return timer->period;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "a running screensaver ticks at HELIX_SCREENSAVER_REFR_PERIOD_MS until it stops",
                 "[screensaver][refresh_period]") {
    const ScreensaverType type = GENERATE(ScreensaverType::FLYING_TOASTERS,
                                          ScreensaverType::STARFIELD, ScreensaverType::PIPES_3D);
    CAPTURE(static_cast<int>(type));
    auto& mgr = ScreensaverManager::instance();
    SaverRefreshEnv env;
    StopSaverOnExit stop_on_exit;
    REQUIRE_FALSE(helix::active_refresh_period_hold().is_held());
    CHECK(default_refr_timer_period() == GLOBAL_PERIOD_MS);

    mgr.start(type);
    REQUIRE(mgr.is_active());
    // The saver reads the refresh period when it starts, so this is only the configured
    // value if the period was set before start().
    CHECK(running_saver_timer_period(type) == SAVER_PERIOD_MS);
    CHECK(default_refr_timer_period() == SAVER_PERIOD_MS);
    CHECK(anim_timer_period() == SAVER_PERIOD_MS);

    mgr.stop();
    CHECK_FALSE(helix::active_refresh_period_hold().is_held());
    CHECK(default_refr_timer_period() == GLOBAL_PERIOD_MS);
    CHECK(anim_timer_period() == GLOBAL_PERIOD_MS);
}

TEST_CASE_METHOD(LVGLTestFixture, "switching screensaver type keeps the saver refresh period",
                 "[screensaver][refresh_period]") {
    auto& mgr = ScreensaverManager::instance();
    SaverRefreshEnv env;
    StopSaverOnExit stop_on_exit;
    REQUIRE_FALSE(helix::active_refresh_period_hold().is_held());

    for (ScreensaverType type : {ScreensaverType::FLYING_TOASTERS, ScreensaverType::STARFIELD,
                                 ScreensaverType::PIPES_3D}) {
        CAPTURE(static_cast<int>(type));
        mgr.start(type);
        REQUIRE(mgr.is_active());
        CHECK(running_saver_timer_period(type) == SAVER_PERIOD_MS);
        CHECK(default_refr_timer_period() == SAVER_PERIOD_MS);
    }

    // One stop gives back everything the three starts took.
    mgr.stop();
    CHECK_FALSE(helix::active_refresh_period_hold().is_held());
    CHECK(default_refr_timer_period() == GLOBAL_PERIOD_MS);
    CHECK(anim_timer_period() == GLOBAL_PERIOD_MS);
}

TEST_CASE_METHOD(LVGLTestFixture, "a screensaver that fails to start gives the refresh period back",
                 "[screensaver][refresh_period]") {
    auto& mgr = ScreensaverManager::instance();
    SaverRefreshEnv env;
    StopSaverOnExit stop_on_exit;
    REQUIRE_FALSE(helix::active_refresh_period_hold().is_held());

    SECTION("no saver is registered for the requested type") {
        mgr.start(ScreensaverType::FLYING_TOASTERS);
        REQUIRE(mgr.is_active());
        REQUIRE(default_refr_timer_period() == SAVER_PERIOD_MS);

        mgr.start(static_cast<ScreensaverType>(7));

        CHECK_FALSE(mgr.is_active());
        CHECK_FALSE(helix::active_refresh_period_hold().is_held());
        CHECK(default_refr_timer_period() == GLOBAL_PERIOD_MS);
        CHECK(anim_timer_period() == GLOBAL_PERIOD_MS);
    }

    SECTION("the saver refuses to start") {
        const ScreensaverType type =
            GENERATE(ScreensaverType::STARFIELD, ScreensaverType::PIPES_3D);
        CAPTURE(static_cast<int>(type));
        // Starfield and pipes refuse to start without a default display.
        lv_display_t* disp = lv_display_get_default();
        REQUIRE(disp != nullptr);
        lv_display_set_default(nullptr);
        mgr.start(type);
        lv_display_set_default(disp);

        CHECK_FALSE(mgr.is_active());
        CHECK_FALSE(helix::active_refresh_period_hold().is_held());
        CHECK(default_refr_timer_period() == GLOBAL_PERIOD_MS);
    }

    // Nothing was left behind: the next saver that does start still gets the fast period.
    mgr.start(ScreensaverType::FLYING_TOASTERS);
    REQUIRE(mgr.is_active());
    CHECK(running_saver_timer_period(ScreensaverType::FLYING_TOASTERS) == SAVER_PERIOD_MS);
    mgr.stop();
    CHECK(default_refr_timer_period() == GLOBAL_PERIOD_MS);
}

#endif // HELIX_ENABLE_SCREENSAVER
