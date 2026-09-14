// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_pointer_frame_hook.cpp
 * @brief Every pointer sample reaches LVGL in the frame its rotation step expects
 *
 * A touch panel reports where on the panel it was touched. A relative pointer's
 * driver accumulates motion into a position on the picture the user moves it
 * across. These read each kind through lv_indev_read(), so LVGL's own rotation
 * step runs on the sample and the point asserted is the one widgets receive.
 */

#include "../lvgl_test_fixture.h"
#include "pointer_frame_hook.h"

#include <vector>

#include "../catch_amalgamated.hpp"

using helix::pointer_transform_for;
using helix::PointerFrameHook;
using helix::PointerTransform;
using helix::input::PointerKind;

namespace {

/// Where the touch driver says the panel was touched, in the panel's own frame.
constexpr lv_point_t TOUCH_ON_PANEL{100, 50};
/// Where the mouse driver has accumulated its motion to.
constexpr lv_point_t MOUSE_POSITION{300, 200};

void touch_driver_read(lv_indev_t* /*indev*/, lv_indev_data_t* data) {
    data->point = TOUCH_ON_PANEL;
    data->state = LV_INDEV_STATE_PRESSED;
}

void mouse_driver_read(lv_indev_t* /*indev*/, lv_indev_data_t* data) {
    data->point = MOUSE_POSITION;
    data->state = LV_INDEV_STATE_RELEASED;
}

/// A mouse far down a portrait picture, past the unrotated display's height.
constexpr lv_point_t MOUSE_DOWN_PORTRAIT{400, 700};

/// The point an evdev mouse's driver reports for MOUSE_DOWN_PORTRAIT: bounded by
/// the unrotated display.
void portrait_mouse_driver_read(lv_indev_t* /*indev*/, lv_indev_data_t* data) {
    data->point = {MOUSE_DOWN_PORTRAIT.x, TEST_DISPLAY_HEIGHT - 1};
    data->state = LV_INDEV_STATE_RELEASED;
}

class PointerFrameHookFixture : public LVGLTestFixture {
  public:
    PointerFrameHookFixture() {
        disp = lv_display_get_default();
        lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_0);
    }
    ~PointerFrameHookFixture() override {
        hook.restore_all();
        for (lv_indev_t* indev : created) {
            lv_indev_delete(indev);
        }
        lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_0);
    }

    lv_indev_t* make_pointer(lv_indev_read_cb_t driver_read) {
        lv_indev_t* indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, driver_read);
        created.push_back(indev);
        return indev;
    }

    /// One read through LVGL's own pipeline, returning the point widgets see.
    static lv_point_t read(lv_indev_t* indev) {
        lv_indev_read(indev);
        lv_point_t point{};
        lv_indev_get_point(indev, &point);
        return point;
    }

    static constexpr int32_t PANEL_W = TEST_DISPLAY_WIDTH;
    static constexpr int32_t PANEL_H = TEST_DISPLAY_HEIGHT;
    lv_display_t* disp = nullptr;
    PointerFrameHook hook;
    std::vector<lv_indev_t*> created;
};

} // namespace

TEST_CASE("A sample's transform follows the kind of device it came from",
          "[display][indev][rotation]") {
    SECTION("under a scanout plane only a touch panel turns") {
        CHECK(pointer_transform_for(PointerKind::PanelAbsolute, 180, 0) == PointerTransform::Plane);
        CHECK(pointer_transform_for(PointerKind::Relative, 180, 0) == PointerTransform::None);
    }

    SECTION("under LVGL's rotation only a relative pointer is handed back") {
        for (int degrees : {90, 180, 270}) {
            INFO("LVGL at " << degrees);
            CHECK(pointer_transform_for(PointerKind::PanelAbsolute, 0, degrees) ==
                  PointerTransform::None);
            CHECK(pointer_transform_for(PointerKind::Relative, 0, degrees) ==
                  PointerTransform::UndoLvglRotation);
        }
    }

    SECTION("with nothing rotated nothing turns here") {
        CHECK(pointer_transform_for(PointerKind::PanelAbsolute, 0, 0) == PointerTransform::None);
        CHECK(pointer_transform_for(PointerKind::Relative, 0, 0) == PointerTransform::None);
    }
}

TEST_CASE_METHOD(PointerFrameHookFixture,
                 "Under plane rotation a mouse keeps its position and touch turns with the picture",
                 "[display][indev][rotation]") {
    REQUIRE(disp != nullptr);
    REQUIRE(lv_display_get_horizontal_resolution(disp) == PANEL_W);
    REQUIRE(lv_display_get_vertical_resolution(disp) == PANEL_H);
    // The plane path leaves LVGL's own rotation at zero.
    REQUIRE(lv_display_get_rotation(disp) == LV_DISPLAY_ROTATION_0);
    hook.set_plane_rotation(180, PANEL_W, PANEL_H);

    lv_indev_t* touch = make_pointer(touch_driver_read);
    lv_indev_t* mouse = make_pointer(mouse_driver_read);
    REQUIRE(hook.install(touch, PointerKind::PanelAbsolute));
    REQUIRE(hook.install(mouse, PointerKind::Relative));

    const lv_point_t touched = read(touch);
    CHECK(touched.x == PANEL_W - 1 - TOUCH_ON_PANEL.x);
    CHECK(touched.y == PANEL_H - 1 - TOUCH_ON_PANEL.y);

    const lv_point_t pointed = read(mouse);
    CHECK(pointed.x == MOUSE_POSITION.x);
    CHECK(pointed.y == MOUSE_POSITION.y);
}

TEST_CASE_METHOD(PointerFrameHookFixture,
                 "Under LVGL rotation a mouse keeps its position and touch turns with the picture",
                 "[display][indev][rotation]") {
    REQUIRE(disp != nullptr);
    hook.set_plane_rotation(0, PANEL_W, PANEL_H);
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_180);

    lv_indev_t* touch = make_pointer(touch_driver_read);
    lv_indev_t* mouse = make_pointer(mouse_driver_read);
    REQUIRE(hook.install(touch, PointerKind::PanelAbsolute));
    REQUIRE(hook.install(mouse, PointerKind::Relative));

    // LVGL's own rotation step turns the touch sample.
    const lv_point_t touched = read(touch);
    CHECK(touched.x == PANEL_W - 1 - TOUCH_ON_PANEL.x);
    CHECK(touched.y == PANEL_H - 1 - TOUCH_ON_PANEL.y);

    const lv_point_t pointed = read(mouse);
    CHECK(pointed.x == MOUSE_POSITION.x);
    CHECK(pointed.y == MOUSE_POSITION.y);
}

TEST_CASE_METHOD(PointerFrameHookFixture,
                 "Under an LVGL quarter turn a mouse reaches the whole picture",
                 "[display][indev][rotation]") {
    REQUIRE(disp != nullptr);
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);
    // The position is on the picture but past the unrotated display's height,
    // where the driver's own point stops.
    REQUIRE(MOUSE_DOWN_PORTRAIT.x < lv_display_get_horizontal_resolution(disp));
    REQUIRE(MOUSE_DOWN_PORTRAIT.y < lv_display_get_vertical_resolution(disp));
    REQUIRE(MOUSE_DOWN_PORTRAIT.y >= PANEL_H);

    lv_indev_t* touch = make_pointer(touch_driver_read);
    lv_indev_t* mouse = make_pointer(portrait_mouse_driver_read);
    REQUIRE(hook.install(touch, PointerKind::PanelAbsolute));
    REQUIRE(hook.install(mouse, PointerKind::Relative, [](int& x, int& y) {
        x = MOUSE_DOWN_PORTRAIT.x;
        y = MOUSE_DOWN_PORTRAIT.y;
        return true;
    }));

    const lv_point_t touched = read(touch);
    CHECK(touched.x == PANEL_H - 1 - TOUCH_ON_PANEL.y);
    CHECK(touched.y == TOUCH_ON_PANEL.x);

    const lv_point_t pointed = read(mouse);
    CHECK(pointed.x == MOUSE_DOWN_PORTRAIT.x);
    CHECK(pointed.y == MOUSE_DOWN_PORTRAIT.y);
}

TEST_CASE_METHOD(PointerFrameHookFixture, "A mouse pushed past the picture's edge stops at it",
                 "[display][indev][rotation]") {
    REQUIRE(disp != nullptr);
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_270);
    const int32_t picture_h = lv_display_get_vertical_resolution(disp);

    lv_indev_t* mouse = make_pointer(mouse_driver_read);
    REQUIRE(hook.install(mouse, PointerKind::Relative, [](int& x, int& y) {
        x = -20;
        y = 5000;
        return true;
    }));

    const lv_point_t pointed = read(mouse);
    CHECK(pointed.x == 0);
    CHECK(pointed.y == picture_h - 1);
}

TEST_CASE_METHOD(PointerFrameHookFixture, "An unrotated display passes every sample through",
                 "[display][indev][rotation]") {
    REQUIRE(disp != nullptr);
    hook.set_plane_rotation(0, PANEL_W, PANEL_H);

    lv_indev_t* touch = make_pointer(touch_driver_read);
    lv_indev_t* mouse = make_pointer(mouse_driver_read);
    REQUIRE(hook.install(touch, PointerKind::PanelAbsolute));
    REQUIRE(hook.install(mouse, PointerKind::Relative));

    const lv_point_t touched = read(touch);
    CHECK(touched.x == TOUCH_ON_PANEL.x);
    CHECK(touched.y == TOUCH_ON_PANEL.y);

    const lv_point_t pointed = read(mouse);
    CHECK(pointed.x == MOUSE_POSITION.x);
    CHECK(pointed.y == MOUSE_POSITION.y);
}
