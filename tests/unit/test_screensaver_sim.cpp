// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "../lvgl_test_fixture.h"
#include "../test_helpers/screensaver_test_access.h"
#include "screensaver_starfield_sim.h"

#include <algorithm>
#include <cstring>
#include <random>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::DirtyRect;
using helix::ui::FrameTarget;
using helix::ui::StarfieldSim;
using StarAccess = StarfieldScreensaverTestAccess;

namespace {

constexpr uint32_t FRAME_W = 320;
constexpr uint32_t FRAME_H = 200;
// Wider than 4 bytes per pixel, as LVGL's aligned strides can be, so a write indexed by
// width instead of stride lands on the wrong row.
constexpr uint32_t FRAME_STRIDE = FRAME_W * 4 + 12;

/// A heap frame painted opaque black.
struct Frame {
    std::vector<uint8_t> bytes;
    FrameTarget target;

    Frame() : bytes(static_cast<size_t>(FRAME_STRIDE) * FRAME_H, 0) {
        target = {bytes.data(), FRAME_STRIDE, FRAME_W, FRAME_H};
        helix::ui::fill_starfield_black(target);
    }
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
};

bool rect_contains(const DirtyRect& r, uint32_t x, uint32_t y) {
    const auto px = static_cast<int32_t>(x);
    const auto py = static_cast<int32_t>(y);
    return !r.empty() && px >= r.x1 && px <= r.x2 && py >= r.y1 && py <= r.y2;
}

struct Diff {
    size_t changed = 0;
    size_t outside = 0;
};

/// Counts the pixels of `after` that differ from `before`, and those of them outside `rect`.
Diff diff_frames(const std::vector<uint8_t>& before, const FrameTarget& after,
                 const DirtyRect& rect) {
    Diff diff;
    for (uint32_t y = 0; y < after.h; y++) {
        for (uint32_t x = 0; x < after.w; x++) {
            const size_t i = static_cast<size_t>(y) * after.stride + x * 4;
            if (std::memcmp(&before[i], after.data + i, 4) == 0) {
                continue;
            }
            diff.changed++;
            diff.outside += rect_contains(rect, x, y) ? 0 : 1;
        }
    }
    return diff;
}

/// Pixels with any colour.
size_t lit_pixels(const FrameTarget& f) {
    size_t count = 0;
    for (uint32_t y = 0; y < f.h; y++) {
        const uint8_t* row = f.data + static_cast<size_t>(y) * f.stride;
        for (uint32_t x = 0; x < f.w; x++) {
            count += (row[x * 4] | row[x * 4 + 1] | row[x * 4 + 2]) != 0 ? 1 : 0;
        }
    }
    return count;
}

/// Pixels whose X byte is not 0xFF.
size_t non_opaque_pixels(const FrameTarget& f) {
    size_t count = 0;
    for (uint32_t y = 0; y < f.h; y++) {
        const uint8_t* row = f.data + static_cast<size_t>(y) * f.stride;
        for (uint32_t x = 0; x < f.w; x++) {
            count += row[x * 4 + 3] != 0xFF ? 1 : 0;
        }
    }
    return count;
}

void run_timer(lv_timer_t* timer) {
    REQUIRE(timer != nullptr);
    REQUIRE(timer->timer_cb != nullptr);
    timer->timer_cb(timer);
}

} // namespace

// ============================================================================
// StarfieldSim: one frame of stars
// ============================================================================

TEST_CASE("StarfieldSim dirties every pixel its step changes", "[screensaver][starfield_sim]") {
    std::minstd_rand rng(7);
    StarfieldSim sim;
    sim.init(FRAME_W, FRAME_H, rng);
    Frame frame;

    size_t changed = 0;
    for (int step = 0; step < 90; step++) {
        CAPTURE(step);
        const std::vector<uint8_t> before = frame.bytes;
        const DirtyRect dirty = sim.step(33, frame.target, rng);
        const Diff diff = diff_frames(before, frame.target, dirty);
        CHECK(diff.outside == 0);
        changed += diff.changed;
    }
    REQUIRE(changed > 0);
}

TEST_CASE("StarfieldSim dirties only around stars near the centre",
          "[screensaver][starfield_sim]") {
    std::minstd_rand rng(11);
    StarfieldSim sim;
    sim.init(FRAME_W, FRAME_H, rng);
    for (auto& star : sim.stars()) {
        star.x = 0.02f;
        star.y = -0.02f;
        star.z = 0.9f;
    }
    Frame frame;
    REQUIRE_FALSE(sim.step(33, frame.target, rng).empty());

    // Erases the stars and draws them again a pixel or two away.
    const DirtyRect dirty = sim.step(33, frame.target, rng);
    REQUIRE_FALSE(dirty.empty());
    const int64_t area = static_cast<int64_t>(dirty.x2 - dirty.x1 + 1) * (dirty.y2 - dirty.y1 + 1);
    CHECK(area < static_cast<int64_t>(FRAME_W) * FRAME_H / 100);
}

TEST_CASE("StarfieldSim replays exactly from a seed", "[screensaver][starfield_sim]") {
    const auto run = [](uint32_t seed) {
        std::minstd_rand rng(seed);
        StarfieldSim sim;
        sim.init(FRAME_W, FRAME_H, rng);
        Frame frame;
        std::vector<DirtyRect> dirty;
        for (int i = 0; i < 60; i++) {
            for (uint32_t dt : {33u, 16u, 50u, 7u, 70u}) {
                dirty.push_back(sim.step(dt, frame.target, rng));
            }
        }
        return std::make_pair(frame.bytes, dirty);
    };

    const auto first = run(42);
    const Frame black;
    REQUIRE(first.first != black.bytes);
    CHECK(run(42) == first);
    CHECK_FALSE(run(43).first == first.first);
}

TEST_CASE("every X byte of a StarfieldSim frame stays 0xFF as stars move and recycle",
          "[screensaver][starfield_sim]") {
    std::minstd_rand rng(5);
    StarfieldSim sim;
    sim.init(FRAME_W, FRAME_H, rng);
    Frame frame;
    REQUIRE(non_opaque_pixels(frame.target) == 0);

    size_t most_lit = 0;
    for (int step = 0; step < 120; step++) {
        CAPTURE(step);
        sim.step(33, frame.target, rng);
        most_lit = std::max(most_lit, lit_pixels(frame.target));
        CHECK(non_opaque_pixels(frame.target) == 0);
    }
    // The check covered drawn stars, not only the black background.
    REQUIRE(most_lit > 0);
}

// ============================================================================
// StarfieldScreensaver: stepping and invalidating in the timer callback
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "a starfield frame invalidates the whole canvas",
                 "[screensaver][starfield_sim]") {
    StarfieldScreensaver ss;
    ScreensaverStopOnExit<StarfieldScreensaver> stop_on_exit{ss};
    ss.start();
    REQUIRE(ss.is_active());
    // Stars near the centre change a few pixels, far less than the canvas, so only a
    // full-canvas invalidation covers it.
    StarAccess::place_stars(ss, 0.02f, -0.02f, 0.9f);
    lv_obj_t* canvas_obj = StarAccess::canvas(ss);
    lv_obj_update_layout(canvas_obj);
    lv_area_t coords;
    lv_obj_get_coords(canvas_obj, &coords);

    helix::test::InvalidatedAreas invalidated(lv_obj_get_display(canvas_obj));
    lv_tick_inc(33);
    run_timer(StarAccess::timer(ss));

    REQUIRE_FALSE(invalidated.areas.empty());
    CHECK(std::any_of(invalidated.areas.begin(), invalidated.areas.end(), [&](const lv_area_t& a) {
        return a.x1 <= coords.x1 && a.y1 <= coords.y1 && a.x2 >= coords.x2 && a.y2 >= coords.y2;
    }));
}

#endif // HELIX_ENABLE_SCREENSAVER
