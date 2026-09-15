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

#endif // HELIX_ENABLE_SCREENSAVER
