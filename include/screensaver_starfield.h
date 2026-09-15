// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver.h"
#include "screensaver_motion.h"
#include "screensaver_starfield_sim.h"

#include <cstdint>
#include <lvgl.h>
#include <optional>
#include <random>

/**
 * @brief Windows 95-style Starfield screensaver
 *
 * Stars fly outward from the center of the screen. Each star starts small
 * and dim near the center, growing larger and brighter as it approaches
 * the edges.
 *
 * helix::ui::StarfieldSim draws each frame with direct pixel writes, erasing only where
 * stars were, and moves the stars by the time since the previous frame. The timer steps it
 * and invalidates the whole canvas.
 */
class StarfieldScreensaver : public Screensaver {
  public:
    StarfieldScreensaver() = default;
    ~StarfieldScreensaver() override;

    void start() override;
    void stop() override;
    bool is_active() const override {
        return active_;
    }
    ScreensaverType type() const override {
        return ScreensaverType::STARFIELD;
    }

  private:
    // Test-only seam: reads the draw buffer, timer and simulation state, and fixes
    // the seed. See tests/test_helpers/screensaver_test_access.h.
    friend class StarfieldScreensaverTestAccess;

    static void frame_timer_cb(lv_timer_t* timer);

    /// Steps the stars and invalidates the whole canvas.
    void render_frame(uint32_t dt_ms);

    /// Shared by stop() and the destructor — a timer cancelled only in stop()
    /// stays armed on a freed `this` on any teardown that skips it.
    void cancel_timer();

    bool active_ = false;
    lv_obj_t* overlay_ = nullptr;
    lv_obj_t* canvas_ = nullptr;
    lv_timer_t* timer_ = nullptr;

    // Draw buffer owned by the canvas, allocated at LVGL's row stride — see
    // screensaver_canvas_stride_bytes(). Frames step rows by draw_buf_stride_ so direct
    // pixel writes land where the canvas reads them.
    uint8_t* draw_buf_ = nullptr;
    size_t draw_buf_size_ = 0;
    uint32_t draw_buf_stride_ = 0;

    helix::ui::StarfieldSim sim_;

    // Owned random sequence, seeded in start(); placing and recycling stars draw from it.
    std::minstd_rand rng_;
    // When set, start() seeds the random sequence from this instead of the clock, so a
    // run replays exactly.
    std::optional<uint32_t> fixed_seed_;
    helix::ui::screensaver::MotionClock clock_;

    int screen_w_ = 0;
    int screen_h_ = 0;
};

#endif // HELIX_ENABLE_SCREENSAVER
