// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_pointer_frame_hook.cpp
 * @brief Every pointer sample reaches LVGL in the frame its rotation step expects
 *
 * A touch panel reports where on the panel it was touched. A relative pointer's
 * driver accumulates motion into a position on the display LVGL lays out on.
 * These read each kind through lv_indev_read(), so LVGL's own rotation step
 * runs on the sample and the point asserted is the one widgets receive.
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
        CHECK(pointer_transform_for(PointerKind::PanelAbsolute, 180) == PointerTransform::Plane);
        CHECK(pointer_transform_for(PointerKind::Relative, 180) == PointerTransform::None);
    }

    SECTION("with no plane angle nothing turns here") {
        CHECK(pointer_transform_for(PointerKind::PanelAbsolute, 0) == PointerTransform::None);
        CHECK(pointer_transform_for(PointerKind::Relative, 0) == PointerTransform::None);
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
