// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver.h"
#include "screensaver_motion.h"

#include <cmath>
#include <cstdint>
#include <lvgl.h>
#include <random>

namespace helix::screensaver_bounce {

/// Room the sprite must leave on each axis for the bounce to read as a bounce.
inline constexpr int MIN_RANGE_PX = 48;

/// Sprite edge as a fraction of the screen's narrow axis.
inline constexpr float SPRITE_FRACTION = 0.28f;

/**
 * @brief Reflect an unbounded coordinate into [0, range]
 *
 * The path of a body bouncing between two walls is the triangle wave of
 * unbounded travel folded into the axis. Computing position this way keeps the
 * motion a pure function of elapsed time: nothing accumulates, so float error
 * cannot drift the sprite into a wall and pin it there.
 */
inline float fold(float u, float range) {
    if (range <= 0.0f) {
        return 0.0f;
    }
    const float period = 2.0f * range;
    float m = std::fmod(u, period);
    if (m < 0.0f) {
        m += period;
    }
    return (m <= range) ? m : (period - m);
}

/**
 * @brief Count of completed folds — increments exactly once per wall hit
 */
inline int fold_index(float u, float range) {
    if (range <= 0.0f) {
        return 0;
    }
    return static_cast<int>(std::floor(u / range));
}

/**
 * @brief True when @p ratio sits within @p eps of a fraction p/q, q <= max_den
 *
 * The sprite's path closes into a short repeating loop when the ratio of its
 * two axis frequencies is a simple fraction — the degenerate cases being a
 * straight line and the four-point diagonal cycle that never reaches a corner.
 * Velocities that land near one are redrawn.
 */
inline bool near_rational(float ratio, int max_den, float eps) {
    for (int q = 1; q <= max_den; ++q) {
        for (int p = 1; p <= max_den; ++p) {
            if (std::fabs(ratio - static_cast<float>(p) / static_cast<float>(q)) < eps) {
                return true;
            }
        }
    }
    return false;
}

/**
 * @brief Longest sprite edge for a screen, or 0 when the screen cannot host a bounce
 *
 * Sized off the narrow axis, then held below what would leave less than
 * MIN_RANGE_PX of travel. 480x320 yields a 90px box over a 390x230 range.
 */
inline int sprite_size_for(int screen_w, int screen_h) {
    const int narrow = (screen_w < screen_h) ? screen_w : screen_h;
    int target = static_cast<int>(std::lround(SPRITE_FRACTION * static_cast<float>(narrow)));
    if (target > 320) {
        target = 320;
    }
    const int ceiling = narrow - MIN_RANGE_PX;
    if (target > ceiling) {
        target = ceiling;
    }
    return (target < 48) ? 0 : target;
}

/**
 * @brief Fit a source image's aspect ratio inside a square box of @p box pixels
 *
 * The walls are the edges of the sprite's own rectangle, so that rectangle has
 * to be the shape of the artwork. A square box around a tall printer render
 * would hold a column of empty pixels on each side and the printer would turn
 * before reaching the screen edge.
 */
inline void fit_sprite(int box, int32_t src_w, int32_t src_h, int& out_w, int& out_h) {
    if (src_w <= 0 || src_h <= 0) {
        out_w = box;
        out_h = box;
        return;
    }
    const double scale =
        static_cast<double>(box) / static_cast<double>((src_w >= src_h) ? src_w : src_h);
    out_w = static_cast<int>(std::lround(static_cast<double>(src_w) * scale));
    out_h = static_cast<int>(std::lround(static_cast<double>(src_h) * scale));
    if (out_w < 1) {
        out_w = 1;
    }
    if (out_h < 1) {
        out_h = 1;
    }
}

/**
 * @brief True when this frame reflected off both walls at once
 *
 * The fold indices alone would call it a corner whenever both axes happened to
 * turn in the same frame, and at the low tier a frame is 150 ms of travel. The
 * sprite has to actually be in the corner as well.
 */
inline bool is_corner_hit(bool bounced_x, bool bounced_y, float x, float y, float range_x,
                          float range_y, float tol) {
    if (!bounced_x || !bounced_y) {
        return false;
    }
    const float dx = (x < range_x - x) ? x : (range_x - x);
    const float dy = (y < range_y - y) ? y : (range_y - y);
    return dx <= tol && dy <= tol;
}

} // namespace helix::screensaver_bounce

namespace helix {

/**
 * @brief Bouncing Printer screensaver
 *
 * The printer this display is attached to drifts across a black field and
 * reflects off the edges, changing tint on every wall. Landing a true corner
 * earns a celebration.
 *
 * One sprite on one lv_timer. Position is computed from elapsed time rather
 * than integrated per frame, matching the rest of the subsystem.
 */
class BouncingPrinterScreensaver : public Screensaver {
  public:
    BouncingPrinterScreensaver() = default;
    ~BouncingPrinterScreensaver() override;

    BouncingPrinterScreensaver(const BouncingPrinterScreensaver&) = delete;
    BouncingPrinterScreensaver& operator=(const BouncingPrinterScreensaver&) = delete;

    void start() override;
    void stop() override;
    bool is_active() const override {
        return active_;
    }
    ScreensaverType type() const override {
        return ScreensaverType::BOUNCING_PRINTER;
    }

  private:
    /// Decode the active printer image into a persistent draw buffer
    bool decode_sprite();
    void free_sprite();
    void create_overlay();
    /// Pick a velocity whose path is not a short repeating loop
    void seed_motion();
    /// Recompute travel ranges after a resolution change without teleporting
    void rebase(int screen_w, int screen_h);
    void apply_tint();

    static void tick_cb(lv_timer_t* timer);
    void tick();

    /// Shared by stop() and the destructor — a timer cancelled only in stop()
    /// stays armed on a freed `this` on any teardown that skips it.
    void cancel_timer();

    bool active_ = false;

    lv_obj_t* overlay_ = nullptr;
    lv_obj_t* img_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    lv_draw_buf_t* decoded_ = nullptr;

    uint32_t tick_period_ms_ = 50;
    uint32_t elapsed_ms_ = 0;
    helix::ui::screensaver::MotionClock clock_;

    int screen_w_ = 0;
    int screen_h_ = 0;
    int sprite_w_ = 0;
    int sprite_h_ = 0;
    int32_t src_w_ = 0;
    int32_t src_h_ = 0;

    // Travel range per axis: screen extent less the sprite footprint
    float range_x_ = 0.0f;
    float range_y_ = 0.0f;

    // Phase offset and velocity (px/s) of the unbounded path
    float x0_ = 0.0f;
    float y0_ = 0.0f;
    float vx_ = 0.0f;
    float vy_ = 0.0f;

    int prev_fold_x_ = 0;
    int prev_fold_y_ = 0;
    int32_t prev_x_ = INT32_MIN;
    int32_t prev_y_ = INT32_MIN;

    float corner_tol_ = 4.0f;
    int color_index_ = 0;
    int corner_flash_ticks_ = 0;

    std::mt19937 rng_;
};

} // namespace helix

#endif // HELIX_ENABLE_SCREENSAVER
