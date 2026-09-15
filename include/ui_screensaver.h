// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver.h"
#include "screensaver_motion.h"

#include <cstdint>
#include <lvgl.h>
#include <vector>

/**
 * @brief Flying Toasters screensaver (After Dark, 1989)
 *
 * Replaces the dim phase when enabled: after inactivity timeout, toasters
 * and toast fly diagonally across a black screen. Touch wakes back to UI.
 *
 * Lifecycle:
 *   start()  — Create black overlay on lv_layer_top(), spawn objects, start animations
 *   stop()   — Delete everything, clean shutdown
 *   is_active() — Check if screensaver is currently running
 *
 * All animation (flight + wing flap) is driven by a single lv_timer running at the
 * display refresh period. Positions and wing frames are computed from elapsed time, so
 * the toasters keep their speed however often or unevenly the timer fires.
 */
class FlyingToasterScreensaver : public Screensaver {
  public:
    FlyingToasterScreensaver() = default;
    ~FlyingToasterScreensaver() override;

    FlyingToasterScreensaver(const FlyingToasterScreensaver&) = delete;
    FlyingToasterScreensaver& operator=(const FlyingToasterScreensaver&) = delete;

    /** @brief Start the screensaver (creates overlay, spawns objects, starts animations) */
    void start() override;

    /** @brief Stop the screensaver (clean shutdown, deletes everything) */
    void stop() override;

    /** @brief Check if screensaver is currently active */
    bool is_active() const override {
        return m_active;
    }

    /** @brief Return FLYING_TOASTERS type */
    ScreensaverType type() const override {
        return ScreensaverType::FLYING_TOASTERS;
    }

  private:
    // Test-only seam: reads the timer, sprites and decoded frames so frame-time-driven
    // motion can be pinned. See tests/test_helpers/screensaver_test_access.h.
    friend class FlyingToasterScreensaverTestAccess;

    struct FlyingObject {
        lv_obj_t* img;
        bool is_toaster;
        int16_t start_x;
        int16_t start_y;
        int fly_ms;
        int delay_ms;
        // Wing flap (toasters only)
        uint8_t initial_frame;
        uint8_t flap_frame;    // frame currently shown
        uint16_t flap_step_ms; // how long each wing frame holds
        // Previous position — skip lv_obj_set_pos() when unchanged to avoid invalidation
        int16_t prev_x = INT16_MIN;
        int16_t prev_y = INT16_MIN;
    };

    /** @brief Create the full-screen black overlay */
    void create_overlay();

    /** @brief Spawn all flying objects with staggered positions and delays */
    void spawn_objects(bool low_tier);

    /** @brief Create a single flying object */
    void create_flying_object(int start_x, int start_y, bool is_toaster, bool reverse_flap,
                              int speed_ms, int delay_ms);

    /** @brief Single timer callback driving all animation (flight + flap) */
    static void tick_cb(lv_timer_t* timer);

    /** @brief Get image scale factor based on screen width */
    int get_scale_factor() const;

    /** @brief Pre-decode all PNG sprites into persistent RAM buffers */
    void decode_sprites();

    /** @brief Free pre-decoded sprite buffers */
    void free_sprites();

    /**
     * @brief Cancel the tick timer — shared by stop() and the destructor
     *
     * A timer cancelled only in stop() stays armed on a freed `this` on any
     * teardown that skips it.
     */
    void cancel_timer();

    bool m_active = false;
    lv_obj_t* m_overlay = nullptr;
    std::vector<FlyingObject> m_objects;
    lv_timer_t* m_tick_timer = nullptr;
    helix::ui::screensaver::MotionClock m_clock;
    uint32_t m_elapsed_ms = 0; // time the saver has run, advanced by m_clock each tick

    // Pre-decoded sprite buffers (avoid per-frame PNG file I/O + decompression)
    lv_draw_buf_t* m_decoded_frames[4] = {}; // toaster_0..3
    lv_draw_buf_t* m_decoded_toast = nullptr;
};

#endif // HELIX_ENABLE_SCREENSAVER
